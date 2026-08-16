/**
 * @file AsyncQueueCoroutineTest.cpp
 * @brief Sender-backed coroutine integration tests for asynchronous queues.
 * @ingroup Testing
 */
#include <Sora/Core/Concurrency/AsyncQueue.h>
#include <Sora/Core/Concurrency/Task.h>

#include "Support/Meta.h"

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include <atomic>
#include <chrono>
#include <concepts>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

using namespace Sora;
using namespace Sora::Concurrency;
using namespace std::chrono_literals;

namespace {

    using Storage = RingBuffer<int, 4, QueueTopologies::Spsc>;
    using Queue = AsyncQueue<Storage>;
    using Producer = ProducerPort<Queue>;
    using Consumer = ConsumerPort<Queue>;

    static_assert(std::same_as<Task<int>, stdexec::task<int>>);
    static_assert(std::same_as<Task<>, stdexec::task<void>>);
    static_assert(stdexec::sender<Task<int>>);
    static_assert(std::move_constructible<Task<>>);
    static_assert(!std::copy_constructible<Task<>>);

    template<class Sender>
    struct StartObservedSender {
        using sender_concept = stdexec::sender_t;
        using completion_signatures = typename Sender::completion_signatures;

        Sender sender;
        std::atomic<bool>* started;

        [[nodiscard]] auto get_env() const noexcept { return stdexec::get_env(sender); }

        template<class Receiver>
        struct Operation {
            using InnerOperation = stdexec::connect_result_t<Sender, Receiver>;

            InnerOperation inner;
            std::atomic<bool>* started;

            Operation(Sender&& source, Receiver&& receiver, std::atomic<bool>* signal)
                : inner(stdexec::connect(std::move(source), std::move(receiver))), started(signal) {}

            Operation(Operation&&) = delete;

            void start() & noexcept {
                stdexec::start(inner);
                started->store(true, std::memory_order_release);
            }
        };

        template<class Receiver>
        [[nodiscard]] auto connect(Receiver&& receiver) && -> Operation<std::remove_cvref_t<Receiver>> {
            return {std::move(sender), std::forward<Receiver>(receiver), started};
        }
    };

    template<class Sender>
    [[nodiscard]] auto ObserveStart(Sender&& sender, std::atomic<bool>& started) {
        return StartObservedSender<std::remove_cvref_t<Sender>>{std::forward<Sender>(sender), &started};
    }

    Task<PushOutcome> Push(Producer& producer, int value) {
        co_return co_await producer.Push(value);
    }

    Task<int> Pop(Consumer& consumer) {
        co_return co_await consumer.Pop();
    }

    Task<int> PopRecordingAffinity(Consumer& consumer, std::atomic<bool>& operationStarted,
                                   std::atomic<bool>& resumedOnStartThread) {
        const auto startThread = std::this_thread::get_id();
        const int value = co_await ObserveStart(consumer.Pop(), operationStarted);
        resumedOnStartThread.store(std::this_thread::get_id() == startThread, std::memory_order_release);
        co_return value;
    }

    template<class Predicate>
    [[nodiscard]] bool WaitUntil(Predicate&& predicate, std::chrono::milliseconds timeout = 2s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::yield();
        }
        return true;
    }

} // namespace

TEST_CASE("AsyncQueue operations are directly awaitable inside Task", AUTO_TAG) {
    Queue queue;
    auto producer = queue.Producer();
    auto consumer = queue.Consumer();

    auto pushed = stdexec::sync_wait(Push(producer, 41));
    auto popped = stdexec::sync_wait(Pop(consumer));

    REQUIRE(pushed.has_value());
    REQUIRE(std::get<0>(*pushed).Enqueued());
    REQUIRE(popped.has_value());
    REQUIRE(std::get<0>(*popped) == 41);
}

TEST_CASE("AsyncQueue coroutine resumes on its start scheduler after cross-thread completion", AUTO_TAG) {
    Queue queue;
    auto producer = queue.Producer();
    auto consumer = queue.Consumer();
    std::atomic<bool> operationStarted{false};
    std::atomic<bool> resumedOnStartThread{false};
    int popped = 0;

    std::thread waiter([&] {
        auto result = stdexec::sync_wait(PopRecordingAffinity(consumer, operationStarted, resumedOnStartThread));
        popped = result ? std::get<0>(*result) : -1;
    });

    const bool startedInTime = WaitUntil([&] { return operationStarted.load(std::memory_order_acquire); });
    auto pushed = stdexec::sync_wait(Push(producer, 42));
    waiter.join();

    REQUIRE(startedInTime);
    REQUIRE(pushed.has_value());
    REQUIRE(popped == 42);
    REQUIRE(resumedOnStartThread.load(std::memory_order_acquire));
}

TEST_CASE("AsyncQueue Close propagates stopped through Task", AUTO_TAG) {
    Queue queue;
    auto consumer = queue.Consumer();

    queue.Close();
    REQUIRE_FALSE(stdexec::sync_wait(Pop(consumer)).has_value());
}

// COCA's Windows ASan runtime corrupts exceptions crossing the bundled stdexec::task coroutine frame.
#if !__has_feature(address_sanitizer)
TEST_CASE("AsyncQueue Abort propagates QueueError through Task", AUTO_TAG) {
    Queue queue;
    auto consumer = queue.Consumer();

    queue.Abort();
    try {
        static_cast<void>(stdexec::sync_wait(Pop(consumer)));
        FAIL("aborted queue task must complete with QueueError");
    } catch (QueueError error) {
        REQUIRE(error == kQueueAborted);
    }
}
#endif

TEST_CASE("AsyncQueue Task forwards cancellation to a parked operation", AUTO_TAG) {
    Queue queue;
    auto consumer = queue.Consumer();
    stdexec::inplace_stop_source stopSource;
    std::atomic<bool> operationStarted{false};
    std::atomic<bool> resumedOnStartThread{false};
    std::atomic<bool> waitFinished{false};
    bool producedValue = true;

    auto task = PopRecordingAffinity(consumer, operationStarted, resumedOnStartThread);
    auto cancellable =
        stdexec::write_env(std::move(task), stdexec::prop{stdexec::get_stop_token, stopSource.get_token()});
    std::thread waiter([&] {
        auto result = stdexec::sync_wait(std::move(cancellable));
        producedValue = result.has_value();
        waitFinished.store(true, std::memory_order_release);
    });

    const bool startedInTime = WaitUntil([&] { return operationStarted.load(std::memory_order_acquire); });
    static_cast<void>(stopSource.request_stop());
    const bool stopRequested = stopSource.stop_requested();
    const bool finishedInTime = WaitUntil([&] { return waitFinished.load(std::memory_order_acquire); });
    if (!finishedInTime) {
        queue.Abort();
    }
    waiter.join();

    REQUIRE(startedInTime);
    REQUIRE(stopRequested);
    REQUIRE(finishedInTime);
    REQUIRE_FALSE(producedValue);
}

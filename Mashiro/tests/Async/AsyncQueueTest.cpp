/**
 * @file AsyncQueueTest.cpp
 * @brief Capability, lifecycle, cancellation, and concurrent data-path tests for asynchronous queues.
 * @ingroup Testing
 */
#include <Mashiro/Concurrency/AsyncQueue.h>

#include "Support/Meta.h"

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

using namespace Mashiro;
using namespace Mashiro::Concurrency;
using namespace std::chrono_literals;

namespace {

    using SpscStorage = SpscRingBuffer<int, 4>;
    using MpscStorage = MpscQueue<int, 1024>;
    using MpmcStorage = MpmcQueue<int, 1024>;
    using SpscAsyncQueue = AsyncQueue<SpscStorage>;
    using MpscAsyncQueue = AsyncQueue<MpscStorage>;
    using MpmcAsyncQueue = AsyncQueue<MpmcStorage>;
    using Batch = QueueBatch<int, SpscAsyncQueue::capacity>;

    enum class Completion : std::uint8_t {
        None,
        Value,
        Stopped,
        Error,
    };

    struct CompletionProbe {
        std::atomic<Completion> completion{Completion::None};
        std::atomic<QueueErrorCode> error{QueueErrorCode::None};
        std::atomic<PushDisposition> disposition{PushDisposition::Enqueued};
        std::atomic<int> value{0};
    };

    struct VoidReceiver {
        using receiver_concept = stdexec::receiver_t;

        CompletionProbe* probe{};
        stdexec::inplace_stop_token token{};

        [[nodiscard]] auto get_env() const noexcept {
            return stdexec::env{stdexec::prop{stdexec::get_stop_token, token}};
        }

        void set_value(PushOutcome outcome) && noexcept {
            probe->disposition.store(outcome.disposition, std::memory_order_relaxed);
            probe->completion.store(Completion::Value, std::memory_order_release);
        }

        void set_stopped() && noexcept { probe->completion.store(Completion::Stopped, std::memory_order_release); }

        void set_error(QueueError error) && noexcept {
            probe->error.store(error.code, std::memory_order_relaxed);
            probe->completion.store(Completion::Error, std::memory_order_release);
        }
    };

    struct IntReceiver {
        using receiver_concept = stdexec::receiver_t;

        CompletionProbe* probe{};
        stdexec::inplace_stop_token token{};

        [[nodiscard]] auto get_env() const noexcept {
            return stdexec::env{stdexec::prop{stdexec::get_stop_token, token}};
        }

        void set_value(int value) && noexcept {
            probe->value.store(value, std::memory_order_relaxed);
            probe->completion.store(Completion::Value, std::memory_order_release);
        }

        void set_stopped() && noexcept { probe->completion.store(Completion::Stopped, std::memory_order_release); }

        void set_error(QueueError error) && noexcept {
            probe->error.store(error.code, std::memory_order_relaxed);
            probe->completion.store(Completion::Error, std::memory_order_release);
        }
    };

    template<class Port, class BatchType>
    concept HasPushBatch = requires(Port& port, BatchType&& batch) { port.PushBatch(std::move(batch)); };

    template<class Port, class Policy>
    concept CanAttachFromLvalue = requires(const Port& port, Policy policy) { port.WithBackpressure(policy); };

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

    using SpscProducer = ProducerPort<SpscAsyncQueue>;
    using SpscConsumer = ConsumerPort<SpscAsyncQueue>;
    using MpscProducer = ProducerPort<MpscAsyncQueue>;
    using MpscConsumer = ConsumerPort<MpscAsyncQueue>;
    using MpmcProducer = ProducerPort<MpmcAsyncQueue>;
    using MpmcConsumer = ConsumerPort<MpmcAsyncQueue>;

    static_assert(SpscAsyncQueue::producerCardinality == QueueCardinality::Single);
    static_assert(SpscAsyncQueue::consumerCardinality == QueueCardinality::Single);
    static_assert(QueueAdapter<SpscStorage>::pushProgress == QueueProgress::WaitFree);
    static_assert(QueueAdapter<SpscStorage>::popProgress == QueueProgress::WaitFree);
    static_assert(QueueAdapter<SpscStorage>::transactionalPushBatch);
    static_assert(HasPushBatch<SpscProducer, Batch>);
    static_assert(!std::is_copy_constructible_v<SpscProducer>);
    static_assert(!std::is_copy_constructible_v<SpscConsumer>);
    static_assert(sizeof(SpscProducer) == sizeof(void*));
    static_assert(!CanAttachFromLvalue<SpscProducer, Backpressure::Reject>);

    static_assert(MpscAsyncQueue::producerCardinality == QueueCardinality::Multiple);
    static_assert(MpscAsyncQueue::consumerCardinality == QueueCardinality::Single);
    static_assert(!QueueAdapter<MpscStorage>::transactionalPushBatch);
    static_assert(!HasPushBatch<MpscProducer, QueueBatch<int, MpscAsyncQueue::capacity>>);
    static_assert(std::is_copy_constructible_v<MpscProducer>);
    static_assert(!std::is_copy_constructible_v<MpscConsumer>);
    static_assert(CanAttachFromLvalue<MpscProducer, Backpressure::Reject>);

    static_assert(MpmcAsyncQueue::producerCardinality == QueueCardinality::Multiple);
    static_assert(MpmcAsyncQueue::consumerCardinality == QueueCardinality::Multiple);
    static_assert(!QueueAdapter<MpmcStorage>::transactionalPushBatch);
    static_assert(!HasPushBatch<MpmcProducer, QueueBatch<int, MpmcAsyncQueue::capacity>>);
    static_assert(std::is_copy_constructible_v<MpmcProducer>);
    static_assert(std::is_copy_constructible_v<MpmcConsumer>);

} // namespace

TEST_CASE("AsyncQueue SPSC immediate sender operations preserve FIFO order", AUTO_TAG) {
    SpscAsyncQueue queue;
    auto producer = queue.Producer();
    auto consumer = queue.Consumer();

    REQUIRE(stdexec::sync_wait(producer.Push(41)).has_value());
    REQUIRE(stdexec::sync_wait(producer.Push(42)).has_value());

    auto first = stdexec::sync_wait(consumer.Pop());
    auto second = stdexec::sync_wait(consumer.Pop());
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(std::get<0>(*first) == 41);
    REQUIRE(std::get<0>(*second) == 42);
    REQUIRE(queue.EmptyApprox());
}

TEST_CASE("AsyncQueue Pop parks and completes after a later Push", AUTO_TAG) {
    SpscAsyncQueue queue;
    auto producer = queue.Producer();
    auto consumer = queue.Consumer();
    std::atomic<bool> started{false};
    std::optional<int> received;

    std::jthread consumerThread{[&] {
        started.store(true, std::memory_order_release);
        auto result = stdexec::sync_wait(consumer.Pop());
        if (result.has_value()) {
            received = std::get<0>(*result);
        }
    }};

    REQUIRE(WaitUntil([&] { return started.load(std::memory_order_acquire); }));
    REQUIRE(stdexec::sync_wait(producer.Push(42)).has_value());
    consumerThread.join();
    REQUIRE(received == 42);
}

TEST_CASE("AsyncQueue sender start returns while a full SPSC queue applies back pressure", AUTO_TAG) {
    using SmallQueue = AsyncQueue<SpscRingBuffer<int, 2>>;

    SmallQueue queue;
    auto producer = queue.Producer();
    auto consumer = queue.Consumer();
    REQUIRE(stdexec::sync_wait(producer.Push(1)).has_value());
    REQUIRE(stdexec::sync_wait(producer.Push(2)).has_value());

    CompletionProbe probe;
    auto sender = producer.Push(3);
    auto operation = stdexec::connect(std::move(sender), VoidReceiver{.probe = &probe});
    stdexec::start(operation);
    REQUIRE(probe.completion.load(std::memory_order_acquire) == Completion::None);

    auto first = consumer.TryPop();
    REQUIRE(first == 1);
    REQUIRE(probe.completion.load(std::memory_order_acquire) == Completion::Value);

    auto second = consumer.TryPop();
    auto third = consumer.TryPop();
    REQUIRE(second == 2);
    REQUIRE(third == 3);
}

TEST_CASE("AsyncQueue producer policies reject or drop without parking", AUTO_TAG) {
    using SmallQueue = AsyncQueue<SpscRingBuffer<int, 2>>;

    SmallQueue rejectQueue;
    auto rejecting = rejectQueue.Producer().WithBackpressure(Backpressure::Reject{});
    REQUIRE(std::get<0>(*stdexec::sync_wait(rejecting.Push(1))).Enqueued());
    REQUIRE(std::get<0>(*stdexec::sync_wait(rejecting.Push(2))).Enqueued());
    auto rejected = stdexec::sync_wait(rejecting.Push(3));
    REQUIRE(rejected.has_value());
    REQUIRE(std::get<0>(*rejected).disposition == PushDisposition::Rejected);
    REQUIRE(rejectQueue.SizeApprox() == 2);

    SmallQueue dropQueue;
    auto dropping = dropQueue.Producer().WithBackpressure(Backpressure::DropNewest{});
    REQUIRE(std::get<0>(*stdexec::sync_wait(dropping.Push(1))).Enqueued());
    REQUIRE(std::get<0>(*stdexec::sync_wait(dropping.Push(2))).Enqueued());
    auto dropped = stdexec::sync_wait(dropping.Push(3));
    REQUIRE(dropped.has_value());
    REQUIRE(std::get<0>(*dropped).disposition == PushDisposition::Dropped);
    REQUIRE(dropQueue.SizeApprox() == 2);
}

TEST_CASE("AsyncQueue bounded retry composes with fallback and opt-in telemetry", AUTO_TAG) {
    using SmallQueue = AsyncQueue<SpscRingBuffer<int, 2>>;
    using RetryPolicy = Backpressure::Retry<3, Backpressure::RetryMode::Spin, Backpressure::Reject>;

    Backpressure::Counters counters;
    SmallQueue queue;
    auto producer = queue.Producer().WithBackpressure(Backpressure::Instrumented{RetryPolicy{}, counters});
    REQUIRE(std::get<0>(*stdexec::sync_wait(producer.Push(1))).Enqueued());
    REQUIRE(std::get<0>(*stdexec::sync_wait(producer.Push(2))).Enqueued());

    auto result = stdexec::sync_wait(producer.Push(3));
    REQUIRE(result.has_value());
    REQUIRE(std::get<0>(*result).disposition == PushDisposition::Rejected);
    REQUIRE(counters.committed.load(std::memory_order_relaxed) == 2);
    REQUIRE(counters.full.load(std::memory_order_relaxed) == 4);
    REQUIRE(counters.retried.load(std::memory_order_relaxed) == 3);
    REQUIRE(counters.rejected.load(std::memory_order_relaxed) == 1);
    REQUIRE(counters.suspended.load(std::memory_order_relaxed) == 0);
    REQUIRE(counters.dropped.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("AsyncQueue bounded retry can fall back to asynchronous suspension", AUTO_TAG) {
    using SmallQueue = AsyncQueue<SpscRingBuffer<int, 2>>;
    using RetryThenSuspend = Backpressure::Retry<2, Backpressure::RetryMode::Yield, Backpressure::Suspend>;

    SmallQueue queue;
    auto producer = queue.Producer().WithBackpressure(RetryThenSuspend{});
    auto consumer = queue.Consumer();
    REQUIRE(std::get<0>(*stdexec::sync_wait(producer.Push(1))).Enqueued());
    REQUIRE(std::get<0>(*stdexec::sync_wait(producer.Push(2))).Enqueued());

    CompletionProbe probe;
    auto sender = producer.Push(3);
    auto operation = stdexec::connect(std::move(sender), VoidReceiver{.probe = &probe});
    stdexec::start(operation);
    REQUIRE(probe.completion.load(std::memory_order_acquire) == Completion::None);

    REQUIRE(consumer.TryPop() == 1);
    REQUIRE(probe.completion.load(std::memory_order_acquire) == Completion::Value);
    REQUIRE(probe.disposition.load(std::memory_order_relaxed) == PushDisposition::Enqueued);
}

TEST_CASE("AsyncQueue SPSC batch push is transactional and rejects impossible batches", AUTO_TAG) {
    SpscAsyncQueue queue;
    auto producer = queue.Producer();
    auto consumer = queue.Consumer();
    REQUIRE(stdexec::sync_wait(producer.Push(1)).has_value());
    REQUIRE(stdexec::sync_wait(producer.Push(2)).has_value());
    REQUIRE(stdexec::sync_wait(producer.Push(3)).has_value());

    CompletionProbe committed;
    auto sender = producer.PushBatch(Batch{10, 20});
    auto operation = stdexec::connect(std::move(sender), VoidReceiver{.probe = &committed});
    stdexec::start(operation);
    REQUIRE(committed.completion.load(std::memory_order_acquire) == Completion::None);

    auto drained = stdexec::sync_wait(consumer.PopBatch());
    REQUIRE(drained.has_value());
    const auto& initial = std::get<0>(*drained);
    REQUIRE(initial.size() == 3);
    REQUIRE(initial[0] == 1);
    REQUIRE(initial[1] == 2);
    REQUIRE(initial[2] == 3);
    REQUIRE(committed.completion.load(std::memory_order_acquire) == Completion::Value);

    auto appended = stdexec::sync_wait(consumer.PopBatch());
    REQUIRE(appended.has_value());
    const auto& tail = std::get<0>(*appended);
    REQUIRE(tail.size() == 2);
    REQUIRE(tail[0] == 10);
    REQUIRE(tail[1] == 20);

    SpscAsyncQueue oversizedQueue;
    auto oversizedProducer = oversizedQueue.Producer();
    CompletionProbe rejected;
    auto oversizedSender = oversizedProducer.PushBatch(Batch{1, 2, 3, 4, 5});
    auto oversizedOperation = stdexec::connect(std::move(oversizedSender), VoidReceiver{.probe = &rejected});
    stdexec::start(oversizedOperation);
    REQUIRE(rejected.completion.load(std::memory_order_acquire) == Completion::Error);
    REQUIRE(rejected.error.load(std::memory_order_relaxed) == QueueErrorCode::BatchTooLarge);
}

TEST_CASE("AsyncQueue Close drains values then stops consumers and rejects producers", AUTO_TAG) {
    SpscAsyncQueue queue;
    auto producer = queue.Producer();
    auto consumer = queue.Consumer();
    REQUIRE(stdexec::sync_wait(producer.Push(1)).has_value());
    REQUIRE(stdexec::sync_wait(producer.Push(2)).has_value());

    queue.Close();

    auto first = stdexec::sync_wait(consumer.Pop());
    auto second = stdexec::sync_wait(consumer.Pop());
    auto exhausted = stdexec::sync_wait(consumer.Pop());
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(std::get<0>(*first) == 1);
    REQUIRE(std::get<0>(*second) == 2);
    REQUIRE_FALSE(exhausted.has_value());

    CompletionProbe rejected;
    auto sender = producer.Push(3);
    auto operation = stdexec::connect(std::move(sender), VoidReceiver{.probe = &rejected});
    stdexec::start(operation);
    REQUIRE(rejected.completion.load(std::memory_order_acquire) == Completion::Error);
    REQUIRE(rejected.error.load(std::memory_order_relaxed) == QueueErrorCode::Closed);
}

TEST_CASE("AsyncQueue Abort fails parked operations", AUTO_TAG) {
    SpscAsyncQueue queue;
    auto consumer = queue.Consumer();
    CompletionProbe probe;
    auto sender = consumer.Pop();
    auto operation = stdexec::connect(std::move(sender), IntReceiver{.probe = &probe});
    stdexec::start(operation);
    REQUIRE(probe.completion.load(std::memory_order_acquire) == Completion::None);

    queue.Abort();
    REQUIRE(probe.completion.load(std::memory_order_acquire) == Completion::Error);
    REQUIRE(probe.error.load(std::memory_order_relaxed) == QueueErrorCode::Aborted);
}

TEST_CASE("AsyncQueue cancellation completes a parked operation exactly once", AUTO_TAG) {
    SpscAsyncQueue queue;
    auto consumer = queue.Consumer();
    stdexec::inplace_stop_source stopSource;
    CompletionProbe probe;
    auto sender = consumer.Pop();
    auto operation = stdexec::connect(std::move(sender), IntReceiver{.probe = &probe, .token = stopSource.get_token()});
    stdexec::start(operation);
    REQUIRE(probe.completion.load(std::memory_order_acquire) == Completion::None);

    stopSource.request_stop();
    REQUIRE(stopSource.stop_requested());
    REQUIRE(probe.completion.load(std::memory_order_acquire) == Completion::Stopped);
    queue.Abort();
    REQUIRE(probe.completion.load(std::memory_order_acquire) == Completion::Stopped);
}

TEST_CASE("AsyncQueue MPSC accepts concurrent producers without loss or duplication", AUTO_TAG) {
    constexpr std::size_t kProducerCount = 4;
    constexpr std::size_t kValuesPerProducer = 1000;
    constexpr std::size_t kTotal = kProducerCount * kValuesPerProducer;

    MpscAsyncQueue queue;
    auto producer = queue.Producer();
    auto consumer = queue.Consumer();
    std::array<bool, kTotal> seen{};
    std::atomic<std::size_t> consumed{0};
    std::atomic<bool> invalidValue{false};
    std::atomic<bool> duplicateValue{false};
    std::atomic<bool> timedOut{false};

    std::jthread consumerThread{[&] {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (consumed.load(std::memory_order_relaxed) != kTotal) {
            auto value = consumer.TryPop();
            if (!value.has_value()) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    timedOut.store(true, std::memory_order_relaxed);
                    return;
                }
                std::this_thread::yield();
                continue;
            }
            if (*value < 0 || static_cast<std::size_t>(*value) >= kTotal) {
                invalidValue.store(true, std::memory_order_relaxed);
            } else if (seen[static_cast<std::size_t>(*value)]) {
                duplicateValue.store(true, std::memory_order_relaxed);
            } else {
                seen[static_cast<std::size_t>(*value)] = true;
            }
            consumed.fetch_add(1, std::memory_order_relaxed);
        }
    }};

    std::vector<std::jthread> producers;
    for (std::size_t producerIndex = 0; producerIndex != kProducerCount; ++producerIndex) {
        producers.emplace_back([port = producer, producerIndex, &timedOut]() mutable {
            for (std::size_t index = 0; index != kValuesPerProducer; ++index) {
                int value = static_cast<int>(producerIndex * kValuesPerProducer + index);
                while (!port.TryPush(value)) {
                    if (timedOut.load(std::memory_order_relaxed)) {
                        return;
                    }
                    std::this_thread::yield();
                }
            }
        });
    }
    for (auto& thread : producers) {
        thread.join();
    }
    consumerThread.join();

    REQUIRE_FALSE(timedOut.load(std::memory_order_relaxed));
    REQUIRE_FALSE(invalidValue.load(std::memory_order_relaxed));
    REQUIRE_FALSE(duplicateValue.load(std::memory_order_relaxed));
    REQUIRE(consumed.load(std::memory_order_relaxed) == kTotal);
    for (bool present : seen) {
        REQUIRE(present);
    }
}

TEST_CASE("AsyncQueue MPMC accepts concurrent producers and consumers", AUTO_TAG) {
    constexpr std::size_t kProducerCount = 4;
    constexpr std::size_t kConsumerCount = 4;
    constexpr std::size_t kValuesPerProducer = 512;
    constexpr std::size_t kTotal = kProducerCount * kValuesPerProducer;

    MpmcAsyncQueue queue;
    auto producer = queue.Producer();
    auto consumer = queue.Consumer();
    std::array<std::atomic_uint8_t, kTotal> seen{};
    std::atomic<std::size_t> consumed{0};
    std::atomic<bool> invalidValue{false};
    std::atomic<bool> timedOut{false};

    std::vector<std::jthread> consumers;
    for (std::size_t index = 0; index != kConsumerCount; ++index) {
        consumers.emplace_back([port = consumer, &seen, &consumed, &invalidValue, &timedOut]() mutable {
            const auto deadline = std::chrono::steady_clock::now() + 10s;
            while (consumed.load(std::memory_order_acquire) != kTotal) {
                auto value = port.TryPop();
                if (!value.has_value()) {
                    if (std::chrono::steady_clock::now() >= deadline) {
                        timedOut.store(true, std::memory_order_relaxed);
                        return;
                    }
                    std::this_thread::yield();
                    continue;
                }
                if (*value < 0 || static_cast<std::size_t>(*value) >= kTotal) {
                    invalidValue.store(true, std::memory_order_relaxed);
                } else {
                    seen[static_cast<std::size_t>(*value)].fetch_add(1, std::memory_order_relaxed);
                }
                consumed.fetch_add(1, std::memory_order_release);
            }
        });
    }

    std::vector<std::jthread> producers;
    for (std::size_t producerIndex = 0; producerIndex != kProducerCount; ++producerIndex) {
        producers.emplace_back([port = producer, producerIndex, &timedOut]() mutable {
            for (std::size_t index = 0; index != kValuesPerProducer; ++index) {
                int value = static_cast<int>(producerIndex * kValuesPerProducer + index);
                while (!port.TryPush(value)) {
                    if (timedOut.load(std::memory_order_relaxed)) {
                        return;
                    }
                    std::this_thread::yield();
                }
            }
        });
    }
    for (auto& thread : producers) {
        thread.join();
    }
    for (auto& thread : consumers) {
        thread.join();
    }

    REQUIRE_FALSE(timedOut.load(std::memory_order_relaxed));
    REQUIRE_FALSE(invalidValue.load(std::memory_order_relaxed));
    REQUIRE(consumed.load(std::memory_order_relaxed) == kTotal);
    for (const auto& count : seen) {
        REQUIRE(count.load(std::memory_order_relaxed) == 1);
    }
}

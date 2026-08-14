/**
 * @file AsyncQueueStressTest.cpp
 * @brief High-contention sender and lifecycle stress for the asynchronous queue facade.
 * @ingroup Testing
 */
#include <Mashiro/Concurrency/AsyncQueue.h>

#include "Support/Meta.h"

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

using namespace Mashiro;
using namespace Mashiro::Concurrency;

namespace {

    enum class RaceCompletion : std::uint8_t { None, Value, Stopped, Error };

    struct RaceProbe {
        std::atomic_uint32_t completions{};
        std::atomic<RaceCompletion> completion{RaceCompletion::None};
        std::atomic<int> value{};
    };

    struct RaceReceiver {
        using receiver_concept = stdexec::receiver_t;

        RaceProbe* probe;
        stdexec::inplace_stop_token token;

        [[nodiscard]] auto get_env() const noexcept {
            return stdexec::env{stdexec::prop{stdexec::get_stop_token, token}};
        }

        void set_value(int input) && noexcept {
            probe->value.store(input, std::memory_order_relaxed);
            probe->completion.store(RaceCompletion::Value, std::memory_order_relaxed);
            probe->completions.fetch_add(1, std::memory_order_release);
        }

        void set_stopped() && noexcept {
            probe->completion.store(RaceCompletion::Stopped, std::memory_order_relaxed);
            probe->completions.fetch_add(1, std::memory_order_release);
        }

        void set_error(QueueError) && noexcept {
            probe->completion.store(RaceCompletion::Error, std::memory_order_relaxed);
            probe->completions.fetch_add(1, std::memory_order_release);
        }
    };

    template<class Queue>
    void RunDirectFacadeStress(std::size_t producerCount, std::size_t consumerCount, std::size_t itemsPerProducer) {
        const std::size_t total = producerCount * itemsPerProducer;
        Queue queue;
        auto producer = queue.Producer();
        auto consumer = queue.Consumer();
        std::vector<std::atomic_uint8_t> seen(total);
        for (auto& value : seen) {
            value.store(0, std::memory_order_relaxed);
        }
        std::atomic<std::size_t> active{producerCount};
        std::atomic<std::size_t> consumed{};
        std::atomic<bool> failed{};
        std::barrier start{static_cast<std::ptrdiff_t>(producerCount + consumerCount)};
        std::vector<std::jthread> workers;
        workers.reserve(producerCount + consumerCount);

        for (std::size_t producerIndex = 0; producerIndex != producerCount; ++producerIndex) {
            workers.emplace_back([&, producerIndex] {
                start.arrive_and_wait();
                for (std::size_t index = 0; index != itemsPerProducer && !failed.load(std::memory_order_relaxed);
                     ++index) {
                    int value = static_cast<int>(producerIndex * itemsPerProducer + index);
                    while (!producer.TryPush(value)) {
                        if (failed.load(std::memory_order_relaxed)) {
                            return;
                        }
                        std::this_thread::yield();
                    }
                }
                active.fetch_sub(1, std::memory_order_release);
            });
        }

        for (std::size_t consumerIndex = 0; consumerIndex != consumerCount; ++consumerIndex) {
            workers.emplace_back([&, consumerIndex] {
                (void)consumerIndex;
                start.arrive_and_wait();
                for (;;) {
                    if (auto value = consumer.TryPop()) {
                        if (*value < 0 || static_cast<std::size_t>(*value) >= total ||
                            seen[static_cast<std::size_t>(*value)].fetch_add(1, std::memory_order_relaxed) != 0) {
                            failed.store(true, std::memory_order_release);
                        }
                        consumed.fetch_add(1, std::memory_order_release);
                    } else if (failed.load(std::memory_order_acquire) ||
                               (active.load(std::memory_order_acquire) == 0 && queue.EmptyApprox())) {
                        return;
                    } else {
                        std::this_thread::yield();
                    }
                }
            });
        }

        workers.clear();
        queue.Close();

        REQUIRE_FALSE(failed.load(std::memory_order_acquire));
        REQUIRE(consumed.load(std::memory_order_acquire) == total);
        REQUIRE(queue.EmptyApprox());
        for (const auto& value : seen) {
            REQUIRE(value.load(std::memory_order_relaxed) == 1);
        }
    }

    template<std::size_t ProducerCount, std::size_t ItemsPerProducer>
    void RunParkedSenderStress() {
        using Storage = BoundedStack<int, 1, QueueTopologies::Mpsc>;
        using Queue = AsyncQueue<Storage>;

        constexpr std::size_t kTotal = ProducerCount * ItemsPerProducer;
        Queue queue;
        auto producer = queue.Producer();
        auto consumer = queue.Consumer();
        std::vector<std::atomic_uint8_t> seen(kTotal);
        std::atomic<std::size_t> producerDone{};
        std::atomic<std::size_t> consumerDone{};
        std::atomic<bool> failed{};
        std::atomic<bool> start{false};
        std::vector<std::thread> workers;
        workers.reserve(ProducerCount + 1);

        for (std::size_t producerIndex = 0; producerIndex != ProducerCount; ++producerIndex) {
            workers.emplace_back([&, producerIndex] {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                try {
                    for (std::size_t index = 0; index != ItemsPerProducer; ++index) {
                        const int value = static_cast<int>(producerIndex * ItemsPerProducer + index);
                        auto result = stdexec::sync_wait(producer.Push(value));
                        if (!result.has_value() || !std::get<0>(*result).Enqueued()) {
                            failed.store(true, std::memory_order_release);
                            break;
                        }
                    }
                } catch (...) {
                    failed.store(true, std::memory_order_release);
                }
                producerDone.fetch_add(1, std::memory_order_release);
            });
        }

        workers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            try {
                for (std::size_t count = 0; count != kTotal; ++count) {
                    auto result = stdexec::sync_wait(consumer.Pop());
                    if (!result.has_value()) {
                        failed.store(true, std::memory_order_release);
                        break;
                    }
                    const int value = std::get<0>(*result);
                    if (value < 0 || static_cast<std::size_t>(value) >= kTotal ||
                        seen[static_cast<std::size_t>(value)].fetch_add(1, std::memory_order_relaxed) != 0) {
                        failed.store(true, std::memory_order_release);
                    }
                }
            } catch (...) {
                failed.store(true, std::memory_order_release);
            }
            consumerDone.store(1, std::memory_order_release);
        });

        start.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while ((producerDone.load(std::memory_order_acquire) != ProducerCount ||
                consumerDone.load(std::memory_order_acquire) == 0) &&
               !failed.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        if (producerDone.load(std::memory_order_acquire) != ProducerCount ||
            consumerDone.load(std::memory_order_acquire) == 0) {
            failed.store(true, std::memory_order_release);
            queue.Abort();
        }
        for (auto& worker : workers) {
            worker.join();
        }

        REQUIRE_FALSE(failed.load(std::memory_order_acquire));
        for (const auto& value : seen) {
            REQUIRE(value.load(std::memory_order_relaxed) == 1);
        }
        REQUIRE(queue.EmptyApprox());
    }

    void RunStartStopTransferRace() {
        using Storage = BoundedStack<int, 1, QueueTopologies::Spsc>;
        using Queue = AsyncQueue<Storage>;

        Queue queue;
        auto producer = queue.Producer();
        auto consumer = queue.Consumer();
        for (int iteration = 0; iteration != 1'000; ++iteration) {
            stdexec::inplace_stop_source stopSource;
            RaceProbe probe;
            auto sender = consumer.Pop();
            auto operation = stdexec::connect(
                std::move(sender), RaceReceiver{.probe = &probe, .token = stopSource.get_token()});
            std::barrier start{3};

            std::jthread starter{[&] {
                start.arrive_and_wait();
                stdexec::start(operation);
            }};
            std::jthread stopper{[&] {
                start.arrive_and_wait();
                stopSource.request_stop();
            }};
            std::jthread pusher{[&] {
                start.arrive_and_wait();
                int value = iteration;
                while (!producer.TryPush(value)) {
                    std::this_thread::yield();
                }
            }};

            starter.join();
            stopper.join();
            pusher.join();

            REQUIRE(probe.completions.load(std::memory_order_acquire) == 1);
            const RaceCompletion completion = probe.completion.load(std::memory_order_relaxed);
            REQUIRE((completion == RaceCompletion::Value || completion == RaceCompletion::Stopped));
            auto remaining = consumer.TryPop();
            if (completion == RaceCompletion::Value) {
                REQUIRE(probe.value.load(std::memory_order_relaxed) == iteration);
                REQUIRE_FALSE(remaining.has_value());
            } else {
                REQUIRE(remaining.has_value());
                REQUIRE(*remaining == iteration);
            }
        }
    }

} // namespace

TEST_CASE("AsyncQueue facade stress preserves exactly-once MPSC transfers", AUTO_TAG_WITH("Stress")) {
    using Storage = BoundedStack<int, 1, QueueTopologies::Mpsc>;
    using Queue = AsyncQueue<Storage>;
    RunDirectFacadeStress<Queue>(4, 1, 10'000);
}

TEST_CASE("AsyncQueue facade stress preserves exactly-once MPMC transfers", AUTO_TAG_WITH("Stress")) {
    using Storage = BoundedStack<int, 3, QueueTopologies::Mpmc>;
    using Queue = AsyncQueue<Storage>;
    RunDirectFacadeStress<Queue>(4, 4, 8'000);
}

TEST_CASE("AsyncQueue parked sender stress preserves wakeup and ownership", AUTO_TAG_WITH("Stress")) {
    RunParkedSenderStress<4, 1'000>();
    RunParkedSenderStress<4, 1'000>();
}

TEST_CASE("AsyncQueue start stop and transfer race completes exactly once", AUTO_TAG_WITH("Stress")) {
    RunStartStopTransferRace();
}

/**
 * @file AsyncQueueTest.cpp
 * @brief Capability, lifecycle, cancellation, and concurrent data-path tests for asynchronous queues.
 * @ingroup Testing
 */
#include <Mashiro/Concurrency/AsyncQueue.h>

#include "Support/Meta.h"

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

using namespace Mashiro;
using namespace Mashiro::Concurrency;
using namespace std::chrono_literals;

namespace {

    using SpscStorage = RingBuffer<int, 4, QueueTopologies::Spsc>;
    using MpscStorage = RingBuffer<int, 1024, QueueTopologies::Mpsc>;
    using MpmcStorage = RingBuffer<int, 1024, QueueTopologies::Mpmc>;
    using SpscAsyncQueue = AsyncQueue<SpscStorage>;
    using MpscAsyncQueue = AsyncQueue<MpscStorage>;
    using MpmcAsyncQueue = AsyncQueue<MpmcStorage>;
    using Batch = QueueBatch<int, QueueAdapter<SpscStorage>::capacity>;
    using SpscRecordStorage = RecordRingBuffer<256, QueueTopologies::Spsc>;
    using MpscRecordStorage = RecordRingBuffer<8192, QueueTopologies::Mpsc>;
    using MpmcRecordStorage = RecordRingBuffer<8192, QueueTopologies::Mpmc>;
    using SpscRecordQueue = AsyncQueue<SpscRecordStorage>;
    using MpscRecordQueue = AsyncQueue<MpscRecordStorage>;
    using SpscRecord = typename SpscRecordQueue::value_type;

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

    template<class Port>
    concept HasPopBatch = requires(Port& port) { port.PopBatch(); };

    template<class Queue>
    concept HasSizeApprox = requires(const Queue& queue) { queue.SizeApprox(); };

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

    template<class Record>
    [[nodiscard]] Record MakeRecord(std::uint64_t value) {
        std::array<std::byte, sizeof(value)> bytes{};
        std::memcpy(bytes.data(), &value, sizeof(value));
        return Record{std::span<const std::byte>{bytes}};
    }

    template<class Record>
    [[nodiscard]] std::uint64_t RecordValue(const Record& record) noexcept {
        std::uint64_t value = 0;
        if (record.size() != sizeof(value)) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        std::memcpy(&value, record.data(), sizeof(value));
        return value;
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
    static_assert(QueueAdapter<MpscStorage>::pushProgress == QueueProgress::Unspecified);
    static_assert(QueueAdapter<MpscStorage>::popProgress == QueueProgress::Unspecified);
    static_assert(!QueueAdapter<MpscStorage>::transactionalPushBatch);
    static_assert(!HasPushBatch<MpscProducer, QueueBatch<int, QueueAdapter<MpscStorage>::capacity>>);
    static_assert(std::is_copy_constructible_v<MpscProducer>);
    static_assert(!std::is_copy_constructible_v<MpscConsumer>);
    static_assert(CanAttachFromLvalue<MpscProducer, Backpressure::Reject>);

    static_assert(MpmcAsyncQueue::producerCardinality == QueueCardinality::Multiple);
    static_assert(MpmcAsyncQueue::consumerCardinality == QueueCardinality::Multiple);
    static_assert(QueueAdapter<MpmcStorage>::pushProgress == QueueProgress::Unspecified);
    static_assert(QueueAdapter<MpmcStorage>::popProgress == QueueProgress::Unspecified);
    static_assert(!QueueAdapter<MpmcStorage>::transactionalPushBatch);
    static_assert(!HasPushBatch<MpmcProducer, QueueBatch<int, QueueAdapter<MpmcStorage>::capacity>>);
    static_assert(std::is_copy_constructible_v<MpmcProducer>);
    static_assert(std::is_copy_constructible_v<MpmcConsumer>);

    static_assert(AsyncQueueAdapter<QueueAdapter<SpscRecordStorage>>);
    static_assert(AsyncQueueAdapter<QueueAdapter<MpscRecordStorage>>);
    static_assert(AsyncQueueAdapter<QueueAdapter<MpmcRecordStorage>>);
    static_assert(QueueAdapter<SpscRecordStorage>::pushProgress == QueueProgress::WaitFree);
    static_assert(QueueAdapter<SpscRecordStorage>::popProgress == QueueProgress::WaitFree);
    static_assert(QueueAdapter<MpscRecordStorage>::pushProgress == QueueProgress::Unspecified);
    static_assert(QueueAdapter<MpscRecordStorage>::popProgress == QueueProgress::Unspecified);
    static_assert(QueueAdapter<MpmcRecordStorage>::pushProgress == QueueProgress::Unspecified);
    static_assert(QueueAdapter<MpmcRecordStorage>::popProgress == QueueProgress::Unspecified);
    static_assert(!BatchQueueAdapter<QueueAdapter<SpscRecordStorage>>);
    static_assert(!ApproximateSizedQueueAdapter<QueueAdapter<SpscRecordStorage>>);
    static_assert(!HasPopBatch<ConsumerPort<SpscRecordQueue>>);
    static_assert(!HasSizeApprox<SpscRecordQueue>);
    static_assert(std::same_as<SpscRecord, BoundedByteRecord<SpscRecordStorage::kMaxRecordBytes>>);

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

TEST_CASE("Record AsyncQueue owns sender inputs and preserves variable-length FIFO values", AUTO_TAG) {
    SpscRecordQueue queue;
    auto producer = queue.Producer();
    auto consumer = queue.Consumer();
    std::array<std::byte, 5> first{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}};
    std::array<std::byte, 3> second{std::byte{9}, std::byte{8}, std::byte{7}};

    auto firstSender = producer.Push(std::span<const std::byte>{first});
    first.fill(std::byte{0});
    REQUIRE(std::get<0>(*stdexec::sync_wait(std::move(firstSender))).Enqueued());
    REQUIRE(std::get<0>(*stdexec::sync_wait(producer.Push(std::span<const std::byte>{second}))).Enqueued());

    auto firstResult = stdexec::sync_wait(consumer.Pop());
    auto secondResult = stdexec::sync_wait(consumer.Pop());
    REQUIRE(firstResult.has_value());
    REQUIRE(secondResult.has_value());
    const auto& firstRecord = std::get<0>(*firstResult);
    const auto& secondRecord = std::get<0>(*secondResult);
    const std::array expectedFirst{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}};
    REQUIRE(std::ranges::equal(firstRecord.Bytes(), expectedFirst));
    REQUIRE(std::ranges::equal(secondRecord.Bytes(), second));
    REQUIRE(queue.EmptyApprox());
}

TEST_CASE("Record AsyncQueue reports an empty value as a permanent sender error", AUTO_TAG) {
    SpscRecordQueue queue;
    auto producer = queue.Producer();
    SpscRecord empty;

    QueueError caught{};
    try {
        static_cast<void>(stdexec::sync_wait(producer.Push(std::move(empty))));
        FAIL("an empty record must not park as if the byte ring were full");
    } catch (QueueError error) {
        caught = error;
    }
    REQUIRE(caught == kQueueInvalidValue);
    REQUIRE(queue.EmptyApprox());
}

TEST_CASE("Record AsyncQueue resumes a parked producer after byte capacity is reclaimed", AUTO_TAG) {
    SpscRecordQueue queue;
    auto producer = queue.Producer();
    auto consumer = queue.Consumer();
    std::size_t enqueued = 0;
    for (;;) {
        auto record = MakeRecord<SpscRecord>(enqueued + 1);
        if (!producer.TryPush(record)) {
            break;
        }
        ++enqueued;
    }
    REQUIRE(enqueued > 0);

    CompletionProbe probe;
    auto sender = producer.Push(MakeRecord<SpscRecord>(enqueued + 1));
    auto operation = stdexec::connect(std::move(sender), VoidReceiver{.probe = &probe});
    stdexec::start(operation);
    REQUIRE(probe.completion.load(std::memory_order_acquire) == Completion::None);

    auto first = consumer.TryPop();
    REQUIRE(first.has_value());
    REQUIRE(RecordValue(*first) == 1);
    REQUIRE(probe.completion.load(std::memory_order_acquire) == Completion::Value);
    REQUIRE(probe.disposition.load(std::memory_order_relaxed) == PushDisposition::Enqueued);
}

TEST_CASE("Record AsyncQueue MPSC adapter transfers every owning record exactly once", AUTO_TAG) {
    constexpr std::size_t kProducerCount = 4;
    constexpr std::size_t kRecordsPerProducer = 256;
    constexpr std::size_t kTotal = kProducerCount * kRecordsPerProducer;

    MpscRecordQueue queue;
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
            auto record = consumer.TryPop();
            if (!record.has_value()) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    timedOut.store(true, std::memory_order_relaxed);
                    return;
                }
                std::this_thread::yield();
                continue;
            }
            const std::uint64_t value = RecordValue(*record);
            if (value >= kTotal) {
                invalidValue.store(true, std::memory_order_relaxed);
            } else if (seen[value]) {
                duplicateValue.store(true, std::memory_order_relaxed);
            } else {
                seen[value] = true;
            }
            consumed.fetch_add(1, std::memory_order_relaxed);
        }
    }};

    std::vector<std::jthread> producers;
    for (std::size_t producerIndex = 0; producerIndex != kProducerCount; ++producerIndex) {
        producers.emplace_back([port = producer, producerIndex, &timedOut]() mutable {
            for (std::size_t index = 0; index != kRecordsPerProducer; ++index) {
                const std::uint64_t id = producerIndex * kRecordsPerProducer + index;
                auto record = MakeRecord<typename MpscRecordQueue::value_type>(id);
                while (!port.TryPush(record)) {
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

TEST_CASE("AsyncQueue sync_wait domain preserves push composition", AUTO_TAG) {
    SpscAsyncQueue queue;
    auto producer = queue.Producer();

    auto push = producer.Push(42) | stdexec::then([](PushOutcome outcome) { return outcome.Enqueued(); });
    auto pushed = stdexec::sync_wait(std::move(push));
    REQUIRE(pushed.has_value());
    REQUIRE(std::get<0>(*pushed));
}

TEST_CASE("AsyncQueue sync_wait domain preserves pop composition", AUTO_TAG) {
    SpscAsyncQueue queue;
    auto producer = queue.Producer();
    auto consumer = queue.Consumer();
    REQUIRE(stdexec::sync_wait(producer.Push(42)).has_value());

    auto pop = consumer.Pop() | stdexec::then([](int value) { return value * 2; });
    auto popped = stdexec::sync_wait(std::move(pop));
    REQUIRE(popped.has_value());
    REQUIRE(std::get<0>(*popped) == 84);
}

TEST_CASE("AsyncQueue sync_wait domain preserves queue errors", AUTO_TAG) {
    SpscAsyncQueue queue;
    auto producer = queue.Producer();

    queue.Close();
    QueueError caught{};
    try {
        static_cast<void>(stdexec::sync_wait(producer.Push(7)));
        FAIL("sync_wait must propagate a closed queue error");
    } catch (QueueError error) {
        caught = error;
    }
    REQUIRE(caught == kQueueClosed);
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
    using SmallQueue = AsyncQueue<RingBuffer<int, 2, QueueTopologies::Spsc>>;

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
    using SmallQueue = AsyncQueue<RingBuffer<int, 2, QueueTopologies::Spsc>>;

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
    using SmallQueue = AsyncQueue<RingBuffer<int, 2, QueueTopologies::Spsc>>;
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
    using SmallQueue = AsyncQueue<RingBuffer<int, 2, QueueTopologies::Spsc>>;
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

/**
 * @file RecordRingBufferTest.cpp
 * @brief Correctness and concurrency tests for packed variable-length record ring buffers.
 * @ingroup Testing
 */
#include "Mashiro/Core/RecordRingBuffer.h"

#include "Support/Meta.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace Mashiro;

namespace {

    template<class T>
    [[nodiscard]] std::span<const std::byte> AsBytes(const T& value) noexcept {
        return std::as_bytes(std::span{&value, 1});
    }

    template<class T>
    [[nodiscard]] T ReadObject(std::span<const std::byte> record) {
        REQUIRE(record.size() >= sizeof(T));
        T value{};
        std::memcpy(&value, record.data(), sizeof(T));
        return value;
    }

    struct RecordIdentity {
        std::uint32_t producer;
        std::uint32_t payloadBytes;
        std::uint64_t sequence;
    };

    [[nodiscard]] bool ValidateIdentity(std::span<const std::byte> record, RecordIdentity& identity) {
        if (record.size() < sizeof(identity)) {
            return false;
        }
        std::memcpy(&identity, record.data(), sizeof(identity));
        if (identity.payloadBytes != record.size()) {
            return false;
        }
        const std::byte expected = static_cast<std::byte>((identity.producer * 31 + identity.sequence) & 0xFF);
        for (std::byte value : record.subspan(sizeof(identity))) {
            if (value != expected) {
                return false;
            }
        }
        return true;
    }

    template<class Ring>
    void WriteIdentities(Ring& ring, std::uint32_t producer, std::uint64_t count,
                         const std::atomic<bool>& stop) {
        std::array<std::byte, 128> payload{};
        for (std::uint64_t sequence = 1; sequence <= count; ++sequence) {
            const std::size_t size = sizeof(RecordIdentity) + sequence % (payload.size() - sizeof(RecordIdentity));
            const RecordIdentity identity{producer, static_cast<std::uint32_t>(size), sequence};
            std::memcpy(payload.data(), &identity, sizeof(identity));
            std::memset(payload.data() + sizeof(identity),
                        static_cast<int>((producer * 31 + sequence) & 0xFF), size - sizeof(identity));
            while (!ring.TryWrite({payload.data(), size})) {
                if (stop.load(std::memory_order_relaxed)) {
                    return;
                }
                std::this_thread::yield();
            }
        }
    }

    struct FixedTransferResult {
        std::uint64_t consumed;
        std::uint64_t checksum;
        std::size_t bytesPending;
        bool failed;
        bool empty;
    };

    template<Concurrency::QueueTopology Topology, std::size_t Producers, std::size_t Consumers,
             std::size_t PayloadBytes>
    [[nodiscard]] FixedTransferResult TransferFixedRecords(std::uint64_t recordsPerProducer) {
        RecordRingBuffer<64 * 1024, Topology> ring;
        std::barrier start{static_cast<std::ptrdiff_t>(Producers + Consumers)};
        std::atomic<std::size_t> producersDone{0};
        std::atomic<std::uint64_t> consumed{0};
        std::atomic<std::uint64_t> checksum{0};
        std::atomic<bool> failed{false};
        std::atomic<bool> stop{false};
        std::array<std::thread, Producers> producers;
        std::array<std::thread, Consumers> consumers;

        for (std::size_t producer = 0; producer < Producers; ++producer) {
            producers[producer] = std::thread([&, producer] {
                std::array<std::byte, PayloadBytes> payload{};
                const std::uint64_t first = producer * recordsPerProducer + 1;
                start.arrive_and_wait();
                for (std::uint64_t offset = 0; offset < recordsPerProducer; ++offset) {
                    const std::uint64_t id = first + offset;
                    std::memcpy(payload.data(), &id, sizeof(id));
                    while (!ring.TryWrite(payload)) {
                        if (stop.load(std::memory_order_relaxed)) {
                            producersDone.fetch_add(1, std::memory_order_release);
                            return;
                        }
                        std::this_thread::yield();
                    }
                }
                producersDone.fetch_add(1, std::memory_order_release);
            });
        }
        for (std::thread& consumer : consumers) {
            consumer = std::thread([&] {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
                start.arrive_and_wait();
                for (;;) {
                    if (stop.load(std::memory_order_relaxed)) {
                        break;
                    }
                    if (ring.TryConsume([&](std::span<const std::byte> record) {
                            std::uint64_t id = 0;
                            if (record.size() != PayloadBytes) {
                                failed.store(true, std::memory_order_relaxed);
                            }
                            std::memcpy(&id, record.data(), sizeof(id));
                            checksum.fetch_add(id, std::memory_order_relaxed);
                            consumed.fetch_add(1, std::memory_order_release);
                        })) {
                        continue;
                    }
                    if (producersDone.load(std::memory_order_acquire) == Producers && ring.Empty()) {
                        break;
                    }
                    if (std::chrono::steady_clock::now() >= deadline) {
                        failed.store(true, std::memory_order_relaxed);
                        stop.store(true, std::memory_order_relaxed);
                        break;
                    }
                    std::this_thread::yield();
                }
            });
        }

        for (std::thread& producer : producers) {
            producer.join();
        }
        for (std::thread& consumer : consumers) {
            consumer.join();
        }
        return {consumed.load(), checksum.load(), ring.BytesPending(), failed.load(), ring.Empty()};
    }

} // namespace

TEST_CASE("RecordRingBuffer SPSC publishes and consumes one complete record", AUTO_TAG) {
    RecordRingBuffer<256> ring;
    const std::uint64_t input = 0x1234'5678'9ABC'DEF0;

    REQUIRE(ring.TryWrite(AsBytes(input)));
    REQUIRE(ring.BytesPending() == 32);

    std::uint64_t output = 0;
    REQUIRE(ring.TryConsume(
        [&](std::span<const std::byte> record) { output = ReadObject<std::uint64_t>(record); }));
    REQUIRE(output == input);
    REQUIRE(ring.Empty());
}

TEST_CASE("RecordRingBuffer SPSC enforces payload and byte capacity", AUTO_TAG) {
    RecordRingBuffer<128> ring;
    std::array<std::byte, 48> payload{};
    std::array<std::byte, 49> oversized{};

    STATIC_REQUIRE(RecordRingBuffer<128>::GetByteCapacity() == 128);
    STATIC_REQUIRE(RecordRingBuffer<128>::GetMaxRecordBytes() == 48);
    REQUIRE(!ring.TryWrite({}));
    REQUIRE(!ring.TryWrite(nullptr, payload.size()));
    REQUIRE(!ring.TryWrite(oversized));
    REQUIRE(ring.TryWrite(payload));
    REQUIRE(ring.TryWrite(payload));
    REQUIRE(!ring.TryWrite(payload));
}

TEST_CASE("RecordRingBuffer SPSC accepts a maximum record after worst-case wrap padding", AUTO_TAG) {
    RecordRingBuffer<128> ring;
    std::array<std::byte, 1> first{};
    std::array<std::byte, 17> second{};
    std::array<std::byte, RecordRingBuffer<128>::GetMaxRecordBytes()> maximum{};

    REQUIRE(ring.TryWrite(first));
    REQUIRE(ring.TryConsume([](std::span<const std::byte>) {}));
    REQUIRE(ring.TryWrite(second));
    REQUIRE(ring.TryConsume([](std::span<const std::byte>) {}));

    REQUIRE(ring.TryWrite(maximum));
    REQUIRE(ring.BytesPending() == 112);
    REQUIRE(ring.TryConsume([&](std::span<const std::byte> record) { REQUIRE(record.size() == maximum.size()); }));
    REQUIRE(ring.Empty());
}

TEST_CASE("RecordRingBuffer SPSC inserts and consumes physical wrap padding", AUTO_TAG) {
    RecordRingBuffer<256> ring;
    std::array<std::byte, 24> payload{};

    for (std::uint64_t id = 1; id <= 5; ++id) {
        std::memcpy(payload.data(), &id, sizeof(id));
        REQUIRE(ring.TryWrite(payload));
    }

    for (std::uint64_t expected = 1; expected <= 3; ++expected) {
        REQUIRE(ring.TryConsume(
            [&](std::span<const std::byte> record) { REQUIRE(ReadObject<std::uint64_t>(record) == expected); }));
    }

    for (std::uint64_t id = 6; id <= 8; ++id) {
        std::memcpy(payload.data(), &id, sizeof(id));
        REQUIRE(ring.TryWrite(payload));
    }

    for (std::uint64_t expected = 4; expected <= 8; ++expected) {
        REQUIRE(ring.TryConsume([&](std::span<const std::byte> record) {
            REQUIRE(ReadObject<std::uint64_t>(record) == expected);
        }));
    }
    REQUIRE(ring.Empty());
}

TEST_CASE("RecordRingBuffer SPSC drain observes one committed entry snapshot", AUTO_TAG) {
    RecordRingBuffer<256> ring;
    std::uint64_t first = 1;
    std::uint64_t deferred = 2;
    REQUIRE(ring.TryWrite(AsBytes(first)));

    const std::size_t count = ring.Drain([&](std::span<const std::byte> record) {
        REQUIRE(ReadObject<std::uint64_t>(record) == first);
        REQUIRE(ring.TryWrite(AsBytes(deferred)));
    });

    REQUIRE(count == 1);
    REQUIRE(ring.TryConsume(
        [&](std::span<const std::byte> record) { REQUIRE(ReadObject<std::uint64_t>(record) == deferred); }));
    REQUIRE(ring.Empty());
}

TEST_CASE("RecordRingBuffer SPSC consumes a throwing callback record at most once", AUTO_TAG) {
    RecordRingBuffer<256> ring;
    for (std::uint64_t value : {1, 2, 3}) {
        REQUIRE(ring.TryWrite(AsBytes(value)));
    }

    REQUIRE_THROWS_AS(
        ring.TryConsume([](std::span<const std::byte> record) {
            REQUIRE(ReadObject<std::uint64_t>(record) == 1);
            throw std::runtime_error("consumer failed");
        }),
        std::runtime_error);

    std::vector<std::uint64_t> remaining;
    ring.Drain(
        [&](std::span<const std::byte> record) { remaining.push_back(ReadObject<std::uint64_t>(record)); });
    REQUIRE(remaining == std::vector<std::uint64_t>{2, 3});
}

TEST_CASE("RecordRingBuffer SPSC reset releases all occupied bytes", AUTO_TAG) {
    RecordRingBuffer<256> ring;
    const std::uint64_t value = 42;
    REQUIRE(ring.TryWrite(AsBytes(value)));
    REQUIRE(ring.TryWrite(AsBytes(value)));
    REQUIRE(!ring.Empty());

    ring.Reset();

    REQUIRE(ring.Empty());
    REQUIRE(!ring.TryConsume([](std::span<const std::byte>) {}));
}

TEST_CASE("RecordRingBuffer SPSC transfers mixed record sizes across threads", AUTO_TAG) {
    constexpr std::uint64_t kRecords = 100'000;
    RecordRingBuffer<4096> ring;
    std::atomic<bool> producerDone{false};
    std::atomic<bool> failed{false};

    std::thread producer([&] {
        std::array<std::byte, 128> payload{};
        for (std::uint64_t id = 1; id <= kRecords; ++id) {
            const std::size_t size = sizeof(id) + id % (payload.size() - sizeof(id));
            std::memcpy(payload.data(), &id, sizeof(id));
            while (!ring.TryWrite({payload.data(), size})) {
                std::this_thread::yield();
            }
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        std::uint64_t expected = 1;
        while (expected <= kRecords) {
            if (ring.TryConsume([&](std::span<const std::byte> record) {
                    std::uint64_t actual = 0;
                    if (record.size() < sizeof(actual)) {
                        failed.store(true, std::memory_order_relaxed);
                    } else {
                        std::memcpy(&actual, record.data(), sizeof(actual));
                        if (actual != expected) {
                            failed.store(true, std::memory_order_relaxed);
                        }
                    }
                    ++expected;
                })) {
                continue;
            }
            if (producerDone.load(std::memory_order_acquire) && ring.Empty()) {
                break;
            }
            std::this_thread::yield();
        }
        if (expected != kRecords + 1) {
            failed.store(true, std::memory_order_relaxed);
        }
    });

    producer.join();
    consumer.join();
    REQUIRE(!failed.load(std::memory_order_relaxed));
    REQUIRE(ring.Empty());
}

TEST_CASE("RecordRingBuffer SPSC layout separates endpoint-owned positions", AUTO_TAG) {
    STATIC_REQUIRE(Concurrency::AuditFalseSharing<RecordRingBuffer<4096>>());
}

TEST_CASE("RecordRingBuffer Local preserves packed-record order without atomic endpoints", AUTO_TAG) {
    RecordRingBuffer<256, Concurrency::QueueTopologies::Local> ring;
    for (std::uint64_t value = 1; value <= 8; ++value) {
        while (!ring.TryWrite(AsBytes(value))) {
            REQUIRE(ring.TryConsume([&](std::span<const std::byte> record) {
                REQUIRE(ReadObject<std::uint64_t>(record) < value);
            }));
        }
    }

    std::uint64_t previous = 0;
    ring.Drain([&](std::span<const std::byte> record) {
        const std::uint64_t value = ReadObject<std::uint64_t>(record);
        REQUIRE(value > previous);
        previous = value;
    });
    REQUIRE(ring.Empty());
}

TEST_CASE("RecordRingBuffer MPSC transfers every producer record in per-producer order", AUTO_TAG) {
    constexpr std::uint32_t kProducers = 4;
    constexpr std::uint64_t kPerProducer = 20'000;
    constexpr std::uint64_t kTotal = kProducers * kPerProducer;
    RecordRingBuffer<8192, Concurrency::QueueTopologies::Mpsc> ring;
    std::atomic<std::uint32_t> producersDone{0};
    std::atomic<bool> failed{false};
    std::atomic<bool> stop{false};
    std::array<std::thread, kProducers> producers;

    for (std::uint32_t producer = 0; producer < kProducers; ++producer) {
        producers[producer] = std::thread([&, producer] {
            WriteIdentities(ring, producer, kPerProducer, stop);
            producersDone.fetch_add(1, std::memory_order_release);
        });
    }

    std::array<std::uint64_t, kProducers> expected{};
    std::uint64_t consumed = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (consumed != kTotal) {
        if (ring.TryConsume([&](std::span<const std::byte> record) {
                RecordIdentity identity{};
                if (!ValidateIdentity(record, identity) || identity.producer >= kProducers ||
                    identity.sequence != ++expected[identity.producer]) {
                    failed.store(true, std::memory_order_relaxed);
                }
                ++consumed;
            })) {
            continue;
        }
        if (producersDone.load(std::memory_order_acquire) == kProducers && ring.Empty()) {
            failed.store(true, std::memory_order_relaxed);
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            failed.store(true, std::memory_order_relaxed);
            stop.store(true, std::memory_order_relaxed);
            break;
        }
        std::this_thread::yield();
    }

    for (std::thread& producer : producers) {
        producer.join();
    }
    CAPTURE(consumed, producersDone.load(), ring.BytesPending());
    REQUIRE(consumed == kTotal);
    REQUIRE(!failed.load(std::memory_order_relaxed));
    REQUIRE(ring.Empty());
}

TEST_CASE("RecordRingBuffer SPMC never reclaims a payload before its callback completes", AUTO_TAG) {
    RecordRingBuffer<256, Concurrency::QueueTopologies::Spmc> ring;
    for (std::uint64_t value = 1; value <= 8; ++value) {
        REQUIRE(ring.TryWrite(AsBytes(value)));
    }

    std::atomic<bool> firstClaimed{false};
    std::atomic<bool> releaseFirst{false};
    std::atomic<bool> failed{false};
    std::thread delayed([&] {
        if (!ring.TryConsume([&](std::span<const std::byte> record) {
            if (ReadObject<std::uint64_t>(record) != 1) {
                failed.store(true, std::memory_order_relaxed);
            }
            firstClaimed.store(true, std::memory_order_release);
            firstClaimed.notify_one();
            releaseFirst.wait(false, std::memory_order_acquire);
        })) {
            failed.store(true, std::memory_order_relaxed);
        }
    });
    firstClaimed.wait(false, std::memory_order_acquire);

    std::uint64_t consumed = 0;
    std::thread follower([&] {
        while (consumed != 7) {
            if (ring.TryConsume([&](std::span<const std::byte> record) {
                    const std::uint64_t value = ReadObject<std::uint64_t>(record);
                    if (value < 2 || value > 8) {
                        failed.store(true, std::memory_order_relaxed);
                    }
                    ++consumed;
                })) {
                continue;
            }
            std::this_thread::yield();
        }
    });
    follower.join();

    const std::uint64_t deferred = 9;
    REQUIRE(ring.BytesPending() == 256);
    REQUIRE(!ring.TryWrite(AsBytes(deferred)));
    releaseFirst.store(true, std::memory_order_release);
    releaseFirst.notify_one();
    delayed.join();
    REQUIRE(!failed.load(std::memory_order_relaxed));
    REQUIRE(ring.TryWrite(AsBytes(deferred)));
    REQUIRE(ring.TryConsume(
        [&](std::span<const std::byte> record) { REQUIRE(ReadObject<std::uint64_t>(record) == deferred); }));
    REQUIRE(ring.Empty());
}

TEST_CASE("RecordRingBuffer SPMC completes a throwing callback exactly once", AUTO_TAG) {
    RecordRingBuffer<256, Concurrency::QueueTopologies::Spmc> ring;
    for (std::uint64_t value : {1, 2, 3}) {
        REQUIRE(ring.TryWrite(AsBytes(value)));
    }

    REQUIRE_THROWS_AS(
        ring.TryConsume([](std::span<const std::byte> record) {
            REQUIRE(ReadObject<std::uint64_t>(record) == 1);
            throw std::runtime_error("consumer failed");
        }),
        std::runtime_error);

    std::vector<std::uint64_t> remaining;
    ring.Drain(
        [&](std::span<const std::byte> record) { remaining.push_back(ReadObject<std::uint64_t>(record)); });
    REQUIRE(remaining == std::vector<std::uint64_t>{2, 3});
    REQUIRE(ring.Empty());
}

TEST_CASE("RecordRingBuffer MPMC transfers mixed records exactly once", AUTO_TAG) {
    constexpr std::uint32_t kProducers = 4;
    constexpr std::uint32_t kConsumers = 4;
    constexpr std::uint64_t kPerProducer = 12'000;
    constexpr std::uint64_t kTotal = kProducers * kPerProducer;
    RecordRingBuffer<8192, Concurrency::QueueTopologies::Mpmc> ring;
    auto seen = std::make_unique<std::atomic<std::uint8_t>[]>(kTotal);
    std::atomic<std::uint64_t> consumed{0};
    std::atomic<std::uint32_t> producersDone{0};
    std::atomic<bool> failed{false};
    std::atomic<bool> stop{false};
    std::array<std::thread, kProducers> producers;
    std::array<std::thread, kConsumers> consumers;

    for (std::uint32_t producer = 0; producer < kProducers; ++producer) {
        producers[producer] = std::thread([&, producer] {
            WriteIdentities(ring, producer, kPerProducer, stop);
            producersDone.fetch_add(1, std::memory_order_release);
        });
    }
    for (std::thread& consumer : consumers) {
        consumer = std::thread([&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
            while (!stop.load(std::memory_order_relaxed) && consumed.load(std::memory_order_acquire) != kTotal) {
                if (ring.TryConsume([&](std::span<const std::byte> record) {
                        RecordIdentity identity{};
                        if (!ValidateIdentity(record, identity) || identity.producer >= kProducers ||
                            identity.sequence == 0 || identity.sequence > kPerProducer) {
                            failed.store(true, std::memory_order_relaxed);
                        } else {
                            const std::uint64_t index = identity.producer * kPerProducer + identity.sequence - 1;
                            if (seen[index].fetch_add(1, std::memory_order_relaxed) != 0) {
                                failed.store(true, std::memory_order_relaxed);
                            }
                        }
                        consumed.fetch_add(1, std::memory_order_release);
                    })) {
                    continue;
                }
                if (producersDone.load(std::memory_order_acquire) == kProducers && ring.Empty()) {
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    failed.store(true, std::memory_order_relaxed);
                    stop.store(true, std::memory_order_relaxed);
                    break;
                }
                std::this_thread::yield();
            }
        });
    }

    for (std::thread& producer : producers) {
        producer.join();
    }
    for (std::thread& consumer : consumers) {
        consumer.join();
    }
    CAPTURE(consumed.load(), producersDone.load(), ring.BytesPending());
    for (std::uint64_t index = 0; index < kTotal; ++index) {
        REQUIRE(seen[index].load(std::memory_order_relaxed) == 1);
    }
    REQUIRE(consumed.load(std::memory_order_relaxed) == kTotal);
    REQUIRE(!failed.load(std::memory_order_relaxed));
    REQUIRE(ring.Empty());
}

TEST_CASE("RecordRingBuffer MPMC cannot lose a completed-prefix reclaim handoff", AUTO_TAG) {
    constexpr std::uint64_t kPerProducer = 1'000;
    constexpr std::uint64_t kTotal = 4 * kPerProducer;
    const FixedTransferResult result =
        TransferFixedRecords<Concurrency::QueueTopologies::Mpmc, 4, 4, 64>(kPerProducer);

    CAPTURE(result.consumed, result.checksum, result.bytesPending);
    REQUIRE(result.consumed == kTotal);
    REQUIRE(result.checksum == kTotal * (kTotal + 1) / 2);
    REQUIRE(!result.failed);
    REQUIRE(result.empty);
}

TEST_CASE("RecordRingBuffer SPMC reclaims a long fixed-record stream without losing a handoff", AUTO_TAG) {
    constexpr std::uint64_t kTotal = 100'000;
    const FixedTransferResult result =
        TransferFixedRecords<Concurrency::QueueTopologies::Spmc, 1, 4, 1024>(kTotal);

    CAPTURE(result.consumed, result.checksum, result.bytesPending);
    REQUIRE(result.consumed == kTotal);
    REQUIRE(result.checksum == kTotal * (kTotal + 1) / 2);
    REQUIRE(!result.failed);
    REQUIRE(result.empty);
}

TEST_CASE("RecordRingBuffer concurrent layouts isolate producer, consumer, and arena writes", AUTO_TAG) {
    STATIC_REQUIRE(Concurrency::AuditFalseSharing<
                   RecordRingBuffer<4096, Concurrency::QueueTopologies::Mpsc>>());
    STATIC_REQUIRE(Concurrency::AuditFalseSharing<
                   RecordRingBuffer<4096, Concurrency::QueueTopologies::Spmc>>());
    STATIC_REQUIRE(Concurrency::AuditFalseSharing<
                   RecordRingBuffer<4096, Concurrency::QueueTopologies::Mpmc>>());
}

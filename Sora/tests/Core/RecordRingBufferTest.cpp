/**
 * @file RecordRingBufferTest.cpp
 * @brief Correctness and concurrency tests for packed variable-length record ring buffers.
 * @ingroup Testing
 */

#include <Sora/Core/Concurrency/RecordRingBuffer.h>

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

namespace {

    using Sora::Concurrency::RecordRingBuffer;
    namespace Topology = Sora::Concurrency::QueueTopologies;

    template<class T>
    [[nodiscard]] std::span<const std::byte> AsBytes(const T& value) noexcept {
        return std::as_bytes(std::span{&value, 1});
    }

    template<class T>
    [[nodiscard]] T ReadObject(std::span<const std::byte> record) noexcept {
        T value{};
        if (record.size_bytes() >= sizeof(T)) {
            std::memcpy(&value, record.data(), sizeof(T));
        }
        return value;
    }

    struct RecordIdentity {
        uint32_t producer;
        uint32_t payloadBytes;
        uint64_t sequence;
    };

    [[nodiscard]] bool ValidateIdentity(std::span<const std::byte> record, RecordIdentity& identity) noexcept {
        if (record.size_bytes() < sizeof(identity)) {
            return false;
        }
        std::memcpy(&identity, record.data(), sizeof(identity));
        if (identity.payloadBytes != record.size_bytes()) {
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
    void WriteIdentities(Ring& ring, uint32_t producer, uint64_t count, const std::atomic<bool>& stop) {
        std::array<std::byte, 128> payload{};
        for (uint64_t sequence = 1; sequence <= count; ++sequence) {
            const size_t size = sizeof(RecordIdentity) + sequence % (payload.size() - sizeof(RecordIdentity));
            const RecordIdentity identity{producer, static_cast<uint32_t>(size), sequence};
            std::memcpy(payload.data(), &identity, sizeof(identity));
            std::memset(payload.data() + sizeof(identity), static_cast<int>((producer * 31 + sequence) & 0xFF),
                        size - sizeof(identity));
            while (!ring.TryWrite({payload.data(), size})) {
                if (stop.load(std::memory_order_relaxed)) {
                    return;
                }
                std::this_thread::yield();
            }
        }
    }

    struct TransferResult {
        uint64_t consumed{};
        uint64_t checksum{};
        size_t occupiedBytes{};
        bool failed{};
        bool empty{};
    };

    template<Sora::Concurrency::QueueTopology TopologyValue, size_t Producers, size_t Consumers, size_t PayloadBytes>
    [[nodiscard]] TransferResult TransferFixedRecords(uint64_t recordsPerProducer) {
        RecordRingBuffer<64 * 1024, TopologyValue> ring;
        std::barrier start{static_cast<std::ptrdiff_t>(Producers + Consumers)};
        std::atomic<size_t> producersDone{0};
        std::atomic<uint64_t> consumed{0};
        std::atomic<uint64_t> checksum{0};
        std::atomic<bool> failed{false};
        std::atomic<bool> stop{false};
        std::array<std::thread, Producers> producers;
        std::array<std::thread, Consumers> consumers;

        for (size_t producer = 0; producer < Producers; ++producer) {
            producers[producer] = std::thread([&, producer] {
                std::array<std::byte, PayloadBytes> payload{};
                const uint64_t first = producer * recordsPerProducer + 1;
                start.arrive_and_wait();
                for (uint64_t offset = 0; offset < recordsPerProducer; ++offset) {
                    const uint64_t id = first + offset;
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
                            uint64_t id = 0;
                            if (record.size_bytes() != PayloadBytes) {
                                failed.store(true, std::memory_order_relaxed);
                            }
                            std::memcpy(&id, record.data(), sizeof(id));
                            checksum.fetch_add(id, std::memory_order_relaxed);
                            consumed.fetch_add(1, std::memory_order_release);
                        })) {
                        continue;
                    }
                    if (producersDone.load(std::memory_order_acquire) == Producers && ring.IsEmpty()) {
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
        return {consumed.load(), checksum.load(), ring.OccupiedBytesApprox(), failed.load(), ring.IsEmpty()};
    }

} // namespace

TEST_CASE("RecordRingBuffer publishes, consumes, and bounds one SPSC record", "[Sora.Core.RecordRingBuffer]") {
    RecordRingBuffer<256> ring;
    const uint64_t input = 0x1234'5678'9ABC'DEF0;

    STATIC_REQUIRE(RecordRingBuffer<256>::kByteCapacity == 256);
    STATIC_REQUIRE(RecordRingBuffer<256>::kMaxPayloadBytes == 112);
    STATIC_REQUIRE(RecordRingBuffer<256, Topology::Local>::kMaxPayloadBytes == 124);
    REQUIRE(ring.TryWrite(AsBytes(input)));
    REQUIRE(ring.OccupiedBytesApprox() == 32);

    uint64_t output = 0;
    REQUIRE(ring.TryConsume([&](std::span<const std::byte> record) { output = ReadObject<uint64_t>(record); }));
    REQUIRE(output == input);
    REQUIRE(ring.IsEmpty());

    std::array<std::byte, 113> oversized{};
    REQUIRE(!ring.TryWrite({}));
    REQUIRE(!ring.TryWrite(nullptr, sizeof(input)));
    REQUIRE(!ring.TryWrite(oversized));
}

TEST_CASE("RecordRingBuffer inserts wrap padding and drains one reservation snapshot", "[Sora.Core.RecordRingBuffer]") {
    RecordRingBuffer<256> ring;
    std::array<std::byte, 24> payload{};
    for (uint64_t id = 1; id <= 5; ++id) {
        std::memcpy(payload.data(), &id, sizeof(id));
        REQUIRE(ring.TryWrite(payload));
    }
    for (uint64_t expected = 1; expected <= 3; ++expected) {
        REQUIRE(ring.TryConsume(
            [&](std::span<const std::byte> record) { CHECK(ReadObject<uint64_t>(record) == expected); }));
    }
    for (uint64_t id = 6; id <= 8; ++id) {
        std::memcpy(payload.data(), &id, sizeof(id));
        REQUIRE(ring.TryWrite(payload));
    }
    REQUIRE(ring.OccupiedBytesApprox() == 256);

    std::vector<uint64_t> values;
    const size_t drained =
        ring.Drain([&](std::span<const std::byte> record) { values.push_back(ReadObject<uint64_t>(record)); });
    CHECK(drained == 5);
    CHECK(values == std::vector<uint64_t>{4, 5, 6, 7, 8});
    REQUIRE(ring.IsEmpty());

    const uint64_t first = 9;
    const uint64_t deferred = 10;
    REQUIRE(ring.TryWrite(AsBytes(first)));
    CHECK(ring.Drain([&](std::span<const std::byte> record) {
        CHECK(ReadObject<uint64_t>(record) == first);
        CHECK(ring.TryWrite(AsBytes(deferred)));
    }) == 1);
    REQUIRE(
        ring.TryConsume([&](std::span<const std::byte> record) { CHECK(ReadObject<uint64_t>(record) == deferred); }));
    REQUIRE(ring.IsEmpty());
}

TEST_CASE("RecordRingBuffer consumes a throwing callback at most once", "[Sora.Core.RecordRingBuffer]") {
    RecordRingBuffer<256> ring;
    for (uint64_t value : {1, 2, 3}) {
        REQUIRE(ring.TryWrite(AsBytes(value)));
    }
    REQUIRE_THROWS_AS(ring.TryConsume([](std::span<const std::byte> record) {
        CHECK(ReadObject<uint64_t>(record) == 1);
        throw std::runtime_error("consumer failed");
    }),
                      std::runtime_error);

    std::vector<uint64_t> remaining;
    ring.Drain([&](std::span<const std::byte> record) { remaining.push_back(ReadObject<uint64_t>(record)); });
    REQUIRE(remaining == std::vector<uint64_t>{2, 3});
    REQUIRE(ring.IsEmpty());
}

TEST_CASE("RecordRingBuffer reset discards retained generations and permits reuse", "[Sora.Core.RecordRingBuffer]") {
    const auto verify = []<class Ring>(Ring& ring, size_t occupiedBytes) {
        for (uint64_t value : {1, 2, 3}) {
            REQUIRE(ring.TryWrite(AsBytes(value)));
        }
        REQUIRE_FALSE(ring.IsEmpty());
        REQUIRE(ring.OccupiedBytesApprox() == occupiedBytes);

        ring.Reset();
        REQUIRE(ring.IsEmpty());
        REQUIRE_FALSE(ring.TryConsume([](std::span<const std::byte>) {}));

        const uint64_t replacement = 4;
        REQUIRE(ring.TryWrite(AsBytes(replacement)));
        REQUIRE(ring.TryConsume(
            [&](std::span<const std::byte> record) { CHECK(ReadObject<uint64_t>(record) == replacement); }));
        REQUIRE(ring.IsEmpty());
    };

    RecordRingBuffer<256> spsc;
    verify(spsc, 96);
    RecordRingBuffer<256, Topology::Mpmc> mpmc;
    verify(mpmc, 96);
}

TEST_CASE("Local RecordRingBuffer preserves packed-record order without atomic endpoints",
          "[Sora.Core.RecordRingBuffer]") {
    RecordRingBuffer<256, Topology::Local> ring;
    uint64_t nextToConsume = 1;
    for (uint64_t value = 1; value <= 16; ++value) {
        while (!ring.TryWrite(AsBytes(value))) {
            REQUIRE(ring.TryConsume(
                [&](std::span<const std::byte> record) { CHECK(ReadObject<uint64_t>(record) == nextToConsume++); }));
        }
    }
    ring.Drain([&](std::span<const std::byte> record) { CHECK(ReadObject<uint64_t>(record) == nextToConsume++); });
    REQUIRE(nextToConsume == 17);
    REQUIRE(ring.IsEmpty());
}

TEST_CASE("MPSC RecordRingBuffer preserves every producer's FIFO order", "[Sora.Core.RecordRingBuffer]") {
    constexpr uint32_t kProducers = 4;
    constexpr uint64_t kPerProducer = 10'000;
    RecordRingBuffer<8192, Topology::Mpsc> ring;
    std::atomic<uint32_t> producersDone{0};
    std::atomic<bool> failed{false};
    std::atomic<bool> stop{false};
    std::array<std::thread, kProducers> producers;

    for (uint32_t producer = 0; producer < kProducers; ++producer) {
        producers[producer] = std::thread([&, producer] {
            WriteIdentities(ring, producer, kPerProducer, stop);
            producersDone.fetch_add(1, std::memory_order_release);
        });
    }

    std::array<uint64_t, kProducers> expected{};
    uint64_t consumed = 0;
    while (consumed != kProducers * kPerProducer) {
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
        if (producersDone.load(std::memory_order_acquire) == kProducers && ring.IsEmpty()) {
            failed.store(true, std::memory_order_relaxed);
            break;
        }
        std::this_thread::yield();
    }
    for (std::thread& producer : producers) {
        producer.join();
    }
    REQUIRE(consumed == kProducers * kPerProducer);
    REQUIRE(!failed.load(std::memory_order_relaxed));
    REQUIRE(ring.IsEmpty());
}

TEST_CASE("SPMC RecordRingBuffer retains a payload until its callback completes", "[Sora.Core.RecordRingBuffer]") {
    RecordRingBuffer<256, Topology::Spmc> ring;
    for (uint64_t value = 1; value <= 8; ++value) {
        REQUIRE(ring.TryWrite(AsBytes(value)));
    }

    std::atomic<bool> firstClaimed{false};
    std::atomic<bool> releaseFirst{false};
    std::atomic<bool> failed{false};
    std::thread delayed([&] {
        if (!ring.TryConsume([&](std::span<const std::byte> record) {
                failed.store(ReadObject<uint64_t>(record) != 1, std::memory_order_relaxed);
                firstClaimed.store(true, std::memory_order_release);
                firstClaimed.notify_one();
                releaseFirst.wait(false, std::memory_order_acquire);
            })) {
            failed.store(true, std::memory_order_relaxed);
        }
    });
    firstClaimed.wait(false, std::memory_order_acquire);

    uint64_t consumed = 0;
    std::thread follower([&] {
        while (consumed != 7) {
            if (ring.TryConsume([&](std::span<const std::byte> record) {
                    const uint64_t value = ReadObject<uint64_t>(record);
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

    const uint64_t deferred = 9;
    REQUIRE(ring.OccupiedBytesApprox() == 256);
    REQUIRE(!ring.TryWrite(AsBytes(deferred)));
    releaseFirst.store(true, std::memory_order_release);
    releaseFirst.notify_one();
    delayed.join();
    REQUIRE(!failed.load(std::memory_order_relaxed));
    REQUIRE(ring.TryWrite(AsBytes(deferred)));
    REQUIRE(
        ring.TryConsume([&](std::span<const std::byte> record) { CHECK(ReadObject<uint64_t>(record) == deferred); }));
    REQUIRE(ring.IsEmpty());
}

TEST_CASE("MPMC RecordRingBuffer transfers each fixed record exactly once", "[Sora.Core.RecordRingBuffer]") {
    constexpr uint64_t kPerProducer = 4'000;
    constexpr uint64_t kTotal = 4 * kPerProducer;
    for (size_t repetition = 0; repetition < 4; ++repetition) {
        const TransferResult result = TransferFixedRecords<Topology::Mpmc, 4, 4, 64>(kPerProducer);
        CAPTURE(repetition, result.consumed, result.checksum, result.occupiedBytes);
        REQUIRE(result.consumed == kTotal);
        REQUIRE(result.checksum == kTotal * (kTotal + 1) / 2);
        REQUIRE(!result.failed);
        REQUIRE(result.empty);
    }
}

TEST_CASE("SPMC RecordRingBuffer cannot lose a completed-prefix reclaim handoff", "[Sora.Core.RecordRingBuffer]") {
    constexpr uint64_t kTotal = 30'000;
    for (size_t repetition = 0; repetition < 4; ++repetition) {
        const TransferResult result = TransferFixedRecords<Topology::Spmc, 1, 4, 1024>(kTotal);
        CAPTURE(repetition, result.consumed, result.checksum, result.occupiedBytes);
        REQUIRE(result.consumed == kTotal);
        REQUIRE(result.checksum == kTotal * (kTotal + 1) / 2);
        REQUIRE(!result.failed);
        REQUIRE(result.empty);
    }
}

TEST_CASE("RecordRingBuffer concurrent layouts isolate all contention domains", "[Sora.Core.RecordRingBuffer]") {
    STATIC_REQUIRE(Sora::Concurrency::Meta::AuditFalseSharing<RecordRingBuffer<4096, Topology::Spsc>>());
    STATIC_REQUIRE(Sora::Concurrency::Meta::AuditFalseSharing<RecordRingBuffer<4096, Topology::Mpsc>>());
    STATIC_REQUIRE(Sora::Concurrency::Meta::AuditFalseSharing<RecordRingBuffer<4096, Topology::Spmc>>());
    STATIC_REQUIRE(Sora::Concurrency::Meta::AuditFalseSharing<RecordRingBuffer<4096, Topology::Mpmc>>());
}

#include <Sora/Core/Concurrency/SequenceLock.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

    struct Snapshot {
        std::uint64_t sequence;
        std::uint64_t complement;
        std::uint64_t doubled;

        [[nodiscard]] bool IsConsistent() const noexcept {
            return complement == ~sequence && doubled == sequence * 2;
        }
    };

    static_assert(std::is_trivially_copyable_v<Snapshot>);

    struct NonDefaultSnapshot {
        std::uint64_t value;

        NonDefaultSnapshot() = delete;
        explicit NonDefaultSnapshot(std::uint64_t initial) noexcept : value(initial) {}
    };

    static_assert(std::is_trivially_copyable_v<NonDefaultSnapshot>);

    template<size_t LaneCount>
    struct PatternSnapshot {
        std::uint64_t generation;
        std::array<std::uint64_t, LaneCount> lanes;
        std::uint64_t checksum;

        [[nodiscard]] bool IsConsistent() const noexcept;
    };

    [[nodiscard]] constexpr std::uint64_t PatternWord(std::uint64_t generation, size_t index) noexcept {
        std::uint64_t value = generation + 0x9e3779b97f4a7c15ULL * (index + 1);
        value ^= value >> 30;
        value *= 0xbf58476d1ce4e5b9ULL;
        value ^= value >> 27;
        value *= 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }

    template<size_t LaneCount>
    [[nodiscard]] PatternSnapshot<LaneCount> MakePatternSnapshot(std::uint64_t generation) noexcept {
        PatternSnapshot<LaneCount> snapshot{.generation = generation, .lanes = {}, .checksum = generation};
        for (size_t index = 0; index < LaneCount; ++index) {
            snapshot.lanes[index] = PatternWord(generation, index);
            snapshot.checksum ^= snapshot.lanes[index];
        }
        return snapshot;
    }

    template<size_t LaneCount>
    bool PatternSnapshot<LaneCount>::IsConsistent() const noexcept {
        std::uint64_t expectedChecksum = generation;
        for (size_t index = 0; index < LaneCount; ++index) {
            if (lanes[index] != PatternWord(generation, index)) {
                return false;
            }
            expectedChecksum ^= lanes[index];
        }
        return checksum == expectedChecksum;
    }

    template<size_t Size>
    void CheckTailSizedPayload() {
        using Payload = std::array<std::byte, Size>;

        Payload expected{};
        for (size_t index = 0; index < Size; ++index) {
            expected[index] = static_cast<std::byte>((index * 37 + Size) & 0xff);
        }

        Sora::Concurrency::SequenceLock<Payload> lock;
        lock.Write(expected);
        REQUIRE(lock.Read() == expected);
        REQUIRE(lock.TryRead() == expected);
    }

    struct ReaderStats {
        std::uint64_t successfulReads = 0;
        std::uint64_t failedReads = 0;
        bool consistent = true;
        bool monotonic = true;
    };

} // namespace

TEST_CASE("SequenceLock publishes and updates consistent snapshots", "[Sora.Core.Concurrency.SequenceLock]") {
    Sora::Concurrency::SequenceLock<std::uint64_t> zero;
    STATIC_REQUIRE(sizeof(decltype(zero)::SequenceType) == sizeof(std::uint64_t));
    REQUIRE(zero.Read() == 0);

    Sora::Concurrency::SequenceLock<Snapshot> lock{Snapshot{4, ~std::uint64_t{4}, 8}};

    const Snapshot initial = lock.Read();
    REQUIRE(initial.IsConsistent());
    REQUIRE(initial.sequence == 4);

    lock.Update([](Snapshot& snapshot) {
        snapshot.sequence = 7;
        snapshot.complement = ~snapshot.sequence;
        snapshot.doubled = snapshot.sequence * 2;
    });

    const std::optional<Snapshot> updated = lock.TryRead();
    REQUIRE(updated.has_value());
    REQUIRE(updated->IsConsistent());
    REQUIRE(updated->sequence == 7);
}

TEST_CASE("SequenceLock supports explicitly initialized non-default payloads", "[Sora.Core.Concurrency.SequenceLock]") {
    Sora::Concurrency::SequenceLock<NonDefaultSnapshot> lock{NonDefaultSnapshot{42}};

    REQUIRE(lock.Read().value == 42);
}

TEST_CASE("SequenceLock never exposes mixed multi-word snapshots", "[Sora.Core.Concurrency.SequenceLock]") {
    constexpr std::uint64_t kWriteCount = 100'000;
    constexpr size_t kReaderCount = 4;

    Sora::Concurrency::SequenceLock<Snapshot> lock{Snapshot{0, ~std::uint64_t{0}, 0}};
    std::atomic<bool> start = false;
    std::atomic<bool> finished = false;
    std::atomic<bool> consistent = true;

    std::vector<std::jthread> readers;
    readers.reserve(kReaderCount);
    for (size_t index = 0; index < kReaderCount; ++index) {
        readers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            while (!finished.load(std::memory_order_acquire)) {
                if (!lock.Read().IsConsistent()) {
                    consistent.store(false, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (std::uint64_t sequence = 1; sequence <= kWriteCount; ++sequence) {
        lock.Write(Snapshot{sequence, ~sequence, sequence * 2});
    }
    finished.store(true, std::memory_order_release);

    readers.clear();
    REQUIRE(consistent.load(std::memory_order_relaxed));
    REQUIRE(lock.Read().sequence == kWriteCount);
}

TEST_CASE("SequenceLock preserves partial final storage words", "[Sora.Core.Concurrency.SequenceLock]") {
    CheckTailSizedPayload<1>();
    CheckTailSizedPayload<3>();
    CheckTailSizedPayload<7>();
    CheckTailSizedPayload<9>();
    CheckTailSizedPayload<15>();
    CheckTailSizedPayload<17>();
    CheckTailSizedPayload<63>();
    CheckTailSizedPayload<65>();
}

TEST_CASE("SequenceLock readers observe consistent monotonic large snapshots", "[Sora.Core.Concurrency.SequenceLock]") {
    using LargeSnapshot = PatternSnapshot<63>;

    constexpr std::uint64_t kWriteCount = 150'000;
    constexpr size_t kReaderCount = 12;

    STATIC_REQUIRE(sizeof(LargeSnapshot) > 8 * sizeof(std::uint64_t));
    Sora::Concurrency::SequenceLock<LargeSnapshot> lock{MakePatternSnapshot<63>(0)};
    std::barrier start{static_cast<std::ptrdiff_t>(kReaderCount + 1)};
    std::atomic<bool> finished = false;
    std::array<ReaderStats, kReaderCount> stats{};
    std::vector<std::jthread> readers;
    readers.reserve(kReaderCount);

    for (size_t readerIndex = 0; readerIndex < kReaderCount; ++readerIndex) {
        readers.emplace_back([&, readerIndex] {
            start.arrive_and_wait();
            std::uint64_t previousGeneration = 0;
            while (!finished.load(std::memory_order_acquire)) {
                const LargeSnapshot snapshot = lock.Read();
                ++stats[readerIndex].successfulReads;
                stats[readerIndex].consistent &= snapshot.IsConsistent();
                stats[readerIndex].monotonic &= snapshot.generation >= previousGeneration;
                previousGeneration = snapshot.generation;
            }
        });
    }

    start.arrive_and_wait();
    for (std::uint64_t generation = 1; generation <= kWriteCount; ++generation) {
        lock.Write(MakePatternSnapshot<63>(generation));
    }
    finished.store(true, std::memory_order_release);
    readers.clear();

    std::uint64_t totalReads = 0;
    for (const ReaderStats& reader : stats) {
        totalReads += reader.successfulReads;
        REQUIRE(reader.consistent);
        REQUIRE(reader.monotonic);
    }
    REQUIRE(totalReads > kReaderCount);
    REQUIRE(lock.Read().generation == kWriteCount);
    REQUIRE(lock.Sequence() == kWriteCount * 2);
    REQUIRE_FALSE(lock.WriteInProgress());
}

TEST_CASE("SequenceLock TryRead remains safe under sustained high contention", "[Sora.Core.Concurrency.SequenceLock]") {
    using HugeSnapshot = PatternSnapshot<255>;

    constexpr std::uint64_t kWriteCount = 40'000;
    constexpr size_t kReaderCount = 12;

    Sora::Concurrency::SequenceLock<HugeSnapshot> lock{MakePatternSnapshot<255>(0)};
    std::barrier start{static_cast<std::ptrdiff_t>(kReaderCount + 1)};
    std::atomic<bool> finished = false;
    std::array<ReaderStats, kReaderCount> stats{};
    std::vector<std::jthread> readers;
    readers.reserve(kReaderCount);

    for (size_t readerIndex = 0; readerIndex < kReaderCount; ++readerIndex) {
        readers.emplace_back([&, readerIndex] {
            start.arrive_and_wait();
            std::uint64_t previousGeneration = 0;
            while (!finished.load(std::memory_order_acquire)) {
                const std::optional<HugeSnapshot> snapshot = lock.TryRead();
                if (!snapshot.has_value()) {
                    ++stats[readerIndex].failedReads;
                    continue;
                }
                ++stats[readerIndex].successfulReads;
                stats[readerIndex].consistent &= snapshot->IsConsistent();
                stats[readerIndex].monotonic &= snapshot->generation >= previousGeneration;
                previousGeneration = snapshot->generation;
            }
        });
    }

    start.arrive_and_wait();
    for (std::uint64_t generation = 1; generation <= kWriteCount; ++generation) {
        lock.Write(MakePatternSnapshot<255>(generation));
    }
    finished.store(true, std::memory_order_release);
    readers.clear();

    std::uint64_t successfulReads = 0;
    std::uint64_t failedReads = 0;
    for (const ReaderStats& reader : stats) {
        successfulReads += reader.successfulReads;
        failedReads += reader.failedReads;
        REQUIRE(reader.consistent);
        REQUIRE(reader.monotonic);
    }
    REQUIRE(successfulReads > 0);
    REQUIRE(failedReads > 0);
    REQUIRE(lock.Read().generation == kWriteCount);
}

TEST_CASE("SequenceLock Update publishes atomically and preserves strong exception safety",
          "[Sora.Core.Concurrency.SequenceLock]") {
    constexpr std::uint64_t kUpdateCount = 50'000;
    constexpr size_t kReaderCount = 8;

    Sora::Concurrency::SequenceLock<Snapshot> lock{Snapshot{0, ~std::uint64_t{0}, 0}};
    std::barrier start{static_cast<std::ptrdiff_t>(kReaderCount + 1)};
    std::atomic<bool> finished = false;
    std::atomic<bool> consistent = true;
    std::vector<std::jthread> readers;
    readers.reserve(kReaderCount);

    for (size_t readerIndex = 0; readerIndex < kReaderCount; ++readerIndex) {
        readers.emplace_back([&] {
            start.arrive_and_wait();
            while (!finished.load(std::memory_order_acquire)) {
                if (!lock.Read().IsConsistent()) {
                    consistent.store(false, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }

    start.arrive_and_wait();
    for (std::uint64_t update = 0; update < kUpdateCount; ++update) {
        lock.Update([](Snapshot& snapshot) noexcept {
            ++snapshot.sequence;
            snapshot.complement = ~snapshot.sequence;
            snapshot.doubled = snapshot.sequence * 2;
        });
    }
    finished.store(true, std::memory_order_release);
    readers.clear();

    REQUIRE(consistent.load(std::memory_order_relaxed));
    const Snapshot beforeFailure = lock.Read();
    const auto sequenceBeforeFailure = lock.Sequence();
    REQUIRE_THROWS(lock.Update([](Snapshot& snapshot) {
        snapshot.sequence = 0;
        throw std::exception{};
    }));
    REQUIRE(lock.Read().sequence == beforeFailure.sequence);
    REQUIRE(lock.Sequence() == sequenceBeforeFailure);
    REQUIRE(beforeFailure.sequence == kUpdateCount);
}

/**
 * @file SpscBadDrain.cpp
 * @brief Demonstrate why an SPSC consumer must acquire the producer's published write cursor.
 * @ingroup Demo
 */

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <print>
#include <string_view>
#include <thread>
#include <type_traits>

namespace {

    struct alignas(64) Payload {
        std::uint64_t sequence = 0;
        std::array<std::uint64_t, 6> words{};
        std::uint64_t checksum = 0;
    };

    static_assert(sizeof(Payload) == 64);

    [[nodiscard]] constexpr std::uint64_t Pattern(std::uint64_t sequence, std::size_t index) noexcept {
        std::uint64_t value = sequence + 0x9e3779b97f4a7c15ULL * (index + 1);
        value ^= value >> 30;
        value *= 0xbf58476d1ce4e5b9ULL;
        value ^= value >> 27;
        value *= 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }

    [[nodiscard]] constexpr Payload MakePayload(std::uint64_t sequence) noexcept {
        Payload payload{.sequence = sequence, .words = {}, .checksum = sequence};
        for (std::size_t index = 0; index < payload.words.size(); ++index) {
            payload.words[index] = Pattern(sequence, index);
            payload.checksum ^= payload.words[index];
        }
        return payload;
    }

    [[nodiscard]] constexpr bool IsValid(const Payload& payload, std::uint64_t expectedSequence) noexcept {
        if (payload.sequence != expectedSequence) {
            return false;
        }
        std::uint64_t checksum = payload.sequence;
        for (std::size_t index = 0; index < payload.words.size(); ++index) {
            if (payload.words[index] != Pattern(payload.sequence, index)) {
                return false;
            }
            checksum ^= payload.words[index];
        }
        return checksum == payload.checksum;
    }

    template<uint32_t Capacity>
        requires(std::has_single_bit(Capacity) && Capacity >= 2)
    class BadSpscRingBuffer {
    public:
        [[nodiscard]] bool TryPush(const Payload& payload) noexcept {
            const uint32_t writePos = writePos_.load(std::memory_order_relaxed);
            if (writePos - cachedReadPos_ == Capacity) {
                cachedReadPos_ = readPos_.load(std::memory_order_acquire);
                if (writePos - cachedReadPos_ == Capacity) {
                    return false;
                }
            }

            slots_[writePos & kMask] = payload;
            writePos_.store(writePos + 1, std::memory_order_release);
            return true;
        }

        template<typename Sink>
            requires std::is_nothrow_invocable_v<Sink&, const Payload&>
        [[nodiscard]] uint32_t Drain(Sink&& sink) noexcept {
            const uint32_t begin = readPos_.load(std::memory_order_relaxed);

            // Deliberate bug: this relaxed load does not synchronize with the producer's release publication.
            const uint32_t end = writePos_.load(std::memory_order_relaxed);

            for (uint32_t pos = begin; pos != end; ++pos) {
                sink(slots_[pos & kMask]);
            }
            if (begin != end) {
                readPos_.store(end, std::memory_order_release);
            }
            return end - begin;
        }

    private:
        inline static constexpr uint32_t kMask = Capacity - 1;

        alignas(64) std::atomic<uint32_t> writePos_{0};
        uint32_t cachedReadPos_ = 0;
        alignas(64) std::atomic<uint32_t> readPos_{0};
        alignas(64) std::array<Payload, Capacity> slots_{};
    };

    [[nodiscard]] constexpr std::string_view ArchitectureName() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
        return "x86-64";
#elif defined(_M_ARM64) || defined(__aarch64__)
        return "ARM64";
#else
        return "unknown";
#endif
    }

} // namespace

int main() {
    constexpr std::uint64_t kTransferCount = 10'000'000;
    BadSpscRingBuffer<1024> ring;
    std::uint64_t consumed = 0;
    std::uint64_t corruptionCount = 0;
    Payload firstCorrupted{};
    std::uint64_t firstExpected = 0;

    std::println("SpscBadDrain: {} payloads on {}", kTransferCount, ArchitectureName());
    std::println("Deliberate defect: writePos_.load(std::memory_order_relaxed) in Drain().");

    std::jthread producer([&] {
        for (std::uint64_t sequence = 1; sequence <= kTransferCount; ++sequence) {
            const Payload payload = MakePayload(sequence);
            while (!ring.TryPush(payload)) {
                std::this_thread::yield();
            }
        }
    });

    std::jthread consumer([&] {
        while (consumed < kTransferCount) {
            const uint32_t count = ring.Drain([&](const Payload& payload) noexcept {
                const std::uint64_t expected = consumed + 1;
                if (!IsValid(payload, expected)) {
                    if (corruptionCount == 0) {
                        firstExpected = expected;
                        firstCorrupted = payload;
                    }
                    ++corruptionCount;
                }
                ++consumed;
            });
            if (count == 0) {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    if (corruptionCount != 0) {
        std::println(stderr, "ERROR: observed {} corrupted or stale payloads.", corruptionCount);
        std::println(stderr, "First failure: expected sequence {}, observed sequence {}, checksum {:#018x}.",
                     firstExpected, firstCorrupted.sequence, firstCorrupted.checksum);
    } else {
        std::println("No visible corruption was observed in this run.");
        std::println("This does not validate the relaxed load: the payload access still lacks a C++ "
                     "happens-before edge.");
        std::println("Use ThreadSanitizer or run an optimized build repeatedly on a weakly ordered architecture "
                     "such as ARM64.");
    }
}

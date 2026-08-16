/**
 * @file RecordRingMigrationAssembly.cpp
 * @brief Stable exported probes for comparing Mashiro and Sora packed-record hot-path code generation.
 * @ingroup Core
 */

#include <Mashiro/Core/RecordRingBuffer.h>
#include <Sora/Core/Concurrency/RecordRingBuffer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace {

    template<Mashiro::Concurrency::QueueTopology MashiroTopology, Sora::Concurrency::QueueTopology SoraTopology>
    struct Rings {
        using MashiroType = Mashiro::RecordRingBuffer<4096, MashiroTopology>;
        using SoraType = Sora::Concurrency::RecordRingBuffer<4096, SoraTopology>;

        static_assert(sizeof(MashiroType) == sizeof(SoraType));
        static_assert(alignof(MashiroType) == alignof(SoraType));
    };

    using Local = Rings<Mashiro::Concurrency::QueueTopologies::Local, Sora::Concurrency::QueueTopologies::Local>;
    using Spsc = Rings<Mashiro::Concurrency::QueueTopologies::Spsc, Sora::Concurrency::QueueTopologies::Spsc>;
    using Mpsc = Rings<Mashiro::Concurrency::QueueTopologies::Mpsc, Sora::Concurrency::QueueTopologies::Mpsc>;
    using Spmc = Rings<Mashiro::Concurrency::QueueTopologies::Spmc, Sora::Concurrency::QueueTopologies::Spmc>;
    using Mpmc = Rings<Mashiro::Concurrency::QueueTopologies::Mpmc, Sora::Concurrency::QueueTopologies::Mpmc>;

    template<std::size_t ByteCapacity>
    class LegacyLoggerThreadRing {
    public:
        [[nodiscard]] bool TryWrite(std::span<const std::byte> payload) noexcept {
            const std::size_t totalSize = sizeof(std::uint32_t) + payload.size_bytes();
            if (totalSize > ByteCapacity) {
                return false;
            }

            const std::size_t write = writeIndex_.load(std::memory_order_relaxed);
            const std::size_t read = readIndex_.load(std::memory_order_acquire);
            if (ByteCapacity - (write - read) < totalSize) {
                return false;
            }

            const auto payloadSize = static_cast<std::uint32_t>(payload.size_bytes());
            WriteBytes(write, std::as_bytes(std::span{&payloadSize, 1}));
            WriteBytes(write + sizeof(payloadSize), payload);
            writeIndex_.store(write + totalSize, std::memory_order_release);
            return true;
        }

        template<class Consumer>
        std::size_t Drain(Consumer&& consumer) {
            std::size_t count = 0;
            std::size_t read = readIndex_.load(std::memory_order_relaxed);
            const std::size_t write = writeIndex_.load(std::memory_order_acquire);
            while (read + sizeof(std::uint32_t) <= write) {
                std::uint32_t payloadSize = 0;
                ReadBytes(read, std::as_writable_bytes(std::span{&payloadSize, 1}));
                if (read + sizeof(payloadSize) + payloadSize > write) {
                    break;
                }
                scratch_.resize(payloadSize);
                ReadBytes(read + sizeof(payloadSize), scratch_);
                consumer(std::span<const std::byte>{scratch_});
                read += sizeof(payloadSize) + payloadSize;
                ++count;
            }
            readIndex_.store(read, std::memory_order_release);
            return count;
        }

    private:
        void WriteBytes(std::size_t index, std::span<const std::byte> bytes) noexcept {
            const std::size_t offset = index % ByteCapacity;
            const std::size_t first = std::min(bytes.size_bytes(), ByteCapacity - offset);
            std::memcpy(storage_.data() + offset, bytes.data(), first);
            if (first < bytes.size_bytes()) {
                std::memcpy(storage_.data(), bytes.data() + first, bytes.size_bytes() - first);
            }
        }

        void ReadBytes(std::size_t index, std::span<std::byte> bytes) noexcept {
            const std::size_t offset = index % ByteCapacity;
            const std::size_t first = std::min(bytes.size_bytes(), ByteCapacity - offset);
            std::memcpy(bytes.data(), storage_.data() + offset, first);
            if (first < bytes.size_bytes()) {
                std::memcpy(bytes.data() + first, storage_.data(), bytes.size_bytes() - first);
            }
        }

        alignas(64) std::atomic<std::size_t> readIndex_{0};
        alignas(64) std::atomic<std::size_t> writeIndex_{0};
        alignas(64) std::array<std::byte, ByteCapacity> storage_{};
        std::vector<std::byte> scratch_{};
    };

    using LegacyLoggerRing = LegacyLoggerThreadRing<64 * 1024>;
    using SoraLoggerRing = Sora::Concurrency::RecordRingBuffer<64 * 1024, Sora::Concurrency::QueueTopologies::Spsc>;

    template<class Ring>
    [[nodiscard]] bool Consume(Ring& ring, uint64_t& value) noexcept {
        return ring.TryConsume([&value](std::span<const std::byte> record) noexcept {
            if (record.size_bytes() >= sizeof(value)) {
                std::memcpy(&value, record.data(), sizeof(value));
            }
        });
    }

} // namespace

extern "C" {

[[gnu::noinline]] bool MashiroLocalRecordWrite(Local::MashiroType* ring, const std::byte* data, size_t size) noexcept {
    return ring->TryWrite({data, size});
}
[[gnu::noinline]] bool SoraLocalRecordWrite(Local::SoraType* ring, const std::byte* data, size_t size) noexcept {
    return ring->TryWrite({data, size});
}
[[gnu::noinline]] bool MashiroLocalRecordConsume(Local::MashiroType* ring, uint64_t* value) noexcept {
    return Consume(*ring, *value);
}
[[gnu::noinline]] bool SoraLocalRecordConsume(Local::SoraType* ring, uint64_t* value) noexcept {
    return Consume(*ring, *value);
}

[[gnu::noinline]] bool MashiroSpscRecordWrite(Spsc::MashiroType* ring, const std::byte* data, size_t size) noexcept {
    return ring->TryWrite({data, size});
}
[[gnu::noinline]] bool SoraSpscRecordWrite(Spsc::SoraType* ring, const std::byte* data, size_t size) noexcept {
    return ring->TryWrite({data, size});
}
[[gnu::noinline]] bool MashiroSpscRecordConsume(Spsc::MashiroType* ring, uint64_t* value) noexcept {
    return Consume(*ring, *value);
}
[[gnu::noinline]] bool SoraSpscRecordConsume(Spsc::SoraType* ring, uint64_t* value) noexcept {
    return Consume(*ring, *value);
}

[[gnu::noinline]] bool MashiroMpscRecordWrite(Mpsc::MashiroType* ring, const std::byte* data, size_t size) noexcept {
    return ring->TryWrite({data, size});
}
[[gnu::noinline]] bool SoraMpscRecordWrite(Mpsc::SoraType* ring, const std::byte* data, size_t size) noexcept {
    return ring->TryWrite({data, size});
}
[[gnu::noinline]] bool MashiroMpscRecordConsume(Mpsc::MashiroType* ring, uint64_t* value) noexcept {
    return Consume(*ring, *value);
}
[[gnu::noinline]] bool SoraMpscRecordConsume(Mpsc::SoraType* ring, uint64_t* value) noexcept {
    return Consume(*ring, *value);
}

[[gnu::noinline]] bool MashiroSpmcRecordWrite(Spmc::MashiroType* ring, const std::byte* data, size_t size) noexcept {
    return ring->TryWrite({data, size});
}
[[gnu::noinline]] bool SoraSpmcRecordWrite(Spmc::SoraType* ring, const std::byte* data, size_t size) noexcept {
    return ring->TryWrite({data, size});
}
[[gnu::noinline]] bool MashiroSpmcRecordConsume(Spmc::MashiroType* ring, uint64_t* value) noexcept {
    return Consume(*ring, *value);
}
[[gnu::noinline]] bool SoraSpmcRecordConsume(Spmc::SoraType* ring, uint64_t* value) noexcept {
    return Consume(*ring, *value);
}

[[gnu::noinline]] bool MashiroMpmcRecordWrite(Mpmc::MashiroType* ring, const std::byte* data, size_t size) noexcept {
    return ring->TryWrite({data, size});
}
[[gnu::noinline]] bool SoraMpmcRecordWrite(Mpmc::SoraType* ring, const std::byte* data, size_t size) noexcept {
    return ring->TryWrite({data, size});
}
[[gnu::noinline]] bool MashiroMpmcRecordConsume(Mpmc::MashiroType* ring, uint64_t* value) noexcept {
    return Consume(*ring, *value);
}
[[gnu::noinline]] bool SoraMpmcRecordConsume(Mpmc::SoraType* ring, uint64_t* value) noexcept {
    return Consume(*ring, *value);
}

[[gnu::noinline]] bool LegacyLoggerWrite64(LegacyLoggerRing* ring, const std::byte* data) noexcept {
    return ring->TryWrite({data, 64});
}
[[gnu::noinline]] bool SoraLoggerWrite64(SoraLoggerRing* ring, const std::byte* data) noexcept {
    return ring->TryWrite({data, 64});
}
[[gnu::noinline]] std::size_t LegacyLoggerDrain(LegacyLoggerRing* ring, std::uint64_t* checksum) noexcept {
    return ring->Drain([checksum](std::span<const std::byte> record) noexcept {
        std::uint64_t value = 0;
        std::memcpy(&value, record.data(), sizeof(value));
        *checksum += value;
    });
}
[[gnu::noinline]] std::size_t SoraLoggerDrain(SoraLoggerRing* ring, std::uint64_t* checksum) noexcept {
    return ring->Drain([checksum](std::span<const std::byte> record) noexcept {
        std::uint64_t value = 0;
        std::memcpy(&value, record.data(), sizeof(value));
        *checksum += value;
    });
}

} // extern "C"

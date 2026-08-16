/**
 * @file RecordRingBufferBenchmark.cpp
 * @brief Paired throughput benchmark for packed variable-length record rings and established baselines.
 * @ingroup Core
 */

#include <Mashiro/Core/RecordRingBuffer.h>
#include <Sora/Core/Concurrency/RecordRingBuffer.h>

#include <rigtorp/MPMCQueue.h>
#include <rigtorp/SPSCQueue.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__)
#    include <immintrin.h>
#endif

#if defined(_WIN32)
#    include <windows.h>
#endif

namespace Mashiro::Benchmark {

    using Clock = std::chrono::steady_clock;

    inline constexpr std::string_view kAeronCommit = "d20f50684cece32f4c692a355fada7f0cb9dfa82";
    inline constexpr std::string_view kDpdkCommit = "9231dc7c072223628a81f5c755796d95230a5572";
    inline constexpr std::string_view kRigtorpMpmcCommit = "b9808ede08f26fa9df4df4e081d19cace8f6c6ea";
    inline constexpr std::string_view kRigtorpSpscCommit = "1053918dbd251fbff69b24ef27fa5d51c29ec2af";
    inline constexpr std::size_t kAeronTrailerBytes = 12 * Platform::kCacheLineSize;

    struct Config {
        std::filesystem::path output = "record-ring-results.csv";
        std::string label = "formal";
        std::size_t samples = 9;
        std::size_t warmups = 1;
        std::size_t recordsPerProducer = 100'000;
        double minimumSampleSeconds = 0.12;
        std::uint64_t seed = 0x5245434F52445242ULL;
        std::string pairFilter;
        std::string topologyFilter;
        bool quick = false;
        bool migration = false;
        bool loggerMigration = false;
        bool pinWorkers = true;
        bool showHelp = false;
    };

    struct alignas(Platform::kCacheLineSize) WorkerResult {
        std::uint64_t transferred{};
        std::uint64_t checksum{};
        std::uint64_t retries{};
    };

    struct Sample {
        double seconds{};
        std::uint64_t records{};
        std::uint64_t pushRetries{};
        std::uint64_t popRetries{};
        std::size_t storageBytes{};
    };

    inline void Relax() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
        _mm_pause();
#else
        std::this_thread::yield();
#endif
    }

    [[nodiscard]] std::vector<std::uint32_t> AvailableCpuSets() {
#if defined(_WIN32)
        ULONG byteCount = 0;
        if (GetSystemCpuSetInformation(nullptr, 0, &byteCount, GetCurrentProcess(), 0) != FALSE ||
            GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            return {};
        }
        std::vector<std::byte> buffer(byteCount);
        if (GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()), byteCount,
                                       &byteCount, GetCurrentProcess(), 0) == FALSE) {
            return {};
        }

        struct CpuSet {
            std::uint32_t id{};
            std::uint16_t group{};
            std::uint8_t logicalProcessor{};
            std::uint8_t core{};
            std::uint8_t efficiency{};
            std::uint8_t sibling{};
        };
        std::vector<CpuSet> cpuSets;
        for (std::size_t offset = 0; offset < byteCount;) {
            const auto* information = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
            if (information->Size == 0 || offset + information->Size > byteCount) {
                return {};
            }
            if (information->Type == CpuSetInformation && information->CpuSet.Parked == 0 &&
                (information->CpuSet.Allocated == 0 || information->CpuSet.AllocatedToTargetProcess != 0)) {
                cpuSets.push_back({
                    .id = information->CpuSet.Id,
                    .group = information->CpuSet.Group,
                    .logicalProcessor = information->CpuSet.LogicalProcessorIndex,
                    .core = information->CpuSet.CoreIndex,
                    .efficiency = information->CpuSet.EfficiencyClass,
                });
            }
            offset += information->Size;
        }
        std::ranges::sort(cpuSets, [](const CpuSet& left, const CpuSet& right) {
            return std::tie(left.group, left.core, left.logicalProcessor) <
                   std::tie(right.group, right.core, right.logicalProcessor);
        });
        for (std::size_t index = 1; index < cpuSets.size(); ++index) {
            if (cpuSets[index].group == cpuSets[index - 1].group && cpuSets[index].core == cpuSets[index - 1].core) {
                cpuSets[index].sibling = static_cast<std::uint8_t>(cpuSets[index - 1].sibling + 1);
            }
        }
        std::ranges::sort(cpuSets, [](const CpuSet& left, const CpuSet& right) {
            if (left.efficiency != right.efficiency) {
                return left.efficiency > right.efficiency;
            }
            return std::tie(left.sibling, left.group, left.core, left.logicalProcessor) <
                   std::tie(right.sibling, right.group, right.core, right.logicalProcessor);
        });
        return cpuSets | std::views::transform(&CpuSet::id) | std::ranges::to<std::vector>();
#else
        return {};
#endif
    }

    [[nodiscard]] std::vector<std::uint32_t> SelectWorkerCpuSets(std::size_t count, bool enabled) {
        if (!enabled) {
            return {};
        }
#if defined(_WIN32)
        static const std::vector<std::uint32_t> available = AvailableCpuSets();
        if (available.size() < count) {
            throw std::runtime_error("not enough available CPU sets to pin every record benchmark worker");
        }
        return {available.begin(), available.begin() + static_cast<std::ptrdiff_t>(count)};
#else
        static_cast<void>(count);
        return {};
#endif
    }

    [[nodiscard]] bool BindWorker(std::span<const std::uint32_t> cpuSets, std::size_t index) noexcept {
        if (cpuSets.empty()) {
            return true;
        }
#if defined(_WIN32)
        const ULONG cpuSet = cpuSets[index];
        return SetThreadSelectedCpuSets(GetCurrentThread(), &cpuSet, 1) != FALSE &&
               SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != FALSE;
#else
        static_cast<void>(index);
        return true;
#endif
    }

    [[nodiscard]] bool ConfigureBenchmarkProcess(bool enabled) noexcept {
        if (!enabled) {
            return true;
        }
#if defined(_WIN32)
        return SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS) != FALSE;
#else
        return true;
#endif
    }

    template<std::size_t ByteCapacity, Concurrency::QueueTopology Topology>
    class MashiroRecordQueue {
    public:
        using Storage = RecordRingBuffer<ByteCapacity, Topology>;
        static constexpr std::string_view name = "Mashiro RecordRingBuffer";
        static constexpr std::string_view family = "Mashiro generation-token";
        static constexpr std::string_view referenceCommit = "working-tree";
        static constexpr std::string_view capacitySemantics = "packed-byte-arena";
        static constexpr std::size_t storageBytes = sizeof(Storage);

        [[nodiscard]] bool Push(std::span<const std::byte> payload) noexcept { return ring_.TryWrite(payload); }

        template<class Consumer>
        [[nodiscard]] bool Consume(Consumer&& consumer) noexcept {
            return ring_.TryConsume([&](std::span<const std::byte> record) noexcept { consumer(record); });
        }

        [[nodiscard]] bool Empty() const noexcept { return ring_.Empty(); }

    private:
        Storage ring_{};
    };

    template<std::size_t ByteCapacity, Sora::Concurrency::QueueTopology Topology>
    class SoraRecordQueue {
    public:
        using Storage = Sora::Concurrency::RecordRingBuffer<ByteCapacity, Topology>;
        static constexpr std::string_view name = "Sora RecordRingBuffer";
        static constexpr std::string_view family = "Sora generation-token";
        static constexpr std::string_view referenceCommit = "working-tree";
        static constexpr std::string_view capacitySemantics = "packed-byte-arena";
        static constexpr std::size_t storageBytes = sizeof(Storage);

        [[nodiscard]] bool Push(std::span<const std::byte> payload) noexcept { return ring_.TryWrite(payload); }

        template<class Consumer>
        [[nodiscard]] bool Consume(Consumer&& consumer) noexcept {
            return ring_.TryConsume([&](std::span<const std::byte> record) noexcept { consumer(record); });
        }

        [[nodiscard]] bool Empty() const noexcept { return ring_.IsEmpty(); }

    private:
        Storage ring_{};
    };

    /** @brief Exact queueing data path removed from StructuredLogger, retained only as a migration baseline. */
    template<std::size_t ByteCapacity>
    class LegacyLoggerThreadRing {
    public:
        static constexpr std::string_view name = "legacy StructuredLogger ThreadRing";
        static constexpr std::string_view family = "length-prefix wrapping byte ring";
        static constexpr std::string_view referenceCommit = "pre-RecordRingBuffer working tree";
        static constexpr std::string_view capacitySemantics = "byte-arena-with-wrapping-records";

        [[nodiscard]] bool Push(std::span<const std::byte> payload) noexcept {
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
        [[nodiscard]] bool Consume(Consumer&& consumer) {
            const std::size_t read = readIndex_.load(std::memory_order_relaxed);
            const std::size_t write = writeIndex_.load(std::memory_order_acquire);
            if (read + sizeof(std::uint32_t) > write) {
                return false;
            }

            std::uint32_t payloadSize = 0;
            ReadBytes(read, std::as_writable_bytes(std::span{&payloadSize, 1}));
            if (read + sizeof(payloadSize) + payloadSize > write) {
                return false;
            }
            scratch_.resize(payloadSize);
            ReadBytes(read + sizeof(payloadSize), scratch_);
            consumer(std::span<const std::byte>{scratch_});
            readIndex_.store(read + sizeof(payloadSize) + payloadSize, std::memory_order_release);
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

        [[nodiscard]] bool Empty() const noexcept {
            return readIndex_.load(std::memory_order_relaxed) == writeIndex_.load(std::memory_order_acquire);
        }

        [[nodiscard]] std::size_t StorageBytes() const noexcept { return sizeof(*this) + scratch_.capacity(); }

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

    /** @brief Sora SPSC record ring adapter exposing StructuredLogger's batch-drain path. */
    template<std::size_t ByteCapacity>
    class LoggerRecordRing {
    public:
        using Storage = Sora::Concurrency::RecordRingBuffer<ByteCapacity, Sora::Concurrency::QueueTopologies::Spsc>;
        static constexpr std::string_view name = "Sora RecordRingBuffer";
        static constexpr std::string_view family = "Sora generation-token batch drain";
        static constexpr std::string_view referenceCommit = "working-tree";
        static constexpr std::string_view capacitySemantics = "packed-byte-arena";
        static constexpr std::size_t storageBytes = sizeof(Storage);

        [[nodiscard]] bool Push(std::span<const std::byte> payload) noexcept { return ring_.TryWrite(payload); }

        template<class Consumer>
        std::size_t Drain(Consumer&& consumer) noexcept {
            return ring_.Drain([&](std::span<const std::byte> record) noexcept { consumer(record); });
        }

        [[nodiscard]] bool Empty() const noexcept { return ring_.IsEmpty(); }

    private:
        Storage ring_{};
    };

    enum class MigrationRole : std::uint8_t {
        MashiroSelfA,
        MashiroSelfB,
        SoraSelfA,
        SoraSelfB,
        MashiroForward,
        SoraForward,
        SoraReverse,
        MashiroReverse,
        LegacyLoggerSelfA,
        LegacyLoggerSelfB,
        SoraLoggerSelfA,
        SoraLoggerSelfB,
        LegacyLoggerForward,
        SoraLoggerForward,
        SoraLoggerReverse,
        LegacyLoggerReverse,
    };

    template<class Queue, MigrationRole Role>
    class NamedRecordQueue : public Queue {
    public:
        static constexpr std::string_view name = [] consteval {
            if constexpr (Role == MigrationRole::MashiroSelfA) {
                return "Mashiro self A";
            } else if constexpr (Role == MigrationRole::MashiroSelfB) {
                return "Mashiro self B";
            } else if constexpr (Role == MigrationRole::SoraSelfA) {
                return "Sora self A";
            } else if constexpr (Role == MigrationRole::SoraSelfB) {
                return "Sora self B";
            } else if constexpr (Role == MigrationRole::MashiroForward) {
                return "Mashiro forward";
            } else if constexpr (Role == MigrationRole::SoraForward) {
                return "Sora forward";
            } else if constexpr (Role == MigrationRole::SoraReverse) {
                return "Sora reverse";
            } else if constexpr (Role == MigrationRole::MashiroReverse) {
                return "Mashiro reverse";
            } else if constexpr (Role == MigrationRole::LegacyLoggerSelfA) {
                return "legacy logger self A";
            } else if constexpr (Role == MigrationRole::LegacyLoggerSelfB) {
                return "legacy logger self B";
            } else if constexpr (Role == MigrationRole::SoraLoggerSelfA) {
                return "Sora logger self A";
            } else if constexpr (Role == MigrationRole::SoraLoggerSelfB) {
                return "Sora logger self B";
            } else if constexpr (Role == MigrationRole::LegacyLoggerForward) {
                return "legacy logger forward";
            } else if constexpr (Role == MigrationRole::SoraLoggerForward) {
                return "Sora logger forward";
            } else if constexpr (Role == MigrationRole::SoraLoggerReverse) {
                return "Sora logger reverse";
            } else {
                return "legacy logger reverse";
            }
        }();
    };

    namespace Aeron {

        inline constexpr std::size_t kAlignment = 8;
        inline constexpr std::size_t kHeaderBytes = 8;
        inline constexpr std::uint32_t kPaddingType = 0xFFFF'FFFFU;

        [[nodiscard]] constexpr std::size_t Align(std::size_t bytes) noexcept {
            return (bytes + kAlignment - 1) & ~(kAlignment - 1);
        }

        template<std::size_t ByteCapacity>
        class Arena {
        protected:
            static_assert(std::has_single_bit(ByteCapacity) && ByteCapacity >= 128);
            static constexpr std::size_t kMask = ByteCapacity - 1;
            static constexpr std::size_t kMaxPayload = ByteCapacity / 8;

            [[nodiscard]] std::byte* Bytes() noexcept { return reinterpret_cast<std::byte*>(words_.data()); }

            [[nodiscard]] std::atomic_ref<std::uint32_t> Length(std::size_t index) noexcept {
                return std::atomic_ref<std::uint32_t>{words_[index / sizeof(std::uint32_t)]};
            }

            void StoreLength(std::size_t index, std::int32_t length, std::memory_order order) noexcept {
                Length(index).store(std::bit_cast<std::uint32_t>(length), order);
            }

            [[nodiscard]] std::int32_t LoadLength(std::size_t index, std::memory_order order) noexcept {
                return std::bit_cast<std::int32_t>(Length(index).load(order));
            }

            void StoreType(std::size_t index, std::uint32_t type) noexcept {
                words_[index / sizeof(std::uint32_t) + 1] = type;
            }

            [[nodiscard]] std::uint32_t LoadType(std::size_t index) const noexcept {
                return words_[index / sizeof(std::uint32_t) + 1];
            }

            void Clear(std::size_t index, std::size_t bytes) noexcept { std::memset(Bytes() + index, 0, bytes); }

        private:
            alignas(Platform::kCacheLineSize) std::array<std::uint32_t, ByteCapacity / sizeof(std::uint32_t)> words_{};
        };

    } // namespace Aeron

    template<std::size_t ByteCapacity>
    class AeronSpscQueue : private Aeron::Arena<ByteCapacity> {
        using Base = Aeron::Arena<ByteCapacity>;

    public:
        static constexpr std::string_view name = "Aeron SPSC algorithm";
        static constexpr std::string_view family = "Aeron C SPSC";
        static constexpr std::string_view referenceCommit = kAeronCommit;
        static constexpr std::string_view capacitySemantics = "packed-byte-arena-plus-trailer";
        static constexpr std::size_t storageBytes = ByteCapacity + kAeronTrailerBytes;

        [[nodiscard]] bool Push(std::span<const std::byte> payload) noexcept {
            if (payload.empty() || payload.size() > Base::kMaxPayload) {
                return false;
            }
            const std::size_t recordLength = Aeron::kHeaderBytes + payload.size();
            const auto index = Reserve(recordLength);
            if (!index.has_value()) {
                return false;
            }
            Base::StoreLength(*index, -static_cast<std::int32_t>(recordLength), std::memory_order_release);
            std::memcpy(Base::Bytes() + *index + Aeron::kHeaderBytes, payload.data(), payload.size());
            Base::StoreType(*index, 1);
            Base::StoreLength(*index, static_cast<std::int32_t>(recordLength), std::memory_order_release);
            return true;
        }

        template<class Consumer>
        [[nodiscard]] bool Consume(Consumer&& consumer) noexcept {
            std::uint64_t head = head_.load(std::memory_order_relaxed);
            const std::uint64_t tail = tail_.load(std::memory_order_acquire);
            while (head != tail) {
                const std::size_t index = static_cast<std::size_t>(head) & Base::kMask;
                const std::int32_t length = Base::LoadLength(index, std::memory_order_relaxed);
                if (length <= 0) {
                    return false;
                }
                const std::size_t consumed = Aeron::Align(static_cast<std::size_t>(length));
                const bool padding = Base::LoadType(index) == Aeron::kPaddingType;
                if (!padding) {
                    consumer(std::span<const std::byte>{Base::Bytes() + index + Aeron::kHeaderBytes,
                                                        static_cast<std::size_t>(length) - Aeron::kHeaderBytes});
                }
                Base::Clear(index, consumed);
                head += consumed;
                head_.store(head, std::memory_order_release);
                if (!padding) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool Empty() const noexcept {
            return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
        }

    private:
        [[nodiscard]] std::optional<std::size_t> Reserve(std::size_t recordLength) noexcept {
            const std::size_t alignedRecordLength = Aeron::Align(recordLength);
            const std::size_t requiredCapacity = alignedRecordLength + Aeron::kHeaderBytes;
            std::uint64_t head = cachedHead_;
            const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
            if (!HasCapacity(tail, head, requiredCapacity)) {
                head = head_.load(std::memory_order_acquire);
                if (!HasCapacity(tail, head, requiredCapacity)) {
                    return std::nullopt;
                }
                cachedHead_ = head;
            }

            std::size_t index = static_cast<std::size_t>(tail) & Base::kMask;
            const std::size_t toEnd = ByteCapacity - index;
            std::uint64_t nextTail = tail + alignedRecordLength;
            if (alignedRecordLength == toEnd) {
                tail_.store(nextTail, std::memory_order_release);
                Base::StoreLength(0, 0, std::memory_order_relaxed);
                Base::StoreType(0, 0);
                return index;
            }

            std::size_t padding = 0;
            if (requiredCapacity > toEnd) {
                std::size_t headIndex = static_cast<std::size_t>(head) & Base::kMask;
                if (requiredCapacity > headIndex) {
                    head = head_.load(std::memory_order_acquire);
                    headIndex = static_cast<std::size_t>(head) & Base::kMask;
                    if (requiredCapacity > headIndex) {
                        return std::nullopt;
                    }
                    cachedHead_ = head;
                }
                padding = toEnd;
                nextTail += padding;
                index = 0;
            }

            tail_.store(nextTail, std::memory_order_release);
            if (padding != 0) {
                const std::size_t paddingIndex = static_cast<std::size_t>(tail) & Base::kMask;
                Base::StoreLength(0, 0, std::memory_order_relaxed);
                Base::StoreType(0, 0);
                Base::StoreLength(paddingIndex, -static_cast<std::int32_t>(padding), std::memory_order_release);
                Base::StoreType(paddingIndex, Aeron::kPaddingType);
                Base::StoreLength(paddingIndex, static_cast<std::int32_t>(padding), std::memory_order_release);
            }
            const std::size_t nextHeader = index + alignedRecordLength;
            Base::StoreLength(nextHeader, 0, std::memory_order_relaxed);
            Base::StoreType(nextHeader, 0);
            return index;
        }

        [[nodiscard]] static bool HasCapacity(std::uint64_t tail, std::uint64_t head, std::size_t required) noexcept {
            const std::uint64_t occupied = tail - head;
            return occupied <= ByteCapacity && required <= ByteCapacity - occupied;
        }

        alignas(2 * Platform::kCacheLineSize) std::atomic<std::uint64_t> tail_{0};
        alignas(2 * Platform::kCacheLineSize) std::uint64_t cachedHead_{0};
        alignas(2 * Platform::kCacheLineSize) std::atomic<std::uint64_t> head_{0};
    };

    template<std::size_t ByteCapacity>
    class AeronMpscQueue : private Aeron::Arena<ByteCapacity> {
        using Base = Aeron::Arena<ByteCapacity>;

    public:
        static constexpr std::string_view name = "Aeron MPSC algorithm";
        static constexpr std::string_view family = "Aeron C ManyToOne";
        static constexpr std::string_view referenceCommit = kAeronCommit;
        static constexpr std::string_view capacitySemantics = "packed-byte-arena-plus-trailer";
        static constexpr std::size_t storageBytes = ByteCapacity + kAeronTrailerBytes;

        [[nodiscard]] bool Push(std::span<const std::byte> payload) noexcept {
            if (payload.empty() || payload.size() > Base::kMaxPayload) {
                return false;
            }
            const std::size_t recordLength = Aeron::kHeaderBytes + payload.size();
            const std::size_t required = Aeron::Align(recordLength);
            const auto reservation = Reserve(required);
            if (!reservation.has_value()) {
                return false;
            }

            std::size_t index = reservation->index;
            if (reservation->padding != 0) {
                Base::StoreLength(index, -static_cast<std::int32_t>(reservation->padding), std::memory_order_release);
                Base::StoreType(index, Aeron::kPaddingType);
                Base::StoreLength(index, static_cast<std::int32_t>(reservation->padding), std::memory_order_release);
                index = 0;
            }
            Base::StoreLength(index, -static_cast<std::int32_t>(recordLength), std::memory_order_release);
            Base::StoreType(index, 1);
            std::memcpy(Base::Bytes() + index + Aeron::kHeaderBytes, payload.data(), payload.size());
            Base::StoreLength(index, static_cast<std::int32_t>(recordLength), std::memory_order_release);
            return true;
        }

        template<class Consumer>
        [[nodiscard]] bool Consume(Consumer&& consumer) noexcept {
            std::uint64_t head = head_.load(std::memory_order_relaxed);
            for (;;) {
                const std::size_t index = static_cast<std::size_t>(head) & Base::kMask;
                const std::int32_t length = Base::LoadLength(index, std::memory_order_acquire);
                if (length <= 0) {
                    return false;
                }
                const std::size_t consumed = Aeron::Align(static_cast<std::size_t>(length));
                const bool padding = Base::LoadType(index) == Aeron::kPaddingType;
                if (!padding) {
                    consumer(std::span<const std::byte>{Base::Bytes() + index + Aeron::kHeaderBytes,
                                                        static_cast<std::size_t>(length) - Aeron::kHeaderBytes});
                }
                Base::Clear(index, consumed);
                head += consumed;
                head_.store(head, std::memory_order_release);
                if (!padding) {
                    return true;
                }
            }
        }

        [[nodiscard]] bool Empty() const noexcept {
            return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
        }

    private:
        struct Reservation {
            std::size_t index;
            std::size_t padding;
        };

        [[nodiscard]] std::optional<Reservation> Reserve(std::size_t required) noexcept {
            std::uint64_t head = cachedHead_.load(std::memory_order_acquire);
            std::uint64_t tail = tail_.load(std::memory_order_acquire);
            for (;;) {
                if (!HasCapacity(tail, head, required)) {
                    head = head_.load(std::memory_order_acquire);
                    if (!HasCapacity(tail, head, required)) {
                        return std::nullopt;
                    }
                    cachedHead_.store(head, std::memory_order_release);
                }

                const std::size_t index = static_cast<std::size_t>(tail) & Base::kMask;
                const std::size_t toEnd = ByteCapacity - index;
                const std::size_t padding = required > toEnd ? toEnd : 0;
                if (padding != 0 && !HasCapacity(tail, head, required + padding)) {
                    head = head_.load(std::memory_order_acquire);
                    if (!HasCapacity(tail, head, required + padding)) {
                        return std::nullopt;
                    }
                    cachedHead_.store(head, std::memory_order_release);
                }

                if (tail_.compare_exchange_weak(tail, tail + required + padding, std::memory_order_relaxed)) {
                    return Reservation{index, padding};
                }
            }
        }

        [[nodiscard]] static bool HasCapacity(std::uint64_t tail, std::uint64_t head, std::size_t required) noexcept {
            const std::uint64_t occupied = tail - head;
            return occupied <= ByteCapacity && required <= ByteCapacity - occupied;
        }

        alignas(2 * Platform::kCacheLineSize) std::atomic<std::uint64_t> tail_{0};
        alignas(2 * Platform::kCacheLineSize) std::atomic<std::uint64_t> cachedHead_{0};
        alignas(2 * Platform::kCacheLineSize) std::atomic<std::uint64_t> head_{0};
    };

    template<std::size_t PayloadBytes>
    struct FixedRecord {
        std::array<std::byte, PayloadBytes> bytes{};

        FixedRecord() = default;
        explicit FixedRecord(std::span<const std::byte> payload) noexcept {
            std::memcpy(bytes.data(), payload.data(), PayloadBytes);
        }
    };

    /** @brief DPDK classic ST/MT head-tail ring port with one fixed-size record per slot. */
    template<std::size_t ByteCapacity, std::size_t PayloadBytes, Concurrency::QueueTopology Topology>
    class DpdkClassicRecordQueue {
        using Record = FixedRecord<PayloadBytes>;
        static constexpr std::size_t kSlotCount = std::bit_floor(ByteCapacity / sizeof(Record));
        static constexpr std::uint32_t kCapacity = static_cast<std::uint32_t>(kSlotCount - 1);
        static constexpr std::uint32_t kMask = static_cast<std::uint32_t>(kSlotCount - 1);
        static constexpr bool kMultipleProducers = Topology.producers == Concurrency::QueueCardinality::Multiple;
        static constexpr bool kMultipleConsumers = Topology.consumers == Concurrency::QueueCardinality::Multiple;

        static_assert(kSlotCount >= 2 && std::has_single_bit(kSlotCount));
        static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

    public:
        static constexpr std::string_view name = "DPDK rte_ring classic fixed-record";
        static constexpr std::string_view family = "DPDK classic ST/MT port";
        static constexpr std::string_view referenceCommit = kDpdkCommit;
        static constexpr std::string_view capacitySemantics = "fixed-slot-budget-includes-sentinel";
        static constexpr std::size_t storageBytes = sizeof(DpdkClassicRecordQueue);

        [[nodiscard]] bool Push(std::span<const std::byte> payload) noexcept {
            if (payload.size() != PayloadBytes) {
                return false;
            }
            std::uint32_t position = 0;
            if (!TryMove<kMultipleProducers>(producerHead_, consumerTail_, kCapacity, position)) {
                return false;
            }
            std::memcpy(records_[position & kMask].bytes.data(), payload.data(), PayloadBytes);
            Publish<kMultipleProducers>(producerTail_, position);
            return true;
        }

        template<class Consumer>
        [[nodiscard]] bool Consume(Consumer&& consumer) noexcept {
            std::uint32_t position = 0;
            if (!TryMove<kMultipleConsumers>(consumerHead_, producerTail_, 0, position)) {
                return false;
            }
            const Record& record = records_[position & kMask];
            consumer(std::span<const std::byte>{record.bytes});
            Publish<kMultipleConsumers>(consumerTail_, position);
            return true;
        }

        [[nodiscard]] bool Empty() const noexcept {
            return consumerTail_.load(std::memory_order_acquire) == producerTail_.load(std::memory_order_acquire);
        }

    private:
        template<bool Multiple>
        [[nodiscard]] static bool TryMove(std::atomic<std::uint32_t>& head,
                                          const std::atomic<std::uint32_t>& oppositeTail, std::uint32_t capacity,
                                          std::uint32_t& position) noexcept {
            if constexpr (!Multiple) {
                position = head.load(std::memory_order_relaxed);
                const std::uint32_t entries = capacity + oppositeTail.load(std::memory_order_acquire) - position;
                if (entries == 0) {
                    return false;
                }
                head.store(position + 1, std::memory_order_relaxed);
                return true;
            } else {
                position = head.load(std::memory_order_acquire);
                for (;;) {
                    const std::uint32_t entries = capacity + oppositeTail.load(std::memory_order_acquire) - position;
                    if (entries == 0) {
                        return false;
                    }
                    if (head.compare_exchange_weak(position, position + 1, std::memory_order_release,
                                                   std::memory_order_acquire)) {
                        return true;
                    }
                }
            }
        }

        template<bool Multiple>
        static void Publish(std::atomic<std::uint32_t>& tail, std::uint32_t position) noexcept {
            if constexpr (Multiple) {
                while (tail.load(std::memory_order_relaxed) != position) {
                    Relax();
                }
            }
            tail.store(position + 1, std::memory_order_release);
        }

        alignas(Platform::kCacheLineSize) std::atomic<std::uint32_t> producerHead_{0};
        alignas(Platform::kCacheLineSize) std::atomic<std::uint32_t> producerTail_{0};
        alignas(Platform::kCacheLineSize) std::atomic<std::uint32_t> consumerHead_{0};
        alignas(Platform::kCacheLineSize) std::atomic<std::uint32_t> consumerTail_{0};
        alignas(Platform::kCacheLineSize) std::array<Record, kSlotCount> records_{};
    };

    /** @brief Rigtorp's fixed-slot SPSC queue under the same nominal byte budget. */
    template<std::size_t ByteCapacity, std::size_t PayloadBytes>
    class RigtorpSpscRecordQueue {
        using Record = FixedRecord<PayloadBytes>;
        static constexpr std::size_t kCapacity = std::max<std::size_t>(1, ByteCapacity / sizeof(Record));
        static constexpr std::size_t kPadding = (Platform::kCacheLineSize - 1) / sizeof(Record) + 1;

    public:
        static constexpr std::string_view name = "Rigtorp SPSCQueue fixed-record";
        static constexpr std::string_view family = "Rigtorp SPSCQueue";
        static constexpr std::string_view referenceCommit = kRigtorpSpscCommit;
        static constexpr std::string_view capacitySemantics = "usable-fixed-slots-plus-sentinel-padding";
        RigtorpSpscRecordQueue() : queue_(kCapacity) {}

        [[nodiscard]] std::size_t StorageBytes() const noexcept {
            return sizeof(queue_) + (queue_.capacity() + 1 + 2 * kPadding) * sizeof(Record);
        }

        [[nodiscard]] bool Push(std::span<const std::byte> payload) noexcept {
            return payload.size() == PayloadBytes && queue_.try_emplace(payload);
        }

        template<class Consumer>
        [[nodiscard]] bool Consume(Consumer&& consumer) noexcept {
            Record* record = queue_.front();
            if (record == nullptr) {
                return false;
            }
            consumer(std::span<const std::byte>{record->bytes});
            queue_.pop();
            return true;
        }

        [[nodiscard]] bool Empty() const noexcept { return queue_.empty(); }

    private:
        rigtorp::SPSCQueue<Record> queue_;
    };

    /** @brief Rigtorp's strict-FIFO MPMC queue carrying one fixed-size record per slot. */
    template<std::size_t ByteCapacity, std::size_t PayloadBytes>
    class RigtorpMpmcRecordQueue {
        using Record = FixedRecord<PayloadBytes>;
        using Slot = rigtorp::mpmc::Slot<Record>;
        static constexpr std::size_t kCapacity = std::max<std::size_t>(1, ByteCapacity / sizeof(Slot));

    public:
        static constexpr std::string_view name = "Rigtorp MPMCQueue fixed-record";
        static constexpr std::string_view family = "Rigtorp MPMCQueue";
        static constexpr std::string_view referenceCommit = kRigtorpMpmcCommit;
        static constexpr std::string_view capacitySemantics = "cache-line-slots-plus-padding-slot";
        static constexpr std::size_t storageBytes = sizeof(rigtorp::MPMCQueue<Record>) + (kCapacity + 1) * sizeof(Slot);

        RigtorpMpmcRecordQueue() : queue_(kCapacity) {}

        [[nodiscard]] bool Push(std::span<const std::byte> payload) noexcept {
            return payload.size() == PayloadBytes && queue_.try_emplace(payload);
        }

        template<class Consumer>
        [[nodiscard]] bool Consume(Consumer&& consumer) noexcept {
            Record record;
            if (!queue_.try_pop(record)) {
                return false;
            }
            consumer(std::span<const std::byte>{record.bytes});
            return true;
        }

        [[nodiscard]] bool Empty() const noexcept { return queue_.empty(); }

    private:
        rigtorp::MPMCQueue<Record> queue_;
    };

    template<std::size_t ByteCapacity>
    class MutexPackedQueue {
    public:
        static constexpr std::string_view name = "mutex packed RecordRingBuffer";
        static constexpr std::string_view family = "std::mutex + Mashiro Local";
        static constexpr std::string_view referenceCommit = "standard-library";
        static constexpr std::string_view capacitySemantics = "packed-byte-arena";
        static constexpr std::size_t storageBytes =
            sizeof(RecordRingBuffer<ByteCapacity, Concurrency::QueueTopologies::Local>) + sizeof(std::mutex);

        [[nodiscard]] bool Push(std::span<const std::byte> payload) {
            std::scoped_lock lock{mutex_};
            return ring_.TryWrite(payload);
        }

        template<class Consumer>
        [[nodiscard]] bool Consume(Consumer&& consumer) {
            std::scoped_lock lock{mutex_};
            return ring_.TryConsume([&](std::span<const std::byte> record) { consumer(record); });
        }

        [[nodiscard]] bool Empty() const {
            std::scoped_lock lock{mutex_};
            return ring_.Empty();
        }

    private:
        mutable std::mutex mutex_{};
        RecordRingBuffer<ByteCapacity, Concurrency::QueueTopologies::Local> ring_{};
    };

    template<std::size_t ByteCapacity>
    class MutexAllocatingQueue {
    public:
        static constexpr std::string_view name = "mutex allocating deque";
        static constexpr std::string_view family = "std::mutex + deque<vector<byte>>";
        static constexpr std::string_view referenceCommit = "standard-library";
        static constexpr std::string_view capacitySemantics = "packed-byte-accounting-with-allocation";
        static constexpr std::size_t storageBytes =
            sizeof(std::mutex) + sizeof(std::deque<std::vector<std::byte>>) + sizeof(std::size_t);

        [[nodiscard]] bool Push(std::span<const std::byte> payload) {
            const std::size_t occupied = Detail::AlignRecordBytes(Detail::kRecordHeaderBytes + payload.size());
            std::scoped_lock lock{mutex_};
            if (occupied_ + occupied > ByteCapacity) {
                return false;
            }
            queue_.emplace_back(payload.begin(), payload.end());
            occupied_ += occupied;
            return true;
        }

        template<class Consumer>
        [[nodiscard]] bool Consume(Consumer&& consumer) {
            std::scoped_lock lock{mutex_};
            if (queue_.empty()) {
                return false;
            }
            const auto& payload = queue_.front();
            consumer(std::span<const std::byte>{payload});
            occupied_ -= Detail::AlignRecordBytes(Detail::kRecordHeaderBytes + payload.size());
            queue_.pop_front();
            return true;
        }

        [[nodiscard]] bool Empty() const {
            std::scoped_lock lock{mutex_};
            return queue_.empty();
        }

    private:
        mutable std::mutex mutex_{};
        std::deque<std::vector<std::byte>> queue_{};
        std::size_t occupied_{};
    };

    template<class Queue>
    [[nodiscard]] std::size_t QueueStorageBytes(const Queue& queue) noexcept {
        if constexpr (requires {
                          { queue.StorageBytes() } -> std::convertible_to<std::size_t>;
                      }) {
            return queue.StorageBytes();
        } else {
            return Queue::storageBytes;
        }
    }

    template<class Queue, std::size_t PayloadBytes>
    [[nodiscard]] Sample RunSample(std::size_t producers, std::size_t consumers, std::size_t recordsPerProducer,
                                   bool pinWorkers) {
        static_assert(PayloadBytes >= sizeof(std::uint64_t));
        auto queue = std::make_unique<Queue>();
        const std::size_t workerCount = producers + consumers;
        const std::vector<std::uint32_t> cpuSets = SelectWorkerCpuSets(workerCount, pinWorkers);
        std::barrier start{static_cast<std::ptrdiff_t>(workerCount + 1)};
        std::latch done{static_cast<std::ptrdiff_t>(workerCount)};
        std::atomic<std::size_t> activeProducers{producers};
        std::atomic<bool> placementFailed{false};
        std::vector<WorkerResult> producerResults(producers);
        std::vector<WorkerResult> consumerResults(consumers);
        std::vector<std::thread> workers;
        workers.reserve(workerCount);

        for (std::size_t producer = 0; producer < producers; ++producer) {
            workers.emplace_back([&, producer] {
                if (!BindWorker(cpuSets, consumers + producer)) {
                    placementFailed.store(true, std::memory_order_relaxed);
                }
                std::array<std::byte, PayloadBytes> payload{};
                std::ranges::fill(payload, static_cast<std::byte>(producer + 1));
                const std::uint64_t first = producer * recordsPerProducer + 1;
                WorkerResult result{};
                start.arrive_and_wait();
                for (std::size_t index = 0; index < recordsPerProducer; ++index) {
                    const std::uint64_t id = first + index;
                    std::memcpy(payload.data(), &id, sizeof(id));
                    while (!queue->Push(payload)) {
                        ++result.retries;
                        Relax();
                    }
                    ++result.transferred;
                }
                producerResults[producer] = result;
                activeProducers.fetch_sub(1, std::memory_order_release);
                done.count_down();
            });
        }

        for (std::size_t consumer = 0; consumer < consumers; ++consumer) {
            workers.emplace_back([&, consumer] {
                if (!BindWorker(cpuSets, consumer)) {
                    placementFailed.store(true, std::memory_order_relaxed);
                }
                WorkerResult result{};
                auto consume = [&](std::span<const std::byte> payload) noexcept {
                    std::uint64_t id = 0;
                    if (payload.size() >= sizeof(id)) {
                        std::memcpy(&id, payload.data(), sizeof(id));
                    }
                    result.checksum += id;
                    ++result.transferred;
                };
                start.arrive_and_wait();
                for (;;) {
                    const bool consumed = [&] {
                        if constexpr (requires { queue->Drain(consume); }) {
                            return queue->Drain(consume) != 0;
                        } else {
                            return queue->Consume(consume);
                        }
                    }();
                    if (consumed) {
                        continue;
                    }
                    ++result.retries;
                    if (activeProducers.load(std::memory_order_acquire) == 0 && queue->Empty()) {
                        break;
                    }
                    Relax();
                }
                consumerResults[consumer] = result;
                done.count_down();
            });
        }

        const auto begin = Clock::now();
        start.arrive_and_wait();
        done.wait();
        const auto end = Clock::now();
        for (std::thread& worker : workers) {
            worker.join();
        }
        if (placementFailed.load(std::memory_order_relaxed)) {
            throw std::runtime_error("failed to bind a record benchmark worker to its selected CPU set");
        }

        const std::uint64_t expectedRecords = producers * recordsPerProducer;
        const std::uint64_t expectedChecksum = expectedRecords * (expectedRecords + 1) / 2;
        const auto fold = [](const std::vector<WorkerResult>& results, auto member) {
            return std::ranges::fold_left(
                results, std::uint64_t{},
                [member](std::uint64_t sum, const WorkerResult& result) { return sum + result.*member; });
        };
        const std::uint64_t records = fold(consumerResults, &WorkerResult::transferred);
        const std::uint64_t checksum = fold(consumerResults, &WorkerResult::checksum);
        if (records != expectedRecords || checksum != expectedChecksum) {
            throw std::runtime_error("record benchmark integrity check failed");
        }
        return {
            .seconds = std::chrono::duration<double>(end - begin).count(),
            .records = records,
            .pushRetries = fold(producerResults, &WorkerResult::retries),
            .popRetries = fold(consumerResults, &WorkerResult::retries),
            .storageBytes = QueueStorageBytes(*queue),
        };
    }

    class CsvWriter {
    public:
        explicit CsvWriter(const Config& config) : stream_(config.output) {
            if (!stream_) {
                throw std::runtime_error("cannot open record benchmark output");
            }
            stream_ << "label,pair,topology,implementation,family,payload_bytes,byte_capacity,producers,consumers,"
                       "sample,order,records,seconds,million_records_per_second,payload_gib_per_second,"
                       "push_retries,pop_retries,storage_bytes,thread_placement,reference_commit,"
                       "capacity_semantics\n";
        }

        template<class Queue>
        void Write(const Config& config, std::string_view pair, std::string_view topology, std::size_t payloadBytes,
                   std::size_t byteCapacity, std::size_t producers, std::size_t consumers, std::size_t sample,
                   std::string_view order, const Sample& result) {
            const double recordsPerSecond = static_cast<double>(result.records) / result.seconds;
            const double payloadGiB = recordsPerSecond * payloadBytes / static_cast<double>(std::uint64_t{1} << 30);
            stream_ << config.label << ',' << pair << ',' << topology << ',' << Queue::name << ',' << Queue::family
                    << ',' << payloadBytes << ',' << byteCapacity << ',' << producers << ',' << consumers << ','
                    << sample << ',' << order << ',' << result.records << ',' << result.seconds << ','
                    << recordsPerSecond / 1'000'000.0 << ',' << payloadGiB << ',' << result.pushRetries << ','
                    << result.popRetries << ',' << result.storageBytes << ','
                    << (config.pinWorkers ? "cpu-set-high-priority" : "scheduler") << ',' << Queue::referenceCommit
                    << ',' << Queue::capacitySemantics << '\n';
        }

    private:
        std::ofstream stream_;
    };

    template<class Queue, std::size_t PayloadBytes>
    [[nodiscard]] std::size_t Calibrate(const Config& config, std::size_t producers, std::size_t consumers) {
        constexpr std::size_t kMaximumAttempts = 6;
        constexpr double kMargin = 1.15;
        std::size_t records = config.recordsPerProducer;
        for (std::size_t attempt = 0; config.minimumSampleSeconds != 0.0; ++attempt) {
            const Sample pilot = RunSample<Queue, PayloadBytes>(producers, consumers, records, config.pinWorkers);
            if (pilot.seconds >= config.minimumSampleSeconds) {
                return records;
            }
            if (attempt + 1 == kMaximumAttempts) {
                throw std::runtime_error("record benchmark calibration did not reach minimum duration");
            }
            const double scale = config.minimumSampleSeconds * kMargin / pilot.seconds;
            records = static_cast<std::size_t>(std::ceil(records * std::max(2.0, scale)));
        }
        return records;
    }

    template<class First, class Second, std::size_t PayloadBytes>
    void RunPair(const Config& config, CsvWriter& writer, std::string_view pair, std::string_view topology,
                 std::size_t byteCapacity, std::size_t producers, std::size_t consumers, std::uint64_t orderSeed) {
        const std::size_t firstRecords = Calibrate<First, PayloadBytes>(config, producers, consumers);
        const std::size_t secondRecords = Calibrate<Second, PayloadBytes>(config, producers, consumers);
        std::cout << topology << " | " << pair << " | payload=" << PayloadBytes << " | " << producers << 'P'
                  << consumers << "C | records/producer=" << firstRecords << '/' << secondRecords << '\n';

        for (std::size_t warmup = 0; warmup < config.warmups; ++warmup) {
            static_cast<void>(
                RunSample<First, PayloadBytes>(producers, consumers, firstRecords / 2, config.pinWorkers));
            static_cast<void>(
                RunSample<Second, PayloadBytes>(producers, consumers, secondRecords / 2, config.pinWorkers));
        }
        for (std::size_t sample = 0; sample < config.samples; ++sample) {
            const bool firstBeforeSecond = ((sample + orderSeed) & 1U) == 0;
            auto runFirst = [&] {
                const Sample result =
                    RunSample<First, PayloadBytes>(producers, consumers, firstRecords, config.pinWorkers);
                writer.Write<First>(config, pair, topology, PayloadBytes, byteCapacity, producers, consumers, sample,
                                    firstBeforeSecond ? "AB:first" : "BA:second", result);
            };
            auto runSecond = [&] {
                const Sample result =
                    RunSample<Second, PayloadBytes>(producers, consumers, secondRecords, config.pinWorkers);
                writer.Write<Second>(config, pair, topology, PayloadBytes, byteCapacity, producers, consumers, sample,
                                     firstBeforeSecond ? "AB:second" : "BA:first", result);
            };
            if (firstBeforeSecond) {
                runFirst();
                runSecond();
            } else {
                runSecond();
                runFirst();
            }
        }
    }

    /** @brief Run one equal-work migration pair so both implementations transfer the same record count. */
    template<class First, class Second, std::size_t PayloadBytes>
    void RunMigrationPair(const Config& config, CsvWriter& writer, std::string_view pair, std::string_view topology,
                          std::size_t byteCapacity, std::size_t producers, std::size_t consumers,
                          std::uint64_t orderSeed) {
        const std::size_t records = std::max(Calibrate<First, PayloadBytes>(config, producers, consumers),
                                             Calibrate<Second, PayloadBytes>(config, producers, consumers));
        std::cout << topology << " | " << pair << " | payload=" << PayloadBytes << " | " << producers << 'P'
                  << consumers << "C | records/producer=" << records << '\n';

        for (std::size_t warmup = 0; warmup < config.warmups; ++warmup) {
            const std::size_t warmupRecords = std::max(1uz, records / 2);
            static_cast<void>(RunSample<First, PayloadBytes>(producers, consumers, warmupRecords, config.pinWorkers));
            static_cast<void>(RunSample<Second, PayloadBytes>(producers, consumers, warmupRecords, config.pinWorkers));
        }
        for (std::size_t sample = 0; sample < config.samples; ++sample) {
            const bool firstBeforeSecond = ((sample + orderSeed) & 1U) == 0;
            auto runFirst = [&] {
                const Sample result = RunSample<First, PayloadBytes>(producers, consumers, records, config.pinWorkers);
                writer.Write<First>(config, pair, topology, PayloadBytes, byteCapacity, producers, consumers, sample,
                                    firstBeforeSecond ? "AB:first" : "BA:second", result);
            };
            auto runSecond = [&] {
                const Sample result = RunSample<Second, PayloadBytes>(producers, consumers, records, config.pinWorkers);
                writer.Write<Second>(config, pair, topology, PayloadBytes, byteCapacity, producers, consumers, sample,
                                     firstBeforeSecond ? "AB:second" : "BA:first", result);
            };
            if (firstBeforeSecond) {
                runFirst();
                runSecond();
            } else {
                runSecond();
                runFirst();
            }
        }
    }

    struct Case {
        std::function<void()> run;
    };

    /** @brief Append self-controls and both template placements for one migration topology. */
    template<class MashiroQueue, class SoraQueue, std::size_t PayloadBytes>
    void AppendMigrationTopologyCases(const Config& config, CsvWriter& writer, std::vector<Case>& cases,
                                      std::string_view topology, std::size_t byteCapacity, std::size_t producers,
                                      std::size_t consumers) {
        static_assert(sizeof(typename MashiroQueue::Storage) == sizeof(typename SoraQueue::Storage));
        static_assert(alignof(typename MashiroQueue::Storage) == alignof(typename SoraQueue::Storage));
        static_assert(MashiroQueue::storageBytes == SoraQueue::storageBytes);

        using MashiroSelfA = NamedRecordQueue<MashiroQueue, MigrationRole::MashiroSelfA>;
        using MashiroSelfB = NamedRecordQueue<MashiroQueue, MigrationRole::MashiroSelfB>;
        using SoraSelfA = NamedRecordQueue<SoraQueue, MigrationRole::SoraSelfA>;
        using SoraSelfB = NamedRecordQueue<SoraQueue, MigrationRole::SoraSelfB>;
        using MashiroForward = NamedRecordQueue<MashiroQueue, MigrationRole::MashiroForward>;
        using SoraForward = NamedRecordQueue<SoraQueue, MigrationRole::SoraForward>;
        using SoraReverse = NamedRecordQueue<SoraQueue, MigrationRole::SoraReverse>;
        using MashiroReverse = NamedRecordQueue<MashiroQueue, MigrationRole::MashiroReverse>;

        auto add = [&]<class First, class Second>(std::string_view pair) {
            if ((!config.pairFilter.empty() && !pair.contains(config.pairFilter)) ||
                (!config.topologyFilter.empty() && topology != config.topologyFilter)) {
                return;
            }
            const std::uint64_t orderSeed = config.seed + cases.size();
            cases.push_back({[&, pair, topology, byteCapacity, producers, consumers, orderSeed] {
                RunMigrationPair<First, Second, PayloadBytes>(config, writer, pair, topology, byteCapacity, producers,
                                                              consumers, orderSeed);
            }});
        };

        add.template operator()<MashiroSelfA, MashiroSelfB>("migration Mashiro self");
        add.template operator()<SoraSelfA, SoraSelfB>("migration Sora self");
        add.template operator()<MashiroForward, SoraForward>("migration forward");
        add.template operator()<SoraReverse, MashiroReverse>("migration reverse");
    }

    /** @brief Append the complete packed-record migration matrix for one payload size. */
    template<std::size_t ByteCapacity, std::size_t PayloadBytes>
    void AppendMigrationPayloadCases(const Config& config, CsvWriter& writer, std::vector<Case>& cases, bool scaling) {
        using MashiroSpsc = MashiroRecordQueue<ByteCapacity, Concurrency::QueueTopologies::Spsc>;
        using SoraSpsc = SoraRecordQueue<ByteCapacity, Sora::Concurrency::QueueTopologies::Spsc>;
        AppendMigrationTopologyCases<MashiroSpsc, SoraSpsc, PayloadBytes>(config, writer, cases, "SPSC", ByteCapacity,
                                                                          1, 1);

        const auto appendMpsc = [&](std::size_t producers) {
            using MashiroMpsc = MashiroRecordQueue<ByteCapacity, Concurrency::QueueTopologies::Mpsc>;
            using SoraMpsc = SoraRecordQueue<ByteCapacity, Sora::Concurrency::QueueTopologies::Mpsc>;
            AppendMigrationTopologyCases<MashiroMpsc, SoraMpsc, PayloadBytes>(config, writer, cases, "MPSC",
                                                                              ByteCapacity, producers, 1);
        };
        const auto appendSpmc = [&](std::size_t consumers) {
            using MashiroSpmc = MashiroRecordQueue<ByteCapacity, Concurrency::QueueTopologies::Spmc>;
            using SoraSpmc = SoraRecordQueue<ByteCapacity, Sora::Concurrency::QueueTopologies::Spmc>;
            AppendMigrationTopologyCases<MashiroSpmc, SoraSpmc, PayloadBytes>(config, writer, cases, "SPMC",
                                                                              ByteCapacity, 1, consumers);
        };
        const auto appendMpmc = [&](std::size_t endpoints) {
            using MashiroMpmc = MashiroRecordQueue<ByteCapacity, Concurrency::QueueTopologies::Mpmc>;
            using SoraMpmc = SoraRecordQueue<ByteCapacity, Sora::Concurrency::QueueTopologies::Mpmc>;
            AppendMigrationTopologyCases<MashiroMpmc, SoraMpmc, PayloadBytes>(config, writer, cases, "MPMC",
                                                                              ByteCapacity, endpoints, endpoints);
        };

        appendMpsc(4);
        appendSpmc(4);
        appendMpmc(4);
        if (scaling) {
            appendMpsc(2);
            appendMpsc(8);
            appendSpmc(2);
            appendSpmc(8);
            appendMpmc(2);
        }
    }

    /** @brief Append the direct StructuredLogger ring-replacement comparison for one payload size. */
    template<std::size_t ByteCapacity, std::size_t PayloadBytes>
    void AppendLoggerMigrationCase(const Config& config, CsvWriter& writer, std::vector<Case>& cases) {
        using Legacy = LegacyLoggerThreadRing<ByteCapacity>;
        using Current = LoggerRecordRing<ByteCapacity>;
        constexpr std::string_view topology = "SPSC";
        using LegacySelfA = NamedRecordQueue<Legacy, MigrationRole::LegacyLoggerSelfA>;
        using LegacySelfB = NamedRecordQueue<Legacy, MigrationRole::LegacyLoggerSelfB>;
        using SoraSelfA = NamedRecordQueue<Current, MigrationRole::SoraLoggerSelfA>;
        using SoraSelfB = NamedRecordQueue<Current, MigrationRole::SoraLoggerSelfB>;
        using LegacyForward = NamedRecordQueue<Legacy, MigrationRole::LegacyLoggerForward>;
        using SoraForward = NamedRecordQueue<Current, MigrationRole::SoraLoggerForward>;
        using SoraReverse = NamedRecordQueue<Current, MigrationRole::SoraLoggerReverse>;
        using LegacyReverse = NamedRecordQueue<Legacy, MigrationRole::LegacyLoggerReverse>;

        auto add = [&]<class First, class Second>(std::string_view pair) {
            if ((!config.pairFilter.empty() && !pair.contains(config.pairFilter)) ||
                (!config.topologyFilter.empty() && topology != config.topologyFilter)) {
                return;
            }
            const std::uint64_t orderSeed = config.seed + cases.size();
            cases.push_back({[&, pair, topology, orderSeed] {
                RunMigrationPair<First, Second, PayloadBytes>(config, writer, pair, topology, ByteCapacity, 1, 1,
                                                              orderSeed);
            }});
        };

        add.template operator()<LegacySelfA, LegacySelfB>("logger legacy self");
        add.template operator()<SoraSelfA, SoraSelfB>("logger Sora self");
        add.template operator()<LegacyForward, SoraForward>("logger migration forward");
        add.template operator()<SoraReverse, LegacyReverse>("logger migration reverse");
    }

    template<std::size_t ByteCapacity, std::size_t PayloadBytes>
    void AppendPayloadCases(const Config& config, CsvWriter& writer, std::vector<Case>& cases, bool scaling) {
        using Packed = MutexPackedQueue<ByteCapacity>;
        using Allocating = MutexAllocatingQueue<ByteCapacity>;

        auto add = [&]<class First, class Second>(std::string_view pair, std::string_view topology,
                                                  std::size_t producers, std::size_t consumers) {
            if ((!config.pairFilter.empty() && !pair.contains(config.pairFilter)) ||
                (!config.topologyFilter.empty() && topology != config.topologyFilter)) {
                return;
            }
            const std::uint64_t orderSeed = config.seed + cases.size();
            cases.push_back({[&, pair, topology, producers, consumers, orderSeed] {
                RunPair<First, Second, PayloadBytes>(config, writer, pair, topology, ByteCapacity, producers, consumers,
                                                     orderSeed);
            }});
        };

        using Spsc = MashiroRecordQueue<ByteCapacity, Concurrency::QueueTopologies::Spsc>;
        add.template operator()<Spsc, AeronSpscQueue<ByteCapacity>>("Mashiro vs Aeron", "SPSC", 1, 1);
        add.template
        operator()<Spsc, DpdkClassicRecordQueue<ByteCapacity, PayloadBytes, Concurrency::QueueTopologies::Spsc>>(
            "Mashiro vs DPDK classic", "SPSC", 1, 1);
        add.template operator()<Spsc, RigtorpSpscRecordQueue<ByteCapacity, PayloadBytes>>("Mashiro vs Rigtorp SPSC",
                                                                                          "SPSC", 1, 1);
        add.template operator()<Spsc, Packed>("Mashiro vs mutex packed", "SPSC", 1, 1);
        add.template operator()<Spsc, Allocating>("Mashiro vs mutex allocating", "SPSC", 1, 1);

        const auto appendMpsc = [&](std::size_t producers) {
            using Mpsc = MashiroRecordQueue<ByteCapacity, Concurrency::QueueTopologies::Mpsc>;
            add.template operator()<Mpsc, AeronMpscQueue<ByteCapacity>>("Mashiro vs Aeron", "MPSC", producers, 1);
            add.template
            operator()<Mpsc, DpdkClassicRecordQueue<ByteCapacity, PayloadBytes, Concurrency::QueueTopologies::Mpsc>>(
                "Mashiro vs DPDK classic", "MPSC", producers, 1);
            add.template operator()<Mpsc, RigtorpMpmcRecordQueue<ByteCapacity, PayloadBytes>>("Mashiro vs Rigtorp MPMC",
                                                                                              "MPSC", producers, 1);
            add.template operator()<Mpsc, Packed>("Mashiro vs mutex packed", "MPSC", producers, 1);
            add.template operator()<Mpsc, Allocating>("Mashiro vs mutex allocating", "MPSC", producers, 1);
        };
        const auto appendSpmc = [&](std::size_t consumers) {
            using Spmc = MashiroRecordQueue<ByteCapacity, Concurrency::QueueTopologies::Spmc>;
            add.template
            operator()<Spmc, DpdkClassicRecordQueue<ByteCapacity, PayloadBytes, Concurrency::QueueTopologies::Spmc>>(
                "Mashiro vs DPDK classic", "SPMC", 1, consumers);
            add.template operator()<Spmc, RigtorpMpmcRecordQueue<ByteCapacity, PayloadBytes>>("Mashiro vs Rigtorp MPMC",
                                                                                              "SPMC", 1, consumers);
            add.template operator()<Spmc, Packed>("Mashiro vs mutex packed", "SPMC", 1, consumers);
            add.template operator()<Spmc, Allocating>("Mashiro vs mutex allocating", "SPMC", 1, consumers);
        };
        const auto appendMpmc = [&](std::size_t endpoints) {
            using Mpmc = MashiroRecordQueue<ByteCapacity, Concurrency::QueueTopologies::Mpmc>;
            add.template
            operator()<Mpmc, DpdkClassicRecordQueue<ByteCapacity, PayloadBytes, Concurrency::QueueTopologies::Mpmc>>(
                "Mashiro vs DPDK classic", "MPMC", endpoints, endpoints);
            add.template operator()<Mpmc, RigtorpMpmcRecordQueue<ByteCapacity, PayloadBytes>>(
                "Mashiro vs Rigtorp MPMC", "MPMC", endpoints, endpoints);
            add.template operator()<Mpmc, Packed>("Mashiro vs mutex packed", "MPMC", endpoints, endpoints);
            add.template operator()<Mpmc, Allocating>("Mashiro vs mutex allocating", "MPMC", endpoints, endpoints);
        };

        appendMpsc(4);
        appendSpmc(4);
        appendMpmc(4);
        if (scaling) {
            appendMpsc(2);
            appendMpsc(8);
            appendSpmc(2);
            appendSpmc(8);
            appendMpmc(2);
        }
    }

    [[nodiscard]] Config ParseConfig(int argc, char** argv) {
        Config config;
        bool samplesSpecified = false;
        bool warmupsSpecified = false;
        bool recordsSpecified = false;
        bool minimumDurationSpecified = false;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            auto value = [&]() -> std::string_view {
                if (++index == argc) {
                    throw std::invalid_argument("missing value after " + std::string{argument});
                }
                return argv[index];
            };
            if (argument == "--output") {
                config.output = value();
            } else if (argument == "--label") {
                config.label = value();
            } else if (argument == "--samples") {
                config.samples = std::stoull(std::string{value()});
                samplesSpecified = true;
            } else if (argument == "--warmups") {
                config.warmups = std::stoull(std::string{value()});
                warmupsSpecified = true;
            } else if (argument == "--records") {
                config.recordsPerProducer = std::stoull(std::string{value()});
                recordsSpecified = true;
            } else if (argument == "--minimum-ms") {
                config.minimumSampleSeconds = std::stod(std::string{value()}) / 1'000.0;
                minimumDurationSpecified = true;
            } else if (argument == "--seed") {
                config.seed = std::stoull(std::string{value()});
            } else if (argument == "--pair") {
                config.pairFilter = value();
            } else if (argument == "--topology") {
                config.topologyFilter = value();
            } else if (argument == "--quick") {
                config.quick = true;
            } else if (argument == "--migration") {
                config.migration = true;
            } else if (argument == "--logger-migration") {
                config.loggerMigration = true;
            } else if (argument == "--no-pin") {
                config.pinWorkers = false;
            } else if (argument == "--help" || argument == "-h") {
                config.showHelp = true;
            } else {
                throw std::invalid_argument("unknown command-line option: " + std::string{argument});
            }
        }
        if (config.quick) {
            if (!samplesSpecified) {
                config.samples = 3;
            }
            if (!warmupsSpecified) {
                config.warmups = 1;
            }
            if (!recordsSpecified) {
                config.recordsPerProducer = 20'000;
            }
            if (!minimumDurationSpecified) {
                config.minimumSampleSeconds = 0.0;
            }
        }
        if (config.samples == 0 || config.recordsPerProducer == 0 || !std::isfinite(config.minimumSampleSeconds) ||
            config.minimumSampleSeconds < 0.0) {
            throw std::invalid_argument("samples and records must be non-zero and minimum-ms must be non-negative");
        }
        if (config.migration && config.loggerMigration) {
            throw std::invalid_argument("--migration and --logger-migration are mutually exclusive");
        }
        return config;
    }

    void PrintUsage() {
        std::cout << "Usage: Benchmark.Mashiro.RecordRings [options]\n"
                     "  --output PATH       CSV output path\n"
                     "  --label TEXT        Run label stored in the CSV\n"
                     "  --samples N         Paired samples per implementation\n"
                     "  --warmups N         Warmup samples per implementation\n"
                     "  --records N         Initial records per producer\n"
                     "  --minimum-ms N      Minimum calibrated sample duration\n"
                     "  --seed N            Case-order random seed\n"
                     "  --pair TEXT         Run pairs whose label contains TEXT\n"
                     "  --topology NAME     Run only SPSC, MPSC, SPMC, or MPMC\n"
                     "  --migration         Compare Mashiro and Sora with placement controls\n"
                     "  --logger-migration  Compare the removed ThreadRing with Sora RecordRingBuffer\n"
                     "  --quick             Run the compact 64-byte smoke matrix\n"
                     "  --no-pin            Disable Windows CPU Set placement\n"
                     "  --help, -h          Show this help\n";
    }

} // namespace Mashiro::Benchmark

int main(int argc, char** argv) {
    try {
        using namespace Mashiro::Benchmark;
        constexpr std::size_t kByteCapacity = 64 * 1024;
        const Config config = ParseConfig(argc, argv);
        if (config.showHelp) {
            PrintUsage();
            return 0;
        }
        if (!ConfigureBenchmarkProcess(config.pinWorkers)) {
            throw std::runtime_error("failed to configure high-priority record benchmark process");
        }
        std::filesystem::create_directories(config.output.parent_path().empty() ? "." : config.output.parent_path());
        CsvWriter writer{config};
        std::vector<Case> cases;
        if (config.quick && config.loggerMigration) {
            AppendLoggerMigrationCase<kByteCapacity, 64>(config, writer, cases);
        } else if (config.loggerMigration) {
            AppendLoggerMigrationCase<kByteCapacity, 8>(config, writer, cases);
            AppendLoggerMigrationCase<kByteCapacity, 64>(config, writer, cases);
            AppendLoggerMigrationCase<kByteCapacity, 256>(config, writer, cases);
            AppendLoggerMigrationCase<kByteCapacity, 1024>(config, writer, cases);
            AppendLoggerMigrationCase<kByteCapacity, 4096>(config, writer, cases);
        } else if (config.quick && config.migration) {
            AppendMigrationPayloadCases<kByteCapacity, 64>(config, writer, cases, true);
        } else if (config.migration) {
            AppendMigrationPayloadCases<kByteCapacity, 8>(config, writer, cases, false);
            AppendMigrationPayloadCases<kByteCapacity, 64>(config, writer, cases, true);
            AppendMigrationPayloadCases<kByteCapacity, 256>(config, writer, cases, false);
            AppendMigrationPayloadCases<kByteCapacity, 1024>(config, writer, cases, false);
            AppendMigrationPayloadCases<kByteCapacity, 4096>(config, writer, cases, false);
        } else if (config.quick) {
            AppendPayloadCases<kByteCapacity, 64>(config, writer, cases, true);
        } else {
            AppendPayloadCases<kByteCapacity, 8>(config, writer, cases, false);
            AppendPayloadCases<kByteCapacity, 64>(config, writer, cases, true);
            AppendPayloadCases<kByteCapacity, 256>(config, writer, cases, false);
            AppendPayloadCases<kByteCapacity, 1024>(config, writer, cases, false);
            AppendPayloadCases<kByteCapacity, 4096>(config, writer, cases, false);
        }
        if (cases.empty()) {
            throw std::invalid_argument("benchmark filters selected no implementation pairs");
        }
        std::mt19937_64 random{config.seed};
        std::ranges::shuffle(cases, random);
        for (Case& benchmark : cases) {
            benchmark.run();
        }
        std::cout << "wrote " << config.output.string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "record benchmark failed: " << error.what() << '\n';
        return 1;
    }
}

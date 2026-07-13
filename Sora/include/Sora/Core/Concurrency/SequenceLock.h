/**
 * @file SequenceLock.h
 * @brief Provide a sequence lock for single-writer, multi-reader publication of a trivially copyable value.
 * @details A sequence lock surrounds each payload update with an odd/even counter protocol. An even counter denotes a
 * quiescent writer, while an odd counter denotes an update in progress. A reader accepts its snapshot only when both
 * counter observations are equal and even. Exactly one thread may call @ref SequenceLock::Write or
 * @ref SequenceLock::Update; concurrent writers need external serialization. Readers do not block the writer and retry
 * whenever their snapshots overlap a write.
 *
 * The intended writer ordering is:
 * @code{.cpp}
 * seq_.store(seq + 1, std::memory_order_relaxed);
 * std::atomic_thread_fence(std::memory_order_release);
 * StorePayload(value);
 * seq_.store(seq + 2, std::memory_order_release);
 * @endcode
 *
 * The intended reader ordering is:
 * @code{.cpp}
 * s0 = seq_.load(std::memory_order_acquire);
 * snapshot = LoadPayload();
 * std::atomic_thread_fence(std::memory_order_acquire);
 * s1 = seq_.load(std::memory_order_relaxed);
 * @endcode
 *
 * The payload is represented as independently atomic 64-bit words, so snapshots that overlap writes remain valid atomic
 * accesses under the C++ memory model and are discarded when sequence validation fails. Both members belong to the
 * @ref Sora::Concurrency::$::ProducerOwned contention domain because only the writer mutates them. Representative
 * instantiations are audited at translation time through the project-wide false-sharing analyzer.
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/Concurrency/FalseSharingChecker.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <meta>
#include <optional>
#include <type_traits>
#include <utility>

namespace Sora::Concurrency {

    /**
     * @brief Publish a trivially copyable value through a single-writer, multi-reader sequence protocol.
     * @tparam T Unqualified, non-array, trivially copyable payload type.
     * @code{.cpp}
     * SequenceLock<std::uint64_t> clock;
     * clock.Write(42);
     * std::uint64_t now = clock.Read();
     * @endcode
     */
    template<typename T>
        requires std::is_trivially_copyable_v<T> && std::same_as<T, std::remove_cv_t<T>> && (!std::is_array_v<T>)
    class SequenceLock {
    public:
        using ValueType = T;                /**< Published payload type. */
        using SequenceType = std::uint64_t; /**< Free-running odd/even sequence counter type. */

        /** @name Construction @{ ----------------------------------------------------------------------------------- */

        /** @brief Construct a sequence lock with a value-initialized payload. */
        SequenceLock() noexcept(std::is_nothrow_default_constructible_v<T>)
            requires std::default_initializable<T>
        {
            StorePayload(T{});
        }

        /**
         * @brief Construct a sequence lock with an initial published value.
         * @param[in] initial Initial payload value.
         */
        explicit SequenceLock(const T& initial) noexcept { StorePayload(initial); }

        SequenceLock(const SequenceLock&) = delete;
        SequenceLock& operator=(const SequenceLock&) = delete;
        SequenceLock(SequenceLock&&) = delete;
        SequenceLock& operator=(SequenceLock&&) = delete;

        /** @} ------------------------------------------------------------------------------------------------------ */

        /** @name Writer @{ ----------------------------------------------------------------------------------------- */

        /**
         * @brief Publish @p value with respect to readers.
         * @details This operation has constant complexity and may only be called by the designated writer thread.
         * @param[in] value Payload value to publish.
         */
        void Write(const T& value) noexcept {
            const SequenceType seq = seq_.load(std::memory_order_relaxed);
            seq_.store(seq + 1, std::memory_order_relaxed);      // begin: odd
            std::atomic_thread_fence(std::memory_order_release); // (W1) order odd-store before payload store
            StorePayload(value);                                 // publish payload through atomic words
            seq_.store(seq + 2, std::memory_order_release);      // (W2) end: even; payload ordered before counter
        }

        /**
         * @brief Apply @p fn to a private payload copy and publish the resulting value.
         * @details If @p fn throws, the current published value remains unchanged. This operation may only be called by
         * the designated writer thread.
         * @tparam F Callable type invocable with @p T as a mutable lvalue.
         * @param[in] fn Mutation applied to the private payload copy.
         */
        template<typename F>
            requires std::invocable<F&, T&>
        void Update(F&& fn) noexcept(std::is_nothrow_invocable_v<F&, T&>) {
            T scratch = Read();
            std::forward<F>(fn)(scratch);
            Write(scratch);
        }

        /** @} ------------------------------------------------------------------------------------------------------ */

        /** @name Reader @{ ----------------------------------------------------------------------------------------- */

        /**
         * @brief Return a snapshot whose surrounding sequence observations are equal and even.
         * @return Consistent payload snapshot according to the sequence protocol.
         */
        [[nodiscard]] T Read() const noexcept {
            while (true) {
                const SequenceType s0 = seq_.load(std::memory_order_acquire);
                if (s0 & SequenceType{1}) [[unlikely]] {
                    continue;
                }
                const ByteArray snapshot = LoadPayload();
                std::atomic_thread_fence(std::memory_order_acquire); // (R2) snapshot ordered before s1
                const SequenceType s1 = seq_.load(std::memory_order_relaxed);
                if (s0 == s1) [[likely]] {
                    return std::bit_cast<T>(snapshot);
                }
            }
        }

        /**
         * @brief Attempt one snapshot without retrying when a write overlaps the read.
         * @return Snapshot when both sequence observations are equal and even; otherwise an empty optional.
         */
        [[nodiscard]] std::optional<T> TryRead() const noexcept {
            const SequenceType s0 = seq_.load(std::memory_order_acquire);
            if (s0 & SequenceType{1}) {
                return std::nullopt;
            }
            const ByteArray snapshot = LoadPayload();
            std::atomic_thread_fence(std::memory_order_acquire);
            const SequenceType s1 = seq_.load(std::memory_order_relaxed);
            if (s0 != s1) {
                return std::nullopt;
            }
            return std::bit_cast<T>(snapshot);
        }

        /** @} ------------------------------------------------------------------------------------------------------ */

        /** @name Diagnostics @{ ------------------------------------------------------------------------------------ */

        /** @brief Return the current sequence value, which is odd while a write is in progress. */
        [[nodiscard]] SequenceType Sequence() const noexcept { return seq_.load(std::memory_order_acquire); }

        /** @brief Return whether the observed sequence value indicates a write in progress. */
        [[nodiscard]] bool WriteInProgress() const noexcept { return Sequence() & SequenceType{1}; }

        /** @brief Return the translation-time physical-layout report for this specialization. */
        [[nodiscard]] static consteval Meta::CacheLayoutReport Layout() {
            return Concurrency::Meta::AnalyzeCacheLayout<SequenceLock>();
        }

        /** @} ------------------------------------------------------------------------------------------------------ */

    private:
        using StorageWord = std::uint64_t;
        using ByteArray = std::array<std::byte, sizeof(T)>;

        static constexpr size_t kStorageWordSize = sizeof(StorageWord);
        static constexpr size_t kStorageWordCount = (sizeof(T) + kStorageWordSize - 1) / kStorageWordSize;

        void StorePayload(const T& value) noexcept {
            ByteArray bytes;
            std::memcpy(bytes.data(), std::addressof(value), sizeof(T));
            for (size_t index = 0; index < kStorageWordCount; ++index) {
                const size_t offset = index * kStorageWordSize;
                const size_t count = std::min(kStorageWordSize, sizeof(T) - offset);
                StorageWord word = 0;
                std::memcpy(std::addressof(word), bytes.data() + offset, count);
                payload_[index].store(word, std::memory_order_relaxed);
            }
        }

        [[nodiscard]] ByteArray LoadPayload() const noexcept {
            ByteArray bytes;
            for (size_t index = 0; index < kStorageWordCount; ++index) {
                const size_t offset = index * kStorageWordSize;
                const size_t count = std::min(kStorageWordSize, sizeof(T) - offset);
                const StorageWord word = payload_[index].load(std::memory_order_relaxed);
                std::memcpy(bytes.data() + offset, std::addressof(word), count);
            }
            return bytes;
        }

        [[= $::ProducerOwned{}]] alignas(Platform::kCacheLineSize) std::atomic<SequenceType> seq_{0};
        [[= $::ProducerOwned{}]] std::array<std::atomic<StorageWord>, kStorageWordCount> payload_{};

        static_assert(std::atomic<SequenceType>::is_always_lock_free,
                      "SequenceLock requires a lock-free sequence counter");
        static_assert(std::atomic<StorageWord>::is_always_lock_free,
                      "SequenceLock requires lock-free atomic payload words");
    };

    /** @cond INTERNAL */
    consteval {

        using Concurrency::$::ProducerOwned;
        using Concurrency::Meta::AuditFalseSharing;
        using Concurrency::Traits::DomainLineSpanOf;
        using Concurrency::Traits::IsDomainStartsAlignedToCacheLine;
        using Concurrency::Traits::IsOccupyingWholeLines;

        // The counter and payload share one writer domain, so their co-location does not create write-write contention.
        static_assert(AuditFalseSharing<SequenceLock<std::uint64_t>>(),
                      "SequenceLock<uint64_t> failed the internal false-sharing audit");
        static_assert(AuditFalseSharing<SequenceLock<double>>(),
                      "SequenceLock<double> failed the internal false-sharing audit");
        // Whole-line occupancy prevents neighboring array elements from sharing a boundary cache line.
        static_assert(IsOccupyingWholeLines<SequenceLock<std::uint64_t>>,
                      "SequenceLock<uint64_t> must occupy whole cache lines");
        // The writer-owned state begins at a cache-line boundary.
        static_assert(IsDomainStartsAlignedToCacheLine<SequenceLock<std::uint64_t>, ProducerOwned>,
                      "SequenceLock counter must start a cache line");
        // A small payload shares the counter's cache line, allowing the reader fast path to touch one line.
        static_assert(Traits::DomainLineSpanOf<SequenceLock<std::uint64_t>, ProducerOwned> == 1,
                      "small SequenceLock payloads must share the counter's cache line");
    }
    /** @endcond */

} // namespace Sora::Concurrency

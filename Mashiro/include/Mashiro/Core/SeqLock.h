/**
 * @file SeqLock.h
 * @brief Sequence lock (seqlock) for single-writer / multi-reader publication of a
 *        trivially-copyable value with wait-free, lock-free reads.
 *
 * A seqlock publishes an entire value atomically *with respect to readers* by
 * wrapping each write in an odd/even sequence counter:
 * - `seq_` **even**  → no writer active; a snapshot taken between two equal even
 *   observations is internally consistent.
 * - `seq_` **odd**   → a writer is mid-update; readers discard the snapshot and retry.
 *
 * ### Concurrency contract
 * - **One writer thread** (`Write` / `Update`). Concurrent writers corrupt the
 *   protocol; serialise them externally if you need multiple producers.
 * - **Any number of reader threads** (`Read` / `TryRead`). Readers never block the
 *   writer and never write shared state, so reads are *wait-free for the writer*
 *   and lock-free for readers (a read retries only while it races a write).
 *
 * ### Memory ordering (correct on weakly-ordered ISAs, e.g. AArch64)
 * Writer:
 * @code
 *   seq_.store(seq + 1, relaxed);                 // begin: odd
 *   atomic_thread_fence(release);                 // (W1) odd-store ordered BEFORE data store
 *   data_ = value;                                // publish payload
 *   seq_.store(seq + 2, release);                 // (W2) end: even; payload ordered BEFORE counter
 * @endcode
 * Reader:
 * @code
 *   s0 = seq_.load(acquire);                      // (R1) if even, syncs-with the writer's (W2)
 *   memcpy(snapshot, &data_, sizeof(T));          // take a private byte snapshot
 *   atomic_thread_fence(acquire);                 // (R2) snapshot load ordered BEFORE s1 load
 *   s1 = seq_.load(relaxed);                      // re-check
 *   // accept iff (s0 is even) && (s0 == s1)
 * @endcode
 * `(W1)` guarantees the "write in progress" marker is visible no later than the
 * payload mutation, so a racing reader is *guaranteed to see an odd counter*.
 * `(R2)` keeps the payload snapshot strictly between the two counter observations,
 * so an unchanged even counter proves the snapshot was not concurrently modified.
 *
 * @note The payload is intentionally a plain (non-atomic) `T`: the snapshot is
 *       taken with `std::memcpy` and validated by the counter, which is the
 *       canonical high-performance seqlock design (cf. Rigtorp/Folly). The
 *       transient read that races an in-progress write is *discarded* before it is
 *       ever observed, so no torn value is exposed. This mirrors the deliberate
 *       non-atomic shared storage used by @ref SpscRingBuffer.h.
 *
 * ### Compile-time guarantees (C++26 reflection)
 * Both data members are written only by the single writer thread, so each is tagged
 * with the project-wide @ref Concurrency::ProducerOwned contention domain (see
 * @ref FalseSharing.h). Sharing one cache line is therefore legitimate — indeed
 * desirable, since a reader's fast path then touches a single line. The `consteval`
 * block at the end of this header proves, via P2996 static reflection, that the
 * layout is free of internal false sharing (@ref Concurrency::AuditFalseSharing),
 * occupies whole cache lines (no *external* false sharing,
 * @ref Concurrency::OccupiesWholeLines), starts a line
 * (@ref Concurrency::DomainStartsLine), and spans exactly one line
 * (@ref Concurrency::DomainLineSpan).
 *
 * @ingroup Core
 */
#pragma once

#include "Mashiro/Core/FalseSharing.h"

#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <meta>
#include <type_traits>
#include <utility>

namespace Mashiro {

    // =========================================================================
    // SeqLock<T>
    // =========================================================================

    /**
     * @brief Single-writer / multi-reader sequence lock over a trivially-copyable @p T.
     *
     * @tparam T Trivially-copyable payload type (the value being published).
     *
     * @code
     * SeqLock<std::uint64_t> clock;
     * clock.Write(42);                 // writer thread
     * std::uint64_t now = clock.Read(); // reader thread(s): always a consistent value
     * @endcode
     */
    template<typename T>
        requires std::is_trivially_copyable_v<T>
    class SeqLock {
    public:
        using value_type = T;                  ///< Published payload type.
        using SequenceType = std::uint32_t;    ///< Free-running odd/even sequence counter type.

        /// @name Construction
        /// @{

        /// @brief Default-construct with a value-initialised payload.
        SeqLock() = default;

        /// @brief Construct with an initial published value.
        explicit SeqLock(const T& initial) noexcept : data_(initial) {}

        SeqLock(const SeqLock&) = delete;
        SeqLock& operator=(const SeqLock&) = delete;
        SeqLock(SeqLock&&) = delete;
        SeqLock& operator=(SeqLock&&) = delete;
        /// @}

        /// @name Writer (single thread)
        /// @{

        /// @brief Publish @p value atomically with respect to readers.
        ///
        /// Accepts both lvalues and rvalues (a copy of a trivially-copyable type is
        /// equivalent to a move). Wait-free for the writer.
        void Write(const T& value) noexcept {
            const SequenceType seq = seq_.load(std::memory_order_relaxed);
            seq_.store(seq + 1, std::memory_order_relaxed);      // begin: odd
            std::atomic_thread_fence(std::memory_order_release); // (W1) order odd-store before payload store
            data_ = value;                                       // publish payload
            seq_.store(seq + 2, std::memory_order_release);      // (W2) end: even; payload ordered before counter
        }

        /// @brief Read-modify-write the published value with @p fn (single writer).
        ///
        /// The current value is snapshotted, mutated by `fn(T&)`, then published via
        /// @ref Write. Strongly exception-safe: if @p fn throws, no change is published.
        ///
        /// @tparam F Invocable as `void(T&)`.
        template<typename F>
            requires std::invocable<F&, T&>
        void Update(F&& fn) noexcept(std::is_nothrow_invocable_v<F&, T&> &&
                                     std::is_nothrow_copy_constructible_v<T>) {
            // Single-writer invariant: only this thread mutates data_, so reading the
            // current value without the seq protocol is race-free for the writer.
            T scratch = data_;
            std::forward<F>(fn)(scratch);
            Write(scratch);
        }
        /// @}

        /// @name Reader (any thread)
        /// @{

        /// @brief Return a consistent snapshot of the published value.
        ///
        /// Retries while it races a concurrent write. Lock-free; never exposes a
        /// torn value.
        [[nodiscard]] T Read() const noexcept {
            alignas(T) std::byte snapshot[sizeof(T)];
            SequenceType s0;
            SequenceType s1;
            do {
                s0 = seq_.load(std::memory_order_acquire);            // (R1)
                std::memcpy(snapshot, std::addressof(data_), sizeof(T));
                std::atomic_thread_fence(std::memory_order_acquire); // (R2) snapshot ordered before s1
                s1 = seq_.load(std::memory_order_relaxed);
            } while ((s0 & SequenceType{1}) || s0 != s1);
            return std::bit_cast<T>(snapshot);
        }

        /// @brief Single-shot read; fails (returns false) if it observes a concurrent write.
        ///
        /// Wait-free (no retry loop). @p out is written only on success.
        [[nodiscard]] bool TryRead(T& out) const noexcept {
            alignas(T) std::byte snapshot[sizeof(T)];
            const SequenceType s0 = seq_.load(std::memory_order_acquire);
            if (s0 & SequenceType{1}) {
                return false; // write in progress
            }
            std::memcpy(snapshot, std::addressof(data_), sizeof(T));
            std::atomic_thread_fence(std::memory_order_acquire);
            const SequenceType s1 = seq_.load(std::memory_order_relaxed);
            if (s0 != s1) {
                return false; // a write happened during the snapshot
            }
            out = std::bit_cast<T>(snapshot);
            return true;
        }
        /// @}

        /// @name Diagnostics (callable from any thread)
        /// @{

        /// @brief Current sequence value (odd ⇒ a write is in progress).
        [[nodiscard]] SequenceType Sequence() const noexcept {
            return seq_.load(std::memory_order_acquire);
        }

        /// @brief True if a write is currently in progress (racy snapshot).
        [[nodiscard]] bool WriteInProgress() const noexcept {
            return Sequence() & SequenceType{1};
        }

        /// @brief Compile-time physical-layout report for this specialisation.
        [[nodiscard]] static consteval Concurrency::CacheLayoutReport Layout() {
            return Concurrency::AnalyzeCacheLayout<SeqLock>();
        }
        /// @}

    private:
        [[=Concurrency::ProducerOwned{}]] alignas(Platform::kCacheLineSize)
        std::atomic<SequenceType> seq_{0};
        [[=Concurrency::ProducerOwned{}]] T data_{};

        static_assert(std::atomic<SequenceType>::is_always_lock_free,
                      "SeqLock requires a lock-free sequence counter");
    };

    // =========================================================================
    // Compile-time layout audit (representative instantiations)
    // =========================================================================

    /** @cond INTERNAL */
    consteval {
        using Concurrency::AuditFalseSharing;
        using Concurrency::OccupiesWholeLines;
        using Concurrency::DomainStartsLine;
        using Concurrency::DomainLineSpan;
        using Concurrency::ProducerOwned;

        // Internal false sharing: the counter and payload share the single writer's
        // domain, so co-locating them on one line is provably conflict-free.
        static_assert(AuditFalseSharing<SeqLock<std::uint64_t>>(),
                      "SeqLock<uint64_t> failed the internal false-sharing audit");
        static_assert(AuditFalseSharing<SeqLock<double>>(),
                      "SeqLock<double> failed the internal false-sharing audit");
        // External false sharing: whole-line occupancy keeps array neighbours apart.
        static_assert(OccupiesWholeLines<SeqLock<std::uint64_t>>(),
                      "SeqLock<uint64_t> must occupy whole cache lines");
        // The writer's state begins a cache line...
        static_assert(DomainStartsLine<SeqLock<std::uint64_t>, ProducerOwned>(),
                      "SeqLock counter must start a cache line");
        // ...and a small payload shares that one line, so a reader touches a single line.
        static_assert(DomainLineSpan<SeqLock<std::uint64_t>, ProducerOwned>() == 1,
                      "small SeqLock payloads must share the counter's cache line");
    }
    /** @endcond */

} // namespace Mashiro

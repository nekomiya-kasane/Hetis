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
 * Members are tagged with @ref Concurrency::SeqCounter / @ref Concurrency::SeqPayload
 * annotations. @ref Concurrency::AuditSeqLockLayout proves — entirely at compile
 * time via P2996 static reflection — that the counter starts a cache line, the
 * object occupies whole cache lines (no *external* false sharing), and that exactly
 * one counter/payload pair exists. The audit runs in the `consteval` block at the
 * end of this header for representative instantiations.
 *
 * @ingroup Core
 */
#pragma once

#include "TypeTraits.h"

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
    // Concurrency role annotations + reflection-driven SeqLock layout audit
    // (reopens the Concurrency namespace introduced in SpscRingBuffer.h)
    // =========================================================================

    /**
     * @brief Thread-ownership annotations and compile-time layout audits.
     *
     * The seqlock additions below tag the two structural members of a `SeqLock`
     * so that @ref AuditSeqLockLayout can verify the physical layout with C++26
     * static reflection.
     */
    namespace Concurrency {

        struct SeqCounter {}; ///< Annotation: the atomic sequence counter of a `SeqLock`.
        struct SeqPayload {}; ///< Annotation: the protected snapshot payload of a `SeqLock`.

        /**
         * @brief Compile-time description of a `SeqLock`'s physical memory layout.
         *
         * Produced by @ref AnalyzeSeqLockLayout; exposed via `SeqLock::Layout()` so
         * callers can assert layout properties (e.g. read locality) at compile time.
         */
        struct SeqLockLayoutReport {
            size_t cacheLine = 0;       ///< Cache-line granularity used for the audit.
            size_t counterOffset = 0;   ///< Byte offset of the sequence counter.
            size_t counterSize = 0;     ///< Size of the sequence counter.
            size_t payloadOffset = 0;   ///< Byte offset of the payload.
            size_t payloadSize = 0;     ///< Size of the payload.
            size_t readCacheLines = 0;  ///< Distinct cache lines a reader's fast path touches.
            bool hasCounter = false;    ///< A `SeqCounter`-tagged member was found.
            bool hasPayload = false;    ///< A `SeqPayload`-tagged member was found.
            bool singleCounter = true;  ///< Exactly one `SeqCounter` member.
            bool singlePayload = true;  ///< Exactly one `SeqPayload` member.
            bool everyMemberTagged = true; ///< Every NSDM carries exactly one role tag.
            bool counterStartsLine = false; ///< The counter begins a cache line.
            bool payloadCoLocated = false;  ///< Counter and payload share a cache line.
            bool wholeLineAligned = false;  ///< The object is aligned to whole cache lines.
            bool valid = false;             ///< All structural invariants hold.
        };

        /**
         * @brief Reflect over @p L's non-static data members and compute its layout report.
         * @tparam L A `SeqLock` specialisation (any reflectable, complete class works).
         */
        template<typename L>
        consteval SeqLockLayoutReport AnalyzeSeqLockLayout() {
            SeqLockLayoutReport r{};
            const size_t line = Platform::kCacheLineSize;
            r.cacheLine = line;
            for (auto member : Traits::Members<L>) {
                const bool counter = Traits::Anno::Has<SeqCounter>(member);
                const bool payload = Traits::Anno::Has<SeqPayload>(member);
                if (int{counter} + int{payload} != 1) {
                    r.everyMemberTagged = false; // member with zero or both role tags
                    continue;
                }
                const size_t offset = static_cast<size_t>(std::meta::offset_of(member).bytes);
                const size_t size = std::meta::size_of(std::meta::type_of(member));
                if (counter) {
                    if (r.hasCounter) {
                        r.singleCounter = false;
                    }
                    r.hasCounter = true;
                    r.counterOffset = offset;
                    r.counterSize = size;
                } else {
                    if (r.hasPayload) {
                        r.singlePayload = false;
                    }
                    r.hasPayload = true;
                    r.payloadOffset = offset;
                    r.payloadSize = size;
                }
            }

            r.wholeLineAligned = (alignof(L) >= line) && (alignof(L) % line == 0);
            r.counterStartsLine = r.hasCounter && (r.counterOffset % line == 0);
            r.payloadCoLocated = r.hasCounter && r.hasPayload &&
                                 (r.counterOffset / line == r.payloadOffset / line);
            if (r.hasCounter && r.hasPayload) {
                const size_t firstLine = r.counterOffset / line;
                const size_t lastByte = r.payloadOffset + (r.payloadSize ? r.payloadSize - 1 : 0);
                const size_t lastLine = lastByte / line;
                r.readCacheLines = lastLine >= firstLine ? lastLine - firstLine + 1 : 1;
            }
            r.valid = r.everyMemberTagged && r.hasCounter && r.hasPayload && r.singleCounter &&
                      r.singlePayload && r.wholeLineAligned && r.counterStartsLine;
            return r;
        }

        /**
         * @brief `consteval` predicate: @p L is a well-formed, cache-friendly seqlock.
         * @return true iff exactly one counter/payload pair exists, the counter starts
         *         a cache line, and the object is aligned to whole cache lines.
         */
        template<typename L>
        consteval bool AuditSeqLockLayout() {
            return AnalyzeSeqLockLayout<L>().valid;
        }

    } // namespace Concurrency

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
        [[nodiscard]] static consteval Concurrency::SeqLockLayoutReport Layout() {
            return Concurrency::AnalyzeSeqLockLayout<SeqLock>();
        }
        /// @}

    private:
        [[=Concurrency::SeqCounter{}]] alignas(Platform::kCacheLineSize)
        std::atomic<SequenceType> seq_{0};
        [[=Concurrency::SeqPayload{}]] T data_{};

        static_assert(std::atomic<SequenceType>::is_always_lock_free,
                      "SeqLock requires a lock-free sequence counter");
    };

    // =========================================================================
    // Compile-time layout audit (representative instantiations)
    // =========================================================================

    /** @cond INTERNAL */
    consteval {
        static_assert(Concurrency::AuditSeqLockLayout<SeqLock<std::uint64_t>>(),
                      "SeqLock<uint64_t> failed the compile-time layout audit");
        static_assert(Concurrency::AuditSeqLockLayout<SeqLock<double>>(),
                      "SeqLock<double> failed the compile-time layout audit");
        static_assert(SeqLock<std::uint64_t>::Layout().readCacheLines == 1,
                      "small SeqLock payloads must share the counter's cache line");
    }
    /** @endcond */

} // namespace Mashiro

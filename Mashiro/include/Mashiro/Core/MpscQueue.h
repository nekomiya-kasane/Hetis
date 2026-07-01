/**
 * @file MpscQueue.h
 * @brief Bounded, lock-free MPSC (multi-producer, single-consumer) FIFO queue.
 *
 * @ref MpscQueue is the project's canonical many-to-one mailbox. It satisfies the two MPSC needs
 * called out by the Platform-thread infrastructure design (`docs/superpowers/specs/2026-06-11-...`):
 * the @c EventPump external-event inbox (`§6.8`, `§8.3`) and any future MPSC-shaped channel where a
 * dedicated consumer thread drains submissions from N producers (worker -> Platform thread, IO
 * threads -> renderer, etc.). The bounded ring layout keeps the queue allocation-free on the hot
 * path and trivially destructible at teardown.
 *
 * @par Why bounded-array Vyukov, not pool-backed nodes
 * @ref Mashiro::ConcurrentObjectPool is the project's ABA-safe, generation-checked, MPMC-friendly
 * slot allocator. Building MPSC on top of it (pool-backed Vyukov linked queue: each node drawn
 * from the pool, linked by a @c next slot index) is a valid design and is the right tool for any
 * MPSC whose payloads are *objects with stable node identity that survive past dequeue* — the
 * canonical example is a wait-list where one CAS detaches a parked waiter and the consumer keeps
 * a pointer to that node alive across multiple state transitions.
 *
 * No mailbox in this project meets that bar. Specifically:
 *  - The @c EventPump external inbox (spec §6.8) drains @c SystemEvent values by move; the cell
 *    payload is destructively read and ceases to exist after @c TryPop. There is no node, so
 *    "node identity" is undefined.
 *  - The @c OwnerMailbox (the recast-of-v1.4 @c OwnerExecutor handle queue, spec §6.5) drains
 *    @c std::coroutine_handle<> values — opaque pointers that the consumer reads exactly once
 *    and resumes; there is no slot, no generation, and the producer never inspects a previously-
 *    enqueued handle. The decision to back this mailbox with @ref MpscQueue (not the pool) was
 *    re-confirmed off-spec: the pool's two guarantees decompose to *no observable benefit* for
 *    this workload, and bounded-array Vyukov is strictly cheaper on every axis (no free-list CAS,
 *    no per-slot generation, no allocation, deterministic capacity).
 *
 * For both of these — and for any future MPSC of moved-in payload values — the pool's two
 * guarantees decompose:
 *  - **ABA safety** is unnecessary: bounded-array Vyukov uses a per-cell monotonic sequence
 *    counter, never a pointer, and the @c seq @c == @c pos+1 / @c seq @c == @c pos+Capacity
 *    handshake makes ABA structurally impossible. A pool-backed implementation would add a
 *    hot-path free-list CAS for no correctness benefit.
 *  - **Generation guard / use-after-free** is moot when the consumer drains by value (or by
 *    opaque pointer that is consumed at the call site) and the payload no longer exists after
 *    @c TryPop.
 *
 * What @em is shared with the pool — the project-wide cache-line domain audit
 * (@c Concurrency::Contended / @c ConsumerOwned / @c SharedStorage tags + @c AuditFalseSharing)
 * — is exposed independently in @ref Mashiro/Core/FalseSharing.h and applied here in full. So
 * "high architecture reuse" lives at the layout-audit level; "node lifetime reuse" is the pool's
 * job, kept for the workloads that actually need it.
 *
 * @par Algorithm — Vyukov MPMC (consumer-restricted to 1)
 * Each cell carries an `atomic<size_t> seq` plus an aligned storage region for `T`. A cell with
 * `seq == k` is ready for the producer that holds ticket `k`; after a producer commits, it bumps
 * `seq` to `k+1`, which is the signal the consumer waits for. After the consumer drains the cell,
 * it stamps `seq = k + Capacity`, releasing the cell to the producer that will hold ticket
 * `k + Capacity`. Producers compete on a single contended `tail_` ticket dispenser via CAS;
 * the consumer alone advances `head_`. There is no pointer chasing, no allocation, and no
 * unbounded helper-state propagation — the only contention is between producers on the tail CAS,
 * and that contention is bounded by the cell-sequence handshake.
 *
 * @par Cost model
 * - `TryEmplace` / `TryPush` (any thread): one acquire load + zero or more CAS retries on `tail_`
 *   + one release store on the cell's `seq`. No allocation, no kernel call. Returns `false` on
 *   ring-full without retrying, so producer-side back-pressure is observable rather than blocking.
 * - `TryPop` (consumer thread only): one acquire load on the cell's `seq` + one move out + one
 *   release store on the cell's `seq` for cell return. No CAS at all on the consumer.
 * - Steady-state false sharing: zero by construction. `tail_` (contended), `head_` (consumer-only),
 *   and the cell array (shared storage at disjoint offsets) each occupy independent cache lines;
 *   the project-wide `AuditFalseSharing` proves it at compile time for representative
 *   instantiations.
 *
 * @par Compile-time guarantees
 * Capacity must be a positive power of two, validated at instantiation through a `requires` clause
 * with rich `consteval` diagnostics. Element type `T` must satisfy @ref Traits::Mpscable —
 * movable, `noexcept` move-constructible, `noexcept` destructible — mirroring the contract that
 * @ref Traits::Poolable already imposes for the same reasons on the lock-free fast path. The
 * project-wide false-sharing audit runs in a `consteval` block at the bottom of the header for
 * representative specialisations.
 *
 * @ingroup Core
 */
#pragma once

#include "Mashiro/Core/FalseSharing.h"
#include "Mashiro/Core/Annotation.h"
#include "Mashiro/Core/TypeTraits.h"

#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace Mashiro {

    namespace Traits {

        /**
         * @brief Element requirements for @ref MpscQueue.
         *
         * MPSC payloads ride producer/consumer hot paths where exceptions cannot be unwound without
         * leaking a cell ticket; the noexcept clauses make that impossibility a type-system fact.
         * @c std::movable subsumes destructibility and the move operations; the explicit noexcept
         * checks are kept for clarity and for symmetry with @ref Traits::Poolable.
         */
        template<typename T>
        concept Mpscable = std::movable<T>                              //
                        && std::is_nothrow_move_constructible_v<T>      //
                        && std::is_nothrow_destructible_v<T>;

    } // namespace Traits

    namespace Detail {

        /// @brief One Vyukov cell: a sequence counter plus opaque storage for the payload.
        ///
        /// Storage is a raw byte buffer placement-`new`-constructed by the producer and
        /// `std::destroy_at`-ed by the consumer; this avoids requiring `T` to be default-
        /// constructible and lets the move-out leave the cell in a logically empty state until
        /// the consumer stamps the next sequence value.
        template<typename T>
        struct alignas(Platform::kCacheLineSize) MpscCell {
            std::atomic<size_t> seq{0};
            alignas(T) std::byte storage[sizeof(T)]{};

            [[nodiscard]] T* ValuePtr() noexcept {
                return std::launder(reinterpret_cast<T*>(&storage[0]));
            }
        };

        /// @brief Reject zero, non-power-of-two, and overflow-prone capacities at compile time.
        consteval bool ValidateMpscCapacity(size_t capacity) {
            return capacity >= 1 && std::has_single_bit(capacity)
                && capacity <= (size_t{1} << 32);  // generous upper bound; ticket counter is 64-bit.
        }

    } // namespace Detail

    /**
     * @brief Lock-free, fixed-capacity MPSC queue of `T` with a Vyukov cell layout.
     *
     * @tparam T        Payload type. Must satisfy @ref Traits::Mpscable.
     * @tparam Capacity Number of cells. Must be a positive power of two.
     *
     * @par Concurrency contract
     * Any number of producers may concurrently call @ref TryEmplace / @ref TryPush. Exactly one
     * consumer thread may call @ref TryPop / @ref Drain. Producers and the consumer may run
     * simultaneously; no other consumer may join without external synchronisation. @ref Empty and
     * @ref ApproxSize are wait-free observation points safe to call from any thread (they are
     * monotonic snapshots of the head/tail counters; their relationship to "right now" is the
     * usual lock-free hazard).
     *
     * @par Why a single consumer
     * The consumer drains cells sequentially without any CAS — it advances `head_` on its own.
     * Adding a second consumer would require a `head_` CAS plus the well-known monotonic-sequence
     * dance for stale-pop avoidance, turning this into MPMC with measurably higher consumer cost.
     * The Platform-thread infrastructure design has exactly one consumer per inbox, so this header
     * stays single-consumer; for MPMC, use a different primitive.
     *
     * @code
     * Mashiro::MpscQueue<SystemEvent, 256> inbox;
     * // ... producers (any thread)
     * if (!inbox.TryPush(std::move(ev))) { logger.Drop(); }
     * // ... consumer (single thread)
     * inbox.Drain([&](SystemEvent&& ev) { Bookkeep(ev); Broadcast(std::move(ev)); });
     * @endcode
     */
    template<typename T, size_t Capacity = 1024> 
        requires(Traits::Mpscable<T> && Detail::ValidateMpscCapacity(Capacity))
    class MpscQueue {
    public:
        using value_type = T;                          ///< Payload type.
        static constexpr size_t kCapacity = Capacity;  ///< Compile-time capacity.

        /// @brief Build an empty queue with each cell primed for ticket `i`.
        MpscQueue() noexcept {
            for (size_t i = 0; i < Capacity; ++i) {
                cells_[i].seq.store(i, std::memory_order_relaxed);
            }
            tail_.store(0, std::memory_order_relaxed);
            head_.store(0, std::memory_order_relaxed);
        }

        MpscQueue(const MpscQueue&)            = delete;
        MpscQueue& operator=(const MpscQueue&) = delete;
        MpscQueue(MpscQueue&&)                 = delete;
        MpscQueue& operator=(MpscQueue&&)      = delete;

        /// @brief Destroy any still-pending payloads (single-threaded teardown).
        ~MpscQueue() {
            if constexpr (!std::is_trivially_destructible_v<T>) {
                while (TryPop()) { /* drains and destroys remaining elements */ }
            }
        }

        /// @name Producer-side API (any thread)
        /// @{

        /**
         * @brief Construct a `T` in the next available cell, returning @c false on full ring.
         *
         * Lock-free; bounded retries under producer contention (only retries the tail CAS, never
         * spins on the cell). On full-ring, returns immediately with @c false — back-pressure is
         * the caller's responsibility (drop / log / coalesce / wake the consumer faster).
         *
         * @tparam Args Constructor argument types (must satisfy @c constructible_from).
         * @param args  Forwarded to @c T's constructor.
         * @return @c true if the element was committed, @c false if the ring was full.
         */
        template<typename... Args>
            requires std::constructible_from<T, Args&&...>
        [[nodiscard]] bool TryEmplace(Args&&... args)
            noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
            size_t pos = tail_.load(std::memory_order_relaxed);
            for (;;) {
                Detail::MpscCell<T>& cell = cells_[pos & kIndexMask];
                const size_t seq         = cell.seq.load(std::memory_order_acquire);
                const intptr_t diff      = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
                if (diff == 0) {
                    // Cell is ours iff we win the tail CAS. If we lose, restart with the new tail.
                    if (tail_.compare_exchange_weak(pos, pos + 1,
                                                   std::memory_order_relaxed,
                                                   std::memory_order_relaxed)) {
                        ::new (static_cast<void*>(cell.ValuePtr())) T(std::forward<Args>(args)...);
                        cell.seq.store(pos + 1, std::memory_order_release);
                        return true;
                    }
                } else if (diff < 0) {
                    return false;  // Cell still holds an unconsumed payload one lap behind: full.
                } else {
                    // Another producer overtook us; reload the tail and try again.
                    pos = tail_.load(std::memory_order_relaxed);
                }
            }
        }

        /// @brief Push a movable @p value, returning @c false if the ring is full.
        [[nodiscard]] bool TryPush(T&& value) noexcept {
            return TryEmplace(std::move(value));
        }

        /// @brief Push a copyable @p value, returning @c false if the ring is full.
        [[nodiscard]] bool TryPush(const T& value) 
            noexcept(std::is_nothrow_copy_constructible_v<T>) requires std::copy_constructible<T> {
            return TryEmplace(value);
        }
        /// @}

        /// @name Consumer-side API (single thread)
        /// @{

        /**
         * @brief Drain at most one element. Returns @c std::nullopt when the queue is empty.
         *
         * Consumer-only — concurrent calls from multiple threads are undefined behaviour. The
         * single-consumer contract lets this function avoid CAS entirely: it advances @c head_
         * with a relaxed store after the move-out, since no other thread reads or writes
         * @c head_.
         */
        [[nodiscard]] std::optional<T> TryPop() noexcept {
            const size_t pos         = head_.load(std::memory_order_relaxed);
            Detail::MpscCell<T>& cell = cells_[pos & kIndexMask];
            const size_t seq         = cell.seq.load(std::memory_order_acquire);
            const intptr_t diff      = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (diff != 0) {
                return std::nullopt;  // Producer hasn't committed this slot yet.
            }
            T value(std::move(*cell.ValuePtr()));
            std::destroy_at(cell.ValuePtr());
            // Release the cell to a producer one full lap later.
            cell.seq.store(pos + Capacity, std::memory_order_release);
            head_.store(pos + 1, std::memory_order_relaxed);
            return std::optional<T>{std::move(value)};
        }

        /**
         * @brief Drain every committed element by feeding it to @p sink, returning the count.
         *
         * Consumer-only. Sink may be invoked as either @c sink(T&&) or @c sink(T) and is
         * called for each drained element in FIFO order. The drain stops when the queue is
         * observed empty; concurrent producers may add new elements after that point — the
         * consumer must call @ref Drain again to see them.
         *
         * @tparam Sink Callable accepting a single @c T&& argument.
         * @param sink  Invoked once per drained element.
         * @return Number of elements drained.
         */
        template<typename Sink>
            requires std::invocable<Sink&, T&&>
        size_t Drain(Sink&& sink) noexcept(std::is_nothrow_invocable_v<Sink&, T&&>) {
            size_t drained = 0;
            while (true) {
                std::optional<T> value = TryPop();
                if (!value.has_value()) {
                    break;
                }
                sink(std::move(*value));
                ++drained;
            }
            return drained;
        }
        /// @}

        /// @name Queries / diagnostics
        /// @{

        /// @brief Compile-time capacity.
        [[nodiscard]] static constexpr size_t Capacity_() noexcept { return Capacity; }

        /**
         * @brief Truthful only on the consumer thread; an unsynchronised hint for producers.
         *
         * Compares the consumer's @c head_ against the producer-side @c tail_ snapshot. On the
         * consumer this is a sound emptiness check between drains. From a producer thread the
         * answer is racy by definition — by the time it returns, another producer may have
         * advanced the tail.
         */
        [[nodiscard]] bool Empty() const noexcept {
            return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_acquire);
        }

        /**
         * @brief Approximate number of pending elements (`tail - head`, racy under contention).
         *
         * Useful for back-pressure dashboards and tests; do not use as a control-flow predicate
         * on the producer side without external coordination.
         */
        [[nodiscard]] size_t ApproxSize() const noexcept {
            const size_t tail = tail_.load(std::memory_order_acquire);
            const size_t head = head_.load(std::memory_order_relaxed);
            return tail - head;  // Wraps cleanly in unsigned arithmetic; bounded by Capacity.
        }

        /// @brief Compile-time physical-layout report for this specialisation.
        [[nodiscard]] static consteval Concurrency::CacheLayoutReport Layout() {
            return Concurrency::AnalyzeCacheLayout<MpscQueue>();
        }
        /// @}

    private:
        static constexpr size_t kIndexMask = Capacity - 1;

        // --- Producer-contended ticket dispenser: alone on its cache line ---
        [[=Concurrency::Contended{}]] alignas(Platform::kCacheLineSize)
        std::atomic<size_t> tail_{0};

        // --- Consumer's private cursor: alone on its cache line ---
        [[=Concurrency::ConsumerOwned{}]] alignas(Platform::kCacheLineSize)
        std::atomic<size_t> head_{0};

        // --- Cell array: each cell already cache-line-aligned (see Detail::MpscCell) ---
        [[=Concurrency::SharedStorage{}]] alignas(Platform::kCacheLineSize)
        Detail::MpscCell<T> cells_[Capacity]{};
    };

    // =========================================================================
    // Compile-time layout audit (representative instantiations)
    // =========================================================================

    /** @cond INTERNAL */
    consteval {

        using Concurrency::AuditFalseSharing;
        using Concurrency::DomainStartsLine;
        using Concurrency::Contended;
        using Concurrency::ConsumerOwned;

        // Internal false sharing: the contended tail, the consumer-owned head, and the cold cell
        // array are three distinct write domains and must never overlap a cache line.
        static_assert(AuditFalseSharing<MpscQueue<std::uint64_t>>(),
                      "MpscQueue<uint64_t> failed the internal false-sharing audit");
        static_assert(DomainStartsLine<MpscQueue<std::uint64_t>, Contended>(),
                      "MpscQueue tail must start its own cache line");
        static_assert(DomainStartsLine<MpscQueue<std::uint64_t>, ConsumerOwned>(),
                      "MpscQueue head must start its own cache line");

    }
    /** @endcond */

} // namespace Mashiro

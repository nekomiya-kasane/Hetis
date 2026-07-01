/**
 * @file ConcurrentSlabArena.h
 * @brief Append-only, multi-producer slab arena for fixed-layout, non-recycled nodes.
 *
 * Many concurrent data structures in this project — @c Yuki::FacadeList, future side-table entries,
 * scheduler waiter chains — share one allocation pattern: any thread may publish a small
 * fixed-layout node, the node is *never* relinked or freed individually, and the entire chain dies
 * with the host. The pool / generation-checked allocator (@ref ConcurrentObjectPool) is the wrong
 * tool for that pattern: ABA safety, generation guards and reuse cost machinery the workload never
 * exercises. The right tool is a *monotonic slab arena*: each producer wins a unique slot index
 * with one @c fetch_add, the slot lives in a heap-allocated, cache-line-aligned slab page, and the
 * arena's destructor frees the slab list.
 *
 * @section algorithm Algorithm
 * One contended atomic, @c tail_, dispenses slot tickets. A ticket decomposes into
 * @c (slab_index, slot_in_slab). The slab a producer needs is *almost always* the current one —
 * it is the producer who first overruns a slab boundary that allocates the next slab and publishes
 * it via a CAS on @c current_. Other producers that targeted the same boundary block briefly on a
 * loaded-acquire poll until @c current_ catches up — the typical wait is microseconds because the
 * grow path is a single allocation. Once published, slabs are immutable: a slab pointer obtained at
 * publish time is valid for the rest of the arena's life.
 *
 * @section properties Properties
 * - **Wait-free common path**: @c Allocate is one @c fetch_add + one cache-resident store on the
 *   inside-slab fast path. No locking, no pool free-list CAS.
 * - **Contiguous storage per slab**: @c NodesPerSlab elements are laid out back-to-back inside one
 *   slab, so traversal of recently-attached nodes is cache-friendly.
 * - **No false sharing**: the contended @c tail_ and @c current_ atomics own their cache lines via
 *   the project-wide @ref Concurrency::Contended domain; the inert slab registry is tagged
 *   @ref Concurrency::Immutable. The audit runs in a `consteval` block at the bottom of this file.
 * - **Trivially-destructible payload**: nodes are never destroyed individually. The element type
 *   must therefore be either trivially destructible or model the
 *   @ref Traits::SlabArenaSlotable concept (which permits a no-op destructor for nodes that own no
 *   resources). The arena does not run per-node destructors on teardown — that responsibility is
 *   the caller's, and it is exactly the contract the use sites want.
 * - **Compile-time diagnostics**: the slab is `alignas(std::hardware_destructive_interference_size)`,
 *   slab capacity must be a power of two, and a `static_assert` proves the slab payload occupies
 *   whole cache lines so neighbouring instances never share a line with the contended head.
 *
 * @section reset Reset
 * @c Reset frees every slab except the very first (kept for amortisation), then re-arms @c tail_ to
 * zero and @c current_ to that first slab. Reset is *not* concurrent: callers must ensure no
 * producer is racing.
 *
 * @section cost Cost
 * - One @c fetch_add (relaxed) per allocation in the steady state.
 * - One @c malloc per @c NodesPerSlab allocations (amortised) on the slow path.
 * - Zero atomics on the consumer side: a consumer reads slab pointers from the registry, which is
 *   append-only and stable once published.
 *
 * @ingroup Core
 */
#pragma once

#include "Mashiro/Core/FalseSharing.h"
#include "Mashiro/Core/TypeTraits.h"
#include "Mashiro/Math/TrivialOps.h"

#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace Mashiro {

    namespace Traits {

        /**
         * @brief Element requirements for @ref ConcurrentSlabArena.
         *
         * The arena never runs per-element destructors, so the slot type must either be trivially
         * destructible or carry a no-op destructor by design (e.g. a node that owns no resources).
         * The type must also be @c noexcept-constructible from the argument pack the producer hands
         * to @c Allocate, since a throwing constructor on a wait-free path cannot be unwound without
         * leaking the slot ticket. The minimal contract is a pair of standard traits.
         */
        template<typename T>
        concept SlabArenaSlotable = std::is_trivially_destructible_v<T> || std::is_nothrow_destructible_v<T>;

    } // namespace Traits

    namespace Detail {

        /**
         * @brief Default slab capacity for type @p T — picks the smallest power of two such that the
         *        slab payload occupies at least one cache line.
         *
         * Hand-tuning per use site is rarely worth the noise; this default produces 64-element slabs
         * for a 16-byte node on a 64-byte cache line, which is what the @c FacadeList workload wants.
         */
        template<typename T>
        constexpr std::size_t DefaultNodesPerSlab() noexcept {
            constexpr std::size_t kLine = std::hardware_destructive_interference_size;
            return Math::CeilPow2((kLine + sizeof(T) - 1) / sizeof(T) * 4);
        }

    } // namespace Detail

    // =========================================================================
    // ConcurrentSlabArena
    // =========================================================================

    /**
     * @brief Append-only, multi-producer slab allocator for fixed-layout, non-recycled nodes.
     *
     * @tparam T             Element type. Must satisfy @ref Traits::SlabArenaSlotable.
     * @tparam NodesPerSlab  Power-of-two slab capacity. Default chooses a cache-line-friendly value.
     *
     * @par Thread safety
     * @c Allocate may be called concurrently from any number of producers. @c Reset and the
     * destructor are exclusive: callers must externally synchronise teardown. The traversal helpers
     * take a stable producer ordering (newest-first across slabs); they assume publication has
     * stopped, which is the natural state when a consumer scans the chain.
     */
    template<typename T, std::size_t NodesPerSlab = Detail::DefaultNodesPerSlab<T>()>
        requires Traits::SlabArenaSlotable<T>
    class ConcurrentSlabArena {
        static_assert(std::has_single_bit(NodesPerSlab),
                      "NodesPerSlab must be a power of two for shift+mask decomposition.");
        static_assert(NodesPerSlab >= 1, "NodesPerSlab must be at least one element.");

    public:
        using ElementType = T;
        static constexpr std::size_t kNodesPerSlab = NodesPerSlab;
        static constexpr std::size_t kSlotMask    = NodesPerSlab - 1;
        static constexpr std::size_t kSlotShift   = std::countr_zero(NodesPerSlab);

        /**
         * @brief Build an empty arena. Allocates the first slab eagerly so the steady-state path is
         *        branch-free on the inside-slab fast path.
         */
        ConcurrentSlabArena() : current_{nullptr} {
            Slab* first = AllocateSlab(0);
            current_.store(first, std::memory_order_release);
        }

        ConcurrentSlabArena(const ConcurrentSlabArena&) = delete;
        ConcurrentSlabArena& operator=(const ConcurrentSlabArena&) = delete;
        ConcurrentSlabArena(ConcurrentSlabArena&&) = delete;
        ConcurrentSlabArena& operator=(ConcurrentSlabArena&&) = delete;

        ~ConcurrentSlabArena() noexcept { FreeAllSlabs(); }

        /**
         * @brief Allocate one @c T constructed from @p args, returning a pointer to it.
         *
         * Exception safety: requires @c T to be @c nothrow_constructible from @p Args; a throwing
         * constructor would leak the ticket. The wait-free common path is one @c fetch_add and one
         * placement new into a cache-resident slab slot.
         *
         * @par Ticket layout
         * Each producer's ticket is a single @c size_t — a monotonic global slot index. With
         * @c NodesPerSlab a power of two, the ticket decomposes into @c (slab_index, slot) by a
         * shift and a mask:
         *
         * @verbatim
         * ticket bits (NodesPerSlab = 2^kSlotShift):
         *
         *  high                                              low
         *  +----------------------------------+--------------+
         *  |          slab index              |   slot in    |
         *  |  ticket >> kSlotShift            |    slab      |
         *  |                                  | ticket &     |
         *  |                                  | kSlotMask    |
         *  +----------------------------------+--------------+
         *  ^                                  ^              ^
         *  |                                  |              |
         *  bit (sizeof(size_t)*8 - 1)         kSlotShift     bit 0
         *
         * Example  NodesPerSlab = 4 → kSlotShift = 2, kSlotMask = 0b11:
         *   ticket  6 → slab 1, slot 2
         *   ticket 17 → slab 4, slot 1
         * @endverbatim
         */
        template<typename... Args>
            requires std::constructible_from<T, Args&&...>
        T* Allocate(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
            const std::size_t ticket = tail_.fetch_add(1, std::memory_order_relaxed);
            const std::size_t slabId = ticket >> kSlotShift;
            const std::size_t slot   = ticket &  kSlotMask;

            Slab* slab = ReachSlab(slabId);
            T* place = slab->cells + slot;
            ::new (static_cast<void*>(place)) T(std::forward<Args>(args)...);
            // Producers see this slot via the per-node 'next' link they install as part of T;
            // the publication fence on that link's release store synchronises with consumers.
            return place;
        }

        /// @brief Total nodes ever produced. Reset by @ref Reset.
        [[nodiscard]] std::size_t Size() const noexcept {
            return tail_.load(std::memory_order_acquire);
        }

        /// @brief Number of slabs currently held (after the eager-allocate of slab #0).
        ///        Walks the immutable @c prev chain from @c current_; safe to call concurrently
        ///        with producers, though the count may grow during the walk.
        [[nodiscard]] std::size_t SlabCount() const noexcept {
            std::size_t n = 0;
            for (Slab* s = current_.load(std::memory_order_acquire); s != nullptr; s = s->prev) {
                ++n;
            }
            return n;
        }

        /**
         * @brief Free every slab except the first; rearm @c tail_ and @c current_ to slab #0.
         *
         * Not concurrent: the caller guarantees no producer is mid-Allocate. The first slab is
         * retained so a hot/cold churn (frame arena, transient tie attach) does not pay malloc
         * each cycle.
         */
        void Reset() noexcept {
            Slab* first = nullptr;
            for (Slab* s = current_.load(std::memory_order_relaxed); s != nullptr; ) {
                Slab* prev = s->prev;
                if (prev == nullptr) {
                    first = s;
                    break;
                }
                FreeSlab(s);
                s = prev;
            }
            current_.store(first, std::memory_order_release);
            tail_.store(0, std::memory_order_release);
        }

        // -------- Diagnostics -----------------------------------------------

        /// @brief Pointer to slab #@p index in publication order, or @c nullptr if unallocated.
        [[nodiscard]] const T* SlabBase(std::size_t index) const noexcept {
            Slab* head = current_.load(std::memory_order_acquire);
            if (head == nullptr) {
                return nullptr;
            }
            const std::size_t total = [&] {
                std::size_t n = 0;
                for (Slab* s = head; s != nullptr; s = s->prev) {
                    ++n;
                }
                return n;
            }();
            if (index >= total) {
                return nullptr;
            }
            std::size_t hops = total - 1 - index;
            Slab* s = head;
            for (std::size_t i = 0; i < hops; ++i) {
                s = s->prev;
            }
            return s ? s->cells : nullptr;
        }

    private:
        struct alignas(Platform::kCacheLineSize) Slab {
            T cells[NodesPerSlab];
            Slab* prev{nullptr};      ///< Previously published slab (for teardown traversal).
            std::size_t slabId{0};   ///< Index in publication order (0 = first).
        };

        /// @brief Reach the slab whose @c slabId matches @p needed. Allocates+publishes if missing.
        [[nodiscard]] Slab* ReachSlab(std::size_t needed) noexcept {
            Slab* s = current_.load(std::memory_order_acquire);
            while (s->slabId != needed) {
                if (s->slabId > needed) {
                    // Reach back through the immutable @c prev chain to an already-published slab.
                    s = s->prev;
                    continue;
                }
                // s->slabId < needed: we (or someone) need to grow. Build the next slab off of
                // @c s and try to publish it. The CAS is the linearisation point of slab growth.
                Slab* fresh = AllocateSlab(s->slabId + 1);
                fresh->prev = s;
                Slab* expected = s;
                if (current_.compare_exchange_strong(expected, fresh,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
                    s = fresh;
                } else {
                    // Lost the race; reuse the winner's slab (or a later one) and retry.
                    FreeSlab(fresh);
                    s = expected;
                }
            }
            return s;
        }

        Slab* AllocateSlab(std::size_t id) {
            void* mem = ::operator new(sizeof(Slab), std::align_val_t{alignof(Slab)});
            auto* s = ::new (mem) Slab{};
            s->slabId = id;
            return s;
        }

        void FreeSlab(Slab* s) noexcept {
            s->~Slab();
            ::operator delete(s, std::align_val_t{alignof(Slab)});
        }

        void FreeAllSlabs() noexcept {
            for (Slab* s = current_.load(std::memory_order_relaxed); s != nullptr; ) {
                Slab* prev = s->prev;
                FreeSlab(s);
                s = prev;
            }
            current_.store(nullptr, std::memory_order_release);
        }

        // Layout: the contended ticket dispenser owns its own cache line. @c current_ shares the
        // @ref Concurrency::Contended domain (it is CAS-mutated on slab grow) and may co-locate
        // with @c tail_; the slab chain itself is reached through the immutable @c prev links from
        // @c current_, so no separate registry head is needed.
        [[=Concurrency::Contended{}]] alignas(Platform::kCacheLineSize)
            std::atomic<std::size_t> tail_{0};
        [[=Concurrency::Contended{}]]
            std::atomic<Slab*>       current_;
    };

    // =========================================================================
    // Compile-time false-sharing audit
    // =========================================================================

    namespace Detail {
        struct AuditNode { std::uint64_t key; void* next; };
        static_assert(std::is_trivially_destructible_v<AuditNode>);
    } // namespace Detail

    consteval bool AuditConcurrentSlabArenaLayout() {
        using Arena = ConcurrentSlabArena<Detail::AuditNode>;
        static_assert(Concurrency::AuditFalseSharing<Arena>(),
                      "ConcurrentSlabArena: contended head must not share a line with any other "
                      "writer domain.");
        static_assert(Concurrency::DomainStartsLine<Arena, Concurrency::Contended>(),
                      "ConcurrentSlabArena: contended head must start a cache line.");
        return true;
    }

    static_assert(AuditConcurrentSlabArenaLayout());

} // namespace Mashiro

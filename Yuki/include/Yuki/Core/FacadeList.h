/**
 * @file FacadeList.h
 * @brief Runtime-attached interface list — the cold-path arm of @c QueryInterface.
 *
 * The hot path of @ref Query reaches an interface through the inline-aggregated facade subobjects
 * baked into the implementation at compile time (Hot interfaces) or through @c DirectCast for true
 * BOA. Some interfaces, however, are not visible to the impl when it is compiled: Cold-flagged
 * interfaces, plug-in extensions installed at load time, and adapters synthesised after the fact.
 * Those land here, in a per-impl facade chain attached to the @c RootObject.
 *
 * @section storage Node storage
 * Facade nodes are allocated from a single program-wide @ref Mashiro::ConcurrentSlabArena: one
 * monotonic, wait-free slab pool shared across every host. Per-host @ref FacadeListHead carries
 * only the chain head (one atomic word), so an impl that never attaches a Cold interface pays the
 * cost of one word and nothing else. The shared arena keeps recently-attached nodes contiguous
 * across hosts; a host that attaches several nodes in sequence sees them land in the same slab,
 * which is exactly the cache-friendly layout the cold-path walk wants.
 *
 * The arena is leak-free with respect to the program: at process exit, its destructor releases
 * every slab. It is not reset at host destruction (a node lives for the host's lifetime, but the
 * cost of leaving its slot in the arena is one slot's worth of memory — recovered at program
 * teardown). This is the right trade for a per-impl-rare-event allocator: zero per-host overhead,
 * zero recycle bookkeeping, perfect cache locality for the rare attach.
 *
 * @section facade Typed facade pointer
 * Each entry stores a @ref RootObject* facade — every facade derives from @ref RootObject through
 * @c IfaceFacadeNode, so the static type already names "a polymorphic facade subobject". The
 * receiver narrows the pointer to the concrete @c InterfaceFacade<I, Impl> type by the IID match.
 * There is no separate per-node interface MetaCore field: the IID *is* the key, and the facade's
 * metaclass is reachable through @c MetaClassDynamic when one is needed.
 *
 * @section discipline Discipline
 * @c Attach is wait-free for the producer (one slab-bump + one CAS on @c head_); the chain is
 * walked with acquire-loads on the head, then plain reads of the immutable @c next links. There
 * is no remove operation — runtime detach is genuinely rare and is modelled by *attaching* a
 * replacement, since @c Lookup takes the first match.
 *
 * @ingroup Core
 */
#pragma once

#include <atomic>
#include <utility>

#include <Mashiro/Core/ConcurrentSlabArena.h>

#include <Yuki/Core/MetaClass.h>

namespace Yuki {

    class RootObject;  // forward — FacadeNode targets a polymorphic facade subobject.

    /**
     * @brief One link in a @ref FacadeListHead chain — an IID paired with its typed facade pointer.
     *
     * The facade pointer is a @ref RootObject* because every interface facade derives from
     * @c RootObject via @c IfaceFacadeNode. Receivers narrow it to the concrete
     * @c InterfaceFacade<I, Impl> by the IID. @c next is non-atomic: a node is linked once at
     * publication and never relinked, so the producer's release on @c head_ also publishes
     * @c next.
     */
    struct FacadeNode {
        Iid iid{};                    ///< The interface this entry resolves.
        RootObject* facade{nullptr};  ///< Typed pointer to the matching facade subobject.
        FacadeNode* next{nullptr};    ///< Next link; one-shot, never reassigned.
    };

    static_assert(std::is_trivially_destructible_v<FacadeNode>,
                  "FacadeNode is allocated in an arena that does not run per-node destructors.");

    /// @brief Program-wide shared arena that owns every @ref FacadeNode in the process.
    ///        Defined inline (C++17) so the symbol is unique without a separate .cpp file.
    inline Mashiro::ConcurrentSlabArena<FacadeNode>& FacadeNodeArena() noexcept {
        static Mashiro::ConcurrentSlabArena<FacadeNode> arena;
        return arena;
    }

    /**
     * @brief Lock-free head of a per-host facade chain — push at front, walk from front.
     *
     * The head is the single atomic word a host pays for the (rare) ability to receive Cold or
     * runtime-attached interfaces. Storage for nodes is drawn from @ref FacadeNodeArena, the
     * program-wide slab pool, so attaching a node is one slab-bump + one CAS and an empty chain
     * costs no allocations.
     *
     * Hosts that never attach a Cold interface keep an empty head — a single null word.
     *
     * Two flavours of attach live on this class:
     *   - @ref Attach allocates a fresh node from @ref FacadeNodeArena and pushes it — the
     *     traditional shape used when the caller has no node yet (e.g. a Cold facade synthesised on
     *     first query);
     *   - @ref FacadeListHead::CompareExchangeHead is the raw CAS primitive that the free function
     *     @ref AttachUnique drives, splicing in a caller-owned node only after an iid scan confirms
     *     no node with the same IID is already present (spec §1.5 cardinality invariant).
     *
     * The raw @ref Head accessor and CAS hook are exposed publicly so that iid-keyed dedup, the
     * eager-materialisation hook (spec §5.4 step 2), and any future external chain walker share one
     * implementation of "load the head with acquire" — no parallel copies of the memory-order
     * contract.
     */
    class alignas(8) FacadeListHead {
    public:
        constexpr FacadeListHead() noexcept = default;

        FacadeListHead(const FacadeListHead&) = delete;
        FacadeListHead& operator=(const FacadeListHead&) = delete;
        FacadeListHead(FacadeListHead&&) = delete;
        FacadeListHead& operator=(FacadeListHead&&) = delete;

        /// @brief Attach a fresh node for @p iid → @p facade. Returns the node on success.
        ///        Wait-free for the slab path; one CAS retry loop on the head.
        ///
        ///        This is the "no-dedup" form: callers that need the iid-uniqueness guarantee should
        ///        allocate a node themselves and call @ref AttachUnique instead. The CAS body is
        ///        shared via @ref CompareExchangeHead so the memory-order contract lives in one
        ///        place.
        FacadeNode* Attach(Iid iid, RootObject* facade) noexcept {
            FacadeNode* node = FacadeNodeArena().Allocate(iid, facade, /*next=*/nullptr);
            FacadeNode* observed = Head();
            do {
                node->next = observed;
            } while (!CompareExchangeHead(observed, node));
            return node;
        }

        /**
         * @brief Walk the chain and return the first facade whose IID matches @p iid, else
         *        @c nullptr. Newest-first: a later-attached entry shadows an earlier one with the
         *        same IID, which is the design's substitute for a remove operation.
         */
        [[nodiscard]] RootObject* Lookup(Iid iid) const noexcept {
            for (FacadeNode* n = Head(); n != nullptr; n = n->next) {
                if (n->iid == iid) {
                    return n->facade;
                }
            }
            return nullptr;
        }

        /// @brief @c true when no facade has ever been attached to this host.
        [[nodiscard]] bool Empty() const noexcept {
            return head_.load(std::memory_order_relaxed) == nullptr;
        }

        /// @brief Acquire-load of the chain head — the entry point for every reader walk.
        ///        Public so that the free-function helpers (@ref FacadeListLookup, @ref AttachUnique)
        ///        share one definition of the read-side memory-order contract.
        [[nodiscard]] FacadeNode* Head() const noexcept {
            return head_.load(std::memory_order_acquire);
        }

        /**
         * @brief Try to splice @p desired in as the new chain head, expecting @p expected.
         *
         * Wraps @c compare_exchange_weak with the release/relaxed pair used by every producer in
         * this class: success publishes @p desired and its @c next link with release semantics;
         * failure reloads @p expected with relaxed semantics so the caller can retry against the
         * fresh head. @p expected is updated in-place on failure (matching the @c std::atomic API)
         * and returns @c true exactly when the CAS succeeded.
         */
        bool CompareExchangeHead(FacadeNode*& expected, FacadeNode* desired) noexcept {
            return head_.compare_exchange_weak(expected, desired,
                                               std::memory_order_release,
                                               std::memory_order_relaxed);
        }

    private:
        std::atomic<FacadeNode*> head_{nullptr};
    };

    static_assert(sizeof(FacadeListHead) == sizeof(void*),
                  "FacadeListHead must stay one pointer wide so per-impl footprint stays minimal.");

    // =========================================================================
    // Free-function helpers — iid-keyed lookup and CAS-dedup AttachUnique
    // =========================================================================

    /**
     * @brief Lookup the facade for @p id on @p head — free-function form of
     *        @ref FacadeListHead::Lookup, kept as a peer of @ref AttachUnique so the two halves of
     *        the cardinality invariant (read and write) read symmetrically at call sites.
     *
     * Returns the first matching @c facade pointer, or @c nullptr when no entry's IID equals
     * @p id. Total: empty head returns @c nullptr without touching memory beyond the head word.
     */
    [[nodiscard]] inline RootObject* FacadeListLookup(const FacadeListHead& head, Iid id) noexcept {
        for (FacadeNode* n = head.Head(); n != nullptr; n = n->next) {
            if (n->iid == id) {
                return n->facade;
            }
        }
        return nullptr;
    }

    /**
     * @brief Attach @p node iff no existing entry already carries its IID; on conflict, return the
     *        previously-installed node.
     *
     * The cardinality kernel for spec §1.5: enforces "exactly one Extension instance per
     * (Extension type, closure)" by combining an iid scan with a head CAS in a single retry loop.
     * On every iteration the scan runs first; if it finds a match the function returns immediately
     * without publishing @p node. Otherwise the loop snapshots the current head, links @p node->next
     * to it, and CASes. On CAS failure the loop restarts the scan against the new head — a racing
     * winner with the same IID is observed and returned; a racing winner with a different IID is
     * absorbed into @p node->next on the next pass.
     *
     * @return @p node when this call successfully published it; otherwise the existing node whose
     *         IID matched. Either way the returned pointer is the unique survivor for that IID, so
     *         callers always have a valid handle to "the one node for this Extension".
     *
     * @note @p node is caller-owned: the function never allocates and never frees, which is what
     *       lets the eager-materialisation path (where the node lives inside an Extension instance)
     *       and the lazy @c SideTableResolver path share one primitive. Use
     *       @ref FacadeListHead::Attach when the caller wants the arena to own the node instead.
     */
    [[nodiscard]] inline FacadeNode* AttachUnique(FacadeListHead& head, FacadeNode* node) noexcept {
        while (true) {
            for (FacadeNode* n = head.Head(); n != nullptr; n = n->next) {
                if (n->iid == node->iid) {
                    return n;
                }
            }
            FacadeNode* observed = head.Head();
            node->next = observed;
            if (head.CompareExchangeHead(observed, node)) {
                return node;
            }
        }
    }

} // namespace Yuki

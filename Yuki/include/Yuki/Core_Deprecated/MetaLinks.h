/**
 * @file MetaLinks.h
 * @brief D18 layer 2 of 3 — RCU-published atomic snapshot pointers + cacheEpoch.
 *
 * MetaLinks is the runtime-mutable middle layer of the three-layer MetaClass (D18).
 * It holds one atomic pointer per snapshot type; all slots are nullptr-initialised and
 * filled by Tasks 13 and 17 once the runtime wire-up phase runs. cacheEpoch is a
 * monotonically increasing counter bumped whenever any slot changes, used by the D15
 * L1 fingerprint cache to detect stale hits without a lock.
 *
 * Companion to MetaCore (rodata, Task 7) and MetaDynamic (instance, Task 9).
 * Snapshot structs are forward-declared here; their definitions land in Tasks 13 and 17.
 *
 * @ingroup Core
 */
#pragma once

#include <Yuki/Core/FingerprintCache.h>

#include <atomic>
#include <cstdint>
#include <mutex>

namespace Yuki {

    struct MetaCore;

    /** @cond INTERNAL — snapshot types forward-declared here so MetaLinks.h compiles before Tasks 13
     *  and 17 land; do NOT replace with includes prematurely. The decoupling is deliberate: MetaLinks
     *  is consumed by MetaDynamic (Task 9) and Y_OBJECT, both of which must build before the dispatch
     *  and extendedBy machinery exists. */
    struct DispatchSnapshot;
    struct MergedDispatchSnapshot;
    struct ExtendedListSnapshot;
    struct ImplementedListSnapshot;
    struct EagerSetSnapshot;
    /** @endcond */

    /// @brief T23 / D16 reverse-edge snapshot: the concrete subclasses of this MetaClass.
    ///
    /// Holds @c const @c MetaCore* (class-level identity), not @c RootObject*. Populated at
    /// @c Registry::Install<T>() time by walking @p T's C++ bases via reflection and
    /// CAS-appending @p T's MetaCore onto each base's slot. Read by the D16 base-chain
    /// flatten in @c RegisterSideTable / @c RegisterCodeExt to push extension entries down
    /// into every subclass's mergedDispatch table.
    ///
    /// Snapshots are immutable once published; mutation produces a fresh snapshot and
    /// retires the old one via @c RetireSnapshot. Iid-sorted for stable diff + binary
    /// search (rare; the typical caller walks the array linearly).
    struct SubclassSnapshot {
        std::size_t count{0};
        const MetaCore* const* data{nullptr};
    };

    /**
     * @brief Runtime-mutable RCU snapshot layer for a Yuki class (D18 layer 2 of 3).
     *
     * Each field is an atomic pointer to an immutable, RCU-published snapshot allocated
     * by the runtime. Readers load with @c memory_order_acquire; writers publish with
     * @c memory_order_release. All slots default to @c nullptr (class has no wired tables
     * yet). @ref BumpCacheEpoch must be called after any slot mutation so that D15 L1
     * readers see a changed epoch and discard stale cached results.
     */
    struct MetaLinks {
        /// RCU slot for the primary dispatch table. 
        std::atomic<const DispatchSnapshot*> dispatch{nullptr};

        /// RCU slot for the merged (flattened) dispatch table. 
        std::atomic<const MergedDispatchSnapshot*> mergedDispatch{nullptr};

        /// RCU slot for the extendedBy reverse-edge list. 
        std::atomic<const ExtendedListSnapshot*> extendedBy{nullptr};

        /// RCU slot for the implementedBy reverse-edge list. 
        std::atomic<const ImplementedListSnapshot*> implementedBy{nullptr};

        /// RCU slot for the eager-materialise set. 
        std::atomic<const EagerSetSnapshot*> eagerSet{nullptr};

        /// T23 / D16 RCU slot: concrete subclasses of this class. Populated by Install<D>()
        /// for each base B of D. Walked by RegisterSideTable / RegisterCodeExt to flatten
        /// extension entries down into subclass mergedDispatch tables.
        std::atomic<const SubclassSnapshot*> subclassedBy{nullptr};

        /// D15 monotonic gate counter — bumped on any link mutation; drives L1 invalidation.
        std::atomic<std::uint64_t> cacheEpoch{0};

        /// D15 L1 — per-class 4-slot lock-free fingerprint ring.
        FingerprintCache l1{};

        /// T23 — per-class writer mutex. Acquired by Install / RegisterSideTable /
        /// RegisterCodeExt / DeleteParkedFor to serialise snapshot mutations. Acquired in
        /// subclassedBy iid-sort order to prevent deadlock when a base + subclass are both
        /// being written concurrently (§5.4). Replaces A2's external @c MutexFor table.
        std::mutex writerMu{};

        /**
         * @brief Monotonically increment @ref cacheEpoch under @c acq_rel ordering.
         *
         * Readers use the epoch value to drive L1 fingerprint cache invalidation: if the
         * epoch observed at lookup time differs from the epoch at result-use time, the
         * cached result is discarded. Must be called after every snapshot pointer swap.
         *
         * @note The @c acq_rel ordering is load-bearing: readers acquire the epoch to
         *       establish a happens-before edge over their subsequent snapshot-pointer
         *       @c load(acquire) against the writer's @c store(release) of those pointers.
         *       @c memory_order_release alone would leave a reader that observes the new
         *       epoch without an ordering guarantee over the pointer stores that preceded
         *       the bump — they could appear to predate the bump, causing the reader to
         *       trust a stale snapshot. The acquire side is therefore required, not
         *       cosmetic. Do not weaken to @c release or @c relaxed.
         */
        void BumpCacheEpoch() noexcept { cacheEpoch.fetch_add(1, std::memory_order_acq_rel); }
    };

} // namespace Yuki

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

#include <atomic>
#include <cstdint>

namespace Yuki {

    /** @cond INTERNAL — snapshot types defined in Tasks 13 and 17 */
    struct DispatchSnapshot;           ///< Defined in Task 13.
    struct MergedDispatchSnapshot;     ///< Defined in Task 17.
    struct ExtendedListSnapshot;       ///< Defined in Task 17.
    struct ImplementedListSnapshot;    ///< Defined in Task 17.
    struct EagerSetSnapshot;           ///< Defined in Task 17.
    /** @endcond */

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
        /// RCU slot for the primary dispatch table. Filled by Task 13 (DispatchEntry array).
        std::atomic<const DispatchSnapshot*>        dispatch{nullptr};

        /// RCU slot for the merged (flattened) dispatch table. Filled by Task 17.
        std::atomic<const MergedDispatchSnapshot*>  mergedDispatch{nullptr};

        /// RCU slot for the extendedBy reverse-edge list. Filled by Task 17.
        std::atomic<const ExtendedListSnapshot*>    extendedBy{nullptr};

        /// RCU slot for the implementedBy reverse-edge list. Filled by Task 17.
        std::atomic<const ImplementedListSnapshot*> implementedBy{nullptr};

        /// RCU slot for the eager-materialise set. Filled by Task 17.
        std::atomic<const EagerSetSnapshot*>        eagerSet{nullptr};

        /// D15 monotonic gate counter — bumped on any link mutation; drives L1 invalidation.
        std::atomic<std::uint64_t>                  cacheEpoch{0};

        // L1 fingerprint cache slot lands in Task 15 (D15).

        /**
         * @brief Monotonically increment @ref cacheEpoch under @c acq_rel ordering.
         *
         * Readers use the epoch value to drive L1 fingerprint cache invalidation: if the
         * epoch observed at lookup time differs from the epoch at result-use time, the
         * cached result is discarded. Must be called after every snapshot pointer swap.
         */
        void BumpCacheEpoch() noexcept { cacheEpoch.fetch_add(1, std::memory_order_acq_rel); }
    };

} // namespace Yuki

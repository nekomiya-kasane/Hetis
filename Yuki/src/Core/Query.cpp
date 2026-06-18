/**
 * @file Query.cpp
 * @brief D15 L2 kernel — folds L1 fingerprint probe + L2 binary search + L3 epoch gate.
 *
 * Companion translation unit for @ref Query.h. The user-facing @c Query<I,T>(node) template
 * stays in the header so the L0 consteval shortcut can fold to a constant; only the runtime
 * kernel is type-erased here.
 */
#include <Yuki/Core/Query.h>

namespace Yuki {

    const DispatchEntry* QueryDynamicRaw(MetaLinks* links, Iid iid) noexcept {
        if (!links) return nullptr;
        const std::uint64_t epoch = links->cacheEpoch.load(std::memory_order_acquire);

        // L1 probe.
        if (auto hit = Probe(links->l1, iid, epoch)) {
            return *hit;  // may be nullptr — a cached negative hit.
        }

        // L2 binary search.
        const MergedDispatchSnapshot* merged =
            links->mergedDispatch.load(std::memory_order_acquire);
        const DispatchEntry* e = LookupMergedDispatch(merged, iid);

        // Publish the (possibly null) result back into L1 with the witnessed epoch.
        Publish(links->l1, iid, e, epoch);
        return e;
    }

} // namespace Yuki

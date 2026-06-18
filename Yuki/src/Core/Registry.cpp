/**
 * @file Registry.cpp
 * @brief Runtime install kernel - D7.2 cross-module seal checks, atomic publish, L3 broadcast.
 *
 * Implements @ref Yuki::Detail::InstallKernel: serialises writers per nucleus, performs the
 * Final / Unique / Important cross-module check against the prior live snapshot, builds a
 * fresh iid-sorted DispatchSnapshot + MergedDispatchSnapshot, publishes both under release
 * ordering, bumps cacheEpoch, and broadcasts L3 invalidation to downstream classes.
 *
 * For A2 RCU retirement is approximated by an in-place deferred-delete pool keyed off a
 * single mutex; full epoch-RCU lands in A3.
 *
 * @ingroup Core
 */
#include <Yuki/Core/Registry.h>
#include <Yuki/Core/ExtendedList.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Yuki::Detail {

    namespace {
        // Per-nucleus writer mutex. Keyed by MetaLinks* - one entry per Y_OBJECT class.
        std::mutex& MutexFor(MetaLinks* links) {
            static std::mutex tableMutex;
            static std::unordered_map<MetaLinks*, std::unique_ptr<std::mutex>> table;
            std::lock_guard<std::mutex> g(tableMutex);
            auto it = table.find(links);
            if (it == table.end()) {
                auto [ins, _] = table.emplace(links, std::make_unique<std::mutex>());
                return *ins->second;
            }
            return *it->second;
        }

        // Deferred-delete pool keeping retired snapshots alive until Install returns.
        // Full epoch-RCU is deferred to A3; for A2 the pool grows for the program lifetime
        // (snapshots are tiny, churn is bounded by the number of classes).
        struct RetiredPool {
            std::vector<std::unique_ptr<DispatchEntry[]>>        entries;
            std::vector<std::unique_ptr<DispatchSnapshot>>       dispatchSnaps;
            std::vector<std::unique_ptr<MergedDispatchSnapshot>> mergedSnaps;
        };
        RetiredPool& Pool() {
            static RetiredPool p;
            return p;
        }
        std::mutex& PoolMutex() {
            static std::mutex m;
            return m;
        }
    } // namespace

    void InstallKernel(const MetaCore* core, MetaLinks* links,
                       const ImplementsInfo* implements, std::size_t implementsCount) noexcept {
        std::lock_guard<std::mutex> g(MutexFor(links));

        // 1) Read the live snapshot (may be null on first install).
        const DispatchSnapshot* prior = links->dispatch.load(std::memory_order_acquire);

        // 2) Per-info seal check (D7.2). On violation, abort with a diagnostic.
        if (prior) {
            for (std::size_t k = 0; k < implementsCount; ++k) {
                const ImplementsInfo& info = implements[k];
                const DispatchEntry* match = nullptr;
                for (std::size_t j = 0; j < prior->count; ++j) {
                    if (prior->entries[j].iid == info.iid) { match = &prior->entries[j]; break; }
                }
                if (!match) continue;
                // Re-install from the same providerClass is benign.
                if (match->providerClass == core) continue;
                // Final on the prior - derived class may not re-implement.
                if (match->seal.final) std::abort();
                // Unique on this install - at most one provider per closure.
                if (info.flags.unique) std::abort();
                // Important on both - fatal cross-module conflict (D7.2).
                if (match->seal.important && info.flags.important) std::abort();
            }
        }

        // 3) Build a fresh iid-sorted entry array (own count + carry-over of prior entries
        //    whose iid is not shadowed by an Important-wins from this install).
        std::vector<DispatchEntry> built;
        built.reserve(implementsCount + (prior ? prior->count : 0));
        for (std::size_t k = 0; k < implementsCount; ++k) {
            const ImplementsInfo& info = implements[k];
            built.push_back(DispatchEntry{
                .iid           = info.iid,
                .kind          = DispatchKind::InlineFacade,  // refined per-arm in A3.
                .seal          = info.flags,
                .armOffset     = 0,
                .providerClass = core,
                .arm           = nullptr,
            });
        }
        if (prior) {
            for (std::size_t j = 0; j < prior->count; ++j) {
                if (prior->entries[j].providerClass == core) continue;  // replaced.
                built.push_back(prior->entries[j]);
            }
        }
        std::sort(built.begin(), built.end(),
                  [](const DispatchEntry& a, const DispatchEntry& b) { return a.iid < b.iid; });

        // 4) Move the built vector into program-lifetime storage.
        const std::size_t n = built.size();
        auto entries = std::make_unique<DispatchEntry[]>(n);
        for (std::size_t i = 0; i < n; ++i) entries[i] = built[i];

        auto snap = std::make_unique<DispatchSnapshot>();
        snap->count = n;
        snap->entries = entries.get();

        // 5) MergedDispatchSnapshot - for A2 we publish the same array as the merged view
        //    (full base-chain flattening per D16 lands when A3 adds the base walker).
        auto merged = std::make_unique<MergedDispatchSnapshot>();
        merged->count = n;
        merged->entries = entries.get();

        // 6) Publish under release ordering.
        const DispatchSnapshot*       newDispatch = snap.get();
        const MergedDispatchSnapshot* newMerged   = merged.get();
        {
            std::lock_guard<std::mutex> p(PoolMutex());
            Pool().entries.push_back(std::move(entries));
            Pool().dispatchSnaps.push_back(std::move(snap));
            Pool().mergedSnaps.push_back(std::move(merged));
        }
        links->dispatch.store(newDispatch, std::memory_order_release);
        links->mergedDispatch.store(newMerged, std::memory_order_release);

        // 7) Bump epoch and broadcast invalidation.
        links->BumpCacheEpoch();
        const ExtendedListSnapshot* downs =
            links->extendedBy.load(std::memory_order_acquire);
        BroadcastInvalidation(downs);

        (void)core;  // reserved for the A3 diagnostic format.
    }

} // namespace Yuki::Detail

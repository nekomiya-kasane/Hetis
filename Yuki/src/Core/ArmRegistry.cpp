/**
 * @file ArmRegistry.cpp
 * @brief T23 §5.3 / §6.3 — single-entry mergedDispatch update + D16 subclass walk.
 *
 * `RegisterArmAt(implLinks, entry)` is the kernel called by @c RegisterSideTable /
 * @c RegisterCodeExt. It allocates a fresh @ref MergedDispatchSnapshot with @p entry
 * inserted in iid-sorted order (replacing any existing entry with the same iid AND same
 * providerClass), publishes it under release ordering, retires the prior, then walks
 * @c subclassedBy in iid-sorted order — taking each subclass's @c writerMu in turn — and
 * recursively propagates the entry into every (transitive) subclass.
 *
 * Lock ordering (§5.4): write-lock @c implLinks first, then iterate subclasses in their
 * stable iid-sorted order; each subclass's writerMu is acquired and released before
 * moving to the next. Two installs that share a subclass take that subclass's mutex in
 * the same order, so no deadlock cycle can form.
 */
#include <Yuki/Core/ArmRegistry.h>
#include <Yuki/Core/EpochRcu.h>
#include <Yuki/Core/ExtendedList.h>
#include <Yuki/Core/MergedDispatch.h>
#include <Yuki/Core/Registry.h>

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Yuki::Detail {

    namespace {
        std::mutex& CoreLinkMu() noexcept { static std::mutex m;                              return m; }
        std::unordered_map<const MetaCore*, MetaLinks*>&
            CoreLinkMap() noexcept   { static std::unordered_map<const MetaCore*, MetaLinks*> m; return m; }
    }

    void RegisterCoreLinkPair(const MetaCore* core, MetaLinks* links) noexcept {
        if (!core || !links) return;
        std::lock_guard<std::mutex> g(CoreLinkMu());
        CoreLinkMap()[core] = links;
    }

    MetaLinks* SubclassLinksFor(const MetaCore* core) noexcept {
        if (!core) return nullptr;
        std::lock_guard<std::mutex> g(CoreLinkMu());
        auto& m = CoreLinkMap();
        auto it = m.find(core);
        return it == m.end() ? nullptr : it->second;
    }

    namespace {

        void DeleteEntryArray(void* p) noexcept {
            delete[] static_cast<DispatchEntry*>(p);
        }
        void DeleteMergedSnapshot(void* p) noexcept {
            delete static_cast<MergedDispatchSnapshot*>(p);
        }

        // Build a new mergedDispatch snapshot from @p prior by inserting/replacing @p entry.
        //
        // Replacement rule: if @p prior already contains an entry with the SAME iid AND the
        // SAME providerClass, that entry is replaced in-place. If iid matches but provider
        // differs, the new entry is appended too (D7.3 Important-wins is handled at sort
        // time by leaving the more-important entry first — for now we keep both and the
        // existing binary search returns the lowest-index match; a future polish can sort
        // with @c important first).
        MergedDispatchSnapshot* BuildAppended(const MergedDispatchSnapshot* prior,
                                              const DispatchEntry& entry) noexcept {
            const std::size_t priorN = prior ? prior->count : 0;
            std::vector<DispatchEntry> built;
            built.reserve(priorN + 1);
            bool replaced = false;
            for (std::size_t i = 0; i < priorN; ++i) {
                if (!replaced
                    && prior->entries[i].iid == entry.iid
                    && prior->entries[i].providerClass == entry.providerClass) {
                    built.push_back(entry);
                    replaced = true;
                } else {
                    built.push_back(prior->entries[i]);
                }
            }
            if (!replaced) built.push_back(entry);

            std::sort(built.begin(), built.end(),
                      [](const DispatchEntry& a, const DispatchEntry& b) {
                          if (a.iid != b.iid) return a.iid < b.iid;
                          // Important wins at sort time so @ref LookupMergedDispatch's
                          // first-of-group result is the Important one (D7.3).
                          return a.seal.important > b.seal.important;
                      });

            const std::size_t n = built.size();
            auto* entries = new DispatchEntry[n];
            for (std::size_t i = 0; i < n; ++i) entries[i] = built[i];
            return new MergedDispatchSnapshot{ n, entries };
        }

        // Single-class update: install @p entry into @p links->mergedDispatch + bump epoch.
        // Caller holds @p links->writerMu.
        void PublishAt(MetaLinks* links, const DispatchEntry& entry) noexcept {
            const MergedDispatchSnapshot* prior =
                links->mergedDispatch.load(std::memory_order_acquire);
            MergedDispatchSnapshot* fresh = BuildAppended(prior, entry);

            links->mergedDispatch.store(fresh, std::memory_order_release);
            links->BumpCacheEpoch();

            // Broadcast L3 invalidation downstream (matches Install path).
            const ExtendedListSnapshot* downs =
                links->extendedBy.load(std::memory_order_acquire);
            BroadcastInvalidation(downs);

            // Retire the prior snapshot + its entries[] backing array via epoch-RCU.
            if (prior) {
                RetireSnapshot(const_cast<DispatchEntry*>(prior->entries), &DeleteEntryArray);
                RetireSnapshot(const_cast<MergedDispatchSnapshot*>(prior), &DeleteMergedSnapshot);
            }
        }

        // Recursive direct-subclass walk. Lock acquisition order is the iid-sort order of
        // @c subclassedBy (§5.4) — both this call AND any concurrent install on a sibling
        // base sharing a subclass acquire the subclass's mutex in the same global order.
        void PropagateToSubclasses(MetaLinks* implLinks, const DispatchEntry& entry) noexcept {
            const SubclassSnapshot* subs =
                implLinks->subclassedBy.load(std::memory_order_acquire);
            if (!subs) return;
            for (std::size_t i = 0; i < subs->count; ++i) {
                const MetaCore* subCore = subs->data[i];
                if (!subCore) continue;
                // subclassedBy stores MetaCore*, but we need the matching MetaLinks*. They
                // pair via @c gLinksFor<T> at the call site of @c Install<T>(); we look the
                // links up indirectly through @ref MetaDynamicOf — but here we only have the
                // MetaCore. Resolution: subCore->iid identifies the class, but the
                // (subCore -> subLinks) mapping is currently only accessible through the
                // @c gLinksFor<T> variable template. For A3 we hand the pairing back via
                // a side-channel registered at Install time — see SubclassLinkRegistry.
                MetaLinks* subLinks = SubclassLinksFor(subCore);
                if (!subLinks) continue;

                std::lock_guard<std::mutex> g(subLinks->writerMu);
                PublishAt(subLinks, entry);
                PropagateToSubclasses(subLinks, entry);
            }
        }

    }  // namespace

    void RegisterArmAt(const MetaCore* implCore, MetaLinks* implLinks,
                       DispatchEntry entry) noexcept {
        if (!implCore || !implLinks) return;
        entry.providerClass = implCore;

        std::lock_guard<std::mutex> g(implLinks->writerMu);
        PublishAt(implLinks, entry);
        PropagateToSubclasses(implLinks, entry);
        (void)TryReclaim();
    }

}  // namespace Yuki::Detail

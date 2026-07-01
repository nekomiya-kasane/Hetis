/**
 * @file Registry.cpp
 * @brief Runtime install kernel — D7.2 cross-module seal checks, atomic publish, L3 broadcast,
 *        epoch-RCU retirement (A3), and subclass-edge maintenance (D16).
 *
 * Implements @ref Yuki::Detail::InstallKernel: serialises writers on
 * @c links->writerMu, performs the Final / Unique / Important cross-module check against
 * the prior live snapshot, builds a fresh iid-sorted @ref DispatchSnapshot +
 * @ref MergedDispatchSnapshot, publishes both under release ordering, bumps cacheEpoch,
 * broadcasts L3 invalidation, and retires the old snapshots through @ref RetireSnapshot
 * (A3 replaces A2's unbounded @c RetiredPool).
 *
 * Also exposes @ref Yuki::Detail::AppendSubclassToBase, called from @c Registry::Install<T>()
 * for each Y_OBJECT base of @c T to populate the D16 reverse-edge.
 *
 * @ingroup Core
 */
#include <Yuki/Core/Registry.h>
#include <Yuki/Core/EpochRcu.h>
#include <Yuki/Core/ExtendedList.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace Yuki::Detail {

    namespace {
        void DeleteDispatchEntryArray(void* p) noexcept {
            delete[] static_cast<DispatchEntry*>(p);
        }
        void DeleteDispatchSnapshot(void* p) noexcept {
            delete static_cast<DispatchSnapshot*>(p);
        }
        void DeleteMergedDispatchSnapshot(void* p) noexcept {
            delete static_cast<MergedDispatchSnapshot*>(p);
        }
        void DeleteSubclassSnapshot(void* p) noexcept {
            // Single allocation: header + trailing const MetaCore* array. See AllocSubclassSnapshot.
            ::operator delete(p);
        }

        SubclassSnapshot* AllocSubclassSnapshot(std::size_t count) noexcept {
            const std::size_t bytes = sizeof(SubclassSnapshot) + count * sizeof(const MetaCore*);
            void* mem = ::operator new(bytes);
            auto* snap = new (mem) SubclassSnapshot{};
            snap->count = count;
            snap->data =
                reinterpret_cast<const MetaCore* const*>(static_cast<unsigned char*>(mem) + sizeof(SubclassSnapshot));
            return snap;
        }
    } // namespace

    void InstallKernel(const MetaCore* core, MetaLinks* links, const ImplementsInfo* implements,
                       std::size_t implementsCount) noexcept {
        // T23 §5.3 side-channel: publish the (MetaCore*, MetaLinks*) pair before any locking, so
        // RegisterArmAt's subclass walk can resolve subclassedBy entries (which store only
        // MetaCore*) back to their writable MetaLinks*. Idempotent.
        RegisterCoreLinkPair(core, links);

        std::lock_guard<std::mutex> g(links->writerMu);

        // 1) Read the live snapshot (may be null on first install).
        const DispatchSnapshot* prior = links->dispatch.load(std::memory_order_acquire);

        // 2) Per-info seal check (D7.2). On violation, abort with a diagnostic.
        if (prior) {
            for (std::size_t k = 0; k < implementsCount; ++k) {
                const ImplementsInfo& info = implements[k];
                const DispatchEntry* match = nullptr;
                for (std::size_t j = 0; j < prior->count; ++j) {
                    if (prior->entries[j].iid == info.iid) {
                        match = &prior->entries[j];
                        break;
                    }
                }
                if (!match) {
                    continue;
                }
                if (match->providerClass == core) {
                    continue; // benign re-install.
                }
                if (match->seal.final) {
                    std::abort(); // D7 Final.
                }
                if (info.flags.unique) {
                    std::abort(); // D7 Unique.
                }
                if (match->seal.important && info.flags.important) {
                    std::abort(); // D7.2.
                }
            }
        }

        // 3) Build a fresh iid-sorted entry array (own count + carry-over of prior entries
        //    whose providerClass is not @p core — those are replaced by this install).
        std::vector<DispatchEntry> built;
        built.reserve(implementsCount + (prior ? prior->count : 0));
        for (std::size_t k = 0; k < implementsCount; ++k) {
            const ImplementsInfo& info = implements[k];
            built.push_back(DispatchEntry{
                .iid = info.iid,
                .kind = DispatchKind::InlineFacade, // refined per-arm by A3 register paths.
                .seal = info.flags,
                .armOffset = 0,
                .providerClass = core,
                .arm = nullptr,
            });
        }
        if (prior) {
            for (std::size_t j = 0; j < prior->count; ++j) {
                if (prior->entries[j].providerClass == core) {
                    continue; // replaced by this install.
                }
                built.push_back(prior->entries[j]);
            }
        }
        std::sort(built.begin(), built.end(),
                  [](const DispatchEntry& a, const DispatchEntry& b) { return a.iid < b.iid; });

        // 4) Move the built vector into program-lifetime storage (raw new; retired via EpochRcu).
        const std::size_t n = built.size();
        auto* entries = new DispatchEntry[n];
        for (std::size_t i = 0; i < n; ++i) {
            entries[i] = built[i];
        }

        auto* snap = new DispatchSnapshot{n, entries};
        auto* merged = new MergedDispatchSnapshot{n, entries};

        // 5) Capture prior snapshot pointers for retirement, then publish under release ordering.
        const DispatchSnapshot* priorDispatch = prior;
        const MergedDispatchSnapshot* priorMerged = links->mergedDispatch.load(std::memory_order_acquire);
        links->dispatch.store(snap, std::memory_order_release);
        links->mergedDispatch.store(merged, std::memory_order_release);

        // 6) Bump epoch and broadcast invalidation.
        links->BumpCacheEpoch();
        const ExtendedListSnapshot* downs = links->extendedBy.load(std::memory_order_acquire);
        BroadcastInvalidation(downs);

        // 7) Retire prior snapshots via epoch-RCU. RetireSnapshot bumps gGlobalEpoch.fetch_add(1)
        //    AFTER the release-stores above, establishing the happens-before edge that lets
        //    TryReclaim safely free pointers whose stamp < min(reader.epoch).
        if (priorDispatch) {
            // The entries[] array backing priorDispatch is also referenced by priorMerged (A2 + A3
            // both publish the same array as the merged view), so the entries[] retirement is
            // single-owner: tied to priorDispatch.
            RetireSnapshot(const_cast<DispatchEntry*>(priorDispatch->entries), &DeleteDispatchEntryArray);
            RetireSnapshot(const_cast<DispatchSnapshot*>(priorDispatch), &DeleteDispatchSnapshot);
        }
        if (priorMerged && priorMerged != reinterpret_cast<const MergedDispatchSnapshot*>(priorDispatch)) {
            RetireSnapshot(const_cast<MergedDispatchSnapshot*>(priorMerged), &DeleteMergedDispatchSnapshot);
        }

        // 8) Opportunistic reclamation (§4.6).
        (void)TryReclaim();
    }

    /// @brief T23 / D16: append @p subCore to @p baseLinks->subclassedBy via copy-on-write.
    ///        Idempotent: if @p subCore is already present, no-op. Caller does NOT need to
    ///        hold @c baseLinks->writerMu; this function acquires it.
    void AppendSubclassToBase(MetaLinks* baseLinks, const MetaCore* subCore) noexcept {
        if (!baseLinks || !subCore) {
            return;
        }
        std::lock_guard<std::mutex> g(baseLinks->writerMu);

        const SubclassSnapshot* prior = baseLinks->subclassedBy.load(std::memory_order_acquire);
        const std::size_t priorCount = prior ? prior->count : 0;

        // Idempotency check.
        for (std::size_t i = 0; i < priorCount; ++i) {
            if (prior->data[i] == subCore) {
                return;
            }
        }

        // Build new snapshot with subCore appended, iid-sorted for deterministic walk order
        // (§5.4 lock ordering).
        SubclassSnapshot* snap = AllocSubclassSnapshot(priorCount + 1);
        auto* mutData = const_cast<const MetaCore**>(snap->data);
        std::size_t w = 0;
        bool inserted = false;
        for (std::size_t i = 0; i < priorCount; ++i) {
            if (!inserted && subCore->iid < prior->data[i]->iid) {
                mutData[w++] = subCore;
                inserted = true;
            }
            mutData[w++] = prior->data[i];
        }
        if (!inserted) {
            mutData[w++] = subCore;
        }

        baseLinks->subclassedBy.store(snap, std::memory_order_release);
        baseLinks->BumpCacheEpoch();
        if (prior) {
            RetireSnapshot(const_cast<SubclassSnapshot*>(prior), &DeleteSubclassSnapshot);
        }
        (void)TryReclaim();
    }

} // namespace Yuki::Detail

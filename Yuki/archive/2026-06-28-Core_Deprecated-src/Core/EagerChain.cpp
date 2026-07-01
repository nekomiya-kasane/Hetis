#include <Yuki/Core/EagerChain.h>
#include <Yuki/Core/Config.h>
#include <Yuki/Core/EpochRcu.h>
#include <Yuki/Core/MetaDynamic.h>
#include <Yuki/Core/MetaLinks.h>
#include <Yuki/Core/TaggedPayload.h>

#include <cassert>
#include <mutex>

namespace Yuki {

    namespace {
        // Force the refcount field of a TaggedPayload word to a specific value while
        // preserving role/everAcquired/armPtr bits.
        //
        // kDebug sentinel assert: if the current word's refcount field already holds
        // TaggedPayload::kExternalSentinel (0xFFFF), the RootObject was constructed with
        // external=true and its lifetime is externally managed. Silently overwriting the
        // sentinel here would demolish external-lifetime tracking and cause subsequent
        // Release attempts to free externally-owned memory. Mirrors the saturation-assert
        // pattern used by Acquire/Release in RootObject.cpp.
        void StoreRefcount(std::atomic<TaggedPayload>& a, std::uint16_t rc) noexcept {
            for (auto cur = a.load(std::memory_order_relaxed);;) {
                if constexpr (kDebug) {
                    assert(cur.refcount() != TaggedPayload::kExternalSentinel
                           && "StoreRefcount: refusing to clobber kExternalSentinel; "
                              "RootObject must be constructed external=false to participate "
                              "in the eager chain");
                }
                TaggedPayload nxt = cur;
                nxt.word = (cur.word & ~(0xFFFFull << 4))
                         | ((std::uint64_t(rc) & 0xFFFF) << 4);
                if (a.compare_exchange_weak(cur, nxt,
                        std::memory_order_acq_rel, std::memory_order_relaxed)) return;
            }
        }
    } // namespace

    void ParkEager(RootObject* ext) noexcept {
        if (!ext) return;
        StoreRefcount(ext->MetaWord(), 0);
    }

    void HotAcquireEager(RootObject* ext, RootObject* extendee) noexcept {
        if (!ext) return;
        // kDebug precondition: ext must currently be parked (refcount == 0). Violation
        // double-Acquires extendee on the next park/hot cycle and leaks an extendee ref.
        if constexpr (kDebug) {
            assert(ext->PayloadRelaxed().refcount() == 0
                   && "HotAcquireEager: ext must be parked (refcount == 0) on entry");
        }
        StoreRefcount(ext->MetaWord(), 1);
        Acquire(extendee);
    }

    void DetachEagerOnRelease(RootObject* ext, RootObject* extendee) noexcept {
        if (!ext) return;
        // kDebug precondition: ext's user-side refcount must be 1 on entry (the call site
        // is the last-Release path). Violation under-counts extendee refs (a Release that
        // was never paired with a HotAcquire) and may prematurely free the extendee.
        if constexpr (kDebug) {
            assert(ext->PayloadRelaxed().refcount() == 1
                   && "DetachEagerOnRelease: ext must have refcount == 1 on entry");
        }
        StoreRefcount(ext->MetaWord(), 0);
        (void)Release(extendee);
    }

    namespace {
        // EagerSetSnapshot is published as a single allocation: { header, trailing
        // RootObject* array }. Mirrors AllocSubclassSnapshot's layout in Registry.cpp so
        // the walker only needs one deleter and there is no separate array buffer to leak.
        void DeleteEagerSetSnapshot(void* p) noexcept {
            ::operator delete(p);
        }
    }

    void DeleteParkedEagers(RootObject* nucleus) noexcept {
        if (!nucleus) return;
        const MetaDynamic& md = nucleus->MetaDyn();
        MetaLinks* links = md.links;
        if (!links) return;

        std::lock_guard<std::mutex> g(links->writerMu);

        const EagerSetSnapshot* snap = links->eagerSet.load(std::memory_order_acquire);
        if (!snap) return;

        for (std::size_t i = 0; i < snap->count; ++i) {
            RootObject* parked = snap->parked[i];
            if (!parked) continue;
            if constexpr (kDebug) {
                assert(parked->PayloadRelaxed().refcount() == 0
                       && "DeleteParkedEagers: eager extension still has refcount > 0 at "
                          "nucleus dtor — D11 invariant violated (a live ext should keep "
                          "the extendee alive)");
            }
            delete parked;
        }

        links->eagerSet.store(nullptr, std::memory_order_release);
        RetireSnapshot(const_cast<EagerSetSnapshot*>(snap), &DeleteEagerSetSnapshot);
    }

    namespace Detail {
        EagerSetSnapshot* AllocEagerSetSnapshot(std::size_t count) noexcept {
            const std::size_t bytes = sizeof(EagerSetSnapshot) + count * sizeof(RootObject*);
            void* mem = ::operator new(bytes);
            auto* snap = new (mem) EagerSetSnapshot{};
            snap->count  = count;
            snap->parked = reinterpret_cast<RootObject* const*>(
                static_cast<unsigned char*>(mem) + sizeof(EagerSetSnapshot));
            return snap;
        }
    }

}  // namespace Yuki // namespace Yuki

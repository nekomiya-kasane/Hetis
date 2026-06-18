#include <Yuki/Core/EagerChain.h>
#include <Yuki/Core/TaggedPayload.h>

namespace Yuki {

    namespace {
        // Force the refcount field of a TaggedPayload word to a specific value while
        // preserving role/everAcquired/armPtr bits.
        void StoreRefcount(std::atomic<TaggedPayload>& a, std::uint16_t rc) noexcept {
            for (auto cur = a.load(std::memory_order_relaxed);;) {
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
        StoreRefcount(ext->MetaWord(), 1);
        Acquire(extendee);
    }

    void DetachEagerOnRelease(RootObject* ext, RootObject* extendee) noexcept {
        if (!ext) return;
        StoreRefcount(ext->MetaWord(), 0);
        (void)Release(extendee);
    }

} // namespace Yuki

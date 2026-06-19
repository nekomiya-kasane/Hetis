#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/Config.h>
#include <Yuki/Core/EagerChain.h>
#include <Yuki/Core/MetaDynamic.h>
#include <Yuki/Core/TaggedPayload.h>

#include <cassert>

namespace Yuki {

    RootObject::~RootObject() noexcept = default;

    namespace {
        // Sentinel returned by the base RootObject::MetaDyn() for direct-derivations that
        // never installed a class identity (test probes, abstract interface bases without
        // Y_OBJECT). Introspection helpers must guard against null arms.
        constexpr MetaDynamic kBaseSentinelMetaDynamic{ nullptr, nullptr };
    }

    const MetaDynamic& RootObject::MetaDyn() const noexcept {
        return kBaseSentinelMetaDynamic;
    }

    // T12 final-form D8/D9/D10/D13 bodies (NOT temporary — supersedes the T10 forwarders).
    //
    // Null tolerance: every call site that bumps a "maybe-null" handle (e.g., a Facadeish-style
    // facade ctor when the underlying may be uninitialised) benefits from the early-return —
    // saves the caller a branch. Acquire(nullptr) is a no-op; Release(nullptr) returns false.
    //
    // kDebug saturation assert: TryIncrement returns false only when the refcount has reached
    // kSaturationLimit (0xFFFE). Hitting that in normal operation indicates an unbounded-acquire
    // bug (a forgotten Release somewhere). Debug builds fire assert; release builds silently
    // saturate so the pointer is never freed prematurely.

    void Acquire(RootObject* p) noexcept {
        if (!p) return;
        const bool ok = TaggedPayload::TryIncrement(p->MetaWord());
        if constexpr (kDebug) { assert(ok && "Acquire saturated"); }
        (void)ok;
    }

    bool Release(RootObject* p) noexcept {
        if (!p) return false;
        // Cleanup MUST run at the 1->0 transition (before ~RootObject), while p's dynamic type
        // is still intact. Doing it from ~RootObject is wrong: during base-class destruction the
        // C++ runtime demotes the vtable to RootObject, so MetaDyn() resolves to the sentinel
        // {nullptr, nullptr} and DeleteParkedEagers becomes a no-op. Here the derived vtable is
        // still live, so MetaDyn() returns the real per-class slot and the parked snapshot is
        // reachable. Only Implementation roles own eager extensions; facades/extensions skip.
        const bool transitioned = TaggedPayload::TryDecrement(p->MetaWord());
        if (transitioned && p->TypeDynamic() == ClassType::Implementation) {
            DeleteParkedEagers(p);
        }
        return transitioned;
    }

}

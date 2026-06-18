#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/Config.h>
#include <Yuki/Core/TaggedPayload.h>

#include <cassert>

namespace Yuki {

    RootObject::~RootObject() noexcept = default;

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
        return TaggedPayload::TryDecrement(p->MetaWord());
    }

}

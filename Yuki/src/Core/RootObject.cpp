#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/TaggedPayload.h>

namespace Yuki {

    RootObject::~RootObject() noexcept = default;

    // T10 temporary forwarders — thin wrappers over TaggedPayload CAS loops.
    // Task 12 (D8/D9/D10/D13) will replace these bodies with hierarchical semantics:
    // facade/arm coalescing, external-lifetime propagation, and sub-object deallocation paths.
    // TryIncrement already handles the external-sentinel no-op and saturation guard.
    // TryDecrement returns true iff the decrement transitioned refcount to 0,
    // which ComPtr::~ComPtr uses as the gate for `delete`.

    void Acquire(RootObject* p) noexcept {
        TaggedPayload::TryIncrement(p->MetaWord());
    }

    bool Release(RootObject* p) noexcept {
        return TaggedPayload::TryDecrement(p->MetaWord());
    }

}

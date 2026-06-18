#pragma once
#include <Yuki/Core/Identity.h>
#include <Yuki/Core/TaggedPayload.h>
#include <atomic>

namespace Yuki {
    struct RootObject {
        explicit RootObject(ClassType role, void* arm, bool external) noexcept
          : metaWord_(external
                ? TaggedPayload::MakeExternal(role, arm)
                : TaggedPayload::Make(role, arm, /*rc=*/1, /*ever=*/true))
        {}
        virtual ~RootObject() noexcept;

        RootObject(const RootObject&) = delete;
        RootObject& operator=(const RootObject&) = delete;

        ClassType TypeDynamic() const noexcept {
            return metaWord_.load(std::memory_order_relaxed).role();
        }
        TaggedPayload PayloadRelaxed() const noexcept {
            return metaWord_.load(std::memory_order_relaxed);
        }

        // Acquire/Release defined in Task 12.
        std::atomic<TaggedPayload>& MetaWord() noexcept { return metaWord_; }

      private:
        std::atomic<TaggedPayload> metaWord_;
    };
    static_assert(sizeof(RootObject) == 2 * sizeof(void*));

    /**
     * @brief ComPtr / MakeOwned reference-count hooks — T10 temporary forwarders (D12).
     *
     * @warning These bodies are temporary. Task 12 replaces them with hierarchical
     *          D8/D9/D10/D13 semantics (facade/arm coalescing, external-lifetime
     *          propagation, sub-object deallocation). Do not add logic here — extend
     *          Task 12 instead.
     *
     * Currently thin wrappers over TaggedPayload::TryIncrement / TryDecrement. The
     * TaggedPayload CAS implementation already handles the external-sentinel no-op
     * and saturation correctly, which is why the temp bodies are safe to ship.
     *
     * @param p  Non-null pointer to any RootObject-derived instance.
     * @return   Release returns true iff the decrement transitioned refcount to 0
     *           (caller must delete p).
     */
    void Acquire(RootObject* p) noexcept;
    bool Release(RootObject* p) noexcept;
}

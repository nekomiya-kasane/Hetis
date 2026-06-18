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

        std::atomic<TaggedPayload>& MetaWord() noexcept { return metaWord_; }

      private:
        std::atomic<TaggedPayload> metaWord_;
    };
    static_assert(sizeof(RootObject) == 2 * sizeof(void*));

    /**
     * @brief ComPtr / MakeOwned reference-count hooks — final D8/D9/D10/D13 form.
     *
     * @note Null-tolerant: Acquire(nullptr) is a no-op; Release(nullptr) returns false,
     *       so callers with "maybe-null" handles need no guard branch.
     *       Saturation assert: in kDebug builds, Acquire fires assert if TryIncrement
     *       returns false (refcount at kSaturationLimit = 0xFFFE, indicating an
     *       unbounded-acquire bug). Release builds silently saturate.
     *
     * @param p  Pointer to any RootObject-derived instance; may be null.
     * @return   Release returns true iff the decrement transitioned refcount to 0
     *           (caller must delete p).
     */
    void Acquire(RootObject* p) noexcept;
    [[nodiscard]] bool Release(RootObject* p) noexcept;
}

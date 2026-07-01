#pragma once

#include <Yuki/Core/Identity.h>
#include <Yuki/Core/TaggedPayload.h>
#include <atomic>

namespace Yuki {

    struct MetaDynamic;

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
            return metaWord_.load(std::memory_order_relaxed).Role();
        }
        TaggedPayload PayloadRelaxed() const noexcept {
            return metaWord_.load(std::memory_order_relaxed);
        }

        std::atomic<TaggedPayload>& MetaWord() noexcept { return metaWord_; }

        /// @brief Type-erased access to the instance's MetaDynamic. Y_OBJECT overrides this
        ///        to return the per-class @c MetaDynamicOf<SelfType>. The base default
        ///        returns a sentinel @c {nullptr,nullptr} for direct-derivations without
        ///        Y_OBJECT (test fixtures, abstract interface bases). T21 introspection
        ///        helpers guard against null arms.
        virtual const MetaDynamic& MetaDyn() const noexcept;

        /// @brief T22 — closure up-pointer. The base returns @c nullptr (impls / standalone
        ///        nodes have no upstream). Materialized facade / extension subclasses override
        ///        to return the nucleus they were materialized against; @ref Yuki::Nucleus
        ///        follows this pointer chain to the closure root. Defaulting to @c nullptr
        ///        keeps Impl instances at zero extra storage per the spec §3 "Up-pointer
        ///        storage" note.
        virtual RootObject* Upstream() const noexcept { return nullptr; }

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

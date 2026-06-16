/**
 * @file RootObject.h
 * @brief Polymorphic anchor of the object model — a thin vtable plus a role-armed tagged payload.
 *
 * Every "thing" in the object model derives, transitively, from @ref RootObject. The class is
 * intentionally minimal:
 *
 *   - one virtual destructor;
 *   - one virtual @ref MetaClassDynamic accessor that takes a type-erased pointer to the
 *     most-derived metaclass, in a single vcall;
 *   - one storage word — a *tagged pointer* whose three low bits hold a @ref ClassType and whose
 *     remaining bits encode one of three role-discriminated arms.
 *
 * That is all. There is no built-in refcount (opt-in via `Mashiro::RefCounted{,Atomic}<Self>`),
 * no name string, no IID, no per-instance flags: those live on the metaclass, reached through the
 * one virtual call, so the per-instance footprint stays at two words on every supported ABI.
 *
 * The three payload arms encode the way different roles relate to a "host" object:
 *
 *   - @ref ClassType::Implementation → @c facades_ — head of a runtime-attached @c FacadeList for
 *     interfaces that were not Hot-aggregated at compile time;
 *   - @ref ClassType::Extension      → @c extendee_ — the implementation this extension augments;
 *   - @ref ClassType::Interface, @ref ClassType::Imposter, @ref ClassType::Bridge
 *                                    → @c target_   — the underlying object the facade forwards to.
 *
 * The arm is selected from the three-bit @ref ClassType tag stored *in the payload itself*. There
 * is no separate discriminator field, no `union` with active member tracking, and no virtual
 * "what arm am I?" lookup: a single mask-and-test reaches the role on the QueryInterface hot path.
 *
 * The CRTP injection layers (@ref MetaNode and friends) supply the @c MetaClassDynamic override
 * with no boilerplate and write the payload word once at construction. They reuse the existing
 * vptr; no class in this file ever introduces a second vptr or any per-instance scalar beyond the
 * payload.
 *
 * @ingroup Core
 */
#pragma once

#include <bit>
#include <cstdint>

#include <Yuki/Core/FacadeList.h>
#include <Yuki/Core/MetaClass.h>

namespace Yuki {

    class RootObject; // forward — payload arms refer to RootObject*

    // =========================================================================
    // Tagged payload — the role-armed storage word at the heart of RootObject
    // =========================================================================

    /**
     * @brief A tagged pointer whose low three bits hold a @ref ClassType and whose remaining bits
     *        encode one of @c target_ / @c extendee_ / @c facades_.
     *
     * The encoding exploits the fact that every plausible payload pointer (a @c RootObject*, a
     * @c FacadeListHead*) is at least 8-byte aligned, leaving the low three bits free. The three-bit
     * @ref ClassType space (0–6) fits, so a single `uintptr_t` carries both the role and the
     * pointer with no extra storage.
     *
     * Decoding is a `band`/`bxor`/`bnot` triple, all expressible as constant expressions in the
     * presence of `std::bit_cast`. The class is a literal type so a default-constructed payload
     * (role = @ref ClassType::None, pointer = @c nullptr) participates in the constexpr ctor of
     * @ref RootObject without forcing a runtime initialiser.
     */
    class TaggedPayload {
    public:
        constexpr TaggedPayload() noexcept = default;

        /// @brief Form a payload from @p role and @p ptr. @p ptr must be 8-byte aligned.
        static constexpr TaggedPayload Make(ClassType role, const void* ptr) noexcept {
            const uintptr_t p = std::bit_cast<uintptr_t>(ptr);
            // Contract: every payload-bearing target must satisfy alignof >= 8.
            return TaggedPayload{p | static_cast<uintptr_t>(role)};
        }

        /// @brief The encoded role (low three bits). Total: yields @ref ClassType::None when unset.
        [[nodiscard]] constexpr ClassType Role() const noexcept {
            return static_cast<ClassType>(bits_ & kClassTypeMask);
        }

        /// @brief The encoded pointer with the role bits cleared. @c nullptr when no payload.
        template<typename T = void>
        [[nodiscard]] constexpr T* Pointer() const noexcept {
            const uintptr_t p = bits_ & ~kClassTypeMask;
            return std::bit_cast<T*>(p);
        }

        /// @brief Raw bit pattern — useful for diagnostics and bit-level tests.
        [[nodiscard]] constexpr uintptr_t Bits() const noexcept { return bits_; }

        constexpr bool operator==(const TaggedPayload&) const noexcept = default;

    private:
        explicit constexpr TaggedPayload(uintptr_t b) noexcept : bits_(b) {}
        uintptr_t bits_{0};
    };

    static_assert(sizeof(TaggedPayload) == sizeof(void*));

    // =========================================================================
    // RootObject — the polymorphic anchor
    // =========================================================================

    /**
     * @brief The polymorphic root of every object-model "thing": one vptr + one tagged payload.
     *
     * The class is *not* abstract — its destructor is virtual and defaulted, and
     * @ref MetaClassDynamic is the sole pure virtual. Concrete classes get a no-boilerplate
     * implementation by deriving through @ref MetaNode (or one of its specialisations such as
     * @c IfaceFacadeNode); user code rarely talks to @ref RootObject directly.
     *
     * The two storage words are deliberate: a real type-erased object needs *something* to identify
     * it, and the role-armed payload is the cheapest way to do so without re-introducing per-class
     * runtime registration. The arm and its meaning are determined by the three-bit @ref ClassType
     * stored in the payload — a bitwise decode, no vcall and no field comparison.
     *
     * @c MetaClassDynamic returns the metaclass of the *most-derived* type, which is what scripting,
     * diagnostics, and the dynamic-face @c Query path all want. Static-type sites should reach the
     * metaclass via @ref MetaClassOf instead and pay nothing.
     */
    class RootObject {
    public:
        virtual ~RootObject() noexcept = default;

        /// @brief The metaclass of the *most-derived* type — one virtual dispatch.
        ///        Static-type callers should prefer @ref MetaClassOf.
        [[nodiscard]] virtual const MetaClass& MetaClassDynamic() const noexcept = 0;

        /// @brief The runtime role (bitwise decode from the payload — no vcall).
        [[nodiscard]] ClassType TypeDynamic() const noexcept { return payload_.Role(); }

        /// @brief Read-only view of the role-armed payload word.
        [[nodiscard]] TaggedPayload Payload() const noexcept { return payload_; }

    protected:
        constexpr RootObject() noexcept = default;

        /// @brief Construct with a pre-built payload — used by the CRTP injection layer.
        constexpr explicit RootObject(TaggedPayload p) noexcept : payload_(p) {}

        /// @brief Late-bind the payload (for ctors that compute it after the base subobject runs).
        void SetPayload(TaggedPayload p) noexcept { payload_ = p; }

    private:
        TaggedPayload payload_{};
    };

    static_assert(sizeof(RootObject) == 2 * sizeof(void*),
                  "RootObject must be exactly one vptr plus one tagged payload word.");

    // =========================================================================
    // RT — runtime role-armed accessors over a RootObject pointer
    // =========================================================================

    /**
     * @brief Runtime-side free functions that read a @ref RootObject's payload arm.
     *
     * Grouped under @c RT so callers spell intent at the call site (`RT::Underlying(p)`,
     * `RT::Nucleus(p)`) and so the names never collide with static-side metadata helpers in
     * @c Yuki proper. Every function in this namespace is total: it accepts @c nullptr, accepts any
     * role, and returns @c nullptr when the requested arm is not present — no preconditions, no
     * assertions, no surprises on cold paths.
     */
    namespace RT {

        /**
         * @brief The forward target of an Interface / Imposter / Bridge — a `RootObject*` to the
         *        underlying object the facade was built over. Returns @c nullptr for any other
         *        role, so the function is total over the role space.
         */
        [[nodiscard]] inline RootObject* Target(const RootObject* p) noexcept {
            if (p == nullptr) {
                return nullptr;
            }
            const auto pl = p->Payload();
            switch (pl.Role()) {
            case ClassType::Interface:
            case ClassType::Imposter:
            case ClassType::Bridge:
                return pl.Pointer<RootObject>();
            default:
                return nullptr;
            }
        }

        /**
         * @brief The extendee of an Extension — the @c RootObject* this extension was attached to,
         *        or @c nullptr for any other role.
         */
        [[nodiscard]] inline RootObject* Extendee(const RootObject* p) noexcept {
            if (p == nullptr) {
                return nullptr;
            }
            const auto pl = p->Payload();
            if (pl.Role() == ClassType::Extension) {
                return pl.Pointer<RootObject>();
            }
            return nullptr;
        }

        /**
         * @brief The runtime facade chain head of an Implementation, or @c nullptr for any other
         *        role. Implementations that never need a runtime-attached interface have a null
         *        chain head, so @ref Query's cold path becomes a single null check.
         */
        [[nodiscard]] inline FacadeListHead* Facades(const RootObject* p) noexcept {
            if (p == nullptr) {
                return nullptr;
            }
            const auto pl = p->Payload();
            if (pl.Role() == ClassType::Implementation) {
                return pl.Pointer<FacadeListHead>();
            }
            return nullptr;
        }

        /**
         * @brief Walk through Interface/Imposter/Bridge facades to the bottom underlying object.
         *
         * Returns @p p itself when @p p already names an Implementation/Extension/None (or is
         * null), so the function is idempotent — callers do not need to know whether they were
         * handed a facade. Used by @c IsEqual, diagnostics, and any code that wants a stable
         * identity for an object regardless of which facade it was reached through.
         */
        [[nodiscard]] inline RootObject* Underlying(const RootObject* p) noexcept {
            while (RootObject* t = Target(p)) {
                p = t;
            }
            return const_cast<RootObject*>(p);
        }

        /**
         * @brief The nucleus of @p p — the underlying Implementation that ultimately backs it.
         *
         * The walk has three transparent steps composed in a loop:
         *   1. follow any Interface/Imposter/Bridge facade through @ref Underlying,
         *   2. if the result is an Extension, hop to its @ref Extendee,
         *   3. otherwise stop.
         *
         * Iteration (not recursion) keeps the function bounded: a tagged-payload object cannot form
         * a cycle in a well-formed object model, and the @ref Underlying step strictly drops one or
         * more facade layers before each Extension hop. Total over the role space: returns @c p
         * unchanged for @ref ClassType::Implementation and @ref ClassType::None, returns @c nullptr
         * for @c nullptr — never recurses past a non-Extension, non-facade arm, so a stray
         * @ref ClassType::None can never spin the call stack.
         */
        [[nodiscard]] inline RootObject* Nucleus(const RootObject* p) noexcept {
            for (;;) {
                p = Underlying(p);
                if (p == nullptr || p->TypeDynamic() != ClassType::Extension) {
                    return const_cast<RootObject*>(p);
                }
                p = Extendee(p);
            }
        }

    } // namespace RT

    // =========================================================================
    // CRTP injection layers — supply MetaClassDynamic and the payload, no boilerplate
    // =========================================================================

    /** @cond INTERNAL */
    namespace Detail {

        /**
         * @brief Bottom anchor of every @ref ClassType::Implementation chain — holds the single
         *        @ref FacadeListHead and sets the payload arm exactly once.
         *
         * Object-model inheritance chains (e.g. `SpecialCircle : MetaNode<SpecialCircle, CircleImpl>`)
         * pass through one @ref MetaNode per level. Putting @c facades_ inside @ref MetaNode would
         * give every level its own copy of the head and waste memory at every chain step. Instead,
         * the chain bottoms out at this anchor, so a chain of any depth has *one* facade head — the
         * payload pointer set here is correct for the most-derived object because @c facades_ lives
         * at the same offset regardless of the derived type running construction.
         */
        struct alignas(8) ImplAnchorBase : RootObject {
            FacadeListHead facades_;

            ImplAnchorBase() noexcept
                : RootObject(TaggedPayload::Make(ClassType::Implementation, &facades_)) {}
        };

        /// @brief Bottom anchor of every @ref ClassType::Extension chain — sets the extendee arm.
        struct alignas(8) ExtensionAnchorBase : RootObject {
            explicit ExtensionAnchorBase(RootObject* extendee) noexcept
                : RootObject(TaggedPayload::Make(ClassType::Extension, extendee)) {}
        };

    } // namespace Detail
    /** @endcond */

    /**
     * @brief CRTP base for an @ref Anno::Implementation — supplies @ref MetaClassDynamic with no
     *        boilerplate.
     *
     * @tparam Self The most-derived implementation class (CRTP self type).
     * @tparam Base The C++ base — defaults to the per-impl anchor that bottoms out at
     *              @ref RootObject. Pass another @c MetaNode-derived class to chain object-model
     *              inheritance; the chain re-uses the bottom anchor's facade head, so a chain of
     *              any depth still pays for exactly one @ref FacadeListHead.
     */
    template<ImplementationClass Self, typename Base = Detail::ImplAnchorBase>
    struct MetaNode : Base {
        using Base::Base;

        [[nodiscard]] const MetaClass& MetaClassDynamic() const noexcept override { return MetaClassOf<Self>; }
    };

    /**
     * @brief CRTP base for an @ref Anno::Interface façade — supplies @ref MetaClassDynamic and
     *        wires the forward target into the payload arm.
     *
     * The compile-time analogue of a CATIA TIE: a small subobject that *truly* derives from
     * @p Iface and forwards every virtual through @c target_. The static-face @ref Query folds
     * the cast to a `static_cast`; the dynamic-face Query goes through @ref MetaClassDynamic.
     *
     * @tparam Self  The most-derived facade class.
     * @tparam Iface The interface this facade implements.
     * @tparam Impl  The host the facade forwards to.
     */
    template<typename Self, InterfaceClass Iface, typename Impl>
    struct alignas(8) IfaceFacadeNode : Iface {
        Impl* target_;

        explicit IfaceFacadeNode(Impl* t) noexcept : target_(t) {
            this->SetPayload(TaggedPayload::Make(ClassType::Interface, t));
        }

        [[nodiscard]] const MetaClass& MetaClassDynamic() const noexcept override { return MetaClassOf<Self>; }

        /// @brief Same-host equality on the back pointer — useful for facade-vs-facade identity.
        [[nodiscard]] constexpr bool SameHost(const IfaceFacadeNode& o) const noexcept { return target_ == o.target_; }
    };

    /**
     * @brief CRTP base for an @ref Anno::Extension — encodes the extendee in the payload arm.
     *
     * @tparam Self The most-derived extension class.
     * @tparam Extendee The implementation this extension augments.
     * @tparam Base The C++ base — defaults to the per-extension anchor that bottoms at
     *              @ref RootObject. Pass another @c ExtensionNode-derived class to chain
     *              object-model inheritance.
     */
    template<ExtensionClass Self, typename Extendee, typename Base = Detail::ExtensionAnchorBase>
    struct ExtensionNode : Base {
        explicit ExtensionNode(Extendee* extendee) noexcept : Base(extendee) {}

        [[nodiscard]] const MetaClass& MetaClassDynamic() const noexcept override { return MetaClassOf<Self>; }
    };

} // namespace Yuki

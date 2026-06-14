/**
 * @file RootObject.h
 * @brief The object-model root anchor and the CRTP injection layer that reaches its metaclass.
 *
 * @ref RootObject is the degenerate root of every "thing" in the object model: a pure polymorphic
 * anchor carrying nothing but the ability to reach its own @ref MetaClass. Compared to the legacy
 * `CATBaseUnknown`, it drops the `IDispatch` base and the eight virtuals
 * (`QueryInterface`/`AddRef`/`Release`/`GetMetaObject`/`IsA`/`IsAKindOf`/`IsEqual`) plus the
 * per-instance `m_cRef` / `NecessaryData` / `delegate` / `m_reserved` — about 24 bytes — leaving a
 * single vptr.
 *
 * @ref MetaNode is the CRTP injection layer: it supplies the one `DynamicMetaClass()` override with
 * zero hand-written boilerplate and **zero per-instance storage**, reusing the existing vptr. There
 * are two reach paths to a metaclass:
 *   - **non-virtual** (static type known): `MetaClassOf<T>` — zero cost, folds at compile time.
 *   - **virtual** (type erased through `RootObject*`): `p->DynamicMetaClass()` — one vcall.
 *
 * @ref MetaNode's @c Base template parameter expresses both the C++ base class and the object-model
 * inheritance edge (`omBase`); the most-derived override wins.
 *
 * @ingroup Core
 */
#pragma once

#include <Yuki/Core/MetaClass.h>

namespace Yuki {

    /**
     * @brief The polymorphic root of every object-model "thing" — zero data members, one vptr.
     *
     * Naming: the legacy `RootClass` is superseded by @c RootObject. The root of a *thing* is an
     * Object; @ref MetaClass is the "class". @ref DynamicMetaClass is the sole type-erasure entry —
     * given any `RootObject*`, it returns the most-derived type's metaclass through the vptr.
     */
    class RootObject {
    public:
        virtual ~RootObject() = default;

        /// @brief The metaclass of this instance's most-derived type (one virtual dispatch).
        [[nodiscard]] virtual const MetaClass& DynamicMetaClass() const noexcept = 0;
    };

    /**
     * @brief CRTP injection layer: supplies @ref RootObject::DynamicMetaClass for @p Self with no
     *        hand-written override and no per-instance storage (reuses the existing vptr).
     *
     * @tparam Self The most-derived class being defined (the CRTP self type).
     * @tparam Base The C++ base class, which also expresses the object-model `omBase` edge. Defaults
     *              to @ref RootObject; pass another @c MetaNode-derived class to chain object-model
     *              inheritance. The most-derived @c MetaNode in the chain wins the override.
     *
     * @code
     * struct [[=Anno::Meta{.type = ClassType::Implementation}]] CircleImpl
     *     : MetaNode<CircleImpl> { double radius; };
     * @endcode
     */
    template<typename Self, typename Base = RootObject>
    struct MetaNode : Base {
        using Base::Base;

        [[nodiscard]] const MetaClass& DynamicMetaClass() const noexcept override {
            return MetaClassOf<Self>;
        }
    };

} // namespace Yuki

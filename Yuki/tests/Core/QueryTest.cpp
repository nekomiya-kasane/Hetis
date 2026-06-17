/**
 * @file QueryTest.cpp
 * @brief Tests for the QueryInterface foundation: FacadeKind annotation, DispatchKind discriminator,
 *        InterfaceFacade CRTP base, and the static-face DirectCast Query<I>.
 *
 * Covers the parts that land before per-impl facade aggregation: that an interface's storage
 * policy is read off Anno::InterfaceTraits with FacadeKind::Hot as the default; that the static
 * `Query<I>` folds to a `static_cast` when the impl truly inherits the interface, and to nullptr
 * otherwise; that overloaded interface methods reach the right host overload through a facade.
 */
#include <Yuki/Core/InterfaceFacade.h>
#include <Yuki/Core/MetaClass.h>
#include <Yuki/Core/Query.h>
#include <Yuki/Core/Registry.h>
#include <Yuki/Core/RootObject.h>

#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <cstddef>

using namespace Yuki;

namespace {

    // --- Two interfaces: one Hot (default), one Cold via InterfaceTraits -----

    struct [[=Anno::Interface]] IShape {
        virtual double Area() const = 0;
        virtual double Perimeter(double scale) const = 0;
        virtual double Perimeter(int sides) const = 0;  // overloaded sibling
        virtual ~IShape() = default;
    };

    struct [[=Anno::Interface]]
           [[=Anno::InterfaceTraits{.facade_kind = FacadeKind::Cold}]] IRareInspect {
        virtual int Inspect() const = 0;
        virtual ~IRareInspect() = default;
    };

    // --- A facade for IShape, written the way interface authors will write theirs.

    template<typename Impl>
    struct IShapeFacade final : InterfaceFacade<IShape, Impl> {
        using Base = InterfaceFacade<IShape, Impl>;
        using Base::Base;
        using Base::target_;

        double Area() const override                  { return target_->Area(); }
        double Perimeter(double s) const override     { return target_->Perimeter(s); }
        double Perimeter(int n) const override        { return target_->Perimeter(n); }
    };

    // --- Two impl flavours: one truly multi-inherits IShape (DirectCast),
    //     one is plain (no inheritance, exercised through a facade subobject).

    struct [[=Anno::Implementation]] [[=Anno::Implements{^^IShape}]]
           DirectCircle : IShape {
        double radius{2.0};

        double Area() const override                  { return 3.14159 * radius * radius; }
        double Perimeter(double s) const override     { return 2 * 3.14159 * radius * s; }
        double Perimeter(int n) const override        { return n * radius; }
    };

    struct [[=Anno::Implementation]] PlainSquare {
        double side{3.0};

        double Area() const                           { return side * side; }
        double Perimeter(double s) const              { return 4 * side * s; }
        double Perimeter(int n) const                 { return n * side; }
    };

} // namespace

// =============================================================================
// FacadeKindOf — defaults to Hot, reads InterfaceTraits when present
// =============================================================================

TEST_CASE("FacadeKindOf defaults to Hot for plain Anno::Interface", AUTO_TAG) {
    STATIC_REQUIRE(FacadeKindOf<IShape> == FacadeKind::Hot);
    STATIC_REQUIRE(HotInterface<IShape>);
    STATIC_REQUIRE_FALSE(ColdInterface<IShape>);
}

TEST_CASE("FacadeKindOf reads Cold from Anno::InterfaceTraits", AUTO_TAG) {
    STATIC_REQUIRE(FacadeKindOf<IRareInspect> == FacadeKind::Cold);
    STATIC_REQUIRE(ColdInterface<IRareInspect>);
    STATIC_REQUIRE_FALSE(HotInterface<IRareInspect>);
}

// =============================================================================
// DispatchKind — present on DispatchEntry, defaults to DirectCast
// =============================================================================

TEST_CASE("DispatchEntry carries a DispatchKind, defaulting to DirectCast", AUTO_TAG) {
    STATIC_REQUIRE(DispatchEntry{}.kind == DispatchKind::DirectCast);
    // The legacy top-level `offset` field is gone with Task 3's DispatchSnapshot reshape; the
    // static byte offset now lives inside the payload union (`payload.staticOffset`) and is only
    // meaningful for the DirectCast / InlineFacade kinds, so a default-constructed entry no longer
    // has a "the offset is zero" invariant to assert here.
}

// =============================================================================
// Static-face Query<I> on a typed host
// =============================================================================

TEST_CASE("Query<I> on a true subclass folds to a static_cast", AUTO_TAG) {
    DirectCircle c;
    IShape* s = Query<IShape>(&c);
    REQUIRE(s != nullptr);
    REQUIRE(s == static_cast<IShape*>(&c));
    REQUIRE(s->Area() == c.Area());
    REQUIRE(s->Perimeter(2.0) == c.Perimeter(2.0));
    REQUIRE(s->Perimeter(4) == c.Perimeter(4));
}

TEST_CASE("Query<I> on a non-subclass returns nullptr", AUTO_TAG) {
    PlainSquare p;
    IShape* s = Query<IShape>(&p);
    REQUIRE(s == nullptr);
}

TEST_CASE("Query<I> propagates const-ness", AUTO_TAG) {
    const DirectCircle c;
    const IShape* s = Query<IShape>(&c);
    REQUIRE(s != nullptr);
    REQUIRE(s->Area() == c.Area());
}

// =============================================================================
// InterfaceFacade — the CRTP base really inherits its interface
// =============================================================================

TEST_CASE("InterfaceFacade gives a native IShape* and forwards through target_", AUTO_TAG) {
    PlainSquare p;
    IShapeFacade<PlainSquare> facade{&p};

    IShape* s = &facade;  // real upcast; InterfaceFacade truly inherits IShape.
    REQUIRE(s->Area() == p.Area());
    REQUIRE(s->Perimeter(1.5) == p.Perimeter(1.5));
    REQUIRE(s->Perimeter(7) == p.Perimeter(7));
}

TEST_CASE("Two facades over the same host compare equal by SameHost", AUTO_TAG) {
    PlainSquare p;
    IShapeFacade<PlainSquare> a{&p};
    IShapeFacade<PlainSquare> b{&p};
    REQUIRE(a.SameHost(b));
}

// =============================================================================
// RT::Query<I, C>(C*) — static face folds: BOA → static_cast,
//                       inline-facade → embedded NSDM address.
// =============================================================================
//
// Fixtures here intentionally diverge from the plan literal (which conflates
// `Anno::Implementation` value with a base class and uses a 2-arg facade ctor
// the project does not have). They follow the established T8/T10 pattern from
// RegistryTest.cpp: separate `[[=Anno::*]]` annotations + `MetaNode<Self>` for
// the construction hook, and the inline-facade override body lives on a
// subclass of `InterfaceFacade<I, Impl>` rather than a lambda passed to ctor.

namespace {

    struct [[=Anno::Interface]] ICool {
        virtual int Celsius() const = 0;
        virtual ~ICool() = default;
    };
    struct [[=Anno::Interface]] IHeat {
        virtual int Celsius() const = 0;
        virtual ~IHeat() = default;
    };

    // BOA path: Fridge C++-inherits ICool, so RT::Query<ICool>(&fridge) folds
    // to a single static_cast.
    struct [[=Anno::Implementation]] [[=Anno::Implements{^^ICool}]]
           Fridge : MetaNode<Fridge>, ICool {
        int Celsius() const override { return 4; }
    };

    // Inline-facade path: Stove does NOT inherit IHeat. It carries an inline
    // facade subobject (subclass of InterfaceFacade<IHeat, Stove>) that the
    // static face must locate by reflection over the NSDMs.
    struct Stove;
    struct StoveHeatFacade : InterfaceFacade<IHeat, Stove> {
        using InterfaceFacade::InterfaceFacade;
        int Celsius() const override;  // defined after Stove is complete
    };
    struct [[=Anno::Implementation]] [[=Anno::Implements{^^IHeat}]]
           Stove : MetaNode<Stove> {
        int burnerC = 0;
        StoveHeatFacade _heatFacade{this};
    };
    inline int StoveHeatFacade::Celsius() const { return target_->burnerC; }

} // namespace

TEST_CASE("RT::Query<I> on BOA path folds to static_cast", AUTO_TAG) {
    Fridge f;
    ICool* p = RT::Query<ICool>(&f);
    REQUIRE(p == static_cast<ICool*>(&f));
    REQUIRE(p->Celsius() == 4);
}

TEST_CASE("RT::Query<I> on inline-facade path returns the embedded facade address", AUTO_TAG) {
    Stove s;
    s.burnerC = 180;
    IHeat* p = RT::Query<IHeat>(&s);
    REQUIRE(p != nullptr);
    REQUIRE(p->Celsius() == 180);

    // The pointer must land inside Stove's storage — it is the inline facade
    // subobject, not a heap allocation. The cast through std::byte* avoids
    // any pointer-arithmetic UB on virtual-base offsetting.
    auto* sBegin = reinterpret_cast<std::byte*>(&s);
    auto* sEnd   = sBegin + sizeof(Stove);
    auto* pAddr  = reinterpret_cast<std::byte*>(p);
    REQUIRE(pAddr >= sBegin);
    REQUIRE(pAddr <  sEnd);
    REQUIRE(p == static_cast<IHeat*>(&s._heatFacade));
}

TEST_CASE("RT::Query<I> returns nullptr on a null host", AUTO_TAG) {
    Fridge* f = nullptr;
    REQUIRE(RT::Query<ICool>(f) == nullptr);
    Stove* s = nullptr;
    REQUIRE(RT::Query<IHeat>(s) == nullptr);
}

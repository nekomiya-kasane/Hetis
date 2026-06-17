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

// =============================================================================
// Dynamic kernel (Task 12) — RT::QueryDynamicRaw + RT::Has switch on DispatchKind
// =============================================================================
//
// Spec refs: §4.2 (dynamic kernel arms) and §6.4 (Materialize policy gating the
// SideTableResolver arm). Each scenario uses its own (Implementation, Extension)
// pair so Catch2's random test order cannot turn one test's Install into another
// test's snapshot composition — the same isolation pattern @c RegistryTest uses
// with @c Steak / @c Brisket / @c Ribeye. Fixtures live in this file's anonymous
// namespace and are T12-suffixed to avoid colliding with the same-named symbols
// in other test binaries.

namespace {

    // ---- Singleton arm fixture (CodeExtensionSingleton + Eager + stateless) ----
    struct [[=Anno::Interface]] ICookableSingT12 {
        virtual void Cook() = 0;
        virtual ~ICookableSingT12() = default;
    };
    struct [[=Anno::Implementation]] [[=Anno::Implements{^^ICookableSingT12}]]
           SteakSingT12 : MetaNode<SteakSingT12>, ICookableSingT12 {
        void Cook() override {}
    };
    // Stateless eager — `StatelessExtensionClass` holds, so Install picks the
    // CodeExtensionSingleton arm. The slot itself is `Detail::SingletonAddrFor`,
    // a `RootObject* const = nullptr` sentinel until a later task fills it in.
    struct [[=Anno::Extension]] [[=Anno::Extends{^^SteakSingT12}]]
           [[=Anno::Implements{^^ICookableSingT12}]] [[=Anno::Eager{}]]
           SaltShakerT12 {};

    // ---- Resolver arm fixture (SideTableResolver + lazy stateful) -------------
    struct [[=Anno::Interface]] ICookableLazyT12 {
        virtual void Cook() = 0;
        virtual ~ICookableLazyT12() = default;
    };
    struct [[=Anno::Implementation]] [[=Anno::Implements{^^ICookableLazyT12}]]
           SteakLazyT12 : MetaNode<SteakLazyT12>, ICookableLazyT12 {
        void Cook() override {}
    };
    // Stateful + lazy — `StatefulExtensionClass`, no `Anno::Eager`. Install picks
    // the SideTableResolver arm; the construction hook does not pre-materialise,
    // so the facade chain stays empty until the dynamic kernel's Materialize::Yes
    // path runs the resolver.
    struct [[=Anno::Extension]] [[=Anno::Extends{^^SteakLazyT12}]]
           [[=Anno::Implements{^^ICookableLazyT12}]]
           CookedLazyT12 : ExtensionNode<CookedLazyT12, SteakLazyT12> {
        using ExtensionNode::ExtensionNode;
        int temperatureC = 0;
    };

} // namespace

TEST_CASE("Dynamic kernel resolves DirectCast through a RootObject*", AUTO_TAG) {
    Fridge f;
    RootObject* erased = &f;
    RootObject* hit = RT::QueryDynamicRaw(erased, IidOf<ICool>());
    REQUIRE(hit != nullptr);

    // The DirectCast arm yields `n + StaticCastOffset<Fridge, ICool>()`, which is the byte
    // address of the ICool subobject of @c f. ICool does not derive from RootObject, so a
    // C++ static_cast between the two pointer types is ill-formed — compare addresses
    // through a common @c std::byte* lens instead, the same shape RT::Query's
    // inline-facade test uses.
    auto* expected = reinterpret_cast<std::byte*>(static_cast<ICool*>(&f));
    auto* got      = reinterpret_cast<std::byte*>(hit);
    REQUIRE(got == expected);
}

TEST_CASE("Dynamic kernel CodeExtensionSingleton arm reads the published slot", AUTO_TAG) {
    Registry::Install<SteakSingT12>();
    Registry::Install<SaltShakerT12>();

    // Confirm the published entry for ICookableSingT12 really is the
    // CodeExtensionSingleton arm — guards against this test silently passing
    // when the snapshot composition stops routing the override.
    const auto* snap = MetaClassOf<SteakSingT12>.links()->dispatch.load(
            std::memory_order_acquire);
    const auto* e = Detail::LookupEntry(snap, IidOf<ICookableSingT12>());
    REQUIRE(e != nullptr);
    REQUIRE(e->kind == DispatchKind::CodeExtensionSingleton);
    REQUIRE(e->payload.singleton != nullptr);  // address of SingletonAddrFor<E>

    SteakSingT12 s;
    RootObject* erased = &s;

    // The kernel's CodeExtensionSingleton arm dereferences the slot. T8 fills
    // it with @c Detail::SingletonAddrFor<SaltShakerT12>, a `RootObject* const
    // = nullptr` sentinel (real materialisation lands in a later task), so the
    // returned pointer mirrors the slot exactly. What this test pins down is
    // the kernel's *contract*: whatever the slot holds, the kernel reads it
    // through the indirection, and two consecutive calls agree.
    RootObject* a = RT::QueryDynamicRaw(erased, IidOf<ICookableSingT12>());
    RootObject* b = RT::QueryDynamicRaw(erased, IidOf<ICookableSingT12>());
    REQUIRE(a == b);
    REQUIRE(a == *e->payload.singleton);
}

TEST_CASE("Dynamic kernel SideTableResolver materialises lazy Extension", AUTO_TAG) {
    Registry::Install<SteakLazyT12>();
    Registry::Install<CookedLazyT12>();

    SteakLazyT12 s;
    auto* head = RT::Facades(&s);
    REQUIRE(head != nullptr);

    // Pre-condition: lazy + no Eager + the construction hook has not published a
    // CookedLazyT12 facade for this nucleus yet. The resolver publishes under
    // both the Extension's own iid and each advertised interface iid, so both
    // lookups confirm the pre-state.
    REQUIRE(FacadeListLookup(*head, IidOf<CookedLazyT12>()) == nullptr);
    REQUIRE(FacadeListLookup(*head, IidOf<ICookableLazyT12>()) == nullptr);

    // Confirm the published entry routes through SideTableResolver — only
    // interface iids land on the snapshot, so we drive the kernel through the
    // interface iid the resolver is keyed on.
    const auto* snap = MetaClassOf<SteakLazyT12>.links()->dispatch.load(
            std::memory_order_acquire);
    const auto* e = Detail::LookupEntry(snap, IidOf<ICookableLazyT12>());
    REQUIRE(e != nullptr);
    REQUIRE(e->kind == DispatchKind::SideTableResolver);

    // Materialize::Yes: the SideTableResolver fires and AttachUnique-publishes
    // the per-instance CookedLazyT12 facade onto the chain under both the
    // Extension's iid and the advertised interface iids.
    RootObject* hit = RT::QueryDynamicRaw(&s, IidOf<ICookableLazyT12>());
    REQUIRE(hit != nullptr);

    // Post-condition: the same lookups that were empty above now find the
    // facade, and the kernel's return value matches the published node.
    REQUIRE(FacadeListLookup(*head, IidOf<CookedLazyT12>()) != nullptr);
    REQUIRE(FacadeListLookup(*head, IidOf<ICookableLazyT12>()) == hit);
}

TEST_CASE("RT::Has<I> with Materialize::No does not materialise a lazy Extension", AUTO_TAG) {
    Registry::Install<SteakLazyT12>();
    Registry::Install<CookedLazyT12>();  // stateful + lazy => SideTableResolver entry override

    SteakLazyT12 s;
    auto* head = RT::Facades(&s);
    REQUIRE(head != nullptr);

    // Pre: the lazy Extension hasn't materialised yet, so neither its own iid
    // nor the ICookableLazyT12 iid the override is keyed on resolves through
    // the chain.
    REQUIRE(FacadeListLookup(*head, IidOf<CookedLazyT12>()) == nullptr);
    REQUIRE(FacadeListLookup(*head, IidOf<ICookableLazyT12>()) == nullptr);

    // Confirm the published entry for ICookableLazyT12 really is the
    // SideTableResolver arm — that is the only Materialize-sensitive arm
    // (spec §6.4); a different kind would make this test pass for the wrong
    // reason.
    const auto* snap = MetaClassOf<SteakLazyT12>.links()->dispatch.load(
            std::memory_order_acquire);
    const auto* e = Detail::LookupEntry(snap, IidOf<ICookableLazyT12>());
    REQUIRE(e != nullptr);
    REQUIRE(e->kind == DispatchKind::SideTableResolver);

    // Has<I> reads the facade chain directly under Materialize::No — no
    // resolver call, so the chain stays empty and the answer is `false`. This
    // is the spec §6.4 contract: lazy + unmaterialised answers @c false
    // without side-effecting.
    const auto preHead = head->Head();
    REQUIRE(RT::Has<ICookableLazyT12>(&s) == false);
    REQUIRE(head->Head() == preHead);
    REQUIRE(FacadeListLookup(*head, IidOf<CookedLazyT12>()) == nullptr);
    REQUIRE(FacadeListLookup(*head, IidOf<ICookableLazyT12>()) == nullptr);
}

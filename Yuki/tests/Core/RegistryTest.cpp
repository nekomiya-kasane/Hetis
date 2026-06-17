/**
 * @file RegistryTest.cpp
 * @brief Tests for the process-wide registrar bookkeeping declared in @ref Yuki/Core/Registry.h.
 *
 * Covers the spec §3.3 step 3–4 contract:
 *  - @ref Yuki::Registry::AlreadyInstalled returns @c false for an unseen iid (so first-time
 *    registrar runs proceed).
 *  - @ref Yuki::Registry::MarkInstalled records an iid and is idempotent — re-registering the
 *    same class across TUs or DLLs is a no-op, not a duplicate publish.
 *  - @ref Yuki::Registry::WriterMutexFor hands out one mutex per metaclass, stable across calls,
 *    so concurrent @c Install on *different* classes does not serialize through a single lock.
 *
 * The body of @c Install lands in Tasks 7–8; this skeleton is the bookkeeping it builds on.
 */
#include <Yuki/Core/Registry.h>
#include <Yuki/Core/MetaClass.h>
#include <Yuki/Core/RootObject.h>

#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

using namespace Yuki;

namespace {

    // Two distinct annotated stubs so the WriterMutexFor distinctness check has separate
    // metaclasses to reference. The Anno::Implementation annotation makes them reflectable
    // through MetaCoreOf<T> — the same path Install<T>() will use in Tasks 7–8.
    struct [[=Anno::Implementation]] StubImplA {};
    struct [[=Anno::Implementation]] StubImplB {};

} // namespace

TEST_CASE("AlreadyInstalled is false for an unseen iid", AUTO_TAG) {
    // 0xDEADBEEF is converted to uint128_t via Iid's constexpr ctor — picks a value unlikely
    // to collide with any real registered class even across re-runs of the test binary, since
    // the registry is process-static and tests run sequentially.
    REQUIRE_FALSE(Registry::AlreadyInstalled(Iid{Mashiro::Uuid{0xDEADBEEFu, 0}}));
}

TEST_CASE("MarkInstalled flips AlreadyInstalled to true; idempotent", AUTO_TAG) {
    const Iid id{Mashiro::Uuid{0xCAFEBABEu, 0}};
    Registry::MarkInstalled(id);
    REQUIRE(Registry::AlreadyInstalled(id));

    // Idempotency matters because the same registrar TU can run twice — once in the main exe
    // and once when a plugin DLL with overlapping classes loads in. We tolerate the duplicate
    // call rather than UB or double-publish; assert that the second call is a no-op.
    Registry::MarkInstalled(id);
    REQUIRE(Registry::AlreadyInstalled(id));
}

TEST_CASE("WriterMutexFor returns a stable per-metaclass mutex", AUTO_TAG) {
    // Same metaclass on both calls → must return the same mutex by address, otherwise a writer
    // could grab two different "writer" locks for the same class and race against itself.
    auto& m1 = Registry::WriterMutexFor(MetaClassOf<StubImplA>);
    auto& m2 = Registry::WriterMutexFor(MetaClassOf<StubImplA>);
    REQUIRE(&m1 == &m2);
}

TEST_CASE("WriterMutexFor returns distinct mutexes for distinct metaclasses", AUTO_TAG) {
    // Different metaclasses must get different mutexes — that's the whole point of the per-class
    // mutex map: concurrent Install on unrelated classes runs in parallel rather than fighting
    // over a global lock.
    auto& mA = Registry::WriterMutexFor(MetaClassOf<StubImplA>);
    auto& mB = Registry::WriterMutexFor(MetaClassOf<StubImplB>);
    REQUIRE(&mA != &mB);
}

// =============================================================================
// Install<T> for Implementation classes (Task 7)
// =============================================================================

namespace {

    // Two interfaces this impl will advertise. They follow the existing convention from
    // MetaClassTest / QueryTest: plain interfaces (no RootObject base — that's an impl concern),
    // virtual destructor for clean teardown, one pure method each so the impl has to override.
    struct [[=Anno::Interface]] IBoaA { virtual int A() const = 0; virtual ~IBoaA() = default; };
    struct [[=Anno::Interface]] IBoaB { virtual int B() const = 0; virtual ~IBoaB() = default; };

    // A real Implementation: inherits the MetaNode CRTP base (so it has a MetaClassDynamic vptr)
    // and the two interfaces directly. The Implements annotation is what kImplementsInfos walks —
    // the C++ base list alone is not enough; the registrar reads the annotation.
    struct [[=Anno::Implementation]] [[=Anno::Implements{^^IBoaA, ^^IBoaB}]]
           BoaImpl : MetaNode<BoaImpl>, IBoaA, IBoaB {
        int A() const override { return 1; }
        int B() const override { return 2; }
    };

} // namespace

TEST_CASE("Registry::Install<T> publishes a DispatchSnapshot for an Implementation", AUTO_TAG) {
    Registry::Install<BoaImpl>();

    // links() returns a *pointer* — atomic-load semantics inside MetaClass. After a successful
    // Install, the pointer must be non-null (we just published our per-T static MetaLinks).
    const auto* links = MetaClassOf<BoaImpl>.links();
    REQUIRE(links != nullptr);

    const auto* s = links->dispatch.load(std::memory_order_acquire);
    REQUIRE(s != nullptr);
    REQUIRE(s->count == 2);

    // Both interfaces present in the snapshot with DirectCast — the simplest dispatch arm,
    // resolved by a static_cast offset baked at registration time.
    const auto* eA = Detail::LookupEntry(s, IidOf<IBoaA>());
    REQUIRE(eA != nullptr);
    REQUIRE(eA->kind == DispatchKind::DirectCast);

    const auto* eB = Detail::LookupEntry(s, IidOf<IBoaB>());
    REQUIRE(eB != nullptr);
    REQUIRE(eB->kind == DispatchKind::DirectCast);
}

TEST_CASE("Registry::Install is idempotent across repeated calls", AUTO_TAG) {
    Registry::Install<BoaImpl>();
    const auto* s1 = MetaClassOf<BoaImpl>.links()->dispatch.load(std::memory_order_acquire);

    // Second call short-circuits at the AlreadyInstalled check; the published snapshot pointer
    // must remain identical so concurrent readers do not observe a spurious epoch flip.
    Registry::Install<BoaImpl>();
    const auto* s2 = MetaClassOf<BoaImpl>.links()->dispatch.load(std::memory_order_acquire);
    REQUIRE(s1 == s2);
}

// =============================================================================
// Install<E> for Extension classes (Task 8)
// =============================================================================
//
// Fixtures intentionally diverge from the plan's literal `struct ... : Anno::Extension {}` spelling
// — `Anno::Extension` is a `Role` *value*, not a base class. Roles are stamped via the `[[=Anno::X]]`
// annotation syntax, matching the convention from MetaClassTest.cpp / IdentityTest.cpp.
//
// Each TEST_CASE uses its own implementation + extension types so the shared, process-static
// registry state never carries cross-test pollution. Sharing one Steak across the three tests
// would let the third test's extra Install<>s leak into the first test under Catch2's random
// ordering — and the entry kinds the tests inspect depend on which extension last won the
// snapshot composition.

namespace {

    // -- Fixture A: stateless-extension test -------------------------------------------------
    struct [[=Anno::Interface]] ICookable {
        virtual void Cook() = 0;
        virtual ~ICookable() = default;
    };
    struct [[=Anno::Implementation]] [[=Anno::Implements{^^ICookable}]]
           Steak : MetaNode<Steak>, ICookable {
        void Cook() override {}
    };
    // Stateless Extension: zero own NSDMs => StatelessExtensionClass<SaltShaker> holds, so the
    // registrar picks the CodeExtensionSingleton arm. The singleton-pointer payload is a
    // `nullptr` sentinel until T10's real singleton materialisation; the tests never dereference
    // it, only check the entry kind.
    struct [[=Anno::Extension]] [[=Anno::Extends{^^Steak}]] [[=Anno::Implements{^^ICookable}]]
           [[=Anno::Eager{}]]
           SaltShaker {};

    // -- Fixture B: stateful-extension test --------------------------------------------------
    struct [[=Anno::Interface]] ISearable {
        virtual void Sear() = 0;
        virtual ~ISearable() = default;
    };
    struct [[=Anno::Implementation]] [[=Anno::Implements{^^ISearable}]]
           Brisket : MetaNode<Brisket>, ISearable {
        void Sear() override {}
    };
    // Stateful Extension: one own NSDM => StatefulExtensionClass<Cooked>, so the registrar picks
    // SideTableResolver. No Eager annotation => default lazy. The resolver body emitted by the
    // registrar calls FacadeListLookup only; the materialisation-on-miss step lands in T10.
    struct [[=Anno::Extension]] [[=Anno::Extends{^^Brisket}]] [[=Anno::Implements{^^ISearable}]]
           Cooked {
        int temperatureC = 0;
    };

    // -- Fixture C: EagerSet "no entries" test -----------------------------------------------
    struct [[=Anno::Interface]] IFlavour {
        virtual void Taste() = 0;
        virtual ~IFlavour() = default;
    };
    struct [[=Anno::Implementation]] [[=Anno::Implements{^^IFlavour}]]
           Ribeye : MetaNode<Ribeye>, IFlavour {
        void Taste() override {}
    };
    // Stateless + eager: stateless wins the discriminator => no EagerSet entry per spec §3.3
    // step 5 (stateless ones share a singleton facade and need no per-closure storage).
    struct [[=Anno::Extension]] [[=Anno::Extends{^^Ribeye}]] [[=Anno::Implements{^^IFlavour}]]
           [[=Anno::Eager{}]]
           Smoky {};
    // Stateful + lazy: no Eager annotation => the lazy SideTableResolver path applies; the
    // construction-time eager-set is not touched either.
    struct [[=Anno::Extension]] [[=Anno::Extends{^^Ribeye}]] [[=Anno::Implements{^^IFlavour}]]
           Marinated {
        int saltyness = 0;
    };

} // namespace

TEST_CASE("Install<E> for stateless extension publishes CodeExtensionSingleton", AUTO_TAG) {
    Registry::Install<Steak>();
    Registry::Install<SaltShaker>();

    auto& mc = MetaClassOf<Steak>;
    const auto* links = mc.links();
    REQUIRE(links != nullptr);
    const auto* snap = links->dispatch.load(std::memory_order_acquire);
    REQUIRE(snap != nullptr);

    // Steak's install left a DirectCast entry for ICookable. SaltShaker overrides it: spec §3.2
    // says Extensions win for the same (target, iid) pair, so the entry kind must flip to
    // CodeExtensionSingleton even though Steak C++-inherits ICookable.
    const auto* e = Detail::LookupEntry(snap, IidOf<ICookable>());
    REQUIRE(e != nullptr);
    REQUIRE(e->kind == DispatchKind::CodeExtensionSingleton);
}

TEST_CASE("Install<E> for stateful extension publishes SideTableResolver", AUTO_TAG) {
    Registry::Install<Brisket>();
    Registry::Install<Cooked>();

    auto& mc = MetaClassOf<Brisket>;
    const auto* links = mc.links();
    REQUIRE(links != nullptr);
    const auto* snap = links->dispatch.load(std::memory_order_acquire);
    REQUIRE(snap != nullptr);

    // Cooked is stateful => SideTableResolver. The resolver pointer comes from
    // Detail::SideTableResolverFor<Cooked> and must be non-null so the dynamic-kernel switch in
    // T12 can dispatch through it.
    const auto* e = Detail::LookupEntry(snap, IidOf<ISearable>());
    REQUIRE(e != nullptr);
    REQUIRE(e->kind == DispatchKind::SideTableResolver);
    REQUIRE(e->payload.resolver != nullptr);
}

TEST_CASE("Install<E> skips EagerSet for stateless and for lazy stateful extensions", AUTO_TAG) {
    Registry::Install<Ribeye>();
    Registry::Install<Smoky>();      // stateless + eager => no EagerSet entry (spec §3.3 step 5)
    Registry::Install<Marinated>();  // stateful  + lazy  => no EagerSet entry either

    auto& mc = MetaClassOf<Ribeye>;
    const auto* links = mc.links();
    REQUIRE(links != nullptr);
    const auto* es = links->eagerSet.load(std::memory_order_acquire);
    // No stateful-and-eager extension extends Ribeye in this test suite, so the eager set stays
    // empty: either an uninstalled null pointer or a snapshot with zero entries.
    REQUIRE((es == nullptr || es->count == 0));
}

// =============================================================================
// CRTP hook (Task 9)
// =============================================================================
//
// The @c MetaNode / @c ExtensionNode bases now ODR-use a per-class @c Detail::Registrar<Self> via
// a @c constexpr address-take anchor; the static's initialiser runs @ref Registry::Install at
// program-startup time, so the contract is "by the time the first instance constructs (or by the
// time the test body runs), @ref AlreadyInstalled returns true."
//
// The second test exercises the spec §7.8 named symbol — the @c YukiRegister_<mangledIid>
// extern-C wrapper that Plan 2's manifest pipeline dlsym's at plugin load. The macro emits the
// definition next to the type; the test re-declares the symbol locally and calls it. Install is
// idempotent, so co-existence with the CRTP path is a no-op.

namespace {

    // Fresh fixture for the CRTP-trigger test so we're not depending on whether T7/T8's direct
    // @c Install<Steak> happened to run before this case under Catch2's randomiser — if @c Steak
    // is already installed, the test would pass even with a broken hook. @c HookProbe is not
    // touched by any other test, so the only path that registers it is the CRTP static-init.
    struct [[=Anno::Implementation]] HookProbe : MetaNode<HookProbe> {};

} // namespace

// Manifest-driven discovery symbol (spec §7.8). The macro is invoked at file scope so the emitted
// @c extern @c "C" function has its own translation-unit-scope definition; the matching extern-C
// *declaration* below lives at namespace scope (block-scope @c extern @c "C" is ill-formed). @c
// Steak's anonymous-namespace linkage is fine because the function's *body* may reference
// internal-linkage types — only the function name itself takes external C linkage.
YUKI_DEFINE_REGISTRAR_SYMBOL(Steak, YukiTest__Steak)

extern "C" void YukiRegister_YukiTest__Steak() noexcept;

TEST_CASE("Constructing a MetaNode<T> triggers Registrar<T> at static-init", AUTO_TAG) {
    // The static-init chain runs before main, so by the time this test body executes
    // @ref Registry::Install<HookProbe> has already been called. Constructing an instance is the
    // ODR-use trigger that emitted @c _registrar in this TU — we exercise it twice to also check
    // the "second construction is a no-op" idempotency contract spec §3.3 step 4 demands.
    {
        HookProbe p;
        REQUIRE(Registry::AlreadyInstalled(IidOf<HookProbe>()));
    }
    {
        HookProbe p;
        REQUIRE(Registry::AlreadyInstalled(IidOf<HookProbe>()));
    }
}

TEST_CASE("Named YukiRegister_<iid> symbol exists and installs", AUTO_TAG) {
    // The CRTP hook fires at static-init; this test exercises the *other* registration path —
    // the manifest symbol Plan 2's discovery layer will call. Calling it twice (here + via any
    // prior CRTP firing) is safe by Install's idempotency. The extern-C forward declaration
    // lives just above, at namespace scope, because block-scope @c extern @c "C" is ill-formed.
    YukiRegister_YukiTest__Steak();
    REQUIRE(Registry::AlreadyInstalled(IidOf<Steak>()));
}

// =============================================================================
// MaterializeInto + MaterializeEagerSet construction hook (Task 10)
// =============================================================================
//
// Spec refs: §1.5 (eager/lazy instantiation policy); §3.3 closure-construction hook; §6.4
// lazy materialisation kernel. The T10 contract: when a class @c B has stateful + eager
// Extensions registered, constructing one @c B materialises every such Extension *during
// nucleus construction* via the @c MetaNode<Self>::MetaNode() body, and a subsequent
// FacadeListLookup on @c IidOf<E>() finds the published facade node. Lazy Extensions, by
// contrast, leave the facade chain empty until the dynamic-kernel @c Query path runs the
// SideTableResolver.
//
// The fixtures below are *fresh* (T10-suffixed) so the tests don't depend on whether T8's
// Install<Steak> or Install<Brisket> happened to run before this section under Catch2's
// randomiser. Anonymous namespaces are unique per block, so a redeclared @c ICookableT10
// in a fresh block is a distinct interface from the one above — no double-registration risk.

namespace {

    struct [[=Anno::Interface]] ICookableT10 {
        virtual void Cook() = 0;
        virtual ~ICookableT10() = default;
    };

    // The nucleus to extend. Plain Implementation that C++-inherits ICookableT10 (and so
    // owns a DirectCast entry for it) — the extension below will override that entry to a
    // SideTableResolver and also publish a FacadeNode at construction time.
    struct [[=Anno::Implementation]] [[=Anno::Implements{^^ICookableT10}]]
           SteakT10 : MetaNode<SteakT10>, ICookableT10 {
        void Cook() override {}
    };

    // Eager + stateful Extension. The `[[=Anno::Eager{}]]` tag flips the default-for-stateful
    // (lazy) to eager; the single @c garnishCount NSDM makes it stateful, so the
    // @c AppendToEagerSet path runs and the closure-construction hook is what we exercise.
    // Inherits ExtensionNode so it has the conforming `(SteakT10*)` ctor and a RootObject
    // base — both required by MaterializeIntoImpl.
    struct [[=Anno::Extension]] [[=Anno::Extends{^^SteakT10}]]
           [[=Anno::Implements{^^ICookableT10}]] [[=Anno::Eager{}]]
           PlatedT10 : ExtensionNode<PlatedT10, SteakT10> {
        using ExtensionNode::ExtensionNode;
        int garnishCount = 0;
        // ICookableT10 is not a C++ base of PlatedT10 — the extension publishes a facade
        // node for that iid but the actual interface call doesn't ride through PlatedT10's
        // vtable; QI's downstream dispatch (T11–T13) consults the FacadeList entry instead.
    };

    // Stateful + lazy Extension (no Eager). Verifies the negative path: even after Install
    // runs (so the SideTableResolver entry is published), a fresh SteakT10 starts with no
    // FacadeNode for SaltedT10 — the lazy path stays inert until Query/Reify materialises.
    struct [[=Anno::Extension]] [[=Anno::Extends{^^SteakT10}]]
           [[=Anno::Implements{^^ICookableT10}]]
           SaltedT10 : ExtensionNode<SaltedT10, SteakT10> {
        using ExtensionNode::ExtensionNode;
        int saltCount = 0;
    };

} // namespace

TEST_CASE("Eager stateful Extension materializes during nucleus construction", AUTO_TAG) {
    // Both classes need their registrars to have run before we observe the side-effect.
    // The CRTP hook would normally have fired at static-init, but be explicit for clarity.
    Registry::Install<SteakT10>();
    Registry::Install<PlatedT10>();

    SteakT10 s;  // <-- the construction hook is the contract under test.
    auto* head = RT::Facades(&s);
    REQUIRE(head != nullptr);

    // The eager hook published a FacadeNode keyed on IidOf<PlatedT10>() — looking it up
    // returns a non-null facade pointer.
    REQUIRE(FacadeListLookup(*head, IidOf<PlatedT10>()) != nullptr);

    // It also published one for each iid PlatedT10 implements — ICookableT10 in this test.
    REQUIRE(FacadeListLookup(*head, IidOf<ICookableT10>()) != nullptr);

    // Both facade pointers resolve to the same @c PlatedT10 instance — the construction hook
    // allocates exactly one @c PlatedT10 per nucleus and attaches it under multiple iids.
    REQUIRE(FacadeListLookup(*head, IidOf<PlatedT10>())
            == FacadeListLookup(*head, IidOf<ICookableT10>()));
}

TEST_CASE("Eager materialization is idempotent across MaterializeInto calls", AUTO_TAG) {
    Registry::Install<SteakT10>();
    Registry::Install<PlatedT10>();

    SteakT10 s;  // construction hook publishes once.
    auto* head = RT::Facades(&s);
    REQUIRE(head != nullptr);

    RootObject* first = FacadeListLookup(*head, IidOf<PlatedT10>());
    REQUIRE(first != nullptr);

    // Hand-calling @c MaterializeInto a second time must be a no-op: the pre-flight
    // FacadeListLookup inside MaterializeIntoImpl short-circuits before allocating a second
    // copy, and AttachUnique would dedup anyway. The published pointer stays identical.
    PlatedT10::MaterializeInto(s);
    RootObject* second = FacadeListLookup(*head, IidOf<PlatedT10>());
    REQUIRE(second == first);
}

TEST_CASE("Lazy Extension is absent until Query/Reify materializes it", AUTO_TAG) {
    Registry::Install<SteakT10>();
    Registry::Install<SaltedT10>();  // stateful + lazy => not in SteakT10's eager set

    SteakT10 s;  // construction hook runs, but SaltedT10 is not enrolled in the eager set.
    auto* head = RT::Facades(&s);
    REQUIRE(head != nullptr);

    // No FacadeNode for SaltedT10 yet — the lazy path stays inert until Query/Reify lands.
    // The T12/T13 dynamic kernel will route through SideTableResolver to materialize on
    // demand; that path is exercised in its own tests.
    REQUIRE(FacadeListLookup(*head, IidOf<SaltedT10>()) == nullptr);
}

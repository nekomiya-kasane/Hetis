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

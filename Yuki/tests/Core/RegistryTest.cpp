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

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

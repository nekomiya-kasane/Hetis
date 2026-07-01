#include <Yuki/Core/Closure.h>
#include <Yuki/Core/MakeOwned.h>
#include <Yuki/Core/Registry.h>
#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/YObjectMacro.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace Yuki;

namespace {
    struct [[=Anno::Interface]] ICloA : RootObject {
        ICloA(ClassType role, bool external) : RootObject(role, nullptr, external) {}
        ~ICloA() override = default;
    };

    struct [[=Anno::Implementation]]
           [[=Anno::Implements{^^ICloA}]]
           CloImpl : ICloA {
        Y_OBJECT;
        CloImpl() : ICloA(ClassType::Implementation, /*external=*/false) {}
        ~CloImpl() override = default;
    };
}

TEST_CASE("Nucleus(impl) returns impl itself when Upstream() is null", AUTO_TAG) {
    Registry::Install<CloImpl>();
    auto impl = MakeOwned<CloImpl>();
    // The base RootObject default returns nullptr from Upstream(), so a plain impl is its
    // own nucleus.
    REQUIRE(Nucleus(impl.Get()) == impl.Get());
}

TEST_CASE("Nucleus(nullptr) returns nullptr", AUTO_TAG) {
    REQUIRE(Nucleus(nullptr) == nullptr);
}

TEST_CASE("InClosure is reflexive on a non-null nucleus", AUTO_TAG) {
    Registry::Install<CloImpl>();
    auto impl = MakeOwned<CloImpl>();
    REQUIRE(InClosure(impl.Get(), impl.Get()));
}

TEST_CASE("InClosure is symmetric across the same nucleus", AUTO_TAG) {
    Registry::Install<CloImpl>();
    auto impl = MakeOwned<CloImpl>();
    REQUIRE(InClosure(impl.Get(), impl.Get()) == InClosure(impl.Get(), impl.Get()));
}

TEST_CASE("InClosure(nullptr, x) returns false", AUTO_TAG) {
    Registry::Install<CloImpl>();
    auto impl = MakeOwned<CloImpl>();
    REQUIRE_FALSE(InClosure(nullptr, impl.Get()));
    REQUIRE_FALSE(InClosure(impl.Get(), nullptr));
}

TEST_CASE("MaterializedFacades and Extensions return empty spans for plain impls", AUTO_TAG) {
    // A3 ship state: the per-nucleus side tables for materialized facades + extensions
    // are not yet populated (facade materialization + eager-set publish land in A4). The
    // surface is final and returns empty spans for every nucleus in the meantime.
    Registry::Install<CloImpl>();
    auto impl = MakeOwned<CloImpl>();
    REQUIRE(MaterializedFacades(impl.Get()).empty());
    REQUIRE(Extensions(impl.Get()).empty());
}

TEST_CASE("WalkClosure invokes fn exactly once on a plain nucleus", AUTO_TAG) {
    Registry::Install<CloImpl>();
    auto impl = MakeOwned<CloImpl>();
    std::vector<RootObject*> visited;
    WalkClosure(impl.Get(), [&](RootObject* n) { visited.push_back(n); });
    REQUIRE(visited.size() == 1);
    REQUIRE(visited[0] == impl.Get());
}

TEST_CASE("WalkClosure(nullptr, fn) is a no-op", AUTO_TAG) {
    int callCount = 0;
    WalkClosure(nullptr, [&](RootObject*) { ++callCount; });
    REQUIRE(callCount == 0);
}

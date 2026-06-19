#include <Yuki/Core/ArmRegistry.h>
#include <Yuki/Core/DispatchEntry.h>
#include <Yuki/Core/MakeOwned.h>
#include <Yuki/Core/MergedDispatch.h>
#include <Yuki/Core/MetaCore.h>
#include <Yuki/Core/MetaDynamic.h>
#include <Yuki/Core/Registry.h>
#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/YObjectMacro.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

using namespace Yuki;

namespace {
    struct [[=Anno::Interface]] IFlatExt : RootObject {
        IFlatExt(ClassType role, bool external) : RootObject(role, nullptr, external) {}
        ~IFlatExt() override = default;
    };

    // Base class — gets RegisterSideTable; subclass D inherits the entry via the D16
    // flatten walk over subclassedBy (§5.3).
    struct [[=Anno::Implementation]] FlatBase : RootObject {
        Y_OBJECT;
        FlatBase() : RootObject(ClassType::Implementation, nullptr, /*external=*/false) {}
        ~FlatBase() override = default;
    };

    struct [[=Anno::Implementation]] FlatSub : FlatBase {
        Y_OBJECT;
        FlatSub() : FlatBase() {}
        ~FlatSub() override = default;
    };

    // Stub side-table resolver — never actually called during this test; the assertion
    // surface is mergedDispatch contents, not arm invocation. ArmKindsTest exercises
    // resolver invocation directly.
    RootObject* FlatResolver(RootObject*) noexcept { return nullptr; }
}

TEST_CASE("RegisterSideTable on base flattens into subclass mergedDispatch (D16)", AUTO_TAG) {
    // Install both classes so their MetaLinks are populated and FlatSub appears in
    // FlatBase's subclassedBy via the Install<T>() reflection pass.
    Registry::Install<FlatBase>();
    Registry::Install<FlatSub>();

    // Snapshot subclass cacheEpoch BEFORE registration so we can confirm the bump.
    auto epochBefore = FlatSub::Meta().links->cacheEpoch.load();

    RegisterSideTable<FlatBase, IFlatExt, &FlatResolver>();

    // Base sees the entry.
    const auto* baseMerged =
        FlatBase::Meta().links->mergedDispatch.load(std::memory_order_acquire);
    REQUIRE(baseMerged != nullptr);
    REQUIRE(LookupMergedDispatch(baseMerged, IidOf<IFlatExt>()) != nullptr);

    // Subclass sees the entry via D16 propagation.
    const auto* subMerged =
        FlatSub::Meta().links->mergedDispatch.load(std::memory_order_acquire);
    REQUIRE(subMerged != nullptr);
    const DispatchEntry* subEntry = LookupMergedDispatch(subMerged, IidOf<IFlatExt>());
    REQUIRE(subEntry != nullptr);
    REQUIRE(subEntry->providerClass == &FlatBase::kMetaCore);
    REQUIRE(subEntry->kind == DispatchKind::SideTableResolver);

    // L3 invalidation bumped subclass cacheEpoch.
    auto epochAfter = FlatSub::Meta().links->cacheEpoch.load();
    REQUIRE(epochAfter > epochBefore);
}

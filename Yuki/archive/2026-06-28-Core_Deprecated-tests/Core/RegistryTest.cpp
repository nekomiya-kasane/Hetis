#include <Yuki/Core/Registry.h>
#include <Yuki/Core/MergedDispatch.h>
#include <Yuki/Core/MetaDynamic.h>
#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/YObjectMacro.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

using namespace Yuki;

namespace {
    struct [[=Anno::Interface]] IRegA { virtual ~IRegA() = default; };
    struct [[=Anno::Interface]] IRegB { virtual ~IRegB() = default; };

    struct [[=Anno::Implementation]]
           [[=Anno::Implements{^^IRegA}]]
           PlainImpl : RootObject, IRegA {
      public:
        Y_OBJECT;
        PlainImpl() : RootObject(ClassType::Implementation, nullptr, /*external=*/true) {}
        ~PlainImpl() override = default;
    };

    struct [[=Anno::Implementation]]
           [[=Anno::Implements{^^IRegB}]]
           [[=Anno::Final{^^IRegB}]]
           FinalImpl : RootObject, IRegB {
      public:
        Y_OBJECT;
        FinalImpl() : RootObject(ClassType::Implementation, nullptr, /*external=*/true) {}
        ~FinalImpl() override = default;
    };
}

TEST_CASE("Install<T> publishes a DispatchSnapshot the class can read", AUTO_TAG) {
    Registry::Install<PlainImpl>();
    const auto* snap = PlainImpl::Meta().links->dispatch.load(std::memory_order_acquire);
    REQUIRE(snap != nullptr);
    REQUIRE(snap->count >= 1);
    REQUIRE(snap->entries[0].iid == IidOf<IRegA>());
}

TEST_CASE("Install<T> publishes a MergedDispatchSnapshot reachable via L2", AUTO_TAG) {
    Registry::Install<PlainImpl>();
    const auto* merged =
        PlainImpl::Meta().links->mergedDispatch.load(std::memory_order_acquire);
    REQUIRE(merged != nullptr);
    REQUIRE(LookupMergedDispatch(merged, IidOf<IRegA>()) != nullptr);
}

TEST_CASE("Install<T> bumps cacheEpoch", AUTO_TAG) {
    auto eBefore = FinalImpl::Meta().links->cacheEpoch.load();
    Registry::Install<FinalImpl>();
    auto eAfter = FinalImpl::Meta().links->cacheEpoch.load();
    REQUIRE(eAfter > eBefore);
}

TEST_CASE("Install<T> is idempotent on the same class", AUTO_TAG) {
    // Second call publishes a new snapshot but does NOT abort - the seal check sees
    // "prior entry from the same providerClass" and accepts it as a re-install.
    Registry::Install<PlainImpl>();
    Registry::Install<PlainImpl>();
    SUCCEED();
}

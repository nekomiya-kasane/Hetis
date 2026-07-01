#include <Yuki/Core/MetaLinks.h>

#include "Meta.h"

#include <catch2/catch_test_macros.hpp>

using namespace Yuki;

TEST_CASE("MetaLinks: default state and epoch bump", AUTO_TAG) {
    MetaLinks l{};
    REQUIRE(l.dispatch.load() == nullptr);
    REQUIRE(l.mergedDispatch.load() == nullptr);
    REQUIRE(l.extendedBy.load() == nullptr);
    REQUIRE(l.implementedBy.load() == nullptr);
    REQUIRE(l.eagerSet.load() == nullptr);
    const auto e0 = l.cacheEpoch.load();
    l.BumpCacheEpoch();
    REQUIRE(l.cacheEpoch.load() == e0 + 1);
}

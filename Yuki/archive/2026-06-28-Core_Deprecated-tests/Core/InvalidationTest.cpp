#include <Yuki/Core/ExtendedList.h>
#include <Yuki/Core/MetaLinks.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

using namespace Yuki;

TEST_CASE("BroadcastInvalidation bumps every downstream's cacheEpoch", AUTO_TAG) {
    MetaLinks downA{}, downB{}, downC{};
    const MetaLinks* downs[3] = { &downA, &downB, &downC };
    ExtendedListSnapshot snap{ .count = 3, .downstreams = downs };

    auto eA = downA.cacheEpoch.load();
    auto eB = downB.cacheEpoch.load();
    auto eC = downC.cacheEpoch.load();

    BroadcastInvalidation(&snap);

    REQUIRE(downA.cacheEpoch.load() == eA + 1);
    REQUIRE(downB.cacheEpoch.load() == eB + 1);
    REQUIRE(downC.cacheEpoch.load() == eC + 1);
}

TEST_CASE("BroadcastInvalidation on empty / nullptr snapshot is a no-op", AUTO_TAG) {
    BroadcastInvalidation(nullptr);
    ExtendedListSnapshot empty{ .count = 0, .downstreams = nullptr };
    BroadcastInvalidation(&empty);
    SUCCEED();
}

TEST_CASE("ImplementedListSnapshot mirrors ExtendedList shape", AUTO_TAG) {
    MetaLinks one{};
    const MetaLinks* arr[1] = { &one };
    ImplementedListSnapshot snap{ .count = 1, .providers = arr };
    REQUIRE(snap.count == 1);
    REQUIRE(snap.providers[0] == &one);
}

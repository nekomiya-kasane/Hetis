#include <Yuki/Core/FingerprintCache.h>
#include <Yuki/Core/DispatchEntry.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>
#include <optional>

namespace M = Mashiro;
using namespace Yuki;

TEST_CASE("Probe on empty cache returns nullopt", AUTO_TAG) {
    FingerprintCache c{};
    constexpr Iid i1{M::Uuid{1, 0}};
    REQUIRE(!Probe(c, i1, /*liveEpoch=*/0).has_value());
}

TEST_CASE("Publish then Probe is a positive hit", AUTO_TAG) {
    FingerprintCache c{};
    constexpr Iid i1{M::Uuid{1, 0}};
    DispatchEntry e{ .iid = i1, .kind = DispatchKind::InlineFacade };
    Publish(c, i1, &e, /*epoch=*/7);
    auto hit = Probe(c, i1, /*liveEpoch=*/7);
    REQUIRE(hit.has_value());
    REQUIRE(*hit == &e);
}

TEST_CASE("Negative hit caches a miss", AUTO_TAG) {
    FingerprintCache c{};
    constexpr Iid i1{M::Uuid{1, 0}};
    Publish(c, i1, /*entry=*/nullptr, /*epoch=*/3);
    auto hit = Probe(c, i1, /*liveEpoch=*/3);
    REQUIRE(hit.has_value());
    REQUIRE(*hit == nullptr);
}

TEST_CASE("Epoch mismatch invalidates a slot", AUTO_TAG) {
    FingerprintCache c{};
    constexpr Iid i1{M::Uuid{1, 0}};
    DispatchEntry e{ .iid = i1, .kind = DispatchKind::InlineFacade };
    Publish(c, i1, &e, /*epoch=*/1);
    REQUIRE(!Probe(c, i1, /*liveEpoch=*/2).has_value());
}

TEST_CASE("Ring evicts old slots after 4 publishes", AUTO_TAG) {
    FingerprintCache c{};
    DispatchEntry e[5]{};
    for (int k = 0; k < 5; ++k) {
        e[k].iid = Iid{M::Uuid{static_cast<std::uint64_t>(k + 1), 0}};
        Publish(c, e[k].iid, &e[k], /*epoch=*/9);
    }
    // The first publish is guaranteed evicted by the 5th publish on a 4-slot ring.
    REQUIRE(!Probe(c, e[0].iid, /*liveEpoch=*/9).has_value());
    REQUIRE(Probe(c, e[4].iid, /*liveEpoch=*/9).has_value());
}

#include <Yuki/Core/TaggedPayload.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>
#include <atomic>

using namespace Yuki;

TEST_CASE("TaggedPayload packs role/everAcquired/refcount/armPtr", AUTO_TAG) {
    auto* arm = reinterpret_cast<void*>(uintptr_t{0x7ffe'cafe'beefULL & ~uintptr_t{0xF}});
    TaggedPayload p = TaggedPayload::Make(ClassType::Extension, arm,
                                          /*refcount=*/3, /*everAcquired=*/true);
    REQUIRE(p.role()         == ClassType::Extension);
    REQUIRE(p.everAcquired() == true);
    REQUIRE(p.refcount()     == 3);
    REQUIRE(p.armPtr()       == arm);
}

TEST_CASE("ExternalLifetimeSentinel reports correctly", AUTO_TAG) {
    TaggedPayload p = TaggedPayload::MakeExternal(ClassType::Implementation, nullptr);
    REQUIRE(p.refcount() == TaggedPayload::kExternalSentinel);
    REQUIRE(p.isExternalLifetime());
}

TEST_CASE("Atomic CAS refcount increment preserves other fields", AUTO_TAG) {
    auto* arm = reinterpret_cast<void*>(uintptr_t{0x1000});
    std::atomic<TaggedPayload> a{TaggedPayload::Make(ClassType::Implementation, arm, 1, true)};
    REQUIRE(TaggedPayload::TryIncrement(a) == true);
    auto loaded = a.load(std::memory_order_relaxed);
    REQUIRE(loaded.refcount() == 2);
    REQUIRE(loaded.role()     == ClassType::Implementation);
    REQUIRE(loaded.armPtr()   == arm);
}

#include <Yuki/Core/MergedDispatch.h>
#include <Yuki/Core/DispatchEntry.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

namespace M = Mashiro;
using namespace Yuki;

TEST_CASE("LookupMergedDispatch returns nullptr on empty snapshot", AUTO_TAG) {
    MergedDispatchSnapshot snap{ .count = 0, .entries = nullptr };
    constexpr Iid i1{M::Uuid{1, 0}};
    REQUIRE(LookupMergedDispatch(&snap, i1) == nullptr);
}

TEST_CASE("LookupMergedDispatch hits an iid-sorted entry", AUTO_TAG) {
    constexpr Iid i1{M::Uuid{1, 0}};
    constexpr Iid i2{M::Uuid{2, 0}};
    constexpr Iid i3{M::Uuid{3, 0}};
    DispatchEntry e[3] = {
        { .iid = i1, .kind = DispatchKind::InlineFacade },
        { .iid = i2, .kind = DispatchKind::SideTableResolver },
        { .iid = i3, .kind = DispatchKind::CodeExtensionSingleton },
    };
    MergedDispatchSnapshot snap{ .count = 3, .entries = e };
    REQUIRE(LookupMergedDispatch(&snap, i1) == &e[0]);
    REQUIRE(LookupMergedDispatch(&snap, i2) == &e[1]);
    REQUIRE(LookupMergedDispatch(&snap, i3) == &e[2]);
}

TEST_CASE("LookupMergedDispatch returns nullptr on miss", AUTO_TAG) {
    constexpr Iid i1{M::Uuid{1, 0}};
    constexpr Iid i2{M::Uuid{2, 0}};
    constexpr Iid i9{M::Uuid{9, 0}};
    DispatchEntry e[2] = {
        { .iid = i1, .kind = DispatchKind::InlineFacade },
        { .iid = i2, .kind = DispatchKind::InlineFacade },
    };
    MergedDispatchSnapshot snap{ .count = 2, .entries = e };
    REQUIRE(LookupMergedDispatch(&snap, i9) == nullptr);
}

TEST_CASE("LookupMergedDispatch prefers Important on iid tie (D7.3)", AUTO_TAG) {
    constexpr Iid i1{M::Uuid{1, 0}};
    // Two entries with the same iid; the second is Important. The function must
    // return the Important entry regardless of insertion order. SealFlags shape
    // (post-T13): `.seal = {.important = true}` rather than a loose `.important` field.
    DispatchEntry e[2] = {
        { .iid = i1, .kind = DispatchKind::InlineFacade,      .seal = {} },
        { .iid = i1, .kind = DispatchKind::SideTableResolver, .seal = {.important = true} },
    };
    MergedDispatchSnapshot snap{ .count = 2, .entries = e };
    auto* hit = LookupMergedDispatch(&snap, i1);
    REQUIRE(hit != nullptr);
    REQUIRE(hit->seal.important == true);
    REQUIRE(hit->kind == DispatchKind::SideTableResolver);
}

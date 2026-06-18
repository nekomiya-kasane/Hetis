#include <Yuki/Core/DispatchEntry.h>
#include <Yuki/Core/MetaCore.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <type_traits>

namespace M = Mashiro;
using namespace Yuki;

TEST_CASE("DispatchKind enumerates the three Y2 arms", AUTO_TAG) {
    static_assert(static_cast<int>(DispatchKind::InlineFacade)            == 0);
    static_assert(static_cast<int>(DispatchKind::SideTableResolver)       == 1);
    static_assert(static_cast<int>(DispatchKind::CodeExtensionSingleton)  == 2);
    static_assert(std::is_same_v<std::underlying_type_t<DispatchKind>, std::uint8_t>);
}

TEST_CASE("DispatchEntry stores iid / kind / seal-bits / arm", AUTO_TAG) {
    constexpr Iid i1{M::Uuid{1, 0}};
    MetaCore probe{ .iid = i1, .name = "P", .role = ClassType::Implementation,
                    .implements = nullptr, .implementsCount = 0,
                    .extends = nullptr, .extendsCount = 0 };
    int armStorage = 0;
    DispatchEntry e{
        .iid           = i1,
        .kind          = DispatchKind::InlineFacade,
        .seal          = {.final = false, .unique = false, .important = true},
        .armOffset     = 0x40,
        .providerClass = &probe,
        .arm           = &armStorage,
    };
    REQUIRE(e.iid == i1);
    REQUIRE(e.kind == DispatchKind::InlineFacade);
    REQUIRE(e.seal.important == true);
    REQUIRE(e.armOffset == 0x40);
    REQUIRE(e.providerClass == &probe);
    REQUIRE(e.arm == &armStorage);
}

TEST_CASE("DispatchSnapshot is a (count, entries) POD", AUTO_TAG) {
    constexpr Iid i1{M::Uuid{1, 0}};
    DispatchEntry e{ .iid = i1, .kind = DispatchKind::SideTableResolver };
    DispatchSnapshot snap{ .count = 1, .entries = &e };
    REQUIRE(snap.count == 1);
    REQUIRE(snap.entries[0].iid == i1);
}

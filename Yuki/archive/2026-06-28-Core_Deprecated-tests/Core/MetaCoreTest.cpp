#include <Yuki/Core/MetaCore.h>
#include <Yuki/Core/Identity.h>
#include <Yuki/Core/RootObject.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

using namespace Yuki;

namespace {

    struct [[=Anno::Interface]] IZ { virtual ~IZ() = default; };

    struct [[=Anno::Implementation]]
           [[=Anno::Implements{^^IZ}]]
           [[=Anno::Final{^^IZ}]]
           Z : RootObject, IZ {
        Z() : RootObject(ClassType::Implementation, nullptr, /*external=*/true) {}
        ~Z() override = default;
    };

}

TEST_CASE("MetaCore exposes iid, role, name, implements list", AUTO_TAG) {
    constexpr auto m = Detail::MakeMetaCoreFor<Z>();
    REQUIRE(m.role == ClassType::Implementation);
    REQUIRE(m.implementsCount >= 1);
    REQUIRE(m.implements[0].iid == IidOf<IZ>());
    REQUIRE(m.implements[0].flags.final == true);
}

TEST_CASE("Iid is deterministic per type", AUTO_TAG) {
    static_assert(IidOf<IZ>() == IidOf<IZ>());
    static_assert(IidOf<IZ>() != IidOf<Z>());
}

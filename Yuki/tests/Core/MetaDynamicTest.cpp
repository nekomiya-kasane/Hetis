#include <Yuki/Core/MetaDynamic.h>
#include <Yuki/Core/MetaCore.h>
#include <Yuki/Core/YObjectMacro.h>
#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/Identity.h>

#include "Meta.h"

#include <catch2/catch_test_macros.hpp>

using namespace Yuki;

namespace {
    struct [[=Anno::Implementation]] D : RootObject {
        Y_OBJECT;
        D() : RootObject(ClassType::Implementation, nullptr, /*external=*/true) {}
    };
}

TEST_CASE("MetaDynamic exposes kMetaCore and a MetaLinks ptr", AUTO_TAG) {
    const auto& md = Yuki::MetaDynamicOf<D>;
    REQUIRE(md.core == &D::kMetaCore);
    REQUIRE(md.links != nullptr);
}

TEST_CASE("Y_OBJECT::Meta() returns the per-class MetaDynamicOf", AUTO_TAG) {
    static_assert(std::is_same_v<decltype(D::Meta()), const MetaDynamic&>);
    const auto& m = D::Meta();
    REQUIRE(&m == &MetaDynamicOf<D>);
}

#include <Yuki/Core/YObjectMacro.h>
#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/Identity.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

using namespace Yuki;

namespace {
    struct [[=Anno::Implementation]] MyImpl : RootObject {
      public:
        Y_OBJECT;
        MyImpl() : RootObject(ClassType::Implementation, nullptr, /*external=*/true) {}
        ~MyImpl() override = default;
    };
}

TEST_CASE("Y_OBJECT exposes kMetaCore via reflection", AUTO_TAG) {
    constexpr auto& core = MyImpl::kMetaCore;
    REQUIRE(core.role == ClassType::Implementation);
}

TEST_CASE("MyImpl still 2 words after Y_OBJECT", AUTO_TAG) {
    // Y_OBJECT adds only statics + friend; no per-instance growth.
    static_assert(sizeof(MyImpl) == sizeof(RootObject));
}

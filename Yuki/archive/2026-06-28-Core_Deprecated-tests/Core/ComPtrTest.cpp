#include <Yuki/Core/ComPtr.h>
#include <Yuki/Core/RootObject.h>

#include "Meta.h"

#include <catch2/catch_test_macros.hpp>

using namespace Yuki;

namespace {
    struct Counted : RootObject {
        Counted() : RootObject(ClassType::Implementation, nullptr, /*external=*/false) {}
        ~Counted() override = default;
    };
}

TEST_CASE("ComPtr Adopt + copy + dtor balances refcount", AUTO_TAG) {
    auto* raw = new Counted();
    ComPtr<Counted> a = ComPtr<Counted>::Adopt(raw);
    REQUIRE(raw->PayloadRelaxed().refcount() == 1);
    {
        ComPtr<Counted> b = a;
        REQUIRE(raw->PayloadRelaxed().refcount() == 2);
    }
    REQUIRE(raw->PayloadRelaxed().refcount() == 1);
}

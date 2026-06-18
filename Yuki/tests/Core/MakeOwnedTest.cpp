#include <Yuki/Core/MakeOwned.h>
#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/Identity.h>

#include "Meta.h"

#include <catch2/catch_test_macros.hpp>

using namespace Yuki;

namespace {
    struct Counted : RootObject {
        int v;
        explicit Counted(int x) : RootObject(ClassType::Implementation, nullptr, /*external=*/false), v(x) {}
        ~Counted() override = default;
    };
}

TEST_CASE("MakeOwned constructs with refcount=1 and ComPtr ownership", AUTO_TAG) {
    auto p = MakeOwned<Counted>(42);
    REQUIRE(p);
    REQUIRE(p->v == 42);
    REQUIRE(p->PayloadRelaxed().refcount() == 1);
}

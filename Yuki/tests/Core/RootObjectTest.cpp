#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/Identity.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>
#include <type_traits>

using namespace Yuki;

TEST_CASE("RootObject is exactly 2 words", AUTO_TAG) {
    static_assert(sizeof(RootObject) == 2 * sizeof(void*));
    static_assert(alignof(RootObject) == alignof(void*));
}

TEST_CASE("RootObject has virtual dtor", AUTO_TAG) {
    static_assert(std::has_virtual_destructor_v<RootObject>);
}

TEST_CASE("RootObject is non-template", AUTO_TAG) {
    // If RootObject were a template, naming it bare wouldn't compile.
    RootObject* p = nullptr; (void)p;
}

namespace {
    struct Probe : Yuki::RootObject {
        Probe() : RootObject(Yuki::ClassType::Implementation, nullptr,
                             /*external=*/true) {}
        ~Probe() override = default;
    };
}

TEST_CASE("External-lifetime instance exposes role via TypeDynamic", AUTO_TAG) {
    Probe p;
    REQUIRE(p.TypeDynamic() == ClassType::Implementation);
}

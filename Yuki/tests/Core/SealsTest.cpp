#include <Yuki/Core/Identity.h>
#include <Yuki/Core/RootObject.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>
using namespace Yuki;
namespace {
    struct [[=Anno::Interface]]      IFoo { virtual ~IFoo() = default; };
    struct [[=Anno::Interface]]      IBar { virtual ~IBar() = default; };
    struct [[=Anno::Implementation]]
           [[=Anno::Implements{^^IFoo}]]
           [[=Anno::Final{^^IFoo}]]
           F : RootObject, IFoo {
        F() : RootObject(ClassType::Implementation, nullptr, /*external=*/true) {}
        ~F() override = default;
    };
    struct [[=Anno::Extension]]
           [[=Anno::Extends{^^F}]]
           [[=Anno::Implements{^^IBar}]]
           [[=Anno::Important{^^IBar}]]
           X : RootObject {
        X() : RootObject(ClassType::Extension, nullptr, /*external=*/true) {}
        ~X() override = default;
    };
}
TEST_CASE("Seal flags reachable consteval", AUTO_TAG) {
    constexpr auto sf = Detail::SealFlagsFor<F, IFoo>();
    static_assert(sf.final == true);
    static_assert(sf.unique == false);
    static_assert(sf.important == false);
    constexpr auto sx = Detail::SealFlagsFor<X, IBar>();
    static_assert(sx.important == true);
    static_assert(sx.final == false);
}

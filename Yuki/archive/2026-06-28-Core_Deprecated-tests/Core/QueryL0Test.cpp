#include <Yuki/Core/QueryL0.h>
#include <Yuki/Core/Identity.h>
#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/YObjectMacro.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

using namespace Yuki;

namespace {
    struct [[=Anno::Interface]]      IFastA { virtual ~IFastA() = default; };
    struct [[=Anno::Interface]]      IFastB { virtual ~IFastB() = default; };

    struct [[=Anno::Implementation]]
           [[=Anno::Implements{^^IFastA}]]
           FastImpl : RootObject, IFastA {
      public:
        Y_OBJECT;
        FastImpl() : RootObject(ClassType::Implementation, nullptr, /*external=*/true) {}
        ~FastImpl() override = default;
    };

    struct [[=Anno::Implementation]]  // no Implements annotation at all.
           LonelyImpl : RootObject {
      public:
        Y_OBJECT;
        LonelyImpl() : RootObject(ClassType::Implementation, nullptr, /*external=*/true) {}
        ~LonelyImpl() override = default;
    };
}

TEST_CASE("L0 positive: BOA provider is detected", AUTO_TAG) {
    static_assert(Detail::IsBoaProvider<FastImpl, IFastA>());
    constexpr const DispatchEntry* e = Detail::L0Shortcut<FastImpl, IFastA>();
    REQUIRE(e != nullptr);
    REQUIRE(e->kind == DispatchKind::InlineFacade);
    REQUIRE(e->iid == IidOf<IFastA>());
}

TEST_CASE("L0 negative: no implements annotation", AUTO_TAG) {
    static_assert(!Detail::IsBoaProvider<LonelyImpl, IFastA>());
    REQUIRE(Detail::L0Shortcut<LonelyImpl, IFastA>() == nullptr);
}

TEST_CASE("L0 negative: impl does not provide the wrong iface", AUTO_TAG) {
    static_assert(!Detail::IsBoaProvider<FastImpl, IFastB>());
    REQUIRE(Detail::L0Shortcut<FastImpl, IFastB>() == nullptr);
}

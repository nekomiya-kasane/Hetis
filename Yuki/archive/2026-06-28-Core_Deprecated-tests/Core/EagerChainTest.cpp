#include <Yuki/Core/EagerChain.h>
#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/YObjectMacro.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

using namespace Yuki;

namespace {
    struct ExtendeeImpl : RootObject {
      public:
        Y_OBJECT;
        ExtendeeImpl() : RootObject(ClassType::Implementation, nullptr, /*external=*/false) {}
        ~ExtendeeImpl() override = default;
    };

    struct EagerExt : RootObject {
      public:
        Y_OBJECT;
        EagerExt() : RootObject(ClassType::Extension, nullptr, /*external=*/false) {}
        ~EagerExt() override = default;
    };
} // namespace

TEST_CASE("ParkEager drops refcount to 0", AUTO_TAG) {
    auto* ext = new EagerExt();
    REQUIRE(ext->PayloadRelaxed().refcount() == 1);
    ParkEager(ext);
    REQUIRE(ext->PayloadRelaxed().refcount() == 0);
    delete ext;
}

TEST_CASE("HotAcquireEager bumps ext to 1 and pins the extendee", AUTO_TAG) {
    auto* extendee = new ExtendeeImpl();
    auto* ext = new EagerExt();
    ParkEager(ext);
    REQUIRE(ext->PayloadRelaxed().refcount() == 0);
    auto extendeeBefore = extendee->PayloadRelaxed().refcount();

    HotAcquireEager(ext, extendee);
    REQUIRE(ext->PayloadRelaxed().refcount() == 1);
    REQUIRE(extendee->PayloadRelaxed().refcount() == extendeeBefore + 1);

    delete ext;
    delete extendee;
}

TEST_CASE("DetachEagerOnRelease un-Acquires extendee + re-parks ext", AUTO_TAG) {
    auto* extendee = new ExtendeeImpl();
    auto* ext = new EagerExt();
    ParkEager(ext);
    HotAcquireEager(ext, extendee);
    auto extendeeHot = extendee->PayloadRelaxed().refcount();

    DetachEagerOnRelease(ext, extendee);
    REQUIRE(ext->PayloadRelaxed().refcount() == 0);
    REQUIRE(extendee->PayloadRelaxed().refcount() == extendeeHot - 1);

    delete ext;
    delete extendee;
}

TEST_CASE("EagerSetSnapshot is a (count, parked) POD", AUTO_TAG) {
    auto* ext = new EagerExt();
    ParkEager(ext);
    RootObject* arr[1] = { ext };
    EagerSetSnapshot snap{ .count = 1, .parked = arr };
    REQUIRE(snap.count == 1);
    REQUIRE(snap.parked[0] == ext);
    delete ext;
}

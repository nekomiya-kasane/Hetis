#include <Yuki/Core/ComPtr.h>
#include <Yuki/Core/MakeOwned.h>
#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/YObjectMacro.h>

#include "Meta.h"

#include <catch2/catch_test_macros.hpp>

#include <utility>

using namespace Yuki;

namespace {
    struct [[=Anno::Interface]] IShape : RootObject {
        IShape(ClassType role, bool external) : RootObject(role, nullptr, external) {}
        ~IShape() override = default;
        virtual int Sides() const = 0;
    };

    // An unrelated interface no impl below provides — used to exercise the closure miss path.
    struct [[=Anno::Interface]] IUnrelated : RootObject {
        IUnrelated(ClassType role, bool external) : RootObject(role, nullptr, external) {}
        ~IUnrelated() override = default;
    };

    struct [[=Anno::Implementation]]
           [[=Anno::Implements{^^IShape}]]
           Triangle : IShape {
        Y_OBJECT;
        Triangle() : IShape(ClassType::Implementation, /*external=*/false) {}
        ~Triangle() override = default;
        int Sides() const override { return 3; }
    };

    auto Rc(const RootObject* p) { return p->PayloadRelaxed().refcount(); }
}

TEST_CASE("Upcast copy bumps the shared object and leaves the source intact", AUTO_TAG) {
    auto impl = MakeOwned<Triangle>();
    RootObject* obj = impl.Get();
    REQUIRE(Rc(obj) == 1);

    ComPtr<IShape> iface = impl;  // base subobject — same RootObject, +1.
    REQUIRE(static_cast<RootObject*>(iface.Get()) == obj);
    REQUIRE(iface->Sides() == 3);
    REQUIRE(Rc(obj) == 2);
    REQUIRE(impl);  // source untouched by copy
}

TEST_CASE("Upcast move steals the +1 with no refcount traffic", AUTO_TAG) {
    auto impl = MakeOwned<Triangle>();
    RootObject* obj = impl.Get();
    REQUIRE(Rc(obj) == 1);

    ComPtr<IShape> iface = std::move(impl);
    REQUIRE(static_cast<RootObject*>(iface.Get()) == obj);
    REQUIRE(Rc(obj) == 1);     // stolen, not bumped
    REQUIRE_FALSE(impl);       // source emptied
}

TEST_CASE("Downcast copy onto the same object resolves via the closure and bumps", AUTO_TAG) {
    auto impl = MakeOwned<Triangle>();
    ComPtr<IShape> iface = impl;          // rc == 2
    RootObject* obj = impl.Get();
    REQUIRE(Rc(obj) == 2);

    ComPtr<Triangle> back = iface;        // dynamic down-cast onto the same node, +1 -> rc == 3
    REQUIRE(back.Get() == impl.Get());
    REQUIRE(Rc(obj) == 3);
    REQUIRE(iface);                       // copy left the source intact
}

TEST_CASE("Downcast move onto the same object transfers ownership without bumping", AUTO_TAG) {
    auto impl = MakeOwned<Triangle>();
    ComPtr<IShape> iface = impl;          // rc == 2
    RootObject* obj = impl.Get();

    ComPtr<Triangle> back = std::move(iface);
    REQUIRE(back.Get() == obj);
    REQUIRE(Rc(obj) == 2);                // count transferred, not bumped
    REQUIRE_FALSE(iface);                 // source emptied on a same-object move
}

TEST_CASE("Cross-cast to a non-facet interface yields null and leaves the copy source intact", AUTO_TAG) {
    auto impl = MakeOwned<Triangle>();
    RootObject* obj = impl.Get();
    REQUIRE(Rc(obj) == 1);

    ComPtr<IUnrelated> miss = impl;       // not in the closure -> null, no refcount change
    REQUIRE_FALSE(miss);
    REQUIRE(Rc(obj) == 1);
    REQUIRE(impl);                        // copy source untouched
}

TEST_CASE("A failed cross-cast move leaves the source intact (no silent destruction)", AUTO_TAG) {
    auto impl = MakeOwned<Triangle>();
    RootObject* obj = impl.Get();

    ComPtr<IUnrelated> miss = std::move(impl);
    REQUIRE_FALSE(miss);
    REQUIRE(impl);                        // a miss must NOT consume/destroy the source
    REQUIRE(Rc(obj) == 1);
}

TEST_CASE("Converting assignment flows through copy-and-swap", AUTO_TAG) {
    auto impl = MakeOwned<Triangle>();
    RootObject* obj = impl.Get();

    ComPtr<IShape> iface;
    iface = impl;                         // ComPtr<Triangle> -> by-value ComPtr<IShape> -> swap
    REQUIRE(iface);
    REQUIRE(static_cast<RootObject*>(iface.Get()) == obj);
    REQUIRE(Rc(obj) == 2);

    iface = ComPtr<IShape>{};             // releasing assignment drops the facet's +1
    REQUIRE_FALSE(iface);
    REQUIRE(Rc(obj) == 1);
}

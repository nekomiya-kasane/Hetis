#include <Yuki/Core/Query.h>
#include <Yuki/Core/Registry.h>
#include <Yuki/Core/RootObject.h>
#include <Yuki/Core/YObjectMacro.h>
#include <Yuki/Core/MakeOwned.h>
#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

using namespace Yuki;

namespace {
    // D5: Interface is a RootObject subclass. Single-base chain (IQA→RootObject,
    // QImpl→IQA) avoids the diamond and matches A1's
    // static_assert(sizeof(RootObject) == 2*sizeof(void*)) without forcing virtual
    // inheritance overhead. IQA's ctor forwards a role+external arg through so QImpl
    // can set its own role at construction without re-declaring RootObject as a
    // direct base.
    struct [[=Anno::Interface]] IQA : RootObject {
        IQA(ClassType role, bool external)
            : RootObject(role, nullptr, external) {}
        ~IQA() override = default;
        virtual int Answer() const = 0;
    };
    struct [[=Anno::Interface]] IQB : RootObject {
        IQB(ClassType role, bool external) : RootObject(role, nullptr, external) {}
        ~IQB() override = default;
    };

    struct [[=Anno::Implementation]]
           [[=Anno::Implements{^^IQA}]]
           QImpl : IQA {
      public:
        Y_OBJECT;
        QImpl() : IQA(ClassType::Implementation, /*external=*/false) {}
        ~QImpl() override = default;
        int Answer() const override { return 42; }
    };
}

TEST_CASE("Query<I>(node) takes the L0 fast path for a BOA provider", AUTO_TAG) {
    Registry::Install<QImpl>();
    auto impl = MakeOwned<QImpl>();
    ComPtr<IQA> a = Query<IQA>(impl.Get());
    REQUIRE(a);
    REQUIRE(a->Answer() == 42);
}

TEST_CASE("Query<I>(node) misses for an unimplemented interface", AUTO_TAG) {
    Registry::Install<QImpl>();
    auto impl = MakeOwned<QImpl>();
    ComPtr<IQB> b = Query<IQB>(impl.Get());
    REQUIRE(!b);
}

TEST_CASE("QueryDynamicRaw hits via L2 binary search", AUTO_TAG) {
    Registry::Install<QImpl>();
    auto impl = MakeOwned<QImpl>();
    const DispatchEntry* e = QueryDynamicRaw(impl->Meta().links, IidOf<IQA>());
    REQUIRE(e != nullptr);
    REQUIRE(e->iid == IidOf<IQA>());
}

TEST_CASE("ComPtr lifetime through Query keeps the node alive", AUTO_TAG) {
    Registry::Install<QImpl>();
    auto impl = MakeOwned<QImpl>();
    auto before = impl->PayloadRelaxed().refcount();
    {
        ComPtr<IQA> a = Query<IQA>(impl.Get());
        REQUIRE(impl->PayloadRelaxed().refcount() == before + 1);
    }
    REQUIRE(impl->PayloadRelaxed().refcount() == before);
}

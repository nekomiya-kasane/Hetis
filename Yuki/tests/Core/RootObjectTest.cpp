/**
 * @file RootObjectTest.cpp
 * @brief Tests for the object-model anchor in Yuki/Core/RootObject.h.
 *
 * Verifies the two reach paths return the same metaclass (non-virtual `MetaClassOf<T>` vs virtual
 * `p->DynamicMetaClass()`), that `MetaNode` injects the override with zero hand-written code, that
 * an object-model inheritance chain lets the most-derived node win, and that `RootObject` adds no
 * per-instance storage beyond a single vptr.
 */
#include <Yuki/Core/RootObject.h>

#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <memory>

using namespace Yuki;

namespace {

    struct [[=Anno::Implementation]] CircleImpl
        : MetaNode<CircleImpl> {
        double radius{1.0};
    };

    struct [[=Anno::Implementation]] SquareImpl
        : MetaNode<SquareImpl> {
        double side{2.0};
    };

    // Object-model inheritance: SpecialCircle chains off CircleImpl via MetaNode's Base param.
    struct [[=Anno::Implementation]] SpecialCircle
        : MetaNode<SpecialCircle, CircleImpl> {
        int flair{0};
    };

} // namespace

// =============================================================================
// Reach: non-virtual and virtual return the same metaclass
// =============================================================================

TEST_CASE("Both reach paths yield the same MetaClass", AUTO_TAG) {
    CircleImpl c;
    const RootObject& erased = c;
    REQUIRE(&erased.DynamicMetaClass() == &MetaClassOf<CircleImpl>);
    REQUIRE(&c.DynamicMetaClass() == &MetaClassOf<CircleImpl>);
}

TEST_CASE("DynamicMetaClass discriminates types through a base pointer", AUTO_TAG) {
    CircleImpl c;
    SquareImpl s;
    const RootObject* pc = &c;
    const RootObject* ps = &s;
    REQUIRE(pc->DynamicMetaClass().classType() == ClassType::Implementation);
    REQUIRE(pc->DynamicMetaClass().name() != ps->DynamicMetaClass().name());
    REQUIRE(&pc->DynamicMetaClass() == &MetaClassOf<CircleImpl>);
    REQUIRE(&ps->DynamicMetaClass() == &MetaClassOf<SquareImpl>);
}

// =============================================================================
// MetaNode injection: most-derived override wins, OM chain reflected
// =============================================================================

TEST_CASE("Most-derived MetaNode wins the override", AUTO_TAG) {
    SpecialCircle sc;
    const RootObject& erased = sc;
    REQUIRE(&erased.DynamicMetaClass() == &MetaClassOf<SpecialCircle>);
    // SpecialCircle's OM base is CircleImpl (its C++ base carrying Anno::Meta).
    REQUIRE(sc.DynamicMetaClass().baseMeta() == &MetaCoreOf<CircleImpl>);
    REQUIRE(sc.DynamicMetaClass().isAKindOf(MetaCoreOf<CircleImpl>));
}

// =============================================================================
// Storage: RootObject is just a vptr; MetaNode adds nothing
// =============================================================================

TEST_CASE("RootObject and MetaNode add no storage beyond a vptr", AUTO_TAG) {
    STATIC_REQUIRE(sizeof(RootObject) == sizeof(void*));
    STATIC_REQUIRE(std::is_polymorphic_v<RootObject>);
    // CircleImpl = vptr + double; MetaNode injects no extra bytes.
    STATIC_REQUIRE(sizeof(CircleImpl) == sizeof(void*) + sizeof(double));
}

TEST_CASE("Polymorphic destruction through RootObject works", AUTO_TAG) {
    std::unique_ptr<RootObject> p = std::make_unique<CircleImpl>();
    REQUIRE(&p->DynamicMetaClass() == &MetaClassOf<CircleImpl>);
}

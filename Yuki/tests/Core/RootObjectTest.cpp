/**
 * @file RootObjectTest.cpp
 * @brief Tests for the object-model anchor in Yuki/Core/RootObject.h.
 *
 * Verifies that the two reach paths return the same metaclass (non-virtual `MetaClassOf<T>` vs.
 * the virtual `p->MetaClassDynamic()`), that the role tag in the tagged payload pointer decodes
 * without a vcall, that the role-armed `RT::Target` / `RT::Extendee` / `RT::Facades` /
 * `RT::Underlying` / `RT::Nucleus` accessors are total, that `MetaNode` chains its `Base` parameter
 * for object-model inheritance, and that the per-instance footprint stays at exactly two words
 * (one vptr + one tagged payload).
 */
#include <Yuki/Core/RootObject.h>

#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <memory>

using namespace Yuki;

namespace {

    // --- Implementations ----------------------------------------------------

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

    // --- A minimal interface + facade for Target/Underlying tests -----------

    struct [[=Anno::Interface]] IShape : RootObject {
        virtual double Area() const = 0;
    };

    // An IfaceFacadeNode subobject — wires Interface arm into payload (target_ = CircleImpl*).
    struct [[=Anno::Bridge]] CircleShapeFacade
        : IfaceFacadeNode<CircleShapeFacade, IShape, CircleImpl> {
        using IfaceFacadeNode::IfaceFacadeNode;
        double Area() const override { return 3.14159 * target_->radius * target_->radius; }
    };

    // --- A minimal extension for Extendee/Nucleus tests ---------------------

    struct [[=Anno::Extension]] CircleExt
        : ExtensionNode<CircleExt, CircleImpl> {
        using ExtensionNode::ExtensionNode;
        int extra{0};
    };

} // namespace

// =============================================================================
// Reach: non-virtual and virtual return the same metaclass
// =============================================================================

TEST_CASE("Both reach paths yield the same MetaClass", AUTO_TAG) {
    CircleImpl c;
    const RootObject& erased = c;
    REQUIRE(&erased.MetaClassDynamic() == &MetaClassOf<CircleImpl>);
    REQUIRE(&c.MetaClassDynamic() == &MetaClassOf<CircleImpl>);
}

TEST_CASE("MetaClassDynamic discriminates types through a base pointer", AUTO_TAG) {
    CircleImpl c;
    SquareImpl s;
    const RootObject* pc = &c;
    const RootObject* ps = &s;
    REQUIRE(pc->MetaClassDynamic().classType() == ClassType::Implementation);
    REQUIRE(pc->MetaClassDynamic().name() != ps->MetaClassDynamic().name());
    REQUIRE(&pc->MetaClassDynamic() == &MetaClassOf<CircleImpl>);
    REQUIRE(&ps->MetaClassDynamic() == &MetaClassOf<SquareImpl>);
}

// =============================================================================
// Tagged payload: role decoded without a vcall, arms are total
// =============================================================================

TEST_CASE("TypeDynamic decodes the role from the payload tag", AUTO_TAG) {
    CircleImpl c;
    const RootObject* p = &c;
    REQUIRE(p->TypeDynamic() == ClassType::Implementation);
}

TEST_CASE("Implementation arm exposes the facade-chain head; other arms are null", AUTO_TAG) {
    CircleImpl c;
    RootObject* p = &c;
    REQUIRE(RT::Facades(p) != nullptr);  // implementations carry a facade chain head.
    REQUIRE(RT::Facades(p)->Empty());    // initially no runtime facade attached.
    REQUIRE(RT::Target(p) == nullptr);   // not a forwarding facade.
    REQUIRE(RT::Extendee(p) == nullptr); // not an extension.
}

TEST_CASE("RT accessors accept nullptr without faulting", AUTO_TAG) {
    REQUIRE(RT::Target(nullptr) == nullptr);
    REQUIRE(RT::Extendee(nullptr) == nullptr);
    REQUIRE(RT::Facades(nullptr) == nullptr);
    REQUIRE(RT::Underlying(nullptr) == nullptr);
    REQUIRE(RT::Nucleus(nullptr) == nullptr);
}

TEST_CASE("Underlying is idempotent on a non-facade", AUTO_TAG) {
    CircleImpl c;
    REQUIRE(RT::Underlying(static_cast<RootObject*>(&c)) == &c);
}

// =============================================================================
// Facade arm: Interface/Imposter/Bridge walk to the underlying
// =============================================================================

TEST_CASE("Interface facade reports target via RT::Target", AUTO_TAG) {
    CircleImpl c;
    CircleShapeFacade facade{&c};
    RootObject* fp = &facade;
    REQUIRE(fp->TypeDynamic() == ClassType::Interface);
    REQUIRE(RT::Target(fp) == static_cast<RootObject*>(&c));
    REQUIRE(RT::Extendee(fp) == nullptr);
    REQUIRE(RT::Facades(fp) == nullptr);
}

TEST_CASE("Underlying walks a facade to its impl", AUTO_TAG) {
    CircleImpl c;
    CircleShapeFacade facade{&c};
    REQUIRE(RT::Underlying(static_cast<RootObject*>(&facade)) == static_cast<RootObject*>(&c));
}

// =============================================================================
// Extension arm
// =============================================================================

TEST_CASE("Extension reports its extendee via RT::Extendee", AUTO_TAG) {
    CircleImpl c;
    CircleExt ext{&c};
    RootObject* ep = &ext;
    REQUIRE(ep->TypeDynamic() == ClassType::Extension);
    REQUIRE(RT::Extendee(ep) == static_cast<RootObject*>(&c));
    REQUIRE(RT::Target(ep) == nullptr);
    REQUIRE(RT::Facades(ep) == nullptr);
}

// =============================================================================
// Nucleus: the canonical impl reached through any facade/extension composition
// =============================================================================

TEST_CASE("Nucleus is idempotent on an Implementation", AUTO_TAG) {
    CircleImpl c;
    RootObject* p = &c;
    REQUIRE(RT::Nucleus(p) == p);
}

TEST_CASE("Nucleus walks a facade to its impl", AUTO_TAG) {
    CircleImpl c;
    CircleShapeFacade facade{&c};
    REQUIRE(RT::Nucleus(static_cast<RootObject*>(&facade)) == static_cast<RootObject*>(&c));
}

TEST_CASE("Nucleus walks an extension to its extendee impl", AUTO_TAG) {
    CircleImpl c;
    CircleExt ext{&c};
    REQUIRE(RT::Nucleus(static_cast<RootObject*>(&ext)) == static_cast<RootObject*>(&c));
}

TEST_CASE("Nucleus composes facade-over-anything and extension-over-anything", AUTO_TAG) {
    // Facade(Impl) → Impl
    CircleImpl c;
    CircleShapeFacade facade{&c};
    REQUIRE(RT::Nucleus(static_cast<RootObject*>(&facade)) == static_cast<RootObject*>(&c));

    // Extension(Impl) → Impl
    CircleExt ext{&c};
    REQUIRE(RT::Nucleus(static_cast<RootObject*>(&ext)) == static_cast<RootObject*>(&c));
}

TEST_CASE("Nucleus on a role-None RootObject returns it unchanged (no recursion)", AUTO_TAG) {
    // A default-constructed RootObject-derived stub with no annotation has role None; Nucleus
    // must terminate immediately rather than spinning through Underlying (which is idempotent
    // on non-facade) — guarding against the historical "Nuclei" infinite-recursion bug.
    struct Stub : RootObject {
        const MetaClass& MetaClassDynamic() const noexcept override { return MetaClassOf<CircleImpl>; }
    } stub;
    REQUIRE(stub.TypeDynamic() == ClassType::None);
    REQUIRE(RT::Nucleus(&stub) == &stub);
}

// =============================================================================
// MetaNode injection: most-derived override wins, OM chain reflected
// =============================================================================

TEST_CASE("Most-derived MetaNode wins the override", AUTO_TAG) {
    SpecialCircle sc;
    const RootObject& erased = sc;
    REQUIRE(&erased.MetaClassDynamic() == &MetaClassOf<SpecialCircle>);
    // SpecialCircle's OM base is CircleImpl (its C++ base carrying Anno::Meta).
    REQUIRE(sc.MetaClassDynamic().baseMeta() == &MetaCoreOf<CircleImpl>);
    REQUIRE(sc.MetaClassDynamic().isAKindOf(MetaCoreOf<CircleImpl>));
}

// =============================================================================
// Storage: RootObject is exactly two words; MetaNode adds the facade head
// =============================================================================

TEST_CASE("RootObject is exactly one vptr plus one tagged payload word", AUTO_TAG) {
    STATIC_REQUIRE(sizeof(RootObject) == 2 * sizeof(void*));
    STATIC_REQUIRE(std::is_polymorphic_v<RootObject>);
}

TEST_CASE("Polymorphic destruction through RootObject works", AUTO_TAG) {
    std::unique_ptr<RootObject> p = std::make_unique<CircleImpl>();
    REQUIRE(&p->MetaClassDynamic() == &MetaClassOf<CircleImpl>);
}

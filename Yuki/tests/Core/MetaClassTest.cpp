/**
 * @file MetaClassTest.cpp
 * @brief Tests for the three-layer metaclass in Yuki/Core/MetaClass.h.
 *
 * Covers the compile-time-baked @c MetaCore fields, the single-source-of-truth equivalence of the
 * static (`XxxOf<T>`) and dynamic (`mc.xxx()`) access surfaces, IID computation (annotation override
 * vs `StableHash`, RFC-4122 stamping, per-type distinctness), `extends` / `implements` resolution,
 * and `isAKindOf` chain traversal over the `omBase` / `extends` / `implements` edges.
 */
#include <Yuki/Core/MetaClass.h>

#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <meta>
#include <string_view>

using namespace Yuki;
using Mashiro::Uuid;

namespace {

    // --- Interfaces ----------------------------------------------------------

    struct [[=Anno::Interface]] IShape {
        virtual double Area() const = 0;
        virtual ~IShape() = default;
    };

    struct [[=Anno::Interface]] IDrawable {
        virtual void Draw() const = 0;
        virtual ~IDrawable() = default;
    };

    // Interface with a pinned identity (annotation override).
    struct [[=Anno::Interface]]
           [[=Iid("3f2504e0-4f89-41d3-9a0c-0305e82c3301")]] IPinned {
        virtual void Ping() const = 0;
        virtual ~IPinned() = default;
    };

    // --- Object-model inheritance chain (via C++ bases, both OM-annotated) ---

    struct [[=Anno::Implementation]] Base2D {
        int x;
    };

    struct [[=Anno::Implementation]] Derived3D : Base2D {
        int z;
    };

    // --- extends / implements edges -----------------------------------------

    inline constexpr std::meta::info kCircleImplements[] = {^^IShape, ^^IDrawable};

    struct [[=Anno::Meta{.type = ClassType::Implementation, .implements = kCircleImplements}]]
           CircleImpl {
        double radius;
    };

    inline constexpr std::meta::info kShinyExtends[] = {^^CircleImpl};

    // Stateful extension: declares one or more NSDMs on the Extension class itself.
    struct [[=Anno::Meta{.type = ClassType::Extension, .extends = kShinyExtends}]] ShinyExt {
        int gloss;
    };

    // --- Composable annotation forms ----------------------------------------

    // Stacked: edge-less role shorthand + a separate Implements annotation. The pipeline merges
    // the stacked Anno::Implements into the same `implements` edge set the full Meta would carry.
    struct [[=Anno::Implementation]] [[=Anno::Implements{^^IShape, ^^IDrawable}]] StackedImpl {
        double radius;
    };

    // Inline brace-list: the implements edges spelled directly in the Meta initializer, promoted
    // to static storage by InfoList's initializer_list constructor (no pre-declared array).
    struct [[=Anno::Meta{.type = ClassType::Implementation,
                         .implements = {^^IShape, ^^IDrawable}}]] InlineImpl {
        double radius;
    };

    // Stacked Extends: role shorthand + a separate Extends annotation, equivalent to ShinyExt.
    struct [[=Anno::Extension]] [[=Anno::Extends{^^CircleImpl}]] StackedExt {
        int gloss;
    };

} // namespace

// =============================================================================
// MetaCore fields — baked at compile time
// =============================================================================

TEST_CASE("MetaCore carries the role, name and identity", AUTO_TAG) {
    STATIC_REQUIRE(MetaCoreOf<IShape>.type == ClassType::Interface);
    STATIC_REQUIRE(MetaCoreOf<CircleImpl>.type == ClassType::Implementation);
    STATIC_REQUIRE(MetaCoreOf<IShape>.qualifiedName.find("IShape") != std::string_view::npos);
    STATIC_REQUIRE(MetaCoreOf<CircleImpl>.qualifiedName.find("CircleImpl") != std::string_view::npos);
    STATIC_REQUIRE(!MetaCoreOf<IShape>.iid.value.IsNil());
}

TEST_CASE("omBase is the nearest OM ancestor, nullptr at a root", AUTO_TAG) {
    STATIC_REQUIRE(MetaCoreOf<Base2D>.omBase == nullptr);
    STATIC_REQUIRE(MetaCoreOf<Derived3D>.omBase == &MetaCoreOf<Base2D>);
    STATIC_REQUIRE(BaseMetaOf<Derived3D> == &MetaCoreOf<Base2D>);
    STATIC_REQUIRE(BaseMetaOf<Base2D> == nullptr);
}

// =============================================================================
// extends / implements resolution
// =============================================================================

TEST_CASE("implements resolves to interface cores", AUTO_TAG) {
    STATIC_REQUIRE(MetaCoreOf<CircleImpl>.implements.size() == 2);
    STATIC_REQUIRE(MetaCoreOf<CircleImpl>.implements[0] == &MetaCoreOf<IShape>);
    STATIC_REQUIRE(MetaCoreOf<CircleImpl>.implements[1] == &MetaCoreOf<IDrawable>);
    STATIC_REQUIRE(MetaCoreOf<CircleImpl>.extends.empty());
}

TEST_CASE("extends resolves to the extended core", AUTO_TAG) {
    STATIC_REQUIRE(MetaCoreOf<ShinyExt>.extends.size() == 1);
    STATIC_REQUIRE(MetaCoreOf<ShinyExt>.extends[0] == &MetaCoreOf<CircleImpl>);
}

TEST_CASE("Stateful extension is reflected as Extension and satisfies StatefulExtensionClass", AUTO_TAG) {
    STATIC_REQUIRE(MetaCoreOf<ShinyExt>.type == ClassType::Extension);
    STATIC_REQUIRE(StatefulExtensionClass<ShinyExt>);
}

// =============================================================================
// Composable annotations: stacked role + edge annotation, and inline brace-lists
// =============================================================================

TEST_CASE("Stacked Implements merges into the implements edge set", AUTO_TAG) {
    STATIC_REQUIRE(MetaCoreOf<StackedImpl>.type == ClassType::Implementation);
    STATIC_REQUIRE(MetaCoreOf<StackedImpl>.implements.size() == 2);
    STATIC_REQUIRE(MetaCoreOf<StackedImpl>.implements[0] == &MetaCoreOf<IShape>);
    STATIC_REQUIRE(MetaCoreOf<StackedImpl>.implements[1] == &MetaCoreOf<IDrawable>);
}

TEST_CASE("Inline brace-list implements resolves like an array-backed Meta", AUTO_TAG) {
    STATIC_REQUIRE(MetaCoreOf<InlineImpl>.implements.size() == 2);
    STATIC_REQUIRE(MetaCoreOf<InlineImpl>.implements[0] == &MetaCoreOf<IShape>);
    STATIC_REQUIRE(MetaCoreOf<InlineImpl>.implements[1] == &MetaCoreOf<IDrawable>);
}

TEST_CASE("Stacked Extends merges into the extends edge set", AUTO_TAG) {
    STATIC_REQUIRE(MetaCoreOf<StackedExt>.type == ClassType::Extension);
    STATIC_REQUIRE(MetaCoreOf<StackedExt>.extends.size() == 1);
    STATIC_REQUIRE(MetaCoreOf<StackedExt>.extends[0] == &MetaCoreOf<CircleImpl>);
}

// =============================================================================
// IID — override vs StableHash, RFC-4122, distinctness
// =============================================================================

TEST_CASE("IidOf honours the annotation override", AUTO_TAG) {
    STATIC_REQUIRE(IidOf<IPinned>().value == Uuid::ParseOrThrow("3f2504e0-4f89-41d3-9a0c-0305e82c3301"));
}

TEST_CASE("IidOf falls back to StableHash when unannotated", AUTO_TAG) {
    STATIC_REQUIRE(IidOf<IShape>().value == StableHash<IShape>.value);
    STATIC_REQUIRE(IidOf<IDrawable>().value == StableHash<IDrawable>.value);
}

TEST_CASE("StableHash is RFC-4122 stamped and per-type distinct", AUTO_TAG) {
    STATIC_REQUIRE(StableHash<IShape>.value.Version() == 8);
    STATIC_REQUIRE(StableHash<IShape>.value.Variant() == 0x2);
    STATIC_REQUIRE(StableHash<IShape>.value != StableHash<IDrawable>.value);
    STATIC_REQUIRE(!StableHash<IShape>.value.IsNil());
}

// =============================================================================
// Static vs dynamic access surface — single source of truth
// =============================================================================

TEST_CASE("Static and dynamic surfaces agree on the same core", AUTO_TAG) {
    const MetaClass& mc = MetaClassOf<CircleImpl>;
    REQUIRE(mc.classType() == ClassTypeOf<CircleImpl>);
    REQUIRE(mc.iid() == IidOf<CircleImpl>());
    REQUIRE(mc.name() == Yuki::NameOf<CircleImpl>);
    REQUIRE(mc.baseMeta() == BaseMetaOf<CircleImpl>);
    REQUIRE(&mc.core() == &MetaCoreOf<CircleImpl>);
}

// =============================================================================
// isAKindOf — walks omBase / extends / implements
// =============================================================================

TEST_CASE("isAKindOf walks the omBase chain", AUTO_TAG) {
    REQUIRE(MetaClassOf<Derived3D>.isAKindOf(MetaCoreOf<Base2D>));
    REQUIRE(MetaClassOf<Derived3D>.isAKindOf(MetaClassOf<Base2D>));
    REQUIRE_FALSE(MetaClassOf<Base2D>.isAKindOf(MetaCoreOf<Derived3D>));
    REQUIRE(MetaClassOf<Derived3D>.isAKindOf(MetaCoreOf<Derived3D>));  // reflexive
}

TEST_CASE("isAKindOf walks implements and extends edges", AUTO_TAG) {
    REQUIRE(MetaClassOf<CircleImpl>.isAKindOf(MetaCoreOf<IShape>));
    REQUIRE(MetaClassOf<CircleImpl>.isAKindOf(MetaCoreOf<IDrawable>));
    REQUIRE(MetaClassOf<ShinyExt>.isAKindOf(MetaCoreOf<CircleImpl>));
    // Transitive: ShinyExt extends CircleImpl which implements IShape.
    REQUIRE(MetaClassOf<ShinyExt>.isAKindOf(MetaCoreOf<IShape>));
    REQUIRE_FALSE(MetaClassOf<IShape>.isAKindOf(MetaCoreOf<CircleImpl>));
}

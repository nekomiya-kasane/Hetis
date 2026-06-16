/**
 * @file IdentityTest.cpp
 * @brief Tests for the class-role taxonomy in Yuki/Core/Identity.h.
 *
 * The header is a pure compile-time facility, so almost every assertion is a `STATIC_REQUIRE` —
 * a regression fails the build itself. Coverage spans the `ClassType` enum properties, the
 * `Anno::Meta`-driven totality and cvref-insensitivity of `ClassTypeOf`, the `ClassTyped` /
 * `IsClassType` probes, and the true/false partition of every named role concept (including their
 * robustness on unannotated class types and on non-class types like `int`).
 */
#include <Yuki/Core/Identity.h>

#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <type_traits>

using namespace Yuki;
namespace T = Mashiro::Traits;

namespace {

    // --- Fixtures: one annotated type per non-None ClassType -----------------

    struct [[=Anno::Interface]] IShape {
        virtual ~IShape() = default;
    };

    struct [[=Anno::Implementation]] CircleImpl {
        int radius;
    };

    // Stateful extension: carries per-instance data, sizeof > 1.
    struct [[=Anno::Extension]] MoveExt {
        int dx;
        int dy;
    };

    // Stateless extension: empty class, sizeof == 1 (the C++ minimum).
    struct [[=Anno::Extension]] DrawExt {};

    struct [[=Anno::Imposter]] ShapeProxy {};

    struct [[=Anno::Bridge]] ShapeBridge {};

    // --- Fixtures: the negative space ---------------------------------------

    struct Plain {
        int value;
    };

} // namespace

// =============================================================================
// ClassType enum properties
// =============================================================================

TEST_CASE("ClassType is a sequential, single-byte enum", AUTO_TAG) {
    STATIC_REQUIRE(T::SequentialEnum<ClassType>);
    STATIC_REQUIRE(sizeof(ClassType) == 1);
    STATIC_REQUIRE(std::same_as<T::EnumUnderlying<ClassType>, std::uint8_t>);
    STATIC_REQUIRE(kClassTypeCount == 6);
    STATIC_REQUIRE(kClassTypeCount == T::EnumeratorsCount<ClassType>);
}

// =============================================================================
// ClassTypeOf — total, cvref-insensitive
// =============================================================================

TEST_CASE("ClassTypeOf reports the annotated role", AUTO_TAG) {
    STATIC_REQUIRE(ClassTypeOf<IShape> == ClassType::Interface);
    STATIC_REQUIRE(ClassTypeOf<CircleImpl> == ClassType::Implementation);
    STATIC_REQUIRE(ClassTypeOf<MoveExt> == ClassType::Extension);
    STATIC_REQUIRE(ClassTypeOf<DrawExt> == ClassType::Extension);
    STATIC_REQUIRE(ClassTypeOf<ShapeProxy> == ClassType::Imposter);
    STATIC_REQUIRE(ClassTypeOf<ShapeBridge> == ClassType::Bridge);
}

TEST_CASE("ClassTypeOf is None for unannotated and non-class types", AUTO_TAG) {
    STATIC_REQUIRE(ClassTypeOf<Plain> == ClassType::None);
    STATIC_REQUIRE(ClassTypeOf<int> == ClassType::None);
    STATIC_REQUIRE(ClassTypeOf<void> == ClassType::None);
    STATIC_REQUIRE(ClassTypeOf<ClassType> == ClassType::None);
}

TEST_CASE("ClassTypeOf ignores cv-qualifiers and references", AUTO_TAG) {
    STATIC_REQUIRE(ClassTypeOf<const IShape> == ClassType::Interface);
    STATIC_REQUIRE(ClassTypeOf<IShape&> == ClassType::Interface);
    STATIC_REQUIRE(ClassTypeOf<const CircleImpl&> == ClassType::Implementation);
    STATIC_REQUIRE(ClassTypeOf<volatile MoveExt> == ClassType::Extension);
}

// =============================================================================
// ClassTyped / IsClassType probes
// =============================================================================

TEST_CASE("ClassTyped detects annotation presence", AUTO_TAG) {
    STATIC_REQUIRE(ClassTyped<IShape>);
    STATIC_REQUIRE(ClassTyped<CircleImpl>);
    STATIC_REQUIRE(ClassTyped<ShapeBridge>);
    STATIC_REQUIRE_FALSE(ClassTyped<Plain>);
    STATIC_REQUIRE_FALSE(ClassTyped<int>);
}

TEST_CASE("IsClassType matches a specific role", AUTO_TAG) {
    STATIC_REQUIRE(IsClassType<IShape, ClassType::Interface>);
    STATIC_REQUIRE(IsClassType<CircleImpl, ClassType::Implementation>);
    STATIC_REQUIRE_FALSE(IsClassType<IShape, ClassType::Implementation>);
    STATIC_REQUIRE_FALSE(IsClassType<Plain, ClassType::Interface>);
    STATIC_REQUIRE_FALSE(IsClassType<int, ClassType::Interface>);
}

// =============================================================================
// Named role concepts — true/false partition
// =============================================================================

TEST_CASE("InterfaceClass spans plain interfaces and bridges", AUTO_TAG) {
    STATIC_REQUIRE(InterfaceClass<IShape>);
    STATIC_REQUIRE(InterfaceClass<ShapeBridge>);
    STATIC_REQUIRE_FALSE(InterfaceClass<CircleImpl>);
    STATIC_REQUIRE_FALSE(InterfaceClass<ShapeProxy>);
    STATIC_REQUIRE_FALSE(InterfaceClass<Plain>);
    STATIC_REQUIRE_FALSE(InterfaceClass<int>);
}

TEST_CASE("BridgeClass and ImposterClass are exact", AUTO_TAG) {
    STATIC_REQUIRE(BridgeClass<ShapeBridge>);
    STATIC_REQUIRE_FALSE(BridgeClass<IShape>);
    STATIC_REQUIRE_FALSE(BridgeClass<Plain>);

    STATIC_REQUIRE(ImposterClass<ShapeProxy>);
    STATIC_REQUIRE_FALSE(ImposterClass<IShape>);
    STATIC_REQUIRE_FALSE(ImposterClass<int>);
}

TEST_CASE("ImplementationClass is exact", AUTO_TAG) {
    STATIC_REQUIRE(ImplementationClass<CircleImpl>);
    STATIC_REQUIRE_FALSE(ImplementationClass<IShape>);
    STATIC_REQUIRE_FALSE(ImplementationClass<MoveExt>);
    STATIC_REQUIRE_FALSE(ImplementationClass<Plain>);
    STATIC_REQUIRE_FALSE(ImplementationClass<int>);
}

TEST_CASE("Extension concepts partition stateful and stateless extensions", AUTO_TAG) {
    STATIC_REQUIRE(StatefulExtensionClass<MoveExt>);
    STATIC_REQUIRE_FALSE(StatefulExtensionClass<DrawExt>);

    STATIC_REQUIRE(StatelessExtensionClass<DrawExt>);
    STATIC_REQUIRE_FALSE(StatelessExtensionClass<MoveExt>);

    STATIC_REQUIRE(ExtensionClass<MoveExt>);
    STATIC_REQUIRE(ExtensionClass<DrawExt>);
    STATIC_REQUIRE_FALSE(ExtensionClass<CircleImpl>);
    STATIC_REQUIRE_FALSE(ExtensionClass<IShape>);
    STATIC_REQUIRE_FALSE(ExtensionClass<Plain>);
    STATIC_REQUIRE_FALSE(ExtensionClass<int>);
}

TEST_CASE("Component class includes implementation and extension", AUTO_TAG) {
    STATIC_REQUIRE(ComponentClass<CircleImpl>);
    STATIC_REQUIRE(ComponentClass<MoveExt>);
    STATIC_REQUIRE(ComponentClass<DrawExt>);
    STATIC_REQUIRE_FALSE(ComponentClass<IShape>);
    STATIC_REQUIRE_FALSE(ComponentClass<ShapeBridge>);
    STATIC_REQUIRE_FALSE(ComponentClass<int>);
}

// =============================================================================
// Cross-cutting: every fixture lands in exactly one base role
// =============================================================================

TEST_CASE("Each fixture satisfies exactly one base-role concept", AUTO_TAG) {
    auto exactlyOne = []<typename U>() consteval {
        int n = 0;
        n += IsClassType<U, ClassType::Interface>;
        n += IsClassType<U, ClassType::Implementation>;
        n += IsClassType<U, ClassType::Extension>;
        n += IsClassType<U, ClassType::Imposter>;
        n += IsClassType<U, ClassType::Bridge>;
        return n;
    };
    STATIC_REQUIRE(exactlyOne.template operator()<IShape>() == 1);
    STATIC_REQUIRE(exactlyOne.template operator()<CircleImpl>() == 1);
    STATIC_REQUIRE(exactlyOne.template operator()<MoveExt>() == 1);
    STATIC_REQUIRE(exactlyOne.template operator()<DrawExt>() == 1);
    STATIC_REQUIRE(exactlyOne.template operator()<ShapeProxy>() == 1);
    STATIC_REQUIRE(exactlyOne.template operator()<ShapeBridge>() == 1);
    STATIC_REQUIRE(exactlyOne.template operator()<Plain>() == 0);
}

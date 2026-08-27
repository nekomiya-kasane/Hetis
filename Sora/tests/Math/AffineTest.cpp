/**
 * @file AffineTest.cpp
 * @brief Verify affine construction, representation selection, handedness, and layout semantics.
 * @ingroup Testing
 */

#include <Sora/Math/Affine.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <concepts>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace {

    using Sora::Math::Affine;
    using Sora::Math::AffineMatrix;
    using Sora::Math::AffineRepresentation;
    using Sora::Math::Handedness;
    using Sora::Math::Matrix;
    using Sora::Math::MatrixLayout;
    using Sora::Math::Point;
    using Sora::Math::Vector;

    struct NonZeroCarrier {
        struct Value {
            int value;
        };

        int value;

        NonZeroCarrier() = delete;
        constexpr explicit NonZeroCarrier(Value initialValue) noexcept : value(initialValue.value) {}
        constexpr NonZeroCarrier(const NonZeroCarrier&) = default;
        constexpr NonZeroCarrier(NonZeroCarrier&&) = default;
        constexpr NonZeroCarrier& operator=(const NonZeroCarrier&) = default;
        constexpr NonZeroCarrier& operator=(NonZeroCarrier&&) = default;
    };

    template<typename T>
    concept AffineType = requires { typename Affine<T, 2>; };

    template<typename T>
    concept TwoDimensionalRotationScalar = requires(T radians) { Sora::Math::Rotate(radians); };

    template<typename T>
    concept LookAtScalar =
        requires(Point<T, 3> point, Vector<T, 3> vector) { Sora::Math::TryLookAt(point, point, vector); };

    template<std::floating_point T>
    [[nodiscard]] constexpr bool Near(T left, T right, T tolerance = T{1.0e-10}) noexcept {
        return Sora::Math::AbsValue(left - right) <= tolerance;
    }

    template<typename T, size_t N>
    [[nodiscard]] constexpr bool Near(const Vector<T, N>& left, const Vector<T, N>& right,
                                      T tolerance = T{1.0e-10}) noexcept {
        for (size_t index = 0; index < N; ++index) {
            if (!Near(left[index], right[index], tolerance)) {
                return false;
            }
        }
        return true;
    }

    template<typename T, size_t Rows, size_t Columns, MatrixLayout LeftLayout, MatrixLayout RightLayout>
    [[nodiscard]] constexpr bool Near(const Matrix<T, Rows, Columns, LeftLayout>& left,
                                      const Matrix<T, Rows, Columns, RightLayout>& right,
                                      T tolerance = T{1.0e-10}) noexcept {
        for (size_t row = 0; row < Rows; ++row) {
            for (size_t column = 0; column < Columns; ++column) {
                if (!Near(left(row, column), right(row, column), tolerance)) {
                    return false;
                }
            }
        }
        return true;
    }

    template<AffineRepresentation Representation, typename Transform>
    [[nodiscard]] constexpr auto HomogeneousMatrix(const Transform& transform) noexcept {
        if constexpr (Representation == AffineRepresentation::Matrix) {
            return transform;
        } else {
            return Sora::Math::ToMatrix(transform);
        }
    }

    template<AffineRepresentation Representation, MatrixLayout Layout>
    [[nodiscard]] consteval bool BuilderFamiliesMatchCanonicalMatrices() {
        constexpr double quarterTurn = Sora::Math::Const::kHalfPi<double>;
        constexpr auto scale =
            HomogeneousMatrix<Representation>(Sora::Math::Scale<Representation, Layout>(Vector<double, 2>{2.0, 3.0}));
        constexpr auto uniform =
            HomogeneousMatrix<Representation>(Sora::Math::UniformScale<3, Representation, Layout>(2.0));
        constexpr auto rotate2 = HomogeneousMatrix<Representation>(
            Sora::Math::Rotate<Representation, Handedness::Right, Layout>(quarterTurn));
        constexpr auto rotateX = HomogeneousMatrix<Representation>(
            Sora::Math::RotateX<Representation, Handedness::Right, Layout>(quarterTurn));
        constexpr auto rotateY = HomogeneousMatrix<Representation>(
            Sora::Math::RotateY<Representation, Handedness::Right, Layout>(quarterTurn));
        constexpr auto rotateZ = HomogeneousMatrix<Representation>(
            Sora::Math::RotateZ<Representation, Handedness::Right, Layout>(quarterTurn));
        constexpr auto axis = Sora::Math::TryRotate<Representation, Handedness::Right, Layout>(
            Vector<double, 3>{0.0, 0.0, 1.0}, quarterTurn);
        constexpr auto lookAt = Sora::Math::TryLookAt<Representation, Handedness::Right, Layout>(
            Point<double, 3>{}, Point<double, 3>{0.0, 0.0, -1.0}, Vector<double, 3>{0.0, 1.0, 0.0});
        constexpr auto reflection = Sora::Math::TryReflect<Representation, Layout>(Vector<double, 2>{1.0, 0.0});
        if (!axis || !lookAt || !reflection) {
            return false;
        }

        const auto axisMatrix = HomogeneousMatrix<Representation>(*axis);
        const auto lookAtMatrix = HomogeneousMatrix<Representation>(*lookAt);
        const auto reflectionMatrix = HomogeneousMatrix<Representation>(*reflection);
        return Near(scale, Matrix<double, 3, 3, Layout>{2.0, 0.0, 0.0, 0.0, 3.0, 0.0, 0.0, 0.0, 1.0}) &&
               Near(uniform, Matrix<double, 4, 4, Layout>{2.0, 0.0, 0.0, 0.0, 0.0, 2.0, 0.0, 0.0, 0.0, 0.0, 2.0, 0.0,
                                                          0.0, 0.0, 0.0, 1.0}) &&
               Near(rotate2, Matrix<double, 3, 3, Layout>{0.0, -1.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0}) &&
               Near(rotateX, Matrix<double, 4, 4, Layout>{1.0, 0.0, 0.0, 0.0, 0.0, 0.0, -1.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                                                          0.0, 0.0, 0.0, 1.0}) &&
               Near(rotateY, Matrix<double, 4, 4, Layout>{0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, -1.0, 0.0, 0.0, 0.0,
                                                          0.0, 0.0, 0.0, 1.0}) &&
               Near(rotateZ, axisMatrix) && Near(lookAtMatrix, Matrix<double, 4, 4, Layout>::Identity()) &&
               Near(reflectionMatrix, Matrix<double, 3, 3, Layout>{-1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0});
    }

    template<typename Result, typename Error>
    void CheckError(const Result& result, Error expected) {
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == expected);
    }

    [[nodiscard]] constexpr double Determinant(const Matrix<double, 3, 3>& matrix) noexcept {
        return matrix(0, 0) * (matrix(1, 1) * matrix(2, 2) - matrix(1, 2) * matrix(2, 1)) -
               matrix(0, 1) * (matrix(1, 0) * matrix(2, 2) - matrix(1, 2) * matrix(2, 0)) +
               matrix(0, 2) * (matrix(1, 0) * matrix(2, 1) - matrix(1, 1) * matrix(2, 0));
    }

} // namespace

TEST_CASE("Affine representation policy selects exact zero-overhead result types", "[Sora][Math][Affine]") {
    using Compact = AffineMatrix<double, 2, MatrixLayout::RowMajor>;
    using Full = Matrix<double, 3, 3, MatrixLayout::RowMajor>;
    STATIC_REQUIRE(std::same_as<Compact, Matrix<double, 2, 3, MatrixLayout::RowMajor>>);
    STATIC_REQUIRE(
        std::same_as<Sora::Math::Traits::AffineResult<AffineRepresentation::Affine, double, 2>, Affine<double, 2>>);
    STATIC_REQUIRE(
        std::same_as<
            Sora::Math::Traits::AffineResult<AffineRepresentation::AffineMatrix, double, 2, MatrixLayout::RowMajor>,
            Compact>);
    STATIC_REQUIRE(
        std::same_as<Sora::Math::Traits::AffineResult<AffineRepresentation::Matrix, double, 2, MatrixLayout::RowMajor>,
                     Full>);
    STATIC_REQUIRE(TwoDimensionalRotationScalar<double>);
    STATIC_REQUIRE_FALSE(TwoDimensionalRotationScalar<int>);
    STATIC_REQUIRE(LookAtScalar<double>);
    STATIC_REQUIRE_FALSE(LookAtScalar<int>);
    STATIC_REQUIRE(Sora::Math::ValueCarrier<NonZeroCarrier>);
    STATIC_REQUIRE(AffineType<NonZeroCarrier>);
    constexpr Affine<NonZeroCarrier, 2> explicitAffine{
        Matrix<NonZeroCarrier, 2, 2>{NonZeroCarrier{{1}}, NonZeroCarrier{{0}}, NonZeroCarrier{{0}},
                                     NonZeroCarrier{{1}}},
        Vector<NonZeroCarrier, 2>{NonZeroCarrier{{2}}, NonZeroCarrier{{3}}}};
    STATIC_REQUIRE(explicitAffine.TranslationPart()[0].value == 2);
    STATIC_REQUIRE(sizeof(Affine<double, 3>) == 12 * sizeof(double));
    STATIC_REQUIRE(std::is_trivially_copyable_v<Affine<double, 3>>);
    STATIC_REQUIRE(Compact::kElementCount == 6);
    STATIC_REQUIRE(Full::kElementCount == 9);

    STATIC_REQUIRE(BuilderFamiliesMatchCanonicalMatrices<AffineRepresentation::Affine, MatrixLayout::ColumnMajor>());
    STATIC_REQUIRE(BuilderFamiliesMatchCanonicalMatrices<AffineRepresentation::Affine, MatrixLayout::RowMajor>());
    STATIC_REQUIRE(
        BuilderFamiliesMatchCanonicalMatrices<AffineRepresentation::AffineMatrix, MatrixLayout::ColumnMajor>());
    STATIC_REQUIRE(BuilderFamiliesMatchCanonicalMatrices<AffineRepresentation::AffineMatrix, MatrixLayout::RowMajor>());
    STATIC_REQUIRE(BuilderFamiliesMatchCanonicalMatrices<AffineRepresentation::Matrix, MatrixLayout::ColumnMajor>());
    STATIC_REQUIRE(BuilderFamiliesMatchCanonicalMatrices<AffineRepresentation::Matrix, MatrixLayout::RowMajor>());
}

TEST_CASE("Translation and scale builders are constexpr and preserve homogeneous laws", "[Sora][Math][Affine]") {
    constexpr Vector<double, 2> translation{3.0, -2.0};
    constexpr auto affine = Sora::Math::Translate(translation);
    constexpr auto compact =
        Sora::Math::Translate<AffineRepresentation::AffineMatrix, MatrixLayout::ColumnMajor>(translation);
    constexpr auto full = Sora::Math::Translate<AffineRepresentation::Matrix, MatrixLayout::ColumnMajor>(translation);
    constexpr auto scale = Sora::Math::Scale(Vector<double, 2>{2.0, -3.0});
    constexpr auto uniform = Sora::Math::UniformScale<3>(2.0);

    STATIC_REQUIRE(std::same_as<std::remove_cvref_t<decltype(affine)>, Affine<double, 2>>);
    STATIC_REQUIRE(std::same_as<std::remove_cvref_t<decltype(compact)>, AffineMatrix<double, 2>>);
    STATIC_REQUIRE(std::same_as<std::remove_cvref_t<decltype(full)>, Matrix<double, 3, 3>>);
    STATIC_REQUIRE(noexcept(Sora::Math::Translate(translation)));
    STATIC_REQUIRE(affine.LinearPart()(0, 0) == 1.0 && affine.TranslationPart() == translation);
    STATIC_REQUIRE(compact(0, 0) == 1.0 && compact(0, 2) == 3.0 && compact(1, 2) == -2.0);
    STATIC_REQUIRE(full(0, 2) == 3.0 && full(1, 2) == -2.0);
    STATIC_REQUIRE(full(2, 0) == 0.0 && full(2, 1) == 0.0 && full(2, 2) == 1.0);
    STATIC_REQUIRE(scale.LinearPart()(0, 0) == 2.0 && scale.LinearPart()(1, 1) == -3.0);
    STATIC_REQUIRE(uniform.LinearPart()(0, 0) == 2.0 && uniform.LinearPart()(2, 2) == 2.0);
}

TEST_CASE("Matrix layout changes storage only, never affine meaning", "[Sora][Math][Affine]") {
    constexpr Vector<double, 2> translation{3.0, -2.0};
    constexpr auto columnFull =
        Sora::Math::Translate<AffineRepresentation::Matrix, MatrixLayout::ColumnMajor>(translation);
    constexpr auto rowFull = Sora::Math::Translate<AffineRepresentation::Matrix, MatrixLayout::RowMajor>(translation);
    constexpr auto columnCompact =
        Sora::Math::Translate<AffineRepresentation::AffineMatrix, MatrixLayout::ColumnMajor>(translation);
    constexpr auto rowCompact =
        Sora::Math::Translate<AffineRepresentation::AffineMatrix, MatrixLayout::RowMajor>(translation);

    STATIC_REQUIRE(Near(columnFull, rowFull));
    STATIC_REQUIRE(Near(columnCompact, rowCompact));
    STATIC_REQUIRE(columnFull.Storage() == std::array{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 3.0, -2.0, 1.0});
    STATIC_REQUIRE(rowFull.Storage() == std::array{1.0, 0.0, 3.0, 0.0, 1.0, -2.0, 0.0, 0.0, 1.0});
    STATIC_REQUIRE(columnCompact.Storage() == std::array{1.0, 0.0, 0.0, 1.0, 3.0, -2.0});
    STATIC_REQUIRE(rowCompact.Storage() == std::array{1.0, 0.0, 3.0, 0.0, 1.0, -2.0});
}

TEST_CASE("Affine encodings round-trip non-symmetric linear maps", "[Sora][Math][Affine]") {
    constexpr Matrix<double, 2, 2, MatrixLayout::RowMajor> linear{1.0, 2.0, -3.0, 4.0};
    constexpr Vector<double, 2> translation{5.0, -6.0};
    constexpr auto affine = Sora::Math::MakeAffine(linear, translation);
    constexpr auto compact = Sora::Math::MakeAffine<AffineRepresentation::AffineMatrix>(linear, translation);
    constexpr auto full = Sora::Math::MakeAffine<AffineRepresentation::Matrix>(linear, translation);
    constexpr auto decoded = Sora::Math::ToAffine(compact);

    STATIC_REQUIRE(decoded.LinearPart() == linear);
    STATIC_REQUIRE(decoded.TranslationPart() == translation);
    STATIC_REQUIRE(Sora::Math::ToAffineMatrix(affine) == compact);
    STATIC_REQUIRE(Sora::Math::ToMatrix(affine) == full);
    STATIC_REQUIRE(Sora::Math::ToMatrix(compact) == full);
}

TEST_CASE("Three affine representations map points and vectors equivalently", "[Sora][Math][Affine]") {
    constexpr Vector<double, 2> translation{3.0, -2.0};
    constexpr Point<double, 2> point{2.0, -1.0};
    constexpr Vector<double, 2> vector{-4.0, 5.0};
    constexpr auto affine = Sora::Math::Translate(translation);
    constexpr auto compact = Sora::Math::Translate<AffineRepresentation::AffineMatrix>(translation);
    constexpr auto full = Sora::Math::Translate<AffineRepresentation::Matrix>(translation);

    STATIC_REQUIRE(Sora::Math::TransformPoint(affine, point) == Point<double, 2>{5.0, -3.0});
    STATIC_REQUIRE(Sora::Math::TransformPoint(compact, point) == Sora::Math::TransformPoint(affine, point));
    STATIC_REQUIRE(Sora::Math::TransformVector(compact, vector) == vector);
    STATIC_REQUIRE(Sora::Math::IsAffineEncoding(full));
    STATIC_REQUIRE(Sora::Math::TransformPoint(full, point) == Sora::Math::TransformPoint(affine, point));
    STATIC_REQUIRE(Sora::Math::TransformVector(full, vector) == vector);
    STATIC_REQUIRE(Sora::Math::TryTransformPoint(full, point) == Point<double, 2>{5.0, -3.0});
    STATIC_REQUIRE(Sora::Math::TryTransformVector(full, vector) == vector);

    constexpr Matrix<double, 3, 3> projective{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.25, 0.0, 1.0};
    STATIC_REQUIRE_FALSE(Sora::Math::IsAffineEncoding(projective));
    STATIC_REQUIRE_FALSE(Sora::Math::TryToAffine(projective).has_value());
    STATIC_REQUIRE(Sora::Math::TryToAffine(projective).error() == Sora::ErrorCode::InvalidArgument);

    constexpr auto shear = Sora::Math::Shear<0, 1, 2, AffineRepresentation::AffineMatrix>(2.0);
    STATIC_REQUIRE(shear(0, 1) == 2.0);
    STATIC_REQUIRE(shear(1, 0) == 0.0);
}

TEST_CASE("Rotations follow documented right- and left-handed positive angles", "[Sora][Math][Affine]") {
    const double quarterTurn = Sora::Math::Const::kHalfPi<double>;
    const auto rotate2 = Sora::Math::Rotate(quarterTurn);
    const auto rotateX = Sora::Math::RotateX(quarterTurn);
    const auto rotateY = Sora::Math::RotateY(quarterTurn);
    const auto rotateZ = Sora::Math::RotateZ(quarterTurn);
    CHECK(Near(Sora::Math::TransformVector(rotate2, Vector<double, 2>{1.0, 0.0}), Vector<double, 2>{0.0, 1.0}));
    CHECK(
        Near(Sora::Math::TransformVector(rotateX, Vector<double, 3>{0.0, 1.0, 0.0}), Vector<double, 3>{0.0, 0.0, 1.0}));
    CHECK(
        Near(Sora::Math::TransformVector(rotateY, Vector<double, 3>{0.0, 0.0, 1.0}), Vector<double, 3>{1.0, 0.0, 0.0}));
    CHECK(
        Near(Sora::Math::TransformVector(rotateZ, Vector<double, 3>{1.0, 0.0, 0.0}), Vector<double, 3>{0.0, 1.0, 0.0}));

    const auto left = Sora::Math::RotateZ<AffineRepresentation::Affine, Handedness::Left>(quarterTurn);
    const auto rightNegative = Sora::Math::RotateZ(-quarterTurn);
    CHECK(Near(left.LinearPart(), rightNegative.LinearPart()));

    CHECK(Near(Sora::Math::Rotate<AffineRepresentation::Affine, Handedness::Left>(quarterTurn).LinearPart(),
               Sora::Math::Rotate(-quarterTurn).LinearPart()));
    CHECK(Near(Sora::Math::RotateX<AffineRepresentation::Affine, Handedness::Left>(quarterTurn).LinearPart(),
               Sora::Math::RotateX(-quarterTurn).LinearPart()));
    CHECK(Near(Sora::Math::RotateY<AffineRepresentation::Affine, Handedness::Left>(quarterTurn).LinearPart(),
               Sora::Math::RotateY(-quarterTurn).LinearPart()));

    const auto axis = Sora::Math::TryNormalize(Vector<double, 3>{0.0, 0.0, 1.0});
    REQUIRE(axis.has_value());
    const auto axisRotation = Sora::Math::Rotate(*axis, quarterTurn);
    CHECK(Near(axisRotation.LinearPart(), rotateZ.LinearPart()));
}

TEST_CASE("General axis-angle rotation satisfies Rodrigues invariants", "[Sora][Math][Affine]") {
    const auto axis = Sora::Math::TryNormalize(Vector<double, 3>{1.0, 2.0, 3.0});
    REQUIRE(axis.has_value());
    const double angle = Sora::Math::Const::kPi<double> / 3.0;
    const auto rotation = Sora::Math::Rotate(*axis, angle);
    const auto [sine, cosine] = Sora::Math::SinCos(angle);
    const double oneMinusCosine = 1.0 - cosine;
    const Vector<double, 3>& a = axis->Value();
    const Matrix<double, 3, 3> expected{
        cosine + oneMinusCosine * a[0] * a[0],      oneMinusCosine * a[0] * a[1] - sine * a[2],
        oneMinusCosine * a[0] * a[2] + sine * a[1], oneMinusCosine * a[1] * a[0] + sine * a[2],
        cosine + oneMinusCosine * a[1] * a[1],      oneMinusCosine * a[1] * a[2] - sine * a[0],
        oneMinusCosine * a[2] * a[0] - sine * a[1], oneMinusCosine * a[2] * a[1] + sine * a[0],
        cosine + oneMinusCosine * a[2] * a[2]};

    CHECK(Near(rotation.LinearPart(), expected));
    CHECK(Near(Sora::Math::Transpose(rotation.LinearPart()) * rotation.LinearPart(), Matrix<double, 3, 3>::Identity()));
    CHECK(Near(Determinant(rotation.LinearPart()), 1.0));
    CHECK(Near(Sora::Math::TransformVector(rotation, a), a));

    const auto left = Sora::Math::Rotate<AffineRepresentation::Affine, Handedness::Left>(*axis, angle);
    const auto rightNegative = Sora::Math::Rotate(*axis, -angle);
    CHECK(Near(left.LinearPart(), rightNegative.LinearPart()));
}

TEST_CASE("Axis-angle construction rejects invalid free axes", "[Sora][Math][Affine]") {
    const auto zero = Sora::Math::TryRotate(Vector<double, 3>{}, 1.0);
    REQUIRE_FALSE(zero.has_value());
    CHECK(zero.error() == Sora::Math::NormalizationError::ZeroLength);

    const double infinity = std::numeric_limits<double>::infinity();
    const auto nonFinite = Sora::Math::TryRotate(Vector<double, 3>{infinity, 0.0, 0.0}, 1.0);
    REQUIRE_FALSE(nonFinite.has_value());
    CHECK(nonFinite.error() == Sora::Math::NormalizationError::NonFinite);

    CheckError(Sora::Math::TryRotate(Vector<double, 3>{1.0, 0.0, 0.0}, infinity),
               Sora::Math::NormalizationError::NonFinite);
    CheckError(Sora::Math::TryReflect(Vector<double, 3>{1.0, 0.0, 0.0}, infinity), Sora::ErrorCode::InvalidArgument);

    const auto nonUnit = Sora::Math::TryReflect(Vector<double, 2>{2.0, 0.0}, -2.0);
    REQUIRE(nonUnit.has_value());
    CHECK(Sora::Math::TransformPoint(*nonUnit, Point<double, 2>{3.0, 4.0}) == Point<double, 2>{-1.0, 4.0});

    const auto unrepresentable = Sora::Math::TryReflect(*Sora::Math::TryNormalize(Vector<double, 2>{1.0, 0.0}),
                                                        (std::numeric_limits<double>::max)());
    REQUIRE_FALSE(unrepresentable.has_value());
    CHECK(unrepresentable.error() == Sora::ErrorCode::OutOfRange);
}

TEST_CASE("LookAt is a world-to-view map with explicit handedness", "[Sora][Math][Affine]") {
    const Vector<double, 3> up{0.0, 1.0, 0.0};
    const auto right = Sora::Math::TryLookAt(Point<double, 3>{}, Point<double, 3>{0.0, 0.0, -1.0}, up);
    const auto left = Sora::Math::TryLookAt<AffineRepresentation::Affine, Handedness::Left>(
        Point<double, 3>{}, Point<double, 3>{0.0, 0.0, 1.0}, up);
    REQUIRE(right.has_value());
    REQUIRE(left.has_value());
    CHECK(Near(right->LinearPart(), Matrix<double, 3, 3>::Identity()));
    CHECK(Near(left->LinearPart(), Matrix<double, 3, 3>::Identity()));

    const Point<double, 3> eye{1.0, 2.0, 3.0};
    const Point<double, 3> target{2.0, 2.0, 3.0};
    const auto view = Sora::Math::TryLookAt(eye, target, up);
    REQUIRE(view.has_value());
    CHECK(Near(Sora::Math::ToVector(Sora::Math::TransformPoint(*view, eye)), Vector<double, 3>{}));
    const Point<double, 3> mappedTarget = Sora::Math::TransformPoint(*view, target);
    CHECK(Near(mappedTarget[0], 0.0));
    CHECK(Near(mappedTarget[1], 0.0));
    CHECK(mappedTarget[2] < 0.0);

    const auto rowView =
        Sora::Math::TryLookAt<AffineRepresentation::Affine, Handedness::Right, MatrixLayout::RowMajor>(eye, target, up);
    REQUIRE(rowView.has_value());
    CHECK(Near(rowView->LinearPart(), view->LinearPart()));
    CHECK(Near(rowView->TranslationPart(), view->TranslationPart()));

    const Point<double, 3> leftEye{1.0, -2.0, 3.0};
    const Point<double, 3> leftTarget{-2.0, 4.0, 5.0};
    const Vector<double, 3> leftUp{0.0, 1.0, 0.25};
    const auto leftAffine =
        Sora::Math::TryLookAt<AffineRepresentation::Affine, Handedness::Left, MatrixLayout::RowMajor>(
            leftEye, leftTarget, leftUp);
    const auto leftCompact =
        Sora::Math::TryLookAt<AffineRepresentation::AffineMatrix, Handedness::Left>(leftEye, leftTarget, leftUp);
    const auto leftFull =
        Sora::Math::TryLookAt<AffineRepresentation::Matrix, Handedness::Left>(leftEye, leftTarget, leftUp);
    REQUIRE(leftAffine.has_value());
    REQUIRE(leftCompact.has_value());
    REQUIRE(leftFull.has_value());
    CHECK(Near(Sora::Math::ToMatrix(*leftAffine), Sora::Math::ToMatrix(*leftCompact)));
    CHECK(Near(Sora::Math::ToMatrix(*leftAffine), *leftFull));
    CHECK(Near(Sora::Math::ToVector(Sora::Math::TransformPoint(*leftAffine, leftEye)), Vector<double, 3>{}));
    const Point<double, 3> leftMappedTarget = Sora::Math::TransformPoint(*leftAffine, leftTarget);
    CHECK(Near(leftMappedTarget[0], 0.0));
    CHECK(Near(leftMappedTarget[1], 0.0));
    CHECK(leftMappedTarget[2] > 0.0);
}

TEST_CASE("LookAt reports every invalid camera-frame category", "[Sora][Math][Affine]") {
    using Sora::Math::LookAtError;
    const Point<double, 3> eye{};
    const Point<double, 3> target{0.0, 0.0, -1.0};
    const Vector<double, 3> up{0.0, 1.0, 0.0};
    const double infinity = std::numeric_limits<double>::infinity();

    CheckError(Sora::Math::TryLookAt(Point<double, 3>{infinity, 0.0, 0.0}, target, up), LookAtError::NonFiniteEye);
    CheckError(Sora::Math::TryLookAt(eye, Point<double, 3>{0.0, infinity, 0.0}, up), LookAtError::NonFiniteTarget);
    CheckError(Sora::Math::TryLookAt(eye, target, Vector<double, 3>{0.0, 0.0, infinity}), LookAtError::NonFiniteUp);
    CheckError(Sora::Math::TryLookAt(eye, eye, up), LookAtError::CoincidentEyeAndTarget);
    CheckError(Sora::Math::TryLookAt(eye, target, Vector<double, 3>{}), LookAtError::ZeroUp);
    CheckError(Sora::Math::TryLookAt(eye, target, Vector<double, 3>{0.0, 0.0, 1.0}),
               LookAtError::CollinearUpAndViewDirection);
    CheckError(Sora::Math::TryLookAt(eye, target, Vector<double, 3>{0.0, 0.0, -1.0}),
               LookAtError::CollinearUpAndViewDirection);

    const auto nearCollinear = Sora::Math::TryLookAt(eye, target, Vector<double, 3>{0.0, 1.0e-300, 1.0});
    REQUIRE(nearCollinear.has_value());
    CHECK(Sora::Math::IsFinite(nearCollinear->LinearPart()(0, 0)));

    const double largest = std::numeric_limits<double>::max();
    const auto extreme =
        Sora::Math::TryLookAt(Point<double, 3>{largest, 0.0, 0.0}, Point<double, 3>{-largest, 0.0, 0.0}, up);
    REQUIRE(extreme.has_value());
    CHECK(Sora::Math::IsFinite(extreme->LinearPart()(0, 0)));

    CheckError(Sora::Math::TryLookAt(Point<double, 3>{largest, largest, 0.0}, Point<double, 3>{},
                                     Vector<double, 3>{0.0, 0.0, 1.0}),
               LookAtError::UnrepresentableViewTransform);
}

TEST_CASE("TryInverse closes singular and non-finite failure paths", "[Sora][Math][Affine]") {
    using Sora::ErrorCode;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    const double largest = std::numeric_limits<double>::max();
    const double smallest = std::numeric_limits<double>::min();

    CheckError(Sora::Math::TryInverse(Affine<double, 2>{Matrix<double, 2, 2>{nan, 0.0, 0.0, 1.0}, {}}),
               ErrorCode::InvalidArgument);
    CheckError(
        Sora::Math::TryInverse(Affine<double, 2>{Matrix<double, 2, 2>::Identity(), Vector<double, 2>{infinity, 0.0}}),
        ErrorCode::InvalidArgument);
    CheckError(Sora::Math::TryInverse(Affine<double, 2>{Matrix<double, 2, 2>{1.0, 2.0, 2.0, 4.0}, Vector<double, 2>{}}),
               ErrorCode::SingularMatrix);
    CheckError(Sora::Math::TryInverse(
                   Affine<double, 2>{Matrix<double, 2, 2>{smallest, largest, 0.0, 1.0}, Vector<double, 2>{}}),
               ErrorCode::OutOfRange);
    CheckError(Sora::Math::TryInverse(
                   Affine<double, 2>{Matrix<double, 2, 2>{0.5, 0.0, 0.0, 1.0}, Vector<double, 2>{largest, 0.0}}),
               ErrorCode::OutOfRange);

    const Affine<double, 2> affine{Matrix<double, 2, 2>{2.0, 1.0, -1.0, 3.0}, Vector<double, 2>{4.0, -5.0}};
    const auto inverse = Sora::Math::TryInverse(affine);
    REQUIRE(inverse.has_value());
    const Point<double, 2> point{7.0, -2.0};
    CHECK(Near(Sora::Math::ToVector(Sora::Math::TransformPoint(*inverse, Sora::Math::TransformPoint(affine, point))),
               Sora::Math::ToVector(point)));
}

TEST_CASE("Box transforms round outward across finite overflow", "[Sora][Math][Affine]") {
    constexpr float a = 14.6148443F;
    constexpr float x = 74.3891678F;
    constexpr float b = 6.14343023F;
    constexpr float y = 96.3450317F;
    constexpr float translation = 19.4322968F;
    const Affine<float, 2> affine{Matrix<float, 2, 2>{a, b, 0.0F, 1.0F},
                                  Vector<float, 2>{translation, 0.0F}};
    const Point<float, 2> corner{x, y};
    const auto transformed = Sora::Math::TryTransform(affine, Sora::Math::Box<float, 2>{corner, corner});
    REQUIRE(transformed.has_value());
    const auto transformedCorner = Sora::Math::TransformPoint(affine, corner);
    CHECK(transformed->Min()[0] <= transformedCorner[0]);
    CHECK(transformed->Max()[0] >= transformedCorner[0]);
    CHECK(transformed->Min()[1] <= transformedCorner[1]);
    CHECK(transformed->Max()[1] >= transformedCorner[1]);

    const float largest = (std::numeric_limits<float>::max)();
    const Affine<float, 2> overflowing{Matrix<float, 2, 2>{largest, largest, 0.0F, 1.0F}, {}};
    const Point<float, 2> cancelling{largest, -largest};
    const auto overflowingResult =
        Sora::Math::TryTransform(overflowing, Sora::Math::Box<float, 2>{cancelling, cancelling});
    REQUIRE(overflowingResult.has_value());
    CHECK(overflowingResult->Min()[0] == -std::numeric_limits<float>::infinity());
    CHECK(overflowingResult->Max()[0] == std::numeric_limits<float>::infinity());

    const float infinity = std::numeric_limits<float>::infinity();
    const auto unbounded = Sora::Math::TryTransform(
        Affine<float, 1>{}, Sora::Math::Box<float, 1>{Point<float, 1>{-infinity}, Point<float, 1>{infinity}});
    REQUIRE(unbounded.has_value());
    CHECK(unbounded->Min()[0] == -infinity);
    CHECK(unbounded->Max()[0] == infinity);

    constexpr int integerLargest = (std::numeric_limits<int>::max)();
    const auto integerOverflow = Sora::Math::TryTransform(Affine<int, 1>{Matrix<int, 1, 1>{integerLargest}, {}},
                                                           Sora::Math::Box<int, 1>{2, 2});
    REQUIRE_FALSE(integerOverflow.has_value());
    CHECK(integerOverflow.error() == Sora::ErrorCode::OutOfRange);

    const float denormal = (std::numeric_limits<float>::denorm_min)();
    const auto underflow = Sora::Math::TryTransform(
        Affine<float, 1>{Matrix<float, 1, 1>{0.5F}, {}}, Sora::Math::Box<float, 1>{denormal, denormal});
    REQUIRE(underflow.has_value());
    CHECK(underflow->Min()[0] == 0.0F);
    CHECK(underflow->Max()[0] == denormal);
}

TEST_CASE("Ball transforms preserve conservative containment", "[Sora][Math][Affine]") {
    constexpr float scale = 0.0078125F;
    constexpr auto transformed = Sora::Math::Transform(
        Affine<float, 1>{Matrix<float, 1, 1>{scale}, {}},
        Sora::Math::Ball<float, 1>{Point<float, 1>{}, 1.0F});
    STATIC_REQUIRE(transformed.Radius() >= scale);

    const float infinity = std::numeric_limits<float>::infinity();
    const auto translatedInfinite = Sora::Math::TryTransform(
        Affine<float, 1>{}, Sora::Math::Ball<float, 1>{Point<float, 1>{infinity}, 1.0F});
    REQUIRE(translatedInfinite.has_value());
    CHECK(translatedInfinite->Center()[0] == infinity);

    const auto collapsed = Sora::Math::TryTransform(
        Affine<float, 1>{Matrix<float, 1, 1>{0.0F}, {}},
        Sora::Math::Ball<float, 1>{Point<float, 1>{infinity}, infinity});
    REQUIRE(collapsed.has_value());
    CHECK(collapsed->Center()[0] == 0.0F);
    CHECK(collapsed->Radius() == 0.0F);

    const float largest = (std::numeric_limits<float>::max)();
    const auto unbounded = Sora::Math::TryTransform(Affine<float, 1>{Matrix<float, 1, 1>{largest}, {}},
                                                     Sora::Math::Ball<float, 1>{Point<float, 1>{}, 2.0F});
    REQUIRE(unbounded.has_value());
    CHECK(unbounded->Radius() == infinity);

    const float denormal = (std::numeric_limits<float>::denorm_min)();
    const auto underflow = Sora::Math::TryTransform(
        Affine<float, 1>{Matrix<float, 1, 1>{0.5F}, {}},
        Sora::Math::Ball<float, 1>{Point<float, 1>{}, denormal});
    REQUIRE(underflow.has_value());
    CHECK(underflow->Radius() >= denormal);
}

TEST_CASE("Integral affine accumulation preserves large cancelling terms", "[Sora][Math][Affine]") {
    constexpr int64_t largest = (std::numeric_limits<int64_t>::max)();
    constexpr auto affine = [=] {
        Matrix<int64_t, 6, 6> linear = Matrix<int64_t, 6, 6>::Identity();
        for (size_t column = 0; column < 6; ++column) {
            linear(0, column) = column < 3 ? largest : -largest;
        }
        return Affine<int64_t, 6>{linear, Vector<int64_t, 6>{}};
    }();
    constexpr Vector<int64_t, 6> vector{largest, largest, largest, largest, largest, largest};
    constexpr auto transformed = Sora::Math::TransformVector(affine, vector);

    STATIC_REQUIRE(transformed[0] == 0);
    STATIC_REQUIRE(transformed[1] == largest);
    STATIC_REQUIRE(transformed[5] == largest);
}

TEST_CASE("Reflection and composition preserve their affine meanings", "[Sora][Math][Affine]") {
    const auto normal = Sora::Math::TryNormalize(Vector<double, 2>{1.0, 0.0});
    REQUIRE(normal.has_value());
    const auto reflection = Sora::Math::Reflect(*normal, -2.0);
    CHECK(Near(Sora::Math::ToVector(Sora::Math::TransformPoint(reflection, Point<double, 2>{5.0, 3.0})),
               Vector<double, 2>{-1.0, 3.0}));

    const auto translation = Sora::Math::Translate(Vector<double, 2>{3.0, -2.0});
    const auto scale =
        Sora::Math::Scale<AffineRepresentation::Affine, MatrixLayout::RowMajor>(Vector<double, 2>{2.0, 4.0});
    const auto composed = Sora::Math::Compose(translation, scale);
    const Point<double, 2> point{1.0, 2.0};
    CHECK(Sora::Math::TransformPoint(composed, point) ==
          Sora::Math::TransformPoint(translation, Sora::Math::TransformPoint(scale, point)));

    const auto compactTranslation =
        Sora::Math::Translate<AffineRepresentation::AffineMatrix>(Vector<double, 2>{3.0, -2.0});
    const auto fullScale = Sora::Math::Scale<AffineRepresentation::Matrix>(Vector<double, 2>{2.0, 4.0});
    const auto compactComposed = Sora::Math::Compose(compactTranslation, fullScale);
    STATIC_REQUIRE(std::same_as<std::remove_cvref_t<decltype(compactComposed)>, AffineMatrix<double, 2>>);
    CHECK(Sora::Math::TransformPoint(compactComposed, point) == Sora::Math::TransformPoint(composed, point));

    const auto fullComposed = Sora::Math::Compose(fullScale, compactTranslation);
    STATIC_REQUIRE(std::same_as<std::remove_cvref_t<decltype(fullComposed)>, Matrix<double, 3, 3>>);
    CHECK(Sora::Math::TransformPoint(fullComposed, point) == Point<double, 2>{8.0, 0.0});
    const auto malformedComposition =
        Sora::Math::TryCompose(Matrix<double, 3, 3>{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.5, 0.0, 1.0}, compactTranslation);
    REQUIRE_FALSE(malformedComposition.has_value());
    CHECK(malformedComposition.error() == Sora::ErrorCode::InvalidArgument);

    constexpr int largest = (std::numeric_limits<int>::max)();
    constexpr Affine<int, 2> leftInt{Matrix<int, 2, 2>{largest, largest, 0, 1}, Vector<int, 2>{}};
    constexpr Affine<int, 2> rightInt{Matrix<int, 2, 2>{largest, 0, -largest, 1}, Vector<int, 2>{}};
    const auto exact = Sora::Math::TryCompose(leftInt, rightInt);
    REQUIRE(exact.has_value());
    CHECK(exact->LinearPart()(0, 0) == 0);
    CHECK(exact->LinearPart()(0, 1) == largest);

    const double infinity = std::numeric_limits<double>::infinity();
    const double doubleLargest = (std::numeric_limits<double>::max)();
    CheckError(Sora::Math::TryCompose(Affine<double, 1>{Matrix<double, 1, 1>{infinity}, Vector<double, 1>{}},
                                      Affine<double, 1>{Matrix<double, 1, 1>::Identity(), Vector<double, 1>{}}),
               Sora::ErrorCode::InvalidArgument);
    CheckError(Sora::Math::TryCompose(Affine<double, 1>{Matrix<double, 1, 1>{doubleLargest}, Vector<double, 1>{}},
                                      Affine<double, 1>{Matrix<double, 1, 1>{2.0}, Vector<double, 1>{}}),
               Sora::ErrorCode::OutOfRange);
    CheckError(Sora::Math::TryCompose(Affine<int, 1>{Matrix<int, 1, 1>{largest}, Vector<int, 1>{}},
                                      Affine<int, 1>{Matrix<int, 1, 1>{2}, Vector<int, 1>{}}),
               Sora::ErrorCode::OutOfRange);
}

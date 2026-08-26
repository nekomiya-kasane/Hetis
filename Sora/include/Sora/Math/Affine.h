/**
 * @file Affine.h
 * @brief Affine representations, construction, algebra, and geometry transformation laws.
 * @ingroup Math
 */
#pragma once

#include <Sora/Core/Contracts.h>
#include <Sora/ErrorCode.h>
#include <Sora/Math/Geom/Primitives.h>
#include <Sora/Math/GeomUtils.h>
#include <Sora/Math/Matrix.h>
#include <Sora/Math/Point.h>
#include <Sora/Math/ScalarMath.h>
#include <Sora/Math/ScalarUtils.h>
#include <Sora/Math/UnitVector.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <meta>
#include <ranges>
#include <type_traits>
#include <utility>

namespace Sora::Math {

    /** @brief Coordinate-system orientation used by direction-sensitive transform construction. */
    enum class Handedness : uint8_t {
        Right,
        Left,
    };

    /**
     * @brief Compile-time selection of an affine transform construction result.
     * @details @c Affine is the invariant-bearing semantic representation. @c AffineMatrix is the compact terminal
     * encoding `[linear | translation]`; @c Matrix is the full homogeneous terminal encoding. Algebra and geometry
     * operations use @c Affine unless an overload explicitly accepts a terminal encoding.
     */
    enum class AffineRepresentation : uint8_t {
        Affine,
        AffineMatrix,
        Matrix,
    };

    /** @brief Failure reported when world-to-view construction has no finite camera frame. */
    enum class LookAtError : uint8_t {
        NonFiniteEye,
        NonFiniteTarget,
        NonFiniteUp,
        CoincidentEyeAndTarget,
        IndeterminateViewDirection,
        ZeroUp,
        CollinearUpAndViewDirection,
        UnrepresentableViewTransform,
    };

    /** @brief Failure reported when an affine map cannot produce a valid ray direction. */
    enum class RayTransformError : uint8_t {
        IndeterminateOrigin,
        CollapsedDirection,
        IndeterminateDirection,
    };

    template<ValueCarrier T, size_t N, MatrixLayout Layout = MatrixLayout::ColumnMajor>
        requires(N > 0 && Detail::ZeroInitializable<T>)
    class Affine;

    /** @brief Compact affine matrix `[linear | translation]` with an implicit bottom row `[0 ... 0 1]`. */
    template<ValueCarrier T, size_t N, MatrixLayout Layout = MatrixLayout::ColumnMajor>
        requires(N > 0 && Detail::ZeroInitializable<T>)
    using AffineMatrix = Matrix<T, N, N + 1, Layout>;

    namespace Traits {

        /** @brief Type selected by an affine representation policy. */
        template<AffineRepresentation Representation, ValueCarrier T, size_t N,
                 MatrixLayout Layout = MatrixLayout::ColumnMajor>
            requires(N > 0 && Detail::ZeroInitializable<T> && (Representation == AffineRepresentation::Affine ||
                               Representation == AffineRepresentation::AffineMatrix ||
                               Representation == AffineRepresentation::Matrix))
        using AffineResult =
            std::conditional_t<Representation == AffineRepresentation::Affine, Affine<T, N, Layout>,
                               std::conditional_t<Representation == AffineRepresentation::AffineMatrix,
                                                  AffineMatrix<T, N, Layout>, Matrix<T, N + 1, N + 1, Layout>>>;

    } // namespace Traits

    /**
     * @brief Affine map `x -> linear * x + translation` in an @p N-dimensional space.
     * @details The representation is mathematical, not a GPU or interchange ABI. It has no hidden allocation and is
     * composed with the same fixed-size Matrix and Vector carriers used by the rest of Sora Math.
     */
    template<ValueCarrier T, size_t N, MatrixLayout Layout>
        requires(N > 0 && Detail::ZeroInitializable<T>)
    class Affine {
    public:
        using CarrierType = T;
        static constexpr size_t kDimension = N;
        static constexpr MatrixLayout kLayout = Layout;
        using LinearType = Matrix<T, N, N, Layout>;
        using TranslationType = Vector<T, N>;

        /** @brief Construct the identity affine map. */
        constexpr Affine() noexcept(Detail::kNothrowZeroCarrier<T>)
            : linear_(LinearType::Identity()), translation_{} {}

        /** @brief Construct an affine map from its linear and translation parts. */
        constexpr explicit Affine(LinearType linear, TranslationType translation) noexcept
            : linear_(std::move(linear)), translation_(std::move(translation)) {}

        /** @brief Return the identity map. */
        [[nodiscard]] static constexpr Affine Identity() noexcept(Detail::kNothrowZeroCarrier<T>) { return {}; }

        [[nodiscard]] constexpr const LinearType& LinearPart() const noexcept { return linear_; }
        [[nodiscard]] constexpr const TranslationType& TranslationPart() const noexcept { return translation_; }

    private:
        LinearType linear_;
        TranslationType translation_;
    };

    /** @brief Encode an affine map in compact matrix form without changing its logical layout. */
    template<ValueCarrier T, size_t N, MatrixLayout Layout>
    [[nodiscard]] constexpr AffineMatrix<T, N, Layout> ToAffineMatrix(const Affine<T, N, Layout>& affine) noexcept {
        AffineMatrix<T, N, Layout> result;
        template for (constexpr size_t row : std::define_static_array(std::views::iota(size_t{0}, N))) {
            template for (constexpr size_t column : std::define_static_array(std::views::iota(size_t{0}, N))) {
                result(row, column) = affine.LinearPart()(row, column);
            }
            result(row, N) = affine.TranslationPart()[row];
        }
        return result;
    }

    /** @brief Materialize an affine map as a homogeneous square matrix. */
    template<ValueCarrier T, size_t N, MatrixLayout Layout>
    [[nodiscard]] constexpr Matrix<T, N + 1, N + 1, Layout> ToMatrix(const Affine<T, N, Layout>& affine) noexcept {
        Matrix<T, N + 1, N + 1, Layout> result = Matrix<T, N + 1, N + 1, Layout>::Identity();
        template for (constexpr size_t row : std::define_static_array(std::views::iota(size_t{0}, N))) {
            template for (constexpr size_t column : std::define_static_array(std::views::iota(size_t{0}, N))) {
                result(row, column) = affine.LinearPart()(row, column);
            }
            result(row, N) = affine.TranslationPart()[row];
        }
        return result;
    }

    /** @brief Decode a compact affine matrix into the semantic affine-map representation. */
    template<ValueCarrier T, size_t N, MatrixLayout Layout>
    [[nodiscard]] constexpr Affine<T, N, Layout> ToAffine(const AffineMatrix<T, N, Layout>& matrix) noexcept {
        Matrix<T, N, N, Layout> linear;
        Vector<T, N> translation;
        template for (constexpr size_t row : std::define_static_array(std::views::iota(size_t{0}, N))) {
            template for (constexpr size_t column : std::define_static_array(std::views::iota(size_t{0}, N))) {
                linear(row, column) = matrix(row, column);
            }
            translation[row] = matrix(row, N);
        }
        return Affine<T, N, Layout>{std::move(linear), std::move(translation)};
    }

    /** @brief Materialize a compact affine matrix as a homogeneous square matrix. */
    template<ValueCarrier T, size_t N, MatrixLayout Layout>
    [[nodiscard]] constexpr Matrix<T, N + 1, N + 1, Layout>
    ToMatrix(const AffineMatrix<T, N, Layout>& matrix) noexcept {
        return ToMatrix(ToAffine(matrix));
    }

    namespace Detail {

        template<std::integral T, size_t Count>
            requires(Count > 0)
        [[nodiscard]] constexpr WideInteger<T>
        ExactIntegralSum(const std::array<WideInteger<T>, Count>& terms) noexcept {
            using Wide = WideInteger<T>;
            std::array<bool, Count> consumed{};
            Wide sum = 0;

            for (size_t count = 0; count < Count; ++count) {
                size_t selected = Count;
                if constexpr (std::is_signed_v<Wide>) {
                    for (size_t index = 0; index < Count; ++index) {
                        if (!consumed[index] && ((sum > 0 && terms[index] < 0) || (sum < 0 && terms[index] > 0))) {
                            selected = index;
                            break;
                        }
                    }
                }
                if (selected == Count) {
                    for (size_t index = 0; index < Count; ++index) {
                        if (!consumed[index]) {
                            selected = index;
                            break;
                        }
                    }
                }

                const Wide term = terms[selected];
                if constexpr (std::is_signed_v<Wide>) {
                    if (term > 0) {
                        SORA_PRE(sum <= (std::numeric_limits<Wide>::max)() - term);
                    } else if (term < 0) {
                        SORA_PRE(sum >= (std::numeric_limits<Wide>::lowest)() - term);
                    }
                } else {
                    SORA_PRE(sum <= (std::numeric_limits<Wide>::max)() - term);
                }
                sum += term;
                consumed[selected] = true;
            }
            return sum;
        }

        template<std::integral T, size_t N, typename Values, typename Coefficient>
        [[nodiscard]] constexpr T IntegralLinearCombination(T bias, const Values& values,
                                                            Coefficient&& coefficient) noexcept {
            std::array<WideInteger<T>, N + 1> terms{};
            terms[0] = static_cast<WideInteger<T>>(bias);
            for (size_t column = 0; column < N; ++column) {
                terms[column + 1] =
                    static_cast<WideInteger<T>>(coefficient(column)) * static_cast<WideInteger<T>>(values[column]);
            }
            return CheckedCast<T>(ExactIntegralSum<T>(terms));
        }

        template<std::floating_point T, size_t Rows, size_t Columns, MatrixLayout Layout>
        [[nodiscard]] constexpr bool IsFiniteMatrix(const Matrix<T, Rows, Columns, Layout>& matrix) noexcept {
            for (size_t row = 0; row < Rows; ++row) {
                for (size_t column = 0; column < Columns; ++column) {
                    if (!IsFinite(matrix(row, column))) {
                        return false;
                    }
                }
            }
            return true;
        }

        template<AffineRepresentation Representation, ValueCarrier T, size_t N, MatrixLayout Layout>
        [[nodiscard]] constexpr Traits::AffineResult<Representation, T, N, Layout>
        EncodeAffine(Affine<T, N, Layout> affine) noexcept {
            if constexpr (Representation == AffineRepresentation::Affine) {
                return affine;
            } else if constexpr (Representation == AffineRepresentation::AffineMatrix) {
                return ToAffineMatrix(affine);
            } else {
                return ToMatrix(affine);
            }
        }

        template<Handedness Hand, std::floating_point T>
            requires(Hand == Handedness::Right || Hand == Handedness::Left)
        [[nodiscard]] constexpr T OrientedSine(T sine) noexcept {
            if constexpr (Hand == Handedness::Right) {
                return sine;
            } else {
                return -sine;
            }
        }

        template<std::floating_point T, size_t N, MatrixLayout Layout>
        [[nodiscard]] constexpr T ConservativeRadiusScale(const Affine<T, N, Layout>& affine) noexcept {
            T oneNorm = T{0};
            T infinityNorm = T{0};
            for (size_t column = 0; column < N; ++column) {
                T sum = T{0};
                for (size_t row = 0; row < N; ++row) {
                    sum += AbsValue(affine.LinearPart()(row, column));
                }
                oneNorm = oneNorm > sum ? oneNorm : sum;
            }
            for (size_t row = 0; row < N; ++row) {
                T sum = T{0};
                for (size_t column = 0; column < N; ++column) {
                    sum += AbsValue(affine.LinearPart()(row, column));
                }
                infinityNorm = infinityNorm > sum ? infinityNorm : sum;
            }
            return Math::Sqrt(oneNorm) * Math::Sqrt(infinityNorm);
        }

        template<std::floating_point T, size_t N>
        [[nodiscard]] constexpr auto TryDirection(const Point<T, N>& from, const Point<T, N>& to) noexcept
            -> std::expected<UnitVector<T, N>, NormalizationError> {
            Vector<T, N> direction = to - from;
            auto normalized = TryNormalize(direction);
            if (normalized || normalized.error() != NormalizationError::NonFinite) {
                return normalized;
            }

            T scale = T{0};
            for (size_t axis = 0; axis < N; ++axis) {
                const T fromMagnitude = AbsValue(from[axis]);
                const T toMagnitude = AbsValue(to[axis]);
                scale = scale > fromMagnitude ? scale : fromMagnitude;
                scale = scale > toMagnitude ? scale : toMagnitude;
            }
            if (scale == T{0}) {
                return std::unexpected(NormalizationError::ZeroLength);
            }
            for (size_t axis = 0; axis < N; ++axis) {
                direction[axis] = to[axis] / scale - from[axis] / scale;
            }
            return TryNormalize(direction);
        }

    } // namespace Detail

    /** @brief Construct any supported affine representation from its linear and translation parts. */
    template<AffineRepresentation Representation = AffineRepresentation::Affine, ValueCarrier T, size_t N,
             MatrixLayout Layout>
    [[nodiscard]] constexpr Traits::AffineResult<Representation, T, N, Layout>
    MakeAffine(Matrix<T, N, N, Layout> linear, Vector<T, N> translation) noexcept {
        return Detail::EncodeAffine<Representation>(Affine<T, N, Layout>{std::move(linear), std::move(translation)});
    }

    /** @brief Construct a pure translation in any positive dimension. */
    template<AffineRepresentation Representation = AffineRepresentation::Affine,
             MatrixLayout Layout = MatrixLayout::ColumnMajor, ValueCarrier T, size_t N>
    [[nodiscard]] constexpr Traits::AffineResult<Representation, T, N, Layout>
    Translate(Vector<T, N> translation) noexcept {
        return MakeAffine<Representation>(Matrix<T, N, N, Layout>::Identity(), std::move(translation));
    }

    /** @brief Construct a coordinate-wise scale in any positive dimension; zero and negative factors are valid. */
    template<AffineRepresentation Representation = AffineRepresentation::Affine,
             MatrixLayout Layout = MatrixLayout::ColumnMajor, ValueCarrier T, size_t N>
    [[nodiscard]] constexpr Traits::AffineResult<Representation, T, N, Layout>
    Scale(const Vector<T, N>& scale) noexcept {
        Matrix<T, N, N, Layout> linear;
        template for (constexpr size_t axis : std::define_static_array(std::views::iota(size_t{0}, N))) {
            linear(axis, axis) = scale[axis];
        }
        return MakeAffine<Representation>(std::move(linear), Vector<T, N>{});
    }

    /** @brief Construct a uniform scale in dimension @p N. */
    template<size_t N, AffineRepresentation Representation = AffineRepresentation::Affine,
             MatrixLayout Layout = MatrixLayout::ColumnMajor, ValueCarrier T>
        requires(N > 0)
    [[nodiscard]] constexpr Traits::AffineResult<Representation, T, N, Layout> UniformScale(T scale) noexcept {
        Vector<T, N> factors;
        template for (constexpr size_t axis : std::define_static_array(std::views::iota(size_t{0}, N))) {
            factors[axis] = scale;
        }
        return Scale<Representation, Layout>(factors);
    }

    /**
     * @brief Construct a two-dimensional rotation.
     * @details Positive angles follow the right-hand rule for @c Right and the opposite orientation for @c Left.
     */
    template<AffineRepresentation Representation = AffineRepresentation::Affine, Handedness Hand = Handedness::Right,
             MatrixLayout Layout = MatrixLayout::ColumnMajor, std::floating_point T>
        requires(Hand == Handedness::Right || Hand == Handedness::Left)
    [[nodiscard]] constexpr Traits::AffineResult<Representation, T, 2, Layout> Rotate(T radians) noexcept {
        SORA_PRE(IsFinite(radians));
        const auto [rawSine, cosine] = SinCos(radians);
        const T sine = Detail::OrientedSine<Hand>(rawSine);
        Matrix<T, 2, 2, Layout> linear{cosine, -sine, sine, cosine};
        return MakeAffine<Representation>(std::move(linear), Vector<T, 2>{});
    }

    /** @brief Construct a three-dimensional rotation about the positive X axis. */
    template<AffineRepresentation Representation = AffineRepresentation::Affine, Handedness Hand = Handedness::Right,
             MatrixLayout Layout = MatrixLayout::ColumnMajor, std::floating_point T>
        requires(Hand == Handedness::Right || Hand == Handedness::Left)
    [[nodiscard]] constexpr Traits::AffineResult<Representation, T, 3, Layout> RotateX(T radians) noexcept {
        SORA_PRE(IsFinite(radians));
        const auto [rawSine, cosine] = SinCos(radians);
        const T sine = Detail::OrientedSine<Hand>(rawSine);
        Matrix<T, 3, 3, Layout> linear{T{1}, T{0}, T{0}, T{0}, cosine, -sine, T{0}, sine, cosine};
        return MakeAffine<Representation>(std::move(linear), Vector<T, 3>{});
    }

    /** @brief Construct a three-dimensional rotation about the positive Y axis. */
    template<AffineRepresentation Representation = AffineRepresentation::Affine, Handedness Hand = Handedness::Right,
             MatrixLayout Layout = MatrixLayout::ColumnMajor, std::floating_point T>
        requires(Hand == Handedness::Right || Hand == Handedness::Left)
    [[nodiscard]] constexpr Traits::AffineResult<Representation, T, 3, Layout> RotateY(T radians) noexcept {
        SORA_PRE(IsFinite(radians));
        const auto [rawSine, cosine] = SinCos(radians);
        const T sine = Detail::OrientedSine<Hand>(rawSine);
        Matrix<T, 3, 3, Layout> linear{cosine, T{0}, sine, T{0}, T{1}, T{0}, -sine, T{0}, cosine};
        return MakeAffine<Representation>(std::move(linear), Vector<T, 3>{});
    }

    /** @brief Construct a three-dimensional rotation about the positive Z axis. */
    template<AffineRepresentation Representation = AffineRepresentation::Affine, Handedness Hand = Handedness::Right,
             MatrixLayout Layout = MatrixLayout::ColumnMajor, std::floating_point T>
        requires(Hand == Handedness::Right || Hand == Handedness::Left)
    [[nodiscard]] constexpr Traits::AffineResult<Representation, T, 3, Layout> RotateZ(T radians) noexcept {
        SORA_PRE(IsFinite(radians));
        const auto [rawSine, cosine] = SinCos(radians);
        const T sine = Detail::OrientedSine<Hand>(rawSine);
        Matrix<T, 3, 3, Layout> linear{cosine, -sine, T{0}, sine, cosine, T{0}, T{0}, T{0}, T{1}};
        return MakeAffine<Representation>(std::move(linear), Vector<T, 3>{});
    }

    /** @brief Construct a three-dimensional axis-angle rotation around an established unit axis. */
    template<AffineRepresentation Representation = AffineRepresentation::Affine, Handedness Hand = Handedness::Right,
             MatrixLayout Layout = MatrixLayout::ColumnMajor, std::floating_point T>
        requires(Hand == Handedness::Right || Hand == Handedness::Left)
    [[nodiscard]] constexpr Traits::AffineResult<Representation, T, 3, Layout> Rotate(const UnitVector<T, 3>& axis,
                                                                                      T radians) noexcept {
        SORA_PRE(IsFinite(radians));
        const auto [rawSine, cosine] = SinCos(radians);
        const T sine = Detail::OrientedSine<Hand>(rawSine);
        const T oneMinusCosine = T{1} - cosine;
        const auto& a = axis.Value();
        Matrix<T, 3, 3, Layout> linear{
            cosine + oneMinusCosine * a[0] * a[0],      oneMinusCosine * a[0] * a[1] - sine * a[2],
            oneMinusCosine * a[0] * a[2] + sine * a[1], oneMinusCosine * a[1] * a[0] + sine * a[2],
            cosine + oneMinusCosine * a[1] * a[1],      oneMinusCosine * a[1] * a[2] - sine * a[0],
            oneMinusCosine * a[2] * a[0] - sine * a[1], oneMinusCosine * a[2] * a[1] + sine * a[0],
            cosine + oneMinusCosine * a[2] * a[2]};
        return MakeAffine<Representation>(std::move(linear), Vector<T, 3>{});
    }

    /** @brief Normalize a free axis and construct a three-dimensional axis-angle rotation. */
    template<AffineRepresentation Representation = AffineRepresentation::Affine, Handedness Hand = Handedness::Right,
             MatrixLayout Layout = MatrixLayout::ColumnMajor, std::floating_point T>
        requires(Hand == Handedness::Right || Hand == Handedness::Left)
    [[nodiscard]] constexpr auto TryRotate(const Vector<T, 3>& axis, T radians) noexcept
        -> std::expected<Traits::AffineResult<Representation, T, 3, Layout>, NormalizationError> {
        if (!IsFinite(radians)) {
            return std::unexpected(NormalizationError::NonFinite);
        }
        const auto unitAxis = TryNormalize(axis);
        if (!unitAxis) {
            return std::unexpected(unitAxis.error());
        }
        return Rotate<Representation, Hand, Layout>(*unitAxis, radians);
    }

    /**
     * @brief Construct a world-to-view affine map from an eye point, target point, and approximate up direction.
     * @details A right-handed view looks along negative Z; a left-handed view looks along positive Z.
     */
    template<AffineRepresentation Representation = AffineRepresentation::Affine, Handedness Hand = Handedness::Right,
             MatrixLayout Layout = MatrixLayout::ColumnMajor, std::floating_point T>
        requires(Hand == Handedness::Right || Hand == Handedness::Left)
    [[nodiscard]] constexpr auto TryLookAt(const Point<T, 3>& eye, const Point<T, 3>& target,
                                           const Vector<T, 3>& up) noexcept
        -> std::expected<Traits::AffineResult<Representation, T, 3, Layout>, LookAtError> {
        if (!IsFinite(eye)) {
            return std::unexpected(LookAtError::NonFiniteEye);
        }
        if (!IsFinite(target)) {
            return std::unexpected(LookAtError::NonFiniteTarget);
        }
        if (!IsFinite(up)) {
            return std::unexpected(LookAtError::NonFiniteUp);
        }
        if (eye == target) {
            return std::unexpected(LookAtError::CoincidentEyeAndTarget);
        }

        const auto forwardUnit = Detail::TryDirection(eye, target);
        if (!forwardUnit) {
            return std::unexpected(LookAtError::IndeterminateViewDirection);
        }
        const auto upUnit = TryNormalize(up);
        if (!upUnit) {
            return std::unexpected(LookAtError::ZeroUp);
        }

        const Vector<T, 3> forward = forwardUnit->Value();
        const Vector<T, 3> z = Hand == Handedness::Right ? -forward : forward;
        const auto xUnit = TryNormalize(Cross(upUnit->Value(), z));
        if (!xUnit) {
            return std::unexpected(LookAtError::CollinearUpAndViewDirection);
        }
        const Vector<T, 3> x = xUnit->Value();
        const Vector<T, 3> y = Cross(z, x);
        const Vector<T, 3> eyeVector = ToVector(eye);

        Matrix<T, 3, 3, Layout> linear{x[0], x[1], x[2], y[0], y[1], y[2], z[0], z[1], z[2]};
        Vector<T, 3> translation{-Dot(x, eyeVector), -Dot(y, eyeVector), -Dot(z, eyeVector)};
        if (!IsFinite(translation)) {
            return std::unexpected(LookAtError::UnrepresentableViewTransform);
        }
        return MakeAffine<Representation>(std::move(linear), std::move(translation));
    }

    /** @brief Construct reflection across a hyperplane represented by a unit normal and signed offset. */
    template<AffineRepresentation Representation = AffineRepresentation::Affine,
             MatrixLayout Layout = MatrixLayout::ColumnMajor, std::floating_point T, size_t N>
    [[nodiscard]] constexpr Traits::AffineResult<Representation, T, N, Layout> Reflect(const UnitVector<T, N>& normal,
                                                                                       T offset = T{0}) noexcept {
        SORA_PRE(IsFinite(offset));
        Matrix<T, N, N, Layout> linear = Matrix<T, N, N, Layout>::Identity();
        Vector<T, N> translation;
        template for (constexpr size_t row : std::define_static_array(std::views::iota(size_t{0}, N))) {
            template for (constexpr size_t column : std::define_static_array(std::views::iota(size_t{0}, N))) {
                linear(row, column) -= T{2} * normal[row] * normal[column];
            }
            translation[row] = T{-2} * offset * normal[row];
        }
        return MakeAffine<Representation>(std::move(linear), std::move(translation));
    }

    /** @brief Construct reflection across an existing normalized hyperplane. */
    template<AffineRepresentation Representation = AffineRepresentation::Affine,
             MatrixLayout Layout = MatrixLayout::ColumnMajor, std::floating_point T, size_t N>
    [[nodiscard]] constexpr Traits::AffineResult<Representation, T, N, Layout>
    Reflect(const Hyperplane<T, N>& plane) noexcept {
        return Reflect<Representation, Layout>(plane.Normal(), plane.Offset());
    }

    /** @brief Normalize a free normal and construct reflection across its hyperplane. */
    template<AffineRepresentation Representation = AffineRepresentation::Affine,
             MatrixLayout Layout = MatrixLayout::ColumnMajor, std::floating_point T, size_t N>
    [[nodiscard]] constexpr auto TryReflect(const Vector<T, N>& normal, T offset = T{0}) noexcept
        -> std::expected<Traits::AffineResult<Representation, T, N, Layout>, NormalizationError> {
        if (!IsFinite(offset)) {
            return std::unexpected(NormalizationError::NonFinite);
        }
        const auto unitNormal = TryNormalize(normal);
        if (!unitNormal) {
            return std::unexpected(unitNormal.error());
        }
        return Reflect<Representation, Layout>(*unitNormal, offset);
    }

    /** @brief Apply an affine map to an affine point; integral results must be representable. */
    template<ValueCarrier T, std::size_t N, MatrixLayout Layout>
    [[nodiscard]] constexpr Point<T, N> TransformPoint(const Affine<T, N, Layout>& affine,
                                                       const Point<T, N>& point) noexcept {
        if constexpr (std::integral<T>) {
            Point<T, N> result;
            for (std::size_t row = 0; row < N; ++row) {
                result[row] = Detail::IntegralLinearCombination<T, N>(
                    affine.TranslationPart()[row], point,
                    [&](size_t column) constexpr noexcept { return affine.LinearPart()(row, column); });
            }
            return result;
        }
        return ToPoint(affine.LinearPart() * ToVector(point)) + affine.TranslationPart();
    }

    /** @brief Apply only the linear part of an affine map; integral results must be representable. */
    template<ValueCarrier T, std::size_t N, MatrixLayout Layout>
    [[nodiscard]] constexpr Vector<T, N> TransformVector(const Affine<T, N, Layout>& affine,
                                                         const Vector<T, N>& vector) noexcept {
        if constexpr (std::integral<T>) {
            Vector<T, N> result;
            for (std::size_t row = 0; row < N; ++row) {
                result[row] = Detail::IntegralLinearCombination<T, N>(
                    T{0}, vector, [&](size_t column) constexpr noexcept { return affine.LinearPart()(row, column); });
            }
            return result;
        }
        return affine.LinearPart() * vector;
    }

    /** @brief Apply a compact affine matrix to an affine point. */
    template<ValueCarrier T, std::size_t N, MatrixLayout Layout>
    [[nodiscard]] constexpr Point<T, N> TransformPoint(const AffineMatrix<T, N, Layout>& affine,
                                                       const Point<T, N>& point) noexcept {
        Point<T, N> result;
        for (size_t row = 0; row < N; ++row) {
            if constexpr (std::integral<T>) {
                result[row] = Detail::IntegralLinearCombination<T, N>(
                    affine(row, N), point, [&](size_t column) constexpr noexcept { return affine(row, column); });
            } else {
                T value = affine(row, N);
                for (size_t column = 0; column < N; ++column) {
                    if consteval {
                        value = Add(Mul(affine(row, column), point[column]), value);
                    } else {
                        value = Fma(affine(row, column), point[column], value);
                    }
                }
                result[row] = value;
            }
        }
        return result;
    }

    /** @brief Apply only the linear block of a compact affine matrix to a free vector. */
    template<ValueCarrier T, std::size_t N, MatrixLayout Layout>
    [[nodiscard]] constexpr Vector<T, N> TransformVector(const AffineMatrix<T, N, Layout>& affine,
                                                         const Vector<T, N>& vector) noexcept {
        Vector<T, N> result;
        for (size_t row = 0; row < N; ++row) {
            if constexpr (std::integral<T>) {
                result[row] = Detail::IntegralLinearCombination<T, N>(
                    T{0}, vector, [&](size_t column) constexpr noexcept { return affine(row, column); });
            } else {
                T value = T{0};
                for (size_t column = 0; column < N; ++column) {
                    if consteval {
                        value = Add(Mul(affine(row, column), vector[column]), value);
                    } else {
                        value = Fma(affine(row, column), vector[column], value);
                    }
                }
                result[row] = value;
            }
        }
        return result;
    }

    /** @brief Compose two affine maps so that `Compose(left, right)(x) = left(right(x))`. */
    template<ValueCarrier T, std::size_t N, MatrixLayout LeftLayout, MatrixLayout RightLayout>
    [[nodiscard]] constexpr Affine<T, N, LeftLayout> Compose(const Affine<T, N, LeftLayout>& left,
                                                             const Affine<T, N, RightLayout>& right) noexcept {
        return Affine<T, N, LeftLayout>{left.LinearPart() * right.LinearPart(),
                                        left.LinearPart() * right.TranslationPart() + left.TranslationPart()};
    }

    /** @brief Return the inverse of a nonsingular affine map. */
    template<std::floating_point T, std::size_t N, MatrixLayout Layout>
    [[nodiscard]] constexpr std::expected<Affine<T, N, Layout>, Sora::ErrorCode>
    TryInverse(const Affine<T, N, Layout>& affine) noexcept {
        if (!Detail::IsFiniteMatrix(affine.LinearPart()) || !IsFinite(affine.TranslationPart())) {
            return std::unexpected(Sora::ErrorCode::InvalidArgument);
        }

        std::array<std::array<T, 2 * N>, N> augmented{};
        for (std::size_t row = 0; row < N; ++row) {
            for (std::size_t column = 0; column < N; ++column) {
                augmented[row][column] = affine.LinearPart()(row, column);
            }
            augmented[row][N + row] = T{1};
        }

        for (std::size_t column = 0; column < N; ++column) {
            std::size_t pivot = column;
            for (std::size_t row = column + 1; row < N; ++row) {
                if (AbsValue(augmented[row][column]) > AbsValue(augmented[pivot][column])) {
                    pivot = row;
                }
            }
            if (augmented[pivot][column] == T{0}) {
                return std::unexpected(Sora::ErrorCode::SingularMatrix);
            }
            if (pivot != column) {
                std::swap(augmented[pivot], augmented[column]);
            }

            const T inversePivot = T{1} / augmented[column][column];
            if (!IsFinite(inversePivot)) {
                return std::unexpected(Sora::ErrorCode::OutOfRange);
            }
            for (std::size_t value = 0; value < 2 * N; ++value) {
                augmented[column][value] *= inversePivot;
                if (!IsFinite(augmented[column][value])) {
                    return std::unexpected(Sora::ErrorCode::OutOfRange);
                }
            }
            for (std::size_t row = 0; row < N; ++row) {
                if (row == column) {
                    continue;
                }
                const T factor = augmented[row][column];
                for (std::size_t value = 0; value < 2 * N; ++value) {
                    augmented[row][value] -= factor * augmented[column][value];
                    if (!IsFinite(augmented[row][value])) {
                        return std::unexpected(Sora::ErrorCode::OutOfRange);
                    }
                }
            }
        }

        typename Affine<T, N, Layout>::LinearType inverseLinear;
        for (std::size_t row = 0; row < N; ++row) {
            for (std::size_t column = 0; column < N; ++column) {
                inverseLinear(row, column) = augmented[row][N + column];
            }
        }
        Vector<T, N> inverseTranslation = -(inverseLinear * affine.TranslationPart());
        if (!IsFinite(inverseTranslation)) {
            return std::unexpected(Sora::ErrorCode::OutOfRange);
        }
        return Affine<T, N, Layout>{std::move(inverseLinear), std::move(inverseTranslation)};
    }

    /** @brief Transform a ray when the affine map preserves a valid origin and non-zero direction. */
    template<std::floating_point T, size_t N, MatrixLayout Layout>
    [[nodiscard]] constexpr auto TryTransform(const Affine<T, N, Layout>& affine, const Ray<T, N>& ray) noexcept
        -> std::expected<Ray<T, N>, RayTransformError> {
        const Point<T, N> origin = TransformPoint(affine, ray.Origin());
        if (IsNaN(origin)) {
            return std::unexpected(RayTransformError::IndeterminateOrigin);
        }
        const Vector<T, N> direction = TransformVector(affine, ray.Direction());
        if (IsNaN(direction)) {
            return std::unexpected(RayTransformError::IndeterminateDirection);
        }
        if (!(SquaredNorm(direction) > T{0})) {
            return std::unexpected(RayTransformError::CollapsedDirection);
        }
        return Ray<T, N>{origin, direction};
    }

    /**
     * @brief Transform an axis-aligned box by exact coordinate-interval propagation.
     * @details Runs in `O(N^2)` time and does not enumerate the box's `2^N` corners.
     * @pre Integral results are representable in @p T.
     */
    template<Sora::Concept::ArithmeticScalar T, size_t N, MatrixLayout Layout>
    [[nodiscard]] constexpr Box<T, N> Transform(const Affine<T, N, Layout>& affine, const Box<T, N>& box) noexcept {
        Point<T, N> minimum;
        Point<T, N> maximum;
        for (size_t row = 0; row < N; ++row) {
            if constexpr (std::integral<T>) {
                std::array<WideInteger<T>, N + 1> lowerTerms{};
                std::array<WideInteger<T>, N + 1> upperTerms{};
                lowerTerms[0] = static_cast<WideInteger<T>>(affine.TranslationPart()[row]);
                upperTerms[0] = lowerTerms[0];
                for (size_t column = 0; column < N; ++column) {
                    const WideInteger<T> coefficient = static_cast<WideInteger<T>>(affine.LinearPart()(row, column));
                    if (coefficient == WideInteger<T>{0}) {
                        continue;
                    }
                    const WideInteger<T> first = coefficient * static_cast<WideInteger<T>>(box.Min()[column]);
                    const WideInteger<T> second = coefficient * static_cast<WideInteger<T>>(box.Max()[column]);
                    lowerTerms[column + 1] = first < second ? first : second;
                    upperTerms[column + 1] = first > second ? first : second;
                }
                minimum[row] = CheckedCast<T>(Detail::ExactIntegralSum<T>(lowerTerms));
                maximum[row] = CheckedCast<T>(Detail::ExactIntegralSum<T>(upperTerms));
            } else {
                T lower = affine.TranslationPart()[row];
                T upper = lower;
                for (size_t column = 0; column < N; ++column) {
                    const T coefficient = affine.LinearPart()(row, column);
                    if (coefficient == T{0}) {
                        continue;
                    }
                    const T first = coefficient * box.Min()[column];
                    const T second = coefficient * box.Max()[column];
                    lower += first < second ? first : second;
                    upper += first > second ? first : second;
                }
                minimum[row] = lower;
                maximum[row] = upper;
            }
        }
        return Box<T, N>{std::move(minimum), std::move(maximum)};
    }

    /** @brief Transform a base/extent rectangle and return its fitted axis-aligned box. */
    template<Sora::Concept::ArithmeticScalar T, size_t N, MatrixLayout Layout>
    [[nodiscard]] constexpr Box<T, N> Transform(const Affine<T, N, Layout>& affine, const Rect<T, N>& rect) noexcept {
        return Transform(affine, rect.ToBox());
    }

    /** @brief Transform a ball conservatively under arbitrary linear distortion. */
    template<std::floating_point T, size_t N, MatrixLayout Layout>
    [[nodiscard]] constexpr Ball<T, N> Transform(const Affine<T, N, Layout>& affine, const Ball<T, N>& ball) noexcept {
        const T scale = Detail::ConservativeRadiusScale(affine);
        const T radius = scale == T{0} ? T{0} : ball.Radius() * scale;
        return Ball<T, N>{TransformPoint(affine, ball.Center()), radius};
    }

    /** @brief Transform a normalized hyperplane through the inverse-transpose covector law. */
    template<std::floating_point T, size_t N, MatrixLayout Layout>
    [[nodiscard]] constexpr auto TryTransform(const Affine<T, N, Layout>& affine,
                                              const Hyperplane<T, N>& plane) noexcept
        -> std::expected<Hyperplane<T, N>, Sora::ErrorCode> {
        if (!IsFinite(plane.Offset())) {
            return std::unexpected(Sora::ErrorCode::InvalidArgument);
        }
        const auto inverse = TryInverse(affine);
        if (!inverse) {
            return std::unexpected(inverse.error());
        }
        const Vector<T, N> covector = Transpose(inverse->LinearPart()) * plane.Normal().Value();
        const T offset = plane.Offset() - Dot(covector, affine.TranslationPart());
        if (!IsFinite(covector) || !IsFinite(offset)) {
            return std::unexpected(Sora::ErrorCode::OutOfRange);
        }

        const auto unitNormal = TryNormalize(covector);
        if (!unitNormal) {
            return std::unexpected(Sora::ErrorCode::OutOfRange);
        }
        T scale = T{0};
        for (size_t axis = 0; axis < N; ++axis) {
            const T magnitude = AbsValue(covector[axis]);
            scale = scale > magnitude ? scale : magnitude;
        }
        Vector<T, N> scaled;
        for (size_t axis = 0; axis < N; ++axis) {
            scaled[axis] = covector[axis] / scale;
        }
        const T normalizedOffset = (offset / scale) / Norm(scaled);
        if (!IsFinite(normalizedOffset)) {
            return std::unexpected(Sora::ErrorCode::OutOfRange);
        }
        return Hyperplane<T, N>{*unitNormal, normalizedOffset};
    }

    /** @brief Transform all raw clip planes of a frustum through the inverse-transpose covector law. */
    template<std::floating_point T, MatrixLayout Layout>
    [[nodiscard]] constexpr auto TryTransform(const Affine<T, 3, Layout>& affine, const Frustum<T>& frustum) noexcept
        -> std::expected<Frustum<T>, Sora::ErrorCode> {
        for (const auto& plane : frustum.planes) {
            if (!IsFinite(plane.normal) || !IsFinite(plane.offset)) {
                return std::unexpected(Sora::ErrorCode::InvalidArgument);
            }
        }
        const auto inverse = TryInverse(affine);
        if (!inverse) {
            return std::unexpected(inverse.error());
        }
        const auto inverseTranspose = Transpose(inverse->LinearPart());
        Frustum<T> result;
        for (size_t index = 0; index < result.planes.size(); ++index) {
            result.planes[index].normal = inverseTranspose * frustum.planes[index].normal;
            result.planes[index].offset =
                frustum.planes[index].offset - Dot(result.planes[index].normal, affine.TranslationPart());
            if (!IsFinite(result.planes[index].normal) || !IsFinite(result.planes[index].offset)) {
                return std::unexpected(Sora::ErrorCode::OutOfRange);
            }
        }
        return result;
    }

} // namespace Sora::Math

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
#include <bit>
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
        requires(N > 0)
    class Affine;

    /** @brief Compact affine matrix `[linear | translation]` with an implicit bottom row `[0 ... 0 1]`. */
    template<ValueCarrier T, size_t N, MatrixLayout Layout = MatrixLayout::ColumnMajor>
        requires(N > 0)
    using AffineMatrix = Matrix<T, N, N + 1, Layout>;

    namespace Traits {

        /** @brief Type selected by an affine representation policy. */
        template<AffineRepresentation Representation, ValueCarrier T, size_t N,
                 MatrixLayout Layout = MatrixLayout::ColumnMajor>
            requires(N > 0 && (Representation == AffineRepresentation::Affine ||
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
        requires(N > 0)
    class Affine {
    public:
        using CarrierType = T;
        static constexpr size_t kDimension = N;
        static constexpr MatrixLayout kLayout = Layout;
        using LinearType = Matrix<T, N, N, Layout>;
        using TranslationType = Vector<T, N>;

        /** @brief Construct the identity affine map. */
        constexpr Affine() noexcept(kNothrowZeroValue<T> && kNothrowOneValue<T>)
            requires(ZeroInitializable<T> && OneInitializable<T>)
            : linear_(LinearType::Identity()), translation_{} {}

        /** @brief Construct an affine map from its linear and translation parts. */
        constexpr explicit Affine(LinearType linear, TranslationType translation) noexcept(
            std::is_nothrow_move_constructible_v<LinearType> && std::is_nothrow_move_constructible_v<TranslationType>)
            : linear_(std::move(linear)), translation_(std::move(translation)) {}

        /** @brief Return the identity map. */
        [[nodiscard]] static constexpr Affine Identity() noexcept(kNothrowZeroValue<T> &&
                                                                  kNothrowOneValue<T>)
            requires(ZeroInitializable<T> && OneInitializable<T>)
        {
            return {};
        }

        [[nodiscard]] constexpr const LinearType& LinearPart() const noexcept { return linear_; }
        [[nodiscard]] constexpr const TranslationType& TranslationPart() const noexcept { return translation_; }

    private:
        LinearType linear_;
        TranslationType translation_;
    };

    namespace Detail {

        template<size_t Index, ValueCarrier T, size_t N, MatrixLayout Layout>
        [[nodiscard]] constexpr T
        AffineMatrixValue(const Affine<T, N, Layout>& affine) noexcept(std::is_nothrow_copy_constructible_v<T>) {
            constexpr size_t columnCount = N + 1;
            constexpr size_t row = Index / columnCount;
            constexpr size_t column = Index % columnCount;
            if constexpr (column == N) {
                return affine.TranslationPart()[row];
            } else {
                return affine.LinearPart()(row, column);
            }
        }

        template<size_t Index, ValueCarrier T, size_t N, MatrixLayout Layout>
        [[nodiscard]] constexpr T
        LinearMatrixValue(const AffineMatrix<T, N, Layout>& matrix) noexcept(std::is_nothrow_copy_constructible_v<T>) {
            constexpr size_t columnCount = N;
            constexpr size_t row = Index / columnCount;
            constexpr size_t column = Index % columnCount;
            return matrix(row, column);
        }

        template<size_t Index, ValueCarrier T, size_t N, MatrixLayout Layout>
        [[nodiscard]] constexpr T
        TranslationValue(const AffineMatrix<T, N, Layout>& matrix) noexcept(std::is_nothrow_copy_constructible_v<T>) {
            return matrix(Index, N);
        }

        template<size_t Index, ValueCarrier T, size_t N, MatrixLayout Layout>
            requires(ZeroInitializable<T> && OneInitializable<T>)
        [[nodiscard]] constexpr T HomogeneousMatrixValue(const Affine<T, N, Layout>& affine) noexcept(
            std::is_nothrow_copy_constructible_v<T> && kNothrowZeroValue<T> && kNothrowOneValue<T>) {
            constexpr size_t dimension = N + 1;
            constexpr size_t row = Index / dimension;
            constexpr size_t column = Index % dimension;
            if constexpr (row < N && column < N) {
                return affine.LinearPart()(row, column);
            } else if constexpr (row < N) {
                return affine.TranslationPart()[row];
            } else if constexpr (column < N) {
                return ZeroValue<T>();
            } else {
                return OneValue<T>();
            }
        }

    } // namespace Detail

    /** @brief Encode an affine map in compact matrix form without changing its logical layout. */
    template<ValueCarrier T, size_t N, MatrixLayout Layout>
    [[nodiscard]] constexpr AffineMatrix<T, N, Layout>
    ToAffineMatrix(const Affine<T, N, Layout>& affine) noexcept(std::is_nothrow_copy_constructible_v<T> &&
                                                                std::is_nothrow_move_constructible_v<T>) {
        return [&]<size_t... Indices>(std::index_sequence<Indices...>) {
            return AffineMatrix<T, N, Layout>{Detail::AffineMatrixValue<Indices>(affine)...};
        }(std::make_index_sequence<N*(N + 1)>{});
    }

    /** @brief Materialize an affine map as a homogeneous square matrix. */
    template<ValueCarrier T, size_t N, MatrixLayout Layout>
        requires(ZeroInitializable<T> && OneInitializable<T>)
    [[nodiscard]] constexpr Matrix<T, N + 1, N + 1, Layout> ToMatrix(const Affine<T, N, Layout>& affine) noexcept(
        std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_move_constructible_v<T> &&
        kNothrowZeroValue<T> && kNothrowOneValue<T>) {
        return [&]<size_t... Indices>(std::index_sequence<Indices...>) {
            return Matrix<T, N + 1, N + 1, Layout>{Detail::HomogeneousMatrixValue<Indices>(affine)...};
        }(std::make_index_sequence<(N + 1) * (N + 1)>{});
    }

    /** @brief Decode a compact affine matrix into the semantic affine-map representation. */
    template<ValueCarrier T, size_t N, MatrixLayout Layout>
    [[nodiscard]] constexpr Affine<T, N, Layout>
    ToAffine(const AffineMatrix<T, N, Layout>& matrix) noexcept(std::is_nothrow_copy_constructible_v<T> &&
                                                                std::is_nothrow_move_constructible_v<T>) {
        auto linear = [&]<size_t... Indices>(std::index_sequence<Indices...>) {
            return Matrix<T, N, N, Layout>{Detail::LinearMatrixValue<Indices>(matrix)...};
        }(std::make_index_sequence<N * N>{});
        auto translation = [&]<size_t... Indices>(std::index_sequence<Indices...>) {
            return Vector<T, N>{Detail::TranslationValue<Indices>(matrix)...};
        }(std::make_index_sequence<N>{});
        return Affine<T, N, Layout>{std::move(linear), std::move(translation)};
    }

    /** @brief Materialize a compact affine matrix as a homogeneous square matrix. */
    template<ValueCarrier T, size_t N, MatrixLayout Layout>
        requires(ZeroInitializable<T> && OneInitializable<T>)
    [[nodiscard]] constexpr Matrix<T, N + 1, N + 1, Layout> ToMatrix(const AffineMatrix<T, N, Layout>& matrix) noexcept(
        std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_move_constructible_v<T> &&
        kNothrowZeroValue<T> && kNothrowOneValue<T>) {
        return ToMatrix(ToAffine(matrix));
    }

    /** @brief Return whether a homogeneous square matrix encodes an affine map. */
    template<ValueCarrier T, size_t Dimension, MatrixLayout Layout>
        requires(Dimension > 1 && ZeroInitializable<T> && OneInitializable<T> &&
                 requires(T left, T right) {
                     { left == right } -> std::convertible_to<bool>;
                 })
    [[nodiscard]] constexpr bool IsAffineEncoding(const Matrix<T, Dimension, Dimension, Layout>& matrix) noexcept(
        noexcept(matrix(0, 0) == matrix(0, 0))) {
        constexpr size_t last = Dimension - 1;
        for (size_t column = 0; column < last; ++column) {
            if (!(matrix(last, column) == ZeroValue<T>())) {
                return false;
            }
        }
        return matrix(last, last) == OneValue<T>();
    }

    /** @brief Decode a full homogeneous affine encoding, rejecting projective matrices. */
    template<ValueCarrier T, size_t Dimension, MatrixLayout Layout>
        requires(Dimension > 1 && ZeroInitializable<T> && OneInitializable<T> &&
                 requires(const Matrix<T, Dimension, Dimension, Layout>& matrix) {
                     { IsAffineEncoding(matrix) } -> std::same_as<bool>;
                 })
    [[nodiscard]] constexpr auto TryToAffine(const Matrix<T, Dimension, Dimension, Layout>& matrix) noexcept(
        std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_move_constructible_v<T> &&
        noexcept(IsAffineEncoding(matrix))) -> std::expected<Affine<T, Dimension - 1, Layout>, Sora::ErrorCode> {
        if (!IsAffineEncoding(matrix)) {
            return std::unexpected(Sora::ErrorCode::InvalidArgument);
        }
        constexpr size_t dimension = Dimension - 1;
        auto linear = Detail::GenerateMatrix<T, dimension, dimension, Layout>(
            [&](size_t index) -> const T& { return matrix(index / dimension, index % dimension); });
        auto translation =
            Detail::GenerateVector<T, dimension>([&](size_t row) -> const T& { return matrix(row, dimension); });
        return Affine<T, dimension, Layout>{std::move(linear), std::move(translation)};
    }

    /** @brief Decode a full homogeneous affine encoding; the affine bottom-row invariant is a precondition. */
    template<ValueCarrier T, size_t Dimension, MatrixLayout Layout>
        requires(Dimension > 1 && ZeroInitializable<T> && OneInitializable<T> &&
                 requires(const Matrix<T, Dimension, Dimension, Layout>& matrix) {
                     { IsAffineEncoding(matrix) } -> std::same_as<bool>;
                 })
    [[nodiscard]] constexpr Affine<T, Dimension - 1, Layout>
    ToAffine(const Matrix<T, Dimension, Dimension, Layout>& matrix) noexcept(std::is_nothrow_copy_constructible_v<T> &&
                                                                             std::is_nothrow_move_constructible_v<T> &&
                                                                             noexcept(IsAffineEncoding(matrix))) {
        SORA_PRE(IsAffineEncoding(matrix));
        constexpr size_t dimension = Dimension - 1;
        auto linear = Detail::GenerateMatrix<T, dimension, dimension, Layout>(
            [&](size_t index) -> const T& { return matrix(index / dimension, index % dimension); });
        auto translation =
            Detail::GenerateVector<T, dimension>([&](size_t row) -> const T& { return matrix(row, dimension); });
        return Affine<T, dimension, Layout>{std::move(linear), std::move(translation)};
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

        template<std::integral T, size_t Count>
            requires(Count > 0)
        [[nodiscard]] constexpr auto TryExactIntegralSum(const std::array<WideInteger<T>, Count>& terms) noexcept
            -> std::expected<WideInteger<T>, Sora::ErrorCode> {
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
                    if ((term > 0 && sum > (std::numeric_limits<Wide>::max)() - term) ||
                        (term < 0 && sum < (std::numeric_limits<Wide>::lowest)() - term)) {
                        return std::unexpected(Sora::ErrorCode::OutOfRange);
                    }
                } else if (sum > (std::numeric_limits<Wide>::max)() - term) {
                    return std::unexpected(Sora::ErrorCode::OutOfRange);
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

        template<std::integral T, size_t N, typename Values, typename Coefficient>
        [[nodiscard]] constexpr auto TryIntegralLinearCombination(T bias, const Values& values,
                                                                  Coefficient&& coefficient) noexcept
            -> std::expected<T, Sora::ErrorCode> {
            std::array<WideInteger<T>, N + 1> terms{};
            terms[0] = static_cast<WideInteger<T>>(bias);
            for (size_t column = 0; column < N; ++column) {
                terms[column + 1] =
                    static_cast<WideInteger<T>>(coefficient(column)) * static_cast<WideInteger<T>>(values[column]);
            }
            const auto sum = TryExactIntegralSum<T>(terms);
            if (!sum || *sum < static_cast<WideInteger<T>>((std::numeric_limits<T>::lowest)()) ||
                *sum > static_cast<WideInteger<T>>((std::numeric_limits<T>::max)())) {
                return std::unexpected(Sora::ErrorCode::OutOfRange);
            }
            return static_cast<T>(*sum);
        }

        template<typename T>
        concept AffineLinearCombinable = ValueCarrier<T> && LinearCombinable<T, T> &&
                                         std::same_as<Traits::MulResult<T, T>, T>;

        template<AffineLinearCombinable T, size_t N, typename Values, typename Coefficient>
        [[nodiscard]] constexpr T
        AffinePointCoordinate(T bias, const Values& values,
                              Coefficient&& coefficient) noexcept(kNothrowLinearCombination<T, T> &&
                                                                  std::is_nothrow_copy_constructible_v<T>) {
            if constexpr (std::integral<T>) {
                return IntegralLinearCombination<T, N>(bias, values, std::forward<Coefficient>(coefficient));
            } else {
                T result = std::move(bias);
                template for (constexpr size_t column : std::define_static_array(std::views::iota(size_t{0}, N))) {
                    if consteval {
                        result = Sora::Math::Add(Sora::Math::Mul(coefficient(column), values[column]), result);
                    } else {
                        result = Sora::Math::Fma(coefficient(column), values[column], result);
                    }
                }
                return result;
            }
        }

        template<AffineLinearCombinable T, size_t N, typename Values, typename Coefficient>
        [[nodiscard]] constexpr T
        AffineVectorCoordinate(const Values& values,
                               Coefficient&& coefficient) noexcept(kNothrowLinearCombination<T, T>) {
            if constexpr (std::integral<T>) {
                return IntegralLinearCombination<T, N>(T{0}, values, std::forward<Coefficient>(coefficient));
            } else {
                T result = Sora::Math::Mul(coefficient(0), values[0]);
                template for (constexpr size_t column : std::define_static_array(std::views::iota(size_t{1}, N))) {
                    if consteval {
                        result = Sora::Math::Add(Sora::Math::Mul(coefficient(column), values[column]), result);
                    } else {
                        result = Sora::Math::Fma(coefficient(column), values[column], result);
                    }
                }
                return result;
            }
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
        [[nodiscard]] consteval bool NothrowEncodeAffine() {
            if constexpr (Representation == AffineRepresentation::Affine) {
                return std::is_nothrow_move_constructible_v<Affine<T, N, Layout>>;
            } else if constexpr (Representation == AffineRepresentation::AffineMatrix) {
                return noexcept(ToAffineMatrix(std::declval<const Affine<T, N, Layout>&>()));
            } else {
                return noexcept(ToMatrix(std::declval<const Affine<T, N, Layout>&>()));
            }
        }

        template<AffineRepresentation Representation, ValueCarrier T, size_t N, MatrixLayout Layout>
            requires(Representation != AffineRepresentation::Matrix || (ZeroInitializable<T> && OneInitializable<T>))
        [[nodiscard]] constexpr Traits::AffineResult<Representation, T, N, Layout>
        EncodeAffine(Affine<T, N, Layout> affine) noexcept(NothrowEncodeAffine<Representation, T, N, Layout>()) {
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

        template<std::floating_point T>
        [[nodiscard]] constexpr T NextAfter(T value, T direction) noexcept {
            static_assert(std::numeric_limits<T>::is_iec559 && (sizeof(T) == 4 || sizeof(T) == 8));
            if (IsNaN(value) || value == direction) {
                return value;
            }
            if (value == T{0}) {
                const T adjacent = (std::numeric_limits<T>::denorm_min)();
                return direction < T{0} ? -adjacent : adjacent;
            }
            using Bits = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
            Bits bits = std::bit_cast<Bits>(value);
            bits += ((value > T{0}) == (direction > value)) ? Bits{1} : Bits(-1);
            return std::bit_cast<T>(bits);
        }

        template<std::floating_point T>
        [[nodiscard]] constexpr T RoundDown(T value) noexcept {
            return !IsFinite(value) || IsNaN(value)
                       ? value
                       : NextAfter(value, -(std::numeric_limits<T>::infinity)());
        }

        template<std::floating_point T>
        [[nodiscard]] constexpr T RoundUp(T value) noexcept {
            return !IsFinite(value) || IsNaN(value)
                       ? value
                       : NextAfter(value, (std::numeric_limits<T>::infinity)());
        }

        template<std::floating_point T>
        [[nodiscard]] constexpr T AddRoundoff(T left, T right, T sum) noexcept {
            const T rightVirtual = sum - left;
            return (left - (sum - rightVirtual)) + (right - rightVirtual);
        }

        template<std::floating_point T>
        [[nodiscard]] constexpr T AddDown(T left, T right) noexcept {
            const T sum = left + right;
            if (IsNaN(sum) || !IsFinite(left) || !IsFinite(right) || left == T{0} || right == T{0}) {
                return sum;
            }
            if (sum == (std::numeric_limits<T>::infinity)()) {
                return (std::numeric_limits<T>::max)();
            }
            if (sum == -(std::numeric_limits<T>::infinity)()) {
                return sum;
            }
            const T roundoff = AddRoundoff(left, right, sum);
            return roundoff < T{0} ? RoundDown(sum) : sum;
        }

        template<std::floating_point T>
        [[nodiscard]] constexpr T AddUp(T left, T right) noexcept {
            const T sum = left + right;
            if (IsNaN(sum) || !IsFinite(left) || !IsFinite(right) || left == T{0} || right == T{0}) {
                return sum;
            }
            if (sum == -(std::numeric_limits<T>::infinity)()) {
                return (std::numeric_limits<T>::lowest)();
            }
            if (sum == (std::numeric_limits<T>::infinity)()) {
                return sum;
            }
            const T roundoff = AddRoundoff(left, right, sum);
            return roundoff > T{0} ? RoundUp(sum) : sum;
        }

        template<std::floating_point T>
        [[nodiscard]] constexpr T MultiplyDown(T left, T right) noexcept {
            const T product = left * right;
            if (IsNaN(product) || !IsFinite(left) || !IsFinite(right) || left == T{0} || right == T{0} ||
                AbsValue(left) == T{1} ||
                AbsValue(right) == T{1}) {
                return product;
            }
            if (product == (std::numeric_limits<T>::infinity)()) {
                return (std::numeric_limits<T>::max)();
            }
            if (product == -(std::numeric_limits<T>::infinity)()) {
                return product;
            }
            if (product == T{0}) {
                const bool negative = (left < T{0}) != (right < T{0});
                return negative ? -(std::numeric_limits<T>::denorm_min)() : T{0};
            }
            if (AbsValue(product) < (std::numeric_limits<T>::min)()) {
                return RoundDown(product);
            }
            if consteval {
                return RoundDown(product);
            } else {
                return Sora::Math::Fma(left, right, -product) < T{0} ? RoundDown(product) : product;
            }
        }

        template<std::floating_point T>
        [[nodiscard]] constexpr T MultiplyUp(T left, T right) noexcept {
            const T product = left * right;
            if (IsNaN(product) || !IsFinite(left) || !IsFinite(right) || left == T{0} || right == T{0} ||
                AbsValue(left) == T{1} ||
                AbsValue(right) == T{1}) {
                return product;
            }
            if (product == -(std::numeric_limits<T>::infinity)()) {
                return (std::numeric_limits<T>::lowest)();
            }
            if (product == (std::numeric_limits<T>::infinity)()) {
                return product;
            }
            if (product == T{0}) {
                const bool negative = (left < T{0}) != (right < T{0});
                return negative ? T{0} : (std::numeric_limits<T>::denorm_min)();
            }
            if (AbsValue(product) < (std::numeric_limits<T>::min)()) {
                return RoundUp(product);
            }
            if consteval {
                return RoundUp(product);
            } else {
                return Sora::Math::Fma(left, right, -product) > T{0} ? RoundUp(product) : product;
            }
        }

        template<std::floating_point T>
        [[nodiscard]] constexpr T SqrtUp(T value) noexcept {
            const T root = Math::Sqrt(value);
            if (!IsFinite(root) || root == T{0} || root == T{1}) {
                return root;
            }
            if (value < (std::numeric_limits<T>::min)()) {
                return RoundUp(root);
            }
            if consteval {
                return root * root < value ? RoundUp(root) : root;
            } else {
                return Sora::Math::Fma(root, root, -value) < T{0} ? RoundUp(root) : root;
            }
        }

        template<std::floating_point T, size_t N, MatrixLayout Layout>
        [[nodiscard]] constexpr T ConservativeRadiusScale(const Affine<T, N, Layout>& affine) noexcept {
            T oneNorm = T{0};
            T infinityNorm = T{0};
            for (size_t column = 0; column < N; ++column) {
                T sum = T{0};
                for (size_t row = 0; row < N; ++row) {
                    sum = AddUp(sum, AbsValue(affine.LinearPart()(row, column)));
                }
                oneNorm = oneNorm > sum ? oneNorm : sum;
            }
            for (size_t row = 0; row < N; ++row) {
                T sum = T{0};
                for (size_t column = 0; column < N; ++column) {
                    sum = AddUp(sum, AbsValue(affine.LinearPart()(row, column)));
                }
                infinityNorm = infinityNorm > sum ? infinityNorm : sum;
            }
            if (oneNorm == T{0} || infinityNorm == T{0}) {
                return T{0};
            }
            return MultiplyUp(SqrtUp(oneNorm), SqrtUp(infinityNorm));
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
        requires(Representation != AffineRepresentation::Matrix ||
                 (ZeroInitializable<T> && OneInitializable<T>))
    [[nodiscard]] constexpr Traits::AffineResult<Representation, T, N, Layout>
    MakeAffine(Matrix<T, N, N, Layout> linear, Vector<T, N> translation) noexcept(
        std::is_nothrow_move_constructible_v<Affine<T, N, Layout>> &&
        noexcept(Detail::EncodeAffine<Representation>(std::declval<Affine<T, N, Layout>>()))) {
        return Detail::EncodeAffine<Representation>(Affine<T, N, Layout>{std::move(linear), std::move(translation)});
    }

    /** @brief Construct a pure translation in any positive dimension. */
    template<AffineRepresentation Representation = AffineRepresentation::Affine,
             MatrixLayout Layout = MatrixLayout::ColumnMajor, ValueCarrier T, size_t N>
        requires(ZeroInitializable<T> && OneInitializable<T>)
    [[nodiscard]] constexpr Traits::AffineResult<Representation, T, N, Layout>
    Translate(Vector<T, N> translation) noexcept(
        noexcept(MakeAffine<Representation>(Matrix<T, N, N, Layout>::Identity(), std::move(translation)))) {
        return MakeAffine<Representation>(Matrix<T, N, N, Layout>::Identity(), std::move(translation));
    }

    /** @brief Construct a coordinate-wise scale in any positive dimension; zero and negative factors are valid. */
    template<AffineRepresentation Representation = AffineRepresentation::Affine,
             MatrixLayout Layout = MatrixLayout::ColumnMajor, ValueCarrier T, size_t N>
        requires(ZeroInitializable<T> &&
                 (Representation != AffineRepresentation::Matrix || OneInitializable<T>))
    [[nodiscard]] constexpr Traits::AffineResult<Representation, T, N, Layout>
    Scale(const Vector<T, N>& scale) noexcept(
        kNothrowZeroValue<T> && std::is_nothrow_copy_constructible_v<T> &&
        std::is_nothrow_move_constructible_v<T> &&
        noexcept(MakeAffine<Representation>(std::declval<Matrix<T, N, N, Layout>>(), std::declval<Vector<T, N>>()))) {
        auto linear = Detail::GenerateMatrix<T, N, N, Layout>([&](size_t index) -> T {
            const size_t row = index / N;
            const size_t column = index % N;
            return row == column ? scale[row] : ZeroValue<T>();
        });
        return MakeAffine<Representation>(std::move(linear), Vector<T, N>{});
    }

    /** @brief Construct a uniform scale in dimension @p N. */
    template<size_t N, AffineRepresentation Representation = AffineRepresentation::Affine,
             MatrixLayout Layout = MatrixLayout::ColumnMajor, ValueCarrier T>
        requires(N > 0 && ZeroInitializable<T> &&
                 (Representation != AffineRepresentation::Matrix || OneInitializable<T>))
    [[nodiscard]] constexpr Traits::AffineResult<Representation, T, N, Layout>
    UniformScale(T scale) noexcept(std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_move_constructible_v<T> &&
                                   noexcept(Scale<Representation, Layout>(std::declval<Vector<T, N>>()))) {
        auto factors = Detail::GenerateVector<T, N>([&](size_t) -> T { return scale; });
        return Scale<Representation, Layout>(factors);
    }

    /** @brief Construct `x[TargetAxis] += factor * x[SourceAxis]` in dimension @p N. */
    template<size_t TargetAxis, size_t SourceAxis, size_t N,
             AffineRepresentation Representation = AffineRepresentation::Affine,
             MatrixLayout Layout = MatrixLayout::ColumnMajor, ValueCarrier T>
        requires(TargetAxis < N && SourceAxis < N && TargetAxis != SourceAxis && ZeroInitializable<T> &&
                 OneInitializable<T>)
    [[nodiscard]] constexpr Traits::AffineResult<Representation, T, N, Layout> Shear(T factor) noexcept(
        std::is_nothrow_move_constructible_v<T> &&
        noexcept(MakeAffine<Representation>(std::declval<Matrix<T, N, N, Layout>>(), std::declval<Vector<T, N>>()))) {
        auto linear = Matrix<T, N, N, Layout>::Identity();
        linear(TargetAxis, SourceAxis) = std::move(factor);
        return MakeAffine<Representation>(std::move(linear), Vector<T, N>{});
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

    /** @brief Construct reflection across a hyperplane represented by a unit normal and signed-distance offset. */
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
        SORA_PRE(IsFinite(translation));
        return MakeAffine<Representation>(std::move(linear), std::move(translation));
    }

    /** @brief Construct reflection from a normalized equation, reporting an unrepresentable translation. */
    template<AffineRepresentation Representation = AffineRepresentation::Affine,
             MatrixLayout Layout = MatrixLayout::ColumnMajor, std::floating_point T, size_t N>
    [[nodiscard]] constexpr auto TryReflect(const UnitVector<T, N>& normal, T offset = T{0}) noexcept
        -> std::expected<Traits::AffineResult<Representation, T, N, Layout>, Sora::ErrorCode> {
        if (!IsFinite(offset)) {
            return std::unexpected(Sora::ErrorCode::InvalidArgument);
        }
        Matrix<T, N, N, Layout> linear = Matrix<T, N, N, Layout>::Identity();
        Vector<T, N> translation;
        template for (constexpr size_t row : std::define_static_array(std::views::iota(size_t{0}, N))) {
            template for (constexpr size_t column : std::define_static_array(std::views::iota(size_t{0}, N))) {
                linear(row, column) -= T{2} * normal[row] * normal[column];
            }
            translation[row] = T{-2} * offset * normal[row];
        }
        if (!Detail::IsFiniteMatrix(linear) || !IsFinite(translation)) {
            return std::unexpected(Sora::ErrorCode::OutOfRange);
        }
        return MakeAffine<Representation>(std::move(linear), std::move(translation));
    }

    /** @brief Construct reflection across an existing normalized hyperplane. */
    template<AffineRepresentation Representation = AffineRepresentation::Affine,
             MatrixLayout Layout = MatrixLayout::ColumnMajor, std::floating_point T, size_t N>
    [[nodiscard]] constexpr auto TryReflect(const Hyperplane<T, N>& plane) noexcept
        -> std::expected<Traits::AffineResult<Representation, T, N, Layout>, Sora::ErrorCode> {
        return TryReflect<Representation, Layout>(plane.Normal(), plane.Offset());
    }

    /** @brief Construct reflection across an existing normalized hyperplane. @pre The result is representable. */
    template<AffineRepresentation Representation = AffineRepresentation::Affine,
             MatrixLayout Layout = MatrixLayout::ColumnMajor, std::floating_point T, size_t N>
    [[nodiscard]] constexpr Traits::AffineResult<Representation, T, N, Layout>
    Reflect(const Hyperplane<T, N>& plane) noexcept {
        const auto result = TryReflect<Representation, Layout>(plane);
        SORA_PRE(result.has_value());
        return std::move(*result);
    }

    /** @brief Normalize the raw equation `normal dot x + offset == 0` and construct its reflection. */
    template<AffineRepresentation Representation = AffineRepresentation::Affine,
             MatrixLayout Layout = MatrixLayout::ColumnMajor, std::floating_point T, size_t N>
    [[nodiscard]] constexpr auto TryReflect(const Vector<T, N>& normal, T offset = T{0}) noexcept
        -> std::expected<Traits::AffineResult<Representation, T, N, Layout>, Sora::ErrorCode> {
        if (!IsFinite(normal) || !IsFinite(offset)) {
            return std::unexpected(Sora::ErrorCode::InvalidArgument);
        }
        const auto unitNormal = TryNormalize(normal);
        if (!unitNormal) {
            return std::unexpected(Sora::ErrorCode::InvalidArgument);
        }

        T scale = T{0};
        for (size_t axis = 0; axis < N; ++axis) {
            const T magnitude = AbsValue(normal[axis]);
            scale = scale > magnitude ? scale : magnitude;
        }
        Vector<T, N> scaled;
        for (size_t axis = 0; axis < N; ++axis) {
            scaled[axis] = normal[axis] / scale;
        }
        const T normalizedOffset = (offset / scale) / Norm(scaled);
        if (!IsFinite(normalizedOffset)) {
            return std::unexpected(Sora::ErrorCode::OutOfRange);
        }
        return TryReflect<Representation, Layout>(*unitNormal, normalizedOffset);
    }

    /** @brief Apply an affine map to an affine point; integral results must be representable. */
    template<ValueCarrier T, std::size_t N, MatrixLayout Layout>
        requires Detail::AffineLinearCombinable<T>
    [[nodiscard]] constexpr Point<T, N>
    TransformPoint(const Affine<T, N, Layout>& affine,
                   const Point<T, N>& point) noexcept(Detail::kNothrowLinearCombination<T, T> &&
                                                      std::is_nothrow_copy_constructible_v<T>) {
        return [&]<size_t... Rows>(std::index_sequence<Rows...>) {
            return Point<T, N>{Detail::AffinePointCoordinate<T, N>(
                affine.TranslationPart()[Rows], point,
                [&](size_t column) constexpr noexcept -> const T& { return affine.LinearPart()(Rows, column); })...};
        }(std::make_index_sequence<N>{});
    }

    /** @brief Apply only the linear part of an affine map; integral results must be representable. */
    template<ValueCarrier T, std::size_t N, MatrixLayout Layout>
        requires Detail::AffineLinearCombinable<T>
    [[nodiscard]] constexpr Vector<T, N>
    TransformVector(const Affine<T, N, Layout>& affine,
                    const Vector<T, N>& vector) noexcept(Detail::kNothrowLinearCombination<T, T> &&
                                                         std::is_nothrow_copy_constructible_v<T>) {
        return [&]<size_t... Rows>(std::index_sequence<Rows...>) {
            return Vector<T, N>{
                Detail::AffineVectorCoordinate<T, N>(vector, [&](size_t column) constexpr noexcept -> const T& {
                    return affine.LinearPart()(Rows, column);
                })...};
        }(std::make_index_sequence<N>{});
    }

    /** @brief Apply a compact affine matrix to an affine point. */
    template<ValueCarrier T, std::size_t N, MatrixLayout Layout>
        requires Detail::AffineLinearCombinable<T>
    [[nodiscard]] constexpr Point<T, N>
    TransformPoint(const AffineMatrix<T, N, Layout>& affine,
                   const Point<T, N>& point) noexcept(Detail::kNothrowLinearCombination<T, T> &&
                                                      std::is_nothrow_copy_constructible_v<T>) {
        return [&]<size_t... Rows>(std::index_sequence<Rows...>) {
            return Point<T, N>{Detail::AffinePointCoordinate<T, N>(
                affine(Rows, N), point,
                [&](size_t column) constexpr noexcept -> const T& { return affine(Rows, column); })...};
        }(std::make_index_sequence<N>{});
    }

    /** @brief Apply only the linear block of a compact affine matrix to a free vector. */
    template<ValueCarrier T, std::size_t N, MatrixLayout Layout>
        requires Detail::AffineLinearCombinable<T>
    [[nodiscard]] constexpr Vector<T, N>
    TransformVector(const AffineMatrix<T, N, Layout>& affine,
                    const Vector<T, N>& vector) noexcept(Detail::kNothrowLinearCombination<T, T> &&
                                                         std::is_nothrow_copy_constructible_v<T>) {
        return [&]<size_t... Rows>(std::index_sequence<Rows...>) {
            return Vector<T, N>{Detail::AffineVectorCoordinate<T, N>(
                vector, [&](size_t column) constexpr noexcept -> const T& { return affine(Rows, column); })...};
        }(std::make_index_sequence<N>{});
    }

    /** @brief Apply an affine full homogeneous matrix to a point after checking its bottom row. */
    template<ValueCarrier T, size_t Dimension, MatrixLayout Layout>
        requires(Dimension > 1 && Detail::AffineLinearCombinable<T> && ZeroInitializable<T> &&
                 OneInitializable<T>)
    [[nodiscard]] constexpr auto TryTransformPoint(
        const Matrix<T, Dimension, Dimension, Layout>& matrix,
        const Point<T, Dimension - 1>&
            point) noexcept(noexcept(TryToAffine(matrix)) &&
                            noexcept(TransformPoint(std::declval<const Affine<T, Dimension - 1, Layout>&>(), point)))
        -> std::expected<Point<T, Dimension - 1>, Sora::ErrorCode> {
        const auto affine = TryToAffine(matrix);
        if (!affine) {
            return std::unexpected(affine.error());
        }
        return TransformPoint(*affine, point);
    }

    /** @brief Apply an affine full homogeneous matrix to a vector after checking its bottom row. */
    template<ValueCarrier T, size_t Dimension, MatrixLayout Layout>
        requires(Dimension > 1 && Detail::AffineLinearCombinable<T> && ZeroInitializable<T> &&
                 OneInitializable<T>)
    [[nodiscard]] constexpr auto TryTransformVector(
        const Matrix<T, Dimension, Dimension, Layout>& matrix,
        const Vector<T, Dimension - 1>&
            vector) noexcept(noexcept(TryToAffine(matrix)) &&
                             noexcept(TransformVector(std::declval<const Affine<T, Dimension - 1, Layout>&>(), vector)))
        -> std::expected<Vector<T, Dimension - 1>, Sora::ErrorCode> {
        const auto affine = TryToAffine(matrix);
        if (!affine) {
            return std::unexpected(affine.error());
        }
        return TransformVector(*affine, vector);
    }

    /** @brief Apply a full homogeneous affine encoding to a point. @pre @p matrix passes IsAffineEncoding. */
    template<ValueCarrier T, size_t Dimension, MatrixLayout Layout>
        requires(Dimension > 1 && Detail::AffineLinearCombinable<T> && ZeroInitializable<T> &&
                 OneInitializable<T>)
    [[nodiscard]] constexpr Point<T, Dimension - 1>
    TransformPoint(const Matrix<T, Dimension, Dimension, Layout>& matrix,
                   const Point<T, Dimension - 1>& point) noexcept(noexcept(TransformPoint(ToAffine(matrix), point))) {
        return TransformPoint(ToAffine(matrix), point);
    }

    /** @brief Apply a full homogeneous affine encoding to a vector. @pre @p matrix passes IsAffineEncoding. */
    template<ValueCarrier T, size_t Dimension, MatrixLayout Layout>
        requires(Dimension > 1 && Detail::AffineLinearCombinable<T> && ZeroInitializable<T> &&
                 OneInitializable<T>)
    [[nodiscard]] constexpr Vector<T, Dimension - 1> TransformVector(
        const Matrix<T, Dimension, Dimension, Layout>& matrix,
        const Vector<T, Dimension - 1>& vector) noexcept(noexcept(TransformVector(ToAffine(matrix), vector))) {
        return TransformVector(ToAffine(matrix), vector);
    }

    namespace Detail {

        template<typename Transform>
        struct AffineEncodingTraits;

        template<ValueCarrier T, size_t N, MatrixLayout Layout>
        struct AffineEncodingTraits<Affine<T, N, Layout>> {
            using CarrierType = T;
            static constexpr size_t kDimension = N;
            static constexpr MatrixLayout kLayout = Layout;
            static constexpr AffineRepresentation kRepresentation = AffineRepresentation::Affine;
        };

        template<ValueCarrier T, size_t N, MatrixLayout Layout>
        struct AffineEncodingTraits<AffineMatrix<T, N, Layout>> {
            using CarrierType = T;
            static constexpr size_t kDimension = N;
            static constexpr MatrixLayout kLayout = Layout;
            static constexpr AffineRepresentation kRepresentation = AffineRepresentation::AffineMatrix;
        };

        template<ValueCarrier T, size_t Dimension, MatrixLayout Layout>
        struct AffineEncodingTraits<Matrix<T, Dimension, Dimension, Layout>> {
            using CarrierType = T;
            static constexpr size_t kDimension = Dimension - 1;
            static constexpr MatrixLayout kLayout = Layout;
            static constexpr AffineRepresentation kRepresentation = AffineRepresentation::Matrix;
        };

        template<typename Transform>
        concept AffineEncoding = requires {
            typename AffineEncodingTraits<std::remove_cvref_t<Transform>>::CarrierType;
            requires(AffineEncodingTraits<std::remove_cvref_t<Transform>>::kDimension > 0);
        };

        template<AffineEncoding Transform>
        [[nodiscard]] constexpr auto TryDecodeAffine(const Transform& transform) noexcept {
            using Type = std::remove_cvref_t<Transform>;
            using Traits = AffineEncodingTraits<Type>;
            using AffineType = Affine<typename Traits::CarrierType, Traits::kDimension, Traits::kLayout>;
            if constexpr (Traits::kRepresentation == AffineRepresentation::Affine) {
                return std::expected<AffineType, Sora::ErrorCode>{transform};
            } else if constexpr (Traits::kRepresentation == AffineRepresentation::AffineMatrix) {
                return std::expected<AffineType, Sora::ErrorCode>{ToAffine(transform)};
            } else {
                return TryToAffine(transform);
            }
        }

        template<AffineEncoding Output, ValueCarrier T, size_t N, MatrixLayout Layout>
        [[nodiscard]] constexpr auto
        EncodeLike(const Affine<T, N, Layout>& affine) noexcept(std::is_nothrow_copy_constructible_v<T> &&
                                                                std::is_nothrow_move_constructible_v<T>)
            -> std::remove_cvref_t<Output> {
            using Traits = AffineEncodingTraits<std::remove_cvref_t<Output>>;
            if constexpr (Traits::kRepresentation == AffineRepresentation::Affine) {
                return affine;
            } else if constexpr (Traits::kRepresentation == AffineRepresentation::AffineMatrix) {
                return ToAffineMatrix(affine);
            } else {
                return ToMatrix(affine);
            }
        }

    } // namespace Detail

    /** @brief Compose two finite or integral affine maps with checked representability. */
    template<Sora::Concept::ArithmeticScalar T, size_t N, MatrixLayout LeftLayout, MatrixLayout RightLayout>
    [[nodiscard]] constexpr auto TryCompose(const Affine<T, N, LeftLayout>& left,
                                            const Affine<T, N, RightLayout>& right) noexcept
        -> std::expected<Affine<T, N, LeftLayout>, Sora::ErrorCode> {
        if constexpr (std::floating_point<T>) {
            if (!Detail::IsFiniteMatrix(left.LinearPart()) || !Detail::IsFiniteMatrix(right.LinearPart()) ||
                !IsFinite(left.TranslationPart()) || !IsFinite(right.TranslationPart())) {
                return std::unexpected(Sora::ErrorCode::InvalidArgument);
            }
            auto linear = left.LinearPart() * right.LinearPart();
            auto translation = left.LinearPart() * right.TranslationPart() + left.TranslationPart();
            if (!Detail::IsFiniteMatrix(linear) || !IsFinite(translation)) {
                return std::unexpected(Sora::ErrorCode::OutOfRange);
            }
            return Affine<T, N, LeftLayout>{std::move(linear), std::move(translation)};
        } else {
            Matrix<T, N, N, LeftLayout> linear;
            Vector<T, N> translation;
            for (size_t row = 0; row < N; ++row) {
                for (size_t column = 0; column < N; ++column) {
                    std::array<T, N> values{};
                    for (size_t inner = 0; inner < N; ++inner) {
                        values[inner] = right.LinearPart()(inner, column);
                    }
                    const auto value = Detail::TryIntegralLinearCombination<T, N>(
                        T{0}, values, [&](size_t inner) noexcept { return left.LinearPart()(row, inner); });
                    if (!value) {
                        return std::unexpected(value.error());
                    }
                    linear(row, column) = *value;
                }
                const auto value = Detail::TryIntegralLinearCombination<T, N>(
                    left.TranslationPart()[row], right.TranslationPart(),
                    [&](size_t column) noexcept { return left.LinearPart()(row, column); });
                if (!value) {
                    return std::unexpected(value.error());
                }
                translation[row] = *value;
            }
            return Affine<T, N, LeftLayout>{std::move(linear), std::move(translation)};
        }
    }

    /** @brief Compose compact or full affine encodings and preserve the left operand's representation. */
    template<Detail::AffineEncoding Left, Detail::AffineEncoding Right>
        requires(Detail::AffineEncodingTraits<std::remove_cvref_t<Left>>::kRepresentation !=
                     AffineRepresentation::Affine ||
                 Detail::AffineEncodingTraits<std::remove_cvref_t<Right>>::kRepresentation !=
                     AffineRepresentation::Affine) &&
                std::same_as<typename Detail::AffineEncodingTraits<std::remove_cvref_t<Left>>::CarrierType,
                             typename Detail::AffineEncodingTraits<std::remove_cvref_t<Right>>::CarrierType> &&
                (Detail::AffineEncodingTraits<std::remove_cvref_t<Left>>::kDimension ==
                 Detail::AffineEncodingTraits<std::remove_cvref_t<Right>>::kDimension) &&
                Sora::Concept::ArithmeticScalar<
                    typename Detail::AffineEncodingTraits<std::remove_cvref_t<Left>>::CarrierType>
    [[nodiscard]] constexpr auto TryCompose(const Left& left, const Right& right) noexcept
        -> std::expected<std::remove_cvref_t<Left>, Sora::ErrorCode> {
        const auto leftAffine = Detail::TryDecodeAffine(left);
        if (!leftAffine) {
            return std::unexpected(leftAffine.error());
        }
        const auto rightAffine = Detail::TryDecodeAffine(right);
        if (!rightAffine) {
            return std::unexpected(rightAffine.error());
        }
        const auto composed = TryCompose(*leftAffine, *rightAffine);
        if (!composed) {
            return std::unexpected(composed.error());
        }
        return Detail::EncodeLike<Left>(*composed);
    }

    /**
     * @brief Compose two affine maps so that `Compose(left, right)(x) = left(right(x))`.
     * @pre Arithmetic results are representable in @p T; use TryCompose for recoverable validation.
     */
    template<ValueCarrier T, size_t N, MatrixLayout LeftLayout, MatrixLayout RightLayout>
        requires Detail::AffineLinearCombinable<T>
    [[nodiscard]] constexpr Affine<T, N, LeftLayout>
    Compose(const Affine<T, N, LeftLayout>& left,
            const Affine<T, N, RightLayout>& right) noexcept(Detail::kNothrowLinearCombination<T, T>) {
        if constexpr (Sora::Concept::ArithmeticScalar<T>) {
            auto result = TryCompose(left, right);
            SORA_PRE(result.has_value());
            return std::move(*result);
        } else {
            return Affine<T, N, LeftLayout>{left.LinearPart() * right.LinearPart(),
                                            left.LinearPart() * right.TranslationPart() + left.TranslationPart()};
        }
    }

    /** @brief Compose affine encodings and preserve the left operand's representation. @pre TryCompose succeeds. */
    template<Detail::AffineEncoding Left, Detail::AffineEncoding Right>
        requires requires(const Left& left, const Right& right) {
            { TryCompose(left, right) };
        } && (Detail::AffineEncodingTraits<std::remove_cvref_t<Left>>::kRepresentation !=
                  AffineRepresentation::Affine ||
              Detail::AffineEncodingTraits<std::remove_cvref_t<Right>>::kRepresentation != AffineRepresentation::Affine)
    [[nodiscard]] constexpr std::remove_cvref_t<Left> Compose(const Left& left, const Right& right) noexcept {
        auto result = TryCompose(left, right);
        SORA_PRE(result.has_value());
        return std::move(*result);
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

            const T pivotValue = augmented[column][column];
            const T inversePivot = T{1} / pivotValue;
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
                    augmented[row][value] = Sora::Math::Fma(-factor, augmented[column][value], augmented[row][value]);
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

    /** @brief Try to transform a box by outward-rounded coordinate-interval propagation. */
    template<Sora::Concept::ArithmeticScalar T, size_t N, MatrixLayout Layout>
    [[nodiscard]] constexpr auto TryTransform(const Affine<T, N, Layout>& affine, const Box<T, N>& box) noexcept
        -> std::expected<Box<T, N>, Sora::ErrorCode> {
        if constexpr (std::floating_point<T>) {
            if (!Detail::IsFiniteMatrix(affine.LinearPart()) || !IsFinite(affine.TranslationPart())) {
                return std::unexpected(Sora::ErrorCode::InvalidArgument);
            }
        }

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
                const auto lower = Detail::TryExactIntegralSum<T>(lowerTerms);
                const auto upper = Detail::TryExactIntegralSum<T>(upperTerms);
                if (!lower || !upper || *lower < static_cast<WideInteger<T>>((std::numeric_limits<T>::lowest)()) ||
                    *lower > static_cast<WideInteger<T>>((std::numeric_limits<T>::max)()) ||
                    *upper < static_cast<WideInteger<T>>((std::numeric_limits<T>::lowest)()) ||
                    *upper > static_cast<WideInteger<T>>((std::numeric_limits<T>::max)())) {
                    return std::unexpected(Sora::ErrorCode::OutOfRange);
                }
                minimum[row] = static_cast<T>(*lower);
                maximum[row] = static_cast<T>(*upper);
            } else {
                T lower = affine.TranslationPart()[row];
                T upper = lower;
                for (size_t column = 0; column < N; ++column) {
                    const T coefficient = affine.LinearPart()(row, column);
                    if (coefficient == T{0}) {
                        continue;
                    }
                    const T lowerProduct = coefficient > T{0} ? Detail::MultiplyDown(coefficient, box.Min()[column])
                                                              : Detail::MultiplyDown(coefficient, box.Max()[column]);
                    const T upperProduct = coefficient > T{0} ? Detail::MultiplyUp(coefficient, box.Max()[column])
                                                              : Detail::MultiplyUp(coefficient, box.Min()[column]);
                    if (IsNaN(lowerProduct) || IsNaN(upperProduct)) {
                        return std::unexpected(Sora::ErrorCode::OutOfRange);
                    }
                    lower = Detail::AddDown(lower, lowerProduct);
                    upper = Detail::AddUp(upper, upperProduct);
                    if (IsNaN(lower) || IsNaN(upper)) {
                        return std::unexpected(Sora::ErrorCode::OutOfRange);
                    }
                }
                minimum[row] = lower;
                maximum[row] = upper;
            }
        }
        return Box<T, N>{std::move(minimum), std::move(maximum)};
    }

    /**
     * @brief Transform an axis-aligned box by outward-rounded coordinate-interval propagation.
     * @details Runs in `O(N^2)` time and does not enumerate the box's `2^N` corners.
     * @pre The affine coefficients are finite and the result is representable without indeterminate arithmetic.
     */
    template<Sora::Concept::ArithmeticScalar T, size_t N, MatrixLayout Layout>
    [[nodiscard]] constexpr Box<T, N> Transform(const Affine<T, N, Layout>& affine, const Box<T, N>& box) noexcept {
        auto result = TryTransform(affine, box);
        SORA_PRE(result.has_value());
        return std::move(*result);
    }

    /** @brief Try to transform a base/extent rectangle and return its fitted axis-aligned box. */
    template<Sora::Concept::ArithmeticScalar T, size_t N, MatrixLayout Layout>
    [[nodiscard]] constexpr auto TryTransform(const Affine<T, N, Layout>& affine, const Rect<T, N>& rect) noexcept
        -> std::expected<Box<T, N>, Sora::ErrorCode> {
        return TryTransform(affine, rect.ToBox());
    }

    /** @brief Transform a base/extent rectangle and return its fitted axis-aligned box. */
    template<Sora::Concept::ArithmeticScalar T, size_t N, MatrixLayout Layout>
    [[nodiscard]] constexpr Box<T, N> Transform(const Affine<T, N, Layout>& affine, const Rect<T, N>& rect) noexcept {
        return Transform(affine, rect.ToBox());
    }

    /** @brief Try to transform a ball conservatively under arbitrary finite linear distortion. */
    template<std::floating_point T, size_t N, MatrixLayout Layout>
    [[nodiscard]] constexpr auto TryTransform(const Affine<T, N, Layout>& affine, const Ball<T, N>& ball) noexcept
        -> std::expected<Ball<T, N>, Sora::ErrorCode> {
        if (!Detail::IsFiniteMatrix(affine.LinearPart()) || !IsFinite(affine.TranslationPart())) {
            return std::unexpected(Sora::ErrorCode::InvalidArgument);
        }

        Point<T, N> center;
        if (IsFinite(ball.Center())) {
            center = TransformPoint(affine, ball.Center());
            if (IsNaN(center)) {
                return std::unexpected(Sora::ErrorCode::OutOfRange);
            }
        } else {
            for (size_t row = 0; row < N; ++row) {
                T coordinate = affine.TranslationPart()[row];
                for (size_t column = 0; column < N; ++column) {
                    const T coefficient = affine.LinearPart()(row, column);
                    if (coefficient == T{0}) {
                        continue;
                    }
                    coordinate += coefficient * ball.Center()[column];
                    if (IsNaN(coordinate)) {
                        return std::unexpected(Sora::ErrorCode::OutOfRange);
                    }
                }
                center[row] = coordinate;
            }
        }

        if (ball.Radius() == T{0}) {
            return Ball<T, N>{std::move(center), T{0}};
        }
        const T scale = Detail::ConservativeRadiusScale(affine);
        if (IsNaN(scale)) {
            return std::unexpected(Sora::ErrorCode::OutOfRange);
        }
        if (scale == T{0}) {
            return Ball<T, N>{std::move(center), T{0}};
        }
        if (!IsFinite(ball.Radius())) {
            return Ball<T, N>{std::move(center), (std::numeric_limits<T>::infinity)()};
        }
        const T radius = Detail::MultiplyUp(ball.Radius(), scale);
        if (IsNaN(radius)) {
            return std::unexpected(Sora::ErrorCode::OutOfRange);
        }
        return Ball<T, N>{std::move(center), radius};
    }

    /**
     * @brief Transform a ball conservatively under arbitrary finite linear distortion.
     * @pre The result is representable without indeterminate arithmetic.
     */
    template<std::floating_point T, size_t N, MatrixLayout Layout>
    [[nodiscard]] constexpr Ball<T, N> Transform(const Affine<T, N, Layout>& affine, const Ball<T, N>& ball) noexcept {
        auto result = TryTransform(affine, ball);
        SORA_PRE(result.has_value());
        return std::move(*result);
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

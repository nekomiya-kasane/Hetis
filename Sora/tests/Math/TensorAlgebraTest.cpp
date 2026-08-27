/**
 * @file TensorAlgebraTest.cpp
 * @brief Compile-time and runtime coverage for logical-index tensor algebra.
 */
#include <Sora/Math/TensorAlgebra.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <concepts>
#include <array>
#include <type_traits>

namespace {

    struct NonDefaultScalar {
        int value;

        NonDefaultScalar() = delete;
        constexpr explicit NonDefaultScalar(int input) noexcept : value(input) {}
        constexpr auto operator<=>(const NonDefaultScalar&) const = default;
    };

    using Sora::Math::AxisPair;
    using Sora::Math::Contract;
    using Sora::Math::Elementwise;
    using Sora::Math::MatrixLayout;
    using Sora::Math::Matrix;
    using Sora::Math::PermuteAxes;
    using Sora::Math::Relayout;
    using Sora::Math::Tensor;

    constexpr Tensor<double, 2, 3> kInput{1.0, 2.0, 3.0, 4.0, 5.0, 6.0};
    constexpr auto kTransposed = PermuteAxes<1, 0>(kInput);
    static_assert(decltype(kTransposed)::kRank == 2);
    static_assert(decltype(kTransposed)::template Extent<0>() == 3);
    static_assert(decltype(kTransposed)::template Extent<1>() == 2);
    static_assert(kTransposed(0, 1) == 4.0);
    static_assert(kTransposed(2, 0) == 3.0);

    constexpr auto kMapped = Elementwise([](double value) { return value * 2.0; }, kInput);
    static_assert(kMapped[4] == 10.0);

    constexpr Sora::Math::Vector<int, 3> kVector{1, 2, 3};
    constexpr auto kMappedVector = Elementwise([](int value) { return value + 1; }, kVector);
    static_assert(std::same_as<std::remove_cv_t<decltype(kMappedVector)>, Sora::Math::Vector<int, 3>>);
    static_assert(kMappedVector[2] == 4);

    constexpr Matrix<int, 2, 2, MatrixLayout::RowMajor> kRowMajorMatrix{std::array{1, 2, 3, 4}};
    constexpr auto kMappedMatrix = Elementwise([](int value, int scale) { return value * scale; }, kRowMajorMatrix, 2);
    static_assert(std::same_as<std::remove_cv_t<decltype(kMappedMatrix)>,
                               Matrix<int, 2, 2, MatrixLayout::RowMajor>>);
    static_assert(kMappedMatrix(1, 0) == 6);

    constexpr Tensor<double, 2> kLeft{1.0, 2.0};
    constexpr Tensor<double, 2> kRight{3.0, 5.0};
    constexpr auto kContracted = Contract<AxisPair{0, 0}>(kLeft, kRight);
    static_assert(std::same_as<std::remove_cv_t<decltype(kContracted)>, double>);
    static_assert(kContracted == 13.0);

    constexpr Tensor<int, 2, 3, 2> kLeftRank3{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    constexpr Tensor<int, 3, 4, 2> kRightRank3{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                                17, 18, 19, 20, 21, 22, 23, 24};
    constexpr auto kMultiContracted = Contract<AxisPair{1, 0}, AxisPair{2, 2}>(kLeftRank3, kRightRank3);
    static_assert(std::same_as<std::remove_cv_t<decltype(kMultiContracted)>, Tensor<int, 2, 4>>);
    static_assert(kMultiContracted(0, 0) == 265);
    static_assert(kMultiContracted(1, 3) == 949);

    constexpr auto kRelayout = Relayout<MatrixLayout::ColumnMajor>(kInput);
    static_assert(decltype(kRelayout)::kLayout == MatrixLayout::ColumnMajor);
    static_assert(kRelayout(1, 2) == 6.0);
    static_assert(kRelayout.Storage()[1] == 4.0);

    constexpr Matrix<int, 2, 2, MatrixLayout::ColumnMajor> kColumnMajorMatrix{std::array{1, 2, 3, 4}};
    static_assert(kColumnMajorMatrix(0, 0) == 1);
    static_assert(kColumnMajorMatrix(0, 1) == 2);
    static_assert(kColumnMajorMatrix(1, 0) == 3);
    static_assert(kColumnMajorMatrix.View()[1, 0] == 3);

    template<typename T>
    concept RepeatedAxisPermutation = requires(const T& value) { PermuteAxes<0, 0>(value); };

    template<typename Left, typename Right>
    concept MismatchedContraction = requires(const Left& left, const Right& right) {
        Contract<AxisPair{0, 0}>(left, right);
    };

    static_assert(!RepeatedAxisPermutation<Tensor<int, 2, 3>>);
    static_assert(!MismatchedContraction<Tensor<int, 2>, Tensor<int, 3>>);

} // namespace

TEST_CASE("tensor algebra preserves logical indexing across transformations", "[Sora][Math][TensorAlgebra]") {
    REQUIRE(kTransposed(1, 0) == 2.0);
    REQUIRE(kMapped[0] == 2.0);
    REQUIRE(kContracted == 13.0);
    REQUIRE(kMultiContracted(0, 0) == 265);
    REQUIRE(kMultiContracted(1, 3) == 949);
    REQUIRE(kRelayout(0, 1) == 2.0);
    REQUIRE(kColumnMajorMatrix(0, 1) == 2);
    REQUIRE(kColumnMajorMatrix.View()[1, 0] == 3);
}

TEST_CASE("tensor contraction switches to bounded runtime materialization", "[Sora][Math][TensorAlgebra]") {
    Tensor<int, 33, 33, 33> left;
    Tensor<int, 33> right;
    for (size_t index = 0; index < left.kSize; ++index) {
        left[index] = 1;
    }
    for (size_t index = 0; index < right.kSize; ++index) {
        right[index] = 1;
    }

    const auto result = Contract<AxisPair{2, 0}>(left, right);
    static_assert(std::same_as<std::remove_cvref_t<decltype(result)>, Tensor<int, 33, 33>>);
    REQUIRE(result(0, 0) == 33);
    REQUIRE(result(32, 32) == 33);
}

TEST_CASE("large elementwise results support non-default carriers", "[Sora][Math][TensorAlgebra]") {
    Tensor<int, 33, 33> input;
    for (size_t index = 0; index < input.kSize; ++index) {
        input[index] = static_cast<int>(index);
    }

    const auto result = Elementwise([](int value) { return NonDefaultScalar{value + 1}; }, input);
    static_assert(std::same_as<std::remove_cvref_t<decltype(result)>, Tensor<NonDefaultScalar, 33, 33>>);
    REQUIRE(result[0].value == 1);
    REQUIRE(result[result.kSize - 1].value == static_cast<int>(result.kSize));
}

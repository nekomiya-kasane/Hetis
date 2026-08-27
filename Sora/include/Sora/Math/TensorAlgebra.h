/**
 * @file TensorAlgebra.h
 * @brief Logical-index tensor algorithms for elementwise maps, axis permutations, relayout, and contraction.
 * @ingroup Math
 */
#pragma once

#include <Sora/Core/Traits/MetaInfo.h>
#include <Sora/Math/Tensor.h>
#include <Sora/Math/Matrix.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <functional>
#include <meta>
#include <type_traits>
#include <utility>

namespace Sora::Math {

    /** @brief One pair of axes contracted by a tensor contraction. */
    struct AxisPair {
        size_t left;
        size_t right;
    };

    namespace TensorAlgebraDetail {

        template<size_t... Axes>
        [[nodiscard]] consteval bool UniqueAxes() noexcept {
            constexpr std::array axes{Axes...};
            for (size_t i = 0; i < axes.size(); ++i) {
                for (size_t j = i + 1; j < axes.size(); ++j) {
                    if (axes[i] == axes[j]) {
                        return false;
                    }
                }
            }
            return true;
        }

        template<size_t Axis, size_t... Extents>
            requires(Axis < sizeof...(Extents))
        [[nodiscard]] consteval size_t ExtentAt() noexcept {
            constexpr std::array extents{Extents...};
            return extents[Axis];
        }

        template<size_t... Extents>
        [[nodiscard]] consteval auto ExtentsArray() noexcept {
            return std::array<size_t, sizeof...(Extents)>{Extents...};
        }

        template<size_t Rank>
        [[nodiscard]] consteval auto PermutedExtents(const std::array<size_t, Rank>& axes,
                                                      const std::array<size_t, Rank>& extents) noexcept {
            std::array<size_t, Rank> result{};
            for (size_t axis = 0; axis < Rank; ++axis) {
                result[axis] = extents[axes[axis]];
            }
            return result;
        }

        template<AxisPair... Pairs>
        [[nodiscard]] consteval bool UniqueContractedAxes() noexcept {
            constexpr std::array leftAxes{Pairs.left...};
            constexpr std::array rightAxes{Pairs.right...};
            for (size_t i = 0; i < leftAxes.size(); ++i) {
                for (size_t j = i + 1; j < leftAxes.size(); ++j) {
                    if (leftAxes[i] == leftAxes[j] || rightAxes[i] == rightAxes[j]) {
                        return false;
                    }
                }
            }
            return true;
        }

        template<size_t PairCount, size_t LeftRank, size_t RightRank>
        [[nodiscard]] consteval auto ContractExtents(const std::array<AxisPair, PairCount>& pairs,
                                                     const std::array<size_t, LeftRank>& left,
                                                     const std::array<size_t, RightRank>& right) noexcept {
            constexpr size_t outputRank = LeftRank + RightRank - 2 * PairCount;
            std::array<size_t, outputRank> output{};
            size_t outputAxis = 0;
            for (size_t axis = 0; axis < LeftRank; ++axis) {
                bool contracted = false;
                for (const auto pair : pairs) {
                    contracted = contracted || axis == pair.left;
                }
                if (!contracted) {
                    output[outputAxis++] = left[axis];
                }
            }
            for (size_t axis = 0; axis < RightRank; ++axis) {
                bool contracted = false;
                for (const auto pair : pairs) {
                    contracted = contracted || axis == pair.right;
                }
                if (!contracted) {
                    output[outputAxis++] = right[axis];
                }
            }
            return output;
        }

        template<size_t Rank>
        [[nodiscard]] constexpr auto Coordinates(size_t linearIndex, const std::array<size_t, Rank>& extents) noexcept {
            std::array<size_t, Rank> coordinates{};
            for (size_t axis = Rank; axis-- > 0;) {
                coordinates[axis] = linearIndex % extents[axis];
                linearIndex /= extents[axis];
            }
            return coordinates;
        }

        template<size_t Rank>
        [[nodiscard]] constexpr size_t LinearIndex(const std::array<size_t, Rank>& coordinates,
                                                   const std::array<size_t, Rank>& extents) noexcept {
            size_t linearIndex = 0;
            for (size_t axis = 0; axis < Rank; ++axis) {
                linearIndex = linearIndex * extents[axis] + coordinates[axis];
            }
            return linearIndex;
        }

        template<size_t Rank>
        [[nodiscard]] consteval size_t ExtentProduct(const std::array<size_t, Rank>& extents) noexcept {
            size_t product = 1;
            for (const auto extent : extents) {
                product *= extent;
            }
            return product;
        }

        inline constexpr size_t kStaticExpansionWorkBudget = 1024;

        template<typename R, size_t Count, typename Function>
            requires std::invocable<Function&, size_t> &&
                     std::constructible_from<R, std::invoke_result_t<Function&, size_t>>
        [[nodiscard]] constexpr auto GenerateValues(Function&& function) {
            if constexpr (Count <= kStaticExpansionWorkBudget || !std::default_initializable<R> ||
                          !std::assignable_from<R&, std::invoke_result_t<Function&, size_t>>) {
                return GenerateStaticArray<R, Count>([&](auto index) {
                    return R(std::invoke(function, size_t{decltype(index)::value}));
                });
            } else {
                std::array<R, Count> values{R(std::invoke(function, size_t{0}))};
                for (size_t index = 1; index < Count; ++index) {
                    values[index] = R(std::invoke(function, index));
                }
                return values;
            }
        }

    } // namespace TensorAlgebraDetail

    /**
     * @brief Apply @p operation to one or more equally shaped tensor operands.
     * @details Operands are read through logical linear indexing; physical dense layout is never observable.
     */
    template<typename Operation, TensorElement FirstElement, size_t... Extents, TensorElement... RestElements>
        requires(std::invocable<Operation&, const FirstElement&, const RestElements&...>)
    [[nodiscard]] constexpr auto Elementwise(Operation&& operation, const Tensor<FirstElement, Extents...>& first,
                                             const Tensor<RestElements, Extents...>&... rest) {
        using Result =
            std::remove_cvref_t<std::invoke_result_t<Operation&, const FirstElement&, const RestElements&...>>;
        static_assert(TensorElement<Result>);
        return Tensor<Result, Extents...>{TensorAlgebraDetail::GenerateValues<
            Result, Tensor<FirstElement, Extents...>::kSize>([&](size_t index) {
            return std::invoke(operation, first[index], rest[index]...);
        })};
    }

    /** @brief Materialize an explicit scalar broadcast over a fixed tensor shape. */
    template<size_t... Extents, TensorElement Scalar>
        requires(sizeof...(Extents) > 0 && ((Extents > 0) && ...))
    [[nodiscard]] constexpr auto BroadcastTo(const Scalar& value) {
        return Tensor<Scalar, Extents...>{TensorAlgebraDetail::GenerateValues<Scalar, (Extents * ...)>(
            [&](size_t) { return value; })};
    }

    /** @brief Apply an elementwise operation to a tensor and an explicitly broadcast scalar. */
    template<typename Operation, TensorElement FirstElement, size_t... Extents, TensorElement Scalar>
        requires(std::is_arithmetic_v<Scalar> && std::invocable<Operation&, const FirstElement&, const Scalar&>)
    [[nodiscard]] constexpr auto Elementwise(Operation&& operation, const Tensor<FirstElement, Extents...>& first,
                                             const Scalar& scalar) {
        using Result = std::remove_cvref_t<std::invoke_result_t<Operation&, const FirstElement&, const Scalar&>>;
        static_assert(TensorElement<Result>);
        return Tensor<Result, Extents...>{TensorAlgebraDetail::GenerateValues<
            Result, Tensor<FirstElement, Extents...>::kSize>([&](size_t index) {
            return std::invoke(operation, first[index], scalar);
        })};
    }

    /**
     * @brief Permute tensor axes according to the output-to-input axis sequence.
     * @details If @p Axes is @c {2,0,1}, output axis 0 reads input axis 2, output axis 1 reads input axis 0, and output
     * axis 2 reads input axis 1. The result is a row-major general tensor.
     */
    template<size_t... Axes, TensorElement T, size_t... Extents>
        requires(sizeof...(Axes) == sizeof...(Extents) && ((Axes < sizeof...(Extents)) && ...) &&
                 TensorAlgebraDetail::UniqueAxes<Axes...>())
    [[nodiscard]] constexpr auto PermuteAxes(const Tensor<T, Extents...>& input) {
        static_assert(TensorAlgebraDetail::UniqueAxes<Axes...>(), "PermuteAxes requires unique axes");
        static_assert(((Axes < sizeof...(Extents)) && ...), "PermuteAxes axis is out of range");
        constexpr std::array axes{Axes...};
        constexpr std::array extents{Extents...};
        constexpr auto outputExtents = TensorAlgebraDetail::PermutedExtents(std::array{Axes...}, std::array{Extents...});
        using Output = Sora::Meta::InfoType<Detail::TensorTypeInfo<T>(outputExtents)>;
        return Output{TensorAlgebraDetail::GenerateValues<T, Output::kSize>([&](size_t outputIndex) {
            const auto outputCoordinates = TensorAlgebraDetail::Coordinates(outputIndex, outputExtents);
            std::array<size_t, sizeof...(Extents)> inputCoordinates{};
            for (size_t outputAxis = 0; outputAxis < axes.size(); ++outputAxis) {
                inputCoordinates[axes[outputAxis]] = outputCoordinates[outputAxis];
            }
            return input[TensorAlgebraDetail::LinearIndex(inputCoordinates, extents)];
        })};
    }

    /** @brief Change physical dense layout without changing logical tensor values or interpretation. */
    template<MatrixLayout Layout, TensorElement T, size_t... Extents>
    [[nodiscard]] constexpr auto Relayout(const Tensor<T, Extents...>& input) {
        using Shape = StaticShape<Extents...>;
        using Storage = StaticDenseStorage<T, Shape, MatrixMapping<Layout>>;
        return BasicTensor<Storage, Algebra::GeneralTensor>{Storage{
            LogicalValues, TensorAlgebraDetail::GenerateValues<T, Tensor<T, Extents...>::kSize>([&](size_t index) {
                return input[index];
            })}};
    }

    /**
     * @brief Contract one or more axis pairs using multiplication followed by addition.
     * @details Result axes are the uncontracted left axes followed by the uncontracted right axes. Every pair must
     * use a distinct axis on each operand and matching extents. A complete contraction returns the product carrier
     * as a scalar.
     */
    template<AxisPair... Pairs, TensorElement LeftElement, size_t... LeftExtents, TensorElement RightElement,
             size_t... RightExtents>
        requires(sizeof...(Pairs) > 0 &&
                  ((Pairs.left < sizeof...(LeftExtents) && Pairs.right < sizeof...(RightExtents)) && ...) &&
                  TensorAlgebraDetail::UniqueContractedAxes<Pairs...>() &&
                  ((TensorAlgebraDetail::ExtentAt<Pairs.left, LeftExtents...>() ==
                    TensorAlgebraDetail::ExtentAt<Pairs.right, RightExtents...>()) && ...) &&
                  requires(const LeftElement& left, const RightElement& right) {
                     Sora::Math::Mul(left, right);
                     Sora::Math::Add(Sora::Math::Mul(left, right), Sora::Math::Mul(left, right));
                 } &&
                 std::assignable_from<
                     std::remove_cvref_t<decltype(Sora::Math::Mul(std::declval<const LeftElement&>(),
                                                                   std::declval<const RightElement&>()))>&,
                     decltype(Sora::Math::Add(Sora::Math::Mul(std::declval<const LeftElement&>(),
                                                               std::declval<const RightElement&>()),
                                              Sora::Math::Mul(std::declval<const LeftElement&>(),
                                                              std::declval<const RightElement&>())))>)
    [[nodiscard]] constexpr auto Contract(const Tensor<LeftElement, LeftExtents...>& left,
                                          const Tensor<RightElement, RightExtents...>& right) {
        static_assert(TensorAlgebraDetail::UniqueContractedAxes<Pairs...>(),
                      "Contract requires unique axes on each operand");
        static_assert(((TensorAlgebraDetail::ExtentAt<Pairs.left, LeftExtents...>() ==
                        TensorAlgebraDetail::ExtentAt<Pairs.right, RightExtents...>()) && ...),
                      "Contracted extents must match");

        using Product = std::remove_cvref_t<decltype(Sora::Math::Mul(std::declval<const LeftElement&>(),
                                                                      std::declval<const RightElement&>()))>;
        static_assert(TensorElement<Product>);
        constexpr size_t pairCount = sizeof...(Pairs);
        constexpr std::array leftAxes{Pairs.left...};
        constexpr std::array rightAxes{Pairs.right...};
        constexpr std::array contractionExtents{TensorAlgebraDetail::ExtentAt<Pairs.left, LeftExtents...>()...};
        constexpr auto leftExtents = std::array{LeftExtents...};
        constexpr auto rightExtents = std::array{RightExtents...};
        constexpr auto outputExtents = TensorAlgebraDetail::ContractExtents(
            std::array{Pairs...}, std::array{LeftExtents...}, std::array{RightExtents...});
        constexpr size_t outputSize = TensorAlgebraDetail::ExtentProduct(outputExtents);
        using Output = Sora::Meta::InfoType<Detail::TensorTypeInfo<Product>(outputExtents)>;

        auto evaluate = [&](size_t outputIndex) {
            const auto outputCoordinates = TensorAlgebraDetail::Coordinates(outputIndex, outputExtents);
            std::array<size_t, sizeof...(LeftExtents)> leftCoordinates{};
            std::array<size_t, sizeof...(RightExtents)> rightCoordinates{};
            size_t outputAxis = 0;
            for (size_t axis = 0; axis < leftCoordinates.size(); ++axis) {
                bool contracted = false;
                for (size_t pair = 0; pair < pairCount; ++pair) {
                    contracted = contracted || leftAxes[pair] == axis;
                }
                if (!contracted) {
                    leftCoordinates[axis] = outputCoordinates[outputAxis++];
                }
            }
            for (size_t axis = 0; axis < rightCoordinates.size(); ++axis) {
                bool contracted = false;
                for (size_t pair = 0; pair < pairCount; ++pair) {
                    contracted = contracted || rightAxes[pair] == axis;
                }
                if (!contracted) {
                    rightCoordinates[axis] = outputCoordinates[outputAxis++];
                }
            }

            std::array<size_t, pairCount> contractionCoordinates{};
            auto reduce = [&](auto&& self, size_t depth) -> Product {
                if (depth == pairCount) {
                    for (size_t pair = 0; pair < pairCount; ++pair) {
                        leftCoordinates[leftAxes[pair]] = contractionCoordinates[pair];
                        rightCoordinates[rightAxes[pair]] = contractionCoordinates[pair];
                    }
                    return Sora::Math::Mul(left[TensorAlgebraDetail::LinearIndex(leftCoordinates, leftExtents)],
                                           right[TensorAlgebraDetail::LinearIndex(rightCoordinates, rightExtents)]);
                }

                Product result = self(self, depth + 1);
                for (size_t coordinate = 1; coordinate < contractionExtents[depth]; ++coordinate) {
                    contractionCoordinates[depth] = coordinate;
                    result = Sora::Math::Add(result, self(self, depth + 1));
                }
                contractionCoordinates[depth] = 0;
                return result;
            };
            return reduce(reduce, 0);
        };

        if constexpr (outputExtents.empty()) {
            return evaluate(0);
        } else {
            return Output{TensorAlgebraDetail::GenerateValues<Product, outputSize>([&](size_t index) {
                return evaluate(index);
            })};
        }
    }

    /** @brief Contract using one pair of integer axis indices. */
    template<size_t LeftAxis, size_t RightAxis, TensorElement LeftElement, size_t... LeftExtents,
             TensorElement RightElement, size_t... RightExtents>
        requires(LeftAxis < sizeof...(LeftExtents) && RightAxis < sizeof...(RightExtents))
    [[nodiscard]] constexpr auto Contract(const Tensor<LeftElement, LeftExtents...>& left,
                                          const Tensor<RightElement, RightExtents...>& right) {
        return Contract<AxisPair{LeftAxis, RightAxis}>(left, right);
    }

    /** @brief Multiply equally shaped tensors elementwise. */
    template<TensorElement LeftElement, TensorElement RightElement, size_t... Extents>
        requires requires(const LeftElement& left, const RightElement& right) { Sora::Math::Mul(left, right); }
    [[nodiscard]] constexpr auto Hadamard(const Tensor<LeftElement, Extents...>& left,
                                          const Tensor<RightElement, Extents...>& right) {
        return Elementwise([](const LeftElement& a, const RightElement& b) { return Sora::Math::Mul(a, b); }, left,
                           right);
    }

    /** @brief Project a rank-two tensor contraction onto the existing matrix-vector operation. */
    template<TensorElement MatrixElement, TensorElement VectorElement, size_t Rows, size_t Columns>
        requires requires(const Matrix<MatrixElement, Rows, Columns>& matrix,
                          const Vector<VectorElement, Columns>& vector) { matrix * vector; }
    [[nodiscard]] constexpr auto MatVec(const Tensor<MatrixElement, Rows, Columns>& matrix,
                                        const Tensor<VectorElement, Columns>& vector)
        -> Vector<Traits::MulResult<MatrixElement, VectorElement>, Rows> {
        using LinearStorage = StaticDenseStorage<MatrixElement, StaticShape<Rows, Columns>,
                                                 MatrixMapping<MatrixLayout::ColumnMajor>>;
        Matrix<MatrixElement, Rows, Columns> linear{LinearStorage{LogicalValues, matrix.Values()}};
        Vector<VectorElement, Columns> freeVector{vector.Values()};
        return linear * freeVector;
    }

    /** @brief Project a rank-two tensor contraction onto the existing matrix-matrix operation. */
    template<TensorElement LeftElement, TensorElement RightElement, size_t Rows, size_t Inner, size_t Columns>
        requires requires(const Matrix<LeftElement, Rows, Inner>& left,
                          const Matrix<RightElement, Inner, Columns>& right) { left * right; }
    [[nodiscard]] constexpr auto MatMul(const Tensor<LeftElement, Rows, Inner>& left,
                                        const Tensor<RightElement, Inner, Columns>& right)
        -> Matrix<Traits::MulResult<LeftElement, RightElement>, Rows, Columns> {
        using LeftStorage = StaticDenseStorage<LeftElement, StaticShape<Rows, Inner>,
                                               MatrixMapping<MatrixLayout::ColumnMajor>>;
        using RightStorage = StaticDenseStorage<RightElement, StaticShape<Inner, Columns>,
                                                MatrixMapping<MatrixLayout::ColumnMajor>>;
        Matrix<LeftElement, Rows, Inner> leftMatrix{LeftStorage{LogicalValues, left.Values()}};
        Matrix<RightElement, Inner, Columns> rightMatrix{RightStorage{LogicalValues, right.Values()}};
        return leftMatrix * rightMatrix;
    }

    /** @brief Project a rank-two tensor axis permutation onto the existing matrix transpose operation. */
    template<TensorElement T, size_t Rows, size_t Columns>
    [[nodiscard]] constexpr auto Transpose(const Tensor<T, Rows, Columns>& input) {
        return PermuteAxes<1, 0>(input);
    }

} // namespace Sora::Math

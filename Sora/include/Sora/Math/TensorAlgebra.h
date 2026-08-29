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

        template<Sora::Concept::TensorValue First, Sora::Concept::TensorValue... Rest>
        inline constexpr bool kSameShape = ((First::kExtents == Rest::kExtents) && ...);

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
    template<typename Operation, Sora::Concept::DenseTensor First, Sora::Concept::DenseTensor... Rest>
        requires TensorAlgebraDetail::kSameShape<First, Rest...> &&
                 std::invocable<Operation&, const typename First::ElementType&, const typename Rest::ElementType&...>
    [[nodiscard]] constexpr auto Elementwise(Operation&& operation, const First& first, const Rest&... rest) {
        using Result = std::remove_cvref_t<std::invoke_result_t<Operation&, const typename First::ElementType&,
                                                               const typename Rest::ElementType&...>>;
        static_assert(TensorElement<Result>);
        using Output = typename First::template RebindElement<Result>;
        using OutputStorage = typename Output::StorageType;
        return Output{OutputStorage{LogicalValues, TensorAlgebraDetail::GenerateValues<Result, First::kElementCount>(
            [&](size_t index) {
            return std::invoke(operation, first[index], rest[index]...);
        })}};
    }

    /** @brief Materialize an explicit scalar broadcast over a fixed tensor shape. */
    template<size_t... Extents, TensorElement Scalar>
        requires(sizeof...(Extents) > 0 && ((Extents > 0) && ...))
    [[nodiscard]] constexpr auto BroadcastTo(const Scalar& value) {
        return Tensor<Scalar, Extents...>{TensorAlgebraDetail::GenerateValues<Scalar, (Extents * ...)>(
            [&](size_t) { return value; })};
    }

    /** @brief Apply an elementwise operation to a tensor and an explicitly broadcast scalar. */
    template<typename Operation, Sora::Concept::DenseTensor First, TensorElement Scalar>
        requires(!Sora::Concept::TensorValue<std::remove_cvref_t<Scalar>> &&
                 std::invocable<Operation&, const typename First::ElementType&, const Scalar&>)
    [[nodiscard]] constexpr auto Elementwise(Operation&& operation, const First& first, const Scalar& scalar) {
        using Result =
            std::remove_cvref_t<std::invoke_result_t<Operation&, const typename First::ElementType&, const Scalar&>>;
        static_assert(TensorElement<Result>);
        using Output = typename First::template RebindElement<Result>;
        using OutputStorage = typename Output::StorageType;
        return Output{OutputStorage{LogicalValues, TensorAlgebraDetail::GenerateValues<Result, First::kElementCount>(
            [&](size_t index) {
            return std::invoke(operation, first[index], scalar);
        })}};
    }

    /**
     * @brief Permute tensor axes according to the output-to-input axis sequence.
     * @details If @p Axes is @c {2,0,1}, output axis 0 reads input axis 2, output axis 1 reads input axis 0, and output
     * axis 2 reads input axis 1. The result is a row-major general tensor.
     */
    template<size_t... Axes, Sora::Concept::DenseTensor Input>
        requires(sizeof...(Axes) == Input::kRank && ((Axes < Input::kRank) && ...) &&
                 TensorAlgebraDetail::UniqueAxes<Axes...>())
    [[nodiscard]] constexpr auto PermuteAxes(const Input& input) {
        using T = typename Input::ElementType;
        static_assert(TensorAlgebraDetail::UniqueAxes<Axes...>(), "PermuteAxes requires unique axes");
        static_assert(((Axes < Input::kRank) && ...), "PermuteAxes axis is out of range");
        constexpr std::array axes{Axes...};
        constexpr auto extents = Input::kExtents;
        constexpr auto outputExtents =
            TensorAlgebraDetail::PermutedExtents(std::array{Axes...}, Input::kExtents);
        using Output = Sora::Meta::InfoType<Detail::TensorTypeInfo<T>(outputExtents)>;
        return Output{TensorAlgebraDetail::GenerateValues<T, Output::kSize>([&](size_t outputIndex) {
            const auto outputCoordinates = TensorAlgebraDetail::Coordinates(outputIndex, outputExtents);
            std::array<size_t, Input::kRank> inputCoordinates{};
            for (size_t outputAxis = 0; outputAxis < axes.size(); ++outputAxis) {
                inputCoordinates[axes[outputAxis]] = outputCoordinates[outputAxis];
            }
            return input[TensorAlgebraDetail::LinearIndex(inputCoordinates, extents)];
        })};
    }

    /** @brief Change physical dense layout without changing logical tensor values or interpretation. */
    template<MatrixLayout Layout, Sora::Concept::DenseTensor Input>
        requires(Layout == MatrixLayout::ColumnMajor || Layout == MatrixLayout::RowMajor)
    [[nodiscard]] constexpr auto Relayout(const Input& input) {
        using T = typename Input::ElementType;
        using Shape = typename Input::ShapeType;
        using Storage = StaticDenseStorage<T, Shape, MatrixMapping<Layout>>;
        return BasicTensor<Storage, typename Input::InterpretationType>{Storage{
            LogicalValues, TensorAlgebraDetail::GenerateValues<T, Input::kElementCount>([&](size_t index) {
                return input[index];
            })}};
    }

    /**
     * @brief Contract one or more axis pairs using multiplication followed by addition.
     * @details Result axes are the uncontracted left axes followed by the uncontracted right axes. Every pair must
     * use a distinct axis on each operand and matching extents. A complete contraction returns the product carrier
     * as a scalar.
     */
    template<AxisPair... Pairs, Sora::Concept::DenseTensor Left, Sora::Concept::DenseTensor Right>
        requires(sizeof...(Pairs) > 0 &&
                  ((Pairs.left < Left::kRank && Pairs.right < Right::kRank) && ...) &&
                  TensorAlgebraDetail::UniqueContractedAxes<Pairs...>() &&
                  ((Left::kExtents[Pairs.left] == Right::kExtents[Pairs.right]) && ...) &&
                  requires(const typename Left::ElementType& left, const typename Right::ElementType& right) {
                     Sora::Math::Mul(left, right);
                     Sora::Math::Add(Sora::Math::Mul(left, right), Sora::Math::Mul(left, right));
                 } &&
                  std::assignable_from<
                     std::remove_cvref_t<decltype(Sora::Math::Mul(
                         std::declval<const typename Left::ElementType&>(),
                         std::declval<const typename Right::ElementType&>()))>&,
                     decltype(Sora::Math::Add(
                         Sora::Math::Mul(std::declval<const typename Left::ElementType&>(),
                                         std::declval<const typename Right::ElementType&>()),
                         Sora::Math::Mul(std::declval<const typename Left::ElementType&>(),
                                         std::declval<const typename Right::ElementType&>())))>)
    [[nodiscard]] constexpr auto Contract(const Left& left, const Right& right) {
        using LeftElement = typename Left::ElementType;
        using RightElement = typename Right::ElementType;
        static_assert(TensorAlgebraDetail::UniqueContractedAxes<Pairs...>(),
                      "Contract requires unique axes on each operand");
        static_assert(((Left::kExtents[Pairs.left] == Right::kExtents[Pairs.right]) && ...),
                      "Contracted extents must match");

        using Product = std::remove_cvref_t<decltype(Sora::Math::Mul(std::declval<const LeftElement&>(),
                                                                      std::declval<const RightElement&>()))>;
        static_assert(TensorElement<Product>);
        constexpr size_t pairCount = sizeof...(Pairs);
        constexpr std::array leftAxes{Pairs.left...};
        constexpr std::array rightAxes{Pairs.right...};
        constexpr std::array contractionExtents{Left::kExtents[Pairs.left]...};
        constexpr auto leftExtents = Left::kExtents;
        constexpr auto rightExtents = Right::kExtents;
        constexpr auto outputExtents = TensorAlgebraDetail::ContractExtents(
            std::array{Pairs...}, Left::kExtents, Right::kExtents);
        constexpr size_t outputSize = TensorAlgebraDetail::ExtentProduct(outputExtents);
        using Output = Sora::Meta::InfoType<Detail::TensorTypeInfo<Product>(outputExtents)>;

        auto evaluate = [&](size_t outputIndex) {
            const auto outputCoordinates = TensorAlgebraDetail::Coordinates(outputIndex, outputExtents);
            std::array<size_t, Left::kRank> leftCoordinates{};
            std::array<size_t, Right::kRank> rightCoordinates{};
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
                    if constexpr (pairCount == 1 &&
                                  requires(const LeftElement& a, const RightElement& b, const Product& addend) {
                                      requires std::assignable_from<Product&, decltype(Sora::Math::Fma(a, b, addend))>;
                                  }) {
                        if consteval {
                            result = Sora::Math::Add(
                                result, self(self, depth + 1));
                        } else {
                            for (size_t pair = 0; pair < pairCount; ++pair) {
                                leftCoordinates[leftAxes[pair]] = contractionCoordinates[pair];
                                rightCoordinates[rightAxes[pair]] = contractionCoordinates[pair];
                            }
                            result = Sora::Math::Fma(
                                left[TensorAlgebraDetail::LinearIndex(leftCoordinates, leftExtents)],
                                right[TensorAlgebraDetail::LinearIndex(rightCoordinates, rightExtents)], result);
                        }
                    } else {
                        result = Sora::Math::Add(result, self(self, depth + 1));
                    }
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
    template<size_t LeftAxis, size_t RightAxis, Sora::Concept::DenseTensor Left, Sora::Concept::DenseTensor Right>
        requires(LeftAxis < Left::kRank && RightAxis < Right::kRank)
    [[nodiscard]] constexpr auto Contract(const Left& left, const Right& right) {
        return Contract<AxisPair{LeftAxis, RightAxis}>(left, right);
    }

    /** @brief Multiply equally shaped dense tensors elementwise. */
    template<Sora::Concept::DenseTensor Left, Sora::Concept::DenseTensor Right>
        requires(Left::kExtents == Right::kExtents) &&
                requires(const typename Left::ElementType& left, const typename Right::ElementType& right) {
                    Sora::Math::Mul(left, right);
                }
    [[nodiscard]] constexpr auto Hadamard(const Left& left, const Right& right) {
        return Elementwise(
            [](const typename Left::ElementType& a, const typename Right::ElementType& b) {
                return Sora::Math::Mul(a, b);
            },
            left, right);
    }

    /** @brief Contract a rank-two dense tensor with a rank-one dense tensor. */
    template<Sora::Concept::DenseTensor MatrixTensor, Sora::Concept::DenseTensor VectorTensor>
        requires(MatrixTensor::kRank == 2 && VectorTensor::kRank == 1 &&
                 MatrixTensor::kExtents[1] == VectorTensor::kExtents[0]) &&
                requires(const MatrixTensor& matrix, const VectorTensor& vector) {
                    Contract<AxisPair{1, 0}>(matrix, vector);
                }
    [[nodiscard]] constexpr auto MatVec(const MatrixTensor& matrix, const VectorTensor& vector) {
        if constexpr (std::same_as<typename MatrixTensor::InterpretationType, Algebra::LinearMap> &&
                      std::same_as<typename VectorTensor::InterpretationType, Algebra::FreeVector> &&
                      requires { matrix * vector; }) {
            return matrix * vector;
        } else {
            return Contract<AxisPair{1, 0}>(matrix, vector);
        }
    }

    /** @brief Contract two rank-two dense tensors over the inner axis. */
    template<Sora::Concept::DenseTensor LeftTensor, Sora::Concept::DenseTensor RightTensor>
        requires(LeftTensor::kRank == 2 && RightTensor::kRank == 2 &&
                 LeftTensor::kExtents[1] == RightTensor::kExtents[0]) &&
                requires(const LeftTensor& left, const RightTensor& right) {
                    Contract<AxisPair{1, 0}>(left, right);
                }
    [[nodiscard]] constexpr auto MatMul(const LeftTensor& left, const RightTensor& right) {
        if constexpr (std::same_as<typename LeftTensor::InterpretationType, Algebra::LinearMap> &&
                      std::same_as<typename RightTensor::InterpretationType, Algebra::LinearMap> &&
                      requires { left * right; }) {
            return left * right;
        } else {
            return Contract<AxisPair{1, 0}>(left, right);
        }
    }

    /** @brief Project a rank-two tensor axis permutation onto the existing matrix transpose operation. */
    template<Sora::Concept::DenseTensor Input>
        requires(Input::kRank == 2)
    [[nodiscard]] constexpr auto Transpose(const Input& input) {
        return PermuteAxes<1, 0>(input);
    }

} // namespace Sora::Math

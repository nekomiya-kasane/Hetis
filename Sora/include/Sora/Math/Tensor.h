/**
 * @file Tensor.h
 * @brief Fixed-shape contiguous tensors and allocation-free tensor products.
 * @ingroup Math
 */
#pragma once

#include <Sora/Core/Traits/MetaInfo.h>
#include <Sora/Math/PrimaryFunctions.h>
#include <Sora/Math/TensorCore.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <functional>
#include <meta>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Math {

    /**
     * @brief Row-major fixed-shape tensor with no allocation or runtime shape state.
     * @tparam T Stored element type.
     * @tparam Extents Positive compile-time extents, ordered from the outermost to the innermost axis.
     */
    template<TensorElement T, size_t... Extents>
        requires(sizeof...(Extents) > 0 && ((Extents > 0) && ...))
    using Tensor = BasicTensor<StaticDenseStorage<T, StaticShape<Extents...>, RightMapping>, Algebra::GeneralTensor>;

    namespace Detail {

        template<TensorElement R, std::ranges::input_range Extents>
            requires std::convertible_to<std::ranges::range_reference_t<Extents>, size_t>
        [[nodiscard]] consteval std::meta::info TensorTypeInfo(Extents&& extents) {
            std::vector<std::meta::info> arguments{^^R};
            for (const size_t extent : extents) {
                if (extent == 0) {
                    throw std::define_static_string("Tensor extents must be positive.");
                }
                arguments.push_back(std::meta::reflect_constant(extent));
            }
            if (arguments.size() == 1) {
                return ^^R;
            }
            if (!std::meta::can_substitute(^^Tensor, arguments)) {
                throw std::define_static_string("Tensor type substitution failed.");
            }
            const auto result = std::meta::substitute(^^Tensor, arguments);
            if (!std::meta::is_type(result)) {
                throw std::define_static_string("Tensor type substitution did not produce a type.");
            }
            return result;
        }

        template<size_t Axis, TensorElement R, size_t... Extents>
            requires(Axis < sizeof...(Extents))
        [[nodiscard]] consteval std::meta::info RemoveTensorAxisInfo() {
            constexpr std::array extents{Extents...};
            constexpr size_t outputRank = sizeof...(Extents) - 1;
            std::array<size_t, outputRank> output{};
            size_t outputAxis = 0;
            for (size_t index = 0; index < extents.size(); ++index) {
                if (index != Axis) {
                    output[outputAxis++] = extents[index];
                }
            }
            return TensorTypeInfo<R>(output);
        }

        template<size_t Axis, TensorElement R, size_t... Extents>
        using RemoveTensorAxis = Sora::Meta::InfoType<RemoveTensorAxisInfo<Axis, R, Extents...>()>;

        template<size_t Axis, size_t NewExtent, TensorElement R, size_t... Extents>
            requires(Axis < sizeof...(Extents) && NewExtent > 0)
        [[nodiscard]] consteval std::meta::info ReplaceTensorAxisInfo() {
            constexpr std::array extents{Extents...};
            std::array<size_t, sizeof...(Extents)> output{};
            for (size_t index = 0; index < extents.size(); ++index) {
                output[index] = index == Axis ? NewExtent : extents[index];
            }
            return TensorTypeInfo<R>(output);
        }

        template<size_t Axis, size_t NewExtent, TensorElement R, size_t... Extents>
        using ReplaceTensorAxis = Sora::Meta::InfoType<ReplaceTensorAxisInfo<Axis, NewExtent, R, Extents...>()>;

        template<size_t Axis, size_t... Extents>
        [[nodiscard]] consteval size_t TensorInnerStride() noexcept {
            constexpr std::array extents{Extents...};
            size_t stride = 1;
            for (size_t index = Axis + 1; index < extents.size(); ++index) {
                stride *= extents[index];
            }
            return stride;
        }

        template<size_t Axis, typename Evaluator, typename Parameters, TensorElement T, size_t... Extents>
        [[nodiscard]] consteval bool NothrowContractTensorAxes(
            std::type_identity<Tensor<T, Extents...>>) {
            using Fiber = std::array<T, Extents...[Axis]>;
            using Parameter = decltype(std::get<Axis>(std::declval<Parameters&>()));
            using AxisIndex = std::integral_constant<size_t, Axis>;
            constexpr bool current = std::is_nothrow_copy_constructible_v<T> &&
                                     std::is_nothrow_invocable_v<Evaluator&, AxisIndex, const Fiber&, Parameter>;
            if constexpr (Axis == 0) {
                return current;
            } else {
                using R = std::remove_cvref_t<
                    std::invoke_result_t<Evaluator&, AxisIndex, const Fiber&, Parameter>>;
                using Reduced = RemoveTensorAxis<Axis, R, Extents...>;
                return current &&
                       NothrowContractTensorAxes<Axis - 1, Evaluator, Parameters>(std::type_identity<Reduced>{});
            }
        }

    } // namespace Detail

    /**
     * @brief Contract one compile-time tensor axis with @p function.
     * @details @p function receives one axis fiber. The axis is removed from the result shape; contracting the sole
     * axis returns the function result directly. Fibers preserve row-major order and require no allocation.
     */
    template<size_t Axis, TensorElement T, size_t... Extents, typename Function>
        requires(Axis < sizeof...(Extents)) &&
                std::invocable<Function&, const std::array<T, Extents...[Axis]>&> &&
                TensorElement<std::remove_cvref_t<std::invoke_result_t<
                    Function&, const std::array<T, Extents...[Axis]>&>>>
    [[nodiscard, gnu::always_inline]] constexpr auto ContractTensorAxis(
        const Tensor<T, Extents...>& input, Function&& function) noexcept(
        std::is_nothrow_copy_constructible_v<T> &&
        std::is_nothrow_invocable_v<Function&, const std::array<T, Extents...[Axis]>&>) {
        constexpr size_t rank = sizeof...(Extents);
        constexpr size_t axisExtent = Extents...[Axis];
        constexpr size_t innerStride = Detail::TensorInnerStride<Axis, Extents...>();
        using Fiber = std::array<T, axisExtent>;
        using R = std::remove_cvref_t<std::invoke_result_t<Function&, const Fiber&>>;

        if constexpr (rank == 1) {
            return std::invoke(function, input.Values());
        } else {
            constexpr size_t outputSize = Tensor<T, Extents...>::kSize / axisExtent;
            using Output = Detail::RemoveTensorAxis<Axis, R, Extents...>;
            auto values = GenerateStaticArray<R, outputSize>([&](auto outputIndex) noexcept(
                std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_invocable_v<Function&, const Fiber&>) {
                constexpr size_t outer = decltype(outputIndex)::value / innerStride;
                constexpr size_t inner = decltype(outputIndex)::value % innerStride;
                const auto fiber = GenerateStaticArray<T, axisExtent>([&](auto axisIndex) noexcept(
                    std::is_nothrow_copy_constructible_v<T>) {
                    constexpr size_t inputIndex = outer * axisExtent * innerStride +
                                                  decltype(axisIndex)::value * innerStride + inner;
                    return input[inputIndex];
                });
                return std::invoke(function, fiber);
            });
            return Output{std::move(values)};
        }
    }

    namespace Detail {

        template<size_t Axis, TensorElement T, size_t... Extents, typename Evaluator, typename Parameters>
        [[nodiscard, gnu::always_inline]] constexpr auto ContractTensorAxesImpl(
            const Tensor<T, Extents...>& input, Evaluator& evaluator, Parameters& parameters) noexcept(
            NothrowContractTensorAxes<Axis, Evaluator, Parameters>(
                std::type_identity<Tensor<T, Extents...>>{})) {
            constexpr size_t extent = Extents...[Axis];
            auto contracted = ContractTensorAxis<Axis>(
                input, [&](const std::array<T, extent>& fiber) noexcept(noexcept(std::invoke(
                           evaluator, std::integral_constant<size_t, Axis>{}, fiber,
                           std::get<Axis>(parameters)))) {
                    return std::invoke(evaluator, std::integral_constant<size_t, Axis>{}, fiber,
                                       std::get<Axis>(parameters));
                });
            if constexpr (Axis == 0) {
                return contracted;
            } else {
                return ContractTensorAxesImpl<Axis - 1>(contracted, evaluator, parameters);
            }
        }

    } // namespace Detail

    /**
     * @brief Contract every tensor axis with one rank-polymorphic evaluator and its corresponding parameter.
     * @details The contiguous last axis is contracted first. Rank, extents, axis indices, intermediate shapes, and
     * result types remain compile-time values; the operation allocates no storage and introduces no runtime dispatch.
     * The evaluator receives an axis index constant, one complete fiber, and that axis's parameter.
     */
    template<TensorElement T, size_t... Extents, typename Evaluator, typename... Parameters>
        requires(sizeof...(Parameters) == sizeof...(Extents))
    [[nodiscard, gnu::always_inline]] constexpr auto ContractTensorAxes(
        const Tensor<T, Extents...>& input, Evaluator&& evaluator, const Parameters&... parameters) noexcept(
        Detail::NothrowContractTensorAxes<sizeof...(Extents) - 1, std::remove_reference_t<Evaluator>,
                                          std::tuple<const Parameters&...>>(
            std::type_identity<Tensor<T, Extents...>>{})) {
        auto parameterTuple = std::forward_as_tuple(parameters...);
        auto&& evaluatorReference = evaluator;
        return Detail::ContractTensorAxesImpl<sizeof...(Extents) - 1>(input, evaluatorReference, parameterTuple);
    }

    /**
     * @brief Replace one compile-time tensor axis by transforming each complete fiber exactly once.
     * @details @p function returns an @c std::array with @p NewExtent values. Both input and output remain flat,
     * row-major tensors; the temporary fiber arrays have fully static storage.
     */
    template<size_t Axis, size_t NewExtent, TensorElement T, size_t... Extents, typename Function>
        requires(Axis < sizeof...(Extents) && NewExtent > 0) &&
                std::invocable<Function&, const std::array<T, Extents...[Axis]>&>
    [[nodiscard]] constexpr auto TransformTensorAxis(const Tensor<T, Extents...>& input, Function&& function) {
        constexpr size_t axisExtent = Extents...[Axis];
        constexpr size_t innerStride = Detail::TensorInnerStride<Axis, Extents...>();
        constexpr size_t fiberCount = Tensor<T, Extents...>::kSize / axisExtent;
        using Fiber = std::array<T, axisExtent>;
        using FiberResult = std::remove_cvref_t<std::invoke_result_t<Function&, const Fiber&>>;
        static_assert(std::same_as<FiberResult, std::array<typename FiberResult::value_type, NewExtent>>,
                      "TransformTensorAxis function must return std::array<R, NewExtent>");
        using R = typename FiberResult::value_type;
        static_assert(TensorElement<R>, "TransformTensorAxis result must satisfy TensorElement");

        const auto transformed = GenerateStaticArray<FiberResult, fiberCount>([&](auto fiberIndex) {
            constexpr size_t outer = decltype(fiberIndex)::value / innerStride;
            constexpr size_t inner = decltype(fiberIndex)::value % innerStride;
            const auto fiber = GenerateStaticArray<T, axisExtent>([&](auto axisIndex) {
                constexpr size_t inputIndex = outer * axisExtent * innerStride +
                                              decltype(axisIndex)::value * innerStride + inner;
                return input[inputIndex];
            });
            return std::invoke(function, fiber);
        });
        constexpr size_t outputSize = fiberCount * NewExtent;
        auto values = GenerateStaticArray<R, outputSize>([&](auto outputIndex) {
            constexpr size_t outer = decltype(outputIndex)::value / (NewExtent * innerStride);
            constexpr size_t remainder = decltype(outputIndex)::value % (NewExtent * innerStride);
            constexpr size_t axisIndex = remainder / innerStride;
            constexpr size_t inner = remainder % innerStride;
            constexpr size_t fiberIndex = outer * innerStride + inner;
            return transformed[fiberIndex][axisIndex];
        });
        using Output = Detail::ReplaceTensorAxis<Axis, NewExtent, R, Extents...>;
        return Output{std::move(values)};
    }

    /** @brief Apply @p function independently to every tensor element with full static unrolling. */
    template<TensorElement T, size_t... Extents, typename Function>
        requires std::invocable<Function&, const T&> &&
                 TensorElement<std::remove_cvref_t<std::invoke_result_t<Function&, const T&>>>
    [[nodiscard]] constexpr auto Transform(const Tensor<T, Extents...>& input, Function&& function) noexcept(
        std::is_nothrow_invocable_v<Function&, const T&>) {
        using R = std::remove_cvref_t<std::invoke_result_t<Function&, const T&>>;
        return Tensor<R, Extents...>{GenerateStaticArray<R, Tensor<T, Extents...>::kSize>(
            [&](auto index) noexcept(std::is_nothrow_invocable_v<Function&, const T&>) {
                return std::invoke(function, input[index]);
            })};
    }

    /** @brief Apply @p function pairwise to equally shaped tensors with full static unrolling. */
    template<TensorElement T, TensorElement U, size_t... Extents, typename Function>
        requires std::invocable<Function&, const T&, const U&> &&
                 TensorElement<std::remove_cvref_t<std::invoke_result_t<Function&, const T&, const U&>>>
    [[nodiscard]] constexpr auto ZipTransform(const Tensor<T, Extents...>& left, const Tensor<U, Extents...>& right,
                                              Function&& function) noexcept(
        std::is_nothrow_invocable_v<Function&, const T&, const U&>) {
        using R = std::remove_cvref_t<std::invoke_result_t<Function&, const T&, const U&>>;
        return Tensor<R, Extents...>{GenerateStaticArray<R, Tensor<T, Extents...>::kSize>(
            [&](auto index) noexcept(std::is_nothrow_invocable_v<Function&, const T&, const U&>) {
                return std::invoke(function, left[index], right[index]);
            })};
    }

    /** @brief Compute the elementwise sum of equally shaped tensors. */
    template<TensorElement T, TensorElement U, size_t... Extents>
        requires requires(T left, U right) { Sora::Math::Add(left, right); } &&
                 TensorElement<std::remove_cvref_t<decltype(Sora::Math::Add(std::declval<T>(), std::declval<U>()))>>
    [[nodiscard]] constexpr auto operator+(const Tensor<T, Extents...>& left,
                                           const Tensor<U, Extents...>& right) {
        return ZipTransform(left, right, [](const T& a, const U& b) noexcept(noexcept(Sora::Math::Add(a, b))) {
            return Sora::Math::Add(a, b);
        });
    }

    /** @brief Compute the elementwise difference of equally shaped tensors. */
    template<TensorElement T, TensorElement U, size_t... Extents>
        requires requires(T left, U right) { Sora::Math::Sub(left, right); } &&
                 TensorElement<std::remove_cvref_t<decltype(Sora::Math::Sub(std::declval<T>(), std::declval<U>()))>>
    [[nodiscard]] constexpr auto operator-(const Tensor<T, Extents...>& left,
                                           const Tensor<U, Extents...>& right) {
        return ZipTransform(left, right, [](const T& a, const U& b) noexcept(noexcept(Sora::Math::Sub(a, b))) {
            return Sora::Math::Sub(a, b);
        });
    }

    /** @brief Scale every tensor element by @p scale. */
    template<TensorElement T, typename S, size_t... Extents>
        requires requires(T value, S scale) { Sora::Math::Mul(value, scale); } &&
                 TensorElement<std::remove_cvref_t<decltype(Sora::Math::Mul(std::declval<T>(), std::declval<S>()))>>
    [[nodiscard]] constexpr auto operator*(const Tensor<T, Extents...>& tensor, const S& scale) {
        return Transform(tensor, [&scale](const T& value) noexcept(noexcept(Sora::Math::Mul(value, scale))) {
            return Sora::Math::Mul(value, scale);
        });
    }

    /** @brief Contract two equally shaped dense tensors to one scalar with fused multiply-add when available. */
    template<Sora::Concept::DenseTensor Left, Sora::Concept::DenseTensor Right>
        requires(Left::kExtents == Right::kExtents) &&
                requires(const typename Left::ElementType& left, const typename Right::ElementType& right) {
                    Sora::Math::Mul(left, right);
                    Sora::Math::Fma(left, right, Sora::Math::Mul(left, right));
                }
    [[nodiscard]] constexpr auto InnerProduct(const Left& left, const Right& right) {
        auto result = Sora::Math::Mul(left[0], right[0]);
        template for (constexpr size_t index :
                      std::define_static_array(std::views::iota(size_t{1}, Left::kElementCount))) {
            if consteval {
                result = Sora::Math::Add(result, Sora::Math::Mul(left[index], right[index]));
            } else {
                result = Sora::Math::Fma(left[index], right[index], result);
            }
        }
        return result;
    }

    /**
     * @brief Form the outer tensor product, concatenating the left and right shapes.
     * @details The result is row-major. Each left element owns one contiguous block of all right elements.
     */
    template<Sora::Concept::DenseTensor Left, Sora::Concept::DenseTensor Right, typename Product>
        requires std::invocable<Product&, const typename Left::ElementType&, const typename Right::ElementType&> &&
                 TensorElement<std::remove_cvref_t<std::invoke_result_t<
                     Product&, const typename Left::ElementType&, const typename Right::ElementType&>>>
    [[nodiscard]] constexpr auto TensorProduct(const Left& left, const Right& right, Product&& product) noexcept(
        std::is_nothrow_invocable_v<Product&, const typename Left::ElementType&, const typename Right::ElementType&>) {
        using LeftElement = typename Left::ElementType;
        using RightElement = typename Right::ElementType;
        using R = std::remove_cvref_t<std::invoke_result_t<Product&, const LeftElement&, const RightElement&>>;
        constexpr auto leftExtents = Left::kExtents;
        constexpr auto rightExtents = Right::kExtents;
        constexpr size_t outputRank = Left::kRank + Right::kRank;
        constexpr auto outputExtents = [&] {
            std::array<size_t, outputRank> result{};
            for (size_t axis = 0; axis < Left::kRank; ++axis) {
                result[axis] = leftExtents[axis];
            }
            for (size_t axis = 0; axis < Right::kRank; ++axis) {
                result[Left::kRank + axis] = rightExtents[axis];
            }
            return result;
        }();
        using Output = Sora::Meta::InfoType<Detail::TensorTypeInfo<R>(outputExtents)>;
        return Output{GenerateStaticArray<R, Left::kElementCount * Right::kElementCount>(
            [&](auto index) noexcept(std::is_nothrow_invocable_v<Product&, const LeftElement&, const RightElement&>) {
                constexpr size_t leftIndex = decltype(index)::value / Right::kElementCount;
                constexpr size_t rightIndex = decltype(index)::value % Right::kElementCount;
                return std::invoke(product, left[leftIndex], right[rightIndex]);
            })};
    }

    /** @brief Form the multiplicative outer tensor product. */
    template<Sora::Concept::DenseTensor Left, Sora::Concept::DenseTensor Right>
        requires requires(const typename Left::ElementType& left, const typename Right::ElementType& right) {
            Sora::Math::Mul(left, right);
        }
    [[nodiscard]] constexpr auto TensorProduct(const Left& left, const Right& right) {
        using LeftElement = typename Left::ElementType;
        using RightElement = typename Right::ElementType;
        return TensorProduct(left, right, [](const LeftElement& a,
                                             const RightElement& b) noexcept(noexcept(Sora::Math::Mul(a, b))) {
            return Sora::Math::Mul(a, b);
        });
    }

} // namespace Sora::Math

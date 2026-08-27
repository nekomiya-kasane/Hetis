/**
 * @file Tensor.h
 * @brief Fixed-shape contiguous tensors and allocation-free tensor products.
 * @ingroup Math
 */
#pragma once

#include <Sora/Math/PrimaryFunctions.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <functional>
#include <meta>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Sora::Math {

    namespace Detail {

        template<typename R, size_t Count, typename Function, size_t... Indices>
        [[nodiscard, gnu::always_inline]] constexpr auto GenerateArray(Function&& function,
                                                                        std::index_sequence<Indices...>)
            noexcept((std::is_nothrow_invocable_r_v<R, Function&, std::integral_constant<size_t, Indices>> && ...)) {
            return std::array<R, Count>{
                std::invoke(function, std::integral_constant<size_t, Indices>{})...};
        }

        template<typename R, size_t Count, typename Function>
        [[nodiscard, gnu::always_inline]] constexpr auto GenerateArray(Function&& function)
            noexcept(noexcept(GenerateArray<R, Count>(std::declval<Function&>(), std::make_index_sequence<Count>{}))) {
            return GenerateArray<R, Count>(function, std::make_index_sequence<Count>{});
        }

    } // namespace Detail

    /** @brief Trivially copyable value suitable for inline fixed-shape tensor storage. */
    template<typename T>
    concept TensorElement = std::is_object_v<T> && std::same_as<T, std::remove_cv_t<T>> && std::copyable<T> &&
                            std::is_trivially_copyable_v<T>;

    /**
     * @brief Row-major fixed-shape tensor with no allocation or runtime shape state.
     * @tparam T Stored element type.
     * @tparam Extents Positive compile-time extents, ordered from the outermost to the innermost axis.
     */
    template<TensorElement T, size_t... Extents>
        requires(sizeof...(Extents) > 0 && ((Extents > 0) && ...))
    class Tensor {
    public:
        using ElementType = T;
        static constexpr size_t kRank = sizeof...(Extents);
        static constexpr size_t kSize = (Extents * ...);
        static constexpr std::array<size_t, kRank> kExtents{Extents...};

        template<TensorElement U>
        using Rebind = Tensor<U, Extents...>;

        /** @brief Value-initialize every tensor element. */
        constexpr Tensor() noexcept(std::is_nothrow_default_constructible_v<T>)
            requires std::default_initializable<T>
            : values_{} {}

        /** @brief Construct from the exact contiguous row-major element array. */
        constexpr explicit Tensor(std::array<T, kSize> values) noexcept(std::is_nothrow_move_constructible_v<T>)
            : values_(std::move(values)) {}

        /** @brief Construct from exactly @c kSize row-major values. */
        template<typename... U>
            requires(sizeof...(U) == kSize && (std::constructible_from<T, U&&> && ...))
        constexpr explicit(!(std::convertible_to<U&&, T> && ...))
            Tensor(U&&... values) noexcept((std::is_nothrow_constructible_v<T, U&&> && ...))
            : values_{T(std::forward<U>(values))...} {}

        /** @brief Access a row-major element by its linear index. @pre @p index is less than @c kSize. */
        template<typename Self>
        [[nodiscard]] constexpr decltype(auto) operator[](this Self&& self, size_t index) noexcept {
            return std::forward_like<Self>(self.values_[index]);
        }

        /** @brief Access an element by one index per axis. @pre Every index is within its corresponding extent. */
        template<typename Self, std::convertible_to<size_t>... Indices>
            requires(sizeof...(Indices) == kRank)
        [[nodiscard]] constexpr decltype(auto) operator()(this Self&& self, Indices... indices) noexcept {
            return std::forward_like<Self>(self.values_[LinearIndex(static_cast<size_t>(indices)...)]);
        }

        /** @brief Return the contiguous row-major array while preserving cv-ref qualification. */
        template<typename Self>
        [[nodiscard]] constexpr decltype(auto) Values(this Self&& self) noexcept {
            return std::forward_like<Self>(self.values_);
        }

        /** @brief Return the first element address from an lvalue tensor. */
        template<typename Self>
            requires std::is_lvalue_reference_v<Self&&>
        [[nodiscard]] constexpr auto Data(this Self&& self) noexcept {
            return self.values_.data();
        }

        /** @brief Return the compile-time extent of @p Axis. */
        template<size_t Axis>
            requires(Axis < kRank)
        [[nodiscard]] static consteval size_t Extent() noexcept {
            return kExtents[Axis];
        }

    private:
        template<typename... Indices>
        [[nodiscard]] static constexpr size_t LinearIndex(Indices... indices) noexcept {
            const std::array<size_t, kRank> coordinates{indices...};
            size_t linear = coordinates[0];
            for (size_t axis = 1; axis < kRank; ++axis) {
                linear = linear * kExtents[axis] + coordinates[axis];
            }
            return linear;
        }

        std::array<T, kSize> values_;
    };

    template<typename T, typename... U>
    Tensor(T, U...) -> Tensor<std::common_type_t<T, U...>, 1 + sizeof...(U)>;

    namespace Detail {

        template<size_t Axis, TensorElement R, size_t... Extents, size_t... Indices>
        [[nodiscard]] consteval auto RemoveTensorAxisType(std::index_sequence<Indices...>)
            -> std::type_identity<Tensor<R, Extents...[Indices < Axis ? Indices : Indices + 1]...>>;

        template<size_t Axis, TensorElement R, size_t... Extents>
        using RemoveTensorAxis = typename decltype(RemoveTensorAxisType<Axis, R, Extents...>(
            std::make_index_sequence<sizeof...(Extents) - 1>{}))::type;

        template<size_t Axis, size_t NewExtent, TensorElement R, size_t... Extents, size_t... Indices>
        [[nodiscard]] consteval auto ReplaceTensorAxisType(std::index_sequence<Indices...>)
            -> std::type_identity<Tensor<R, (Indices == Axis ? NewExtent : Extents...[Indices])...>>;

        template<size_t Axis, size_t NewExtent, TensorElement R, size_t... Extents>
        using ReplaceTensorAxis = typename decltype(ReplaceTensorAxisType<Axis, NewExtent, R, Extents...>(
            std::make_index_sequence<sizeof...(Extents)>{}))::type;

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
            auto values = Detail::GenerateArray<R, outputSize>([&](auto outputIndex) noexcept(
                std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_invocable_v<Function&, const Fiber&>) {
                constexpr size_t outer = decltype(outputIndex)::value / innerStride;
                constexpr size_t inner = decltype(outputIndex)::value % innerStride;
                const auto fiber = Detail::GenerateArray<T, axisExtent>([&](auto axisIndex) noexcept(
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

        const auto transformed = Detail::GenerateArray<FiberResult, fiberCount>([&](auto fiberIndex) {
            constexpr size_t outer = decltype(fiberIndex)::value / innerStride;
            constexpr size_t inner = decltype(fiberIndex)::value % innerStride;
            const auto fiber = Detail::GenerateArray<T, axisExtent>([&](auto axisIndex) {
                constexpr size_t inputIndex = outer * axisExtent * innerStride +
                                              decltype(axisIndex)::value * innerStride + inner;
                return input[inputIndex];
            });
            return std::invoke(function, fiber);
        });
        constexpr size_t outputSize = fiberCount * NewExtent;
        auto values = Detail::GenerateArray<R, outputSize>([&](auto outputIndex) {
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
        return Tensor<R, Extents...>{Detail::GenerateArray<R, Tensor<T, Extents...>::kSize>(
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
        return Tensor<R, Extents...>{Detail::GenerateArray<R, Tensor<T, Extents...>::kSize>(
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

    /** @brief Contract two equally shaped tensors to one scalar with fused multiply-add when available. */
    template<TensorElement T, TensorElement U, size_t... Extents>
        requires requires(T left, U right) {
            Sora::Math::Mul(left, right);
            Sora::Math::Fma(left, right, Sora::Math::Mul(left, right));
        }
    [[nodiscard]] constexpr auto InnerProduct(const Tensor<T, Extents...>& left,
                                              const Tensor<U, Extents...>& right) {
        auto result = Sora::Math::Mul(left[0], right[0]);
        template for (constexpr size_t index :
                      std::define_static_array(std::views::iota(size_t{1}, Tensor<T, Extents...>::kSize))) {
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
    template<TensorElement T, size_t... LeftExtents, TensorElement U, size_t... RightExtents, typename Product>
        requires std::invocable<Product&, const T&, const U&> &&
                 TensorElement<std::remove_cvref_t<std::invoke_result_t<Product&, const T&, const U&>>>
    [[nodiscard]] constexpr auto TensorProduct(const Tensor<T, LeftExtents...>& left,
                                               const Tensor<U, RightExtents...>& right, Product&& product) noexcept(
        std::is_nothrow_invocable_v<Product&, const T&, const U&>) {
        using R = std::remove_cvref_t<std::invoke_result_t<Product&, const T&, const U&>>;
        using Left = Tensor<T, LeftExtents...>;
        using Right = Tensor<U, RightExtents...>;
        return Tensor<R, LeftExtents..., RightExtents...>{Detail::GenerateArray<R, Left::kSize * Right::kSize>(
            [&](auto index) noexcept(std::is_nothrow_invocable_v<Product&, const T&, const U&>) {
                constexpr size_t leftIndex = decltype(index)::value / Right::kSize;
                constexpr size_t rightIndex = decltype(index)::value % Right::kSize;
                return std::invoke(product, left[leftIndex], right[rightIndex]);
            })};
    }

    /** @brief Form the multiplicative outer tensor product. */
    template<TensorElement T, size_t... LeftExtents, TensorElement U, size_t... RightExtents>
        requires requires(T left, U right) { Sora::Math::Mul(left, right); }
    [[nodiscard]] constexpr auto TensorProduct(const Tensor<T, LeftExtents...>& left,
                                               const Tensor<U, RightExtents...>& right) {
        return TensorProduct(left, right, [](const T& a, const U& b) noexcept(noexcept(Sora::Math::Mul(a, b))) {
            return Sora::Math::Mul(a, b);
        });
    }

} // namespace Sora::Math

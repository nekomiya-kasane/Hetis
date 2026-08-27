/**
 * @file Bezier.h
 * @brief Allocation-free polynomial Bezier curves, tensor-product surfaces, derivatives, and SIMD batching.
 * @ingroup Geom
 *
 * @details This header closes the polynomial Bernstein family under exact differentiation, subdivision, degree
 * elevation, tensor products, mixed partials, and isoparametric restriction. Rational projection and its weight
 * singularities belong to a separate rational spline layer.
 */
#pragma once

#include <Sora/Math/Autodiff.h>
#include <Sora/Math/Point.h>
#include <Sora/Math/Tensor.h>
#include <Sora/Math/ValueBatch.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <meta>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Sora::Math {

    template<TensorElement Control, size_t... Degrees>
        requires(sizeof...(Degrees) > 0)
    class BezierPatch;

    namespace Detail {

        template<typename Left, typename Right>
        inline constexpr bool kNothrowBezierDifference = [] {
            if constexpr (requires(const Left& left, const Right& right) { Sora::Math::Sub(left, right); }) {
                return noexcept(Sora::Math::Sub(std::declval<const Left&>(), std::declval<const Right&>()));
            } else {
                return noexcept(std::declval<const Left&>() - std::declval<const Right&>());
            }
        }();

        template<typename Value, typename Scalar>
        inline constexpr bool kNothrowBezierScale = [] {
            if constexpr (requires(const Value& value, const Scalar& scale) { Sora::Math::Mul(value, scale); }) {
                return noexcept(Sora::Math::Mul(std::declval<const Value&>(), std::declval<const Scalar&>()));
            } else {
                return noexcept(std::declval<const Value&>() * std::declval<const Scalar&>());
            }
        }();

        template<typename Left, typename Right>
        inline constexpr bool kNothrowBezierSum = [] {
            if constexpr (requires(const Left& left, const Right& right) { Sora::Math::Add(left, right); }) {
                return noexcept(Sora::Math::Add(std::declval<const Left&>(), std::declval<const Right&>()));
            } else {
                return noexcept(std::declval<const Left&>() + std::declval<const Right&>());
            }
        }();

        template<typename Left, typename Right>
        [[nodiscard]] constexpr auto BezierDifference(const Left& left, const Right& right) noexcept(
            kNothrowBezierDifference<Left, Right>) {
            if constexpr (requires { Sora::Math::Sub(left, right); }) {
                return Sora::Math::Sub(left, right);
            } else {
                return left - right;
            }
        }

        template<typename Value, typename Scalar>
        [[nodiscard]] constexpr auto BezierScale(const Value& value, const Scalar& scale) noexcept(
            kNothrowBezierScale<Value, Scalar>) {
            if constexpr (requires { Sora::Math::Mul(value, scale); }) {
                return Sora::Math::Mul(value, scale);
            } else {
                return value * scale;
            }
        }

        template<typename Left, typename Right>
        [[nodiscard]] constexpr auto BezierSum(const Left& left, const Right& right) noexcept(
            kNothrowBezierSum<Left, Right>) {
            if constexpr (requires { Sora::Math::Add(left, right); }) {
                return Sora::Math::Add(left, right);
            } else {
                return left + right;
            }
        }

        template<typename Value, typename Scalar, typename Addend>
        inline constexpr bool kNothrowBezierMultiplyAdd = [] {
            if constexpr (requires(const Value& value, const Scalar& scale, const Addend& addend) {
                              Sora::Math::Fma(value, scale, addend);
                          }) {
                return noexcept(Sora::Math::Fma(std::declval<const Value&>(), std::declval<const Scalar&>(),
                                                std::declval<const Addend&>()));
            } else {
                using Product = std::remove_cvref_t<decltype(BezierScale(
                    std::declval<const Value&>(), std::declval<const Scalar&>()))>;
                return kNothrowBezierScale<Value, Scalar> && kNothrowBezierSum<Product, Addend>;
            }
        }();

        template<typename Value, typename Scalar, typename Addend>
        [[nodiscard]] constexpr auto BezierMultiplyAdd(const Value& value, const Scalar& scale,
                                                       const Addend& addend) noexcept(
            kNothrowBezierMultiplyAdd<Value, Scalar, Addend>) {
            if constexpr (requires { Sora::Math::Fma(value, scale, addend); }) {
                return Sora::Math::Fma(value, scale, addend);
            } else {
                return BezierSum(BezierScale(value, scale), addend);
            }
        }

        template<ValueCarrier T, typename Scalar, ValueCarrier U, size_t N>
            requires requires(const T& value, const Scalar& scale, const U& addend) {
                Sora::Math::Fma(value, scale, addend);
            }
        [[nodiscard]] constexpr auto BezierMultiplyAdd(const Vector<T, N>& value, const Scalar& scale,
                                                       const Vector<U, N>& addend) noexcept(
            noexcept(Sora::Math::Fma(value[0], scale, addend[0]))) {
            using R = std::remove_cvref_t<decltype(Sora::Math::Fma(value[0], scale, addend[0]))>;
            return Vector<R, N>{GenerateStaticArray<R, N>(
                [&](auto index) noexcept(noexcept(Sora::Math::Fma(value[0], scale, addend[0]))) {
                    return Sora::Math::Fma(value[index], scale, addend[index]);
                })};
        }

        template<ValueCarrier T, typename Scalar, ValueCarrier U, size_t N>
            requires requires(const T& value, const Scalar& scale, const U& addend) {
                Sora::Math::Fma(value, scale, addend);
            }
        [[nodiscard]] constexpr auto BezierMultiplyAdd(const Vector<T, N>& value, const Scalar& scale,
                                                       const Point<U, N>& addend) noexcept(
            noexcept(Sora::Math::Fma(value[0], scale, addend[0]))) {
            using R = std::remove_cvref_t<decltype(Sora::Math::Fma(value[0], scale, addend[0]))>;
            return Point<R, N>{GenerateStaticArray<R, N>(
                [&](auto index) noexcept(noexcept(Sora::Math::Fma(value[0], scale, addend[0]))) {
                    return Sora::Math::Fma(value[index], scale, addend[index]);
                })};
        }

        template<typename Left, typename Right, typename Parameter>
        inline constexpr bool kNothrowBezierBlend = [] {
            if constexpr (requires(const Left& left, const Right& right, const Parameter& parameter) {
                              Sora::Math::Lerp(left, right, parameter);
                          }) {
                return noexcept(Sora::Math::Lerp(std::declval<const Left&>(), std::declval<const Right&>(),
                                                 std::declval<const Parameter&>()));
            } else {
                using Difference = std::remove_cvref_t<decltype(BezierDifference(
                    std::declval<const Right&>(), std::declval<const Left&>()))>;
                return kNothrowBezierDifference<Right, Left> &&
                       noexcept(BezierMultiplyAdd(std::declval<const Difference&>(),
                                                  std::declval<const Parameter&>(),
                                                  std::declval<const Left&>()));
            }
        }();

        template<typename Left, typename Right, typename Parameter>
        [[nodiscard]] constexpr auto BezierBlend(const Left& left, const Right& right,
                                                 const Parameter& parameter) noexcept(
            kNothrowBezierBlend<Left, Right, Parameter>) {
            if constexpr (requires { Sora::Math::Lerp(left, right, parameter); }) {
                return Sora::Math::Lerp(left, right, parameter);
            } else {
                return BezierMultiplyAdd(BezierDifference(right, left), parameter, left);
            }
        }

        template<typename Control, size_t Count, typename Parameter>
        inline constexpr bool kNothrowEvaluateBezierControls = [] {
            if constexpr (Count == 1) {
                return kNothrowBezierBlend<Control, Control, Parameter>;
            } else {
                using Result = std::remove_cvref_t<decltype(BezierBlend(
                    std::declval<const Control&>(), std::declval<const Control&>(),
                    std::declval<const Parameter&>()))>;
                return kNothrowBezierBlend<Control, Control, Parameter> &&
                       kNothrowBezierBlend<Result, Result, Parameter>;
            }
        }();

        template<typename Control, size_t Count, typename Parameter>
        [[nodiscard]] constexpr auto EvaluateBezierControls(const std::array<Control, Count>& controls,
                                                            const Parameter& parameter) noexcept(
            kNothrowEvaluateBezierControls<Control, Count, Parameter>) {
            static_assert(Count > 0);
            if constexpr (Count == 1) {
                return BezierBlend(controls[0], controls[0], parameter);
            } else {
                using Result = std::remove_cvref_t<decltype(BezierBlend(controls[0], controls[1], parameter))>;
                auto level = GenerateStaticArray<Result, Count - 1>(
                    [&](auto index) noexcept(kNothrowBezierBlend<Control, Control, Parameter>) {
                        return BezierBlend(controls[index], controls[index + 1], parameter);
                    });
                for (size_t remaining = Count - 1; remaining > 1; --remaining) {
                    for (size_t index = 0; index + 1 < remaining; ++index) {
                        level[index] = BezierBlend(level[index], level[index + 1], parameter);
                    }
                }
                return level[0];
            }
        }

        template<TensorElement Control, size_t... Counts>
        [[nodiscard]] consteval auto BezierPatchType(std::type_identity<Tensor<Control, Counts...>>)
            -> std::type_identity<BezierPatch<Control, (Counts - 1)...>>;

        template<typename TensorType>
        using BezierPatchFor = typename decltype(
            BezierPatchType(std::type_identity<std::remove_cvref_t<TensorType>>{}))::type;

        struct BezierAxisContraction {
            template<size_t Axis, typename Control, size_t Count, typename Parameter>
            [[nodiscard]] constexpr auto operator()(std::integral_constant<size_t, Axis>,
                                                    const std::array<Control, Count>& controls,
                                                    const Parameter& parameter) const noexcept(
                kNothrowEvaluateBezierControls<Control, Count, Parameter>) {
                return EvaluateBezierControls(controls, parameter);
            }
        };

        template<typename Control, size_t Count, typename Parameter>
        [[nodiscard]] constexpr auto SubdivideBezierControls(const std::array<Control, Count>& controls,
                                                              const Parameter& parameter) {
            static_assert(Count > 0);
            using Result = std::remove_cvref_t<decltype(BezierBlend(controls[0], controls[0], parameter))>;
            auto level = GenerateStaticArray<Result, Count>(
                [&](auto index) { return BezierBlend(controls[index], controls[index], parameter); });
            auto left = GenerateStaticArray<Result, Count>([&](auto) { return level[0]; });
            auto right = GenerateStaticArray<Result, Count>([&](auto) { return level[Count - 1]; });
            for (size_t remaining = Count - 1; remaining > 0; --remaining) {
                for (size_t index = 0; index < remaining; ++index) {
                    level[index] = BezierBlend(level[index], level[index + 1], parameter);
                }
                const size_t depth = Count - remaining;
                left[depth] = level[0];
                right[Count - 1 - depth] = level[remaining - 1];
            }
            return std::pair{std::move(left), std::move(right)};
        }

        template<typename Control>
        [[nodiscard]] consteval auto BezierScalarType() {
            using T = std::remove_cvref_t<Control>;
            if constexpr (requires { typename T::CarrierType; }) {
                return BezierScalarType<typename T::CarrierType>();
            } else if constexpr (DualValue<T> || ReverseValue<T>) {
                return BezierScalarType<typename T::PrimalType>();
            } else if constexpr (Simd::SimdVecType<T>) {
                return std::type_identity<typename T::ValueType>{};
            } else {
                return std::type_identity<T>{};
            }
        }

        template<typename Control>
        using BezierScalar = typename decltype(BezierScalarType<Control>())::type;

        template<TensorElement Value>
        struct CubicPowerData {
            using Direction = std::remove_cvref_t<decltype(BezierDifference(
                std::declval<const Value&>(), std::declval<const Value&>()))>;

            Value origin;
            Tensor<Direction, 3> coefficients;
        };

        template<TensorElement Value>
        [[nodiscard]] constexpr auto CubicPower(const std::array<Value, 4>& controls) {
            const auto first = BezierDifference(controls[1], controls[0]);
            const auto second = BezierDifference(controls[2], controls[1]);
            const auto third = BezierDifference(controls[3], controls[2]);
            using Data = CubicPowerData<Value>;
            return Data{
                controls[0],
                Tensor<typename Data::Direction, 3>{
                    BezierScale(first, 3), BezierScale(BezierDifference(second, first), 3),
                    BezierDifference(BezierDifference(third, second), BezierDifference(second, first))}};
        }

        template<typename Origin, typename Direction, typename Parameter>
        [[nodiscard, gnu::always_inline]] constexpr auto EvaluateCubicHorner(
            const Origin& origin, const Direction& linear, const Direction& quadratic, const Direction& cubic,
            const Parameter& parameter) noexcept(
            noexcept(BezierMultiplyAdd(
                BezierMultiplyAdd(BezierMultiplyAdd(cubic, parameter, quadratic), parameter, linear), parameter,
                origin))) {
            auto direction = BezierMultiplyAdd(cubic, parameter, quadratic);
            direction = BezierMultiplyAdd(direction, parameter, linear);
            return BezierMultiplyAdd(direction, parameter, origin);
        }

        [[nodiscard]] consteval size_t StaticPower(size_t base, size_t exponent) noexcept {
            size_t result = 1;
            for (size_t index = 0; index < exponent; ++index) {
                result *= base;
            }
            return result;
        }

        template<TensorElement Control, size_t Rank>
            requires(Rank > 0)
        struct CubicPatchPowerData {
            using Direction = typename CubicPowerData<Control>::Direction;
            static constexpr size_t kCoefficientCount = StaticPower(4, Rank);

            Control origin;
            std::array<Direction, kCoefficientCount - 1> coefficients;
        };

        template<size_t Axis, TensorElement Direction, size_t... Extents>
        [[nodiscard]] constexpr auto CubicTensorPower(const Tensor<Direction, Extents...>& coefficients) {
            if constexpr (Axis == sizeof...(Extents)) {
                return coefficients;
            } else {
                auto transformed = TransformTensorAxis<Axis, 4>(
                    coefficients, [](const std::array<Direction, 4>& fiber) {
                        const auto power = CubicPower(fiber);
                        return std::array{power.origin, power.coefficients[0], power.coefficients[1],
                                          power.coefficients[2]};
                    });
                return CubicTensorPower<Axis + 1>(transformed);
            }
        }

        template<TensorElement Control, size_t... Extents>
            requires(((Extents == 4) && ...))
        [[nodiscard]] constexpr auto CubicPatchPower(const Tensor<Control, Extents...>& controls) {
            constexpr size_t rank = sizeof...(Extents);
            using Data = CubicPatchPowerData<Control, rank>;
            using Direction = typename Data::Direction;
            auto directions = Transform(controls, [&](const Control& control) {
                return BezierDifference(control, controls[0]);
            });
            const auto coefficients = CubicTensorPower<0>(directions);
            return Data{controls[0], GenerateStaticArray<Direction, Data::kCoefficientCount - 1>(
                                         [&](auto index) { return coefficients[index + 1]; })};
        }

        template<size_t Axis, size_t Base, size_t Rank, typename Control, typename Direction, typename Parameters>
        [[nodiscard]] consteval auto CubicPowerBlockType() {
            using Parameter = std::tuple_element_t<Axis, Parameters>;
            constexpr size_t stride = StaticPower(4, Rank - Axis - 1);
            if constexpr (Axis + 1 == Rank) {
                if constexpr (Base == 0) {
                    return std::type_identity<std::remove_cvref_t<decltype(EvaluateCubicHorner(
                        std::declval<const Control&>(), std::declval<const Direction&>(),
                        std::declval<const Direction&>(), std::declval<const Direction&>(),
                        std::declval<const Parameter&>()))>>{};
                } else {
                    return std::type_identity<std::remove_cvref_t<decltype(EvaluateCubicHorner(
                        std::declval<const Direction&>(), std::declval<const Direction&>(),
                        std::declval<const Direction&>(), std::declval<const Direction&>(),
                        std::declval<const Parameter&>()))>>{};
                }
            } else {
                using Constant = typename decltype(
                    CubicPowerBlockType<Axis + 1, Base, Rank, Control, Direction, Parameters>())::type;
                using Coefficient = typename decltype(
                    CubicPowerBlockType<Axis + 1, Base + stride, Rank, Control, Direction, Parameters>())::type;
                return std::type_identity<std::remove_cvref_t<decltype(EvaluateCubicHorner(
                    std::declval<const Constant&>(), std::declval<const Coefficient&>(),
                    std::declval<const Coefficient&>(), std::declval<const Coefficient&>(),
                    std::declval<const Parameter&>()))>>{};
            }
        }

        template<size_t Axis, size_t Base, size_t Rank, typename Control, typename Direction, typename Parameters>
        [[nodiscard]] consteval bool NothrowEvaluateCubicPowerBlock() {
            using Parameter = std::tuple_element_t<Axis, Parameters>;
            constexpr size_t stride = StaticPower(4, Rank - Axis - 1);
            if constexpr (Axis + 1 == Rank) {
                if constexpr (Base == 0) {
                    return noexcept(EvaluateCubicHorner(
                        std::declval<const Control&>(), std::declval<const Direction&>(),
                        std::declval<const Direction&>(), std::declval<const Direction&>(),
                        std::declval<const Parameter&>()));
                } else {
                    return noexcept(EvaluateCubicHorner(
                        std::declval<const Direction&>(), std::declval<const Direction&>(),
                        std::declval<const Direction&>(), std::declval<const Direction&>(),
                        std::declval<const Parameter&>()));
                }
            } else {
                using Constant = typename decltype(
                    CubicPowerBlockType<Axis + 1, Base, Rank, Control, Direction, Parameters>())::type;
                using Coefficient = typename decltype(
                    CubicPowerBlockType<Axis + 1, Base + stride, Rank, Control, Direction, Parameters>())::type;
                return NothrowEvaluateCubicPowerBlock<Axis + 1, Base, Rank, Control, Direction, Parameters>() &&
                       NothrowEvaluateCubicPowerBlock<Axis + 1, Base + stride, Rank, Control, Direction,
                                                      Parameters>() &&
                       NothrowEvaluateCubicPowerBlock<Axis + 1, Base + 2 * stride, Rank, Control, Direction,
                                                      Parameters>() &&
                       NothrowEvaluateCubicPowerBlock<Axis + 1, Base + 3 * stride, Rank, Control, Direction,
                                                      Parameters>() &&
                       noexcept(EvaluateCubicHorner(
                           std::declval<const Constant&>(), std::declval<const Coefficient&>(),
                           std::declval<const Coefficient&>(), std::declval<const Coefficient&>(),
                           std::declval<const Parameter&>()));
            }
        }

        template<size_t Axis, size_t Base, size_t Rank, TensorElement Control, typename Parameters>
        [[nodiscard, gnu::always_inline]] constexpr auto EvaluateCubicPowerBlock(
            const CubicPatchPowerData<Control, Rank>& power, const Parameters& parameters) noexcept(
            NothrowEvaluateCubicPowerBlock<Axis, Base, Rank, Control,
                                            typename CubicPatchPowerData<Control, Rank>::Direction, Parameters>()) {
            using Direction = typename CubicPatchPowerData<Control, Rank>::Direction;
            constexpr size_t stride = StaticPower(4, Rank - Axis - 1);
            if constexpr (Axis + 1 == Rank) {
                if constexpr (Base == 0) {
                    return EvaluateCubicHorner(power.origin, power.coefficients[stride - 1],
                                               power.coefficients[2 * stride - 1],
                                               power.coefficients[3 * stride - 1], std::get<Axis>(parameters));
                } else {
                    return EvaluateCubicHorner(power.coefficients[Base - 1],
                                               power.coefficients[Base + stride - 1],
                                               power.coefficients[Base + 2 * stride - 1],
                                               power.coefficients[Base + 3 * stride - 1],
                                               std::get<Axis>(parameters));
                }
            } else {
                const auto constant = EvaluateCubicPowerBlock<Axis + 1, Base>(power, parameters);
                const auto linear = EvaluateCubicPowerBlock<Axis + 1, Base + stride>(power, parameters);
                const auto quadratic = EvaluateCubicPowerBlock<Axis + 1, Base + 2 * stride>(power, parameters);
                const auto cubic = EvaluateCubicPowerBlock<Axis + 1, Base + 3 * stride>(power, parameters);
                return EvaluateCubicHorner(constant, linear, quadratic, cubic, std::get<Axis>(parameters));
            }
        }

    } // namespace Detail

    /**
     * @brief Rank-generic tensor-product Bernstein patch over an affine or linear control value.
     * @details Rank one is a curve and rank two is a surface. Evaluation contracts the contiguous last axis first,
     * then proceeds toward axis zero. SIMD and automatic-differentiation carriers use the same expression graph.
     */
    template<TensorElement Control, size_t... Degrees>
        requires(sizeof...(Degrees) > 0)
    class BezierPatch {
    public:
        using ControlType = Control;
        using ControlTensor = Tensor<Control, (Degrees + 1)...>;
        static constexpr size_t kRank = sizeof...(Degrees);
        static constexpr size_t kControlCount = ((Degrees + 1) * ...);
        static constexpr std::array<size_t, kRank> kDegrees{Degrees...};

        /** @brief Construct a value-initialized control tensor when the control type supports it. */
        constexpr BezierPatch() noexcept(std::is_nothrow_default_constructible_v<Control>)
            requires std::default_initializable<Control>
        = default;

        /** @brief Construct from the exact control tensor. */
        constexpr explicit BezierPatch(ControlTensor controls) noexcept : controls_(std::move(controls)) {}

        /** @brief Construct from exactly @c kControlCount row-major control values. */
        template<typename... U>
            requires(sizeof...(U) == kControlCount && (std::constructible_from<Control, U&&> && ...))
        constexpr explicit BezierPatch(U&&... controls) noexcept(
            (std::is_nothrow_constructible_v<Control, U&&> && ...))
            : controls_(Control(std::forward<U>(controls))...) {}

        /** @brief Access one control value while preserving cv-ref qualification. */
        template<typename Self>
        [[nodiscard]] constexpr decltype(auto) operator[](this Self&& self, size_t index) noexcept {
            return std::forward_like<Self>(self.controls_[index]);
        }

        /** @brief Return the control tensor while preserving cv-ref qualification. */
        template<typename Self>
        [[nodiscard]] constexpr decltype(auto) Controls(this Self&& self) noexcept {
            return std::forward_like<Self>(self.controls_);
        }

        /**
         * @brief Evaluate by one statically shaped de Casteljau contraction per parameter axis.
         * @details Parameters are not clamped; values outside `[0,1]` extrapolate the polynomial patch.
         */
        template<typename... Parameters>
            requires(sizeof...(Parameters) == kRank)
        [[nodiscard, gnu::always_inline]] constexpr auto Evaluate(const Parameters&... parameters) const noexcept(
            noexcept(ContractTensorAxes(controls_, Detail::BezierAxisContraction{}, parameters...))) {
            return ContractTensorAxes(controls_, Detail::BezierAxisContraction{}, parameters...);
        }

        /**
         * @brief Return the exact @p Order partial derivative along @p Axis.
         * @details Orders above the axis degree produce its canonical degree-zero zero field.
         */
        template<size_t Axis, size_t Order = 1>
            requires(Axis < kRank)
        [[nodiscard]] constexpr auto PartialDerivative() const {
            if constexpr (Order == 0) {
                return *this;
            } else {
                constexpr size_t degree = Degrees...[Axis];
                constexpr size_t extent = degree + 1;
                if constexpr (degree == 0) {
                    using DerivativeControl = std::remove_cvref_t<decltype(Detail::BezierDifference(
                        std::declval<const Control&>(), std::declval<const Control&>()))>;
                    auto derivativeControls = TransformTensorAxis<Axis, 1>(
                        controls_, [](const std::array<Control, 1>& fiber) {
                            return std::array<DerivativeControl, 1>{
                                Detail::BezierDifference(fiber[0], fiber[0])};
                        });
                    using Result = Detail::BezierPatchFor<decltype(derivativeControls)>;
                    return Result{std::move(derivativeControls)};
                } else {
                    using Difference = decltype(Detail::BezierDifference(
                        std::declval<const Control&>(), std::declval<const Control&>()));
                    using DerivativeControl =
                        std::remove_cvref_t<decltype(Detail::BezierScale(std::declval<Difference>(), degree))>;
                    auto derivativeControls = TransformTensorAxis<Axis, degree>(
                        controls_, [&](const std::array<Control, extent>& fiber) {
                            return GenerateStaticArray<DerivativeControl, degree>([&](auto index) {
                                return Detail::BezierScale(
                                    Detail::BezierDifference(fiber[index + 1], fiber[index]), degree);
                            });
                        });
                    using Result = Detail::BezierPatchFor<decltype(derivativeControls)>;
                    return Result{std::move(derivativeControls)}.template PartialDerivative<Axis, Order - 1>();
                }
            }
        }

        /** @brief Return the exact @p Order derivative of a rank-one patch. */
        template<size_t Order = 1>
            requires(kRank == 1)
        [[nodiscard]] constexpr auto Derivative() const {
            return PartialDerivative<0, Order>();
        }

        /** @brief Evaluate one exact derivative of a rank-one patch without persistent runtime state. */
        template<size_t Order, typename Parameter>
            requires(kRank == 1)
        [[nodiscard, gnu::always_inline]] constexpr auto EvaluateDerivative(const Parameter& parameter) const {
            return Derivative<Order>().Evaluate(parameter);
        }

        /** @brief Return the exact @p Order derivative in the first direction of a rank-two patch. */
        template<size_t Order = 1>
            requires(kRank == 2)
        [[nodiscard]] constexpr auto DerivativeU() const {
            return PartialDerivative<0, Order>();
        }

        /** @brief Return the exact @p Order derivative in the second direction of a rank-two patch. */
        template<size_t Order = 1>
            requires(kRank == 2)
        [[nodiscard]] constexpr auto DerivativeV() const {
            return PartialDerivative<1, Order>();
        }

        /** @brief Evaluate one mixed partial, with one compile-time derivative order per parameter axis. */
        template<size_t... Orders, typename... Parameters>
            requires(sizeof...(Orders) == kRank && sizeof...(Parameters) == kRank)
        [[nodiscard, gnu::always_inline]] constexpr auto EvaluatePartial(const Parameters&... parameters) const {
            return DifferentiateAxes<0, Orders...>(*this).Evaluate(parameters...);
        }

        /** @brief Restrict @p Axis to one parameter, reducing the patch rank by one. */
        template<size_t Axis, typename Parameter>
            requires(Axis < kRank)
        [[nodiscard]] constexpr auto Restrict(const Parameter& parameter) const {
            constexpr size_t extent = Degrees...[Axis] + 1;
            auto restricted = ContractTensorAxis<Axis>(
                controls_, [&](const std::array<Control, extent>& fiber) {
                    return Detail::EvaluateBezierControls(fiber, parameter);
                });
            if constexpr (kRank == 1) {
                return restricted;
            } else {
                using Result = Detail::BezierPatchFor<decltype(restricted)>;
                return Result{std::move(restricted)};
            }
        }

        /** @brief Return the second-direction isoparametric curve at fixed @p u. */
        template<typename UParameter>
            requires(kRank == 2)
        [[nodiscard]] constexpr auto IsoU(const UParameter& u) const {
            return Restrict<0>(u);
        }

        /** @brief Return the first-direction isoparametric curve at fixed @p v. */
        template<typename VParameter>
            requires(kRank == 2)
        [[nodiscard]] constexpr auto IsoV(const VParameter& v) const {
            return Restrict<1>(v);
        }

        /** @brief Split along @p Axis into two patches whose concatenation equals this patch. */
        template<size_t Axis, typename Parameter>
            requires(Axis < kRank)
        [[nodiscard]] constexpr auto Subdivide(const Parameter& parameter) const {
            constexpr size_t extent = Degrees...[Axis] + 1;
            constexpr size_t innerStride = Detail::TensorInnerStride<Axis, (Degrees + 1)...>();
            constexpr size_t fiberCount = kControlCount / extent;
            using Fiber = std::array<Control, extent>;
            using Split = decltype(Detail::SubdivideBezierControls(std::declval<const Fiber&>(), parameter));
            using ResultControl = typename Split::first_type::value_type;
            const auto splits = GenerateStaticArray<Split, fiberCount>([&](auto fiberIndex) {
                constexpr size_t outer = decltype(fiberIndex)::value / innerStride;
                constexpr size_t inner = decltype(fiberIndex)::value % innerStride;
                const auto fiber = GenerateStaticArray<Control, extent>([&](auto axisIndex) {
                    constexpr size_t inputIndex = outer * extent * innerStride +
                                                  decltype(axisIndex)::value * innerStride + inner;
                    return controls_[inputIndex];
                });
                return Detail::SubdivideBezierControls(fiber, parameter);
            });
            const auto values = [&]<size_t Side>() {
                return GenerateStaticArray<ResultControl, kControlCount>([&](auto outputIndex) {
                    constexpr size_t outer = decltype(outputIndex)::value / (extent * innerStride);
                    constexpr size_t remainder = decltype(outputIndex)::value % (extent * innerStride);
                    constexpr size_t axisIndex = remainder / innerStride;
                    constexpr size_t inner = remainder % innerStride;
                    constexpr size_t fiberIndex = outer * innerStride + inner;
                    return std::get<Side>(splits[fiberIndex])[axisIndex];
                });
            };
            using Result = BezierPatch<ResultControl, Degrees...>;
            return std::pair{Result{typename Result::ControlTensor{values.template operator()<0>()}},
                             Result{typename Result::ControlTensor{values.template operator()<1>()}}};
        }

        /** @brief Split a rank-one patch at @p parameter. */
        template<typename Parameter>
            requires(kRank == 1)
        [[nodiscard]] constexpr auto Subdivide(const Parameter& parameter) const {
            return Subdivide<0>(parameter);
        }

        /** @brief Elevate one axis degree without changing the represented patch. */
        template<size_t Axis>
            requires(Axis < kRank)
        [[nodiscard]] constexpr auto ElevatedAxis() const {
            constexpr size_t degree = Degrees...[Axis];
            constexpr size_t extent = degree + 1;
            using Scalar = Detail::BezierScalar<Control>;
            const Scalar endpointWeight = Scalar{1} / static_cast<Scalar>(degree + 1);
            using ElevatedControl = std::remove_cvref_t<decltype(Detail::BezierBlend(
                std::declval<const Control&>(), std::declval<const Control&>(), endpointWeight))>;
            auto elevated = TransformTensorAxis<Axis, degree + 2>(
                controls_, [&](const std::array<Control, extent>& fiber) {
                return GenerateStaticArray<ElevatedControl, degree + 2>([&](auto index) {
                    if constexpr (decltype(index)::value == 0) {
                        return Detail::BezierBlend(fiber[0], fiber[0], endpointWeight);
                    } else if constexpr (decltype(index)::value == degree + 1) {
                        return Detail::BezierBlend(fiber[degree], fiber[degree], endpointWeight);
                    } else {
                        const Scalar weight = static_cast<Scalar>(index) / static_cast<Scalar>(degree + 1);
                        return Detail::BezierBlend(fiber[index], fiber[index - 1], weight);
                    }
                });
            });
            using Result = Detail::BezierPatchFor<decltype(elevated)>;
            return Result{std::move(elevated)};
        }

        /** @brief Elevate the degree of a rank-one patch once. */
        [[nodiscard]] constexpr auto Elevated() const
            requires(kRank == 1)
        {
            return ElevatedAxis<0>();
        }

    private:
        template<size_t Axis, size_t... Orders, typename Patch>
        [[nodiscard]] static constexpr auto DifferentiateAxes(Patch&& patch) {
            if constexpr (Axis == kRank) {
                return std::forward<Patch>(patch);
            } else {
                return DifferentiateAxes<Axis + 1, Orders...>(
                    std::forward<Patch>(patch).template PartialDerivative<Axis, Orders...[Axis]>());
            }
        }

        ControlTensor controls_;
    };

    template<TensorElement Control, size_t... Counts>
    BezierPatch(Tensor<Control, Counts...>) -> BezierPatch<Control, (Counts - 1)...>;

    /** @brief Polynomial Bezier curve with compile-time degree. */
    template<TensorElement Control, size_t Degree>
    using BezierCurve = BezierPatch<Control, Degree>;

    /** @brief Tensor-product Bezier surface with compile-time U/V degrees. */
    template<TensorElement Control, size_t UDegree, size_t VDegree>
    using BezierSurface = BezierPatch<Control, UDegree, VDegree>;

    /** @brief Two-dimensional polynomial Bezier curve. */
    template<ValueCarrier T, size_t Degree>
    using BezierCurve2D = BezierCurve<Point<T, 2>, Degree>;

    /** @brief Three-dimensional polynomial Bezier curve. */
    template<ValueCarrier T, size_t Degree>
    using BezierCurve3D = BezierCurve<Point<T, 3>, Degree>;

    /** @brief Three-dimensional tensor-product polynomial Bezier surface. */
    template<ValueCarrier T, size_t UDegree, size_t VDegree>
    using BezierSurface3D = BezierSurface<Point<T, 3>, UDegree, VDegree>;

    /** @brief Value and the first @p Order exact curve derivatives at one parameter. */
    template<TensorElement Value, TensorElement Derivative, size_t Order>
        requires(Order > 0)
    struct BezierCurveJet {
        Value value;
        Tensor<Derivative, Order> derivatives;
    };

    /** @brief Evaluate position and all derivatives through @p Order. */
    template<size_t Order, TensorElement Control, size_t Degree, typename Parameter>
        requires(Order > 0)
    [[nodiscard]] constexpr auto EvaluateJet(const BezierCurve<Control, Degree>& curve,
                                             const Parameter& parameter) {
        using Value = std::remove_cvref_t<decltype(curve.Evaluate(parameter))>;
        using Derivative = std::remove_cvref_t<decltype(curve.template EvaluateDerivative<1>(parameter))>;
        auto derivatives = GenerateStaticArray<Derivative, Order>([&](auto derivative) {
            return curve.template EvaluateDerivative<decltype(derivative)::value + 1>(parameter);
        });
        return BezierCurveJet<Value, Derivative, Order>{
            curve.Evaluate(parameter), Tensor<Derivative, Order>{std::move(derivatives)}};
    }

    /**
     * @brief Precomputed power-basis kernel for a tensor product of cubic Bernstein axes.
     * @details Storage is exactly one affine origin and `4^Rank-1` direction coefficients. Evaluation is a statically
     * nested Horner DAG; rank one and rank two therefore retain the minimal 1+3 and 1+15 coefficient layouts.
     */
    template<TensorElement Control, size_t Rank>
        requires(Rank > 0)
    class CubicBezierPatch {
    public:
        using Direction = typename Detail::CubicPatchPowerData<Control, Rank>::Direction;
        static constexpr size_t kRank = Rank;

        template<size_t... Degrees>
            requires(sizeof...(Degrees) == Rank && ((Degrees == 3) && ...))
        constexpr explicit CubicBezierPatch(const BezierPatch<Control, Degrees...>& patch)
            : power_(Detail::CubicPatchPower(patch.Controls())) {}

        template<typename... Parameters>
            requires(sizeof...(Parameters) == Rank)
        [[nodiscard, gnu::always_inline]] constexpr auto Evaluate(const Parameters&... parameters) const noexcept(
            Detail::NothrowEvaluateCubicPowerBlock<0, 0, Rank, Control, Direction,
                                                   std::tuple<const Parameters&...>>()) {
            const auto parameterTuple = std::forward_as_tuple(parameters...);
            return Detail::EvaluateCubicPowerBlock<0, 0>(power_, parameterTuple);
        }

    private:
        Detail::CubicPatchPowerData<Control, Rank> power_;
    };

    /** @brief Precomputed cubic curve kernel. */
    template<TensorElement Control>
    using CubicBezierPolynomial = CubicBezierPatch<Control, 1>;

    /** @brief Precomputed bicubic surface kernel. */
    template<TensorElement Control>
    using BicubicBezierPolynomial = CubicBezierPatch<Control, 2>;

    /** @brief Prepare an arbitrary-rank all-cubic patch for repeated nested Horner evaluation. */
    template<TensorElement Control, size_t... Degrees>
        requires(((Degrees == 3) && ...))
    [[nodiscard]] constexpr auto Prepare(const BezierPatch<Control, Degrees...>& patch) {
        return CubicBezierPatch<Control, sizeof...(Degrees)>{patch};
    }

    /** @brief Form the tensor product of two Bernstein patches and concatenate their parameter axes. */
    template<TensorElement LeftControl, size_t... LeftDegrees, TensorElement RightControl, size_t... RightDegrees,
             typename Product>
    [[nodiscard]] constexpr auto TensorProduct(const BezierPatch<LeftControl, LeftDegrees...>& left,
                                               const BezierPatch<RightControl, RightDegrees...>& right,
                                               Product&& product) {
        auto controls = Sora::Math::TensorProduct(left.Controls(), right.Controls(), std::forward<Product>(product));
        return BezierPatch{std::move(controls)};
    }

    /** @brief Form the multiplicative tensor product of two Bernstein patches. */
    template<TensorElement LeftControl, size_t... LeftDegrees, TensorElement RightControl, size_t... RightDegrees>
        requires requires(LeftControl left, RightControl right) { Sora::Math::Mul(left, right); }
    [[nodiscard]] constexpr auto TensorProduct(const BezierPatch<LeftControl, LeftDegrees...>& left,
                                               const BezierPatch<RightControl, RightDegrees...>& right) {
        return TensorProduct(left, right, [](const LeftControl& a, const RightControl& b) noexcept(
            noexcept(Sora::Math::Mul(a, b))) { return Sora::Math::Mul(a, b); });
    }

    /** @brief Evaluate one Bernstein patch at one parameter per axis. */
    template<TensorElement Control, size_t... Degrees, typename... Parameters>
        requires(sizeof...(Parameters) == sizeof...(Degrees))
    [[nodiscard, gnu::always_inline]] constexpr auto Evaluate(const BezierPatch<Control, Degrees...>& patch,
                                                              const Parameters&... parameters) {
        return patch.Evaluate(parameters...);
    }

    /** @brief Evaluate one prepared all-cubic patch at one parameter per axis. */
    template<TensorElement Control, size_t Rank, typename... Parameters>
        requires(sizeof...(Parameters) == Rank)
    [[nodiscard, gnu::always_inline]] constexpr auto Evaluate(const CubicBezierPatch<Control, Rank>& patch,
                                                              const Parameters&... parameters) {
        return patch.Evaluate(parameters...);
    }

    /** @brief Evaluate one Bernstein patch over one dynamic parameter batch per axis with automatic SIMD. */
    template<HomogeneousBatch Output, TensorElement Control, size_t... Degrees, HomogeneousBatch... Parameters>
        requires(sizeof...(Parameters) == sizeof...(Degrees) &&
                 ((std::remove_cvref_t<Parameters>::kDimension == 1) && ...))
    constexpr void Evaluate(Output output, const BezierPatch<Control, Degrees...>& patch,
                            Parameters... parameters) {
        TransformBatch(
            output,
            [&patch](const auto&... parameter) noexcept(noexcept(patch.Evaluate(parameter[0]...))) {
                return patch.Evaluate(parameter[0]...);
            },
            parameters...);
    }

    /** @brief Evaluate one prepared all-cubic patch over one dynamic parameter batch per axis. */
    template<HomogeneousBatch Output, TensorElement Control, size_t Rank, HomogeneousBatch... Parameters>
        requires(sizeof...(Parameters) == Rank &&
                 ((std::remove_cvref_t<Parameters>::kDimension == 1) && ...))
    constexpr void Evaluate(Output output, const CubicBezierPatch<Control, Rank>& patch,
                            Parameters... parameters) {
        TransformBatch<4>(
            output,
            [prepared = patch](const auto&... parameter) noexcept(noexcept(prepared.Evaluate(parameter[0]...))) {
                return prepared.Evaluate(parameter[0]...);
            },
            parameters...);
    }

    /**
     * @brief Evaluate one curve per lane/sample from structure-of-arrays control and parameter batches.
     * @details Passing a scalar @p parameter instead selects multi-instance, single-sample evaluation. Passing a
     * one-dimensional homogeneous batch selects multi-instance, multi-sample lane-wise evaluation.
     */
    template<HomogeneousBatch Output, HomogeneousBatch ControlBatch, size_t ControlCount, HomogeneousBatch Parameters>
        requires(ControlCount > 0 && std::remove_cvref_t<Parameters>::kDimension == 1)
    constexpr void Evaluate(Output output, const std::array<ControlBatch, ControlCount>& controls,
                            Parameters parameters) {
        std::apply(
            [&](const auto&... controlBatches) {
                TransformBatch(
                    output,
                    [](const auto& parameter, const auto&... controlValues) noexcept(noexcept(
                        BezierCurve{Tensor{controlValues...}}.Evaluate(parameter[0]))) {
                        auto curve = BezierCurve{Tensor{controlValues...}};
                        return curve.Evaluate(parameter[0]);
                    },
                    parameters, controlBatches...);
            },
            controls);
    }

    /** @brief Evaluate one curve per sample from control batches at a common scalar parameter. */
    template<HomogeneousBatch Output, HomogeneousBatch ControlBatch, size_t ControlCount, typename Parameter>
        requires(ControlCount > 0 && !HomogeneousBatch<std::remove_cvref_t<Parameter>>)
    constexpr void Evaluate(Output output, const std::array<ControlBatch, ControlCount>& controls,
                            const Parameter& parameter) {
        std::apply(
            [&](const auto&... controlBatches) {
                TransformBatch(
                    output,
                    [&parameter](const auto&... controlValues) noexcept(noexcept(
                        BezierCurve{Tensor{controlValues...}}.Evaluate(parameter))) {
                        auto curve = BezierCurve{Tensor{controlValues...}};
                        return curve.Evaluate(parameter);
                    },
                    controlBatches...);
            },
            controls);
    }

    /** @brief Evaluate one arbitrary-rank Bernstein patch per lane from control and parameter batches. */
    template<HomogeneousBatch Output, HomogeneousBatch ControlBatch, size_t... Counts,
             HomogeneousBatch... Parameters>
        requires(sizeof...(Parameters) == sizeof...(Counts) &&
                 ((std::remove_cvref_t<Parameters>::kDimension == 1) && ...))
    constexpr void Evaluate(Output output, const Tensor<ControlBatch, Counts...>& controls,
                            Parameters... parameters) {
        constexpr size_t rank = sizeof...(Counts);
        constexpr size_t controlCount = (Counts * ...);
        std::apply(
            [&](const auto&... controlBatches) {
                TransformBatch(
                    output,
                    [](const auto&... values) {
                        const auto arguments = std::forward_as_tuple(values...);
                        return [&]<size_t... ParameterIndices, size_t... ControlIndices>(
                                   std::index_sequence<ParameterIndices...>,
                                   std::index_sequence<ControlIndices...>) {
                            using Value = std::common_type_t<std::remove_cvref_t<decltype(
                                std::get<rank + ControlIndices>(arguments))>...>;
                            auto patch = BezierPatch{
                                Tensor<Value, Counts...>{std::get<rank + ControlIndices>(arguments)...}};
                            return patch.Evaluate(std::get<ParameterIndices>(arguments)[0]...);
                        }(std::make_index_sequence<rank>{}, std::make_index_sequence<controlCount>{});
                    },
                    parameters..., controlBatches...);
            },
            controls.Values());
    }

    /** @brief Evaluate one arbitrary-rank Bernstein patch per sample at common scalar parameters. */
    template<HomogeneousBatch Output, HomogeneousBatch ControlBatch, size_t... Counts, typename... Parameters>
        requires(sizeof...(Parameters) == sizeof...(Counts) &&
                 (!HomogeneousBatch<std::remove_cvref_t<Parameters>> && ...))
    constexpr void Evaluate(Output output, const Tensor<ControlBatch, Counts...>& controls,
                            const Parameters&... parameters) {
        const auto parameterTuple = std::forward_as_tuple(parameters...);
        std::apply(
            [&](const auto&... controlBatches) {
                TransformBatch(
                    output,
                    [&parameterTuple](const auto&... controlValues) {
                        using Value = std::common_type_t<std::remove_cvref_t<decltype(controlValues)>...>;
                        auto patch = BezierPatch{Tensor<Value, Counts...>{controlValues...}};
                        return std::apply(
                            [&patch](const auto&... parameter) { return patch.Evaluate(parameter...); },
                            parameterTuple);
                    },
                    controlBatches...);
            },
            controls.Values());
    }

} // namespace Sora::Math

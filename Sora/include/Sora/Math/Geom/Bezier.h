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
            return Vector<R, N>{Detail::GenerateArray<R, N>(
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
            return Point<R, N>{Detail::GenerateArray<R, N>(
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
                auto level = Detail::GenerateArray<Result, Count - 1>(
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

        template<typename Control, size_t UCount, size_t VCount, typename UParameter, typename VParameter>
        inline constexpr bool kNothrowEvaluateBezierSurface = [] {
            using VValue = std::remove_cvref_t<decltype(EvaluateBezierControls(
                std::declval<const std::array<Control, VCount>&>(), std::declval<const VParameter&>()))>;
            return kNothrowEvaluateBezierControls<Control, VCount, VParameter> &&
                   kNothrowEvaluateBezierControls<VValue, UCount, UParameter>;
        }();

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

        template<TensorElement Control>
        struct BicubicPowerData {
            using Direction = typename CubicPowerData<Control>::Direction;

            Control origin;
            Tensor<Direction, 3> vCoefficients;
            Tensor<Direction, 3, 4> uCoefficients;
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

        template<TensorElement Control>
        [[nodiscard]] constexpr auto BicubicPower(const Tensor<Control, 4, 4>& controls) {
            using Data = BicubicPowerData<Control>;
            using Direction = typename Data::Direction;
            using DirectionPower = CubicPowerData<Direction>;

            const auto vPower = GenerateArray<CubicPowerData<Control>, 4>([&](auto u) {
                const auto row = GenerateArray<Control, 4>([&](auto v) { return controls(u, v); });
                return CubicPower(row);
            });
            const auto origins = GenerateArray<Control, 4>([&](auto u) { return vPower[u].origin; });
            const auto uPower = CubicPower(origins);
            const auto crossPower = GenerateArray<DirectionPower, 3>([&](auto v) {
                const auto column = GenerateArray<Direction, 4>([&](auto u) {
                    return vPower[u].coefficients[v];
                });
                return CubicPower(column);
            });
            auto vCoefficients = GenerateArray<Direction, 3>([&](auto v) {
                return crossPower[v].origin;
            });
            auto uCoefficients = GenerateArray<Direction, 12>([&](auto index) {
                constexpr size_t uPowerIndex = decltype(index)::value / 4;
                constexpr size_t vPowerIndex = decltype(index)::value % 4;
                if constexpr (vPowerIndex == 0) {
                    return uPower.coefficients[uPowerIndex];
                } else {
                    return crossPower[vPowerIndex - 1].coefficients[uPowerIndex];
                }
            });
            return Data{uPower.origin, Tensor<Direction, 3>{std::move(vCoefficients)},
                        Tensor<Direction, 3, 4>{std::move(uCoefficients)}};
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

        template<typename Control, typename Direction, typename UParameter, typename VParameter>
        inline constexpr bool kNothrowEvaluateBicubicHorner = [] {
            using VValue = std::remove_cvref_t<decltype(EvaluateCubicHorner(
                std::declval<const Control&>(), std::declval<const Direction&>(),
                std::declval<const Direction&>(), std::declval<const Direction&>(),
                std::declval<const VParameter&>()))>;
            using VDirection = std::remove_cvref_t<decltype(EvaluateCubicHorner(
                std::declval<const Direction&>(), std::declval<const Direction&>(),
                std::declval<const Direction&>(), std::declval<const Direction&>(),
                std::declval<const VParameter&>()))>;
            return noexcept(EvaluateCubicHorner(
                       std::declval<const Control&>(), std::declval<const Direction&>(),
                       std::declval<const Direction&>(), std::declval<const Direction&>(),
                       std::declval<const VParameter&>())) &&
                   noexcept(EvaluateCubicHorner(
                       std::declval<const Direction&>(), std::declval<const Direction&>(),
                       std::declval<const Direction&>(), std::declval<const Direction&>(),
                       std::declval<const VParameter&>())) &&
                   noexcept(EvaluateCubicHorner(
                       std::declval<const VValue&>(), std::declval<const VDirection&>(),
                       std::declval<const VDirection&>(), std::declval<const VDirection&>(),
                       std::declval<const UParameter&>()));
        }();

    } // namespace Detail

    /**
     * @brief Polynomial Bezier curve of compile-time degree over an affine or linear control value.
     * @details A SIMD parameter evaluates several samples of one curve. SIMD control carriers evaluate several
     * curves, and combining both evaluates lane-wise curve/sample pairs without changing this type or algorithm.
     */
    template<TensorElement Control, size_t Degree>
    class BezierCurve {
    public:
        using ControlType = Control;
        using ControlTensor = Tensor<Control, Degree + 1>;
        static constexpr size_t kDegree = Degree;
        static constexpr size_t kControlCount = Degree + 1;

        /** @brief Construct a zero/value-initialized control polygon. */
        constexpr BezierCurve() noexcept(std::is_nothrow_default_constructible_v<Control>)
            requires std::default_initializable<Control>
        = default;

        /** @brief Construct from the exact control tensor. */
        constexpr explicit BezierCurve(ControlTensor controls) noexcept : controls_(std::move(controls)) {}

        /** @brief Construct from exactly @c Degree+1 control values. */
        template<typename... U>
            requires(sizeof...(U) == kControlCount && (std::constructible_from<Control, U&&> && ...))
        constexpr explicit BezierCurve(U&&... controls) noexcept((std::is_nothrow_constructible_v<Control, U&&> && ...))
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
         * @brief Evaluate by statically shaped de Casteljau reduction.
         * @details The parameter is not clamped; values outside `[0,1]` extrapolate the polynomial.
         * @complexity Quadratic in @c Degree with no allocation.
         */
        template<typename Parameter>
        [[nodiscard, gnu::always_inline]] constexpr auto Evaluate(const Parameter& parameter) const noexcept(
            Detail::kNothrowEvaluateBezierControls<Control, kControlCount, Parameter>) {
            return Detail::EvaluateBezierControls(controls_.Values(), parameter);
        }

        /** @brief Return the exact @p Order derivative curve. */
        template<size_t Order = 1>
            requires(Order <= Degree)
        [[nodiscard]] constexpr auto Derivative() const {
            if constexpr (Order == 0) {
                return *this;
            } else {
                using Difference = decltype(Detail::BezierDifference(controls_[1], controls_[0]));
                using DerivativeControl =
                    std::remove_cvref_t<decltype(Detail::BezierScale(std::declval<Difference>(), Degree))>;
                auto derivativeControls = Detail::GenerateArray<DerivativeControl, Degree>([&](auto index) {
                    return Detail::BezierScale(
                        Detail::BezierDifference(controls_[index + 1], controls_[index]), Degree);
                });
                BezierCurve<DerivativeControl, Degree - 1> derivative{
                    Tensor<DerivativeControl, Degree>{std::move(derivativeControls)}};
                return derivative.template Derivative<Order - 1>();
            }
        }

        /** @brief Evaluate one exact derivative without constructing runtime state. */
        template<size_t Order, typename Parameter>
            requires(Order <= Degree)
        [[nodiscard, gnu::always_inline]] constexpr auto EvaluateDerivative(const Parameter& parameter) const {
            if constexpr (Order == 0) {
                return Evaluate(parameter);
            } else {
                return Derivative<Order>().Evaluate(parameter);
            }
        }

        /** @brief Split at @p parameter into two curves whose concatenation equals this curve. */
        template<typename Parameter>
        [[nodiscard]] constexpr auto Subdivide(const Parameter& parameter) const {
            using ResultControl =
                std::remove_cvref_t<decltype(Detail::BezierBlend(controls_[0], controls_[0], parameter))>;
            auto level = Detail::GenerateArray<ResultControl, kControlCount>(
                [&](auto index) { return Detail::BezierBlend(controls_[index], controls_[index], parameter); });
            auto left = Detail::GenerateArray<ResultControl, kControlCount>([&](auto) { return level[0]; });
            auto right = Detail::GenerateArray<ResultControl, kControlCount>([&](auto) { return level[Degree]; });
            for (size_t remaining = Degree; remaining > 0; --remaining) {
                for (size_t index = 0; index < remaining; ++index) {
                    level[index] = Detail::BezierBlend(level[index], level[index + 1], parameter);
                }
                const size_t depth = Degree - remaining + 1;
                left[depth] = level[0];
                right[Degree - depth] = level[remaining - 1];
            }
            using Result = BezierCurve<ResultControl, Degree>;
            return std::pair{Result{Tensor<ResultControl, kControlCount>{std::move(left)}},
                             Result{Tensor<ResultControl, kControlCount>{std::move(right)}}};
        }

        /** @brief Elevate the degree once without changing the represented curve. */
        [[nodiscard]] constexpr auto Elevated() const {
            using Scalar = Detail::BezierScalar<Control>;
            const Scalar endpointWeight = Scalar{1} / static_cast<Scalar>(Degree + 1);
            using ElevatedControl = std::remove_cvref_t<decltype(Detail::BezierBlend(
                controls_[0], controls_[0], endpointWeight))>;
            auto elevated = Detail::GenerateArray<ElevatedControl, Degree + 2>([&](auto index) {
                if constexpr (decltype(index)::value == 0) {
                    return Detail::BezierBlend(controls_[0], controls_[0], endpointWeight);
                } else if constexpr (decltype(index)::value == Degree + 1) {
                    return Detail::BezierBlend(controls_[Degree], controls_[Degree], endpointWeight);
                } else {
                    const Scalar weight = static_cast<Scalar>(index) / static_cast<Scalar>(Degree + 1);
                    return Detail::BezierBlend(controls_[index], controls_[index - 1], weight);
                }
            });
            return BezierCurve<ElevatedControl, Degree + 1>{Tensor<ElevatedControl, Degree + 2>{std::move(elevated)}};
        }

    private:
        ControlTensor controls_;
    };

    template<TensorElement Control, size_t Count>
    BezierCurve(Tensor<Control, Count>) -> BezierCurve<Control, Count - 1>;

    /** @brief Value and the first @p Order exact curve derivatives at one parameter. */
    template<TensorElement Value, TensorElement Derivative, size_t Order>
        requires(Order > 0)
    struct BezierCurveJet {
        Value value;
        Tensor<Derivative, Order> derivatives;
    };

    /** @brief Evaluate position and all derivatives through @p Order. */
    template<size_t Order, TensorElement Control, size_t Degree, typename Parameter>
        requires(Order > 0 && Order <= Degree)
    [[nodiscard]] constexpr auto EvaluateJet(const BezierCurve<Control, Degree>& curve,
                                             const Parameter& parameter) {
        using Value = std::remove_cvref_t<decltype(curve.Evaluate(parameter))>;
        using Derivative = std::remove_cvref_t<decltype(curve.template EvaluateDerivative<1>(parameter))>;
        auto derivatives = Detail::GenerateArray<Derivative, Order>([&](auto derivative) {
            return curve.template EvaluateDerivative<decltype(derivative)::value + 1>(parameter);
        });
        return BezierCurveJet<Value, Derivative, Order>{
            curve.Evaluate(parameter), Tensor<Derivative, Order>{std::move(derivatives)}};
    }

    /**
     * @brief Precomputed cubic power-basis kernel for repeated evaluation of one curve.
     * @details Construction performs the control-point differences once. Evaluation uses a three-stage Horner chain;
     * SIMD and automatic-differentiation parameters flow through the same carrier operations.
     */
    template<TensorElement Control>
    class CubicBezierPolynomial {
    public:
        using Direction = std::remove_cvref_t<decltype(Detail::BezierDifference(
            std::declval<const Control&>(), std::declval<const Control&>()))>;

        constexpr explicit CubicBezierPolynomial(const BezierCurve<Control, 3>& curve)
            : power_(Detail::CubicPower(curve.Controls().Values())) {}

        template<typename Parameter>
        [[nodiscard, gnu::always_inline]] constexpr auto Evaluate(const Parameter& parameter) const noexcept(
            noexcept(Detail::EvaluateCubicHorner(power_.origin, power_.coefficients[0], power_.coefficients[1],
                                                  power_.coefficients[2], parameter))) {
            return Detail::EvaluateCubicHorner(power_.origin, power_.coefficients[0], power_.coefficients[1],
                                               power_.coefficients[2], parameter);
        }

    private:
        Detail::CubicPowerData<Control> power_;
    };

    /** @brief Prepare a cubic curve for repeated Horner evaluation. */
    template<TensorElement Control>
    [[nodiscard]] constexpr auto Prepare(const BezierCurve<Control, 3>& curve) {
        return CubicBezierPolynomial<Control>{curve};
    }

    /**
     * @brief Tensor-product polynomial Bezier surface with compile-time degrees in both parameter directions.
     * @details The control tensor has shape `(UDegree+1, VDegree+1)` and row-major storage. Evaluation contracts the
     * contiguous V direction first and then U.
     */
    template<TensorElement Control, size_t UDegree, size_t VDegree>
    class BezierSurface {
    public:
        using ControlType = Control;
        using ControlTensor = Tensor<Control, UDegree + 1, VDegree + 1>;
        static constexpr size_t kUDegree = UDegree;
        static constexpr size_t kVDegree = VDegree;

        /** @brief Construct a zero/value-initialized control net when the control type supports it. */
        constexpr BezierSurface() noexcept(std::is_nothrow_default_constructible_v<Control>)
            requires std::default_initializable<Control>
        = default;

        /** @brief Construct from the exact row-major control tensor. */
        constexpr explicit BezierSurface(ControlTensor controls) noexcept : controls_(std::move(controls)) {}

        /** @brief Return the control tensor while preserving cv-ref qualification. */
        template<typename Self>
        [[nodiscard]] constexpr decltype(auto) Controls(this Self&& self) noexcept {
            return std::forward_like<Self>(self.controls_);
        }

        /** @brief Evaluate by statically contracting the contiguous V direction and then the U direction. */
        template<typename UParameter, typename VParameter>
        [[nodiscard, gnu::always_inline]] constexpr auto Evaluate(const UParameter& u,
                                                                  const VParameter& v) const noexcept(
            Detail::kNothrowEvaluateBezierSurface<Control, UDegree + 1, VDegree + 1, UParameter, VParameter>) {
            using VValue = std::remove_cvref_t<decltype(Detail::EvaluateBezierControls(
                std::declval<std::array<Control, VDegree + 1>>(), v))>;
            const auto vContracted = Detail::GenerateArray<VValue, UDegree + 1>(
                [&](auto uIndex) noexcept(
                    Detail::kNothrowEvaluateBezierControls<Control, VDegree + 1, VParameter>) {
                const auto vControls = Detail::GenerateArray<Control, VDegree + 1>(
                    [&](auto vIndex) { return controls_(uIndex, vIndex); });
                return Detail::EvaluateBezierControls(vControls, v);
            });
            return Detail::EvaluateBezierControls(vContracted, u);
        }

        /** @brief Return the exact @p Order derivative surface in the U direction. */
        template<size_t Order = 1>
            requires(Order <= UDegree)
        [[nodiscard]] constexpr auto DerivativeU() const {
            if constexpr (Order == 0) {
                return *this;
            } else {
                using Difference = decltype(Detail::BezierDifference(controls_(1, 0), controls_(0, 0)));
                using DerivativeControl =
                    std::remove_cvref_t<decltype(Detail::BezierScale(std::declval<Difference>(), UDegree))>;
                auto derivative = Detail::GenerateArray<DerivativeControl, UDegree * (VDegree + 1)>(
                    [&](auto index) {
                        constexpr size_t uIndex = decltype(index)::value / (VDegree + 1);
                        constexpr size_t vIndex = decltype(index)::value % (VDegree + 1);
                        return Detail::BezierScale(
                            Detail::BezierDifference(controls_(uIndex + 1, vIndex), controls_(uIndex, vIndex)),
                            UDegree);
                    });
                BezierSurface<DerivativeControl, UDegree - 1, VDegree> result{
                    Tensor<DerivativeControl, UDegree, VDegree + 1>{std::move(derivative)}};
                return result.template DerivativeU<Order - 1>();
            }
        }

        /** @brief Return the exact @p Order derivative surface in the V direction. */
        template<size_t Order = 1>
            requires(Order <= VDegree)
        [[nodiscard]] constexpr auto DerivativeV() const {
            if constexpr (Order == 0) {
                return *this;
            } else {
                using Difference = decltype(Detail::BezierDifference(controls_(0, 1), controls_(0, 0)));
                using DerivativeControl =
                    std::remove_cvref_t<decltype(Detail::BezierScale(std::declval<Difference>(), VDegree))>;
                auto derivative = Detail::GenerateArray<DerivativeControl, (UDegree + 1) * VDegree>(
                    [&](auto index) {
                        constexpr size_t uIndex = decltype(index)::value / VDegree;
                        constexpr size_t vIndex = decltype(index)::value % VDegree;
                        return Detail::BezierScale(
                            Detail::BezierDifference(controls_(uIndex, vIndex + 1), controls_(uIndex, vIndex)),
                            VDegree);
                    });
                BezierSurface<DerivativeControl, UDegree, VDegree - 1> result{
                    Tensor<DerivativeControl, UDegree + 1, VDegree>{std::move(derivative)}};
                return result.template DerivativeV<Order - 1>();
            }
        }

        /** @brief Evaluate the exact mixed partial derivative `d^(DU+DV)/(du^DU dv^DV)`. */
        template<size_t DU, size_t DV, typename UParameter, typename VParameter>
            requires(DU <= UDegree && DV <= VDegree)
        [[nodiscard]] constexpr auto EvaluatePartial(const UParameter& u, const VParameter& v) const {
            if constexpr (DU == 0 && DV == 0) {
                return Evaluate(u, v);
            } else {
                return DerivativeU<DU>().template DerivativeV<DV>().Evaluate(u, v);
            }
        }

        /** @brief Return the V-direction isoparametric curve at @p u. */
        template<typename UParameter>
        [[nodiscard]] constexpr auto IsoU(const UParameter& u) const {
            using Value = std::remove_cvref_t<decltype(Detail::EvaluateBezierControls(
                std::declval<const std::array<Control, UDegree + 1>&>(), u))>;
            auto controls = Detail::GenerateArray<Value, VDegree + 1>([&](auto vIndex) {
                const auto uControls = Detail::GenerateArray<Control, UDegree + 1>(
                    [&](auto uIndex) { return controls_(uIndex, vIndex); });
                return Detail::EvaluateBezierControls(uControls, u);
            });
            return BezierCurve<Value, VDegree>{Tensor<Value, VDegree + 1>{std::move(controls)}};
        }

        /** @brief Return the U-direction isoparametric curve at @p v. */
        template<typename VParameter>
        [[nodiscard]] constexpr auto IsoV(const VParameter& v) const {
            using Value = std::remove_cvref_t<decltype(Detail::EvaluateBezierControls(
                std::declval<const std::array<Control, VDegree + 1>&>(), v))>;
            auto controls = Detail::GenerateArray<Value, UDegree + 1>([&](auto uIndex) {
                const auto vControls = Detail::GenerateArray<Control, VDegree + 1>(
                    [&](auto vIndex) { return controls_(uIndex, vIndex); });
                return Detail::EvaluateBezierControls(vControls, v);
            });
            return BezierCurve<Value, UDegree>{Tensor<Value, UDegree + 1>{std::move(controls)}};
        }

    private:
        ControlTensor controls_;
    };

    template<TensorElement Control, size_t UCount, size_t VCount>
    BezierSurface(Tensor<Control, UCount, VCount>) -> BezierSurface<Control, UCount - 1, VCount - 1>;

    /**
     * @brief Precomputed bicubic power-basis kernel for repeated surface evaluation.
     * @details The constructor performs both Bernstein-to-power transforms once. Evaluation uses nested Horner chains
     * over fifteen direction coefficients and one affine origin, with no temporary control polygons.
     */
    template<TensorElement Control>
    class BicubicBezierPolynomial {
    public:
        using Direction = std::remove_cvref_t<decltype(Detail::BezierDifference(
            std::declval<const Control&>(), std::declval<const Control&>()))>;

        constexpr explicit BicubicBezierPolynomial(const BezierSurface<Control, 3, 3>& surface)
            : power_(Detail::BicubicPower(surface.Controls())) {}

        template<typename UParameter, typename VParameter>
        [[nodiscard, gnu::always_inline]] constexpr auto Evaluate(const UParameter& u,
                                                                   const VParameter& v) const noexcept(
            Detail::kNothrowEvaluateBicubicHorner<Control, Direction, UParameter, VParameter>) {
            auto result = Detail::EvaluateCubicHorner(power_.origin, power_.vCoefficients[0],
                                                      power_.vCoefficients[1], power_.vCoefficients[2], v);

            using EvaluatedDirection = std::remove_cvref_t<decltype(
                Detail::EvaluateCubicHorner(power_.uCoefficients(0, 0), power_.uCoefficients(0, 1),
                                            power_.uCoefficients(0, 2), power_.uCoefficients(0, 3), v))>;
            const auto uCoefficients = Detail::GenerateArray<EvaluatedDirection, 3>(
                [&](auto index) noexcept(noexcept(Detail::EvaluateCubicHorner(
                    power_.uCoefficients(0, 0), power_.uCoefficients(0, 1), power_.uCoefficients(0, 2),
                    power_.uCoefficients(0, 3), v))) {
                constexpr size_t uPower = decltype(index)::value;
                return Detail::EvaluateCubicHorner(
                    power_.uCoefficients(uPower, 0), power_.uCoefficients(uPower, 1),
                    power_.uCoefficients(uPower, 2), power_.uCoefficients(uPower, 3), v);
            });
            return Detail::EvaluateCubicHorner(result, uCoefficients[0], uCoefficients[1], uCoefficients[2], u);
        }

    private:
        Detail::BicubicPowerData<Control> power_;
    };

    /** @brief Prepare a bicubic surface for repeated nested Horner evaluation. */
    template<TensorElement Control>
    [[nodiscard]] constexpr auto Prepare(const BezierSurface<Control, 3, 3>& surface) {
        return BicubicBezierPolynomial<Control>{surface};
    }

    /** @brief Form a tensor-product Bezier surface from two coefficient curves and a control-value product. */
    template<TensorElement UControl, size_t UDegree, TensorElement VControl, size_t VDegree, typename Product>
    [[nodiscard]] constexpr auto TensorProduct(const BezierCurve<UControl, UDegree>& uCurve,
                                               const BezierCurve<VControl, VDegree>& vCurve,
                                               Product&& product) {
        auto controls = Sora::Math::TensorProduct(uCurve.Controls(), vCurve.Controls(), std::forward<Product>(product));
        return BezierSurface{std::move(controls)};
    }

    /** @brief Evaluate one curve at one scalar, automatic-differentiation, or SIMD parameter. */
    template<TensorElement Control, size_t Degree, typename Parameter>
    [[nodiscard, gnu::always_inline]] constexpr auto Evaluate(const BezierCurve<Control, Degree>& curve,
                                                              const Parameter& parameter) {
        return curve.Evaluate(parameter);
    }

    /** @brief Evaluate one prepared cubic at one scalar, automatic-differentiation, or SIMD parameter. */
    template<TensorElement Control, typename Parameter>
    [[nodiscard, gnu::always_inline]] constexpr auto Evaluate(const CubicBezierPolynomial<Control>& curve,
                                                              const Parameter& parameter) {
        return curve.Evaluate(parameter);
    }

    /** @brief Evaluate one surface at scalar, automatic-differentiation, or SIMD parameters. */
    template<TensorElement Control, size_t UDegree, size_t VDegree, typename UParameter, typename VParameter>
    [[nodiscard, gnu::always_inline]] constexpr auto Evaluate(const BezierSurface<Control, UDegree, VDegree>& surface,
                                                              const UParameter& u, const VParameter& v) {
        return surface.Evaluate(u, v);
    }

    /** @brief Evaluate one prepared bicubic surface at scalar, automatic-differentiation, or SIMD parameters. */
    template<TensorElement Control, typename UParameter, typename VParameter>
    [[nodiscard, gnu::always_inline]] constexpr auto Evaluate(const BicubicBezierPolynomial<Control>& surface,
                                                              const UParameter& u, const VParameter& v) {
        return surface.Evaluate(u, v);
    }

    /** @brief Evaluate one curve over a dynamic parameter batch with automatic SIMD carrier selection. */
    template<HomogeneousBatch Output, TensorElement Control, size_t Degree, HomogeneousBatch Parameters>
        requires(std::remove_cvref_t<Parameters>::kDimension == 1)
    constexpr void Evaluate(Output output, const BezierCurve<Control, Degree>& curve, Parameters parameters) {
        TransformBatch(
            output,
            [&curve](const auto& parameter) noexcept(noexcept(curve.Evaluate(parameter[0]))) {
                return curve.Evaluate(parameter[0]);
            },
            parameters);
    }

    /** @brief Evaluate one prepared cubic over a dynamic parameter batch with automatic SIMD carrier selection. */
    template<HomogeneousBatch Output, TensorElement Control, HomogeneousBatch Parameters>
        requires(std::remove_cvref_t<Parameters>::kDimension == 1)
    constexpr void Evaluate(Output output, const CubicBezierPolynomial<Control>& curve, Parameters parameters) {
        TransformBatch(
            output,
            [&curve](const auto& parameter) noexcept(noexcept(curve.Evaluate(parameter[0]))) {
                return curve.Evaluate(parameter[0]);
            },
            parameters);
    }

    /** @brief Evaluate one surface over dynamic U/V parameter batches with automatic SIMD carrier selection. */
    template<HomogeneousBatch Output, TensorElement Control, size_t UDegree, size_t VDegree,
             HomogeneousBatch UParameters, HomogeneousBatch VParameters>
        requires(std::remove_cvref_t<UParameters>::kDimension == 1 &&
                 std::remove_cvref_t<VParameters>::kDimension == 1)
    constexpr void Evaluate(Output output, const BezierSurface<Control, UDegree, VDegree>& surface,
                            UParameters uParameters, VParameters vParameters) {
        TransformBatch(
            output,
            [&surface](const auto& u, const auto& v) noexcept(noexcept(surface.Evaluate(u[0], v[0]))) {
                return surface.Evaluate(u[0], v[0]);
            },
            uParameters, vParameters);
    }

    /** @brief Evaluate one prepared bicubic surface over dynamic U/V parameter batches with automatic SIMD. */
    template<HomogeneousBatch Output, TensorElement Control, HomogeneousBatch UParameters,
             HomogeneousBatch VParameters>
        requires(std::remove_cvref_t<UParameters>::kDimension == 1 &&
                 std::remove_cvref_t<VParameters>::kDimension == 1)
    constexpr void Evaluate(Output output, const BicubicBezierPolynomial<Control>& surface, UParameters uParameters,
                            VParameters vParameters) {
        TransformBatch(
            output,
            [&surface](const auto& u, const auto& v) noexcept(noexcept(surface.Evaluate(u[0], v[0]))) {
                return surface.Evaluate(u[0], v[0]);
            },
            uParameters, vParameters);
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

    /** @brief Evaluate one tensor-product surface per lane/sample from control and U/V parameter batches. */
    template<HomogeneousBatch Output, HomogeneousBatch ControlBatch, size_t UCount, size_t VCount,
             HomogeneousBatch UParameters, HomogeneousBatch VParameters>
        requires(std::remove_cvref_t<UParameters>::kDimension == 1 &&
                 std::remove_cvref_t<VParameters>::kDimension == 1)
    constexpr void Evaluate(Output output, const Tensor<ControlBatch, UCount, VCount>& controls,
                            UParameters uParameters, VParameters vParameters) {
        std::apply(
            [&](const auto&... controlBatches) {
                TransformBatch(
                    output,
                    [](const auto& u, const auto& v, const auto&... controlValues) noexcept(noexcept(
                        BezierSurface{Tensor<std::common_type_t<std::remove_cvref_t<decltype(controlValues)>...>,
                                             UCount, VCount>{controlValues...}}
                            .Evaluate(u[0], v[0]))) {
                        using Value = std::common_type_t<std::remove_cvref_t<decltype(controlValues)>...>;
                        auto surface = BezierSurface{Tensor<Value, UCount, VCount>{controlValues...}};
                        return surface.Evaluate(u[0], v[0]);
                    },
                    uParameters, vParameters, controlBatches...);
            },
            controls.Values());
    }

    /** @brief Evaluate one tensor-product surface per sample at common scalar U/V parameters. */
    template<HomogeneousBatch Output, HomogeneousBatch ControlBatch, size_t UCount, size_t VCount,
             typename UParameter, typename VParameter>
        requires(!HomogeneousBatch<std::remove_cvref_t<UParameter>> &&
                 !HomogeneousBatch<std::remove_cvref_t<VParameter>>)
    constexpr void Evaluate(Output output, const Tensor<ControlBatch, UCount, VCount>& controls,
                            const UParameter& u, const VParameter& v) {
        std::apply(
            [&](const auto&... controlBatches) {
                TransformBatch(
                    output,
                    [&u, &v](const auto&... controlValues) noexcept(noexcept(
                        BezierSurface{Tensor<std::common_type_t<std::remove_cvref_t<decltype(controlValues)>...>,
                                             UCount, VCount>{controlValues...}}
                            .Evaluate(u, v))) {
                        using Value = std::common_type_t<std::remove_cvref_t<decltype(controlValues)>...>;
                        auto surface = BezierSurface{Tensor<Value, UCount, VCount>{controlValues...}};
                        return surface.Evaluate(u, v);
                    },
                    controlBatches...);
            },
            controls.Values());
    }

} // namespace Sora::Math

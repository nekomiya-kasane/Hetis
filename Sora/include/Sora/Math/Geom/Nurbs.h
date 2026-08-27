/**
 * @file Nurbs.h
 * @brief Fixed-shape non-uniform rational B-spline curves and tensor-product surfaces.
 * @ingroup Geom
 */
#pragma once

#include <Sora/Math/Autodiff.h>
#include <Sora/Math/Point.h>
#include <Sora/Math/Tensor.h>
#include <Sora/Math/ValueBatch.h>

#include <array>
#include <cassert>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <expected>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Sora::Math {

    /** @brief Recoverable validation and evaluation failures of fixed NURBS patches. */
    enum class NurbsError {
        NonFiniteKnot,
        DecreasingKnots,
        ExcessiveInteriorMultiplicity,
        EmptyParameterDomain,
        NonFiniteControl,
        NonFiniteWeight,
        ZeroWeight,
        ParameterOutsideDomain,
        SingularProjection
    };

    namespace Detail {

        template<typename T>
        [[nodiscard]] constexpr decltype(auto) NurbsPrimal(const T& value) noexcept {
            if constexpr (DualValue<T> || ReverseValue<T>) {
                return NurbsPrimal(value.primal);
            } else {
                return value;
            }
        }

        template<ValueCarrier Left, ValueCarrier Right, size_t N, typename Parameter>
        [[nodiscard]] constexpr auto NurbsBlend(const Vector<Left, N>& left, const Vector<Right, N>& right,
                                                const Parameter& parameter) {
            using R = std::remove_cvref_t<decltype(Sora::Math::Fma(
                Sora::Math::Sub(right[0], left[0]), parameter, left[0]))>;
            return Vector<R, N>{GenerateStaticArray<R, N>([&](auto coordinate) {
                return Sora::Math::Fma(Sora::Math::Sub(right[coordinate], left[coordinate]), parameter,
                                       left[coordinate]);
            })};
        }

        template<ValueCarrier T, size_t Count>
            requires(Count > 1)
        [[nodiscard]] constexpr auto ProjectHomogeneous(const Vector<T, Count>& homogeneous) {
            constexpr size_t dimension = Count - 1;
            using R = std::remove_cvref_t<decltype(Sora::Math::Div(homogeneous[0], homogeneous[dimension]))>;
            return Point<R, dimension>{GenerateStaticArray<R, dimension>([&](auto coordinate) {
                return Sora::Math::Div(homogeneous[coordinate], homogeneous[dimension]);
            })};
        }

    } // namespace Detail

    /**
     * @brief Validated fixed-size B-spline basis axis.
     * @details The knot vector owns no allocation. The active parameter domain is
     * `[knots[Degree], knots[ControlCount]]`; span lookup is logarithmic and de Boor depth is compile-time fixed.
     */
    template<std::floating_point T, size_t Degree, size_t ControlCount>
        requires(ControlCount > Degree)
    class BSplineAxis {
    public:
        using ScalarType = T;
        static constexpr size_t kDegree = Degree;
        static constexpr size_t kControlCount = ControlCount;
        static constexpr size_t kKnotCount = ControlCount + Degree + 1;
        using KnotArray = std::array<T, kKnotCount>;

        /** @brief Validate and construct a B-spline axis. */
        [[nodiscard]] static constexpr auto Create(KnotArray knots) noexcept
            -> std::expected<BSplineAxis, NurbsError> {
            for (const T knot : knots) {
                if (!std::isfinite(knot)) {
                    return std::unexpected(NurbsError::NonFiniteKnot);
                }
            }
            for (size_t index = 1; index < kKnotCount; ++index) {
                if (knots[index] < knots[index - 1]) {
                    return std::unexpected(NurbsError::DecreasingKnots);
                }
            }
            if (!(knots[Degree] < knots[ControlCount])) {
                return std::unexpected(NurbsError::EmptyParameterDomain);
            }
            size_t runBegin = 0;
            while (runBegin < kKnotCount) {
                size_t runEnd = runBegin + 1;
                while (runEnd < kKnotCount && knots[runEnd] == knots[runBegin]) {
                    ++runEnd;
                }
                const bool interior = knots[Degree] < knots[runBegin] && knots[runBegin] < knots[ControlCount];
                if (interior && runEnd - runBegin > Degree + 1) {
                    return std::unexpected(NurbsError::ExcessiveInteriorMultiplicity);
                }
                runBegin = runEnd;
            }
            return BSplineAxis{std::move(knots), Validated{}};
        }

        /** @brief Return the immutable knot vector. */
        [[nodiscard]] constexpr const KnotArray& Knots() const noexcept { return knots_; }

        /** @brief Return the inclusive start of the active parameter domain. */
        [[nodiscard]] constexpr T DomainStart() const noexcept { return knots_[Degree]; }

        /** @brief Return the inclusive end of the active parameter domain. */
        [[nodiscard]] constexpr T DomainEnd() const noexcept { return knots_[ControlCount]; }

        /** @brief Test whether a scalar or automatic-differentiation parameter lies in the active domain. */
        template<typename Parameter>
            requires std::convertible_to<decltype(Detail::NurbsPrimal(std::declval<const Parameter&>())), T>
        [[nodiscard]] constexpr bool Contains(const Parameter& parameter) const noexcept {
            const T value = static_cast<T>(Detail::NurbsPrimal(parameter));
            return std::isfinite(value) && DomainStart() <= value && value <= DomainEnd();
        }

        /** @brief Locate the active knot span. @pre @p parameter lies in the active domain. */
        template<typename Parameter>
            requires std::convertible_to<decltype(Detail::NurbsPrimal(std::declval<const Parameter&>())), T>
        [[nodiscard]] constexpr size_t LocateSpan(const Parameter& parameter) const noexcept {
            const T value = static_cast<T>(Detail::NurbsPrimal(parameter));
            if (!(value < DomainEnd())) {
                return ControlCount - 1;
            }
            size_t lower = Degree;
            size_t upper = ControlCount;
            while (upper - lower > 1) {
                const size_t middle = lower + (upper - lower) / 2;
                if (value < knots_[middle]) {
                    upper = middle;
                } else {
                    lower = middle;
                }
            }
            return lower;
        }

        /** @brief Return a scalar or SIMD mask for membership in an active span. */
        template<typename Parameter>
            requires requires(const Parameter& parameter, T knot) {
                parameter >= knot;
                parameter < knot;
                parameter <= knot;
            }
        [[nodiscard]] constexpr auto ContainsSpan(const Parameter& parameter, size_t span) const noexcept {
            const auto lower = parameter >= knots_[span];
            const auto upper = span + 1 == ControlCount ? parameter <= knots_[ControlCount]
                                                         : parameter < knots_[span + 1];
            return lower && upper;
        }

        /**
         * @brief Contract one homogeneous control fiber with de Boor's algorithm in a known span.
         * @pre @p span is the active span for @p parameter and lies in `[Degree, ControlCount)`.
         */
        template<ValueCarrier Carrier, size_t N, typename Parameter>
        [[nodiscard, gnu::always_inline]] constexpr auto EvaluateInSpan(
            const std::array<Vector<Carrier, N>, ControlCount>& controls, const Parameter& parameter,
            size_t span) const {
            if constexpr (Degree == 0) {
                return Detail::NurbsBlend(controls[span], controls[span], parameter);
            } else {
                using Result = std::remove_cvref_t<decltype(
                    Detail::NurbsBlend(controls[0], controls[1], parameter))>;
                auto level = GenerateStaticArray<Result, Degree>([&](auto index) {
                    constexpr size_t offset = decltype(index)::value + 1;
                    const size_t knot = span - Degree + offset;
                    const T denominator = knots_[knot + Degree] - knots_[knot];
                    const auto alpha = Sora::Math::Div(Sora::Math::Sub(parameter, knots_[knot]), denominator);
                    return Detail::NurbsBlend(controls[span - Degree + offset - 1],
                                              controls[span - Degree + offset], alpha);
                });
                for (size_t depth = 2; depth <= Degree; ++depth) {
                    for (size_t offset = Degree + 1; offset-- > depth;) {
                        const size_t knot = span - Degree + offset;
                        const T denominator = knots_[knot + Degree - depth + 1] - knots_[knot];
                        const auto alpha = Sora::Math::Div(Sora::Math::Sub(parameter, knots_[knot]), denominator);
                        level[offset - 1] = Detail::NurbsBlend(level[offset - 2], level[offset - 1], alpha);
                    }
                }
                return level[Degree - 1];
            }
        }

    private:
        struct Validated {};

        constexpr BSplineAxis(KnotArray knots, Validated) noexcept : knots_(std::move(knots)) {}

        KnotArray knots_;
    };

    /** @brief Validated tensor product of B-spline axes over homogeneous control coordinates. */
    template<std::floating_point T, size_t Dimension, typename... Axes>
        requires(Dimension > 0 && sizeof...(Axes) > 0 &&
                 (std::same_as<typename Axes::ScalarType, T> && ...))
    class NurbsPatch {
    public:
        using ScalarType = T;
        using HomogeneousControl = Vector<T, Dimension + 1>;
        using EuclideanControl = Point<T, Dimension>;
        using HomogeneousControlTensor = Tensor<HomogeneousControl, Axes::kControlCount...>;
        using EuclideanControlTensor = Tensor<EuclideanControl, Axes::kControlCount...>;
        using WeightTensor = Tensor<T, Axes::kControlCount...>;
        static constexpr size_t kDimension = Dimension;
        static constexpr size_t kRank = sizeof...(Axes);
        static constexpr size_t kControlCount = (Axes::kControlCount * ...);

        /** @brief Validate Euclidean controls and finite nonzero projective weights, then homogenize them. */
        [[nodiscard]] static constexpr auto Create(EuclideanControlTensor controls, WeightTensor weights,
                                                   Axes... axes) noexcept
            -> std::expected<NurbsPatch, NurbsError> {
            for (size_t index = 0; index < kControlCount; ++index) {
                for (size_t coordinate = 0; coordinate < Dimension; ++coordinate) {
                    if (!std::isfinite(controls[index][coordinate])) {
                        return std::unexpected(NurbsError::NonFiniteControl);
                    }
                }
                if (!std::isfinite(weights[index])) {
                    return std::unexpected(NurbsError::NonFiniteWeight);
                }
                if (weights[index] == T{0}) {
                    return std::unexpected(NurbsError::ZeroWeight);
                }
            }
            auto homogeneousValues = GenerateStaticArray<HomogeneousControl, kControlCount>([&](auto index) {
                const T weight = weights[index];
                return HomogeneousControl{GenerateStaticArray<T, Dimension + 1>([&](auto coordinate) {
                    if constexpr (decltype(coordinate)::value == Dimension) {
                        return weight;
                    } else {
                        return Sora::Math::Mul(controls[index][coordinate], weight);
                    }
                })};
            });
            return NurbsPatch{HomogeneousControlTensor{std::move(homogeneousValues)},
                              std::tuple<Axes...>{std::move(axes)...}, Validated{}};
        }

        /** @brief Construct a unit-weight non-rational B-spline patch. */
        [[nodiscard]] static constexpr auto Create(EuclideanControlTensor controls, Axes... axes) noexcept
            -> std::expected<NurbsPatch, NurbsError> {
            auto weights = WeightTensor{GenerateStaticArray<T, kControlCount>([](auto) { return T{1}; })};
            return Create(std::move(controls), std::move(weights), std::move(axes)...);
        }

        /** @brief Return the immutable homogeneous control tensor. */
        [[nodiscard]] constexpr const HomogeneousControlTensor& Controls() const noexcept { return controls_; }

        /** @brief Return the immutable tuple of validated B-spline axes. */
        [[nodiscard]] constexpr const std::tuple<Axes...>& BasisAxes() const noexcept { return axes_; }

        /** @brief Evaluate with explicit prelocated spans and no validation branches. */
        template<typename... Parameters>
            requires(sizeof...(Parameters) == kRank)
        [[nodiscard, gnu::always_inline]] constexpr auto EvaluateInSpans(
            const std::array<size_t, kRank>& spans, const Parameters&... parameters) const {
            const auto evaluator = [&]<size_t Axis>(std::integral_constant<size_t, Axis>, const auto& fiber,
                                                    const auto& parameter) {
                return std::get<Axis>(axes_).EvaluateInSpan(fiber, parameter, spans[Axis]);
            };
            const auto homogeneous = ContractTensorAxes(controls_, evaluator, parameters...);
            return Detail::ProjectHomogeneous(homogeneous);
        }

        /** @brief Evaluate without domain checking while retaining logarithmic span lookup. */
        template<typename... Parameters>
            requires(sizeof...(Parameters) == kRank &&
                     (std::convertible_to<decltype(Detail::NurbsPrimal(std::declval<const Parameters&>())), T> && ...))
        [[nodiscard, gnu::always_inline]] constexpr auto EvaluateUnchecked(const Parameters&... parameters) const {
            const auto spans = std::apply(
                [&](const auto&... axes) { return std::array<size_t, kRank>{axes.LocateSpan(parameters)...}; },
                axes_);
            return EvaluateInSpans(spans, parameters...);
        }

        /** @brief Validate parameters and projective denominator, then evaluate the NURBS patch. */
        template<typename... Parameters>
            requires(sizeof...(Parameters) == kRank &&
                     (std::convertible_to<decltype(Detail::NurbsPrimal(std::declval<const Parameters&>())), T> && ...))
        [[nodiscard]] constexpr auto Evaluate(const Parameters&... parameters) const {
            const bool inside = std::apply(
                [&](const auto&... axes) { return (axes.Contains(parameters) && ...); }, axes_);
            using Result = std::remove_cvref_t<decltype(EvaluateUnchecked(parameters...))>;
            if (!inside) {
                return std::expected<Result, NurbsError>{
                    std::unexpected(NurbsError::ParameterOutsideDomain)};
            }

            const auto spans = std::apply(
                [&](const auto&... axes) { return std::array<size_t, kRank>{axes.LocateSpan(parameters)...}; },
                axes_);
            const auto evaluator = [&]<size_t Axis>(std::integral_constant<size_t, Axis>, const auto& fiber,
                                                    const auto& parameter) {
                return std::get<Axis>(axes_).EvaluateInSpan(fiber, parameter, spans[Axis]);
            };
            const auto homogeneous = ContractTensorAxes(controls_, evaluator, parameters...);
            const T denominator = static_cast<T>(Detail::NurbsPrimal(homogeneous[Dimension]));
            if (!std::isfinite(denominator) || denominator == T{0}) {
                return std::expected<Result, NurbsError>{std::unexpected(NurbsError::SingularProjection)};
            }
            return std::expected<Result, NurbsError>{
                Detail::ProjectHomogeneous(homogeneous)};
        }

    private:
        struct Validated {};

        constexpr NurbsPatch(HomogeneousControlTensor controls, std::tuple<Axes...> axes, Validated) noexcept
            : controls_(std::move(controls)), axes_(std::move(axes)) {}

        HomogeneousControlTensor controls_;
        std::tuple<Axes...> axes_;
    };

    /**
     * @brief Evaluate a NURBS patch over parameter batches whose knot spans are known and shared by every sample.
     * @details This is the branch-free batching primitive for span-bucketed workloads. Coordinate-major batches use
     * the preferred SIMD ABI through @ref TransformBatch; other layouts execute the identical scalar kernel.
     * @pre Every parameter sample lies in the supplied active span on each axis and all batch sizes are equal.
     */
    template<HomogeneousBatch Output, std::floating_point T, size_t Dimension, typename... Axes,
             HomogeneousBatch... Parameters>
        requires(sizeof...(Parameters) == sizeof...(Axes) &&
                 std::same_as<typename std::remove_cvref_t<Output>::template Rebind<T>, Point<T, Dimension>> &&
                 ((std::remove_cvref_t<Parameters>::kDimension == 1) && ...) &&
                 (std::same_as<typename std::remove_cvref_t<Parameters>::ScalarType, T> && ...))
    constexpr void EvaluateInSpans(Output output, const NurbsPatch<T, Dimension, Axes...>& patch,
                                   const std::array<size_t, sizeof...(Axes)>& spans, Parameters... parameters) {
        TransformBatch(
            output,
            [&patch, spans](const auto&... parameter) {
                return patch.EvaluateInSpans(spans, parameter[0]...);
            },
            parameters...);
    }

    /**
     * @brief Evaluate a NURBS patch over dynamic parameter batches with automatic span-aware SIMD.
     * @details A full SIMD packet uses one de Boor evaluation when every lane occupies the same span on every axis.
     * Only packets crossing a knot fall back to scalar lane evaluation. Call @ref EvaluateInSpans after span bucketing
     * when the caller can provide stronger coherence. The function performs no allocation or layout conversion.
     * @pre Every parameter sample lies in the active domain, the projective denominator is nonzero, and batch sizes
     * are equal.
     */
    template<HomogeneousBatch Output, std::floating_point T, size_t Dimension, typename... Axes,
             HomogeneousBatch... Parameters>
        requires(sizeof...(Parameters) == sizeof...(Axes) &&
                 std::same_as<typename std::remove_cvref_t<Output>::template Rebind<T>, Point<T, Dimension>> &&
                 ((std::remove_cvref_t<Parameters>::kDimension == 1) && ...) &&
                 (std::same_as<typename std::remove_cvref_t<Parameters>::ScalarType, T> && ...))
    constexpr void EvaluateUnchecked(Output output, const NurbsPatch<T, Dimension, Axes...>& patch,
                                     Parameters... parameters) {
        assert(((parameters.Size() == output.Size()) && ...) &&
               "EvaluateUnchecked requires equally-sized input and output batches");

        const auto evaluateScalar = [&](size_t sample) {
            output.Store(sample, patch.EvaluateUnchecked(parameters.Load(sample)[0]...));
        };

        if constexpr (CoordinateMajorBatch<Output> && (CoordinateMajorBatch<Parameters> && ...) &&
                      Simd::Vectorizable<T>) {
            using V = Simd::BasicVector<T, Simd::PreferredAbiT<T>>;
            constexpr size_t width = V::kSize.value;
            const auto parameterBatches = std::tuple<const Parameters&...>{parameters...};
            size_t offset = 0;
            for (; output.Size() - offset >= width; offset += width) {
                const auto packedParameters = std::tuple{Detail::LoadBatchValue<V>(parameters, offset)...};
                const auto spans = [&]<size_t... Axis>(std::index_sequence<Axis...>) {
                    return std::apply(
                        [&](const auto&... axes) {
                            return std::array<size_t, sizeof...(Axes)>{
                                axes.LocateSpan(std::get<Axis>(parameterBatches).Data()[offset])...};
                        },
                        patch.BasisAxes());
                }(std::make_index_sequence<sizeof...(Axes)>{});
                const bool coherent = [&]<size_t... Axis>(std::index_sequence<Axis...>) {
                    return std::apply(
                        [&](const auto&... axes) {
                            return (Simd::AllOf(axes.ContainsSpan(std::get<Axis>(packedParameters)[0], spans[Axis])) &&
                                    ...);
                        },
                        patch.BasisAxes());
                }(std::make_index_sequence<sizeof...(Axes)>{});
                if (coherent) {
                    const auto result = [&]<size_t... Axis>(std::index_sequence<Axis...>) {
                        return patch.EvaluateInSpans(spans, std::get<Axis>(packedParameters)[0]...);
                    }(std::make_index_sequence<sizeof...(Axes)>{});
                    Detail::StoreBatchValue<V>(output, offset, result);
                } else {
                    for (size_t lane = 0; lane < width; ++lane) {
                        evaluateScalar(offset + lane);
                    }
                }
            }
            for (; offset < output.Size(); ++offset) {
                evaluateScalar(offset);
            }
        } else {
            for (size_t sample = 0; sample < output.Size(); ++sample) {
                evaluateScalar(sample);
            }
        }
    }

    /** @brief Two-dimensional fixed NURBS curve. */
    template<std::floating_point T, size_t Degree, size_t ControlCount>
    using NurbsCurve2D = NurbsPatch<T, 2, BSplineAxis<T, Degree, ControlCount>>;

    /** @brief Three-dimensional fixed NURBS curve. */
    template<std::floating_point T, size_t Degree, size_t ControlCount>
    using NurbsCurve3D = NurbsPatch<T, 3, BSplineAxis<T, Degree, ControlCount>>;

    /** @brief Three-dimensional fixed tensor-product NURBS surface. */
    template<std::floating_point T, size_t UDegree, size_t UControlCount, size_t VDegree, size_t VControlCount>
    using NurbsSurface3D = NurbsPatch<T, 3, BSplineAxis<T, UDegree, UControlCount>,
                                     BSplineAxis<T, VDegree, VControlCount>>;

} // namespace Sora::Math

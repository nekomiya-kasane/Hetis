/**
 * @file NurbsTest.cpp
 * @brief Validation, dense reference sampling, automatic differentiation, and SIMD tests for fixed NURBS patches.
 */
#include <Sora/Math/Geom/Bezier.h>
#include <Sora/Math/Geom/Nurbs.h>
#include <Sora/Math/Simd.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace {

    using Sora::Math::BSplineAxis;
    using Sora::Math::BatchLayout;
    using Sora::Math::NurbsCurve2D;
    using Sora::Math::NurbsCurve3D;
    using Sora::Math::NurbsError;
    using Sora::Math::NurbsSurface3D;
    using Sora::Math::Point;
    using Sora::Math::PointBatchView;
    using Sora::Math::Tensor;
    using Sora::Math::VectorBatchView;
    using Float4 = Sora::Math::Simd::Vector<float, 4>;

    template<typename T>
    void CheckNear(T actual, T expected, T margin = static_cast<T>(1.0e-11)) {
        CHECK(actual == Catch::Approx(expected).margin(margin));
    }

    template<typename T, size_t N>
    void CheckNear(const Point<T, N>& actual, const Point<T, N>& expected,
                   T margin = static_cast<T>(1.0e-11)) {
        for (size_t coordinate = 0; coordinate < N; ++coordinate) {
            CheckNear(actual[coordinate], expected[coordinate], margin);
        }
    }

    template<size_t Degree, size_t ControlCount>
    [[nodiscard]] double Basis(size_t index, size_t degree, double parameter,
                               const std::array<double, ControlCount + Degree + 1>& knots) {
        if (degree == 0) {
            if (parameter == knots[ControlCount]) {
                return index + 1 == ControlCount ? 1.0 : 0.0;
            }
            return knots[index] <= parameter && parameter < knots[index + 1] ? 1.0 : 0.0;
        }
        double left = 0.0;
        const double leftDenominator = knots[index + degree] - knots[index];
        if (leftDenominator != 0.0) {
            left = (parameter - knots[index]) / leftDenominator *
                   Basis<Degree, ControlCount>(index, degree - 1, parameter, knots);
        }
        double right = 0.0;
        const double rightDenominator = knots[index + degree + 1] - knots[index + 1];
        if (rightDenominator != 0.0) {
            right = (knots[index + degree + 1] - parameter) / rightDenominator *
                    Basis<Degree, ControlCount>(index + 1, degree - 1, parameter, knots);
        }
        return left + right;
    }

    template<size_t Degree, size_t ControlCount, size_t Dimension>
    [[nodiscard]] Point<double, Dimension> RationalCurveReference(
        const Tensor<Point<double, Dimension>, ControlCount>& controls,
        const Tensor<double, ControlCount>& weights,
        const std::array<double, ControlCount + Degree + 1>& knots, double parameter) {
        std::array<double, Dimension> numerator{};
        double denominator = 0.0;
        for (size_t control = 0; control < ControlCount; ++control) {
            const double weightedBasis = Basis<Degree, ControlCount>(control, Degree, parameter, knots) *
                                         weights[control];
            denominator += weightedBasis;
            for (size_t coordinate = 0; coordinate < Dimension; ++coordinate) {
                numerator[coordinate] += controls[control][coordinate] * weightedBasis;
            }
        }
        for (double& coordinate : numerator) {
            coordinate /= denominator;
        }
        return Point<double, Dimension>{numerator};
    }

} // namespace

TEST_CASE("B-spline axes reject every invalid knot invariant", "[Sora][Math][Nurbs][Validation]") {
    using Axis = BSplineAxis<double, 2, 5>;
    const auto valid = Axis::Create({0.0, 0.0, 0.0, 0.35, 0.7, 1.0, 1.0, 1.0});
    REQUIRE(valid.has_value());
    CHECK(valid->DomainStart() == 0.0);
    CHECK(valid->DomainEnd() == 1.0);

    auto nonFinite = valid->Knots();
    nonFinite[3] = std::numeric_limits<double>::infinity();
    REQUIRE(Axis::Create(nonFinite).error() == NurbsError::NonFiniteKnot);

    auto decreasing = valid->Knots();
    decreasing[4] = 0.2;
    REQUIRE(Axis::Create(decreasing).error() == NurbsError::DecreasingKnots);

    REQUIRE(Axis::Create({0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0}).error() ==
            NurbsError::EmptyParameterDomain);

    using MultiplicityAxis = BSplineAxis<double, 2, 6>;
    REQUIRE(MultiplicityAxis::Create({0.0, 0.0, 0.0, 0.5, 0.5, 0.5, 1.0, 1.0, 1.0}).has_value());
    using ExcessiveMultiplicityAxis = BSplineAxis<double, 2, 7>;
    REQUIRE(ExcessiveMultiplicityAxis::Create({0.0, 0.0, 0.0, 0.5, 0.5, 0.5, 0.5, 1.0, 1.0, 1.0}).error() ==
            NurbsError::ExcessiveInteriorMultiplicity);
}

TEST_CASE("degree-zero, linear, and high-degree NURBS preserve their exact polynomial limits",
          "[Sora][Math][Nurbs][Degree]") {
    using ConstantAxis = BSplineAxis<double, 0, 3>;
    const auto constantAxis = ConstantAxis::Create({0.0, 0.3, 0.7, 1.0});
    REQUIRE(constantAxis.has_value());
    const Tensor<Point<double, 2>, 3> constantControls{
        Point<double, 2>{1.0, 2.0}, Point<double, 2>{3.0, 5.0}, Point<double, 2>{7.0, 11.0}};
    const auto constant = NurbsCurve2D<double, 0, 3>::Create(constantControls, *constantAxis);
    REQUIRE(constant.has_value());
    for (size_t sample = 0; sample <= 1000; ++sample) {
        const double parameter = static_cast<double>(sample) / 1000.0;
        const size_t control = parameter < 0.3 ? 0 : parameter < 0.7 ? 1 : 2;
        REQUIRE(constant->Evaluate(parameter).has_value());
        CheckNear(*constant->Evaluate(parameter), constantControls[control]);
    }

    using LinearAxis = BSplineAxis<double, 1, 2>;
    const auto linearAxis = LinearAxis::Create({0.0, 0.0, 1.0, 1.0});
    REQUIRE(linearAxis.has_value());
    const Tensor<Point<double, 2>, 2> linearControls{
        Point<double, 2>{-2.0, 1.0}, Point<double, 2>{4.0, 7.0}};
    const auto linear = NurbsCurve2D<double, 1, 2>::Create(linearControls, *linearAxis);
    REQUIRE(linear.has_value());
    for (size_t sample = 0; sample <= 2048; ++sample) {
        const double parameter = static_cast<double>(sample) / 2048.0;
        REQUIRE(linear->Evaluate(parameter).has_value());
        CheckNear(*linear->Evaluate(parameter), Sora::Math::BezierCurve{linearControls}.Evaluate(parameter), 2.0e-15);
    }

    using QuinticAxis = BSplineAxis<double, 5, 6>;
    const auto quinticAxis = QuinticAxis::Create(
        {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0});
    REQUIRE(quinticAxis.has_value());
    const Tensor<Point<double, 2>, 6> quinticControls{
        Point<double, 2>{-1.0, 0.0}, Point<double, 2>{0.5, 2.0}, Point<double, 2>{2.0, -1.0},
        Point<double, 2>{3.5, 4.0}, Point<double, 2>{5.0, 1.0}, Point<double, 2>{8.0, 3.0}};
    const auto quintic = NurbsCurve2D<double, 5, 6>::Create(quinticControls, *quinticAxis);
    REQUIRE(quintic.has_value());
    const Sora::Math::BezierCurve quinticBezier{quinticControls};
    for (size_t sample = 0; sample <= 4096; ++sample) {
        const double parameter = static_cast<double>(sample) / 4096.0;
        REQUIRE(quintic->Evaluate(parameter).has_value());
        CheckNear(*quintic->Evaluate(parameter), quinticBezier.Evaluate(parameter), 2.0e-13);
    }
}

TEST_CASE("unit-weight clamped cubic NURBS equals Bezier densely and composes with AD and SIMD",
          "[Sora][Math][Nurbs][Bezier][Autodiff][Simd]") {
    using Axis = BSplineAxis<float, 3, 4>;
    const auto axis = Axis::Create({0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F, 1.0F});
    REQUIRE(axis.has_value());
    const Tensor<Point<float, 3>, 4> controls{
        Point<float, 3>{0.0F, 0.0F, 0.0F}, Point<float, 3>{1.0F, 2.0F, -0.5F},
        Point<float, 3>{2.0F, 1.0F, 1.0F}, Point<float, 3>{4.0F, -1.0F, 2.0F}};
    const auto curve = NurbsCurve3D<float, 3, 4>::Create(controls, *axis);
    REQUIRE(curve.has_value());
    const Sora::Math::BezierCurve bezier{controls};

    for (size_t sample = 0; sample <= 4096; ++sample) {
        const float parameter = static_cast<float>(sample) / 4096.0F;
        const auto value = curve->Evaluate(parameter);
        REQUIRE(value.has_value());
        CheckNear(*value, bezier.Evaluate(parameter), 3.0e-5F);
    }
    REQUIRE(curve->Evaluate(-0.01F).error() == NurbsError::ParameterOutsideDomain);
    REQUIRE(curve->Evaluate(1.01F).error() == NurbsError::ParameterOutsideDomain);

    constexpr float parameter = 0.37F;
    const auto automatic = curve->Evaluate(Sora::Math::Variable(parameter));
    REQUIRE(automatic.has_value());
    const auto derivative = bezier.Derivative().Evaluate(parameter);
    for (size_t coordinate = 0; coordinate < 3; ++coordinate) {
        CheckNear((*automatic)[coordinate].primal, bezier.Evaluate(parameter)[coordinate], 2.0e-5F);
        CheckNear((*automatic)[coordinate].tangent, derivative[coordinate], 3.0e-5F);
    }

    Sora::Math::ReverseTape<float, 256> tape;
    const auto reverseParameter = tape.Variable(parameter);
    const auto reverse = curve->Evaluate(reverseParameter);
    REQUIRE(reverse.has_value());
    for (size_t coordinate = 0; coordinate < 3; ++coordinate) {
        tape.Backward((*reverse)[coordinate]);
        CheckNear(tape.Gradient(reverseParameter), derivative[coordinate], 3.0e-5F);
    }

    const Float4 parameters([](int lane) { return 0.2F + 0.2F * static_cast<float>(lane); });
    const auto packed = curve->EvaluateInSpans({3}, parameters);
    for (int lane = 0; lane < 4; ++lane) {
        const auto scalar = curve->EvaluateUnchecked(parameters[lane]);
        for (size_t coordinate = 0; coordinate < 3; ++coordinate) {
            CheckNear(packed[coordinate][lane], scalar[coordinate], 2.0e-5F);
        }
    }
}

TEST_CASE("rational quadratic NURBS represents a quarter circle", "[Sora][Math][Nurbs][Rational]") {
    using Axis = BSplineAxis<double, 2, 3>;
    const auto axis = Axis::Create({0.0, 0.0, 0.0, 1.0, 1.0, 1.0});
    REQUIRE(axis.has_value());
    const Tensor<Point<double, 2>, 3> controls{
        Point<double, 2>{1.0, 0.0}, Point<double, 2>{1.0, 1.0}, Point<double, 2>{0.0, 1.0}};
    const Tensor<double, 3> weights{1.0, std::sqrt(0.5), 1.0};
    const auto curve = NurbsCurve2D<double, 2, 3>::Create(controls, weights, *axis);
    REQUIRE(curve.has_value());

    for (size_t sample = 0; sample <= 4096; ++sample) {
        const double parameter = static_cast<double>(sample) / 4096.0;
        const auto value = curve->Evaluate(parameter);
        REQUIRE(value.has_value());
        CheckNear((*value)[0] * (*value)[0] + (*value)[1] * (*value)[1], 1.0, 3.0e-13);
    }
    const auto middle = curve->Evaluate(0.5);
    REQUIRE(middle.has_value());
    CheckNear((*middle)[0], std::sqrt(0.5), 2.0e-15);
    CheckNear((*middle)[1], std::sqrt(0.5), 2.0e-15);
}

TEST_CASE("non-uniform multi-span NURBS matches an independent Cox-de Boor reference densely",
          "[Sora][Math][Nurbs][Reference]") {
    constexpr std::array knots{0.0, 0.0, 0.0, 0.2, 0.55, 1.0, 1.0, 1.0};
    using Axis = BSplineAxis<double, 2, 5>;
    const auto axis = Axis::Create(knots);
    REQUIRE(axis.has_value());
    const Tensor<Point<double, 3>, 5> controls{
        Point<double, 3>{-1.0, 0.0, 0.5}, Point<double, 3>{0.0, 2.0, -0.5},
        Point<double, 3>{2.0, 1.0, 1.5}, Point<double, 3>{3.0, -1.0, 0.0},
        Point<double, 3>{5.0, 0.5, 2.0}};
    const Tensor<double, 5> weights{0.8, 1.3, 0.6, 1.8, 1.1};
    const auto curve = NurbsCurve3D<double, 2, 5>::Create(controls, weights, *axis);
    REQUIRE(curve.has_value());

    for (size_t sample = 0; sample <= 8192; ++sample) {
        const double parameter = static_cast<double>(sample) / 8192.0;
        const auto value = curve->Evaluate(parameter);
        REQUIRE(value.has_value());
        CheckNear(*value, RationalCurveReference<2, 5>(controls, weights, knots, parameter), 3.0e-12);
    }
}

TEST_CASE("NURBS curve batches vectorize coherent spans and preserve cross-knot and generic-layout samples",
          "[Sora][Math][Nurbs][Batch][Simd]") {
    constexpr std::array knots{0.0, 0.0, 0.0, 0.2, 0.55, 1.0, 1.0, 1.0};
    using Axis = BSplineAxis<double, 2, 5>;
    const auto axis = Axis::Create(knots);
    REQUIRE(axis.has_value());
    const Tensor<Point<double, 3>, 5> controls{
        Point<double, 3>{-1.0, 0.0, 0.5}, Point<double, 3>{0.0, 2.0, -0.5},
        Point<double, 3>{2.0, 1.0, 1.5}, Point<double, 3>{3.0, -1.0, 0.0},
        Point<double, 3>{5.0, 0.5, 2.0}};
    const Tensor<double, 5> weights{0.8, 1.3, 0.6, 1.8, 1.1};
    const auto curve = NurbsCurve3D<double, 2, 5>::Create(controls, weights, *axis);
    REQUIRE(curve.has_value());

    constexpr size_t sampleCount = 1031;
    std::array<double, sampleCount> parameters{};
    std::array<double, sampleCount * 3> results{};
    for (size_t sample = 0; sample < sampleCount; ++sample) {
        parameters[sample] = static_cast<double>(sample) / static_cast<double>(sampleCount - 1);
    }
    for (size_t sample = 0; sample < 8; ++sample) {
        parameters[sample] = sample % 2 == 0 ? 0.199 : 0.201;
    }
    const VectorBatchView<const double, 1> parameterBatch{parameters.data(), sampleCount};
    const PointBatchView<double, 3> resultBatch{results.data(), sampleCount};
    Sora::Math::EvaluateUnchecked(resultBatch, *curve, parameterBatch);
    for (size_t sample = 0; sample < sampleCount; ++sample) {
        CheckNear(resultBatch.Load(sample), curve->EvaluateUnchecked(parameters[sample]), 3.0e-12);
    }

    constexpr size_t bucketCount = 259;
    std::array<double, bucketCount> bucketParameters{};
    std::array<double, bucketCount * 3> bucketResults{};
    for (size_t sample = 0; sample < bucketCount; ++sample) {
        bucketParameters[sample] = 0.2 + 0.349 * static_cast<double>(sample) /
                                               static_cast<double>(bucketCount - 1);
    }
    const VectorBatchView<const double, 1> bucketInput{bucketParameters.data(), bucketCount};
    const PointBatchView<double, 3> bucketOutput{bucketResults.data(), bucketCount};
    Sora::Math::EvaluateInSpans(bucketOutput, *curve, {3}, bucketInput);
    for (size_t sample = 0; sample < bucketCount; ++sample) {
        CheckNear(bucketOutput.Load(sample), curve->EvaluateInSpans({3}, bucketParameters[sample]), 3.0e-12);
    }

    constexpr size_t pointMajorCount = 37;
    std::array<double, pointMajorCount> pointMajorParameters{};
    std::array<double, pointMajorCount * 3> pointMajorResults{};
    for (size_t sample = 0; sample < pointMajorCount; ++sample) {
        pointMajorParameters[sample] = static_cast<double>(sample) / static_cast<double>(pointMajorCount - 1);
    }
    const VectorBatchView<const double, 1, BatchLayout::PointMajor> pointMajorInput{
        pointMajorParameters.data(), pointMajorCount};
    const PointBatchView<double, 3, BatchLayout::PointMajor> pointMajorOutput{
        pointMajorResults.data(), pointMajorCount};
    Sora::Math::EvaluateUnchecked(pointMajorOutput, *curve, pointMajorInput);
    for (size_t sample = 0; sample < pointMajorCount; ++sample) {
        CheckNear(pointMajorOutput.Load(sample), curve->EvaluateUnchecked(pointMajorParameters[sample]), 3.0e-12);
    }
}

TEST_CASE("tensor-product NURBS surfaces match direct rational basis contraction",
          "[Sora][Math][Nurbs][Surface]") {
    constexpr std::array knots{0.0, 0.0, 0.0, 1.0, 1.0, 1.0};
    using Axis = BSplineAxis<double, 2, 3>;
    const auto uAxis = Axis::Create(knots);
    const auto vAxis = Axis::Create(knots);
    REQUIRE(uAxis.has_value());
    REQUIRE(vAxis.has_value());

    Tensor<Point<double, 3>, 3, 3> controls;
    Tensor<double, 3, 3> weights;
    for (size_t u = 0; u < 3; ++u) {
        for (size_t v = 0; v < 3; ++v) {
            controls(u, v) = Point<double, 3>{static_cast<double>(u), static_cast<double>(v),
                                               static_cast<double>(u * v + u)};
            weights(u, v) = 0.7 + 0.2 * static_cast<double>(u) + 0.15 * static_cast<double>(v * v);
        }
    }
    const auto surface = NurbsSurface3D<double, 2, 3, 2, 3>::Create(controls, weights, *uAxis, *vAxis);
    REQUIRE(surface.has_value());

    for (size_t uSample = 0; uSample <= 64; ++uSample) {
        const double u = static_cast<double>(uSample) / 64.0;
        for (size_t vSample = 0; vSample <= 64; ++vSample) {
            const double v = static_cast<double>(vSample) / 64.0;
            std::array<double, 3> numerator{};
            double denominator = 0.0;
            for (size_t uControl = 0; uControl < 3; ++uControl) {
                for (size_t vControl = 0; vControl < 3; ++vControl) {
                    const double basis = Basis<2, 3>(uControl, 2, u, knots) *
                                         Basis<2, 3>(vControl, 2, v, knots) * weights(uControl, vControl);
                    denominator += basis;
                    for (size_t coordinate = 0; coordinate < 3; ++coordinate) {
                        numerator[coordinate] += controls(uControl, vControl)[coordinate] * basis;
                    }
                }
            }
            const Point<double, 3> expected{
                numerator[0] / denominator, numerator[1] / denominator, numerator[2] / denominator};
            const auto value = surface->Evaluate(u, v);
            REQUIRE(value.has_value());
            CheckNear(*value, expected, 3.0e-12);
        }
    }
}

TEST_CASE("multi-span NURBS surfaces use the same span-aware batch interface on every tensor axis",
          "[Sora][Math][Nurbs][Surface][Batch][Simd]") {
    using UAxis = BSplineAxis<double, 2, 4>;
    using VAxis = BSplineAxis<double, 1, 3>;
    const auto uAxis = UAxis::Create({0.0, 0.0, 0.0, 0.4, 1.0, 1.0, 1.0});
    const auto vAxis = VAxis::Create({0.0, 0.0, 0.6, 1.0, 1.0});
    REQUIRE(uAxis.has_value());
    REQUIRE(vAxis.has_value());

    Tensor<Point<double, 3>, 4, 3> controls;
    Tensor<double, 4, 3> weights;
    for (size_t u = 0; u < 4; ++u) {
        for (size_t v = 0; v < 3; ++v) {
            const double x = static_cast<double>(u);
            const double y = static_cast<double>(v);
            controls(u, v) = Point<double, 3>{x, y, x * x + 2.0 * x * y - y};
            weights(u, v) = 0.65 + 0.17 * static_cast<double>(u) + 0.11 * static_cast<double>(v * v);
        }
    }
    const auto surface = NurbsSurface3D<double, 2, 4, 1, 3>::Create(controls, weights, *uAxis, *vAxis);
    REQUIRE(surface.has_value());

    constexpr size_t sampleCount = 769;
    std::array<double, sampleCount> uParameters{};
    std::array<double, sampleCount> vParameters{};
    std::array<double, sampleCount * 3> results{};
    for (size_t sample = 0; sample < sampleCount; ++sample) {
        uParameters[sample] = static_cast<double>(sample) / static_cast<double>(sampleCount - 1);
        vParameters[sample] = static_cast<double>((sample * 113) % sampleCount) /
                              static_cast<double>(sampleCount - 1);
    }
    const VectorBatchView<const double, 1> uInput{uParameters.data(), sampleCount};
    const VectorBatchView<const double, 1> vInput{vParameters.data(), sampleCount};
    const PointBatchView<double, 3> output{results.data(), sampleCount};
    Sora::Math::EvaluateUnchecked(output, *surface, uInput, vInput);
    for (size_t sample = 0; sample < sampleCount; ++sample) {
        CheckNear(output.Load(sample), surface->EvaluateUnchecked(uParameters[sample], vParameters[sample]),
                  4.0e-12);
    }
}

TEST_CASE("NURBS construction and evaluation expose weight and projection failures",
          "[Sora][Math][Nurbs][Errors]") {
    using Axis = BSplineAxis<double, 1, 2>;
    const auto axis = Axis::Create({0.0, 0.0, 1.0, 1.0});
    REQUIRE(axis.has_value());
    const Tensor<Point<double, 2>, 2> controls{Point<double, 2>{0.0, 0.0}, Point<double, 2>{1.0, 0.0}};

    REQUIRE(NurbsCurve2D<double, 1, 2>::Create(controls, Tensor<double, 2>{1.0, 0.0}, *axis).error() ==
            NurbsError::ZeroWeight);
    REQUIRE(NurbsCurve2D<double, 1, 2>::Create(
                controls, Tensor<double, 2>{1.0, std::numeric_limits<double>::quiet_NaN()}, *axis)
                .error() == NurbsError::NonFiniteWeight);

    const auto singular =
        NurbsCurve2D<double, 1, 2>::Create(controls, Tensor<double, 2>{1.0, -1.0}, *axis);
    REQUIRE(singular.has_value());
    REQUIRE(singular->Evaluate(0.5).error() == NurbsError::SingularProjection);
}

/**
 * @file TensorBezierTest.cpp
 * @brief Correctness, automatic-differentiation, SIMD, and batch tests for fixed tensors and Bezier geometry.
 */
#include <Sora/Math/Geom/Bezier.h>
#include <Sora/Math/Simd.h>
#include <Sora/Math/Types.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {

    using Sora::Math::BezierCurve;
    using Sora::Math::BezierSurface;
    using Sora::Math::Point;
    using Sora::Math::Point3f;
    using Sora::Math::Tensor;
    using Sora::Math::Vec3f;
    using Float4 = Sora::Math::Simd::Vector<float, 4>;

    struct NonDefaultScalar {
        float value;

        NonDefaultScalar() = delete;
        constexpr explicit NonDefaultScalar(float input) noexcept : value(input) {}

        friend constexpr NonDefaultScalar operator+(NonDefaultScalar left, NonDefaultScalar right) noexcept {
            return NonDefaultScalar{left.value + right.value};
        }

        friend constexpr NonDefaultScalar operator-(NonDefaultScalar left, NonDefaultScalar right) noexcept {
            return NonDefaultScalar{left.value - right.value};
        }

        friend constexpr NonDefaultScalar operator*(NonDefaultScalar left, NonDefaultScalar right) noexcept {
            return NonDefaultScalar{left.value * right.value};
        }

        template<typename Scalar>
            requires std::is_arithmetic_v<Scalar>
        friend constexpr NonDefaultScalar operator*(NonDefaultScalar left, Scalar right) noexcept {
            return NonDefaultScalar{left.value * static_cast<float>(right)};
        }

        friend constexpr NonDefaultScalar operator/(NonDefaultScalar left, NonDefaultScalar right) noexcept {
            return NonDefaultScalar{left.value / right.value};
        }
    };

    struct ThrowingDefaultScalar {
        float value;

        ThrowingDefaultScalar() noexcept(false) : value(0.0F) {}
    };

    struct ThrowingZeroScalar {
        int value;

        explicit ThrowingZeroScalar(int input) noexcept(false) : value(input) {}
    };

    static_assert(Sora::Math::TensorElement<NonDefaultScalar>);
    static_assert(!std::default_initializable<NonDefaultScalar>);
    static_assert(Sora::Math::TensorElement<ThrowingDefaultScalar>);
    static_assert(Sora::Math::ValueCarrier<ThrowingZeroScalar>);

    template<typename T>
    void CheckNear(T actual, T expected, T margin = static_cast<T>(1.0e-5)) {
        CHECK(actual == Catch::Approx(expected).margin(margin));
    }

    void CheckNear(const Point3f& actual, const Point3f& expected, float margin = 1.0e-5F) {
        for (size_t coordinate = 0; coordinate < 3; ++coordinate) {
            CheckNear(actual[coordinate], expected[coordinate], margin);
        }
    }

    void CheckNear(const Vec3f& actual, const Vec3f& expected, float margin = 1.0e-5F) {
        for (size_t coordinate = 0; coordinate < 3; ++coordinate) {
            CheckNear(actual[coordinate], expected[coordinate], margin);
        }
    }

    [[nodiscard]] constexpr double Binomial(size_t n, size_t k) noexcept {
        double value = 1.0;
        for (size_t index = 1; index <= k; ++index) {
            value *= static_cast<double>(n + 1 - index) / static_cast<double>(index);
        }
        return value;
    }

    template<size_t Degree>
    [[nodiscard]] double BernsteinReference(const BezierCurve<double, Degree>& curve, double parameter) {
        double result = 0.0;
        for (size_t index = 0; index <= Degree; ++index) {
            result += curve[index] * Binomial(Degree, index) * std::pow(parameter, static_cast<double>(index)) *
                      std::pow(1.0 - parameter, static_cast<double>(Degree - index));
        }
        return result;
    }

} // namespace

TEST_CASE("fixed tensors preserve row-major shape and tensor-product algebra", "[Sora][Math][Tensor]") {
    constexpr Tensor left{1.0, 2.0};
    constexpr Tensor right{3.0, 5.0, 7.0};
    constexpr auto product = Sora::Math::TensorProduct(left, right);
    STATIC_REQUIRE(decltype(product)::kRank == 2);
    STATIC_REQUIRE(decltype(product)::template Extent<0>() == 2);
    STATIC_REQUIRE(decltype(product)::template Extent<1>() == 3);
    STATIC_REQUIRE(product(0, 0) == 3.0);
    STATIC_REQUIRE(product(1, 2) == 14.0);
    STATIC_REQUIRE(Sora::Math::InnerProduct(left, left) == 5.0);
    STATIC_REQUIRE((left + left)[1] == 4.0);
}

TEST_CASE("Bezier construction does not impose hidden default carrier requirements",
          "[Sora][Math][Bezier][Concepts]") {
    STATIC_REQUIRE_FALSE(noexcept(BezierCurve<ThrowingDefaultScalar, 3>{}));
    STATIC_REQUIRE_FALSE(noexcept(BezierSurface<ThrowingDefaultScalar, 3, 3>{}));
    STATIC_REQUIRE_FALSE(noexcept(Sora::Math::Vector<ThrowingZeroScalar, 1>{}));
    STATIC_REQUIRE_FALSE(noexcept(Point<ThrowingZeroScalar, 1>{}));

    constexpr BezierCurve curve{
        Tensor<NonDefaultScalar, 4>{NonDefaultScalar{0.0F}, NonDefaultScalar{1.0F}, NonDefaultScalar{2.0F},
                                    NonDefaultScalar{4.0F}}};
    constexpr auto derivative = curve.Derivative();
    constexpr auto elevated = curve.Elevated();
    constexpr auto prepared = Sora::Math::Prepare(curve);
    STATIC_REQUIRE(derivative.Evaluate(0.5F).value == 3.75F);
    STATIC_REQUIRE(elevated.Evaluate(0.5F).value == curve.Evaluate(0.5F).value);
    STATIC_REQUIRE(prepared.Evaluate(0.5F).value == curve.Evaluate(0.5F).value);

    constexpr auto controls = []<size_t... Indices>(std::index_sequence<Indices...>) {
        return Tensor<NonDefaultScalar, 4, 4>{NonDefaultScalar{static_cast<float>(Indices)}...};
    }(std::make_index_sequence<16>{});
    constexpr BezierSurface surface{controls};
    constexpr auto derivativeU = surface.DerivativeU();
    constexpr auto derivativeV = surface.DerivativeV();
    constexpr auto preparedSurface = Sora::Math::Prepare(surface);
    STATIC_REQUIRE(derivativeU.Evaluate(0.25F, 0.75F).value == 12.0F);
    STATIC_REQUIRE(derivativeV.Evaluate(0.25F, 0.75F).value == 3.0F);
    STATIC_REQUIRE(preparedSurface.Evaluate(0.25F, 0.75F).value == surface.Evaluate(0.25F, 0.75F).value);
    STATIC_REQUIRE(surface.IsoU(0.25F).Evaluate(0.75F).value == surface.Evaluate(0.25F, 0.75F).value);
    STATIC_REQUIRE(surface.IsoV(0.75F).Evaluate(0.25F).value == surface.Evaluate(0.25F, 0.75F).value);
}

TEST_CASE("Bezier curves cover constant through high degrees", "[Sora][Math][Bezier][Curve]") {
    constexpr BezierCurve constant{Tensor{2.0}};
    constexpr BezierCurve linear{Tensor{1.0, 5.0}};
    constexpr BezierCurve quadratic{Tensor{0.0, 2.0, 0.0}};
    STATIC_REQUIRE(constant.Evaluate(0.75) == 2.0);
    STATIC_REQUIRE(constant.Derivative<1>().Evaluate(0.75) == 0.0);
    STATIC_REQUIRE(constant.Derivative<8>().Evaluate(0.75) == 0.0);
    STATIC_REQUIRE(linear.Evaluate(0.25) == 2.0);
    STATIC_REQUIRE(linear.Derivative<2>().Evaluate(0.25) == 0.0);
    STATIC_REQUIRE(quadratic.Evaluate(0.5) == 1.0);

    constexpr BezierCurve pointConstant{Tensor{Point3f{1.0F, 2.0F, 3.0F}}};
    constexpr auto pointDerivative = pointConstant.Derivative<4>();
    STATIC_REQUIRE(std::same_as<typename decltype(pointDerivative)::ControlType, Vec3f>);
    CHECK(pointDerivative.Evaluate(0.5F) == Vec3f{0.0F, 0.0F, 0.0F});

    const BezierCurve degreeFive{Tensor{0.0, 1.0, -2.0, 4.0, 3.0, 5.0}};
    for (size_t sample = 0; sample <= 512; ++sample) {
        const double parameter = static_cast<double>(sample) / 512.0;
        CHECK(degreeFive.Evaluate(parameter) ==
              Catch::Approx(BernsteinReference(degreeFive, parameter)).epsilon(2.0e-13));
    }
}

TEST_CASE("cubic preparation, derivatives, jets, subdivision, and elevation preserve one curve",
          "[Sora][Math][Bezier][Cubic]") {
    const BezierCurve curve{Tensor{Point3f{0.0F, 0.0F, 0.0F}, Point3f{1.0F, 2.0F, 0.0F},
                                   Point3f{2.0F, 2.0F, 1.0F}, Point3f{4.0F, 0.0F, 1.0F}}};
    const auto prepared = Sora::Math::Prepare(curve);
    const auto jet = Sora::Math::EvaluateJet<5>(curve, 0.25F);
    CheckNear(jet.value, curve.Evaluate(0.25F));
    CheckNear(jet.derivatives[0], curve.Derivative<1>().Evaluate(0.25F));
    CheckNear(jet.derivatives[1], curve.Derivative<2>().Evaluate(0.25F));
    CheckNear(jet.derivatives[2], curve.Derivative<3>().Evaluate(0.25F));
    CheckNear(jet.derivatives[3], Vec3f{});
    CheckNear(jet.derivatives[4], Vec3f{});

    for (size_t sample = 0; sample <= 1024; ++sample) {
        const float parameter = static_cast<float>(sample) / 1024.0F;
        CheckNear(prepared.Evaluate(parameter), curve.Evaluate(parameter), 2.0e-5F);
    }

    constexpr float split = 0.37F;
    const auto [left, right] = curve.Subdivide(split);
    const auto elevated = curve.Elevated();
    for (size_t sample = 0; sample <= 256; ++sample) {
        const float local = static_cast<float>(sample) / 256.0F;
        CheckNear(left.Evaluate(local), curve.Evaluate(local * split), 2.0e-5F);
        CheckNear(right.Evaluate(local), curve.Evaluate(split + local * (1.0F - split)), 2.0e-5F);
        CheckNear(elevated.Evaluate(local), curve.Evaluate(local), 2.0e-5F);
    }
}

TEST_CASE("Bezier evaluation composes with forward AD and explicit derivatives", "[Sora][Math][Bezier][Autodiff]") {
    constexpr BezierCurve curve{Tensor{Point3f{0.0F, 0.0F, 0.0F}, Point3f{1.0F, 2.0F, 0.0F},
                                        Point3f{2.0F, 2.0F, 1.0F}, Point3f{4.0F, 0.0F, 1.0F}}};
    const auto automatic = curve.Evaluate(Sora::Math::Variable(0.35F));
    const auto explicitDerivative = curve.Derivative().Evaluate(0.35F);
    for (size_t coordinate = 0; coordinate < 3; ++coordinate) {
        CHECK(automatic[coordinate].primal == curve.Evaluate(0.35F)[coordinate]);
        CheckNear(automatic[coordinate].tangent, explicitDerivative[coordinate]);
    }
}

TEST_CASE("Bezier curves and surfaces propagate reverse AD and preserve tape failures",
          "[Sora][Math][Bezier][Autodiff][Reverse]") {
    const BezierCurve curve{Tensor{Point3f{0.0F, 0.0F, 0.0F}, Point3f{1.0F, 2.0F, 0.0F},
                                   Point3f{2.0F, 2.0F, 1.0F}, Point3f{4.0F, 0.0F, 1.0F}}};
    Sora::Math::ReverseTape<float, 256> curveTape;
    const auto parameter = curveTape.Variable(0.35F);
    STATIC_REQUIRE(noexcept(curve.Evaluate(0.35F)));
    STATIC_REQUIRE_FALSE(noexcept(curve.Evaluate(parameter)));
    const auto curveValue = curve.Evaluate(parameter);
    const auto curveDerivative = curve.EvaluateDerivative<1>(0.35F);
    for (size_t coordinate = 0; coordinate < 3; ++coordinate) {
        CheckNear(curveValue[coordinate].primal, curve.Evaluate(0.35F)[coordinate]);
        curveTape.Backward(curveValue[coordinate]);
        CheckNear(curveTape.Gradient(parameter), curveDerivative[coordinate]);
    }

    Sora::Math::ReverseTape<float, 64> controlTape;
    const auto control0 = controlTape.Variable(0.0F);
    const auto control1 = controlTape.Variable(1.0F);
    const auto control2 = controlTape.Variable(2.0F);
    const auto control3 = controlTape.Variable(4.0F);
    const BezierCurve controlledCurve{Tensor{control0, control1, control2, control3}};
    const auto controlledValue = controlledCurve.Evaluate(0.25F);
    controlTape.Backward(controlledValue);
    CheckNear(controlTape.Gradient(control0), 0.421875F);
    CheckNear(controlTape.Gradient(control1), 0.421875F);
    CheckNear(controlTape.Gradient(control2), 0.140625F);
    CheckNear(controlTape.Gradient(control3), 0.015625F);

    Tensor<Point3f, 4, 4> controls;
    for (size_t u = 0; u < 4; ++u) {
        for (size_t v = 0; v < 4; ++v) {
            controls(u, v) = Point3f{static_cast<float>(u), static_cast<float>(v), static_cast<float>(u * v)};
        }
    }
    const BezierSurface surface{controls};
    Sora::Math::ReverseTape<float, 1024> surfaceTape;
    const auto u = surfaceTape.Variable(0.2F);
    const auto v = surfaceTape.Variable(0.7F);
    STATIC_REQUIRE(noexcept(surface.Evaluate(0.2F, 0.7F)));
    STATIC_REQUIRE_FALSE(noexcept(surface.Evaluate(u, v)));
    const auto surfaceValue = surface.Evaluate(u, v);
    const auto derivativeU = surface.EvaluatePartial<1, 0>(0.2F, 0.7F);
    const auto derivativeV = surface.EvaluatePartial<0, 1>(0.2F, 0.7F);
    for (size_t coordinate = 0; coordinate < 3; ++coordinate) {
        surfaceTape.Backward(surfaceValue[coordinate]);
        CheckNear(surfaceTape.Gradient(u), derivativeU[coordinate]);
        CheckNear(surfaceTape.Gradient(v), derivativeV[coordinate]);
    }

    const auto scalarPrepared = Sora::Math::Prepare(BezierCurve{Tensor{0.0F, 1.0F, 2.0F, 4.0F}});
    Sora::Math::ReverseTape<float, 3> exhaustedTape;
    const auto exhaustedParameter = exhaustedTape.Variable(0.5F);
    STATIC_REQUIRE_FALSE(noexcept(scalarPrepared.Evaluate(exhaustedParameter)));
    CHECK_THROWS_AS(scalarPrepared.Evaluate(exhaustedParameter), std::length_error);
}

TEST_CASE("one curve evaluates several samples in SIMD lanes", "[Sora][Math][Bezier][Simd]") {
    const BezierCurve curve{Tensor{Point3f{0.0F, 0.0F, 0.0F}, Point3f{1.0F, 2.0F, 0.0F},
                                   Point3f{2.0F, 2.0F, 1.0F}, Point3f{4.0F, 0.0F, 1.0F}}};
    const Float4 parameters([](int lane) { return 0.25F * static_cast<float>(lane); });
    const auto result = curve.Evaluate(parameters);
    for (int lane = 0; lane < 4; ++lane) {
        const auto expected = curve.Evaluate(parameters[lane]);
        for (size_t coordinate = 0; coordinate < 3; ++coordinate) {
            CheckNear(result[coordinate][lane], expected[coordinate]);
        }
    }
}

TEST_CASE("Bezier batch evaluation vectorizes samples and heterogeneous parameter dimensions",
          "[Sora][Math][Bezier][Batch]") {
    constexpr size_t count = 37;
    const BezierCurve curve{Tensor{Point3f{0.0F, 0.0F, 0.0F}, Point3f{1.0F, 2.0F, 0.0F},
                                   Point3f{2.0F, 2.0F, 1.0F}, Point3f{4.0F, 0.0F, 1.0F}}};
    std::array<float, count> parameters{};
    std::array<float, 3 * count> outputStorage{};
    for (size_t sample = 0; sample < count; ++sample) {
        parameters[sample] = static_cast<float>(sample) / static_cast<float>(count - 1);
    }
    const Sora::Math::VectorBatchView<const float, 1> parameterBatch(parameters.data(), count);
    const Sora::Math::PointBatchView<float, 3> output(outputStorage.data(), count);
    Sora::Math::Evaluate(output, curve, parameterBatch);
    for (size_t sample = 0; sample < count; ++sample) {
        CheckNear(output.Load(sample), curve.Evaluate(parameters[sample]));
    }
}

TEST_CASE("Bezier control batches vectorize multiple curves with common or lane-wise parameters",
          "[Sora][Math][Bezier][Batch][Instances]") {
    constexpr size_t count = 19;
    constexpr size_t controlCount = 4;
    std::array<std::array<float, 3 * count>, controlCount> controlStorage{};
    std::array<float, count> parameters{};
    std::array<float, 3 * count> outputStorage{};
    for (size_t sample = 0; sample < count; ++sample) {
        parameters[sample] = static_cast<float>(sample) / static_cast<float>(count);
        for (size_t control = 0; control < controlCount; ++control) {
            controlStorage[control][sample] = static_cast<float>(sample + control);
            controlStorage[control][count + sample] = static_cast<float>(2 * control);
            controlStorage[control][2 * count + sample] = static_cast<float>(sample - control);
        }
    }
    using ControlBatch = Sora::Math::PointBatchView<const float, 3>;
    const std::array controls{ControlBatch{controlStorage[0].data(), count},
                              ControlBatch{controlStorage[1].data(), count},
                              ControlBatch{controlStorage[2].data(), count},
                              ControlBatch{controlStorage[3].data(), count}};
    const Sora::Math::VectorBatchView<const float, 1> parameterBatch(parameters.data(), count);
    const Sora::Math::PointBatchView<float, 3> output(outputStorage.data(), count);

    Sora::Math::Evaluate(output, controls, parameterBatch);
    for (size_t sample = 0; sample < count; ++sample) {
        const BezierCurve curve{Tensor{controls[0].Load(sample), controls[1].Load(sample), controls[2].Load(sample),
                                       controls[3].Load(sample)}};
        CheckNear(output.Load(sample), curve.Evaluate(parameters[sample]));
    }

    Sora::Math::Evaluate(output, controls, 0.4F);
    for (size_t sample = 0; sample < count; ++sample) {
        const BezierCurve curve{Tensor{controls[0].Load(sample), controls[1].Load(sample), controls[2].Load(sample),
                                       controls[3].Load(sample)}};
        CheckNear(output.Load(sample), curve.Evaluate(0.4F));
    }
}

TEST_CASE("bicubic tensor-product surfaces expose isocurves and exact mixed partials",
          "[Sora][Math][Bezier][Surface]") {
    Tensor<Point3f, 4, 4> controls;
    for (size_t u = 0; u < 4; ++u) {
        for (size_t v = 0; v < 4; ++v) {
            controls(u, v) = Point3f{static_cast<float>(u), static_cast<float>(v), static_cast<float>(u * v)};
        }
    }
    const BezierSurface surface{controls};
    const auto prepared = Sora::Math::Prepare(surface);
    constexpr float u = 0.2F;
    constexpr float v = 0.7F;
    CheckNear(surface.Evaluate(u, v), Point3f{3.0F * u, 3.0F * v, 9.0F * u * v});
    CheckNear(surface.EvaluatePartial<1, 0>(u, v), Vec3f{3.0F, 0.0F, 9.0F * v});
    CheckNear(surface.EvaluatePartial<0, 1>(u, v), Vec3f{0.0F, 3.0F, 9.0F * u});
    CheckNear(surface.EvaluatePartial<1, 1>(u, v), Vec3f{0.0F, 0.0F, 9.0F});
    CheckNear(surface.IsoU(u).Evaluate(v), surface.Evaluate(u, v));
    CheckNear(surface.IsoV(v).Evaluate(u), surface.Evaluate(u, v));
    for (size_t uSample = 0; uSample <= 64; ++uSample) {
        for (size_t vSample = 0; vSample <= 64; ++vSample) {
            const float sampleU = static_cast<float>(uSample) / 64.0F;
            const float sampleV = static_cast<float>(vSample) / 64.0F;
            CheckNear(prepared.Evaluate(sampleU, sampleV), surface.Evaluate(sampleU, sampleV), 4.0e-5F);
        }
    }
}

TEST_CASE("bicubic surfaces batch scalar and SIMD parameter pairs", "[Sora][Math][Bezier][Surface][Batch]") {
    constexpr size_t count = 35;
    Tensor<Point3f, 4, 4> controls;
    for (size_t u = 0; u < 4; ++u) {
        for (size_t v = 0; v < 4; ++v) {
            controls(u, v) = Point3f{static_cast<float>(u), static_cast<float>(v), static_cast<float>(u * v)};
        }
    }
    const BezierSurface surface{controls};
    const auto prepared = Sora::Math::Prepare(surface);
    std::array<float, count> uParameters{};
    std::array<float, count> vParameters{};
    std::array<float, 3 * count> outputStorage{};
    for (size_t sample = 0; sample < count; ++sample) {
        uParameters[sample] = static_cast<float>(sample) / static_cast<float>(count - 1);
        vParameters[sample] = 1.0F - uParameters[sample];
    }
    const Sora::Math::VectorBatchView<const float, 1> uBatch(uParameters.data(), count);
    const Sora::Math::VectorBatchView<const float, 1> vBatch(vParameters.data(), count);
    const Sora::Math::PointBatchView<float, 3> output(outputStorage.data(), count);
    Sora::Math::Evaluate(output, surface, uBatch, vBatch);
    for (size_t sample = 0; sample < count; ++sample) {
        CheckNear(output.Load(sample), surface.Evaluate(uParameters[sample], vParameters[sample]));
    }
    Sora::Math::Evaluate(output, prepared, uBatch, vBatch);
    for (size_t sample = 0; sample < count; ++sample) {
        CheckNear(output.Load(sample), surface.Evaluate(uParameters[sample], vParameters[sample]), 4.0e-5F);
    }
}

TEST_CASE("Bezier control tensors vectorize multiple surfaces with common or lane-wise parameter pairs",
          "[Sora][Math][Bezier][Surface][Batch][Instances]") {
    constexpr size_t count = 11;
    std::array<std::array<float, 3 * count>, 16> controlStorage{};
    std::array<float, count> uParameters{};
    std::array<float, count> vParameters{};
    std::array<float, 3 * count> outputStorage{};
    for (size_t sample = 0; sample < count; ++sample) {
        uParameters[sample] = static_cast<float>(sample) / static_cast<float>(count);
        vParameters[sample] = 1.0F - uParameters[sample];
        for (size_t u = 0; u < 4; ++u) {
            for (size_t v = 0; v < 4; ++v) {
                const size_t control = 4 * u + v;
                controlStorage[control][sample] = static_cast<float>(u + sample);
                controlStorage[control][count + sample] = static_cast<float>(v);
                controlStorage[control][2 * count + sample] = static_cast<float>(u * v + sample);
            }
        }
    }

    using ControlBatch = Sora::Math::PointBatchView<const float, 3>;
    const auto controlBatches = [&]<size_t... Indices>(std::index_sequence<Indices...>) {
        return Tensor<ControlBatch, 4, 4>{ControlBatch{controlStorage[Indices].data(), count}...};
    }(std::make_index_sequence<16>{});
    const Sora::Math::VectorBatchView<const float, 1> uBatch(uParameters.data(), count);
    const Sora::Math::VectorBatchView<const float, 1> vBatch(vParameters.data(), count);
    const Sora::Math::PointBatchView<float, 3> output(outputStorage.data(), count);

    Sora::Math::Evaluate(output, controlBatches, uBatch, vBatch);
    for (size_t sample = 0; sample < count; ++sample) {
        Tensor<Point3f, 4, 4> controls;
        for (size_t control = 0; control < 16; ++control) {
            controls[control] = controlBatches[control].Load(sample);
        }
        CheckNear(output.Load(sample), BezierSurface{controls}.Evaluate(uParameters[sample], vParameters[sample]));
    }

    Sora::Math::Evaluate(output, controlBatches, 0.3F, 0.6F);
    for (size_t sample = 0; sample < count; ++sample) {
        Tensor<Point3f, 4, 4> controls;
        for (size_t control = 0; control < 16; ++control) {
            controls[control] = controlBatches[control].Load(sample);
        }
        CheckNear(output.Load(sample), BezierSurface{controls}.Evaluate(0.3F, 0.6F));
    }
}

TEST_CASE("curve tensor products produce separable Bezier surfaces", "[Sora][Math][Bezier][TensorProduct]") {
    constexpr BezierCurve uCurve{Tensor{1.0, 2.0}};
    constexpr BezierCurve vCurve{Tensor{3.0, 5.0, 7.0}};
    constexpr auto surface = Sora::Math::TensorProduct(
        uCurve, vCurve, [](double left, double right) noexcept { return left * right; });
    constexpr double u = 0.3;
    constexpr double v = 0.6;
    STATIC_REQUIRE(surface.Evaluate(u, v) == uCurve.Evaluate(u) * vCurve.Evaluate(v));
}

TEST_CASE("rank-generic cubic Bezier patches preserve closure and minimal prepared storage",
          "[Sora][Math][Bezier][RankN]") {
    STATIC_REQUIRE(sizeof(Sora::Math::CubicBezierPatch<Point3f, 1>) == sizeof(Point3f) + 3 * sizeof(Vec3f));
    STATIC_REQUIRE(sizeof(Sora::Math::CubicBezierPatch<Point3f, 2>) == sizeof(Point3f) + 15 * sizeof(Vec3f));
    STATIC_REQUIRE(sizeof(Sora::Math::CubicBezierPatch<Point3f, 3>) == sizeof(Point3f) + 63 * sizeof(Vec3f));

    Tensor<Point3f, 4, 4, 4> controls;
    for (size_t u = 0; u < 4; ++u) {
        for (size_t v = 0; v < 4; ++v) {
            for (size_t w = 0; w < 4; ++w) {
                controls(u, v, w) = Point3f{static_cast<float>(u), static_cast<float>(v),
                                             static_cast<float>(w + u * v * w)};
            }
        }
    }
    const Sora::Math::BezierPatch patch{controls};
    const auto prepared = Sora::Math::Prepare(patch);
    constexpr float split = 0.41F;
    const auto [left, right] = patch.Subdivide<2>(split);
    const auto elevated = patch.ElevatedAxis<1>();
    const auto restricted = patch.Restrict<1>(0.37F);

    for (size_t uSample = 0; uSample <= 16; ++uSample) {
        const float u = static_cast<float>(uSample) / 16.0F;
        for (size_t vSample = 0; vSample <= 16; ++vSample) {
            const float v = static_cast<float>(vSample) / 16.0F;
            for (size_t wSample = 0; wSample <= 16; ++wSample) {
                const float w = static_cast<float>(wSample) / 16.0F;
                const Point3f expected{3.0F * u, 3.0F * v, 3.0F * w + 27.0F * u * v * w};
                CheckNear(patch.Evaluate(u, v, w), expected, 5.0e-5F);
                CheckNear(prepared.Evaluate(u, v, w), expected, 8.0e-5F);
                CheckNear(elevated.Evaluate(u, v, w), expected, 8.0e-5F);
                CheckNear(restricted.Evaluate(u, w), patch.Evaluate(u, 0.37F, w), 6.0e-5F);
                CheckNear(left.Evaluate(u, v, w), patch.Evaluate(u, v, w * split), 8.0e-5F);
                CheckNear(right.Evaluate(u, v, w), patch.Evaluate(u, v, split + w * (1.0F - split)), 8.0e-5F);
            }
        }
    }
    CheckNear(patch.EvaluatePartial<1, 1, 1>(0.2F, 0.4F, 0.7F), Vec3f{0.0F, 0.0F, 27.0F});

    constexpr BezierCurve coefficientCurve{Tensor{1.0, 2.0}};
    constexpr BezierSurface coefficientSurface{Tensor<double, 2, 2>{3.0, 4.0, 5.0, 7.0}};
    constexpr auto product = Sora::Math::TensorProduct(coefficientCurve, coefficientSurface);
    CheckNear(product.Evaluate(0.2, 0.4, 0.7),
              coefficientCurve.Evaluate(0.2) * coefficientSurface.Evaluate(0.4, 0.7), 2.0e-15);
}

TEST_CASE("rank-generic Bezier control tensors SIMD-vectorize multi-instance evaluation",
          "[Sora][Math][Bezier][RankN][Batch]") {
    constexpr size_t count = 17;
    constexpr size_t controlCount = 8;
    std::array<std::array<float, 3 * count>, controlCount> controlStorage{};
    std::array<float, count> uParameters{};
    std::array<float, count> vParameters{};
    std::array<float, count> wParameters{};
    std::array<float, 3 * count> outputStorage{};
    for (size_t sample = 0; sample < count; ++sample) {
        uParameters[sample] = static_cast<float>(sample) / static_cast<float>(count - 1);
        vParameters[sample] = 1.0F - 0.5F * uParameters[sample];
        wParameters[sample] = 0.25F + 0.5F * uParameters[sample];
        for (size_t control = 0; control < controlCount; ++control) {
            controlStorage[control][sample] = static_cast<float>(sample + control);
            controlStorage[control][count + sample] =
                static_cast<float>(2 * static_cast<int>(control) - static_cast<int>(sample));
            controlStorage[control][2 * count + sample] = static_cast<float>(sample * (control + 1));
        }
    }

    using ControlBatch = Sora::Math::PointBatchView<const float, 3>;
    const auto controlBatches = [&]<size_t... Indices>(std::index_sequence<Indices...>) {
        return Tensor<ControlBatch, 2, 2, 2>{ControlBatch{controlStorage[Indices].data(), count}...};
    }(std::make_index_sequence<controlCount>{});
    const Sora::Math::VectorBatchView<const float, 1> uBatch(uParameters.data(), count);
    const Sora::Math::VectorBatchView<const float, 1> vBatch(vParameters.data(), count);
    const Sora::Math::VectorBatchView<const float, 1> wBatch(wParameters.data(), count);
    const Sora::Math::PointBatchView<float, 3> output(outputStorage.data(), count);

    Sora::Math::Evaluate(output, controlBatches, uBatch, vBatch, wBatch);
    for (size_t sample = 0; sample < count; ++sample) {
        Tensor<Point3f, 2, 2, 2> scalarControls;
        for (size_t control = 0; control < controlCount; ++control) {
            scalarControls[control] = controlBatches[control].Load(sample);
        }
        const Sora::Math::BezierPatch patch{scalarControls};
        CheckNear(output.Load(sample), patch.Evaluate(uParameters[sample], vParameters[sample], wParameters[sample]));
    }

    Sora::Math::Evaluate(output, controlBatches, 0.2F, 0.4F, 0.6F);
    for (size_t sample = 0; sample < count; ++sample) {
        Tensor<Point3f, 2, 2, 2> scalarControls;
        for (size_t control = 0; control < controlCount; ++control) {
            scalarControls[control] = controlBatches[control].Load(sample);
        }
        CheckNear(output.Load(sample), Sora::Math::BezierPatch{scalarControls}.Evaluate(0.2F, 0.4F, 0.6F));
    }
}

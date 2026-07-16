#include <Sora/Math/Autodiff.h>
#include <Sora/Math/Batch.h>
#include <Sora/Math/Compile.h>
#include <Sora/Math/PrimaryFunctions.h>
#include <Sora/Math/ScalarMath.h>
#include <Sora/Math/Simd.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <complex>
#include <cstdint>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace PrimaryFunctionTest {

    struct Number {
        int value;
    };

    struct Backend {
        [[nodiscard]] static constexpr Number Add(Number left, Number right) noexcept {
            return Number{left.value + right.value};
        }

        [[nodiscard]] static constexpr Number Square(Number value) noexcept {
            return Number{value.value * value.value};
        }
    };

} // namespace PrimaryFunctionTest

template<>
struct Sora::Math::Hook::Backend<PrimaryFunctionTest::Number> {
    using Type = PrimaryFunctionTest::Backend;
};

namespace {

    namespace Simd = Sora::Math::Simd;

    using Float4 = Simd::Vector<float, 4>;
    using Int1 = Simd::Vector<int, 1>;
    using Int4 = Simd::Vector<int, 4>;
    using UInt4 = Simd::Vector<std::uint32_t, 4>;
    using Float4Backend = Sora::Math::Backend::FixedSimdCPU<4>;

    consteval bool ConstexprArithmeticWorks() {
        const Int1 left(4);
        const Int1 right(2);
        const Int1 result = left + right;
        return result[0] == 6;
    }

    static_assert(ConstexprArithmeticWorks());

    consteval bool ConstexprBackendArithmeticWorks() {
        using Backend = Sora::Math::Backend::FixedSimdCPU<1>;
        const Int1 two(2);
        const Int1 three(3);
        return Backend::Add(two, three)[0] == 5 && Backend::Mul(two, three)[0] == 6 &&
               Backend::Fma(two, three, two)[0] == 8 && Backend::Sign(Backend::Neg(two))[0] == -1;
    }

    static_assert(ConstexprBackendArithmeticWorks());

    static_assert(std::same_as<Sora::Traits::BackendTypeOf<float>, Sora::Math::Backend::ScalarCPU>);
    static_assert(std::same_as<Sora::Traits::BackendTypeOf<Float4>, Float4Backend>);
    static_assert(std::same_as<decltype(Sora::Math::Add(1.0F, 2.0F)), float>);
    static_assert(noexcept(Sora::Math::Add(1.0F, 2.0F)));
    static_assert(std::invocable<decltype(Sora::Math::Sin), float>);
    static_assert(!std::invocable<decltype(Sora::Math::Sin), int>);
    static_assert(std::invocable<decltype(Sora::Math::Add), float, double>);
    static_assert(std::same_as<decltype(Sora::Math::Add(1.0F, 2.0)), double>);
    static_assert(Sora::Math::Factorial(5) == 120);

    constexpr auto kDualInput = Sora::Math::Variable(2.0);
    constexpr auto kDualOutput = Sora::Math::Add(Sora::Math::Square(kDualInput), 3.0);
    static_assert(kDualOutput.primal == 7.0);
    static_assert(kDualOutput.tangent == 4.0);
    static_assert(Sora::Math::Add(kDualInput).primal == 2.0);
    static_assert(Sora::Math::Mul(kDualInput).tangent == 1.0);
    static_assert(sizeof(Sora::Math::Dual<float>) == 2 * sizeof(float));
    static_assert(std::is_trivially_copyable_v<Sora::Math::Dual<float>>);
    static_assert(Sora::Math::DifferentiableValue<float>);
    static_assert(Sora::Math::DifferentiableValue<Float4>);
    static_assert(!Sora::Math::DifferentiableValue<int>);
    static_assert(!std::copy_constructible<Sora::Math::ReverseTape<float, 4>>);
    static_assert(!std::movable<Sora::Math::ReverseTape<float, 4>>);
    static_assert(sizeof(Sora::Math::Reverse<float, 128>) == 16);
    static_assert(sizeof(Sora::Math::ReverseTape<float, 128>) == 2568);
    static_assert(std::is_trivially_copyable_v<Sora::Math::Reverse<float, 128>>);
    static_assert(std::same_as<decltype(Sora::Math::Abs(std::numeric_limits<int>::min())), unsigned int>);
    static_assert(Sora::Math::Abs(std::numeric_limits<int>::min()) == 1U + unsigned(std::numeric_limits<int>::max()));
    static_assert(!std::invocable<decltype(Sora::Math::Inv), int>);
    static_assert(!std::invocable<decltype(Sora::Math::Inv), Int4>);
    static_assert(std::same_as<typename Simd::Vector<float, 8>::AbiType, Simd::AbiT<8, 2>>);
    static_assert(Sora::Math::Sqrt(-1.0) != Sora::Math::Sqrt(-1.0));
    static_assert(Sora::Math::Log(-1.0) != Sora::Math::Log(-1.0));
    static_assert(Sora::Math::Pow(-2.0, 3.0) == -8.0);
    static_assert(Sora::Math::Sin(0.5) > 0.4794255386042 && Sora::Math::Sin(0.5) < 0.4794255386043);
    static_assert(Sora::Math::Sin(1.0e6) > -0.349993502172 && Sora::Math::Sin(1.0e6) < -0.349993502170);
    static_assert(Sora::Math::Exp(1.0) > 2.7182818284590 && Sora::Math::Exp(1.0) < 2.7182818284591);
    static_assert(Sora::Math::Log(2.0) == Sora::Math::Const::kLn2<double>);
    static_assert((std::bit_cast<std::uint64_t>(Sora::Math::Atan(-0.0)) >> 63U) != 0);
    static_assert(Sora::Math::Atan2(std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()) ==
                  Sora::Math::Const::kPiOverFour<double>);
    static_assert((std::bit_cast<std::uint64_t>(Sora::Math::Pow(-0.0, 3.0)) >> 63U) != 0);
    static_assert(Sora::Math::Pow(1.0, std::numeric_limits<double>::quiet_NaN()) == 1.0);
    static_assert((std::bit_cast<std::uint64_t>(Sora::Math::CopySign(0.0, -1.0)) >> 63U) != 0);
    static_assert(noexcept(Sora::Math::Sin(Float4{})));
    static_assert(noexcept(Sora::Math::Sin(Sora::Math::Variable(1.0F))));

    constexpr Float4 kConstexprVector([](int index) { return static_cast<float>(index); });
    constexpr Float4 kConstexprExp = Sora::Math::Exp(kConstexprVector);
    static_assert(kConstexprExp[0] == 1.0F);
    static_assert(kConstexprExp[1] > 2.71828F && kConstexprExp[1] < 2.71829F);

    constexpr auto kSquaredSum = Sora::Compose(Sora::Math::Square, Sora::Math::Add);
    static_assert(kSquaredSum(2, 3) == 25);
    static_assert(kSquaredSum(PrimaryFunctionTest::Number{2}, PrimaryFunctionTest::Number{3}).value == 25);
    static_assert(std::same_as<Sora::Traits::BackendTypeOf<PrimaryFunctionTest::Number>, PrimaryFunctionTest::Backend>);
    static_assert(std::is_trivially_copyable_v<decltype(kSquaredSum)>);
    static_assert(noexcept(kSquaredSum(2, 3)));
    static_assert(!std::invocable<decltype(Sora::Math::Sin), PrimaryFunctionTest::Number>);

    constexpr auto kCompiledSquareAdd = Sora::Math::Compile<1>([](auto x) {
        return Sora::Math::Add(Sora::Math::Square(x), 1.0F);
    });
    constexpr auto kCompiledSquare = Sora::Math::Compile<1>([](auto x) { return Sora::Math::Square(x); });
    static_assert(std::invocable<decltype(kCompiledSquareAdd), float>);
    static_assert(std::is_trivially_copyable_v<decltype(kCompiledSquareAdd)>);
    static_assert(noexcept(kCompiledSquareAdd(2.0F)));
    static_assert(sizeof(kCompiledSquare) == 1);
    static_assert(sizeof(kCompiledSquareAdd) == sizeof(float));

    struct ThrowingCopy {
        constexpr ThrowingCopy() = default;
        ThrowingCopy(const ThrowingCopy&) noexcept(false) {}
    };

    struct RvalueOnlyCompiler {
        template<typename T>
        [[nodiscard]] constexpr auto operator()(T value) && {
            return Sora::Math::Square(value);
        }
    };

    constexpr auto kCompiledIdentity = Sora::Math::Compile<1>([](auto x) { return x; });
    constexpr auto kCompiledRvalueOnly = Sora::Math::Compile<1>(RvalueOnlyCompiler{});
    static_assert(!noexcept(kCompiledIdentity(std::declval<ThrowingCopy&>())));
    static_assert(std::invocable<decltype(kCompiledRvalueOnly), float>);

    using Staged0 = Sora::Math::Detail::StagedInput<0>;
    using Staged1 = Sora::Math::Detail::StagedInput<1>;
    using Staged2 = Sora::Math::Detail::StagedInput<2>;
    using StagedProduct = Sora::Math::Detail::StagedBinary<Sora::Math::Mul, Staged0, Staged1>;
    using StagedFma = Sora::Math::Detail::StagedTernary<Sora::Math::Fma, Staged0, Staged1, Staged2>;
    using StagedNms = Sora::Math::Detail::StagedTernary<Sora::Math::Nms, Staged0, Staged1, Staged2>;
    using StagedNegatedFma = Sora::Math::Detail::StagedUnary<Sora::Math::Neg, StagedFma>;

    constexpr StagedProduct kStagedProduct{Staged0{}, Staged1{}};
    constexpr Sora::Math::Detail::StagedBinary<Sora::Math::Add, StagedProduct, Staged2> kStagedMultiplyAdd{
        kStagedProduct, Staged2{}};
    constexpr auto kNormalizedFma = Sora::Math::Detail::Normalize(kStagedMultiplyAdd);
    constexpr auto kNormalizedNms =
        Sora::Math::Detail::Normalize(Sora::Math::Detail::MakeUnary<Sora::Math::Neg>(kStagedMultiplyAdd));
    constexpr auto kNormalizedExplicitNegation =
        Sora::Math::Detail::Normalize(Sora::Math::Detail::MakeUnary<Sora::Math::Neg>(kNormalizedFma));
    static_assert(std::same_as<std::remove_cv_t<decltype(kNormalizedFma)>, StagedFma>);
    static_assert(std::same_as<std::remove_cv_t<decltype(kNormalizedNms)>, StagedNms>);
    static_assert(std::same_as<std::remove_cv_t<decltype(kNormalizedExplicitNegation)>, StagedNegatedFma>);

    constexpr auto kCompiledProductSum = Sora::Math::Compile<4>([](auto a, auto b, auto c, auto d) {
        return Sora::Math::Add(Sora::Math::Mul(a, b), Sora::Math::Mul(c, d));
    });
    constexpr auto kCompiledSquareSum = Sora::Math::Compile<2>([](auto a, auto b) {
        return Sora::Math::Add(Sora::Math::Square(a), Sora::Math::Square(b));
    });
    constexpr auto kCompiledNegatedProductSum = Sora::Math::Compile<4>([](auto a, auto b, auto c, auto d) {
        return Sora::Math::Neg(Sora::Math::Add(Sora::Math::Mul(a, b), Sora::Math::Mul(c, d)));
    });
    static_assert(std::invocable<decltype(kCompiledProductSum), float, float, float, float>);
    static_assert(std::invocable<decltype(kCompiledSquareSum), float, float>);
    static_assert(std::invocable<decltype(kCompiledNegatedProductSum), float, float, float, float>);

} // namespace

TEST_CASE("primary function objects expose one scalar and SIMD interface", "[Sora][Math][Backend]") {
    using namespace Sora::Math;

    REQUIRE(Add(2.0F, 3.0F, 4.0F) == 9.0F);
    REQUIRE(Sub(3.0F, 2.0F) == 1.0F);
    REQUIRE(Mul(2.0F, 3.0F, 4.0F) == 24.0F);
    REQUIRE(Div(4.0F, 2.0F) == 2.0F);
    REQUIRE(Neg(2.0F) == -2.0F);
    REQUIRE(Inv(2.0F) == 0.5F);
    REQUIRE(Square(3.0F) == 9.0F);
    REQUIRE(Abs(-2.0F) == 2.0F);
    REQUIRE(Sin(0.0F) == 0.0F);
    REQUIRE(Cos(0.0F) == 1.0F);
    REQUIRE(Tan(0.0F) == 0.0F);
    REQUIRE(Asin(0.0F) == 0.0F);
    REQUIRE(Acos(1.0F) == 0.0F);
    REQUIRE(Atan(0.0F) == 0.0F);
    REQUIRE(Atan2(0.0F, 1.0F) == 0.0F);
    REQUIRE(Exp(0.0F) == 1.0F);
    REQUIRE(Log(1.0F) == 0.0F);
    REQUIRE(Sqrt(4.0F) == 2.0F);
    REQUIRE(Pow(2.0F, 3.0F) == 8.0F);
    REQUIRE(Fma(2.0F, 3.0F, 1.0F) == 7.0F);
    REQUIRE(Mfs(2.0F, 3.0F, 1.0F) == 5.0F);
    REQUIRE(Nms(2.0F, 3.0F, 1.0F) == -7.0F);
    REQUIRE(Nma(2.0F, 3.0F, 1.0F) == -5.0F);
    REQUIRE(Lerp(1.0F, 3.0F, 0.5F) == 2.0F);
    REQUIRE(Clamp(4.0F, 1.0F, 3.0F) == 3.0F);
    REQUIRE(Saturate(-1.0F) == 0.0F);
    REQUIRE(Sign(-2.0F) == -1.0F);

    const Float4 values([](int index) { return static_cast<float>(index); });
    const Float4 result = Fma(Cos(values), Float4(2.0F), Float4(1.0F));
    REQUIRE(result[0] == 3.0F);
}

TEST_CASE("forward and reverse automatic differentiation share primitive rules", "[Sora][Math][Autodiff]") {
    using namespace Sora::Math;

    const auto forward = Exp(Mul(Variable(2.0), 3.0));
    REQUIRE(std::abs(forward.primal - std::exp(6.0)) < 1e-12);
    REQUIRE(std::abs(forward.tangent - 3.0 * std::exp(6.0)) < 1e-10);

    ReverseTape<double, 16> tape;
    const auto x = tape.Variable(2.0);
    const auto y = tape.Variable(3.0);
    const auto output = Add(Mul(x, y), Sin(x));
    REQUIRE_THROWS_AS(tape.Gradient(x), std::logic_error);
    tape.Backward(output);

    REQUIRE(std::abs(tape.Gradient(x) - (3.0 + std::cos(2.0))) < 1e-12);
    REQUIRE(std::abs(tape.Gradient(y) - 2.0) < 1e-12);
    REQUIRE(tape.Size() == 5);

    static_cast<void>(tape.Variable(4.0));
    REQUIRE_THROWS_AS(tape.Gradient(x), std::logic_error);
}

TEST_CASE("automatic differentiation preserves fused primitive semantics", "[Sora][Math][Autodiff][Fma]") {
    using namespace Sora::Math;

    const double epsilon = std::numeric_limits<double>::epsilon();
    const double aValue = 1.0 + epsilon;
    const double bValue = 1.0 - epsilon;
    constexpr double cValue = -1.0;
    const double fusedPrimal = std::fma(aValue, bValue, cValue);
    REQUIRE(fusedPrimal != aValue * bValue + cValue);

    const auto forward = Fma(Dual{aValue, 1.0}, Dual{bValue, 0.0}, Dual{cValue, 0.0});
    REQUIRE(forward.primal == fusedPrimal);
    REQUIRE(forward.tangent == bValue);

    constexpr double aTangent = 0.25;
    constexpr double bTangent = -0.5;
    constexpr double cTangent = 0.75;
    const auto fullyActive = Fma(Dual{aValue, aTangent}, Dual{bValue, bTangent}, Dual{cValue, cTangent});
    const double fusedTangent = std::fma(aTangent, bValue, std::fma(aValue, bTangent, cTangent));
    REQUIRE(fullyActive.tangent == fusedTangent);

    ReverseTape<double, 5> tape;
    const auto a = tape.Variable(aValue);
    const auto b = tape.Variable(bValue);
    const auto c = tape.Variable(cValue);
    const auto output = Fma(a, b, c);
    REQUIRE(output.primal == fusedPrimal);
    REQUIRE(tape.Size() == 5);
    tape.Backward(output);
    REQUIRE(tape.Gradient(a) == bValue);
    REQUIRE(tape.Gradient(b) == aValue);
    REQUIRE(tape.Gradient(c) == 1.0);

    ReverseTape<double, 2> compressedTape;
    const auto variable = compressedTape.Variable(aValue);
    const auto compressed = Fma(variable, bValue, cValue);
    REQUIRE(compressed.primal == fusedPrimal);
    REQUIRE(compressedTape.Size() == 2);
}

TEST_CASE("batch executor vectorizes multiple inputs and masks the tail", "[Sora][Math][Batch]") {
    const std::array<float, 7> left{1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F};
    const std::array<float, 7> right{2.0F, 2.0F, 2.0F, 2.0F, 2.0F, 2.0F, 2.0F};
    std::array<float, 7> output{};

    Sora::Math::TransformBatchNative<float>(
        std::span(output), [](auto x, auto y) noexcept { return Sora::Math::Fma(x, y, 1.0F); },
        std::span<const float>(left), std::span<const float>(right));

    REQUIRE(output == std::array<float, 7>{3.0F, 5.0F, 7.0F, 9.0F, 11.0F, 13.0F, 15.0F});

    constexpr std::size_t count = 39;
    std::array<float, count> largeLeft{};
    std::array<float, count> largeRight{};
    std::array<float, count> unroll1{};
    std::array<float, count> unroll2{};
    std::array<float, count> unroll4{};
    for (std::size_t i = 0; i < count; ++i) {
        largeLeft[i] = static_cast<float>(i) * 0.25F;
        largeRight[i] = 2.0F + static_cast<float>(i % 3);
    }

    const auto run = [&]<std::size_t Unroll>(std::array<float, count>& destination) {
        Sora::Math::TransformBatch<Float4, Unroll>(
            std::span<float>(destination), [](auto x, auto y) noexcept { return Sora::Math::Fma(x, y, 1.0F); },
            std::span<const float>(largeLeft), std::span<const float>(largeRight));
    };
    run.template operator()<1>(unroll1);
    run.template operator()<2>(unroll2);
    run.template operator()<4>(unroll4);

    REQUIRE(unroll1 == unroll2);
    REQUIRE(unroll1 == unroll4);
    for (std::size_t i = 0; i < count; ++i) {
        REQUIRE(unroll4[i] == std::fma(largeLeft[i], largeRight[i], 1.0F));
    }
}

TEST_CASE("batched JVP combines automatic differentiation with SIMD tail handling", "[Sora][Math][Batch][Autodiff]") {
    constexpr std::size_t count = 7;
    const std::array<float, count> x{0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F, 0.7F};
    const std::array<float, count> y{1.0F, 1.1F, 1.2F, 1.3F, 1.4F, 1.5F, 1.6F};
    const std::array<float, count> dx{1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    const std::array<float, count> dy{0.5F, 0.5F, 0.5F, 0.5F, 0.5F, 0.5F, 0.5F};
    std::array<float, count> primal{};
    std::array<float, count> tangent{};

    Sora::Math::TransformBatchJvpNative<float>(
        std::span(primal), std::span(tangent),
        [](auto xValue, auto yValue) noexcept {
            return Sora::Math::Add(Sora::Math::Mul(xValue, yValue), Sora::Math::Sin(xValue));
        },
        Sora::Math::JvpInput<float>{x, dx}, Sora::Math::JvpInput<float>{y, dy});

    for (std::size_t i = 0; i < count; ++i) {
        REQUIRE(std::abs(primal[i] - (x[i] * y[i] + std::sin(x[i]))) < 1e-6F);
        REQUIRE(std::abs(tangent[i] - (dx[i] * y[i] + x[i] * dy[i] + std::cos(x[i]) * dx[i])) < 1e-6F);
    }

    constexpr std::size_t largeCount = 39;
    std::array<float, largeCount> largeX{};
    std::array<float, largeCount> largeDx{};
    std::array<float, largeCount> referencePrimal{};
    std::array<float, largeCount> referenceTangent{};
    std::array<float, largeCount> candidatePrimal{};
    std::array<float, largeCount> candidateTangent{};
    for (std::size_t i = 0; i < largeCount; ++i) {
        largeX[i] = 0.01F * static_cast<float>(i + 1);
        largeDx[i] = 0.5F + 0.01F * static_cast<float>(i);
    }

    const auto run = [&]<std::size_t Unroll>(std::array<float, largeCount>& primalDestination,
                                             std::array<float, largeCount>& tangentDestination) {
        Sora::Math::TransformBatchJvp<Float4, Unroll>(
            std::span<float>(primalDestination), std::span<float>(tangentDestination),
            [](auto value) noexcept { return Sora::Math::Add(Sora::Math::Exp(value), Sora::Math::Square(value)); },
            Sora::Math::JvpInput<float>{largeX, largeDx});
    };
    run.template operator()<1>(referencePrimal, referenceTangent);
    run.template operator()<2>(candidatePrimal, candidateTangent);
    REQUIRE(candidatePrimal == referencePrimal);
    REQUIRE(candidateTangent == referenceTangent);
    run.template operator()<4>(candidatePrimal, candidateTangent);
    REQUIRE(candidatePrimal == referencePrimal);
    REQUIRE(candidateTangent == referenceTangent);
}

TEST_CASE("compiled kernels fuse expressions across scalar SIMD JVP and VJP", "[Sora][Math][Compile]") {
    using namespace Sora::Math;

    constexpr auto kernel = Compile<1>([](auto x) { return Add(Exp(x), Square(x)); });
    constexpr auto binary = Compile<2>([](auto x, auto y) { return Sub(Mul(x, y), Sin(x)); });

    const double xValue = 0.75;
    const double scalar = kernel(xValue);
    REQUIRE(scalar == std::fma(xValue, xValue, std::exp(xValue)));
    REQUIRE(binary(1.25, -0.5) == std::fma(1.25, -0.5, -std::sin(1.25)));

    const Float4 simdInput([](int index) { return 0.25F * static_cast<float>(index + 1); });
    const Float4 simdOutput = kernel(simdInput);
    for (int index = 0; index < 4; ++index) {
        REQUIRE(std::abs(simdOutput[index] - std::fma(simdInput[index], simdInput[index],
                                                      std::exp(simdInput[index]))) < 1e-6F);
    }

    const auto jvp = kernel(Dual{xValue, 1.0});
    REQUIRE(jvp.primal == scalar);
    REQUIRE(std::abs(jvp.tangent - (2.0 * xValue + std::exp(xValue))) < 1e-12);

    ReverseTape<double, 8> tape;
    const auto variable = tape.Variable(xValue);
    const auto output = kernel(variable);
    tape.Backward(output);
    REQUIRE(output.primal == scalar);
    REQUIRE(std::abs(tape.Gradient(variable) - (2.0 * xValue + std::exp(xValue))) < 1e-12);

    constexpr auto fusedSquare = Compile<1>([](auto x) { return Add(Square(Exp(x)), 1.0); });
    ReverseTape<double, 8> squareTape;
    const auto squareVariable = squareTape.Variable(xValue);
    (void)fusedSquare(squareVariable);
    REQUIRE(squareTape.Size() == 3);
}

TEST_CASE("compiled multiply-add has explicit fused rounding semantics", "[Sora][Math][Compile][Fma]") {
    using namespace Sora::Math;

    constexpr auto kernel = Compile<3>([](auto a, auto b, auto c) { return Add(Mul(a, b), c); });
    const double epsilon = std::numeric_limits<double>::epsilon();
    const double a = 1.0 + epsilon;
    const double b = 1.0 - epsilon;
    constexpr double c = -1.0;

    REQUIRE(kernel(a, b, c) == std::fma(a, b, c));
    REQUIRE(kernel(a, b, c) != a * b + c);
}

TEST_CASE("compiled backend stages every public primitive", "[Sora][Math][Compile][Surface]") {
    using namespace Sora::Math;

    constexpr auto kernel = Compile<1>([](auto x) {
        const auto shifted = Add(x, 2.0);
        const auto half = Div(x, 2.0);
        return Add(Neg(x), Inv(shifted), Square(x), Abs(x), Sin(x), Cos(x), Tan(half), Asin(half), Acos(half),
                   Atan(x), Atan2(x, shifted), Exp(half), Log(shifted), Sqrt(shifted), Pow(shifted, x),
                   Fma(x, x, 1.0), Mfs(x, shifted, 0.5), Nms(x, shifted, 0.5), Nma(x, shifted, 0.5),
                   Lerp(x, 2.0, 0.3), Clamp(x, -1.0, 1.0), Saturate(x), Sign(x), Sub(shifted, x), Mul(x, half));
    });

    const auto eager = [](double x) {
        const auto shifted = Add(x, 2.0);
        const auto half = Div(x, 2.0);
        return Add(Neg(x), Inv(shifted), Square(x), Abs(x), Sin(x), Cos(x), Tan(half), Asin(half), Acos(half),
                   Atan(x), Atan2(x, shifted), Exp(half), Log(shifted), Sqrt(shifted), Pow(shifted, x),
                   Fma(x, x, 1.0), Mfs(x, shifted, 0.5), Nms(x, shifted, 0.5), Nma(x, shifted, 0.5),
                   Lerp(x, 2.0, 0.3), Clamp(x, -1.0, 1.0), Saturate(x), Sign(x), Sub(shifted, x), Mul(x, half));
    };

    constexpr double value = 0.25;
    REQUIRE(std::abs(kernel(value) - eager(value)) < 1e-12);
}

TEST_CASE("masked backend memory operations never touch inactive lanes", "[Sora][Math][Backend][Mask]") {
    const Float4::MaskType firstLane([](int index) { return index == 0; });
    const std::array<float, 1> source{42.0F};
    const Float4 loaded = Float4Backend::MaskedLoad<Float4>(source.data(), firstLane, Float4(-1.0F));

    REQUIRE(loaded[0] == 42.0F);
    REQUIRE(loaded[1] == -1.0F);
    REQUIRE(loaded[3] == -1.0F);

    std::array<float, 1> destination{0.0F};
    Float4Backend::MaskedStore<Float4>(destination.data(), Float4(7.0F), firstLane);
    REQUIRE(destination[0] == 7.0F);
}

TEST_CASE("scalar and SIMD primitives agree on IEEE exceptional values", "[Sora][Math][IEEE754]") {
    using namespace Sora::Math;

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const Float4 values([&](int index) { return index == 0 ? nan : (index == 1 ? -0.0F : 2.0F); });
    const Float4 result = Clamp(values, 0.0F, 1.0F);

    REQUIRE(std::isnan(Clamp(nan, 0.0F, 1.0F)));
    REQUIRE(std::isnan(result[0]));
    REQUIRE(std::signbit(Clamp(-0.0F, 0.0F, 1.0F)));
    REQUIRE(std::signbit(result[1]));
    REQUIRE(result[2] == 1.0F);
    REQUIRE(std::isnan(Sqrt(-1.0F)));
    REQUIRE(std::isnan(Asin(2.0F)));
    REQUIRE(Pow(-2.0F, 3.0F) == -8.0F);

    const Int4 signedValues([](int index) { return index == 0 ? std::numeric_limits<int>::min() : -index; });
    const UInt4 magnitudes = Abs(signedValues);
    REQUIRE(magnitudes[0] == 1U + unsigned(std::numeric_limits<int>::max()));
    REQUIRE(magnitudes[3] == 3U);
}

TEST_CASE("heterogeneous scalar and SIMD operands join into one carrier", "[Sora][Math][Backend][Join]") {
    using namespace Sora::Math;
    using Double4 = Simd::Vector<double, 4>;

    const Float4 values(2.0F);
    const auto sum = Add(1.0, values);
    const auto fused = Fma(values, 3.0, 1);

    STATIC_REQUIRE(std::same_as<std::remove_cvref_t<decltype(sum)>, Double4>);
    STATIC_REQUIRE(std::same_as<std::remove_cvref_t<decltype(fused)>, Double4>);
    REQUIRE(sum[0] == 3.0);
    REQUIRE(fused[3] == 7.0);
}

TEST_CASE("complex SIMD operations expose a complete mathematical surface", "[Sora][Math][Simd][Complex]") {
    using Complex2 = Simd::Vector<std::complex<float>, 2>;
    const Complex2 numerator([](int index) { return std::complex<float>(index + 1.0F, index + 2.0F); });
    const Complex2 denominator(std::complex<float>(2.0F, -1.0F));
    const Complex2 quotient = numerator / denominator;
    const auto magnitude = Simd::Abs(numerator);
    const auto argument = Simd::Arg(numerator);
    const auto projection = Simd::Proj(numerator);

    for (int index = 0; index < 2; ++index) {
        const std::complex<float> expected = numerator[index] / denominator[index];
        REQUIRE(std::abs(quotient[index] - expected) < 1e-6F);
        REQUIRE(std::abs(magnitude[index] - std::abs(numerator[index])) < 1e-6F);
        REQUIRE(std::abs(argument[index] - std::arg(numerator[index])) < 1e-6F);
        REQUIRE(projection[index] == std::proj(numerator[index]));
    }

    const float large = std::numeric_limits<float>::max() / 4.0F;
    const Complex2 largeValue(std::complex<float>(large, large));
    const Complex2 largeDivisor(std::complex<float>(large, -large));
    const Complex2 stableQuotient = largeValue / largeDivisor;
    const auto stableMagnitude = Simd::Abs(largeValue);
    REQUIRE(std::abs(stableQuotient[0] - std::complex<float>(0.0F, 1.0F)) < 1e-6F);
    REQUIRE(std::isfinite(stableMagnitude[0]));
    REQUIRE(std::abs(stableMagnitude[0] - std::abs(largeValue[0])) / stableMagnitude[0] < 1e-6F);

    const float infinity = std::numeric_limits<float>::infinity();
    const Complex2 exceptional([&](int index) {
        return index == 0 ? std::complex<float>(infinity, -2.0F)
                          : std::complex<float>(std::numeric_limits<float>::quiet_NaN(), 3.0F);
    });
    const Complex2 exceptionalProjection = Simd::Proj(exceptional);
    REQUIRE(std::isinf(exceptionalProjection[0].real()));
    REQUIRE(exceptionalProjection[0].imag() == 0.0F);
    REQUIRE(std::signbit(exceptionalProjection[0].imag()));
    REQUIRE(std::isnan(exceptionalProjection[1].real()));
    REQUIRE(exceptionalProjection[1].imag() == 3.0F);
}

TEST_CASE("JVP and VJP satisfy directional duality", "[Sora][Math][Autodiff]") {
    using namespace Sora::Math;

    constexpr double xValue = 1.25;
    constexpr double yValue = -0.75;
    constexpr double xDirection = 0.4;
    constexpr double yDirection = -0.2;
    const auto jvp = Add(Mul(Dual{xValue, xDirection}, Dual{yValue, yDirection}), Sin(Dual{xValue, xDirection}));

    ReverseTape<double, 16> tape;
    const auto x = tape.Variable(xValue);
    const auto y = tape.Variable(yValue);
    const auto output = Add(Mul(x, y), Sin(x));
    tape.Backward(output);
    const double vjpDotDirection = tape.Gradient(x) * xDirection + tape.Gradient(y) * yDirection;

    REQUIRE(std::abs(jvp.tangent - vjpDotDirection) < 1e-12);
}

TEST_CASE("automatic differentiation primitive matrix agrees with finite differences", "[Sora][Math][Autodiff]") {
    using namespace Sora::Math;

    const auto expression = [](const auto& x) {
        const auto shifted = Add(x, 2.0);
        const auto half = Div(x, 2.0);
        return Add(Sin(x), Cos(x), Tan(half), Asin(half), Acos(half), Atan(x), Atan2(x, shifted), Exp(half),
                   Log(shifted), Sqrt(shifted), Pow(shifted, x), Div(1.0, shifted), Fma(x, x, 1.0),
                   Mfs(x, shifted, 0.5), Nms(x, shifted, 0.5), Nma(x, shifted, 0.5), Lerp(x, 2.0, 0.3),
                   Clamp(x, -1.0, 1.0), Saturate(x), Abs(x), Sign(x));
    };

    constexpr double value = 0.25;
    constexpr double step = 1e-6;
    const auto jvp = expression(Dual{value, 1.0});
    const double finiteDifference = (expression(value + step) - expression(value - step)) / (2.0 * step);

    ReverseTape<double, 256> tape;
    const auto variable = tape.Variable(value);
    const auto output = expression(variable);
    tape.Backward(output);

    REQUIRE(std::abs(jvp.tangent - tape.Gradient(variable)) < 1e-11);
    REQUIRE(std::abs(jvp.tangent - finiteDifference) < 1e-7);
}

TEST_CASE("fixed SIMD CPU backend preserves lane-wise arithmetic", "[Sora][Math][Backend]") {
    const Float4 a([](int index) { return static_cast<float>(index + 1); });
    const Float4 b(2.0F);
    const Float4 c(1.0F);

    REQUIRE(Float4Backend::Add(a, b, c)[3] == 7.0F);
    REQUIRE(Float4Backend::Mul(a, b, c)[2] == 6.0F);
    REQUIRE(Float4Backend::Sub(a, b)[0] == -1.0F);
    REQUIRE(Float4Backend::Div(a, b)[3] == 2.0F);
    REQUIRE(Float4Backend::Inv(b)[0] == 0.5F);
    REQUIRE(Float4Backend::Square(a)[2] == 9.0F);
    REQUIRE(Float4Backend::Abs(Float4Backend::Neg(a))[1] == 2.0F);
    REQUIRE(Float4Backend::Clamp(a, Float4(2.0F), Float4(3.0F))[3] == 3.0F);
    REQUIRE(Float4Backend::Saturate(Float4(-0.5F))[0] == 0.0F);
    REQUIRE(Float4Backend::Lerp(c, b, Float4(0.25F))[0] == 1.25F);
    REQUIRE(Float4Backend::Fma(a, b, c)[2] == 7.0F);
    REQUIRE(Float4Backend::Mfs(a, b, c)[2] == 5.0F);
    REQUIRE(Float4Backend::Nms(a, b, c)[2] == -7.0F);
    REQUIRE(Float4Backend::Nma(a, b, c)[2] == -5.0F);
    REQUIRE(Float4Backend::Sign(Float4([](int index) { return static_cast<float>(index - 1); }))[0] == -1.0F);
}

TEST_CASE("fixed SIMD CPU backend applies transcendental functions lane-wise", "[Sora][Math][Backend]") {
    const Float4 values([](int index) { return static_cast<float>(index); });
    const Float4 units(1.0F);

    REQUIRE(Float4Backend::Sin(values)[0] == 0.0F);
    REQUIRE(Float4Backend::Cos(values)[0] == 1.0F);
    REQUIRE(Float4Backend::Exp(values)[0] == 1.0F);
    REQUIRE(Float4Backend::Log(units)[0] == 0.0F);
    REQUIRE(Float4Backend::Pow(Float4(2.0F), Float4(3.0F))[0] == 8.0F);
    REQUIRE(Float4Backend::Sqrt(Float4(4.0F))[0] == 2.0F);
    REQUIRE(Float4Backend::Atan(values)[0] == 0.0F);
    REQUIRE(Float4Backend::Atan2(values, units)[0] == 0.0F);
    REQUIRE(Float4Backend::Asin(values)[0] == 0.0F);
    REQUIRE(Float4Backend::Acos(units)[0] == 0.0F);
    REQUIRE(Float4Backend::Tan(values)[0] == 0.0F);
}

TEST_CASE("SIMD vectors preserve arithmetic and mask semantics", "[Sora][Math][Simd]") {
    const Float4 left([](int index) { return static_cast<float>(index + 1); });
    const Float4 right(2.0F);
    const Float4 sum = left + right;
    const Float4 product = left * right;
    const auto mask = sum > Float4(4.0F);

    REQUIRE(sum[0] == 3.0F);
    REQUIRE(sum[3] == 6.0F);
    REQUIRE(product[2] == 6.0F);
    REQUIRE(Simd::AnyOf(mask));
    REQUIRE_FALSE(Simd::AllOf(mask));
    REQUIRE(Simd::ReduceCount(mask) == 2);
}

TEST_CASE("SIMD reductions and algorithms preserve lane ordering", "[Sora][Math][Simd]") {
    const Float4 left([](int index) { return static_cast<float>(index + 1); });
    const Float4 right(2.0F);
    const Float4 sum = left + right;

    REQUIRE(Simd::Reduce(sum) == 18.0F);
    REQUIRE(Simd::ReduceMin(sum) == 3.0F);
    REQUIRE(Simd::ReduceMax(sum) == 6.0F);

    const Float4 minimum = Simd::Min(left, right);
    const Float4 maximum = Simd::Max(left, right);
    const auto [minimum2, maximum2] = Simd::Minmax(left, right);
    const Float4 clamped = Simd::Clamp(left, Float4(2.0F), Float4(3.0F));

    REQUIRE(minimum[3] == 2.0F);
    REQUIRE(maximum[3] == 4.0F);
    REQUIRE(minimum2[0] == 1.0F);
    REQUIRE(maximum2[3] == 4.0F);
    REQUIRE(clamped[0] == 2.0F);
    REQUIRE(clamped[3] == 3.0F);
}

TEST_CASE("SIMD load, store, bit operations, and type transforms agree with scalar values", "[Sora][Math][Simd]") {
    const std::array<float, 4> source{10.0F, 20.0F, 30.0F, 40.0F};
    const Float4 loaded = Simd::UncheckedLoad<Float4>(source);
    std::array<float, 4> destination{};
    Simd::UncheckedStore(loaded, destination);
    REQUIRE(destination == source);

    const UInt4 bits([](int index) { return std::uint32_t{1} << index; });
    const UInt4 swapped = Simd::Byteswap(bits);
    const auto widths = Simd::BitWidth(bits);
    const auto counts = Simd::Popcount(bits);
    REQUIRE(swapped[0] == 0x01000000U);
    REQUIRE(widths[3] == 4);
    REQUIRE(counts[2] == 1);

    const Int4 integers([](int index) { return index - 2; });
    const auto shifted = integers << 1;
    REQUIRE(shifted[0] == -4);
    REQUIRE(shifted[3] == 2);

    using Double4 = Simd::Rebind<double, Float4>;
    using Float2 = Simd::Resize<2, Float4>;
    STATIC_REQUIRE(std::same_as<Double4, Simd::Vector<double, 4>>);
    STATIC_REQUIRE(std::same_as<Float2, Simd::Vector<float, 2>>);
}

TEST_CASE("SIMD complex vectors preserve component and conjugation semantics", "[Sora][Math][Simd]") {
    using Complex2 = Simd::Vector<std::complex<float>, 2>;
    const Complex2 values([](int index) { return std::complex<float>(index + 1.0F, index + 2.0F); });
    const auto real = Simd::Real(values);
    const auto conjugate = Simd::Conj(values);

    REQUIRE(real[1] == 2.0F);
    REQUIRE(conjugate[0] == std::complex<float>(1.0F, -2.0F));
}

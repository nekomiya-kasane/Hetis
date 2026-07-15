#include <Sora/Math/PrimaryFunctions.h>
#include <Sora/Math/Simd.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <complex>
#include <cstdint>
#include <type_traits>

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
    static_assert(!std::invocable<decltype(Sora::Math::Add), float, double>);

    constexpr auto kSquaredSum = Sora::Compose(Sora::Math::Square, Sora::Math::Add);
    static_assert(kSquaredSum(2, 3) == 25);
    static_assert(kSquaredSum(PrimaryFunctionTest::Number{2}, PrimaryFunctionTest::Number{3}).value == 25);
    static_assert(std::same_as<Sora::Traits::BackendTypeOf<PrimaryFunctionTest::Number>,
                               PrimaryFunctionTest::Backend>);
    static_assert(std::is_trivially_copyable_v<decltype(kSquaredSum)>);
    static_assert(noexcept(kSquaredSum(2, 3)));
    static_assert(!std::invocable<decltype(Sora::Math::Sin), PrimaryFunctionTest::Number>);

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

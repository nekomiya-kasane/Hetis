#include <Sora/Math/PrimaryFunctions.h>

#include <concepts>
#include <print>

using namespace Sora::Math;

namespace Demo {

    struct Number {
        float value;
    };

    struct NumberBackend {
        [[nodiscard]] static constexpr Number Add(Number left, Number right) noexcept {
            return Number{left.value + right.value};
        }

        [[nodiscard]] static constexpr Number Square(Number value) noexcept {
            return Number{value.value * value.value};
        }
    };

} // namespace Demo

template<>
struct Sora::Math::Hook::Backend<Demo::Number> {
    using Type = Demo::NumberBackend;
};

int main() {
    const float x = 0.5F;
    const float y = 2.0F;

    static_assert(std::same_as<decltype(Add(x, y)), float>);

    const float sum = Add(x, y);
    const float difference = Sub(x, y);
    const float product = Mul(x, y);
    const float quotient = Div(x, y);
    const float sine = Sin(x);
    const float cosine = Cos(x);
    const float tangent = Tan(x);
    const float arcsine = Asin(x);

    std::println("Sum: {}, Difference: {}, Product: {}, Quotient: {}, Sine: {}, Cosine: {}, Tangent: {}, Arcsine: {}",
                 sum, difference, product, quotient, sine, cosine, tangent, arcsine);

    using Float4 = Sora::Simd::F32<4>;
    const Float4 vectorX([](int lane) { return 0.25F * static_cast<float>(lane + 1); });
    const Float4 vectorY(2.0F);
    const Float4 vectorResult = Fma(Sin(vectorX), vectorY, Float4(1.0F));

    static_assert(std::same_as<decltype(Add(vectorX, vectorY)), Float4>);
    std::println("SIMD Fma(Sin(x), 2, 1): [{}, {}, {}, {}]", vectorResult[0], vectorResult[1], vectorResult[2],
                 vectorResult[3]);

    constexpr auto squaredSum = Sora::Compose(Square, Add);
    static_assert(squaredSum(2.0F, 3.0F) == 25.0F);
    static_assert(std::is_trivially_copyable_v<decltype(squaredSum)>);

    const Float4 vectorSquaredSum = squaredSum(vectorX, vectorY);
    const Demo::Number customSquaredSum = squaredSum(Demo::Number{2.0F}, Demo::Number{3.0F});
    std::println("Composed Square(Add): scalar={}, SIMD lane 0={}, custom={}", squaredSum(x, y),
                 vectorSquaredSum[0], customSquaredSum.value);
}

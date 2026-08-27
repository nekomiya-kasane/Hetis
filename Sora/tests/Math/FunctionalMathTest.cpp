#include <Sora/Math/FunctionalMath.h>
#include <Sora/Math/PrimaryFunctions.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

    struct Number {
        int value;
        constexpr bool operator==(const Number&) const = default;
    };

    struct AddNumbers {
        [[nodiscard]] constexpr Number operator()(const Number& left, const Number& right) const noexcept {
            return Number{left.value + right.value};
        }
    };

    constexpr auto kAdd = [](const float& left, const float& right) noexcept { return left + right; };
    static_assert(noexcept(Sora::Math::MapInto(
        std::declval<std::span<float>>(), kAdd, std::declval<std::span<const float>>(),
        std::declval<std::span<const float>>())));

} // namespace

TEST_CASE("Listable preserves scalar calls and performs strict one-level lazy threading",
          "[Sora][Math][Functional][Listable]") {
    auto add = Sora::Math::Listable(Sora::Math::Add);
    REQUIRE(add(2.0F, 3.0F) == 5.0F);

    std::array<float, 3> left{1.0F, 2.0F, 3.0F};
    std::array<float, 3> right{10.0F, 20.0F, 30.0F};
    auto sums = add(left, right);
    REQUIRE(std::ranges::equal(sums, std::array{11.0F, 22.0F, 33.0F}));

    auto shifted = add(left, 5.0F);
    REQUIRE(std::ranges::equal(shifted, std::array{6.0F, 7.0F, 8.0F}));
}

TEST_CASE("Listable owns temporary ranges scalars and move-only callables", "[Sora][Math][Functional][Ownership]") {
    auto scaled = Sora::Math::Listable(
        [factor = std::make_unique<float>(3.0F)](float value, float offset) {
            return value * *factor + offset;
        })(std::vector<float>{1.0F, 2.0F, 3.0F}, 1.0F);

    REQUIRE(std::ranges::equal(scaled, std::array{4.0F, 7.0F, 10.0F}));
}

TEST_CASE("MapInto lifts Math CPOs to preferred SIMD batches and masks the tail", "[Sora][Math][Functional]") {
    const std::array<float, 7> left{1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F};
    const std::array<float, 7> right{10.0F, 20.0F, 30.0F, 40.0F, 50.0F, 60.0F, 70.0F};
    std::array<float, 7> output{};

    Sora::Math::MapInto(std::span(output), Sora::Math::Add,
                        std::span<const float>(left), std::span<const float>(right));

    REQUIRE(output == std::array<float, 7>{11.0F, 22.0F, 33.0F, 44.0F, 55.0F, 66.0F, 77.0F});
}

TEST_CASE("MapInto covers empty unaligned full-pack and tail boundaries", "[Sora][Math][Functional][Boundary]") {
    using V = Sora::Math::Simd::BasicVector<float, Sora::Math::Simd::PreferredAbiT<float>>;
    constexpr size_t width = V::kSize.value;
    const std::array counts{size_t{0}, size_t{1}, width - 1, width, width + 1, 4 * width + 1};

    for (size_t count : counts) {
        std::vector<float> left(count + 1, -1.0F);
        std::vector<float> right(count + 1, -1.0F);
        std::vector<float> output(count + 1, -1.0F);
        for (size_t index = 0; index < count; ++index) {
            left[index + 1] = static_cast<float>(index);
            right[index + 1] = static_cast<float>(2 * index);
        }

        Sora::Math::MapInto(
            std::span<float>{output.data() + 1, count}, Sora::Math::Add,
            std::span<const float>{left.data() + 1, count}, std::span<const float>{right.data() + 1, count});

        REQUIRE(output.front() == -1.0F);
        for (size_t index = 0; index < count; ++index) {
            REQUIRE(output[index + 1] == static_cast<float>(3 * index));
        }
    }
}

TEST_CASE("MapInto permits exact in-place updates", "[Sora][Math][Functional][Aliasing]") {
    std::array<float, 5> values{1.0F, 2.0F, 3.0F, 4.0F, 5.0F};
    const std::array<float, 5> increments{10.0F, 20.0F, 30.0F, 40.0F, 50.0F};

    Sora::Math::MapInto(std::span(values), Sora::Math::Add,
                        std::span<const float>(values), std::span<const float>(increments));

    REQUIRE(values == std::array<float, 5>{11.0F, 22.0F, 33.0F, 44.0F, 55.0F});
}

TEST_CASE("MapInto composes bound scalars without a second broadcast abstraction",
          "[Sora][Math][Functional][Broadcast]") {
    const std::array<float, 5> input{1.0F, 2.0F, 3.0F, 4.0F, 5.0F};
    std::array<float, 5> output{};
    auto addTen = Sora::BindBack(Sora::Math::Add, 10.0F);

    Sora::Math::MapInto(std::span(output), addTen, std::span<const float>(input));

    REQUIRE(output == std::array<float, 5>{11.0F, 12.0F, 13.0F, 14.0F, 15.0F});
}

TEST_CASE("MapInto falls back to the direct scalar loop when no SIMD carrier exists",
          "[Sora][Math][Functional][Scalar]") {
    const std::array left{Number{1}, Number{2}, Number{3}};
    const std::array right{Number{10}, Number{20}, Number{30}};
    std::array<Number, 3> output{};

    Sora::Math::MapInto(std::span(output), AddNumbers{},
                        std::span<const Number>(left), std::span<const Number>(right));

    REQUIRE(output == std::array{Number{11}, Number{22}, Number{33}});
}

TEST_CASE("MapInto preserves throwing scalar callable semantics", "[Sora][Math][Functional][Scalar]") {
    const std::array<float, 3> input{1.0F, 2.0F, 3.0F};
    std::array<float, 3> output{};
    size_t calls = 0;
    auto function = [&](const float& value) {
        ++calls;
        return value * 2.0F;
    };

    Sora::Math::MapInto(std::span(output), function, std::span<const float>(input));

    REQUIRE(output == std::array<float, 3>{2.0F, 4.0F, 6.0F});
    REQUIRE(calls == input.size());
}

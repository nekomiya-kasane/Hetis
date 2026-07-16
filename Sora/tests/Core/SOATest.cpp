#include "Sora/Core/SOA.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <concepts>
#include <string>
#include <type_traits>

namespace {

    struct Particle {
        float x;
        float y;
        float life;
    };

    struct WithSkip {
        int kept;
        [[=Sora::SoA::$::Skip{}]] int skipped;
        double other;
    };

    struct WithString {
        int id;
        std::string name;
    };

    struct WithAlignment {
        [[=Sora::SoA::$::Align{32}]] float x;
        float y;
    };

    struct BaseState {
        float inherited;
    };

    struct DerivedState : BaseState {
        float direct;
    };

    struct Kinematic {
        float position;
        float velocity;
        float mass;

        [[nodiscard]] constexpr Kinematic Step(float deltaTime) const noexcept {
            return {std::fma(velocity, deltaTime, position), velocity, mass};
        }

        friend constexpr Kinematic operator+(const Kinematic& left, const Kinematic& right) noexcept {
            return {left.position + right.position, left.velocity + right.velocity, left.mass + right.mass};
        }

        friend constexpr Kinematic operator+(Kinematic value) noexcept {
            return value;
        }

        friend constexpr Kinematic operator-(Kinematic value) noexcept {
            return {-value.position, -value.velocity, -value.mass};
        }

        friend constexpr Kinematic operator*(Kinematic value, float scale) noexcept {
            return {value.position * scale, value.velocity * scale, value.mass * scale};
        }

        friend constexpr Kinematic operator*(float scale, Kinematic value) noexcept {
            return value * scale;
        }
    };

    [[nodiscard]] constexpr Kinematic Integrate(Kinematic state, Kinematic acceleration) noexcept {
        return {std::fma(state.velocity, 2.0F, state.position), state.velocity + acceleration.velocity,
                state.mass + acceleration.mass};
    }

    [[nodiscard]] constexpr float Speed(Kinematic state) noexcept {
        return state.velocity;
    }

    struct Accelerate {
        float gain;

        [[nodiscard]] constexpr Kinematic operator()(Kinematic state, Kinematic acceleration) const noexcept {
            state.velocity = std::fma(acceleration.velocity, gain, state.velocity);
            return state;
        }

        constexpr bool operator==(const Accelerate&) const = default;
    };

    inline constexpr auto ShiftPosition = [](Kinematic state, float shift) constexpr noexcept -> Kinematic {
        state.position += shift;
        return state;
    };

} // namespace

consteval { Sora::SoA::Define<Particle, 4>(); }
consteval { Sora::SoA::Define<WithSkip, 2>(); }
consteval { Sora::SoA::Define<Kinematic, 16>(); }

TEST_CASE("SoAType generates reflected array fields", "[Sora.Core.SOA]") {
    using ParticleSoA = Sora::SoA::SoAType<Particle, 4>;
    using SkippedSoA = Sora::SoA::SoAType<WithSkip, 2>;

    STATIC_REQUIRE(std::is_same_v<decltype(ParticleSoA{}.x), std::array<float, 4>>);
    STATIC_REQUIRE(std::is_same_v<decltype(ParticleSoA{}.y), std::array<float, 4>>);
    STATIC_REQUIRE(std::is_same_v<decltype(ParticleSoA{}.life), std::array<float, 4>>);
    STATIC_REQUIRE(std::is_same_v<decltype(SkippedSoA{}.kept), std::array<int, 2>>);
    STATIC_REQUIRE(std::is_same_v<decltype(SkippedSoA{}.other), std::array<double, 2>>);
    STATIC_REQUIRE_FALSE(Sora::SoA::Concept::SoATransformable<DerivedState>);
    STATIC_REQUIRE_FALSE(Sora::SoA::Concept::SoATransformable<float[4]>);

    Sora::SoA::Array<WithAlignment> aligned;
    aligned.Resize(1);
    REQUIRE(reinterpret_cast<std::uintptr_t>(aligned.Field<Sora::FixedString{"x"}>().data()) % 32 == 0);
}

TEST_CASE("SoA Array supports push, named access, field spans, and gather", "[Sora.Core.SOA]") {
    Sora::SoA::Array<Particle> particles;
    particles.Push(Particle{1.0f, 2.0f, 3.0f});
    particles.Push(Particle{4.0f, 5.0f, 6.0f});

    REQUIRE(particles.Size() == 2);
    REQUIRE(particles.Get<Sora::FixedString{"x"}>(1) == 4.0f);

    auto xs = particles.Field<Sora::FixedString{"x"}>();
    REQUIRE(xs.size() == 2);
    xs[0] = 10.0f;

    Particle gathered = particles.Gather(0);
    REQUIRE(gathered.x == 10.0f);
    REQUIRE(gathered.y == 2.0f);
    REQUIRE(gathered.life == 3.0f);
}

TEST_CASE("SoA Array respects Skip annotation", "[Sora.Core.SOA]") {
    Sora::SoA::Array<WithSkip> skipped;
    STATIC_REQUIRE(decltype(skipped)::FieldCount == 2);

    skipped.Push(WithSkip{7, 99, 2.5});
    REQUIRE(skipped.Get<Sora::FixedString{"kept"}>(0) == 7);
    REQUIRE(skipped.Get<Sora::FixedString{"other"}>(0) == 2.5);
}

TEST_CASE("SoA Array handles non-trivial fields and swap-remove", "[Sora.Core.SOA]") {
    Sora::SoA::Array<WithString> strings;
    strings.Push(WithString{1, "alpha"});
    strings.Push(WithString{2, "beta"});

    strings.SwapRemove(0);
    REQUIRE(strings.Size() == 1);
    REQUIRE(strings.Get<Sora::FixedString{"id"}>(0) == 2);
    REQUIRE(strings.Get<Sora::FixedString{"name"}>(0) == "beta");

    strings.Clear();
    REQUIRE(strings.Empty());
}

TEST_CASE("SoA adapts an original n-ary function directly to its generated static type", "[Sora.Core.SOA][SIMD]") {
    using SA = Sora::SoA::SoAType<Kinematic, 16>;
    STATIC_REQUIRE(Sora::SoA::Concept::OperationLiftable<Kinematic>);
    STATIC_REQUIRE_FALSE(Sora::SoA::Concept::OperationLiftable<WithSkip>);
    STATIC_REQUIRE(std::is_aggregate_v<SA>);
    STATIC_REQUIRE(sizeof(SA) == 3 * 16 * sizeof(float));
    STATIC_REQUIRE_FALSE(std::invocable<decltype(Sora::SoA::Adapt<&Speed>), const SA&>);

    SA state{};
    SA acceleration{};
    for (std::size_t i = 0; i < 16; ++i) {
        state.position[i] = static_cast<float>(i);
        state.velocity[i] = 1.0F + static_cast<float>(i);
        state.mass[i] = 2.0F;
        acceleration.position[i] = 0.0F;
        acceleration.velocity[i] = 0.5F;
        acceleration.mass[i] = 3.0F;
    }

    const SA output = Sora::SoA::Adapt<&Integrate>(state, acceleration);
    for (std::size_t i = 0; i < 16; ++i) {
        REQUIRE(output.position[i] == std::fma(state.velocity[i], 2.0F, state.position[i]));
        REQUIRE(output.velocity[i] == state.velocity[i] + 0.5F);
        REQUIRE(output.mass[i] == 5.0F);
    }
}

TEST_CASE("SoA lifts the original aggregate operator without a parallel pack type", "[Sora.Core.SOA][Operator]") {
    using SA = Sora::SoA::SoAType<Kinematic, 16>;
    SA left{};
    SA right{};
    left.position.fill(1.0F);
    left.velocity.fill(2.0F);
    left.mass.fill(3.0F);
    right.position.fill(4.0F);
    right.velocity.fill(5.0F);
    right.mass.fill(6.0F);

    const SA output = left + right;
    REQUIRE(output.position[7] == 5.0F);
    REQUIRE(output.velocity[7] == 7.0F);
    REQUIRE(output.mass[7] == 9.0F);
}

TEST_CASE("SoA operators preserve source unary and scalar-broadcast semantics", "[Sora.Core.SOA][Operator]") {
    using SA = Sora::SoA::SoAType<Kinematic, 16>;
    SA state{};
    state.position.fill(1.0F);
    state.velocity.fill(2.0F);
    state.mass.fill(3.0F);

    const SA positive = +state;
    const SA negative = -state;
    const SA rightScaled = state * 2.0F;
    const SA leftScaled = 3.0F * state;

    REQUIRE(positive.velocity[5] == 2.0F);
    REQUIRE(negative.velocity[5] == -2.0F);
    REQUIRE(rightScaled.mass[5] == 6.0F);
    REQUIRE(leftScaled.position[5] == 3.0F);
}

TEST_CASE("SoA adapts a member function and broadcasts ordinary parameters", "[Sora.Core.SOA][Member]") {
    using SA = Sora::SoA::SoAType<Kinematic, 16>;
    SA state{};
    for (std::size_t i = 0; i < 16; ++i) {
        state.position[i] = static_cast<float>(i);
        state.velocity[i] = 2.0F;
        state.mass[i] = 3.0F;
    }

    const SA output = Sora::SoA::Adapt<&Kinematic::Step>(state, 0.5F);
    REQUIRE(output.position[9] == 10.0F);
    REQUIRE(output.velocity[9] == 2.0F);
    REQUIRE(output.mass[9] == 3.0F);
    STATIC_REQUIRE(sizeof(Sora::SoA::Adapt<&Kinematic::Step>) == 1);
}

TEST_CASE("SoA adapts structural function objects and non-capturing lambdas", "[Sora.Core.SOA][Callable]") {
    using SA = Sora::SoA::SoAType<Kinematic, 16>;
    SA state{};
    SA acceleration{};
    state.position.fill(1.0F);
    state.velocity.fill(2.0F);
    state.mass.fill(3.0F);
    acceleration.velocity.fill(0.5F);

    const SA accelerated = Sora::SoA::Adapt<Accelerate{4.0F}>(state, acceleration);
    const SA shifted = Sora::SoA::Adapt<ShiftPosition>(accelerated, 3.0F);

    REQUIRE(accelerated.velocity[11] == 4.0F);
    REQUIRE(shifted.position[11] == 4.0F);
    STATIC_REQUIRE(sizeof(Sora::SoA::Adapt<Accelerate{4.0F}>) == 1);
}

TEST_CASE("SoA Transform accepts runtime callables and permits alias-safe output", "[Sora.Core.SOA][Callable]") {
    using SA = Sora::SoA::SoAType<Kinematic, 16>;
    SA state{};
    state.position.fill(1.0F);
    state.velocity.fill(2.0F);
    state.mass.fill(3.0F);

    const float gain = 2.5F;
    const SA scaled = Sora::SoA::Transform(
        [gain](Kinematic value) constexpr noexcept -> Kinematic { return value * gain; }, state);
    Sora::SoA::TransformTo(state, ShiftPosition, state, 4.0F);

    REQUIRE(scaled.velocity[3] == 5.0F);
    REQUIRE(state.position[3] == 5.0F);
}

TEST_CASE("SoA scalar and SIMD execution policies preserve identical element semantics", "[Sora.Core.SOA][SIMD]") {
    using SA = Sora::SoA::SoAType<Kinematic, 16>;
    SA state{};
    SA acceleration{};
    for (std::size_t index = 0; index < 16; ++index) {
        state.position[index] = static_cast<float>(index);
        state.velocity[index] = static_cast<float>(index) * 0.25F;
        state.mass[index] = 2.0F;
        acceleration.velocity[index] = 0.5F;
        acceleration.mass[index] = 1.0F;
    }

    SA scalar{};
    SA simd{};
    Sora::SoA::Adapt<&Integrate>.ScalarTo(scalar, state, acceleration);
    Sora::SoA::Adapt<&Integrate>.SimdTo(simd, state, acceleration);

    REQUIRE(scalar.position == simd.position);
    REQUIRE(scalar.velocity == simd.velocity);
    REQUIRE(scalar.mass == simd.mass);
}

#include "Sora/Core/SOA.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
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
        [[=Sora::SoA::Skip{}]] int skipped;
        double other;
    };

    struct WithString {
        int id;
        std::string name;
    };

} // namespace

consteval { Sora::SoA::Define<Particle, 4>(); }
consteval { Sora::SoA::Define<WithSkip, 2>(); }

TEST_CASE("SoAType generates reflected array fields", "[Sora.Core.SOA]") {
    using ParticleSoA = Sora::SoA::SoAType<Particle, 4>;
    using SkippedSoA = Sora::SoA::SoAType<WithSkip, 2>;

    STATIC_REQUIRE(std::is_same_v<decltype(ParticleSoA{}.x), std::array<float, 4>>);
    STATIC_REQUIRE(std::is_same_v<decltype(ParticleSoA{}.y), std::array<float, 4>>);
    STATIC_REQUIRE(std::is_same_v<decltype(ParticleSoA{}.life), std::array<float, 4>>);
    STATIC_REQUIRE(std::is_same_v<decltype(SkippedSoA{}.kept), std::array<int, 2>>);
    STATIC_REQUIRE(std::is_same_v<decltype(SkippedSoA{}.other), std::array<double, 2>>);
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

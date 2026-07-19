/**
 * @file SOABenchmark.cpp
 * @brief Compare scalar AoS, scalar SoA, and compiler-vectorized SoA execution of one source operation.
 */

#include <Sora/Core/SOA.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <memory>
#include <print>
#include <span>

namespace {

    struct Particle {
        float position;
        float velocity;
        float acceleration;
        float damping;
        float phase;
        float frequency;
        float temperature;
        float source;
    };

    [[nodiscard, gnu::always_inline]] Particle Step(Particle value, float deltaTime) noexcept {
        const float attenuation = std::exp(-value.damping * deltaTime);
        const float phase = std::fma(value.frequency, deltaTime, value.phase);
        const float drive = std::sin(phase) * 0.01F;
        const float velocity = std::fma(value.acceleration, deltaTime, value.velocity) * attenuation;
        const float temperature = std::fma(value.source, deltaTime, value.temperature);
        return {std::fma(velocity + drive, deltaTime, value.position), velocity, value.acceleration, value.damping,
                phase, value.frequency, temperature, value.source};
    }

    inline constexpr size_t kElementCount = 1U << 18U;
    inline constexpr size_t kSampleCount = 11;
    inline constexpr size_t kRepetitionsPerSample = 4;
    inline constexpr float kDeltaTime = 1.0F / 120.0F;

} // namespace

consteval { Sora::SoA::Define<Particle, kElementCount>(); }

namespace {

    using ParticleSoA = Sora::SoA::SoAType<Particle, kElementCount>;

    [[gnu::noinline]] void RunAoS(std::span<Particle> output, std::span<const Particle> input) noexcept {
#pragma clang loop vectorize(disable) interleave(disable)
        for (size_t index = 0; index < input.size(); ++index) {
            output[index] = Step(input[index], kDeltaTime);
        }
    }

    [[gnu::noinline]] void RunSoAScalar(ParticleSoA& output, const ParticleSoA& input) noexcept {
        Sora::SoA::Adapt<&Step>.ScalarTo(output, input, kDeltaTime);
    }

    [[gnu::noinline]] void RunSoASimd(ParticleSoA& output, const ParticleSoA& input) noexcept {
        Sora::SoA::Adapt<&Step>.SimdTo(output, input, kDeltaTime);
    }

    [[gnu::noinline]] void RunSoAManualSimd(ParticleSoA& output, const ParticleSoA& input) noexcept {
#pragma clang loop vectorize(enable) interleave(enable)
        for (size_t index = 0; index < kElementCount; ++index) {
            const float positionInput = input.position[index];
            const float velocityInput = input.velocity[index];
            const float accelerationInput = input.acceleration[index];
            const float dampingInput = input.damping[index];
            const float phaseInput = input.phase[index];
            const float frequencyInput = input.frequency[index];
            const float temperatureInput = input.temperature[index];
            const float sourceInput = input.source[index];
            const float attenuation = std::exp(-dampingInput * kDeltaTime);
            const float phase = std::fma(frequencyInput, kDeltaTime, phaseInput);
            const float drive = std::sin(phase) * 0.01F;
            const float velocity = std::fma(accelerationInput, kDeltaTime, velocityInput) * attenuation;
            output.position[index] = std::fma(velocity + drive, kDeltaTime, positionInput);
            output.velocity[index] = velocity;
            output.acceleration[index] = accelerationInput;
            output.damping[index] = dampingInput;
            output.phase[index] = phase;
            output.frequency[index] = frequencyInput;
            output.temperature[index] = std::fma(sourceInput, kDeltaTime, temperatureInput);
            output.source[index] = sourceInput;
        }
    }

    template<typename Function>
    [[nodiscard]] double MedianNanosecondsPerElement(Function&& function) {
        std::array<double, kSampleCount> samples{};
        for (size_t warmup = 0; warmup < 3; ++warmup) {
            function();
        }

        for (double& sample : samples) {
            const auto begin = std::chrono::steady_clock::now();
            for (size_t repetition = 0; repetition < kRepetitionsPerSample; ++repetition) {
                function();
            }
            const auto end = std::chrono::steady_clock::now();
            sample = std::chrono::duration<double, std::nano>(end - begin).count() /
                     static_cast<double>(kElementCount * kRepetitionsPerSample);
        }

        std::ranges::sort(samples);
        return samples[samples.size() / 2];
    }

    struct SimdPairTiming {
        double adapted;
        double manual;
        double ratio;
    };

    template<typename Adapted, typename Manual>
    [[nodiscard]] SimdPairTiming MedianSimdPair(Adapted&& adapted, Manual&& manual) {
        std::array<double, kSampleCount> adaptedSamples{};
        std::array<double, kSampleCount> manualSamples{};
        std::array<double, kSampleCount> ratioSamples{};
        for (size_t warmup = 0; warmup < 3; ++warmup) {
            adapted();
            manual();
        }

        const auto measure = [](auto&& function) {
            const auto begin = std::chrono::steady_clock::now();
            for (size_t repetition = 0; repetition < kRepetitionsPerSample; ++repetition) {
                function();
            }
            const auto end = std::chrono::steady_clock::now();
            return std::chrono::duration<double, std::nano>(end - begin).count() /
                   static_cast<double>(kElementCount * kRepetitionsPerSample);
        };

        for (size_t sample = 0; sample < kSampleCount; ++sample) {
            if (sample % 2 == 0) {
                adaptedSamples[sample] = measure(adapted);
                manualSamples[sample] = measure(manual);
            } else {
                manualSamples[sample] = measure(manual);
                adaptedSamples[sample] = measure(adapted);
            }
            ratioSamples[sample] = adaptedSamples[sample] / manualSamples[sample];
        }

        std::ranges::sort(adaptedSamples);
        std::ranges::sort(manualSamples);
        std::ranges::sort(ratioSamples);
        return {adaptedSamples[adaptedSamples.size() / 2], manualSamples[manualSamples.size() / 2],
                ratioSamples[ratioSamples.size() / 2]};
    }

    [[nodiscard]] float RelativeError(float expected, float actual) noexcept {
        const float scale = std::max({1.0F, std::abs(expected), std::abs(actual)});
        return std::abs(expected - actual) / scale;
    }

    [[nodiscard]] float MaxRelativeError(std::span<const Particle> expected, const ParticleSoA& actual) {
        float result = 0.0F;
        for (size_t index = 0; index < expected.size(); ++index) {
            const Particle value = Sora::SoA::Gather(actual, index);
            result = std::max({result, RelativeError(expected[index].position, value.position),
                               RelativeError(expected[index].velocity, value.velocity),
                               RelativeError(expected[index].acceleration, value.acceleration),
                               RelativeError(expected[index].damping, value.damping),
                               RelativeError(expected[index].phase, value.phase),
                               RelativeError(expected[index].frequency, value.frequency),
                               RelativeError(expected[index].temperature, value.temperature),
                               RelativeError(expected[index].source, value.source)});
        }
        return result;
    }

} // namespace

int main() {
    auto aosInput = std::make_unique<Particle[]>(kElementCount);
    auto aosOutput = std::make_unique<Particle[]>(kElementCount);
    auto soaInput = std::make_unique<ParticleSoA>();
    auto soaScalarOutput = std::make_unique<ParticleSoA>();
    auto soaSimdOutput = std::make_unique<ParticleSoA>();
    auto soaManualSimdOutput = std::make_unique<ParticleSoA>();

    for (size_t index = 0; index < kElementCount; ++index) {
        const float unit = static_cast<float>(index % 1024U) / 1024.0F;
        const Particle value{unit * 10.0F, 1.0F + unit, 0.25F + unit * 0.5F, 0.05F + unit * 0.1F,
                             unit,         0.5F + unit, 290.0F + unit * 20.0F, 0.1F + unit * 0.2F};
        aosInput[index] = value;
        Sora::SoA::Scatter(*soaInput, index, value);
    }

    RunAoS({aosOutput.get(), kElementCount}, {aosInput.get(), kElementCount});
    RunSoAScalar(*soaScalarOutput, *soaInput);
    RunSoASimd(*soaSimdOutput, *soaInput);
    RunSoAManualSimd(*soaManualSimdOutput, *soaInput);

    const float scalarError = MaxRelativeError({aosOutput.get(), kElementCount}, *soaScalarOutput);
    const float simdError = MaxRelativeError({aosOutput.get(), kElementCount}, *soaSimdOutput);
    const float manualSimdError = MaxRelativeError({aosOutput.get(), kElementCount}, *soaManualSimdOutput);
    if (scalarError != 0.0F || simdError > 2.0e-6F || manualSimdError > 2.0e-6F) {
        std::println("result mismatch: scalar={}, adapted SIMD={}, manual SIMD={}", scalarError, simdError,
                     manualSimdError);
        return 2;
    }

    const double aosTime = MedianNanosecondsPerElement(
        [&] { RunAoS({aosOutput.get(), kElementCount}, {aosInput.get(), kElementCount}); });
    const double soaScalarTime =
        MedianNanosecondsPerElement([&] { RunSoAScalar(*soaScalarOutput, *soaInput); });
    const SimdPairTiming simdTiming =
        MedianSimdPair([&] { RunSoASimd(*soaSimdOutput, *soaInput); },
                       [&] { RunSoAManualSimd(*soaManualSimdOutput, *soaInput); });

    const double checksum = static_cast<double>(soaSimdOutput->position[kElementCount / 3]) +
                            static_cast<double>(soaSimdOutput->temperature[kElementCount / 2]);
    std::println("AOS scalar : {:.3f} ns/element", aosTime);
    std::println("SOA scalar : {:.3f} ns/element", soaScalarTime);
    std::println("SOA SIMD adapted: {:.3f} ns/element", simdTiming.adapted);
    std::println("SOA SIMD manual : {:.3f} ns/element", simdTiming.manual);
    std::println("SIMD speedup over AOS scalar: {:.2f}x", aosTime / simdTiming.adapted);
    std::println("SIMD speedup over SOA scalar: {:.2f}x", soaScalarTime / simdTiming.adapted);
    std::println("adapted/manual SIMD paired ratio: {:.3f}", simdTiming.ratio);
    std::println("maximum relative error: {:.3e}; checksum: {:.6f}", simdError, checksum);
}

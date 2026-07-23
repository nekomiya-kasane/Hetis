/**
 * @file SOABenchmark.cpp
 * @brief Compare sample-loop and vectorized execution of one reflection-lifted particle operation.
 */

#include <BenchmarkHarness.h>
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
        return {std::fma(velocity + drive, deltaTime, value.position),
                velocity,
                value.acceleration,
                value.damping,
                phase,
                value.frequency,
                temperature,
                value.source};
    }

    inline constexpr size_t kElementCount = 1U << 18U;
    inline constexpr size_t kSampleCount = 11;
    inline constexpr auto kMinimumSampleTime = std::chrono::milliseconds(100);
    inline constexpr float kDeltaTime = 1.0F / 120.0F;

} // namespace

consteval {
    Sora::SoA::Define<Particle, kElementCount>();
}

namespace {

    using ParticleSoA = Sora::SoA::SoAType<Particle, kElementCount>;

    [[gnu::noinline]] void RunAoSSampleLoop(std::span<Particle> output, std::span<const Particle> input) noexcept {
#pragma clang loop vectorize(disable) interleave(disable)
        for (size_t index = 0; index < input.size(); ++index) {
            output[index] = Step(input[index], kDeltaTime);
        }
    }

    [[gnu::noinline]] void RunSoASampleLoop(ParticleSoA& output, const ParticleSoA& input) noexcept {
        Sora::SoA::Adapt<&Step>.ScalarTo(output, input, kDeltaTime);
    }

    [[gnu::noinline]] void RunSoAAdapted(ParticleSoA& output, const ParticleSoA& input) noexcept {
        Sora::SoA::Adapt<&Step>.SimdTo(output, input, kDeltaTime);
    }

    [[gnu::noinline]] void RunSoAManual(ParticleSoA& output, const ParticleSoA& input) noexcept {
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
    auto soaSampleOutput = std::make_unique<ParticleSoA>();
    auto soaAdaptedOutput = std::make_unique<ParticleSoA>();
    auto soaManualOutput = std::make_unique<ParticleSoA>();

    for (size_t index = 0; index < kElementCount; ++index) {
        const float unit = static_cast<float>(index % 1024U) / 1024.0F;
        const Particle value{unit * 10.0F, 1.0F + unit, 0.25F + unit * 0.5F,   0.05F + unit * 0.1F,
                             unit,         0.5F + unit, 290.0F + unit * 20.0F, 0.1F + unit * 0.2F};
        aosInput[index] = value;
        Sora::SoA::Scatter(*soaInput, index, value);
    }

    RunAoSSampleLoop({aosOutput.get(), kElementCount}, {aosInput.get(), kElementCount});
    RunSoASampleLoop(*soaSampleOutput, *soaInput);
    RunSoAAdapted(*soaAdaptedOutput, *soaInput);
    RunSoAManual(*soaManualOutput, *soaInput);

    const float sampleError = MaxRelativeError({aosOutput.get(), kElementCount}, *soaSampleOutput);
    const float adaptedError = MaxRelativeError({aosOutput.get(), kElementCount}, *soaAdaptedOutput);
    const float manualError = MaxRelativeError({aosOutput.get(), kElementCount}, *soaManualOutput);
    const float maximumError = std::max({sampleError, adaptedError, manualError});
    if (sampleError != 0.0F || maximumError > 2.0e-6F) {
        std::println("result mismatch: sample={}, adapted={}, manual={}", sampleError, adaptedError, manualError);
        return 2;
    }

    auto runAos = [&]() noexcept {
        RunAoSSampleLoop({aosOutput.get(), kElementCount}, {aosInput.get(), kElementCount});
    };
    auto runSoaSample = [&]() noexcept { RunSoASampleLoop(*soaSampleOutput, *soaInput); };
    auto runSoaAdapted = [&]() noexcept { RunSoAAdapted(*soaAdaptedOutput, *soaInput); };
    auto runSoaManual = [&]() noexcept { RunSoAManual(*soaManualOutput, *soaInput); };
    const std::array cases{
        Sora::Benchmark::MakeCase("particle_aos_sample_loop", runAos),
        Sora::Benchmark::MakeCase("particle_soa_sample_loop", runSoaSample),
        Sora::Benchmark::MakeCase("particle_soa_adapted", runSoaAdapted),
        Sora::Benchmark::MakeCase("particle_soa_manual", runSoaManual),
    };
    const auto timing = Sora::Benchmark::MeasureInterleaved<kSampleCount>(cases, kElementCount, kMinimumSampleTime);

    const double checksum = static_cast<double>(soaAdaptedOutput->position[kElementCount / 3]) +
                            static_cast<double>(soaAdaptedOutput->temperature[kElementCount / 2]);
    std::println("metric,value,unit");
    std::println("particle_aos_sample_loop,{:.6f},ns/element", timing.MedianTime(0));
    std::println("particle_soa_sample_loop,{:.6f},ns/element", timing.MedianTime(1));
    std::println("particle_soa_adapted,{:.6f},ns/element", timing.MedianTime(2));
    std::println("particle_soa_manual,{:.6f},ns/element", timing.MedianTime(3));
    std::println("adapted_speedup_over_aos,{:.6f},ratio", timing.PairedRatio(0, 2));
    std::println("adapted_speedup_over_sample_loop,{:.6f},ratio", timing.PairedRatio(1, 2));
    std::println("adapted_manual_paired_ratio,{:.6f},ratio", timing.PairedRatio(2, 3));
    std::println("measurement_repetitions,{},count", timing.repetitions);
    std::println("maximum_relative_error,{:.9e},ratio", maximumError);
    std::println("checksum,{:.9f},value", checksum);
}

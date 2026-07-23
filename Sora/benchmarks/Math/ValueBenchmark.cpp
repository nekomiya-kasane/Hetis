/**
 * @file ValueBenchmark.cpp
 * @brief Controlled baselines for canonical values, SoA execution, Eigen interop, and runtime dispatch.
 */

#include <BenchmarkHarness.h>
#include <Sora/Math/EigenAdaptor.h>
#include <Sora/Math/KernelContext.h>
#include <Sora/Math/PortableCompile.h>
#include <Sora/Math/Types.h>
#include <Sora/Math/ValueBatch.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <memory>
#include <meta>
#include <print>
#include <ranges>
#include <span>

namespace {

    namespace EigenInterop = Sora::Math::Interop::Eigen;
    constexpr size_t kCount = 1U << 18U;
    constexpr size_t kSamples = 11;
    constexpr size_t kSimdUnroll = 4;
    constexpr auto kMinimumSampleTime = std::chrono::milliseconds(100);

    template<typename Value>
    [[nodiscard, gnu::always_inline]] constexpr Value Step(const Value& value) noexcept {
        using T = typename Value::CarrierType;
        const Sora::Math::Mat<T, 3, 3> matrix{T(1.125F),  T(-0.25F), T(0.5F),  T(0.125F), T(0.875F),
                                              T(-0.375F), T(-0.5F),  T(0.25F), T(1.25F)};
        return matrix * value + Sora::Math::Vec<T, 3>{T(0.25F), T(-0.5F), T(1.0F)};
    }

    [[gnu::noinline]] void RunAoSSampleLoop(std::span<Sora::Math::Vec3f> output,
                                            std::span<const Sora::Math::Vec3f> input) noexcept {
#pragma clang loop vectorize(disable) interleave(disable)
        for (size_t i = 0; i < input.size(); ++i) {
            output[i] = Step(input[i]);
        }
    }

    using ConstBatch = Sora::Math::VectorBatchView<const float, 3>;
    using MutableBatch = Sora::Math::VectorBatchView<float, 3>;
    using PreferredPack = Sora::Math::Simd::BasicVector<float, Sora::Math::Simd::PreferredAbiT<float>>;

    [[gnu::noinline]] void RunSoASampleLoop(MutableBatch output, ConstBatch input) noexcept {
#pragma clang loop vectorize(disable) interleave(disable)
        for (size_t i = 0; i < input.Size(); ++i) {
            output.Store(i, Step(input.Load(i)));
        }
    }

    [[gnu::noinline]] void RunSoAAuto(MutableBatch output, ConstBatch input) noexcept {
#pragma clang loop vectorize(enable) interleave(enable)
        for (size_t i = 0; i < input.Size(); ++i) {
            output.Store(i, Step(input.Load(i)));
        }
    }

    [[gnu::noinline]] void RunSoASimd(MutableBatch output, ConstBatch input) noexcept {
        Sora::Math::TransformBatch(output, [](auto value) noexcept { return Step(value); }, input);
    }

    template<Sora::Math::Simd::SimdVecType V>
    [[nodiscard, gnu::always_inline]] Sora::Math::Vec<V, 3> LoadManual(ConstBatch input, size_t offset) noexcept {
        Sora::Math::Vec<V, 3> result;
        template for (constexpr size_t coordinate : std::define_static_array(std::views::iota(size_t{0}, size_t{3}))) {
            result[coordinate] =
                Sora::Math::Simd::UncheckedLoad<V>(input.Coordinate(coordinate).subspan(offset, V::kSize.value));
        }
        return result;
    }

    template<Sora::Math::Simd::SimdVecType V>
    [[gnu::always_inline]] void StoreManual(MutableBatch output, size_t offset,
                                            const Sora::Math::Vec<V, 3>& value) noexcept {
        template for (constexpr size_t coordinate : std::define_static_array(std::views::iota(size_t{0}, size_t{3}))) {
            Sora::Math::Simd::UncheckedStore(value[coordinate],
                                             output.Coordinate(coordinate).subspan(offset, V::kSize.value));
        }
    }

    [[gnu::noinline]] void RunSoAManualSimd(MutableBatch output, ConstBatch input) noexcept {
        constexpr size_t width = PreferredPack::kSize.value;
        constexpr size_t blockWidth = kSimdUnroll * width;
        size_t offset = 0;
        for (; output.Size() - offset >= blockWidth; offset += blockWidth) {
            template for (constexpr size_t batch : std::define_static_array(std::views::iota(size_t{0}, kSimdUnroll))) {
                const size_t batchOffset = offset + batch * width;
                StoreManual(output, batchOffset, Step(LoadManual<PreferredPack>(input, batchOffset)));
            }
        }
        for (; output.Size() - offset >= width; offset += width) {
            StoreManual(output, offset, Step(LoadManual<PreferredPack>(input, offset)));
        }
        for (; offset < output.Size(); ++offset) {
            output.Store(offset, Step(input.Load(offset)));
        }
    }

    [[gnu::noinline]] void RunEigenAoSSampleLoop(std::span<Sora::Math::Vec3f> output,
                                                 std::span<const Sora::Math::Vec3f> input) noexcept {
        const ::Eigen::Matrix3f matrix =
            (::Eigen::Matrix3f() << 1.125F, -0.25F, 0.5F, 0.125F, 0.875F, -0.375F, -0.5F, 0.25F, 1.25F).finished();
        const ::Eigen::Vector3f bias{0.25F, -0.5F, 1.0F};
#pragma clang loop vectorize(disable) interleave(disable)
        for (size_t i = 0; i < input.size(); ++i) {
            auto destination = EigenInterop::AsEigen(output[i]);
            destination.noalias() = matrix * EigenInterop::AsEigen(input[i]) + bias;
        }
    }

    [[nodiscard]] float Error(float expected, float actual) noexcept {
        return std::abs(expected - actual) / std::max({1.0F, std::abs(expected), std::abs(actual)});
    }

    [[nodiscard]] float MaxError(std::span<const Sora::Math::Vec3f> expected, ConstBatch actual) noexcept {
        float result = 0.0F;
        for (size_t i = 0; i < expected.size(); ++i) {
            const auto value = actual.Load(i);
            for (size_t coordinate = 0; coordinate < 3; ++coordinate) {
                result = std::max(result, Error(expected[i][coordinate], value[coordinate]));
            }
        }
        return result;
    }

    [[nodiscard]] float MaxError(std::span<const Sora::Math::Vec3f> expected,
                                 std::span<const Sora::Math::Vec3f> actual) noexcept {
        float result = 0.0F;
        for (size_t i = 0; i < expected.size(); ++i) {
            for (size_t coordinate = 0; coordinate < 3; ++coordinate) {
                result = std::max(result, Error(expected[i][coordinate], actual[i][coordinate]));
            }
        }
        return result;
    }

    inline constexpr auto kCompiledScale =
        Sora::Math::Compile<2>([](auto value, auto scale) { return Sora::Math::Mul(value, scale); });
    inline constexpr auto kPortableScale =
        Sora::Math::CompilePortable<float, 2>([](auto value, auto scale) { return Sora::Math::Mul(value, scale); });

    struct ScaleState {
        float scale;
    };

    [[gnu::noinline]] void ScaleKernel(void* opaque, const Sora::Math::KernelInvocation& invocation) noexcept {
        const auto scale = static_cast<const ScaleState*>(opaque)->scale;
        const auto* input = static_cast<const float*>(invocation.Inputs()[0].data);
        auto* output = static_cast<float*>(invocation.Outputs()[0].data);
        for (size_t i = 0; i < invocation.workItemCount; ++i) {
            output[invocation.workItemOffset + i] = input[invocation.workItemOffset + i] * scale;
        }
    }

    [[gnu::noinline]] void RunDirect(ScaleState& state, const Sora::Math::KernelInvocation& invocation) noexcept {
        ScaleKernel(&state, invocation);
    }
    [[gnu::noinline]] void RunCompiledMath(std::span<float> output, std::span<const float> input,
                                           float scale) noexcept {
        for (size_t i = 0; i < input.size(); ++i) {
            output[i] = kCompiledScale(input[i], scale);
        }
    }

    [[gnu::noinline]] void RunPortableDirect(std::span<float> output, std::span<const float> input,
                                             float scale) noexcept {
        for (size_t i = 0; i < input.size(); ++i) {
            output[i] = kPortableScale(input[i], scale);
        }
    }

    [[gnu::noinline]] void RunResolved(const Sora::Math::KernelExecutable& executable,
                                       const Sora::Math::KernelInvocation& invocation) noexcept {
        executable(invocation);
    }

    Sora::Math::KernelCompileResult CompileCached(void* state, const Sora::Math::KernelRequest&,
                                                  Sora::Math::KernelFeatureMask,
                                                  const Sora::Math::KernelPolicy&) noexcept {
        return {.entry = ScaleKernel,
                .state = state,
                .requiredFeatures = Sora::Math::KernelFeature::Scalar,
                .error = {},
                .deterministic = true};
    }

} // namespace

int main() {
    auto input = std::make_unique<Sora::Math::Vec3f[]>(kCount);
    auto aosOutput = std::make_unique<Sora::Math::Vec3f[]>(kCount);
    auto eigenOutput = std::make_unique<Sora::Math::Vec3f[]>(kCount);
    auto soaInput = std::make_unique<float[]>(3 * kCount);
    auto soaSampleOutput = std::make_unique<float[]>(3 * kCount);
    auto soaAutoOutput = std::make_unique<float[]>(3 * kCount);
    auto soaSimdOutput = std::make_unique<float[]>(3 * kCount);
    auto soaManualOutput = std::make_unique<float[]>(3 * kCount);

    for (size_t i = 0; i < kCount; ++i) {
        const float unit = static_cast<float>(i % 1024U) / 1024.0F;
        input[i] = {unit * 4.0F - 2.0F, unit * unit + 0.25F, 1.0F - unit};
        for (size_t coordinate = 0; coordinate < 3; ++coordinate) {
            soaInput[coordinate * kCount + i] = input[i][coordinate];
        }
    }

    const ConstBatch inputBatch(soaInput.get(), kCount);
    const MutableBatch sampleBatch(soaSampleOutput.get(), kCount);
    const MutableBatch autoBatch(soaAutoOutput.get(), kCount);
    const MutableBatch simdBatch(soaSimdOutput.get(), kCount);
    const MutableBatch manualBatch(soaManualOutput.get(), kCount);
    RunAoSSampleLoop({aosOutput.get(), kCount}, {input.get(), kCount});
    RunSoASampleLoop(sampleBatch, inputBatch);
    RunSoAAuto(autoBatch, inputBatch);
    RunSoASimd(simdBatch, inputBatch);
    RunSoAManualSimd(manualBatch, inputBatch);
    RunEigenAoSSampleLoop({eigenOutput.get(), kCount}, {input.get(), kCount});

    const auto expected = std::span<const Sora::Math::Vec3f>{aosOutput.get(), kCount};
    const float sampleError = MaxError(expected, ConstBatch(soaSampleOutput.get(), kCount));
    const float autoError = MaxError(expected, ConstBatch(soaAutoOutput.get(), kCount));
    const float simdError = MaxError(expected, ConstBatch(soaSimdOutput.get(), kCount));
    const float manualError = MaxError(expected, ConstBatch(soaManualOutput.get(), kCount));
    const float eigenError = MaxError(expected, {eigenOutput.get(), kCount});
    const float maximumError = std::max({sampleError, autoError, simdError, manualError, eigenError});
    if (sampleError != 0.0F || maximumError > 2.0e-6F) {
        std::println("result mismatch: sample={}, auto={}, SIMD={}, manual={}, Eigen={}", sampleError, autoError,
                     simdError, manualError, eigenError);
        return 2;
    }

    auto runAos = [&]() noexcept { RunAoSSampleLoop({aosOutput.get(), kCount}, {input.get(), kCount}); };
    auto runSoaSample = [&]() noexcept { RunSoASampleLoop(sampleBatch, inputBatch); };
    auto runSoaAuto = [&]() noexcept { RunSoAAuto(autoBatch, inputBatch); };
    auto runSoaSimd = [&]() noexcept { RunSoASimd(simdBatch, inputBatch); };
    auto runSoaManual = [&]() noexcept { RunSoAManualSimd(manualBatch, inputBatch); };
    auto runEigen = [&]() noexcept { RunEigenAoSSampleLoop({eigenOutput.get(), kCount}, {input.get(), kCount}); };
    const std::array valueCases{
        Sora::Benchmark::MakeCase("vec3f_aos_sample_loop", runAos),
        Sora::Benchmark::MakeCase("vec3f_soa_sample_loop", runSoaSample),
        Sora::Benchmark::MakeCase("vec3f_soa_auto", runSoaAuto),
        Sora::Benchmark::MakeCase("vec3f_soa_explicit_simd", runSoaSimd),
        Sora::Benchmark::MakeCase("vec3f_soa_manual_simd", runSoaManual),
        Sora::Benchmark::MakeCase("eigen_vec3f_aos_sample_loop", runEigen),
    };
    const auto valueTiming = Sora::Benchmark::MeasureInterleaved<kSamples>(valueCases, kCount, kMinimumSampleTime);

    auto scaleInput = std::make_unique<float[]>(kCount);
    auto scaleOutput = std::make_unique<float[]>(kCount);
    std::ranges::fill(std::span{scaleInput.get(), kCount}, 1.25F);
    ScaleState scale{1.75F};
    Sora::Math::KernelInput kernelInput{scaleInput.get(), kCount, sizeof(float),
                                        Sora::Math::KernelScalarFormat::Float32};
    Sora::Math::KernelOutput kernelOutput{scaleOutput.get(), kCount, sizeof(float),
                                          Sora::Math::KernelScalarFormat::Float32};
    const Sora::Math::KernelInvocation invocation{&kernelInput, 1, &kernelOutput, 1, 0, kCount, nullptr, 0};
    const Sora::Math::KernelRequest staticRequest{
        .familyId = 0x7363616C65, .workItemCount = kCount, .requiredFeatures = Sora::Math::KernelFeature::Scalar};
    const std::array candidates{Sora::Math::KernelCandidate{ScaleKernel, &scale, Sora::Math::KernelFeature::Scalar,
                                                            Sora::Math::KernelBackendKind::Scalar, 0, 1, true}};
    const Sora::Math::KernelContext staticContext{Sora::Math::KernelFeature::Scalar};
    auto staticExecutable = staticContext.Resolve(staticRequest, candidates, {.jit = Sora::Math::JitMode::Disabled});
    if (!staticExecutable) {
        return 3;
    }

    constexpr std::array<std::byte, 4> ir{std::byte{0x53}, std::byte{0x4F}, std::byte{0x52}, std::byte{0x41}};
    const Sora::Math::KernelRequest jitRequest{.familyId = 0x7363616C65,
                                               .semanticHash = 0x12345678,
                                               .irKind = Sora::Math::KernelIrKind::SoraKernelIr,
                                               .irData = ir.data(),
                                               .irSize = ir.size(),
                                               .workItemCount = kCount,
                                               .requiredFeatures = Sora::Math::KernelFeature::Scalar};
    const Sora::Math::KernelContext jitContext{
        Sora::Math::KernelFeature::Scalar,
        Sora::Math::JitProviderRef{&scale, CompileCached, Sora::Math::kKernelAbiVersion}};
    auto jitExecutable = jitContext.Resolve(jitRequest, {}, {.jit = Sora::Math::JitMode::Require});
    if (!jitExecutable) {
        return 4;
    }

    auto runDirect = [&]() noexcept { RunDirect(scale, invocation); };
    auto runCompiled = [&]() noexcept {
        RunCompiledMath({scaleOutput.get(), kCount}, {scaleInput.get(), kCount}, scale.scale);
    };
    auto runPortable = [&]() noexcept {
        RunPortableDirect({scaleOutput.get(), kCount}, {scaleInput.get(), kCount}, scale.scale);
    };
    auto runStatic = [&]() noexcept { RunResolved(*staticExecutable, invocation); };
    auto runJit = [&]() noexcept { RunResolved(*jitExecutable, invocation); };
    const std::array directCases{
        Sora::Benchmark::MakeCase("compiled_math_kernel", runCompiled),
        Sora::Benchmark::MakeCase("portable_direct_kernel", runPortable),
    };
    const auto directTiming = Sora::Benchmark::MeasureInterleaved<kSamples>(directCases, kCount, kMinimumSampleTime);
    const std::array kernelCases{
        Sora::Benchmark::MakeCase("direct_static_kernel", runDirect),
        Sora::Benchmark::MakeCase("resolved_static_kernel", runStatic),
        Sora::Benchmark::MakeCase("cached_jit_executable", runJit),
    };
    const auto kernelTiming = Sora::Benchmark::MeasureInterleaved<kSamples>(kernelCases, kCount, kMinimumSampleTime);
    if (scaleOutput[kCount / 2] != 1.25F * scale.scale) {
        return 5;
    }

    const double checksum =
        soaSimdOutput[kCount / 3] + soaSimdOutput[2 * kCount + kCount / 2] + scaleOutput[kCount / 4];
    std::println("metric,value,unit");
    std::println("vec3f_aos_sample_loop,{:.6f},ns/element", valueTiming.MedianTime(0));
    std::println("vec3f_soa_sample_loop,{:.6f},ns/element", valueTiming.MedianTime(1));
    std::println("vec3f_soa_auto,{:.6f},ns/element", valueTiming.MedianTime(2));
    std::println("vec3f_soa_explicit_simd,{:.6f},ns/element", valueTiming.MedianTime(3));
    std::println("vec3f_soa_manual_simd,{:.6f},ns/element", valueTiming.MedianTime(4));
    std::println("eigen_vec3f_aos_sample_loop,{:.6f},ns/element", valueTiming.MedianTime(5));
    std::println("soa_sample_loop_speedup_over_aos,{:.6f},ratio", valueTiming.PairedRatio(0, 1));
    std::println("soa_auto_speedup_over_sample_loop,{:.6f},ratio", valueTiming.PairedRatio(1, 2));
    std::println("explicit_simd_speedup_over_sample_loop,{:.6f},ratio", valueTiming.PairedRatio(1, 3));
    std::println("explicit_simd_speedup_over_aos,{:.6f},ratio", valueTiming.PairedRatio(0, 3));
    std::println("explicit_simd_speedup_over_soa_auto,{:.6f},ratio", valueTiming.PairedRatio(2, 3));
    std::println("explicit_manual_paired_ratio,{:.6f},ratio", valueTiming.PairedRatio(3, 4));
    std::println("value_measurement_repetitions,{},count", valueTiming.repetitions);
    std::println("direct_static_kernel,{:.6f},ns/element", kernelTiming.MedianTime(0));
    std::println("compiled_math_kernel,{:.6f},ns/element", directTiming.MedianTime(0));
    std::println("portable_direct_kernel,{:.6f},ns/element", directTiming.MedianTime(1));
    std::println("resolved_static_kernel,{:.6f},ns/element", kernelTiming.MedianTime(1));
    std::println("cached_jit_executable,{:.6f},ns/element", kernelTiming.MedianTime(2));
    std::println("portable_direct_over_compiled,{:.6f},ratio", directTiming.PairedRatio(1, 0));
    std::println("static_dispatch_over_direct,{:.6f},ratio", kernelTiming.PairedRatio(1, 0));
    std::println("jit_dispatch_over_direct,{:.6f},ratio", kernelTiming.PairedRatio(2, 0));
    std::println("direct_measurement_repetitions,{},count", directTiming.repetitions);
    std::println("kernel_measurement_repetitions,{},count", kernelTiming.repetitions);
    std::println("maximum_relative_error,{:.9e},ratio", maximumError);
    std::println("checksum,{:.9f},value", checksum);
}

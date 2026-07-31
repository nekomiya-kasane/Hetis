/**
 * @file SystemRuntimeBenchmark.cpp
 * @brief CPU-only automatic runtime discovery and context-construction baselines.
 */

#include <BenchmarkHarness.h>
#include <Sora/Compute/SystemComputeContext.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <print>
#include <utility>

namespace {

    constexpr size_t kSampleCount = 9;
    constexpr auto kMinimumSampleTime = std::chrono::milliseconds(50);

    [[nodiscard]] Sora::Compute::SystemRuntimeConfig CpuOnlyRuntime() {
        return {.cpu = {.workerCount = 1},
                .cpuCode = Sora::Compute::SystemCpuCodePolicy::InterpreterOnly,
                .vulkanPolicy = Sora::Compute::SystemVulkanPolicy::Disabled};
    }

    class SystemRuntimeFixture final {
    public:
        [[nodiscard]] static std::expected<SystemRuntimeFixture, Sora::Compute::RuntimeDiscoveryError>
        Create() noexcept {
            auto runtime = Sora::Compute::SystemRuntimeFactory::Create(CpuOnlyRuntime());
            if (!runtime) {
                return std::unexpected(runtime.error());
            }
            return SystemRuntimeFixture{std::move(*runtime)};
        }

        void Discover() noexcept {
            auto epoch = runtime_.Discover(sequence_++);
            if (!epoch) {
                failed_ = true;
                return;
            }
            checksum_ += epoch->Devices().size() + epoch->Resources().Inventory().Queues().size();
        }

        void CreateContext() noexcept {
            Sora::Compute::SystemComputeContextConfig config;
            config.runtime = CpuOnlyRuntime();
            auto context = Sora::Compute::CreateSystemComputeContext(std::move(config));
            if (!context) {
                failed_ = true;
                return;
            }
            checksum_ += context->Recovery().Current()->Devices().size();
        }

        [[nodiscard]] bool Failed() const noexcept { return failed_; }

        [[nodiscard]] uint64_t Checksum() const noexcept { return checksum_; }

    private:
        explicit SystemRuntimeFixture(Sora::Compute::SystemRuntimeFactory runtime) noexcept
            : runtime_(std::move(runtime)) {}

        Sora::Compute::SystemRuntimeFactory runtime_;
        uint64_t sequence_ = 1;
        uint64_t checksum_ = 0;
        bool failed_ = false;
    };

} // namespace

int main() {
    auto fixture = SystemRuntimeFixture::Create();
    if (!fixture) {
        std::println(stderr, "system runtime construction failed: code={}", std::to_underlying(fixture.error().code));
        return 2;
    }

    auto discover = [&fixture]() noexcept { fixture->Discover(); };
    auto context = [&fixture]() noexcept { fixture->CreateContext(); };
    const std::array cases{
        Sora::Benchmark::MakeCase("cpu_only_discovery", discover),
        Sora::Benchmark::MakeCase("cpu_only_context", context),
    };
    const auto timing = Sora::Benchmark::MeasureInterleavedIndependent<kSampleCount>(cases, 1, kMinimumSampleTime);
    if (fixture->Failed()) {
        return 2;
    }

    std::println("family,configuration,metric,value,unit");
    std::println("system_runtime,cpu_interpreter,discovery,{:.6f},ns/epoch", timing.MedianTime(0));
    std::println("system_runtime,cpu_interpreter,context,{:.6f},ns/context", timing.MedianTime(1));
    std::println("system_runtime,cpu_interpreter,context_over_discovery,{:.6f},ratio", timing.PairedRatio(1, 0));
    std::println("checksum,cpu_interpreter,construction,{},value", fixture->Checksum());
}

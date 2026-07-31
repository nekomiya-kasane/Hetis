/**
 * @file VulkanBackendBenchmark.cpp
 * @brief Vulkan service materialization, device timing, resident dispatch, transfer, and end-to-end baselines.
 */

#include <BenchmarkHarness.h>
#include <Sora/Compute/Backend/VulkanBackend.h>
#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>
#include <Sora/Compute/Runtime/PipelineBindingLayout.h>

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <print>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {

    using namespace std::chrono_literals;

    constexpr Sora::Compute::ValueType kFloat{Sora::Compute::ScalarKind::Float32, 1};
    constexpr size_t kColdSampleCount = 5;
    constexpr size_t kHotSampleCount = 7;
    constexpr auto kMinimumSampleTime = 40ms;
    constexpr uint64_t kElementCount = 1U << 20U;
    constexpr uint64_t kDataBytes = kElementCount * sizeof(float);
    constexpr float kAlpha = 1.75F;

    consteval auto MakeSaxpyProgram() {
        Sora::Compute::KernelBuilder<3, 4> scalar;
        const auto alpha = scalar.AddValueParameter(kFloat);
        const auto x = scalar.AddValueParameter(kFloat);
        const auto y = scalar.AddValueParameter(kFloat);
        auto module = std::move(scalar).Finish(scalar.Ternary(Sora::Compute::OpCode::Fma, alpha, x, y));

        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<4, 3, 4> program{
            domain, module, Sora::Compute::PartitionContract::Elementwise(domain.rank)};
        const auto alphaArgument = program.AddValueArgument(kFloat);
        const auto xArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, domain.rank,
                                                         Sora::Compute::LayoutKind::Contiguous, 64, {1});
        const auto yArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, domain.rank,
                                                         Sora::Compute::LayoutKind::Contiguous, 64, {2});
        const auto output = program.AddBufferArgument(Sora::Compute::AccessMode::Write, kFloat, domain.rank,
                                                      Sora::Compute::LayoutKind::Contiguous, 64, {3});
        program.BindArgument(alphaArgument);
        program.BindArgument(xArgument);
        program.BindArgument(yArgument);
        return std::move(program).Finish(output);
    }

    constexpr auto kSaxpy = MakeSaxpyProgram();

    template<typename T, typename E>
    [[nodiscard]] T Take(std::expected<T, E>&& result, std::string_view stage) {
        if (!result) {
            std::println(stderr, "{} failed: code={}, detail={}", stage, std::to_underlying(result.error().code),
                         result.error().detail);
            std::exit(2);
        }
        return std::move(*result);
    }

    struct MaterializedKernel {
        Sora::Compute::Artifact executable{};
        Sora::Compute::Artifact pipeline{};
    };

    [[nodiscard]] MaterializedKernel Materialize(const Sora::Compute::VulkanBackend& backend) {
        const auto bytes = kSaxpy.Serialize();
        auto executable =
            Take(backend.Compiler().Compile({.moduleData = bytes.Bytes().data(),
                                             .moduleSize = bytes.Bytes().size(),
                                             .moduleHash = kSaxpy.Hash(),
                                             .target = backend.Target(),
                                             .artifactKind = Sora::Compute::ArtifactKind::DeviceExecutable}),
                 "compile SPIR-V executable");
        const Sora::Compute::PipelineStateKey state{
            .state = backend.PipelineStateIdentity(),
            .bindingLayout = Sora::Compute::PipelineBindingLayoutIdentity(kSaxpy.Arguments())};
        auto pipeline = Take(backend.PipelineFactory().CreatePipeline(executable, state), "create Vulkan pipeline");
        return {std::move(executable), std::move(pipeline)};
    }

    [[nodiscard]] Sora::Compute::Allocation Allocate(const Sora::Compute::MemoryAllocator& allocator,
                                                     Sora::Compute::MemoryLocation location,
                                                     Sora::Compute::MemoryPropertyMask properties) {
        return Take(allocator.Allocate({.location = location,
                                        .requiredProperties = properties,
                                        .preferredProperties = properties,
                                        .sizeBytes = kDataBytes,
                                        .alignment = 64}),
                    "allocate Vulkan buffer");
    }

    [[nodiscard]] const Sora::Compute::TransferEngine& FindTransfer(const Sora::Compute::VulkanBackend& backend,
                                                                    Sora::Compute::MemoryLocation source,
                                                                    Sora::Compute::MemoryLocation destination) {
        for (const Sora::Compute::TransferEngine& engine : backend.TransferEngines()) {
            if (engine.Source() == source && engine.Destination() == destination &&
                engine.Kind() == Sora::Compute::TransferKind::Copy) {
                return engine;
            }
        }
        std::println(stderr, "required Vulkan transfer engine is unavailable");
        std::exit(2);
    }

    [[nodiscard]] Sora::Compute::BackendBinding MakeBinding(const Sora::Compute::Allocation& allocation,
                                                            uint32_t binding, Sora::Compute::AccessMode access) {
        return {.allocation = allocation.View(),
                .size = kDataBytes,
                .binding = binding,
                .access = access,
                .kind = Sora::Compute::BackendBindingKind::AffineElements,
                .elementType = kFloat,
                .layout = Sora::Compute::LayoutKind::Contiguous,
                .rank = 1,
                .extents = {kElementCount},
                .byteStrides = {sizeof(float)},
                .componentStrideBytes = sizeof(float)};
    }

    class VulkanWorkload {
    public:
        VulkanWorkload()
            : backend_(Take(Sora::Compute::VulkanBackend::Create(), "create Vulkan backend")),
              materialized_(Materialize(backend_)),
              xHost_(Allocate(backend_.HostAllocator(), backend_.HostLocation(), HostProperties())),
              yHost_(Allocate(backend_.HostAllocator(), backend_.HostLocation(), HostProperties())),
              outputHost_(Allocate(backend_.HostAllocator(), backend_.HostLocation(), HostProperties())),
              xDevice_(Allocate(backend_.DeviceAllocator(), backend_.DeviceLocation(), DeviceProperties())),
              yDevice_(Allocate(backend_.DeviceAllocator(), backend_.DeviceLocation(), DeviceProperties())),
              outputDevice_(Allocate(backend_.DeviceAllocator(), backend_.DeviceLocation(), DeviceProperties())),
              upload_(FindTransfer(backend_, backend_.HostLocation(), backend_.DeviceLocation())),
              download_(FindTransfer(backend_, backend_.DeviceLocation(), backend_.HostLocation())),
              bindings_{MakeBinding(xDevice_, 1, Sora::Compute::AccessMode::Read),
                        MakeBinding(yDevice_, 2, Sora::Compute::AccessMode::Read),
                        MakeBinding(outputDevice_, 3, Sora::Compute::AccessMode::Write)},
              scalarData_(std::bit_cast<std::array<std::byte, sizeof(float)>>(kAlpha)),
              scalarBinding_{.dataOffset = 0, .size = sizeof(float), .binding = 0, .type = kFloat},
              command_{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                       .bindingCount = static_cast<uint32_t>(bindings_.size()),
                       .scalarBindingCount = 1,
                       .artifact = materialized_.pipeline.View(),
                       .workgroupCount = {static_cast<uint32_t>(kElementCount / 64), 1, 1}} {
            InitializeInputs();
            UploadInputs();
            RunResidentDispatch();
            completedEvent_ = SubmitDispatch({});
            RequireTimedDispatch(completedEvent_.WaitUntilTerminal(), "cached event seed");
            ValidateOutput();
        }

        [[nodiscard]] const Sora::Compute::VulkanBackend& Backend() const noexcept { return backend_; }

        void RunResidentDispatch() noexcept {
            auto completion = SubmitDispatch({});
            RequireTimedDispatch(completion.WaitUntilTerminal(), "resident dispatch");
        }

        void QueryCompletedEvent() noexcept { RequireTimedDispatch(completedEvent_.Query(), "completed event query"); }

        [[nodiscard]] uint64_t SampleDeviceExecutionNanoseconds() {
            auto completion = SubmitDispatch({});
            const Sora::Compute::EventObservation observation = completion.WaitUntilTerminal();
            RequireTimedDispatch(observation, "device execution sample");
            return observation.executionNanoseconds;
        }

        void RunTransferRoundTrip() noexcept {
            auto uploadedX = Take(upload_.Submit(xHost_, xDevice_, transferRegions_), "upload x");
            auto uploadedY = Take(upload_.Submit(yHost_, yDevice_, transferRegions_), "upload y");
            std::vector<Sora::Compute::Event> waits{uploadedX.Completion(), uploadedY.Completion()};
            auto downloaded =
                Take(download_.Submit(xDevice_, outputHost_, transferRegions_, std::move(waits)), "download x");
            RequireCompleted(downloaded.WaitUntilTerminal(), "transfer round trip");
        }

        void RunRoundTrip() noexcept {
            auto uploadedX = Take(upload_.Submit(xHost_, xDevice_, transferRegions_), "upload x");
            auto uploadedY = Take(upload_.Submit(yHost_, yDevice_, transferRegions_), "upload y");
            const std::array waits{uploadedX.Completion(), uploadedY.Completion()};
            auto dispatched = SubmitDispatch(waits);
            auto downloaded = Take(download_.Submit(outputDevice_, outputHost_, transferRegions_,
                                                    std::vector<Sora::Compute::Event>{dispatched}),
                                   "download output");
            RequireCompleted(downloaded.WaitUntilTerminal(), "full round trip");
        }

        void ValidateOutput() {
            RunRoundTrip();
            auto bytes = Take(backend_.Map(outputHost_), "map output");
            const auto output = std::span<const float>{reinterpret_cast<const float*>(bytes.data()), kElementCount};
            for (uint64_t index = 0; index < kElementCount; ++index) {
                const float expected = std::fma(kAlpha, InputX(index), InputY(index));
                if (std::abs(output[index] - expected) > 1.0e-5F) {
                    std::println(stderr, "SAXPY validation failed at {}: actual={}, expected={}", index, output[index],
                                 expected);
                    std::exit(2);
                }
            }
            checksum_ = static_cast<double>(output[kElementCount / 3]) + output[kElementCount / 2];
        }

        [[nodiscard]] double Checksum() const noexcept { return checksum_; }

        [[nodiscard]] bool ReportsExecutionNanoseconds() const noexcept {
            return backend_.ExecutionQueue().View().reportsExecutionNanoseconds;
        }

    private:
        [[nodiscard]] static constexpr Sora::Compute::MemoryPropertyMask HostProperties() noexcept {
            return Sora::Compute::MemoryProperty::HostVisible | Sora::Compute::MemoryProperty::HostCoherent;
        }

        [[nodiscard]] static constexpr Sora::Compute::MemoryPropertyMask DeviceProperties() noexcept {
            return Sora::Compute::MemoryProperty::DeviceLocal;
        }

        [[nodiscard]] static constexpr float InputX(uint64_t index) noexcept {
            return static_cast<float>(index % 4096U) * 0.25F;
        }

        [[nodiscard]] static constexpr float InputY(uint64_t index) noexcept {
            return 1000.0F - static_cast<float>(index % 2048U) * 0.125F;
        }

        void InitializeInputs() {
            auto xBytes = Take(backend_.Map(xHost_), "map x input");
            auto yBytes = Take(backend_.Map(yHost_), "map y input");
            auto x = std::span<float>{reinterpret_cast<float*>(xBytes.data()), kElementCount};
            auto y = std::span<float>{reinterpret_cast<float*>(yBytes.data()), kElementCount};
            for (uint64_t index = 0; index < kElementCount; ++index) {
                x[index] = InputX(index);
                y[index] = InputY(index);
            }
        }

        void UploadInputs() {
            auto uploadedX = Take(upload_.Submit(xHost_, xDevice_, transferRegions_), "initial upload x");
            auto uploadedY = Take(upload_.Submit(yHost_, yDevice_, transferRegions_), "initial upload y");
            RequireCompleted(uploadedX.WaitUntilTerminal(), "initial upload x");
            RequireCompleted(uploadedY.WaitUntilTerminal(), "initial upload y");
        }

        [[nodiscard]] Sora::Compute::Event SubmitDispatch(std::span<const Sora::Compute::Event> waits) {
            std::array<Sora::Compute::EventView, 2> waitViews{};
            for (size_t index = 0; index < waits.size(); ++index) {
                waitViews[index] = waits[index].View();
            }
            const Sora::Compute::BackendSubmitRequest request{
                .commands = &command_,
                .commandCount = 1,
                .bindings = bindings_.data(),
                .bindingCount = static_cast<uint32_t>(bindings_.size()),
                .scalarData = scalarData_.data(),
                .scalarDataSize = scalarData_.size(),
                .scalarBindings = &scalarBinding_,
                .scalarBindingCount = 1,
                .waits = waitViews.data(),
                .waitCount = static_cast<uint32_t>(waits.size()),
            };
            return Take(backend_.ExecutionQueue().Submit(request), "submit dispatch");
        }

        static void RequireCompleted(Sora::Compute::EventObservation observation, std::string_view stage) {
            if (observation.status != Sora::Compute::EventStatus::Completed) {
                std::println(stderr, "{} failed: status={}, code={}, detail={}", stage,
                             std::to_underlying(observation.status), std::to_underlying(observation.error.code),
                             observation.error.detail);
                std::exit(2);
            }
        }

        void RequireTimedDispatch(Sora::Compute::EventObservation observation, std::string_view stage) const {
            RequireCompleted(observation, stage);
            if ((observation.executionNanoseconds != 0) != ReportsExecutionNanoseconds()) {
                std::println(stderr, "{} returned an execution duration inconsistent with queue capability", stage);
                std::exit(2);
            }
        }

        Sora::Compute::VulkanBackend backend_;
        MaterializedKernel materialized_;
        Sora::Compute::Allocation xHost_;
        Sora::Compute::Allocation yHost_;
        Sora::Compute::Allocation outputHost_;
        Sora::Compute::Allocation xDevice_;
        Sora::Compute::Allocation yDevice_;
        Sora::Compute::Allocation outputDevice_;
        const Sora::Compute::TransferEngine& upload_;
        const Sora::Compute::TransferEngine& download_;
        std::array<Sora::Compute::TransferRegion, 1> transferRegions_{
            Sora::Compute::TransferRegion{.sizeBytes = kDataBytes}};
        std::array<Sora::Compute::BackendBinding, 3> bindings_{};
        std::array<std::byte, sizeof(float)> scalarData_{};
        Sora::Compute::BackendScalarBinding scalarBinding_{};
        Sora::Compute::BackendCommand command_{};
        Sora::Compute::Event completedEvent_{};
        double checksum_ = 0.0;
    };

    [[nodiscard]] double MeasureColdMaterialization() {
        std::array<double, kColdSampleCount> samples{};
        for (double& sample : samples) {
            const auto begin = std::chrono::steady_clock::now();
            auto backend = Take(Sora::Compute::VulkanBackend::Create(), "create cold Vulkan backend");
            auto materialized = Materialize(backend);
            const auto end = std::chrono::steady_clock::now();
            sample = std::chrono::duration<double, std::milli>(end - begin).count();
            if (!materialized.pipeline) {
                std::exit(2);
            }
        }
        return Sora::Benchmark::Median(samples);
    }

    struct ColdBackendSetTiming {
        double milliseconds = 0.0;
        size_t deviceCount = 0;
    };

    [[nodiscard]] ColdBackendSetTiming MeasureColdBackendSet() {
        std::array<double, kColdSampleCount> samples{};
        size_t deviceCount = 0;
        for (double& sample : samples) {
            const auto begin = std::chrono::steady_clock::now();
            auto backends = Take(Sora::Compute::VulkanBackend::CreateAll(), "create complete Vulkan backend set");
            std::vector<MaterializedKernel> materialized;
            materialized.reserve(backends.size());
            for (const Sora::Compute::VulkanBackend& backend : backends) {
                materialized.push_back(Materialize(backend));
            }
            const auto end = std::chrono::steady_clock::now();
            if (backends.empty() || (deviceCount != 0 && deviceCount != backends.size())) {
                std::println(stderr, "Vulkan device set changed during cold benchmark");
                std::exit(2);
            }
            deviceCount = backends.size();
            sample = std::chrono::duration<double, std::milli>(end - begin).count();
        }
        return {.milliseconds = Sora::Benchmark::Median(samples), .deviceCount = deviceCount};
    }

    void PrintMetadata(const Sora::Compute::VulkanBackend& backend) {
        const auto& device = backend.Device();
        const auto& target = backend.Target();
        std::println("vulkan_metadata,0,device_id_low,{:016x},hex", device.id.low);
        std::println("vulkan_metadata,0,device_id_high,{:016x},hex", device.id.high);
        std::println("vulkan_metadata,0,architecture_id,{:016x},hex", device.architectureId);
        std::println("vulkan_metadata,0,driver_identity_low,{:016x},hex", target.driverIdentity.low);
        std::println("vulkan_metadata,0,driver_identity_high,{:016x},hex", target.driverIdentity.high);
        std::println("vulkan_metadata,0,compiler_identity_low,{:016x},hex", target.compilerIdentity.low);
        std::println("vulkan_metadata,0,compiler_identity_high,{:016x},hex", target.compilerIdentity.high);
        std::println("vulkan_metadata,0,device_memory,{},bytes", device.totalMemoryBytes);
        std::println("vulkan_metadata,0,subgroup_width,{},lanes", device.subgroupWidthMinimum);
        std::println("vulkan_metadata,0,reports_execution_nanoseconds,{},bool",
                     backend.ExecutionQueue().View().reportsExecutionNanoseconds);
    }

} // namespace

int main() {
    VulkanWorkload workload;
    PrintMetadata(workload.Backend());

    const double coldMilliseconds = MeasureColdMaterialization();
    const ColdBackendSetTiming coldSet = MeasureColdBackendSet();
    auto resident = [&workload]() noexcept { workload.RunResidentDispatch(); };
    auto completedEventQuery = [&workload]() noexcept { workload.QueryCompletedEvent(); };
    auto transfer = [&workload]() noexcept { workload.RunTransferRoundTrip(); };
    auto roundTrip = [&workload]() noexcept { workload.RunRoundTrip(); };
    const std::array cases{
        Sora::Benchmark::MakeCase("resident_dispatch", resident),
        Sora::Benchmark::MakeCase("transfer_round_trip", transfer),
        Sora::Benchmark::MakeCase("round_trip", roundTrip),
    };
    const auto timing =
        Sora::Benchmark::MeasureInterleavedIndependent<kHotSampleCount>(cases, kElementCount, kMinimumSampleTime);
    const std::array queryCases{Sora::Benchmark::MakeCase("completed_event_query", completedEventQuery)};
    const auto queryTiming =
        Sora::Benchmark::MeasureInterleavedIndependent<kHotSampleCount>(queryCases, 1, kMinimumSampleTime);
    const uint64_t deviceExecutionNanoseconds = workload.SampleDeviceExecutionNanoseconds();
    workload.ValidateOutput();

    std::println("family,elements,mode,value,unit");
    std::println("vulkan_cold,0,cold_service_materialization,{:.6f},ms/service", coldMilliseconds);
    std::println("vulkan_cold_set,{},complete_set_materialization,{:.6f},ms/set", coldSet.deviceCount,
                 coldSet.milliseconds);
    std::println("vulkan_cold_set,{},amortized_device_materialization,{:.6f},us/device", coldSet.deviceCount,
                 coldSet.milliseconds * 1'000.0 / static_cast<double>(coldSet.deviceCount));
    std::println("vulkan_cold_ratio,{},set_amortized_over_single,{:.6f},ratio", coldSet.deviceCount,
                 coldSet.milliseconds / (static_cast<double>(coldSet.deviceCount) * coldMilliseconds));
    std::println("vulkan_hot,{},resident_dispatch,{:.6f},ns/element", kElementCount, timing.MedianTime(0));
    std::println("vulkan_hot,{},transfer_round_trip,{:.6f},ns/element", kElementCount, timing.MedianTime(1));
    std::println("vulkan_hot,{},round_trip,{:.6f},ns/element", kElementCount, timing.MedianTime(2));
    std::println("vulkan_timing,1,completed_event_query,{:.6f},ns/query", queryTiming.MedianTime(0));
    std::println("vulkan_timing,{},device_execution,{},ns/dispatch", kElementCount, deviceExecutionNanoseconds);
    if (deviceExecutionNanoseconds != 0) {
        const double hostDispatchNanoseconds = timing.MedianTime(0) * static_cast<double>(kElementCount);
        std::println("vulkan_timing,{},host_over_device,{:.6f},ratio", kElementCount,
                     hostDispatchNanoseconds / static_cast<double>(deviceExecutionNanoseconds));
    }
    std::println("vulkan_ratio,{},round_trip_over_resident,{:.6f},ratio", kElementCount, timing.PairedRatio(2, 0));
    std::println("checksum,{},saxpy,{:.9f},value", kElementCount, workload.Checksum());
}

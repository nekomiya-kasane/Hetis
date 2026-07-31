#include <Sora/Compute/Runtime/PerformanceModel.h>
#include <Sora/Compute/Runtime/PerformancePrior.h>

#include <catch2/catch_test_macros.hpp>

#include <limits>

namespace {

    constexpr Sora::Compute::DeviceId kDevice{11, 12};
    constexpr Sora::Compute::DeviceId kHostDevice{21, 22};
    constexpr size_t kCpuPriorIndex = static_cast<size_t>(Sora::Compute::DeviceKind::Cpu);
    constexpr size_t kVulkanPriorIndex = static_cast<size_t>(Sora::Compute::BackendKind::Vulkan);
    constexpr Sora::Compute::MemoryLocation kHost{
        .domain = {31, 32}, .device = kHostDevice, .backend = Sora::Compute::BackendKind::Cpu};
    constexpr Sora::Compute::MemoryLocation kDeviceMemory{
        .domain = {41, 42}, .device = kDevice, .backend = Sora::Compute::BackendKind::Vulkan};

    constexpr Sora::Compute::TargetFingerprint Target(uint64_t architecture) noexcept {
        return {.device = kDevice,
                .backend = Sora::Compute::BackendKind::Vulkan,
                .hostArchitecture = Sora::CpuArchitecture::X64,
                .operatingSystem = Sora::OperatingSystem::Windows,
                .binaryFormat = Sora::BinaryFormat::PE,
                .byteOrder = Sora::ByteOrder::Little,
                .abi = Sora::AbiFamily::MSVC,
                .identityStability = Sora::Compute::TargetIdentityStability::MachineStable,
                .addressBits = 64,
                .architectureId = architecture,
                .compilerIdentity = {51, 52},
                .driverIdentity = {53, 54}};
    }

    constexpr Sora::Compute::KernelCalibrationKey KernelKey(uint64_t identity, uint64_t architecture = 61) noexcept {
        return {.kernel = {identity, 1}, .target = Target(architecture), .queue = {71, 72}};
    }

    constexpr Sora::Compute::TransferCalibrationKey TransferKey() noexcept {
        return {.service = {81, 82}, .engine = {83, 84}, .source = kHost, .destination = kDeviceMemory};
    }

    constexpr Sora::Compute::PipelineKey PipelineKey(uint64_t state) noexcept {
        constexpr auto kernel = KernelKey(91);
        constexpr auto executable = Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
            .lowered = kernel.kernel, .target = kernel.target, .specialization = kernel.specialization});
        return {.executable = Sora::Compute::ArtifactIdentity(executable),
                .target = kernel.target,
                .builder = {92, 93},
                .state = {state, 94},
                .bindingLayout = {95, 96}};
    }

    constexpr Sora::Compute::ArtifactKey ExecutableArtifact(uint64_t identity, uint64_t architecture = 61) noexcept {
        const auto kernel = KernelKey(identity, architecture);
        return Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
            .lowered = kernel.kernel, .target = kernel.target, .specialization = kernel.specialization});
    }

} // namespace

TEST_CASE("PerformancePrior keeps cold-start forecasts distinct from measured calibration",
          "[Sora][Compute][Runtime][PerformancePrior]") {
    constexpr Sora::Compute::DeviceDescriptor cpu{.id = {101, 102},
                                                  .backend = Sora::Compute::BackendKind::Cpu,
                                                  .kind = Sora::Compute::DeviceKind::Cpu,
                                                  .computeUnitCount = 4,
                                                  .subgroupWidthMinimum = 1,
                                                  .subgroupWidthMaximum = 4,
                                                  .maximumWorkgroupInvocations = 1024};
    constexpr Sora::Compute::PerformancePriorModel priors;
    constexpr Sora::Compute::KernelResourceDemand saxpyDemand{
        .floatingPointOperations = 2, .bytesRead = 8, .bytesWritten = 4};
    constexpr auto kernel = priors.ForecastKernel(cpu, saxpyDemand, true);
    STATIC_REQUIRE(kernel.has_value());
    STATIC_REQUIRE(kernel->Valid());
    STATIC_REQUIRE(kernel->estimate.workItemsPerSecond == 200'000'000);
    STATIC_REQUIRE(kernel->estimate.sampleCount == 0);
    STATIC_REQUIRE(kernel->evidence.kind == Sora::Compute::PerformanceEvidenceKind::Prior);
    STATIC_REQUIRE(kernel->evidence.sampleCount == 0);

    constexpr Sora::Compute::KernelResourceDemand copyDemand{.bytesRead = 4, .bytesWritten = 4};
    constexpr auto copy = priors.ForecastKernel(cpu, copyDemand, true);
    STATIC_REQUIRE(copy->estimate.workItemsPerSecond == 300'000'000);

    constexpr Sora::Compute::KernelResourceDemand specialDemand{
        .floatingPointOperations = 1, .specialFunctionOperations = 1, .bytesRead = 4, .bytesWritten = 4};
    constexpr auto special = priors.ForecastKernel(cpu, specialDemand, true);
    STATIC_REQUIRE(special->estimate.workItemsPerSecond == 50'000'000);

    constexpr Sora::Compute::KernelResourceDemand stridedDemand{.floatingPointOperations = 2,
                                                                .bytesRead = 8,
                                                                .bytesWritten = 4,
                                                                .stridedBytesRead = 8,
                                                                .stridedBytesWritten = 4};
    constexpr auto strided = priors.ForecastKernel(cpu, stridedDemand, true);
    STATIC_REQUIRE(strided->estimate.workItemsPerSecond == 100'000'000);

    constexpr auto artifact = priors.ForecastArtifact(
        Sora::Compute::BackendKind::Vulkan, Sora::Compute::ArtifactResolutionPath::TransientProduction, false);
    STATIC_REQUIRE(artifact.has_value());
    STATIC_REQUIRE(artifact->nanoseconds == 20'000'000);

    constexpr auto persistent = priors.ForecastPersistent(Sora::Compute::BackendKind::Vulkan, true);
    STATIC_REQUIRE(persistent.has_value());
    STATIC_REQUIRE(persistent->Valid());
    STATIC_REQUIRE(persistent->importPermille == 750);
    STATIC_REQUIRE(persistent->productionPermille == 250);

    constexpr auto maximumPolicy = [] {
        Sora::Compute::PerformancePriorPolicy policy;
        auto& artifact = policy.artifacts[kVulkanPriorIndex];
        artifact.persistentImportNanoseconds = std::numeric_limits<uint64_t>::max();
        artifact.pipelineProductionNanoseconds = std::numeric_limits<uint64_t>::max();
        return policy;
    }();
    constexpr Sora::Compute::PerformancePriorModel maximumPriors{maximumPolicy};
    constexpr auto maximumPersistent = maximumPriors.ForecastPersistent(Sora::Compute::BackendKind::Vulkan, true);
    STATIC_REQUIRE(maximumPersistent.has_value());
    STATIC_REQUIRE(maximumPersistent->importNanoseconds ==
                   std::numeric_limits<uint64_t>::max() / 1'000 * 750 +
                       std::numeric_limits<uint64_t>::max() % 1'000 * 750 / 1'000);
    STATIC_REQUIRE(maximumPersistent->productionNanoseconds ==
                   std::numeric_limits<uint64_t>::max() / 1'000 * 250 +
                       std::numeric_limits<uint64_t>::max() % 1'000 * 250 / 1'000);

    constexpr Sora::Compute::PerformancePriorPolicy disabledPolicy{.enabled = false};
    constexpr Sora::Compute::PerformancePriorModel disabled{disabledPolicy};
    STATIC_REQUIRE_FALSE(disabled.ForecastKernel(cpu, saxpyDemand, true).has_value());
    STATIC_REQUIRE_FALSE(disabled
                             .ForecastArtifact(Sora::Compute::BackendKind::Vulkan,
                                               Sora::Compute::ArtifactResolutionPath::TransientProduction, false)
                             .has_value());
    STATIC_REQUIRE_FALSE(disabled.ForecastPersistent(Sora::Compute::BackendKind::Vulkan, false).has_value());

    constexpr auto invalid = [] {
        Sora::Compute::PerformancePriorPolicy policy;
        policy.kernels[kCpuPriorIndex].memoryBytesPerSecondPerComputeUnit = 0;
        return Sora::Compute::PerformancePriorModel{policy};
    }();
    STATIC_REQUIRE_FALSE(invalid.ForecastKernel(cpu, saxpyDemand, true).has_value());
    STATIC_REQUIRE_FALSE(invalid
                             .ForecastArtifact(Sora::Compute::BackendKind::Vulkan,
                                               Sora::Compute::ArtifactResolutionPath::TransientProduction, false)
                             .has_value());
    STATIC_REQUIRE_FALSE(invalid.ForecastPersistent(Sora::Compute::BackendKind::Vulkan, false).has_value());

    constexpr auto saturated = [] {
        Sora::Compute::PerformancePriorPolicy policy;
        auto& cpuPrior = policy.kernels[kCpuPriorIndex];
        cpuPrior.memoryBytesPerSecondPerComputeUnit = std::numeric_limits<uint64_t>::max();
        cpuPrior.stridedMemoryEfficiencyPermille = 500;
        return Sora::Compute::PerformancePriorModel{policy};
    }();
    constexpr Sora::Compute::DeviceDescriptor oneUnitCpu{.id = {103, 104},
                                                         .backend = Sora::Compute::BackendKind::Cpu,
                                                         .kind = Sora::Compute::DeviceKind::Cpu,
                                                         .computeUnitCount = 1,
                                                         .subgroupWidthMinimum = 1,
                                                         .subgroupWidthMaximum = 1,
                                                         .maximumWorkgroupInvocations = 1};
    constexpr Sora::Compute::KernelResourceDemand maximumStridedDemand{
        .bytesRead = std::numeric_limits<uint64_t>::max(), .stridedBytesRead = std::numeric_limits<uint64_t>::max()};
    constexpr auto saturatedForecast = saturated.ForecastKernel(oneUnitCpu, maximumStridedDemand, true);
    STATIC_REQUIRE(saturatedForecast.has_value());
    STATIC_REQUIRE(saturatedForecast->estimate.workItemsPerSecond == 1);
}

TEST_CASE("PerformanceModel publishes immutable bounded kernel snapshots",
          "[Sora][Compute][Runtime][PerformanceModel]") {
    Sora::Compute::PerformanceModel model(
        {.maximumKernelKeys = 1, .maximumTransferKeys = 1, .maximumSamplesPerBucket = 2});
    REQUIRE(
        model.Update({.key = KernelKey(1), .workItemCount = 1024, .computeNanoseconds = 1024, .launchNanoseconds = 100})
            .has_value());
    const auto first = model.Snapshot();
    REQUIRE(first);
    REQUIRE(first->Estimate(KernelKey(1), 1024)->sampleCount == 1);
    REQUIRE(first->Estimate(KernelKey(1), 1024)->workItemsPerSecond == 1'000'000'000);

    REQUIRE(
        model.Update({.key = KernelKey(1), .workItemCount = 1024, .computeNanoseconds = 512, .launchNanoseconds = 200})
            .has_value());
    REQUIRE(
        model.Update({.key = KernelKey(1), .workItemCount = 1024, .computeNanoseconds = 256, .launchNanoseconds = 300})
            .has_value());
    const auto latest = model.Snapshot();
    REQUIRE(first->Estimate(KernelKey(1), 1024)->sampleCount == 1);
    REQUIRE(latest->Estimate(KernelKey(1), 1024)->sampleCount == 2);
    REQUIRE(latest->Estimate(KernelKey(1), 1024)->workItemsPerSecond > 2'000'000'000);

    REQUIRE(model.Update({.key = KernelKey(2, 62), .workItemCount = 1024, .computeNanoseconds = 1024}).has_value());
    const auto evicted = model.Snapshot();
    REQUIRE_FALSE(evicted->Estimate(KernelKey(1), 1024).has_value());
    REQUIRE(evicted->Estimate(KernelKey(2, 62), 1024).has_value());
}

TEST_CASE("PerformanceModel keeps transfer calibration orthogonal to topology and kernel identity",
          "[Sora][Compute][Runtime][PerformanceModel]") {
    Sora::Compute::PerformanceModel model;
    REQUIRE(
        model
            .Update(
                {.key = TransferKey(), .bytes = 1'000'000, .payloadNanoseconds = 100'000, .latencyNanoseconds = 5000})
            .has_value());
    const auto snapshot = model.Snapshot();
    REQUIRE(snapshot->KernelEntries().empty());
    REQUIRE(snapshot->TransferEntries().size() == 1);
    REQUIRE(snapshot->Estimate(TransferKey(), 1'000'000)->bytesPerSecond == 10'000'000'000);
    REQUIRE_FALSE(snapshot->Estimate(KernelKey(1), 1'000'000).has_value());
}

TEST_CASE("PerformanceModel separates exact artifact resolution paths and evicts by artifact key",
          "[Sora][Compute][Runtime][PerformanceModel][Artifact]") {
    Sora::Compute::PerformanceModel model(
        {.maximumKernelKeys = 1, .maximumTransferKeys = 1, .maximumArtifactKeys = 1, .maximumSamplesPerBucket = 2});
    const auto firstKey = Sora::Compute::MakeArtifactKey(PipelineKey(1));
    const auto secondKey = Sora::Compute::MakeArtifactKey(PipelineKey(2));
    REQUIRE(
        model
            .Update(
                {.key = firstKey, .path = Sora::Compute::ArtifactResolutionPath::PackageImport, .nanoseconds = 1'000})
            .has_value());
    const auto first = model.Snapshot();
    REQUIRE(first->Estimate(firstKey, Sora::Compute::ArtifactResolutionPath::PackageImport)->nanoseconds == 1'000);
    REQUIRE(first->Estimate(firstKey, Sora::Compute::ArtifactResolutionPath::PackageImport)->sampleCount == 1);
    REQUIRE_FALSE(first->Estimate(secondKey, Sora::Compute::ArtifactResolutionPath::PackageImport).has_value());

    REQUIRE(
        model
            .Update(
                {.key = firstKey, .path = Sora::Compute::ArtifactResolutionPath::PackageImport, .nanoseconds = 2'000})
            .has_value());
    REQUIRE(
        model
            .Update(
                {.key = firstKey, .path = Sora::Compute::ArtifactResolutionPath::PackageImport, .nanoseconds = 3'000})
            .has_value());
    const auto blended = model.Snapshot();
    REQUIRE(blended->Estimate(firstKey, Sora::Compute::ArtifactResolutionPath::PackageImport)->nanoseconds == 2'250);
    REQUIRE(blended->Estimate(firstKey, Sora::Compute::ArtifactResolutionPath::PackageImport)->sampleCount == 2);
    REQUIRE(first->Estimate(firstKey, Sora::Compute::ArtifactResolutionPath::PackageImport)->nanoseconds == 1'000);

    REQUIRE(model
                .Update({.key = secondKey,
                         .path = Sora::Compute::ArtifactResolutionPath::TransientProduction,
                         .nanoseconds = 4'000})
                .has_value());
    const auto evicted = model.Snapshot();
    REQUIRE_FALSE(evicted->Estimate(firstKey, Sora::Compute::ArtifactResolutionPath::PackageImport).has_value());
    REQUIRE(evicted->Estimate(secondKey, Sora::Compute::ArtifactResolutionPath::TransientProduction)->nanoseconds ==
            4'000);
}

TEST_CASE("PerformanceModel weights persistent import and production without claiming disk availability",
          "[Sora][Compute][Runtime][PerformanceModel][Artifact]") {
    Sora::Compute::PerformanceModel model;
    const auto key = ExecutableArtifact(1);
    REQUIRE(
        model
            .Update({.key = key, .path = Sora::Compute::ArtifactResolutionPath::PersistentImport, .nanoseconds = 1'000})
            .has_value());
    REQUIRE(
        model
            .Update(
                {.key = key, .path = Sora::Compute::ArtifactResolutionPath::PersistentProduction, .nanoseconds = 9'000})
            .has_value());
    const auto estimate = model.Snapshot()->EstimatePersistentResolution(key);
    REQUIRE(estimate->importNanoseconds == 500);
    REQUIRE(estimate->productionNanoseconds == 4'500);
    REQUIRE(estimate->importWeight == 1);
    REQUIRE(estimate->productionWeight == 1);
}

TEST_CASE("PerformanceModel adapts bounded persistent outcome weights after saturation",
          "[Sora][Compute][Runtime][PerformanceModel][Artifact]") {
    Sora::Compute::PerformanceModel model({.maximumSamplesPerBucket = 4});
    const auto key = ExecutableArtifact(1);
    REQUIRE(
        model
            .Update({.key = key, .path = Sora::Compute::ArtifactResolutionPath::PersistentImport, .nanoseconds = 1'000})
            .has_value());
    for (uint32_t sample = 0; sample < 4; ++sample) {
        REQUIRE(model
                    .Update({.key = key,
                             .path = Sora::Compute::ArtifactResolutionPath::PersistentProduction,
                             .nanoseconds = 9'000})
                    .has_value());
    }

    const auto estimate = model.Snapshot()->EstimatePersistentResolution(key);
    REQUIRE(estimate->importWeight == 0);
    REQUIRE(estimate->productionWeight == 2);
    REQUIRE(estimate->importNanoseconds == 0);
    REQUIRE(estimate->productionNanoseconds == 9'000);

    Sora::Compute::PerformanceModel latestOutcome({.maximumSamplesPerBucket = 1});
    const auto latestKey = ExecutableArtifact(2);
    REQUIRE(latestOutcome
                .Update({.key = latestKey,
                         .path = Sora::Compute::ArtifactResolutionPath::PersistentImport,
                         .nanoseconds = 1'000})
                .has_value());
    REQUIRE(latestOutcome
                .Update({.key = latestKey,
                         .path = Sora::Compute::ArtifactResolutionPath::PersistentProduction,
                         .nanoseconds = 9'000})
                .has_value());
    const auto latestEstimate = latestOutcome.Snapshot()->EstimatePersistentResolution(latestKey);
    REQUIRE(latestEstimate->importWeight == 0);
    REQUIRE(latestEstimate->productionWeight == 1);
    REQUIRE(latestEstimate->importNanoseconds == 0);
    REQUIRE(latestEstimate->productionNanoseconds == 9'000);
}

TEST_CASE("PerformanceModel rejects incomplete observations and invalid limits",
          "[Sora][Compute][Runtime][PerformanceModel][Validation]") {
    Sora::Compute::PerformanceModel invalid({.maximumKernelKeys = 0});
    const auto invalidLimits = invalid.Update({.key = KernelKey(1), .workItemCount = 1, .computeNanoseconds = 1});
    REQUIRE(invalidLimits.error() == Sora::Compute::PerformanceModelError::InvalidLimits);

    Sora::Compute::PerformanceModel model;
    const auto invalidObservation = model.Update(Sora::Compute::KernelPerformanceObservation{});
    REQUIRE(invalidObservation.error() == Sora::Compute::PerformanceModelError::InvalidObservation);
    const auto invalidResolution = model.Update(Sora::Compute::ArtifactResolutionObservation{});
    REQUIRE(invalidResolution.error() == Sora::Compute::PerformanceModelError::InvalidObservation);
    const auto liveResolution = model.Update(Sora::Compute::ArtifactResolutionObservation{
        .key = ExecutableArtifact(1), .path = Sora::Compute::ArtifactResolutionPath::Live, .nanoseconds = 1});
    REQUIRE(liveResolution.error() == Sora::Compute::PerformanceModelError::InvalidObservation);
    REQUIRE_FALSE(model.Snapshot());
}

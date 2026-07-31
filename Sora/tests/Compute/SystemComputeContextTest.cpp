/**
 * @file SystemComputeContextTest.cpp
 * @brief Automatic target-machine runtime and context integration tests.
 * @ingroup Testing
 */

#include <Sora/Compute/ElementwiseKernel.h>
#include <Sora/Compute/Graph/GraphBuilder.h>
#include <Sora/Compute/SystemComputeContext.h>
#include <Sora/Core/PAL/Environment.h>
#include <Sora/Math/PortableCompile.h>

#include <catch2/catch_test_macros.hpp>
#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

    inline constexpr auto kMathRuntimeDefinition =
        Sora::Math::CompilePortable<float, 1>([](auto value) { return Sora::Math::Add(value, 2.0F); });
    inline constexpr Sora::Compute::IndexDomain kMathRuntimeDomain{.rank = 1};
    inline constexpr auto kMathRuntimeProgram = Sora::Compute::LiftElementwise<3>(
        kMathRuntimeDefinition, kMathRuntimeDomain,
        {.layout = Sora::Compute::LayoutKind::Contiguous, .minimumAlignment = 64, .aliasClass = {2}},
        Sora::Compute::ElementwiseInputSpec::Value());

    static_assert(kMathRuntimeDefinition(1.5F) == 3.5F);

    [[nodiscard]] Sora::Compute::SystemRuntimeConfig CpuOnlyRuntime() {
        return {.cpu = {.workerCount = 1},
                .cpuCode = Sora::Compute::SystemCpuCodePolicy::InterpreterOnly,
                .vulkanPolicy = Sora::Compute::SystemVulkanPolicy::Disabled};
    }

    class TemporaryDirectory final {
    public:
        TemporaryDirectory() {
            static std::atomic<uint64_t> sequence = 0;
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path_ =
                std::filesystem::temp_directory_path() / std::format("sora-system-artifact-store-test-{}-{}", stamp,
                                                                     sequence.fetch_add(1, std::memory_order_relaxed));
        }

        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }

        TemporaryDirectory(const TemporaryDirectory&) = delete;
        TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

        [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

    private:
        std::filesystem::path path_;
    };

    class RestoreArtifactStoreRoot final {
    public:
        RestoreArtifactStoreRoot() {
            auto original = Sora::PAL::ReadEnvironmentVariable(Sora::Compute::kSystemArtifactStoreRootEnvironment);
            if (!original) {
                throw std::runtime_error("failed to preserve the system artifact-store override");
            }
            original_ = std::move(*original);
        }

        ~RestoreArtifactStoreRoot() {
            if (original_) {
                std::ignore =
                    Sora::PAL::WriteEnvironmentVariable(Sora::Compute::kSystemArtifactStoreRootEnvironment, *original_);
            } else {
                std::ignore = Sora::PAL::RemoveEnvironmentVariable(Sora::Compute::kSystemArtifactStoreRootEnvironment);
            }
        }

        RestoreArtifactStoreRoot(const RestoreArtifactStoreRoot&) = delete;
        RestoreArtifactStoreRoot& operator=(const RestoreArtifactStoreRoot&) = delete;

    private:
        std::optional<std::string> original_;
    };

} // namespace

TEST_CASE("System compute context submits a lifted Math definition through the authoritative runtime",
          "[Sora][Compute][System][Context][MathFrontend]") {
    constexpr uint64_t count = 1'024;
    constexpr Sora::Compute::ValueType vec3{Sora::Compute::ScalarKind::Float32, 3};
    const std::array values{1.0F, 2.0F, 3.0F};
    auto value = Sora::Compute::ScalarArgument::Create(vec3, std::as_bytes(std::span{values}));
    REQUIRE(value.has_value());

    auto output = Sora::Compute::Buffer::Create({
        .id = {0x4D4154485F46524FULL, 0x4E54454E445F3031ULL},
        .elementType = vec3,
        .layout = Sora::Compute::LayoutKind::Contiguous,
        .rank = 1,
        .sizeBytes = count * vec3.ByteSize(),
        .alignment = 64,
    });
    REQUIRE(output.has_value());
    const std::array extents{count};
    auto outputView = Sora::Compute::BufferView::Contiguous(*output, extents);
    REQUIRE(outputView.has_value());

    std::vector<Sora::Compute::KernelArgumentBinding> bindings;
    bindings.emplace_back(std::move(*value));
    bindings.emplace_back(std::move(*outputView));
    auto invocation = Sora::Compute::BindKernelProgram(kMathRuntimeProgram, extents, std::move(bindings));
    REQUIRE(invocation.has_value());
    Sora::Compute::GraphBuilder graphBuilder;
    REQUIRE(graphBuilder.AddDispatch(std::move(*invocation)).has_value());
    auto graph = std::move(graphBuilder).Finish();
    REQUIRE(graph.has_value());

    Sora::Compute::SystemComputeContextConfig config;
    config.runtime = CpuOnlyRuntime();
    auto context = Sora::Compute::CreateSystemComputeContext(std::move(config));
    REQUIRE(context.has_value());
    auto package = context->RegisterProgram(kMathRuntimeProgram);
    REQUIRE(package.has_value());
    REQUIRE(package->Program().Hash() == kMathRuntimeProgram.Hash());
    const auto beforeSubmit = context->Performance().Snapshot();
    REQUIRE_FALSE((beforeSubmit && !beforeSubmit->KernelEntries().empty()));

    exec::static_thread_pool pool{1};
    std::optional<Sora::Compute::SystemComputeContext::Error> observedError;
    auto sender = context->Submit(pool.get_scheduler(), std::move(*graph)) |
                  stdexec::upon_error([&observedError](Sora::Compute::SystemComputeContext::Error error) noexcept {
                      observedError.emplace(std::move(error));
                      return Sora::Compute::EventObservation{};
                  });
    auto completed = stdexec::sync_wait(std::move(sender));
    REQUIRE(completed.has_value());
    REQUIRE_FALSE(observedError.has_value());
    REQUIRE(std::get<0>(*completed).status == Sora::Compute::EventStatus::Completed);
    REQUIRE(output->CurrentVersion() == 1);
    REQUIRE(output->ReplicaCount() == 1);

    const auto performance = context->Performance().Snapshot();
    REQUIRE(performance);
    REQUIRE(performance->KernelEntries().size() == 1);
    REQUIRE(performance->KernelEntries().front().key.kernel == kMathRuntimeProgram.Hash());
    const auto estimate = performance->Estimate(performance->KernelEntries().front().key, count);
    REQUIRE(estimate.has_value());
    REQUIRE(estimate->sampleCount == 1);
    REQUIRE(estimate->workItemsPerSecond != 0);
}

TEST_CASE("System runtime validates closed discovery policies", "[Sora][Compute][System][Validation]") {
    auto config = CpuOnlyRuntime();
    config.cpuCode = static_cast<Sora::Compute::SystemCpuCodePolicy>(UINT8_MAX);
    auto invalid = Sora::Compute::SystemRuntimeFactory::Create(std::move(config));
    REQUIRE_FALSE(invalid.has_value());
    REQUIRE(invalid.error().code == Sora::Compute::RuntimeDiscoveryErrorCode::InvalidConfiguration);
}

TEST_CASE("System runtime never hides invalid opportunistic Vulkan configuration",
          "[Sora][Compute][System][Validation]") {
    if (!Sora::Compute::SystemRuntimeFactory::VulkanBackendCompiled()) {
        SKIP("built-in Vulkan backend is not part of this build");
    }
    auto config = CpuOnlyRuntime();
    config.vulkanPolicy = Sora::Compute::SystemVulkanPolicy::Opportunistic;
    config.vulkan.compilerWorkerCount = UINT16_MAX;
    auto factory = Sora::Compute::SystemRuntimeFactory::Create(std::move(config));
    REQUIRE(factory.has_value());

    auto epoch = factory->Discover(1);
    REQUIRE_FALSE(epoch.has_value());
    REQUIRE(epoch.error().code == Sora::Compute::RuntimeDiscoveryErrorCode::ProviderFailed);
    REQUIRE(epoch.error().provider == Sora::Compute::kSystemVulkanRuntimeProvider);
    REQUIRE(epoch.error().providerError.domain == Sora::Compute::kSystemVulkanBackendErrorDomain);
    REQUIRE(epoch.error().providerError.code ==
            static_cast<uint64_t>(Sora::Compute::VulkanBackendErrorCode::InvalidConfiguration));
}

TEST_CASE("System runtime discovers a complete portable CPU epoch without external assembly",
          "[Sora][Compute][System][Discovery]") {
    auto factory = Sora::Compute::SystemRuntimeFactory::Create(CpuOnlyRuntime());
    REQUIRE(factory.has_value());

    auto epoch = factory->Discover(7);
    REQUIRE(epoch.has_value());
    REQUIRE(epoch->Sequence() == 7);
    REQUIRE(epoch->Devices().size() == 1);
    REQUIRE(epoch->Devices().front().descriptor.backend == Sora::Compute::BackendKind::Cpu);
    REQUIRE(epoch->Snapshots().front().state == Sora::Compute::DeviceState::Available);
    REQUIRE(epoch->Resources().Inventory().Queues().size() == 1);
    REQUIRE(epoch->Resources().Inventory().Compilers().size() == 1);
}

TEST_CASE("System runtime recovery re-discovers physical coordinates under fresh service identities",
          "[Sora][Compute][System][Recovery]") {
    auto factory = Sora::Compute::SystemRuntimeFactory::Create(CpuOnlyRuntime());
    REQUIRE(factory.has_value());
    auto initial = factory->Discover(1);
    REQUIRE(initial.has_value());
    const Sora::Compute::BackendServiceId failedService = initial->Devices().front().service;

    auto replacement = (*factory)(
        Sora::Compute::RuntimeRecoveryRequest{.previous = {}, .failedService = failedService, .sequence = 2});
    REQUIRE(replacement.has_value());
    REQUIRE(replacement->Sequence() == 2);
    REQUIRE(replacement->Devices().size() == 1);
    REQUIRE(replacement->Devices().front().descriptor.id == initial->Devices().front().descriptor.id);
    REQUIRE(replacement->Devices().front().service != failedService);
}

TEST_CASE("System compute context closes discovery planning calibration and recovery ownership",
          "[Sora][Compute][System][Context]") {
    Sora::Compute::SystemComputeContextConfig config;
    config.runtime = CpuOnlyRuntime();
    auto context = Sora::Compute::CreateSystemComputeContext(std::move(config));
    REQUIRE(context.has_value());
    REQUIRE(context->Recovery().Current()->Sequence() == 1);
    REQUIRE(context->Recovery().Current()->Devices().size() == 1);
    REQUIRE(context->Recovery().Current()->Devices().front().descriptor.backend == Sora::Compute::BackendKind::Cpu);
}

TEST_CASE("System compute context retains an injected shared automatic-planning calibration",
          "[Sora][Compute][System][Context][Calibration]") {
    std::weak_ptr<Sora::Compute::AutomaticPartitionPlanningCalibration> retained;
    {
        auto calibration = std::make_shared<Sora::Compute::AutomaticPartitionPlanningCalibration>();
        retained = calibration;
        Sora::Compute::SystemComputeContextConfig config;
        config.runtime = CpuOnlyRuntime();
        config.automaticPlanningCalibration = calibration;
        auto context = Sora::Compute::CreateSystemComputeContext(std::move(config));
        REQUIRE(context.has_value());
        calibration.reset();
        REQUIRE_FALSE(retained.expired());
    }
    REQUIRE(retained.expired());
}

TEST_CASE("System compute context opportunistically assembles target-native backends",
          "[Sora][Compute][System][Context][Native]") {
    Sora::Compute::SystemComputeContextConfig config;
    config.automaticArtifacts.mode = Sora::Compute::SystemArtifactStoreMode::Disabled;
    auto context = Sora::Compute::CreateSystemComputeContext(std::move(config));
    REQUIRE(context.has_value());
    const auto epoch = context->Recovery().Current();
    REQUIRE_FALSE(epoch->Devices().empty());
    REQUIRE(std::ranges::any_of(epoch->Devices(), [](const Sora::Compute::RuntimeDeviceBinding& binding) {
        return binding.descriptor.backend == Sora::Compute::BackendKind::Cpu;
    }));
    REQUIRE(epoch->Resources().Inventory().Queues().size() == epoch->Devices().size());
    REQUIRE(epoch->Resources().Inventory().Compilers().size() == epoch->Devices().size());
}

TEST_CASE("System artifact store resolves an isolated platform namespace and creates it on demand",
          "[Sora][Compute][System][Context][ArtifactStore]") {
    RestoreArtifactStoreRoot restore;
    TemporaryDirectory temporary;
    REQUIRE(Sora::PAL::WriteEnvironmentVariable(Sora::Compute::kSystemArtifactStoreRootEnvironment,
                                                temporary.Path().string())
                .has_value());

    Sora::Compute::SystemArtifactStorePolicy policy{.mode = Sora::Compute::SystemArtifactStoreMode::Required,
                                                    .maximumPayloadBytes = 4096,
                                                    .maximumStorageBytes = 8192,
                                                    .maximumEntryCount = 4,
                                                    .durableWrites = false};
    auto resolved = Sora::Compute::ResolveSystemArtifactStoreConfig(policy);
    REQUIRE(resolved.has_value());
    REQUIRE(resolved->has_value());
    const auto expectedRoot = temporary.Path();
    REQUIRE((**resolved).root == expectedRoot);
    REQUIRE((**resolved).maximumStorageBytes == 8192);
    REQUIRE((**resolved).maximumEntryCount == 4);

    Sora::Compute::SystemComputeContextConfig config;
    config.runtime = CpuOnlyRuntime();
    config.automaticArtifacts = policy;
    auto context = Sora::Compute::CreateSystemComputeContext(std::move(config));
    REQUIRE(context.has_value());
    REQUIRE(std::filesystem::is_directory(expectedRoot));
}

TEST_CASE("System artifact store distinguishes disabled opportunistic and required resolution",
          "[Sora][Compute][System][Context][ArtifactStore][Policy]") {
    RestoreArtifactStoreRoot restore;
    REQUIRE(
        Sora::PAL::WriteEnvironmentVariable(Sora::Compute::kSystemArtifactStoreRootEnvironment, "relative-cache-root")
            .has_value());

    auto disabled =
        Sora::Compute::ResolveSystemArtifactStoreConfig({.mode = Sora::Compute::SystemArtifactStoreMode::Disabled,
                                                         .maximumPayloadBytes = 0,
                                                         .maximumStorageBytes = 0,
                                                         .maximumEntryCount = 0,
                                                         .reconciliationPublicationInterval = 0,
                                                         .reconciliationByteInterval = 0});
    REQUIRE(disabled.has_value());
    REQUIRE_FALSE(disabled->has_value());

    Sora::Compute::SystemComputeContextConfig opportunistic;
    opportunistic.runtime = CpuOnlyRuntime();
    opportunistic.automaticArtifacts.mode = Sora::Compute::SystemArtifactStoreMode::Opportunistic;
    auto invalidOpportunistic = Sora::Compute::CreateSystemComputeContext(std::move(opportunistic));
    REQUIRE_FALSE(invalidOpportunistic.has_value());
    REQUIRE(invalidOpportunistic.error().code == Sora::Compute::SystemComputeContextErrorCode::ContextCreationFailed);
    REQUIRE(invalidOpportunistic.error().context.code ==
            Sora::Compute::ComputeContextErrorCode::ArtifactStoreInitializationFailed);
    REQUIRE(invalidOpportunistic.error().context.artifactStore.code ==
            Sora::Compute::ArtifactStoreErrorCode::InvalidConfiguration);

    Sora::Compute::SystemComputeContextConfig required;
    required.runtime = CpuOnlyRuntime();
    required.automaticArtifacts.mode = Sora::Compute::SystemArtifactStoreMode::Required;
    auto failed = Sora::Compute::CreateSystemComputeContext(std::move(required));
    REQUIRE_FALSE(failed.has_value());
    REQUIRE(failed.error().code == Sora::Compute::SystemComputeContextErrorCode::ContextCreationFailed);
    REQUIRE(failed.error().context.code == Sora::Compute::ComputeContextErrorCode::ArtifactStoreInitializationFailed);
    REQUIRE(failed.error().context.artifactStore.code == Sora::Compute::ArtifactStoreErrorCode::InvalidConfiguration);
}

TEST_CASE("System compute context preserves context configuration failures", "[Sora][Compute][System][Context]") {
    Sora::Compute::SystemComputeContextConfig config;
    config.runtime = CpuOnlyRuntime();
    config.context.hostTransfers.measuredSamples = 0;
    auto context = Sora::Compute::CreateSystemComputeContext(std::move(config));
    REQUIRE_FALSE(context.has_value());
    REQUIRE(context.error().code == Sora::Compute::SystemComputeContextErrorCode::ContextCreationFailed);
    REQUIRE(context.error().context.code == Sora::Compute::ComputeContextErrorCode::InvalidConfiguration);
}

TEST_CASE("System compute context preserves artifact store initialization failures",
          "[Sora][Compute][System][Context][ArtifactStore]") {
    Sora::Compute::SystemComputeContextConfig config;
    config.runtime = CpuOnlyRuntime();
    config.context.artifactStore = Sora::Compute::ArtifactStoreConfig{};
    auto context = Sora::Compute::CreateSystemComputeContext(std::move(config));
    REQUIRE_FALSE(context.has_value());
    REQUIRE(context.error().code == Sora::Compute::SystemComputeContextErrorCode::ContextCreationFailed);
    REQUIRE(context.error().context.code == Sora::Compute::ComputeContextErrorCode::ArtifactStoreInitializationFailed);
    REQUIRE(context.error().context.artifactStore.code == Sora::Compute::ArtifactStoreErrorCode::InvalidConfiguration);
}

TEST_CASE("Opportunistic artifact persistence never hides an invalid explicit store",
          "[Sora][Compute][System][Context][ArtifactStore][Validation]") {
    Sora::Compute::SystemComputeContextConfig config;
    config.runtime = CpuOnlyRuntime();
    config.context.artifactStore = Sora::Compute::ArtifactStoreConfig{};
    config.context.artifactStoreInitialization = Sora::Compute::ArtifactStoreInitializationPolicy::Opportunistic;
    auto context = Sora::Compute::CreateSystemComputeContext(std::move(config));
    REQUIRE_FALSE(context.has_value());
    REQUIRE(context.error().code == Sora::Compute::SystemComputeContextErrorCode::ContextCreationFailed);
    REQUIRE(context.error().context.code == Sora::Compute::ComputeContextErrorCode::ArtifactStoreInitializationFailed);
    REQUIRE(context.error().context.artifactStore.code == Sora::Compute::ArtifactStoreErrorCode::InvalidConfiguration);
}

TEST_CASE("Explicit artifact persistence ignores the automatic store policy",
          "[Sora][Compute][System][Context][ArtifactStore][Priority]") {
    TemporaryDirectory temporary;
    Sora::Compute::SystemComputeContextConfig config;
    config.runtime = CpuOnlyRuntime();
    config.context.artifactStore = Sora::Compute::ArtifactStoreConfig{
        .root = temporary.Path(),
        .maximumPayloadBytes = 4096,
        .maximumStorageBytes = 8192,
        .maximumEntryCount = 4,
        .durableWrites = false,
    };
    config.automaticArtifacts.maximumStorageBytes = 0;
    auto context = Sora::Compute::CreateSystemComputeContext(std::move(config));
    REQUIRE(context.has_value());
}

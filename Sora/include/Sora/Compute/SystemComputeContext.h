/**
 * @file SystemComputeContext.h
 * @brief Target-machine discovery and automatic heterogeneous compute-context construction.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Backend/CpuBackend.h>
#include <Sora/Compute/Backend/LlvmCpuCodeProvider.h>
#include <Sora/Compute/Backend/VulkanBackend.h>
#include <Sora/Compute/Execution/ComputeContext.h>
#include <Sora/Compute/Runtime/BackendPluginCatalog.h>
#include <Sora/Compute/Runtime/HeftPlanner.h>
#include <Sora/Compute/Runtime/RuntimeDiscovery.h>
#include <Sora/Compute/SystemArtifactStore.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Compute {

    inline constexpr RuntimeProviderId kSystemCpuRuntimeProvider{0x53595354454D4350ULL, 0x534F524143505531ULL};
    inline constexpr RuntimeProviderId kSystemVulkanRuntimeProvider{0x53595354454D564BULL, 0x534F5241564B3031ULL};

    inline constexpr uint64_t kSystemCpuCodeProviderErrorDomain = 0x534F52414C4C564DULL;
    inline constexpr uint64_t kSystemCpuBackendErrorDomain = 0x534F524143505542ULL;
    inline constexpr uint64_t kSystemVulkanBackendErrorDomain = 0x534F524156554C4BULL;
    inline constexpr uint64_t kSystemIntegrationErrorDomain = 0x534F524153595354ULL;

    /** @brief Policy for selecting the CPU backend's target-native LLVM ORC provider. */
    enum class SystemCpuCodePolicy : uint8_t {
        InterpreterOnly,
        PreferNativeJit,
        RequireNativeJit,
    };

    /** @brief Policy for adding every compatible Vulkan physical device to a system runtime. */
    enum class SystemVulkanPolicy : uint8_t {
        Disabled,
        Opportunistic,
        Required,
    };

    /** @brief Integration-layer failures that do not originate in one concrete backend API. */
    enum class SystemIntegrationErrorCode : uint8_t {
        VulkanBackendNotBuilt = 1,
    };

    /** @brief Owning target-machine discovery policy retained for device-loss reconstruction. */
    struct SystemRuntimeConfig {
        CpuBackendConfig cpu{};
        SystemCpuCodePolicy cpuCode = SystemCpuCodePolicy::PreferNativeJit;
        std::vector<std::filesystem::path> llvmSearchPaths{};
        SystemVulkanPolicy vulkanPolicy = SystemVulkanPolicy::Opportunistic;
        VulkanBackendSetConfig vulkan{};
        std::shared_ptr<const BackendPluginCatalog> backendPlugins{};

        /** @brief Return whether every closed policy enumeration is recognized. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            const bool validCpuCode = cpuCode == SystemCpuCodePolicy::InterpreterOnly ||
                                      cpuCode == SystemCpuCodePolicy::PreferNativeJit ||
                                      cpuCode == SystemCpuCodePolicy::RequireNativeJit;
            const bool validVulkan = vulkanPolicy == SystemVulkanPolicy::Disabled ||
                                     vulkanPolicy == SystemVulkanPolicy::Opportunistic ||
                                     vulkanPolicy == SystemVulkanPolicy::Required;
            return validCpuCode && validVulkan;
        }
    };

    /** @brief Automatic runtime factory that re-discovers the current target machine for every epoch. */
    class SystemRuntimeFactory final {
        struct State;

    public:
        SystemRuntimeFactory() = delete;

        /** @brief Validate and retain one immutable discovery policy. */
        [[nodiscard]] static std::expected<SystemRuntimeFactory, RuntimeDiscoveryError>
        Create(SystemRuntimeConfig config = {}) noexcept;

        /** @brief Return whether this build can materialize the built-in Vulkan backend. */
        [[nodiscard]] static bool VulkanBackendCompiled() noexcept;

        /** @brief Discover CPU and permitted accelerator backends into one unpublished runtime epoch. */
        [[nodiscard]] std::expected<RuntimeEpoch, RuntimeDiscoveryError> Discover(uint64_t sequence) const noexcept;

        /** @brief Re-discover the target machine for one single-flight recovery request. */
        [[nodiscard]] std::expected<RuntimeEpoch, RuntimeRecoveryError>
        operator()(const RuntimeRecoveryRequest& request) const noexcept;

    private:
        explicit SystemRuntimeFactory(std::shared_ptr<const State> state) noexcept : state_(std::move(state)) {}

        std::shared_ptr<const State> state_;
    };

    static_assert(RuntimeEpochRecoveryProvider<SystemRuntimeFactory>);
    static_assert(std::is_nothrow_move_constructible_v<SystemRuntimeFactory>);

    /** @brief Complete policy for automatic discovery, HEFT planning, caching, calibration, and recovery. */
    struct SystemComputeContextConfig {
        SystemRuntimeConfig runtime{};
        ComputeContextConfig context{};
        SystemArtifactStorePolicy automaticArtifacts{};
        HeftPlanningPolicy planning{};
        std::shared_ptr<AutomaticPartitionPlanningCalibration> automaticPlanningCalibration{};
    };

    /** @brief Concrete user-facing context type produced by target-machine discovery. */
    using SystemComputeContext = ComputeContext<HeftPlanner, SystemRuntimeFactory>;

    /** @brief Stage at which automatic system-context construction failed. */
    enum class SystemComputeContextErrorCode : uint8_t {
        RuntimeDiscoveryFailed,
        ContextCreationFailed,
    };

    /** @brief Automatic context failure retaining its exact discovery or context-construction cause. */
    struct SystemComputeContextError {
        SystemComputeContextErrorCode code = SystemComputeContextErrorCode::RuntimeDiscoveryFailed;
        RuntimeDiscoveryError discovery{};
        ComputeContextError context{};

        friend constexpr bool operator==(SystemComputeContextError, SystemComputeContextError) noexcept = default;
    };

    /** @brief Construct an isolated context using current CPU, JIT, GPU, topology, and calibration state. */
    [[nodiscard]] std::expected<SystemComputeContext, SystemComputeContextError>
    CreateSystemComputeContext(SystemComputeContextConfig config = {}) noexcept;

} // namespace Sora::Compute

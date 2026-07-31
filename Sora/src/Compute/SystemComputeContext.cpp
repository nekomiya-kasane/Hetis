/**
 * @file SystemComputeContext.cpp
 * @brief Built-in CPU, LLVM ORC, and optional Vulkan runtime integration.
 * @ingroup Compute
 */

#include <Sora/Compute/SystemComputeContext.h>

#include <cstddef>
#include <new>
#include <span>
#include <stdexcept>
#include <utility>

namespace Sora::Compute {

    namespace {

        [[nodiscard]] constexpr RuntimeDiscoveryError InvalidConfiguration() noexcept {
            return {.code = RuntimeDiscoveryErrorCode::InvalidConfiguration};
        }

        [[nodiscard]] constexpr RuntimeDiscoveryError ProviderFailure(RuntimeProviderId provider, uint64_t domain,
                                                                      uint64_t code, uint64_t detail) noexcept {
            return {.code = RuntimeDiscoveryErrorCode::ProviderFailed,
                    .provider = provider,
                    .providerError = {.domain = domain, .code = code, .detail = detail}};
        }

        [[nodiscard]] constexpr bool MayFallbackFromNativeCode(CompileErrorCode code) noexcept {
            return code == CompileErrorCode::BackendUnavailable || code == CompileErrorCode::InvalidApi ||
                   code == CompileErrorCode::UnsupportedTarget;
        }

        [[nodiscard]] constexpr bool MayFallbackFromVulkan(VulkanBackendErrorCode code) noexcept {
            return code != VulkanBackendErrorCode::InvalidConfiguration && code != VulkanBackendErrorCode::OutOfMemory;
        }

    } // namespace

    struct SystemRuntimeFactory::State final {
        explicit State(SystemRuntimeConfig value) noexcept : config(std::move(value)) {}

        [[nodiscard]] std::expected<RuntimeEpoch, RuntimeDiscoveryError> Discover(uint64_t sequence) const noexcept {
            try {
                const std::shared_ptr<const BackendPluginCatalogSnapshot> pluginSnapshot =
                    config.backendPlugins != nullptr ? config.backendPlugins->Snapshot() : nullptr;
                CpuCodeProvider cpuCode;
                if (config.cpuCode != SystemCpuCodePolicy::InterpreterOnly) {
                    const std::span<const std::filesystem::path> searchPaths{config.llvmSearchPaths};
                    auto nativeCode = CreateLlvmCpuCodeProvider({.searchPaths = searchPaths});
                    if (!nativeCode && (config.cpuCode == SystemCpuCodePolicy::RequireNativeJit ||
                                        !MayFallbackFromNativeCode(nativeCode.error().code))) {
                        return std::unexpected(
                            ProviderFailure(kSystemCpuRuntimeProvider, kSystemCpuCodeProviderErrorDomain,
                                            static_cast<uint64_t>(nativeCode.error().code), nativeCode.error().detail));
                    }
                    if (nativeCode) {
                        cpuCode = std::move(*nativeCode);
                    }
                }

                auto cpu = CpuBackend::Create(config.cpu, std::move(cpuCode));
                if (!cpu) {
                    return std::unexpected(ProviderFailure(kSystemCpuRuntimeProvider, kSystemCpuBackendErrorDomain,
                                                           static_cast<uint64_t>(cpu.error().code),
                                                           cpu.error().detail));
                }
                auto cpuContribution = MakeRuntimeDiscoveryContribution(kSystemCpuRuntimeProvider, *cpu);
                if (!cpuContribution) {
                    return std::unexpected(cpuContribution.error());
                }

                std::vector<RuntimeDiscoveryContribution> contributions;
                const size_t pluginCount = pluginSnapshot != nullptr ? pluginSnapshot->Entries().size() : 0;
                if (pluginCount > contributions.max_size() - 2) {
                    return std::unexpected(RuntimeDiscoveryError{.code = RuntimeDiscoveryErrorCode::CapacityExceeded});
                }
                contributions.reserve(2 + pluginCount);
                contributions.push_back(std::move(*cpuContribution));

                if (config.vulkanPolicy != SystemVulkanPolicy::Disabled) {
#if SORA_COMPUTE_SYSTEM_HAS_VULKAN
                    auto vulkan = VulkanBackend::CreateAll(config.vulkan);
                    if (!vulkan) {
                        if (config.vulkanPolicy == SystemVulkanPolicy::Required ||
                            !MayFallbackFromVulkan(vulkan.error().code)) {
                            return std::unexpected(
                                ProviderFailure(kSystemVulkanRuntimeProvider, kSystemVulkanBackendErrorDomain,
                                                static_cast<uint64_t>(vulkan.error().code), vulkan.error().detail));
                        }
                    } else {
                        auto vulkanContribution =
                            MakeRuntimeDiscoveryContribution(kSystemVulkanRuntimeProvider, *vulkan);
                        if (!vulkanContribution) {
                            return std::unexpected(vulkanContribution.error());
                        }
                        contributions.push_back(std::move(*vulkanContribution));
                    }
#else
                    if (config.vulkanPolicy == SystemVulkanPolicy::Required) {
                        return std::unexpected(ProviderFailure(
                            kSystemVulkanRuntimeProvider, kSystemIntegrationErrorDomain,
                            static_cast<uint64_t>(SystemIntegrationErrorCode::VulkanBackendNotBuilt), 0));
                    }
#endif
                }

                if (pluginSnapshot != nullptr) {
                    for (const BackendPluginCatalogEntry& plugin : pluginSnapshot->Entries()) {
                        auto contribution = plugin.Discover();
                        if (!contribution) {
                            RuntimeDiscoveryError error = contribution.error();
                            error.providerIndex = static_cast<uint32_t>(contributions.size());
                            return std::unexpected(error);
                        }
                        contributions.push_back(std::move(*contribution));
                    }
                }

                auto snapshot = RuntimeDiscoverySnapshot::Create(std::move(contributions));
                if (!snapshot) {
                    return std::unexpected(snapshot.error());
                }
                return std::move(*snapshot).BuildEpoch(sequence);
            } catch (const std::bad_alloc&) {
                return std::unexpected(RuntimeDiscoveryError{.code = RuntimeDiscoveryErrorCode::OutOfMemory});
            } catch (const std::length_error&) {
                return std::unexpected(RuntimeDiscoveryError{.code = RuntimeDiscoveryErrorCode::CapacityExceeded});
            }
        }

        SystemRuntimeConfig config;
    };

    std::expected<SystemRuntimeFactory, RuntimeDiscoveryError>
    SystemRuntimeFactory::Create(SystemRuntimeConfig config) noexcept {
        if (!config.Valid()) {
            return std::unexpected(InvalidConfiguration());
        }
        try {
            return SystemRuntimeFactory{std::make_shared<State>(std::move(config))};
        } catch (const std::bad_alloc&) {
            return std::unexpected(RuntimeDiscoveryError{.code = RuntimeDiscoveryErrorCode::OutOfMemory});
        }
    }

    bool SystemRuntimeFactory::VulkanBackendCompiled() noexcept {
#if SORA_COMPUTE_SYSTEM_HAS_VULKAN
        return true;
#else
        return false;
#endif
    }

    std::expected<RuntimeEpoch, RuntimeDiscoveryError>
    SystemRuntimeFactory::Discover(uint64_t sequence) const noexcept {
        return state_->Discover(sequence);
    }

    std::expected<RuntimeEpoch, RuntimeRecoveryError>
    SystemRuntimeFactory::operator()(const RuntimeRecoveryRequest& request) const noexcept {
        auto replacement = Discover(request.sequence);
        if (!replacement) {
            return std::unexpected(RuntimeRecoveryError{.code = RuntimeRecoveryErrorCode::RuntimeDiscoveryFailed,
                                                        .service = request.failedService,
                                                        .expectedSequence = request.sequence,
                                                        .discovery = replacement.error()});
        }
        return std::move(*replacement);
    }

    std::expected<SystemComputeContext, SystemComputeContextError>
    CreateSystemComputeContext(SystemComputeContextConfig config) noexcept {
        if (!config.context.artifactStore) {
            if (!config.automaticArtifacts.Valid()) {
                return std::unexpected(SystemComputeContextError{
                    .code = SystemComputeContextErrorCode::ContextCreationFailed,
                    .context = ComputeContextError{ComputeContextErrorCode::InvalidConfiguration}});
            }
            auto automatic = ResolveSystemArtifactStoreConfig(config.automaticArtifacts);
            if (!automatic) {
                const bool mayFallback = config.automaticArtifacts.mode == SystemArtifactStoreMode::Opportunistic &&
                                         automatic.error().code == ArtifactStoreErrorCode::IoError;
                if (!mayFallback) {
                    return std::unexpected(SystemComputeContextError{
                        .code = SystemComputeContextErrorCode::ContextCreationFailed,
                        .context =
                            ComputeContextError{
                                .code = ComputeContextErrorCode::ArtifactStoreInitializationFailed,
                                .artifactStore = automatic.error(),
                            },
                    });
                }
            } else if (*automatic) {
                config.context.artifactStore = std::move(**automatic);
                config.context.artifactStoreInitialization =
                    config.automaticArtifacts.mode == SystemArtifactStoreMode::Required
                        ? ArtifactStoreInitializationPolicy::Required
                        : ArtifactStoreInitializationPolicy::Opportunistic;
            }
        }
        std::shared_ptr<AutomaticPartitionPlanningCalibration> planningCalibration =
            std::move(config.automaticPlanningCalibration);
        if (planningCalibration == nullptr) {
            try {
                planningCalibration = std::make_shared<AutomaticPartitionPlanningCalibration>();
            } catch (const std::bad_alloc&) {
                return std::unexpected(
                    SystemComputeContextError{.code = SystemComputeContextErrorCode::ContextCreationFailed,
                                              .context = ComputeContextError{ComputeContextErrorCode::OutOfMemory}});
            }
        }
        auto runtime = SystemRuntimeFactory::Create(std::move(config.runtime));
        if (!runtime) {
            return std::unexpected(SystemComputeContextError{
                .code = SystemComputeContextErrorCode::RuntimeDiscoveryFailed, .discovery = runtime.error()});
        }
        auto epoch = runtime->Discover(1);
        if (!epoch) {
            return std::unexpected(SystemComputeContextError{
                .code = SystemComputeContextErrorCode::RuntimeDiscoveryFailed, .discovery = epoch.error()});
        }
        auto context =
            CreateComputeContext(std::move(*epoch), config.context,
                                 HeftPlanner{config.planning, std::move(planningCalibration)}, std::move(*runtime));
        if (!context) {
            return std::unexpected(SystemComputeContextError{
                .code = SystemComputeContextErrorCode::ContextCreationFailed, .context = context.error()});
        }
        return std::move(*context);
    }

} // namespace Sora::Compute

/**
 * @file PhysicalResourceInventory.h
 * @brief Immutable value-semantic inventory exposed to pure physical planners.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Artifact.h>
#include <Sora/Compute/Backend/Pipeline.h>
#include <Sora/Compute/Backend/Queue.h>
#include <Sora/Compute/Memory/Memory.h>
#include <Sora/Compute/Memory/Transfer.h>

#include <algorithm>
#include <ranges>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Compute {

    class PhysicalResourceCatalog;

    /** @brief Allocation-free queue facts with no callback or backend-state access. */
    struct PhysicalQueueDescriptor {
        BackendServiceId service{};
        QueueId queue{};
        DeviceId device{};
        BackendKind backend = BackendKind::Cpu;
        QueueCapabilityMask capabilities{};
        ArtifactKind dispatchArtifactKind = ArtifactKind::DeviceExecutable;
        bool reportsExecutionNanoseconds = false;

        friend constexpr bool operator==(PhysicalQueueDescriptor, PhysicalQueueDescriptor) noexcept = default;
    };

    /** @brief Allocation domain and supported properties available to a planner. */
    struct PhysicalMemoryDescriptor {
        BackendServiceId service{};
        MemoryLocation location{};
        MemoryPropertyMask supportedProperties{};

        friend constexpr bool operator==(PhysicalMemoryDescriptor, PhysicalMemoryDescriptor) noexcept = default;
    };

    /** @brief Directed transfer topology without callable backend state or calibration. */
    struct PhysicalTransferDescriptor {
        BackendServiceId sourceService{};
        BackendServiceId service{};
        TransferEngineId engine{};
        MemoryLocation source{};
        MemoryLocation destination{};
        TransferKind kind = TransferKind::Copy;
        BackendKind completionBackend = BackendKind::Cpu;
        DeviceId completionDevice{};

        /** @brief Return whether this value describes one complete directed topology edge. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            const bool validCompletion = kind == TransferKind::CoherentAlias ||
                                         (ValidBackendKind(completionBackend) && completionDevice.Valid());
            const bool validAlias = kind != TransferKind::CoherentAlias || source.domain == destination.domain;
            const bool serviceTransition = sourceService != service || source != destination;
            return sourceService.Valid() && service.Valid() && engine.Valid() && source.Valid() &&
                   destination.Valid() && serviceTransition && ValidTransferKind(kind) && validCompletion && validAlias;
        }

        friend constexpr bool operator==(PhysicalTransferDescriptor, PhysicalTransferDescriptor) noexcept = default;
    };

    /** @brief Compiler identity and persistence capability available to placement. */
    struct PhysicalCompilerDescriptor {
        BackendServiceId service{};
        SemanticHash compiler{};
        BackendKind backend = BackendKind::Cpu;
        ExecutableTargetDomain executableDomain = ExecutableTargetDomain::TargetNative;
        bool supportsPersistentArtifacts = false;

        friend constexpr bool operator==(PhysicalCompilerDescriptor, PhysicalCompilerDescriptor) noexcept = default;
    };

    /** @brief Device pipeline factory identity available to physical planning. */
    struct PhysicalPipelineFactoryDescriptor {
        BackendServiceId service{};
        SemanticHash factory{};
        DeviceId device{};
        BackendKind backend = BackendKind::Cpu;
        bool supportsPersistentArtifacts = false;

        friend constexpr bool operator==(PhysicalPipelineFactoryDescriptor,
                                         PhysicalPipelineFactoryDescriptor) noexcept = default;
    };

    /**
     * @brief Immutable pure-data projection of one validated physical resource catalog.
     * @details The inventory intentionally omits callbacks, native state pointers, and mutating facades. It can be
     * shared with concurrent planners without granting authority to compile, allocate, transfer, or submit.
     */
    class PhysicalResourceInventory final {
    private:
        friend class PhysicalResourceCatalog;

        [[nodiscard]] static constexpr auto
        PipelineConfigurationTargetOrderKey(BackendServiceId service, BackendKind backend, DeviceId device) noexcept {
            return std::tuple{service.low, service.high, std::to_underlying(backend), device.low, device.high};
        }

        [[nodiscard]] static constexpr auto CompilerTargetOrderKey(BackendServiceId service,
                                                                   BackendKind backend) noexcept {
            return std::tuple{service.low, service.high, std::to_underlying(backend)};
        }

        [[nodiscard]] static constexpr auto QueueTargetOrderKey(BackendServiceId service, BackendKind backend,
                                                                DeviceId device) noexcept {
            return std::tuple{service.low, service.high, std::to_underlying(backend), device};
        }

        [[nodiscard]] static constexpr auto
        PipelineConfigurationOrderKey(const PipelineConfiguration& configuration) noexcept {
            return std::tuple{
                PipelineConfigurationTargetOrderKey(configuration.service, configuration.backend, configuration.device),
                configuration.factory.low, configuration.factory.high, configuration.state.low,
                configuration.state.high};
        }

    public:
        [[nodiscard]] std::span<const PhysicalQueueDescriptor> Queues() const noexcept { return queues_; }
        [[nodiscard]] std::span<const PhysicalMemoryDescriptor> Memories() const noexcept { return memories_; }
        [[nodiscard]] std::span<const PhysicalTransferDescriptor> Transfers() const noexcept { return transfers_; }
        [[nodiscard]] std::span<const PhysicalCompilerDescriptor> Compilers() const noexcept { return compilers_; }
        [[nodiscard]] std::span<const PhysicalPipelineFactoryDescriptor> PipelineFactories() const noexcept {
            return pipelineFactories_;
        }
        [[nodiscard]] std::span<const PipelineConfiguration> PipelineConfigurations() const noexcept {
            return pipelineConfigurations_;
        }

        /** @brief Return the canonical compiler variants available in one exact backend service. */
        [[nodiscard]] std::span<const PhysicalCompilerDescriptor> CompilersFor(BackendServiceId service,
                                                                               BackendKind backend) const noexcept {
            const auto key = CompilerTargetOrderKey(service, backend);
            const auto range =
                std::ranges::equal_range(compilers_, key, {}, [](const PhysicalCompilerDescriptor& compiler) {
                    return CompilerTargetOrderKey(compiler.service, compiler.backend);
                });
            return {range.begin(), static_cast<size_t>(range.end() - range.begin())};
        }

        /** @brief Return the canonical queues available on one exact backend device. */
        [[nodiscard]] std::span<const PhysicalQueueDescriptor> QueuesFor(BackendServiceId service, BackendKind backend,
                                                                         DeviceId device) const noexcept {
            const auto key = QueueTargetOrderKey(service, backend, device);
            const auto range = std::ranges::equal_range(queues_, key, {}, [](const PhysicalQueueDescriptor& queue) {
                return QueueTargetOrderKey(queue.service, queue.backend, queue.device);
            });
            return {range.begin(), static_cast<size_t>(range.end() - range.begin())};
        }

        /** @brief Return the contiguous configurations catalogued for one exact backend target. */
        [[nodiscard]] std::span<const PipelineConfiguration>
        PipelineConfigurationsFor(BackendServiceId service, BackendKind backend, DeviceId device) const noexcept {
            const auto key = PipelineConfigurationTargetOrderKey(service, backend, device);
            const auto range = std::ranges::equal_range(
                pipelineConfigurations_, key, {}, [](const PipelineConfiguration& configuration) {
                    return PipelineConfigurationTargetOrderKey(configuration.service, configuration.backend,
                                                               configuration.device);
                });
            return {range.begin(), static_cast<size_t>(range.end() - range.begin())};
        }

        /** @brief Return whether the catalog contains one exact factory, state, and target configuration. */
        [[nodiscard]] bool HasPipelineConfiguration(const PipelineConfiguration& configuration) const noexcept {
            const auto key = PipelineConfigurationOrderKey(configuration);
            const auto found =
                std::ranges::lower_bound(pipelineConfigurations_, key, {}, [](const PipelineConfiguration& candidate) {
                    return PipelineConfigurationOrderKey(candidate);
                });
            return found != pipelineConfigurations_.end() && PipelineConfigurationOrderKey(*found) == key;
        }

    private:
        PhysicalResourceInventory(std::vector<PhysicalQueueDescriptor> queues,
                                  std::vector<PhysicalMemoryDescriptor> memories,
                                  std::vector<PhysicalTransferDescriptor> transfers,
                                  std::vector<PhysicalCompilerDescriptor> compilers,
                                  std::vector<PhysicalPipelineFactoryDescriptor> pipelineFactories,
                                  std::vector<PipelineConfiguration> pipelineConfigurations) noexcept
            : queues_(std::move(queues)),
              memories_(std::move(memories)),
              transfers_(std::move(transfers)),
              compilers_(std::move(compilers)),
              pipelineFactories_(std::move(pipelineFactories)),
              pipelineConfigurations_(std::move(pipelineConfigurations)) {}

        std::vector<PhysicalQueueDescriptor> queues_{};
        std::vector<PhysicalMemoryDescriptor> memories_{};
        std::vector<PhysicalTransferDescriptor> transfers_{};
        std::vector<PhysicalCompilerDescriptor> compilers_{};
        std::vector<PhysicalPipelineFactoryDescriptor> pipelineFactories_{};
        std::vector<PipelineConfiguration> pipelineConfigurations_{};
    };

    static_assert(std::is_trivially_copyable_v<PhysicalQueueDescriptor>);
    static_assert(std::is_trivially_copyable_v<PhysicalMemoryDescriptor>);
    static_assert(std::is_trivially_copyable_v<PhysicalTransferDescriptor>);
    static_assert(std::is_trivially_copyable_v<PhysicalCompilerDescriptor>);
    static_assert(std::is_trivially_copyable_v<PhysicalPipelineFactoryDescriptor>);

} // namespace Sora::Compute

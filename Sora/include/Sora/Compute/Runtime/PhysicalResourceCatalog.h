/**
 * @file PhysicalResourceCatalog.h
 * @brief Immutable validated index of physical compute services.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Backend.h>
#include <Sora/Compute/Backend/DeviceObserver.h>
#include <Sora/Compute/Backend/HostMemoryApi.h>
#include <Sora/Compute/Backend/MemoryApi.h>
#include <Sora/Compute/Backend/Pipeline.h>
#include <Sora/Compute/Backend/Queue.h>
#include <Sora/Compute/Backend/TransferApi.h>
#include <Sora/Compute/Runtime/PhysicalResourceInventory.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>
#include <vector>

namespace Sora::Compute {

    /** @brief All owning physical resources transferred into one immutable catalog.
     */
    struct PhysicalResourceCatalogStorage {
        std::vector<DeviceObserver> deviceObservers;
        std::vector<Queue> queues;
        std::vector<MemoryAllocator> allocators;
        std::vector<HostMemoryAccessor> hostMemoryAccessors;
        std::vector<TransferEngine> transferEngines;
        std::vector<BackendCompiler> compilers;
        std::vector<BackendPipelineFactory> pipelineFactories;
        std::vector<PipelineConfiguration> pipelineConfigurations;
    };

    /** @brief Catalog construction failure detected before physical graph
     * materialization. */
    enum class PhysicalResourceCatalogError : uint8_t {
        InvalidQueue,
        InvalidDeviceObserver,
        DuplicateDeviceObserver,
        DuplicateQueue,
        DuplicateAllocator,
        DuplicateHostMemoryAccessor,
        DuplicateTransferEngine,
        DuplicateCompiler,
        DuplicatePipelineFactory,
        InvalidPipelineConfiguration,
        DuplicatePipelineConfiguration,
        MissingPipelineFactory,
        MissingDeviceObserver,
        IncompatibleDeviceObserver,
        MissingDestinationAllocator,
        MissingHostAllocator,
        OutOfMemory,
    };

    /** @brief Immutable deterministic lookup index for physical graph
     * materialization. */
    class PhysicalResourceCatalog final {
    public:
        /** @brief Sort, validate, and take ownership of all physical resources. */
        [[nodiscard]] static std::expected<PhysicalResourceCatalog, PhysicalResourceCatalogError>
        Create(PhysicalResourceCatalogStorage storage) noexcept;

        /** @brief Find the single physical device generation owned by @p service. */
        [[nodiscard]] const DeviceObserver* FindDeviceObserver(BackendServiceId service) const noexcept;

        /** @brief Return the immutable pure-data projection available to physical planners. */
        [[nodiscard]] const PhysicalResourceInventory& Inventory() const noexcept { return inventory_; }

        /** @brief Return the number of service-scoped device observers in this catalog. */
        [[nodiscard]] size_t DeviceObserverCount() const noexcept { return storage_.deviceObservers.size(); }

        /** @brief Find one exact service-scoped queue identity on @p device. */
        [[nodiscard]] const Queue* FindQueue(BackendServiceId service, BackendKind backend, DeviceId device,
                                             QueueId id) const noexcept;

        /** @brief Select the smallest queue identity supporting @p command on @p
         * device. */
        [[nodiscard]] const Queue* SelectQueue(BackendServiceId service, BackendKind backend, DeviceId device,
                                               BackendCommandKind command) const noexcept;

        /** @brief Find the allocator for one exact service-scoped backend-addressable memory location. */
        [[nodiscard]] const MemoryAllocator* FindAllocator(BackendServiceId service,
                                                           MemoryLocation location) const noexcept;

        /** @brief Find coherent host-mapping authority for one exact service-scoped location. */
        [[nodiscard]] const HostMemoryAccessor* FindHostMemoryAccessor(BackendServiceId service,
                                                                       MemoryLocation location) const noexcept;

        /** @brief Return all coherent host accessors owned by this catalog in canonical order. */
        [[nodiscard]] std::span<const HostMemoryAccessor> HostMemoryAccessors() const noexcept {
            return storage_.hostMemoryAccessors;
        }

        /** @brief Find one directed transfer engine by destination service and stable local identity. */
        [[nodiscard]] const TransferEngine* FindTransferEngine(BackendServiceId service,
                                                               TransferEngineId id) const noexcept;

        /** @brief Find a compiler in one exact backend service context. */
        [[nodiscard]] const BackendCompiler* FindCompiler(BackendServiceId service, BackendKind backend,
                                                          SemanticHash identity) const noexcept;

        /** @brief Find a pipeline factory in one exact backend service and device
         * context. */
        [[nodiscard]] const BackendPipelineFactory* FindPipelineFactory(BackendServiceId service, BackendKind backend,
                                                                        DeviceId device,
                                                                        SemanticHash identity) const noexcept;

    private:
        PhysicalResourceCatalog(PhysicalResourceCatalogStorage storage, PhysicalResourceInventory inventory) noexcept
            : storage_(std::move(storage)), inventory_(std::move(inventory)) {}

        PhysicalResourceCatalogStorage storage_;
        PhysicalResourceInventory inventory_;
    };

} // namespace Sora::Compute

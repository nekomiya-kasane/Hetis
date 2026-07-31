/**
 * @file PhysicalResourceCatalog.cpp
 * @brief Validation and deterministic indexing for physical compute resources.
 * @ingroup Compute
 */

#include <Sora/Compute/Runtime/PhysicalResourceCatalog.h>

#include <algorithm>
#include <new>
#include <ranges>
#include <tuple>
#include <utility>

namespace Sora::Compute {

    namespace {

        [[nodiscard]] auto QueueKey(const Queue& queue) noexcept {
            const QueueView view = queue.View();
            return std::tuple{view.service.low, view.service.high, std::to_underlying(view.backend), view.device,
                              view.id};
        }

        [[nodiscard]] auto AllocatorKey(const MemoryAllocator& allocator) noexcept {
            const BackendServiceId service = allocator.Service();
            return std::tuple{service.low, service.high, allocator.Location()};
        }

        [[nodiscard]] auto HostMemoryAccessorKey(const HostMemoryAccessor& accessor) noexcept {
            const BackendServiceId service = accessor.Service();
            return std::tuple{service.low, service.high, accessor.Location()};
        }

        [[nodiscard]] auto TransferEngineKey(const TransferEngine& engine) noexcept {
            const BackendServiceId service = engine.Service();
            return std::tuple{service.low, service.high, engine.Id()};
        }

        [[nodiscard]] BackendServiceId DeviceObserverKey(const DeviceObserver& observer) noexcept {
            return observer.Service();
        }

        [[nodiscard]] auto CompilerKey(const BackendCompiler& compiler) noexcept {
            const BackendServiceId service = compiler.Service();
            const SemanticHash identity = compiler.Identity();
            return std::tuple{service.low, service.high, std::to_underlying(compiler.Backend()), identity.low,
                              identity.high};
        }

        [[nodiscard]] auto PipelineFactoryKey(const BackendPipelineFactory& factory) noexcept {
            const BackendServiceId service = factory.Service();
            const DeviceId device = factory.Device();
            const SemanticHash identity = factory.Identity();
            return std::tuple{service.low,  service.high, std::to_underlying(factory.Backend()),
                              device.low,   device.high,  identity.low,
                              identity.high};
        }

        [[nodiscard]] auto PipelineFactoryConfigurationKey(const PipelineConfiguration& configuration) noexcept {
            return std::tuple{
                configuration.service.low, configuration.service.high, std::to_underlying(configuration.backend),
                configuration.device.low,  configuration.device.high,  configuration.factory.low,
                configuration.factory.high};
        }

        [[nodiscard]] bool HasPipelineFactory(std::span<const BackendPipelineFactory> factories,
                                              const PipelineConfiguration& configuration) noexcept {
            const auto key = PipelineFactoryConfigurationKey(configuration);
            const auto found = std::ranges::lower_bound(factories, key, {}, PipelineFactoryKey);
            return found != factories.end() && PipelineFactoryKey(*found) == key;
        }

        [[nodiscard]] bool HasAllocator(const std::vector<MemoryAllocator>& allocators, BackendServiceId service,
                                        MemoryLocation location) noexcept {
            const auto key = std::tuple{service.low, service.high, location};
            const auto found = std::ranges::lower_bound(allocators, key, {}, AllocatorKey);
            return found != allocators.end() && AllocatorKey(*found) == key;
        }

        [[nodiscard]] const DeviceObserver* FindObserverIn(std::span<const DeviceObserver> observers,
                                                           BackendServiceId service) noexcept {
            const auto found = std::ranges::lower_bound(observers, service, {}, DeviceObserverKey);
            return found != observers.end() && found->Service() == service ? std::addressof(*found) : nullptr;
        }

    } // namespace

    std::expected<PhysicalResourceCatalog, PhysicalResourceCatalogError>
    PhysicalResourceCatalog::Create(PhysicalResourceCatalogStorage storage) noexcept {
        for (const DeviceObserver& observer : storage.deviceObservers) {
            if (!observer) {
                return std::unexpected(PhysicalResourceCatalogError::InvalidDeviceObserver);
            }
        }
        for (const Queue& queue : storage.queues) {
            if (!queue) {
                return std::unexpected(PhysicalResourceCatalogError::InvalidQueue);
            }
        }

        std::ranges::sort(storage.deviceObservers, {}, DeviceObserverKey);
        std::ranges::sort(storage.queues, {}, QueueKey);
        std::ranges::sort(storage.allocators, {}, AllocatorKey);
        std::ranges::sort(storage.hostMemoryAccessors, {}, HostMemoryAccessorKey);
        std::ranges::sort(storage.transferEngines, {}, TransferEngineKey);
        std::ranges::sort(storage.compilers, {}, CompilerKey);
        std::ranges::sort(storage.pipelineFactories, {}, PipelineFactoryKey);
        std::ranges::sort(storage.pipelineConfigurations, {}, [](const PipelineConfiguration& configuration) {
            return PhysicalResourceInventory::PipelineConfigurationOrderKey(configuration);
        });

        if (std::ranges::adjacent_find(storage.deviceObservers, {}, DeviceObserverKey) !=
            storage.deviceObservers.end()) {
            return std::unexpected(PhysicalResourceCatalogError::DuplicateDeviceObserver);
        }
        if (std::ranges::adjacent_find(storage.queues, {}, QueueKey) != storage.queues.end()) {
            return std::unexpected(PhysicalResourceCatalogError::DuplicateQueue);
        }
        if (std::ranges::adjacent_find(storage.allocators, {}, AllocatorKey) != storage.allocators.end()) {
            return std::unexpected(PhysicalResourceCatalogError::DuplicateAllocator);
        }
        if (std::ranges::adjacent_find(storage.hostMemoryAccessors, {}, HostMemoryAccessorKey) !=
            storage.hostMemoryAccessors.end()) {
            return std::unexpected(PhysicalResourceCatalogError::DuplicateHostMemoryAccessor);
        }
        if (std::ranges::adjacent_find(storage.transferEngines, {}, TransferEngineKey) !=
            storage.transferEngines.end()) {
            return std::unexpected(PhysicalResourceCatalogError::DuplicateTransferEngine);
        }
        if (std::ranges::adjacent_find(storage.compilers, {}, CompilerKey) != storage.compilers.end()) {
            return std::unexpected(PhysicalResourceCatalogError::DuplicateCompiler);
        }
        if (std::ranges::adjacent_find(storage.pipelineFactories, {}, PipelineFactoryKey) !=
            storage.pipelineFactories.end()) {
            return std::unexpected(PhysicalResourceCatalogError::DuplicatePipelineFactory);
        }
        for (const PipelineConfiguration& configuration : storage.pipelineConfigurations) {
            if (!configuration.Valid()) {
                return std::unexpected(PhysicalResourceCatalogError::InvalidPipelineConfiguration);
            }
        }
        if (std::ranges::adjacent_find(
                storage.pipelineConfigurations, {}, [](const PipelineConfiguration& configuration) {
                    return PhysicalResourceInventory::PipelineConfigurationOrderKey(configuration);
                }) != storage.pipelineConfigurations.end()) {
            return std::unexpected(PhysicalResourceCatalogError::DuplicatePipelineConfiguration);
        }
        const auto resolveObserver = [&storage](BackendServiceId service) {
            return FindObserverIn(storage.deviceObservers, service);
        };
        for (const Queue& queue : storage.queues) {
            const QueueView view = queue.View();
            const DeviceObserver* observer = resolveObserver(view.service);
            if (observer == nullptr) {
                return std::unexpected(PhysicalResourceCatalogError::MissingDeviceObserver);
            }
            if (observer->Backend() != view.backend || observer->Device() != view.device) {
                return std::unexpected(PhysicalResourceCatalogError::IncompatibleDeviceObserver);
            }
        }
        for (const MemoryAllocator& allocator : storage.allocators) {
            if (resolveObserver(allocator.Service()) == nullptr) {
                return std::unexpected(PhysicalResourceCatalogError::MissingDeviceObserver);
            }
        }
        for (const HostMemoryAccessor& accessor : storage.hostMemoryAccessors) {
            if (resolveObserver(accessor.Service()) == nullptr) {
                return std::unexpected(PhysicalResourceCatalogError::MissingDeviceObserver);
            }
            const MemoryAllocator* allocator = nullptr;
            if (HasAllocator(storage.allocators, accessor.Service(), accessor.Location())) {
                const auto key = std::tuple{accessor.Service().low, accessor.Service().high, accessor.Location()};
                const auto found = std::ranges::lower_bound(storage.allocators, key, {}, AllocatorKey);
                allocator = std::addressof(*found);
            }
            if (allocator == nullptr ||
                !allocator->SupportedProperties().Contains(Detail::kCoherentHostMemoryProperties)) {
                return std::unexpected(PhysicalResourceCatalogError::MissingHostAllocator);
            }
        }
        for (const TransferEngine& engine : storage.transferEngines) {
            if (resolveObserver(engine.SourceService()) == nullptr || resolveObserver(engine.Service()) == nullptr) {
                return std::unexpected(PhysicalResourceCatalogError::MissingDeviceObserver);
            }
        }
        for (const BackendCompiler& compiler : storage.compilers) {
            const DeviceObserver* observer = resolveObserver(compiler.Service());
            if (observer == nullptr) {
                return std::unexpected(PhysicalResourceCatalogError::MissingDeviceObserver);
            }
            if (observer->Backend() != compiler.Backend()) {
                return std::unexpected(PhysicalResourceCatalogError::IncompatibleDeviceObserver);
            }
        }
        for (const BackendPipelineFactory& factory : storage.pipelineFactories) {
            const DeviceObserver* observer = resolveObserver(factory.Service());
            if (observer == nullptr) {
                return std::unexpected(PhysicalResourceCatalogError::MissingDeviceObserver);
            }
            if (observer->Backend() != factory.Backend() || observer->Device() != factory.Device()) {
                return std::unexpected(PhysicalResourceCatalogError::IncompatibleDeviceObserver);
            }
        }
        for (const PipelineConfiguration& configuration : storage.pipelineConfigurations) {
            if (!HasPipelineFactory(storage.pipelineFactories, configuration)) {
                return std::unexpected(PhysicalResourceCatalogError::MissingPipelineFactory);
            }
        }
        for (const TransferEngine& engine : storage.transferEngines) {
            if (engine.Kind() != TransferKind::CoherentAlias &&
                !HasAllocator(storage.allocators, engine.Service(), engine.Destination())) {
                return std::unexpected(PhysicalResourceCatalogError::MissingDestinationAllocator);
            }
        }
        try {
            std::vector<PhysicalQueueDescriptor> queues;
            std::vector<PhysicalMemoryDescriptor> memories;
            std::vector<PhysicalTransferDescriptor> transfers;
            std::vector<PhysicalCompilerDescriptor> compilers;
            std::vector<PhysicalPipelineFactoryDescriptor> pipelineFactories;
            queues.reserve(storage.queues.size());
            memories.reserve(storage.allocators.size());
            transfers.reserve(storage.transferEngines.size());
            compilers.reserve(storage.compilers.size());
            pipelineFactories.reserve(storage.pipelineFactories.size());

            for (const Queue& queue : storage.queues) {
                const QueueView view = queue.View();
                queues.push_back({.service = view.service,
                                  .queue = view.id,
                                  .device = view.device,
                                  .backend = view.backend,
                                  .capabilities = view.capabilities,
                                  .dispatchArtifactKind = view.dispatchArtifactKind,
                                  .reportsExecutionNanoseconds = view.reportsExecutionNanoseconds});
            }
            for (const MemoryAllocator& allocator : storage.allocators) {
                memories.push_back({.service = allocator.Service(),
                                    .location = allocator.Location(),
                                    .supportedProperties = allocator.SupportedProperties()});
            }
            for (const TransferEngine& engine : storage.transferEngines) {
                transfers.push_back({.sourceService = engine.SourceService(),
                                     .service = engine.Service(),
                                     .engine = engine.Id(),
                                     .source = engine.Source(),
                                     .destination = engine.Destination(),
                                     .kind = engine.Kind(),
                                     .completionBackend = engine.CompletionBackend(),
                                     .completionDevice = engine.CompletionDevice()});
            }
            for (const BackendCompiler& compiler : storage.compilers) {
                compilers.push_back({.service = compiler.Service(),
                                     .compiler = compiler.Identity(),
                                     .backend = compiler.Backend(),
                                     .executableDomain = compiler.ExecutableDomain(),
                                     .supportsPersistentArtifacts = compiler.SupportsPersistentArtifacts()});
            }
            for (const BackendPipelineFactory& factory : storage.pipelineFactories) {
                pipelineFactories.push_back({.service = factory.Service(),
                                             .factory = factory.Identity(),
                                             .device = factory.Device(),
                                             .backend = factory.Backend(),
                                             .supportsPersistentArtifacts = factory.SupportsPersistentArtifacts()});
            }
            auto pipelineConfigurations = std::move(storage.pipelineConfigurations);
            PhysicalResourceInventory inventory{
                std::move(queues),    std::move(memories),          std::move(transfers),
                std::move(compilers), std::move(pipelineFactories), std::move(pipelineConfigurations)};
            return PhysicalResourceCatalog{std::move(storage), std::move(inventory)};
        } catch (const std::bad_alloc&) {
            return std::unexpected(PhysicalResourceCatalogError::OutOfMemory);
        }
    }

    const DeviceObserver* PhysicalResourceCatalog::FindDeviceObserver(BackendServiceId service) const noexcept {
        return FindObserverIn(storage_.deviceObservers, service);
    }

    const Queue* PhysicalResourceCatalog::FindQueue(BackendServiceId service, BackendKind backend, DeviceId device,
                                                    QueueId id) const noexcept {
        const auto key = std::tuple{service.low, service.high, std::to_underlying(backend), device, id};
        const auto found = std::ranges::lower_bound(storage_.queues, key, {}, QueueKey);
        return found != storage_.queues.end() && QueueKey(*found) == key ? std::addressof(*found) : nullptr;
    }

    const Queue* PhysicalResourceCatalog::SelectQueue(BackendServiceId service, BackendKind backend, DeviceId device,
                                                      BackendCommandKind command) const noexcept {
        const auto key = std::tuple{service.low, service.high, std::to_underlying(backend), device};
        const auto first = std::ranges::lower_bound(storage_.queues, key, {}, [](const Queue& queue) {
            const QueueView view = queue.View();
            return std::tuple{view.service.low, view.service.high, std::to_underlying(view.backend), view.device};
        });
        for (auto queue = first; queue != storage_.queues.end() && queue->View().service == service &&
                                 queue->View().backend == backend && queue->View().device == device;
             ++queue) {
            if (queue->View().capabilities.Supports(command)) {
                return std::addressof(*queue);
            }
        }
        return nullptr;
    }

    const MemoryAllocator* PhysicalResourceCatalog::FindAllocator(BackendServiceId service,
                                                                  MemoryLocation location) const noexcept {
        const auto key = std::tuple{service.low, service.high, location};
        const auto found = std::ranges::lower_bound(storage_.allocators, key, {}, AllocatorKey);
        return found != storage_.allocators.end() && AllocatorKey(*found) == key ? std::addressof(*found) : nullptr;
    }

    const HostMemoryAccessor* PhysicalResourceCatalog::FindHostMemoryAccessor(BackendServiceId service,
                                                                              MemoryLocation location) const noexcept {
        const auto key = std::tuple{service.low, service.high, location};
        const auto found = std::ranges::lower_bound(storage_.hostMemoryAccessors, key, {}, HostMemoryAccessorKey);
        return found != storage_.hostMemoryAccessors.end() && HostMemoryAccessorKey(*found) == key
                   ? std::addressof(*found)
                   : nullptr;
    }

    const TransferEngine* PhysicalResourceCatalog::FindTransferEngine(BackendServiceId service,
                                                                      TransferEngineId id) const noexcept {
        const auto key = std::tuple{service.low, service.high, id};
        const auto found = std::ranges::lower_bound(storage_.transferEngines, key, {}, TransferEngineKey);
        return found != storage_.transferEngines.end() && TransferEngineKey(*found) == key ? std::addressof(*found)
                                                                                           : nullptr;
    }

    const BackendCompiler* PhysicalResourceCatalog::FindCompiler(BackendServiceId service, BackendKind backend,
                                                                 SemanticHash identity) const noexcept {
        const auto key =
            std::tuple{service.low, service.high, std::to_underlying(backend), identity.low, identity.high};
        const auto found = std::ranges::lower_bound(storage_.compilers, key, {}, CompilerKey);
        return found != storage_.compilers.end() && CompilerKey(*found) == key ? std::addressof(*found) : nullptr;
    }

    const BackendPipelineFactory* PhysicalResourceCatalog::FindPipelineFactory(BackendServiceId service,
                                                                               BackendKind backend, DeviceId device,
                                                                               SemanticHash identity) const noexcept {
        const auto key = std::tuple{service.low,  service.high, std::to_underlying(backend), device.low, device.high,
                                    identity.low, identity.high};
        const auto found = std::ranges::lower_bound(storage_.pipelineFactories, key, {}, PipelineFactoryKey);
        return found != storage_.pipelineFactories.end() && PipelineFactoryKey(*found) == key ? std::addressof(*found)
                                                                                              : nullptr;
    }

} // namespace Sora::Compute

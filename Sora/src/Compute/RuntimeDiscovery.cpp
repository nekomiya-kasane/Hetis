/**
 * @file RuntimeDiscovery.cpp
 * @brief Canonical merge and validation of complete backend discovery
 * contributions.
 * @ingroup Compute
 */

#include <Sora/Compute/Runtime/RuntimeDiscovery.h>

#include <algorithm>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <tuple>

namespace Sora::Compute {

    namespace {

        [[nodiscard]] constexpr auto ProviderKey(const RuntimeDiscoveryContribution& contribution) noexcept {
            return std::tuple{contribution.provider.low, contribution.provider.high};
        }

        [[nodiscard]] constexpr RuntimeDiscoveryError Failure(RuntimeDiscoveryErrorCode code,
                                                              uint32_t providerIndex = UINT32_MAX,
                                                              RuntimeProviderId provider = {}) noexcept {
            return {.code = code, .providerIndex = providerIndex, .provider = provider};
        }

        [[nodiscard]] bool AccumulateSize(size_t& total, size_t increment) noexcept {
            if (increment > std::numeric_limits<size_t>::max() - total) {
                return false;
            }
            total += increment;
            return true;
        }

        template<typename T>
        void Append(std::vector<T>& destination, std::vector<T>& source) {
            destination.insert(destination.end(), std::make_move_iterator(source.begin()),
                               std::make_move_iterator(source.end()));
        }

    } // namespace

    std::expected<RuntimeDiscoverySnapshot, RuntimeDiscoveryError>
    RuntimeDiscoverySnapshot::Create(std::vector<RuntimeDiscoveryContribution> contributions) noexcept {
        if (contributions.size() > UINT32_MAX) {
            return std::unexpected(Failure(RuntimeDiscoveryErrorCode::CapacityExceeded));
        }
        for (uint32_t index = 0; index < contributions.size(); ++index) {
            if (!contributions[index].provider.Valid()) {
                return std::unexpected(
                    Failure(RuntimeDiscoveryErrorCode::InvalidProvider, index, contributions[index].provider));
            }
        }
        std::ranges::sort(contributions, {}, ProviderKey);
        for (uint32_t index = 1; index < contributions.size(); ++index) {
            if (contributions[index - 1].provider == contributions[index].provider) {
                return std::unexpected(
                    Failure(RuntimeDiscoveryErrorCode::DuplicateProvider, index, contributions[index].provider));
            }
        }

        try {
            size_t observerCount = 0;
            size_t queueCount = 0;
            size_t allocatorCount = 0;
            size_t hostAccessorCount = 0;
            size_t transferCount = 0;
            size_t compilerCount = 0;
            size_t pipelineFactoryCount = 0;
            size_t pipelineConfigurationCount = 0;
            size_t deviceCount = 0;
            for (const RuntimeDiscoveryContribution& contribution : contributions) {
                const PhysicalResourceCatalogStorage& resources = contribution.resources;
                if (!AccumulateSize(observerCount, resources.deviceObservers.size()) ||
                    !AccumulateSize(queueCount, resources.queues.size()) ||
                    !AccumulateSize(allocatorCount, resources.allocators.size()) ||
                    !AccumulateSize(hostAccessorCount, resources.hostMemoryAccessors.size()) ||
                    !AccumulateSize(transferCount, resources.transferEngines.size()) ||
                    !AccumulateSize(compilerCount, resources.compilers.size()) ||
                    !AccumulateSize(pipelineFactoryCount, resources.pipelineFactories.size()) ||
                    !AccumulateSize(pipelineConfigurationCount, resources.pipelineConfigurations.size()) ||
                    !AccumulateSize(deviceCount, contribution.devices.size())) {
                    return std::unexpected(Failure(RuntimeDiscoveryErrorCode::CapacityExceeded));
                }
            }
            if (deviceCount > UINT32_MAX) {
                return std::unexpected(Failure(RuntimeDiscoveryErrorCode::CapacityExceeded));
            }
            if (hostAccessorCount > 1 &&
                hostAccessorCount > std::numeric_limits<size_t>::max() / (hostAccessorCount - 1U)) {
                return std::unexpected(Failure(RuntimeDiscoveryErrorCode::CapacityExceeded));
            }
            if (!AccumulateSize(transferCount,
                                hostAccessorCount * (hostAccessorCount - std::min<size_t>(hostAccessorCount, 1)))) {
                return std::unexpected(Failure(RuntimeDiscoveryErrorCode::CapacityExceeded));
            }

            std::vector<RuntimeProviderId> providers;
            providers.reserve(contributions.size());
            PhysicalResourceCatalogStorage resources;
            resources.deviceObservers.reserve(observerCount);
            resources.queues.reserve(queueCount);
            resources.allocators.reserve(allocatorCount);
            resources.hostMemoryAccessors.reserve(hostAccessorCount);
            resources.transferEngines.reserve(transferCount);
            resources.compilers.reserve(compilerCount);
            resources.pipelineFactories.reserve(pipelineFactoryCount);
            resources.pipelineConfigurations.reserve(pipelineConfigurationCount);
            std::vector<RuntimeDeviceBinding> devices;
            devices.reserve(deviceCount);
            for (RuntimeDiscoveryContribution& contribution : contributions) {
                providers.push_back(contribution.provider);
                Append(resources.deviceObservers, contribution.resources.deviceObservers);
                Append(resources.queues, contribution.resources.queues);
                Append(resources.allocators, contribution.resources.allocators);
                Append(resources.hostMemoryAccessors, contribution.resources.hostMemoryAccessors);
                Append(resources.transferEngines, contribution.resources.transferEngines);
                Append(resources.compilers, contribution.resources.compilers);
                Append(resources.pipelineFactories, contribution.resources.pipelineFactories);
                Append(resources.pipelineConfigurations, contribution.resources.pipelineConfigurations);
                Append(devices, contribution.devices);
            }
            auto hostBridges = CreateHostTransferBridges(resources.hostMemoryAccessors);
            if (!hostBridges) {
                RuntimeDiscoveryError error = Failure(RuntimeDiscoveryErrorCode::HostBridgeConstructionFailed);
                error.hostBridge = hostBridges.error();
                return std::unexpected(error);
            }
            Append(resources.transferEngines, *hostBridges);
            auto catalog = PhysicalResourceCatalog::Create(std::move(resources));
            if (!catalog) {
                RuntimeDiscoveryError error = Failure(RuntimeDiscoveryErrorCode::CatalogRejected);
                error.catalog = catalog.error();
                return std::unexpected(error);
            }
            return RuntimeDiscoverySnapshot{std::move(providers), std::move(*catalog), std::move(devices)};
        } catch (const std::bad_alloc&) {
            return std::unexpected(Failure(RuntimeDiscoveryErrorCode::OutOfMemory));
        } catch (const std::length_error&) {
            return std::unexpected(Failure(RuntimeDiscoveryErrorCode::CapacityExceeded));
        }
    }

    std::expected<RuntimeEpoch, RuntimeDiscoveryError>
    RuntimeDiscoverySnapshot::BuildEpoch(uint64_t sequence) && noexcept {
        auto epoch = RuntimeEpoch::Create(sequence, std::move(resources_), std::move(devices_));
        if (!epoch) {
            RuntimeDiscoveryError error = Failure(RuntimeDiscoveryErrorCode::EpochRejected);
            error.epoch = epoch.error();
            return std::unexpected(error);
        }
        return std::move(*epoch);
    }

} // namespace Sora::Compute

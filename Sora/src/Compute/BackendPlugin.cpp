/**
 * @file BackendPlugin.cpp
 * @brief Negotiation, ownership, and transactional projection of dynamic backend plugins.
 * @ingroup Compute
 */

#include <Sora/Compute/Runtime/BackendPlugin.h>

#include <algorithm>
#include <limits>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace Sora::Compute {

    namespace {

        [[nodiscard]] constexpr bool ValidPluginResultCode(BackendPluginResultCode code) noexcept {
            return code == BackendPluginResultCode::Success || code == BackendPluginResultCode::InvalidRequest ||
                   code == BackendPluginResultCode::IncompatibleAbi || code == BackendPluginResultCode::Incomplete ||
                   code == BackendPluginResultCode::Failed || code == BackendPluginResultCode::OutOfMemory;
        }

        [[nodiscard]] constexpr bool ValidPluginResult(const BackendPluginCallResult& result) noexcept {
            return result.pluginAbiVersion == kBackendPluginAbiVersion &&
                   result.structSize >= sizeof(BackendPluginCallResult) && ValidPluginResultCode(result.code);
        }

        [[nodiscard]] constexpr bool ValidPluginApi(const BackendPluginApiV1& api) noexcept {
            return api.pluginAbiVersion == kBackendPluginAbiVersion && api.structSize >= sizeof(BackendPluginApiV1) &&
                   api.provider.Valid() && api.state != nullptr && api.destroy != nullptr && api.enumerate != nullptr;
        }

        [[nodiscard]] constexpr bool CompleteArray(const void* data, uint32_t count) noexcept {
            return (data == nullptr) == (count == 0);
        }

        [[nodiscard]] constexpr bool CompleteBundle(const BackendPluginServiceBundleV1& bundle) noexcept {
            return bundle.pluginAbiVersion == kBackendPluginAbiVersion &&
                   bundle.structSize >= sizeof(BackendPluginServiceBundleV1) && ValidDeviceDescriptor(bundle.device) &&
                   bundle.target.Valid() && bundle.target.device == bundle.device.id &&
                   bundle.target.backend == bundle.device.backend && bundle.target.features == bundle.device.features &&
                   bundle.target.architectureId == bundle.device.architectureId && bundle.queueCount != 0 &&
                   bundle.allocatorCount != 0 && bundle.compilerCount != 0 &&
                   CompleteArray(bundle.queues, bundle.queueCount) &&
                   CompleteArray(bundle.allocators, bundle.allocatorCount) &&
                   CompleteArray(bundle.hostMemoryAccessors, bundle.hostMemoryAccessorCount) &&
                   CompleteArray(bundle.transferEngines, bundle.transferEngineCount) &&
                   CompleteArray(bundle.compilers, bundle.compilerCount) &&
                   CompleteArray(bundle.pipelines, bundle.pipelineCount);
        }

        [[nodiscard]] constexpr RuntimeDiscoveryError
        PluginFailure(RuntimeProviderId provider, BackendPluginProviderErrorCode code, uint64_t detail = 0) noexcept {
            return {.code = RuntimeDiscoveryErrorCode::ProviderFailed,
                    .provider = provider,
                    .providerError = {.domain = kBackendPluginProviderErrorDomain,
                                      .code = std::to_underlying(code),
                                      .detail = detail}};
        }

        [[nodiscard]] constexpr RuntimeDiscoveryError PluginFailure(RuntimeProviderId provider,
                                                                    RuntimeProviderError error) noexcept {
            return {.code = RuntimeDiscoveryErrorCode::ProviderFailed, .provider = provider, .providerError = error};
        }

        [[nodiscard]] constexpr uint64_t ResourceDetail(uint32_t bundle, uint32_t resource = 0) noexcept {
            return (static_cast<uint64_t>(bundle) << 32U) | resource;
        }

        [[nodiscard]] bool Accumulate(size_t& total, uint32_t increment) noexcept {
            if (increment > std::numeric_limits<size_t>::max() - total) {
                return false;
            }
            total += increment;
            return true;
        }

        void ReleaseService(BackendServicePayload payload) noexcept {
            if (payload.state != nullptr && payload.destroy != nullptr) {
                payload.destroy(payload.state);
            }
        }

        void ReleaseQueue(BackendQueuePayload payload) noexcept {
            if (payload.state != nullptr && payload.destroy != nullptr) {
                payload.destroy(payload.state);
            }
        }

        /** @brief Reclaims every successfully transferred owning payload not yet consumed by a facade. */
        class TransferredBundleGuard final {
        public:
            explicit TransferredBundleGuard(std::span<const BackendPluginServiceBundleV1> bundles) noexcept
                : bundles_(bundles) {}

            TransferredBundleGuard(const TransferredBundleGuard&) = delete;
            TransferredBundleGuard& operator=(const TransferredBundleGuard&) = delete;

            ~TransferredBundleGuard() noexcept {
                if (released_) {
                    return;
                }
                for (size_t index = completedBundles_; index < bundles_.size(); ++index) {
                    const BackendPluginServiceBundleV1& bundle = bundles_[index];
                    const bool current = index == completedBundles_;
                    if (!current || !serviceConsumed_) {
                        ReleaseService(bundle.service);
                    }
                    if (bundle.queues == nullptr) {
                        continue;
                    }
                    const uint32_t first = current ? queuesConsumed_ : 0;
                    for (uint32_t queue = first; queue < bundle.queueCount; ++queue) {
                        ReleaseQueue(bundle.queues[queue]);
                    }
                }
            }

            void ServiceConsumed() noexcept { serviceConsumed_ = true; }
            void QueueConsumed() noexcept { ++queuesConsumed_; }

            void BundleConsumed() noexcept {
                ++completedBundles_;
                serviceConsumed_ = false;
                queuesConsumed_ = 0;
            }

            void Release() noexcept { released_ = true; }

        private:
            std::span<const BackendPluginServiceBundleV1> bundles_;
            size_t completedBundles_ = 0;
            uint32_t queuesConsumed_ = 0;
            bool serviceConsumed_ = false;
            bool released_ = false;
        };

    } // namespace

    struct RuntimeBackendPlugin::Control {
        Control(BackendCodeLease codeOwner, BackendPluginApiV1 root) noexcept : code(std::move(codeOwner)), api(root) {}

        ~Control() noexcept { api.destroy(api.state); }

        std::mutex enumerationMutex;
        BackendCodeLease code;
        BackendPluginApiV1 api;
    };

    std::expected<RuntimeBackendPlugin, BackendPluginLoadError>
    RuntimeBackendPlugin::Adopt(BackendPluginApiV1 api, BackendCodeLease code) noexcept {
        const bool valid = ValidPluginApi(api);
        if (!valid || !code) {
            if (api.state != nullptr && api.destroy != nullptr) {
                api.destroy(api.state);
            }
            return std::unexpected(BackendPluginLoadError{.code = BackendPluginLoadErrorCode::InvalidPluginApi});
        }
        try {
            return RuntimeBackendPlugin{std::make_shared<Control>(std::move(code), api)};
        } catch (const std::bad_alloc&) {
            api.destroy(api.state);
            return std::unexpected(BackendPluginLoadError{.code = BackendPluginLoadErrorCode::OutOfMemory});
        }
    }

    RuntimeProviderId RuntimeBackendPlugin::Provider() const noexcept {
        return control_->api.provider;
    }

    std::expected<RuntimeDiscoveryContribution, RuntimeDiscoveryError>
    RuntimeBackendPlugin::operator()() const noexcept {
        const BackendPluginApiV1 api = control_->api;
        try {
            const std::lock_guard lock(control_->enumerationMutex);
            uint32_t bundleCount = 0;
            BackendPluginCallResult query{};
            api.enumerate(api.state, nullptr, 0, &bundleCount, &query);
            if (!ValidPluginResult(query)) {
                return std::unexpected(
                    PluginFailure(api.provider, BackendPluginProviderErrorCode::InvalidEnumerationResult));
            }
            if (query.code != BackendPluginResultCode::Success) {
                return std::unexpected(PluginFailure(api.provider, query.error));
            }

            RuntimeDiscoveryContribution contribution{.provider = api.provider};
            if (bundleCount == 0) {
                return contribution;
            }

            std::vector<BackendPluginServiceBundleV1> bundles(bundleCount);
            uint32_t written = bundleCount;
            BackendPluginCallResult enumeration{};
            api.enumerate(api.state, bundles.data(), bundleCount, &written, &enumeration);
            if (!ValidPluginResult(enumeration)) {
                return std::unexpected(
                    PluginFailure(api.provider, BackendPluginProviderErrorCode::InvalidEnumerationResult));
            }
            if (enumeration.code != BackendPluginResultCode::Success) {
                if (enumeration.code == BackendPluginResultCode::Incomplete) {
                    return std::unexpected(
                        PluginFailure(api.provider, BackendPluginProviderErrorCode::IncompleteEnumeration, written));
                }
                return std::unexpected(PluginFailure(api.provider, enumeration.error));
            }
            if (written != bundleCount) {
                return std::unexpected(
                    PluginFailure(api.provider, BackendPluginProviderErrorCode::IncompleteEnumeration, written));
            }

            TransferredBundleGuard transferred{bundles};
            size_t queueCount = 0;
            size_t allocatorCount = 0;
            size_t hostAccessorCount = 0;
            size_t transferCount = 0;
            size_t compilerCount = 0;
            size_t pipelineCount = 0;
            std::vector<BackendServiceId> services;
            std::vector<std::tuple<BackendKind, DeviceId>> devices;
            services.reserve(bundleCount);
            devices.reserve(bundleCount);
            for (uint32_t index = 0; index < bundleCount; ++index) {
                const BackendPluginServiceBundleV1& bundle = bundles[index];
                if (!CompleteBundle(bundle)) {
                    return std::unexpected(
                        PluginFailure(api.provider, BackendPluginProviderErrorCode::InvalidBundle, index));
                }
                if (!Accumulate(queueCount, bundle.queueCount) || !Accumulate(allocatorCount, bundle.allocatorCount) ||
                    !Accumulate(hostAccessorCount, bundle.hostMemoryAccessorCount) ||
                    !Accumulate(transferCount, bundle.transferEngineCount) ||
                    !Accumulate(compilerCount, bundle.compilerCount) ||
                    !Accumulate(pipelineCount, bundle.pipelineCount)) {
                    return std::unexpected(
                        PluginFailure(api.provider, BackendPluginProviderErrorCode::CapacityExceeded, index));
                }
                services.push_back(bundle.service.id);
                devices.emplace_back(bundle.device.backend, bundle.device.id);
            }
            std::ranges::sort(services);
            std::ranges::sort(devices);
            if (std::ranges::adjacent_find(services) != services.end() ||
                std::ranges::adjacent_find(devices) != devices.end()) {
                return std::unexpected(PluginFailure(api.provider, BackendPluginProviderErrorCode::InvalidBundle));
            }

            contribution.resources.deviceObservers.reserve(bundleCount);
            contribution.resources.queues.reserve(queueCount);
            contribution.resources.allocators.reserve(allocatorCount);
            contribution.resources.hostMemoryAccessors.reserve(hostAccessorCount);
            contribution.resources.transferEngines.reserve(transferCount);
            contribution.resources.compilers.reserve(compilerCount);
            contribution.resources.pipelineFactories.reserve(pipelineCount);
            contribution.resources.pipelineConfigurations.reserve(pipelineCount);
            contribution.devices.reserve(bundleCount);

            for (uint32_t bundleIndex = 0; bundleIndex < bundleCount; ++bundleIndex) {
                const BackendPluginServiceBundleV1& bundle = bundles[bundleIndex];
                auto service = BackendService::Adopt(bundle.service, control_->code);
                transferred.ServiceConsumed();
                if (!service) {
                    return std::unexpected(PluginFailure(api.provider, BackendPluginProviderErrorCode::InvalidService,
                                                         ResourceDetail(bundleIndex)));
                }

                auto observer = DeviceObserver::Create(*service, bundle.observer);
                if (!observer || observer->Device() != bundle.device.id ||
                    observer->Backend() != bundle.device.backend) {
                    return std::unexpected(PluginFailure(api.provider, BackendPluginProviderErrorCode::InvalidObserver,
                                                         ResourceDetail(bundleIndex)));
                }
                contribution.resources.deviceObservers.push_back(std::move(*observer));
                contribution.devices.push_back(
                    {.service = service->Id(), .descriptor = bundle.device, .target = RuntimeTarget(bundle.target)});

                for (uint32_t index = 0; index < bundle.queueCount; ++index) {
                    auto queue = Queue::Adopt(*service, bundle.queues[index]);
                    transferred.QueueConsumed();
                    if (!queue) {
                        return std::unexpected(PluginFailure(api.provider, BackendPluginProviderErrorCode::InvalidQueue,
                                                             ResourceDetail(bundleIndex, index)));
                    }
                    contribution.resources.queues.push_back(std::move(*queue));
                }
                for (uint32_t index = 0; index < bundle.allocatorCount; ++index) {
                    auto allocator = MemoryAllocator::Create(*service, bundle.allocators[index]);
                    if (!allocator) {
                        return std::unexpected(PluginFailure(api.provider,
                                                             BackendPluginProviderErrorCode::InvalidAllocator,
                                                             ResourceDetail(bundleIndex, index)));
                    }
                    contribution.resources.allocators.push_back(std::move(*allocator));
                }
                for (uint32_t index = 0; index < bundle.hostMemoryAccessorCount; ++index) {
                    auto accessor = HostMemoryAccessor::Create(*service, bundle.hostMemoryAccessors[index]);
                    if (!accessor) {
                        return std::unexpected(PluginFailure(api.provider,
                                                             BackendPluginProviderErrorCode::InvalidHostMemoryAccessor,
                                                             ResourceDetail(bundleIndex, index)));
                    }
                    contribution.resources.hostMemoryAccessors.push_back(std::move(*accessor));
                }
                for (uint32_t index = 0; index < bundle.transferEngineCount; ++index) {
                    auto transfer = TransferEngine::Create(*service, bundle.transferEngines[index]);
                    if (!transfer) {
                        return std::unexpected(PluginFailure(api.provider,
                                                             BackendPluginProviderErrorCode::InvalidTransferEngine,
                                                             ResourceDetail(bundleIndex, index)));
                    }
                    contribution.resources.transferEngines.push_back(std::move(*transfer));
                }
                for (uint32_t index = 0; index < bundle.compilerCount; ++index) {
                    auto compiler = BackendCompiler::Create(*service, bundle.compilers[index]);
                    if (!compiler) {
                        return std::unexpected(PluginFailure(api.provider,
                                                             BackendPluginProviderErrorCode::InvalidCompiler,
                                                             ResourceDetail(bundleIndex, index)));
                    }
                    contribution.resources.compilers.push_back(std::move(*compiler));
                }
                for (uint32_t index = 0; index < bundle.pipelineCount; ++index) {
                    const BackendPluginPipelineV1& pipeline = bundle.pipelines[index];
                    if (pipeline.pluginAbiVersion != kBackendPluginAbiVersion ||
                        pipeline.structSize < sizeof(BackendPluginPipelineV1) || !ValidSemanticHash(pipeline.state)) {
                        return std::unexpected(PluginFailure(api.provider,
                                                             BackendPluginProviderErrorCode::InvalidPipelineFactory,
                                                             ResourceDetail(bundleIndex, index)));
                    }
                    auto factory = BackendPipelineFactory::Create(*service, bundle.target, pipeline.factory);
                    if (!factory) {
                        return std::unexpected(PluginFailure(api.provider,
                                                             BackendPluginProviderErrorCode::InvalidPipelineFactory,
                                                             ResourceDetail(bundleIndex, index)));
                    }
                    contribution.resources.pipelineConfigurations.push_back(factory->Configuration(pipeline.state));
                    contribution.resources.pipelineFactories.push_back(std::move(*factory));
                }
                transferred.BundleConsumed();
            }
            transferred.Release();
            return contribution;
        } catch (const std::bad_alloc&) {
            return std::unexpected(PluginFailure(api.provider, BackendPluginProviderErrorCode::OutOfMemory));
        } catch (const std::length_error&) {
            return std::unexpected(PluginFailure(api.provider, BackendPluginProviderErrorCode::CapacityExceeded));
        } catch (const std::system_error&) {
            return std::unexpected(PluginFailure(api.provider, BackendPluginProviderErrorCode::SynchronizationFailed));
        }
    }

    std::expected<RuntimeBackendPlugin, BackendPluginLoadError>
    LoadRuntimeBackendPlugin(PAL::ModulePtr module) noexcept {
        if (module == nullptr || !*module) {
            return std::unexpected(BackendPluginLoadError{.code = BackendPluginLoadErrorCode::InvalidModule});
        }
        BackendPluginNegotiate negotiate =
            module->TryFindFunction<BackendPluginNegotiate>(kBackendPluginNegotiationExport);
        if (negotiate == nullptr) {
            return std::unexpected(
                BackendPluginLoadError{.code = BackendPluginLoadErrorCode::MissingNegotiationExport});
        }

        const BackendPluginNegotiationRequest request{};
        BackendPluginNegotiationResult result{};
        negotiate(&request, &result);
        if (result.pluginAbiVersion != kBackendPluginAbiVersion ||
            result.structSize < sizeof(BackendPluginNegotiationResult) || !ValidPluginResult(result.result)) {
            return std::unexpected(
                BackendPluginLoadError{.code = BackendPluginLoadErrorCode::InvalidNegotiationResult});
        }
        if (result.result.code != BackendPluginResultCode::Success) {
            const BackendPluginLoadErrorCode code = result.result.code == BackendPluginResultCode::IncompatibleAbi
                                                        ? BackendPluginLoadErrorCode::IncompatibleAbi
                                                        : BackendPluginLoadErrorCode::InvalidNegotiationResult;
            return std::unexpected(BackendPluginLoadError{.code = code, .plugin = result.result});
        }
        return RuntimeBackendPlugin::Adopt(result.api, BackendCodeLease::Retain(std::move(module)));
    }

} // namespace Sora::Compute

/**
 * @file RuntimeDiscovery.h
 * @brief Pure-value backend discovery composition for immutable runtime epochs.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Runtime/HostTransferBridge.h>
#include <Sora/Compute/Runtime/RuntimeProvider.h>
#include <Sora/Compute/Runtime/RuntimeEpoch.h>

#include <concepts>
#include <cstdint>
#include <expected>
#include <functional>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Compute {

    /** @brief One provider's complete owning contribution to a target-machine
     * discovery pass. */
    struct RuntimeDiscoveryContribution {
        RuntimeProviderId provider{};
        PhysicalResourceCatalogStorage resources{};
        std::vector<RuntimeDeviceBinding> devices{};
    };

    /** @brief Stable stage at which construction of a complete discovery snapshot
     * failed. */
    enum class RuntimeDiscoveryErrorCode : uint8_t {
        InvalidConfiguration,
        InvalidProvider,
        ProviderFailed,
        DuplicateProvider,
        CapacityExceeded,
        CatalogRejected,
        HostBridgeConstructionFailed,
        EpochRejected,
        OutOfMemory,
    };

    /** @brief Discovery failure preserving provider position and nested catalog or
     * epoch validation. */
    struct RuntimeDiscoveryError {
        RuntimeDiscoveryErrorCode code = RuntimeDiscoveryErrorCode::InvalidProvider;
        uint32_t providerIndex = UINT32_MAX;
        RuntimeProviderId provider{};
        RuntimeProviderError providerError{};
        PhysicalResourceCatalogError catalog = PhysicalResourceCatalogError::OutOfMemory;
        HostTransferBridgeError hostBridge{};
        RuntimeEpochError epoch{};

        friend constexpr bool operator==(RuntimeDiscoveryError, RuntimeDiscoveryError) noexcept = default;
    };

    /**
     * @brief Validated, complete physical-resource reconstruction input before one
     * epoch observation pass.
     * @details Provider contributions are canonicalized and merged exactly once.
     * The snapshot owns backend service facades through the catalog and remains
     * unpublished until BuildEpoch succeeds.
     */
    class RuntimeDiscoverySnapshot final {
    public:
        RuntimeDiscoverySnapshot(const RuntimeDiscoverySnapshot&) = delete;
        RuntimeDiscoverySnapshot& operator=(const RuntimeDiscoverySnapshot&) = delete;
        RuntimeDiscoverySnapshot(RuntimeDiscoverySnapshot&&) noexcept = default;
        RuntimeDiscoverySnapshot& operator=(RuntimeDiscoverySnapshot&&) noexcept = default;

        /** @brief Validate provider identity, merge all contributions, and construct
         * one complete catalog. */
        [[nodiscard]] static std::expected<RuntimeDiscoverySnapshot, RuntimeDiscoveryError>
        Create(std::vector<RuntimeDiscoveryContribution> contributions) noexcept;

        /** @brief Return canonical provider identities participating in this complete
         * pass. */
        [[nodiscard]] std::span<const RuntimeProviderId> Providers() const noexcept { return providers_; }

        /** @brief Return canonical service, device, and target bindings awaiting
         * observation. */
        [[nodiscard]] std::span<const RuntimeDeviceBinding> Devices() const noexcept { return devices_; }

        /** @brief Observe the complete device set and consume this snapshot into one
         * unpublished epoch. */
        [[nodiscard]] std::expected<RuntimeEpoch, RuntimeDiscoveryError> BuildEpoch(uint64_t sequence) && noexcept;

    private:
        RuntimeDiscoverySnapshot(std::vector<RuntimeProviderId> providers, PhysicalResourceCatalog resources,
                                 std::vector<RuntimeDeviceBinding> devices) noexcept
            : providers_(std::move(providers)), resources_(std::move(resources)), devices_(std::move(devices)) {}

        std::vector<RuntimeProviderId> providers_{};
        PhysicalResourceCatalog resources_;
        std::vector<RuntimeDeviceBinding> devices_{};
    };

    /** @brief Compile-time structural contract for an owning built-in backend
     * resource bundle. */
    template<typename Backend>
    concept RuntimeBackendBundle = requires(const Backend& backend) {
        { backend.Observer() } -> std::same_as<const DeviceObserver&>;
        { backend.Device() } -> std::same_as<const DeviceDescriptor&>;
        { backend.Target() } -> std::same_as<const TargetFingerprint&>;
        { backend.DeviceAllocator() } -> std::same_as<const MemoryAllocator&>;
        { backend.HostAllocator() } -> std::same_as<const MemoryAllocator&>;
        { backend.HostAccessor() } -> std::same_as<const HostMemoryAccessor&>;
        { backend.TransferEngines() } -> std::convertible_to<std::span<const TransferEngine>>;
        { backend.Compiler() } -> std::same_as<const BackendCompiler&>;
        { backend.ExecutionQueue() } -> std::same_as<const Queue&>;
    };

    /** @brief Runtime backend bundle that also materializes an independent pipeline stage. */
    template<typename Backend>
    concept RuntimePipelineBackendBundle = RuntimeBackendBundle<Backend> && requires(const Backend& backend) {
        { backend.PipelineFactory() } -> std::same_as<const BackendPipelineFactory&>;
        { backend.PipelineStateIdentity() } -> std::same_as<SemanticHash>;
    };

    /** @brief Forward range whose elements are complete built-in backend bundles. */
    template<typename Range>
    concept RuntimeBackendRange =
        std::ranges::forward_range<const Range> &&
        RuntimeBackendBundle<std::remove_cvref_t<std::ranges::range_reference_t<const Range>>>;

    namespace Detail {

        template<RuntimeBackendBundle Backend>
        void AppendRuntimeBackend(RuntimeDiscoveryContribution& result, const Backend& backend) {
            result.resources.deviceObservers.push_back(backend.Observer());
            result.resources.queues.push_back(backend.ExecutionQueue());
            result.resources.allocators.push_back(backend.DeviceAllocator());
            result.resources.allocators.push_back(backend.HostAllocator());
            result.resources.hostMemoryAccessors.push_back(backend.HostAccessor());
            const std::span<const TransferEngine> transfers = backend.TransferEngines();
            result.resources.transferEngines.insert(result.resources.transferEngines.end(), transfers.begin(),
                                                    transfers.end());
            result.resources.compilers.push_back(backend.Compiler());
            if constexpr (RuntimePipelineBackendBundle<Backend>) {
                result.resources.pipelineFactories.push_back(backend.PipelineFactory());
                result.resources.pipelineConfigurations.push_back(
                    backend.PipelineFactory().Configuration(backend.PipelineStateIdentity()));
            }
            result.devices.push_back({.service = backend.Observer().Service(),
                                      .descriptor = backend.Device(),
                                      .target = RuntimeTarget(backend.Target())});
        }

    } // namespace Detail

    /**
     * @brief Project one already constructed built-in backend into an owning
     * discovery contribution.
     * @details Facade copies retain the same BackendService and executable-code
     * leases. Optional pipeline resources are selected at compile time and add no
     * type erasure or runtime dispatch.
     */
    template<RuntimeBackendBundle Backend>
    [[nodiscard]] std::expected<RuntimeDiscoveryContribution, RuntimeDiscoveryError>
    MakeRuntimeDiscoveryContribution(RuntimeProviderId provider, const Backend& backend) noexcept {
        if (!provider.Valid()) {
            return std::unexpected(
                RuntimeDiscoveryError{.code = RuntimeDiscoveryErrorCode::InvalidProvider, .provider = provider});
        }
        try {
            RuntimeDiscoveryContribution result{.provider = provider};
            result.resources.deviceObservers.reserve(1);
            result.resources.queues.reserve(1);
            result.resources.allocators.reserve(2);
            result.resources.hostMemoryAccessors.reserve(1);
            const std::span<const TransferEngine> transfers = backend.TransferEngines();
            result.resources.transferEngines.reserve(transfers.size());
            result.resources.compilers.reserve(1);
            if constexpr (RuntimePipelineBackendBundle<Backend>) {
                result.resources.pipelineFactories.reserve(1);
                result.resources.pipelineConfigurations.reserve(1);
            }
            result.devices.reserve(1);
            Detail::AppendRuntimeBackend(result, backend);
            return result;
        } catch (const std::bad_alloc&) {
            return std::unexpected(
                RuntimeDiscoveryError{.code = RuntimeDiscoveryErrorCode::OutOfMemory, .provider = provider});
        } catch (const std::length_error&) {
            return std::unexpected(
                RuntimeDiscoveryError{.code = RuntimeDiscoveryErrorCode::CapacityExceeded, .provider = provider});
        }
    }

    /** @brief Project a complete homogeneous backend set into one provider contribution. */
    template<RuntimeBackendRange Backends>
    [[nodiscard]] std::expected<RuntimeDiscoveryContribution, RuntimeDiscoveryError>
    MakeRuntimeDiscoveryContribution(RuntimeProviderId provider, const Backends& backends) noexcept {
        if (!provider.Valid()) {
            return std::unexpected(
                RuntimeDiscoveryError{.code = RuntimeDiscoveryErrorCode::InvalidProvider, .provider = provider});
        }
        try {
            RuntimeDiscoveryContribution result{.provider = provider};
            if constexpr (std::ranges::sized_range<const Backends>) {
                const size_t count = std::ranges::size(backends);
                if (count > SIZE_MAX / 2) {
                    return std::unexpected(RuntimeDiscoveryError{.code = RuntimeDiscoveryErrorCode::CapacityExceeded,
                                                                 .provider = provider});
                }
                result.resources.deviceObservers.reserve(count);
                result.resources.queues.reserve(count);
                result.resources.allocators.reserve(count * 2);
                result.resources.hostMemoryAccessors.reserve(count);
                size_t transferCount = 0;
                for (const auto& backend : backends) {
                    const std::span<const TransferEngine> transfers = backend.TransferEngines();
                    if (transfers.size() > SIZE_MAX - transferCount) {
                        return std::unexpected(RuntimeDiscoveryError{
                            .code = RuntimeDiscoveryErrorCode::CapacityExceeded, .provider = provider});
                    }
                    transferCount += transfers.size();
                }
                result.resources.transferEngines.reserve(transferCount);
                result.resources.compilers.reserve(count);
                using Backend = std::remove_cvref_t<std::ranges::range_reference_t<const Backends>>;
                if constexpr (RuntimePipelineBackendBundle<Backend>) {
                    result.resources.pipelineFactories.reserve(count);
                    result.resources.pipelineConfigurations.reserve(count);
                }
                result.devices.reserve(count);
            }
            for (const auto& backend : backends) {
                Detail::AppendRuntimeBackend(result, backend);
            }
            return result;
        } catch (const std::bad_alloc&) {
            return std::unexpected(
                RuntimeDiscoveryError{.code = RuntimeDiscoveryErrorCode::OutOfMemory, .provider = provider});
        } catch (const std::length_error&) {
            return std::unexpected(
                RuntimeDiscoveryError{.code = RuntimeDiscoveryErrorCode::CapacityExceeded, .provider = provider});
        }
    }

    /** @brief A noexcept provider producing one complete, owning discovery
     * contribution. */
    template<typename Provider>
    concept RuntimeDiscoveryProvider = requires(Provider& provider) {
        {
            std::invoke(provider)
        } noexcept -> std::same_as<std::expected<RuntimeDiscoveryContribution, RuntimeDiscoveryError>>;
    };

    /**
     * @brief Invoke a compile-time provider set, then atomically validate and build
     * one unpublished runtime epoch.
     * @details Invocation stops at the first provider failure; no partial catalog
     * or epoch escapes.
     */
    template<RuntimeDiscoveryProvider... Providers>
        requires(sizeof...(Providers) > 0)
    [[nodiscard]] std::expected<RuntimeEpoch, RuntimeDiscoveryError> DiscoverRuntime(uint64_t sequence,
                                                                                     Providers&... providers) noexcept {
        try {
            std::vector<RuntimeDiscoveryContribution> contributions;
            contributions.reserve(sizeof...(Providers));
            std::optional<RuntimeDiscoveryError> failure;
            uint32_t providerIndex = 0;
            const auto discover = [&](auto& provider) {
                const uint32_t currentIndex = providerIndex++;
                if (failure) {
                    return;
                }
                auto contribution = std::invoke(provider);
                if (!contribution) {
                    failure = contribution.error();
                    failure->providerIndex = currentIndex;
                    return;
                }
                contributions.push_back(std::move(*contribution));
            };
            (discover(providers), ...);
            if (failure) {
                return std::unexpected(*failure);
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

} // namespace Sora::Compute

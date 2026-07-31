/**
 * @file BackendPluginAbi.h
 * @brief Versioned POD boundary for portable runtime backend plugins.
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
#include <Sora/Compute/Runtime/RuntimeProvider.h>

#include <cstdint>
#include <type_traits>

namespace Sora::Compute {

    inline constexpr uint32_t kBackendPluginAbiVersion = 1;
    inline constexpr char kBackendPluginNegotiationExport[] = "SoraComputeBackendPluginNegotiate";

    /** @brief Stable outcome of one plugin negotiation or enumeration callback. */
    enum class BackendPluginResultCode : uint32_t {
        Success,
        InvalidRequest,
        IncompatibleAbi,
        Incomplete,
        Failed,
        OutOfMemory,
    };

    /** @brief Versioned plugin result carrying an optional provider-defined diagnostic. */
    struct BackendPluginCallResult {
        uint32_t pluginAbiVersion = kBackendPluginAbiVersion;
        uint32_t structSize = sizeof(BackendPluginCallResult);
        BackendPluginResultCode code = BackendPluginResultCode::Failed;
        RuntimeProviderError error{};

        friend constexpr bool operator==(BackendPluginCallResult, BackendPluginCallResult) noexcept = default;
    };

    /** @brief One pipeline factory paired with a planner-visible semantic state. */
    struct BackendPluginPipelineV1 {
        uint32_t pluginAbiVersion = kBackendPluginAbiVersion;
        uint32_t structSize = sizeof(BackendPluginPipelineV1);
        BackendPipelineApiV1 factory{};
        SemanticHash state{};
    };

    /**
     * @brief One complete service/device bundle returned by a plugin enumeration snapshot.
     * @details Array pointers and their callback-table records remain plugin-owned, immutable, and valid from a
     * successful complete fill until the next enumeration begins or the plugin root is destroyed. The host serializes
     * enumeration calls. Ownership of service and queue payloads transfers only when the fill returns Success and its
     * written count exactly matches the preceding query; all other entries remain borrowed value records. Every
     * callback state reachable from a borrowed record must remain valid through the transferred service lifetime.
     */
    struct BackendPluginServiceBundleV1 {
        uint32_t pluginAbiVersion = kBackendPluginAbiVersion;
        uint32_t structSize = sizeof(BackendPluginServiceBundleV1);
        BackendServicePayload service{};
        DeviceDescriptor device{};
        TargetFingerprint target{};
        BackendDeviceObserverApiV4 observer{};
        const BackendQueuePayload* queues = nullptr;
        uint32_t queueCount = 0;
        const BackendMemoryApiV1* allocators = nullptr;
        uint32_t allocatorCount = 0;
        const BackendHostMemoryApiV1* hostMemoryAccessors = nullptr;
        uint32_t hostMemoryAccessorCount = 0;
        const TransferEngineApiV7* transferEngines = nullptr;
        uint32_t transferEngineCount = 0;
        const BackendCompilerApiV5* compilers = nullptr;
        uint32_t compilerCount = 0;
        const BackendPluginPipelineV1* pipelines = nullptr;
        uint32_t pipelineCount = 0;
    };

    using BackendPluginDestroy = void (*)(void* state) noexcept;
    /**
     * @brief Query or fill one complete provider snapshot through host-owned top-level storage.
     * @details A query passes null @p bundles with zero @p capacity. A fill passes exactly the queried capacity. The
     * plugin must not transfer owning payloads unless it returns Success with @p count equal to that capacity.
     */
    using BackendPluginEnumerate = void (*)(void* state, BackendPluginServiceBundleV1* bundles, uint32_t capacity,
                                            uint32_t* count, BackendPluginCallResult* result) noexcept;

    /** @brief Negotiated root callback table for one complete runtime provider. */
    struct BackendPluginApiV1 {
        uint32_t pluginAbiVersion = kBackendPluginAbiVersion;
        uint32_t structSize = sizeof(BackendPluginApiV1);
        RuntimeProviderId provider{};
        void* state = nullptr;
        BackendPluginDestroy destroy = nullptr;
        BackendPluginEnumerate enumerate = nullptr;
    };

    /** @brief Host-supported interface range passed to the plugin root export. */
    struct BackendPluginNegotiationRequest {
        uint32_t structSize = sizeof(BackendPluginNegotiationRequest);
        uint32_t minimumPluginAbiVersion = kBackendPluginAbiVersion;
        uint32_t maximumPluginAbiVersion = kBackendPluginAbiVersion;
        uint32_t backendAbiVersion = kComputeBackendAbiVersion;
    };

    /** @brief Plugin-selected interface and provider root returned to the host. */
    struct BackendPluginNegotiationResult {
        uint32_t pluginAbiVersion = kBackendPluginAbiVersion;
        uint32_t structSize = sizeof(BackendPluginNegotiationResult);
        BackendPluginCallResult result{};
        BackendPluginApiV1 api{};
    };

    using BackendPluginNegotiate = void (*)(const BackendPluginNegotiationRequest* request,
                                            BackendPluginNegotiationResult* result) noexcept;

    static_assert(std::is_trivially_copyable_v<BackendPluginCallResult>);
    static_assert(std::is_trivially_copyable_v<BackendPluginPipelineV1>);
    static_assert(std::is_trivially_copyable_v<BackendPluginServiceBundleV1>);
    static_assert(std::is_trivially_copyable_v<BackendPluginApiV1>);
    static_assert(std::is_trivially_copyable_v<BackendPluginNegotiationRequest>);
    static_assert(std::is_trivially_copyable_v<BackendPluginNegotiationResult>);
    static_assert(std::is_standard_layout_v<BackendPluginCallResult>);
    static_assert(std::is_standard_layout_v<BackendPluginPipelineV1>);
    static_assert(std::is_standard_layout_v<BackendPluginServiceBundleV1>);
    static_assert(std::is_standard_layout_v<BackendPluginApiV1>);
    static_assert(std::is_standard_layout_v<BackendPluginNegotiationRequest>);
    static_assert(std::is_standard_layout_v<BackendPluginNegotiationResult>);

} // namespace Sora::Compute

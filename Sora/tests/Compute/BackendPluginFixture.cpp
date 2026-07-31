/**
 * @file BackendPluginFixture.cpp
 * @brief Minimal dynamic backend plugin used to verify the public POD boundary.
 */

#include <Sora/Compute/Runtime/BackendPluginAbi.h>

#include <array>
#include <cstdint>
#include <new>

namespace {

    using namespace Sora::Compute;

    constexpr RuntimeProviderId kProvider{0x44594E414D49435FULL, 0x504C5547494E5F31ULL};
    constexpr DeviceId kDevice{0x44594E414D4943ULL, 1};
    constexpr BackendServiceId kService{0x44594E414D4943ULL, 2};
    constexpr MemoryLocation kMemory{
        .domain = {0x44594E414D4943ULL, 3}, .device = kDevice, .backend = BackendKind::Vulkan};

    struct ServiceState {};
    struct QueueState {};

    struct PluginState {
        BackendQueuePayload queue{};
        BackendMemoryApiV1 allocator{};
        BackendCompilerApiV5 compiler{};
    };

    void DestroyPlugin(void* opaque) noexcept {
        delete static_cast<PluginState*>(opaque);
    }

    void DestroyService(void* opaque) noexcept {
        delete static_cast<ServiceState*>(opaque);
    }

    void DestroyQueue(void* opaque) noexcept {
        delete static_cast<QueueState*>(opaque);
    }

    void Observe(void*, DeviceSnapshot* snapshot) noexcept {
        *snapshot = {.id = kDevice, .state = DeviceState::Available, .generation = 1};
    }

    void Submit(void*, const BackendSubmitRequest*, BackendSubmitResult*) noexcept {}

    void Allocate(void*, const BackendAllocationRequest*, BackendAllocationResult*) noexcept {}

    void Compile(void*, const BackendCompileRequest*, BackendCompileResult*) noexcept {}

    [[nodiscard]] DeviceDescriptor Descriptor() noexcept {
        return {.id = kDevice,
                .backend = BackendKind::Vulkan,
                .kind = DeviceKind::DiscreteGpu,
                .features = DeviceFeature::Float64,
                .architectureId = 0xA0B0C0D0,
                .totalMemoryBytes = 4ULL << 30U,
                .maximumAllocationBytes = 1ULL << 30U,
                .computeUnitCount = 8,
                .subgroupWidthMinimum = 32,
                .subgroupWidthMaximum = 32,
                .maximumWorkgroupInvocations = 1024};
    }

    [[nodiscard]] TargetFingerprint Target() noexcept {
        return {.device = kDevice,
                .backend = BackendKind::Vulkan,
                .hostArchitecture = Sora::CpuArchitecture::X64,
                .operatingSystem = Sora::OperatingSystem::Windows,
                .binaryFormat = Sora::BinaryFormat::PE,
                .byteOrder = Sora::ByteOrder::Little,
                .abi = Sora::AbiFamily::MSVC,
                .identityStability = TargetIdentityStability::ProcessStable,
                .addressBits = 64,
                .features = DeviceFeature::Float64,
                .architectureId = 0xA0B0C0D0,
                .compilerIdentity = {0x44594E5F434F4D50ULL, 1},
                .driverIdentity = {0x44594E5F44524956ULL, 1}};
    }

    void Enumerate(void* opaque, BackendPluginServiceBundleV1* bundles, uint32_t capacity, uint32_t* count,
                   BackendPluginCallResult* result) noexcept {
        if (count == nullptr || result == nullptr) {
            return;
        }
        if (bundles == nullptr && capacity == 0) {
            *count = 1;
            *result = {.code = BackendPluginResultCode::Success};
            return;
        }
        if (bundles == nullptr || capacity < 1) {
            *count = 1;
            *result = {.code = BackendPluginResultCode::Incomplete};
            return;
        }

        auto& state = *static_cast<PluginState*>(opaque);
        auto* service = new (std::nothrow) ServiceState;
        auto* queue = new (std::nothrow) QueueState;
        if (service == nullptr || queue == nullptr) {
            delete service;
            delete queue;
            *result = {.code = BackendPluginResultCode::OutOfMemory};
            return;
        }
        state.queue = {.id = {0x44594E414D4943ULL, 4},
                       .device = kDevice,
                       .backend = BackendKind::Vulkan,
                       .capabilities = QueueCapability::Dispatch,
                       .state = queue,
                       .destroy = DestroyQueue,
                       .submit = Submit};
        state.allocator = {.location = kMemory,
                           .supportedProperties = MemoryProperty::DeviceLocal,
                           .state = service,
                           .allocate = Allocate};
        state.compiler = {.identity = {0x44594E5F434F4D50ULL, 1},
                          .backend = BackendKind::Vulkan,
                          .state = service,
                          .compile = Compile};
        bundles[0] = {
            .service = {.id = kService, .state = service, .destroy = DestroyService},
            .device = Descriptor(),
            .target = Target(),
            .observer = {.device = kDevice, .backend = BackendKind::Vulkan, .state = service, .observe = Observe},
            .queues = &state.queue,
            .queueCount = 1,
            .allocators = &state.allocator,
            .allocatorCount = 1,
            .compilers = &state.compiler,
            .compilerCount = 1};
        *count = 1;
        *result = {.code = BackendPluginResultCode::Success};
    }

} // namespace

#if defined(_WIN32)
#    define SORA_BACKEND_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#    define SORA_BACKEND_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

SORA_BACKEND_PLUGIN_EXPORT void
SoraComputeBackendPluginNegotiate(const Sora::Compute::BackendPluginNegotiationRequest* request,
                                  Sora::Compute::BackendPluginNegotiationResult* result) noexcept {
    using namespace Sora::Compute;
    if (result == nullptr) {
        return;
    }
    if (request == nullptr || request->structSize < sizeof(BackendPluginNegotiationRequest) ||
        request->minimumPluginAbiVersion > kBackendPluginAbiVersion ||
        request->maximumPluginAbiVersion < kBackendPluginAbiVersion ||
        request->backendAbiVersion != kComputeBackendAbiVersion) {
        *result = {.result = {.code = BackendPluginResultCode::IncompatibleAbi}};
        return;
    }
    auto* state = new (std::nothrow) PluginState;
    if (state == nullptr) {
        *result = {.result = {.code = BackendPluginResultCode::OutOfMemory}};
        return;
    }
    *result = {.result = {.code = BackendPluginResultCode::Success},
               .api = {.provider = kProvider, .state = state, .destroy = DestroyPlugin, .enumerate = Enumerate}};
}

#undef SORA_BACKEND_PLUGIN_EXPORT

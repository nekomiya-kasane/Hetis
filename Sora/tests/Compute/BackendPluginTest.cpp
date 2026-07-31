/**
 * @file BackendPluginTest.cpp
 * @brief Version negotiation, transactional enumeration, and module-lifetime tests for backend plugins.
 */

#include <Sora/Compute/Runtime/BackendPlugin.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

namespace {

    using namespace Sora::Compute;

    constexpr RuntimeProviderId kProvider{0x534F52415F544553ULL, 0x545F504C5547494EULL};
    constexpr DeviceId kDevice{0x504C5547494E, 1};
    constexpr MemoryLocation kMemory{.domain = {0x504C5547494E, 2}, .device = kDevice, .backend = BackendKind::Vulkan};

    enum class EnumerationMode : uint8_t {
        Complete,
        Incomplete,
        InvalidBundle,
        InvalidService,
        InvalidQueue,
        DuplicateIdentity,
        ProviderFailure,
    };

    struct LifecycleCounters {
        uint32_t roots = 0;
        uint32_t services = 0;
        uint32_t queues = 0;
        uint32_t codeOwners = 0;

        friend constexpr bool operator==(LifecycleCounters, LifecycleCounters) noexcept = default;
    };

    struct ServiceState {
        LifecycleCounters* counters = nullptr;
    };

    struct QueueState {
        LifecycleCounters* counters = nullptr;
    };

    struct CodeOwner {
        explicit CodeOwner(LifecycleCounters* value) noexcept : counters(value) {}

        LifecycleCounters* counters = nullptr;
        ~CodeOwner() { ++counters->codeOwners; }
    };

    struct PluginState {
        LifecycleCounters* counters = nullptr;
        EnumerationMode mode = EnumerationMode::Complete;
        std::array<BackendQueuePayload, 2> queues{};
        std::array<BackendMemoryApiV1, 2> allocators{};
        std::array<BackendCompilerApiV5, 2> compilers{};
    };

    void DestroyPlugin(void* opaque) noexcept {
        auto* state = static_cast<PluginState*>(opaque);
        ++state->counters->roots;
        delete state;
    }

    void DestroyService(void* opaque) noexcept {
        auto* state = static_cast<ServiceState*>(opaque);
        ++state->counters->services;
        delete state;
    }

    void DestroyQueue(void* opaque) noexcept {
        auto* state = static_cast<QueueState*>(opaque);
        ++state->counters->queues;
        delete state;
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
                .architectureId = 0x10203040,
                .totalMemoryBytes = 8ULL << 30U,
                .maximumAllocationBytes = 2ULL << 30U,
                .computeUnitCount = 16,
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
                .identityStability = TargetIdentityStability::MachineStable,
                .addressBits = 64,
                .features = DeviceFeature::Float64,
                .architectureId = 0x10203040,
                .compilerIdentity = {0x434F4D50494C4552ULL, 1},
                .driverIdentity = {0x445249564552, 1}};
    }

    [[nodiscard]] bool FillBundle(PluginState& root, uint32_t index, BackendPluginServiceBundleV1& bundle) noexcept {
        auto* service = new (std::nothrow) ServiceState{root.counters};
        auto* queue = new (std::nothrow) QueueState{root.counters};
        if (service == nullptr || queue == nullptr) {
            delete service;
            delete queue;
            return false;
        }

        const BackendServiceId serviceId = root.mode == EnumerationMode::DuplicateIdentity
                                               ? BackendServiceId{0x504C5547494E, 3}
                                               : BackendServiceId{0x504C5547494E, 3 + index};
        root.queues[index] = {.id = {0x504C5547494E, 10 + index},
                              .device = kDevice,
                              .backend = BackendKind::Vulkan,
                              .capabilities = QueueCapability::Dispatch,
                              .state = queue,
                              .destroy = DestroyQueue,
                              .submit = Submit};
        root.allocators[index] = {.location = kMemory,
                                  .supportedProperties = MemoryProperty::DeviceLocal,
                                  .state = service,
                                  .allocate = Allocate};
        root.compilers[index] = {.identity = {0x434F4D50494C4552ULL, 1},
                                 .backend = BackendKind::Vulkan,
                                 .state = service,
                                 .compile = Compile};
        bundle = {.service = {.id = serviceId, .state = service, .destroy = DestroyService},
                  .device = Descriptor(),
                  .target = Target(),
                  .observer = {.device = kDevice, .backend = BackendKind::Vulkan, .state = service, .observe = Observe},
                  .queues = &root.queues[index],
                  .queueCount = 1,
                  .allocators = &root.allocators[index],
                  .allocatorCount = 1,
                  .compilers = &root.compilers[index],
                  .compilerCount = 1};
        if (root.mode == EnumerationMode::InvalidBundle) {
            bundle.compilerCount = 0;
            bundle.compilers = nullptr;
        } else if (root.mode == EnumerationMode::InvalidService) {
            bundle.service.abiVersion = 0;
        } else if (root.mode == EnumerationMode::InvalidQueue) {
            root.queues[index].submit = nullptr;
        }
        return true;
    }

    void Enumerate(void* opaque, BackendPluginServiceBundleV1* bundles, uint32_t capacity, uint32_t* count,
                   BackendPluginCallResult* result) noexcept {
        auto& state = *static_cast<PluginState*>(opaque);
        if (count == nullptr || result == nullptr) {
            return;
        }
        if (state.mode == EnumerationMode::ProviderFailure) {
            *result = {.code = BackendPluginResultCode::Failed, .error = {.domain = 77, .code = 88, .detail = 99}};
            return;
        }
        const uint32_t required = state.mode == EnumerationMode::DuplicateIdentity ? 2 : 1;
        if (bundles == nullptr && capacity == 0) {
            *count = required;
            *result = {.code = BackendPluginResultCode::Success};
            return;
        }
        if (state.mode == EnumerationMode::Incomplete) {
            *count = required + 1;
            *result = {.code = BackendPluginResultCode::Incomplete};
            return;
        }
        if (bundles == nullptr || capacity < required) {
            *count = required;
            *result = {.code = BackendPluginResultCode::Incomplete};
            return;
        }
        for (uint32_t index = 0; index < required; ++index) {
            if (!FillBundle(state, index, bundles[index])) {
                for (uint32_t cleanup = 0; cleanup < index; ++cleanup) {
                    bundles[cleanup].service.destroy(bundles[cleanup].service.state);
                    bundles[cleanup].queues[0].destroy(bundles[cleanup].queues[0].state);
                }
                *result = {.code = BackendPluginResultCode::OutOfMemory};
                return;
            }
        }
        *count = required;
        *result = {.code = BackendPluginResultCode::Success};
    }

    [[nodiscard]] std::expected<RuntimeBackendPlugin, BackendPluginLoadError>
    MakePlugin(LifecycleCounters& counters, EnumerationMode mode = EnumerationMode::Complete) {
        auto* state = new PluginState{.counters = &counters, .mode = mode};
        auto code = BackendCodeLease::Retain(std::make_shared<CodeOwner>(&counters));
        return RuntimeBackendPlugin::Adopt(
            {.provider = kProvider, .state = state, .destroy = DestroyPlugin, .enumerate = Enumerate}, std::move(code));
    }

} // namespace

TEST_CASE("Backend plugin ABI records remain C-compatible POD values", "[Sora][Compute][Runtime][Plugin][ABI]") {
    STATIC_REQUIRE(std::is_same_v<decltype(BackendQueuePayload{}.reportsExecutionNanoseconds), uint8_t>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<BackendPluginServiceBundleV1>);
    STATIC_REQUIRE(std::is_standard_layout_v<BackendPluginServiceBundleV1>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<BackendPluginApiV1>);
    STATIC_REQUIRE(std::is_standard_layout_v<BackendPluginApiV1>);
}

TEST_CASE("Backend plugin contribution outlives its provider and executable root",
          "[Sora][Compute][Runtime][Plugin][Lifetime]") {
    LifecycleCounters counters;
    std::expected<RuntimeDiscoveryContribution, RuntimeDiscoveryError> contribution =
        std::unexpected(RuntimeDiscoveryError{});
    {
        auto plugin = MakePlugin(counters);
        REQUIRE(plugin.has_value());
        contribution = (*plugin)();
        REQUIRE(contribution.has_value());
        REQUIRE(contribution->resources.queues.size() == 1);
        REQUIRE(contribution->resources.allocators.size() == 1);
        REQUIRE(contribution->resources.compilers.size() == 1);
        REQUIRE(contribution->devices.size() == 1);
    }
    REQUIRE(counters.roots == 1);
    REQUIRE(counters.services == 0);
    REQUIRE(counters.queues == 0);
    REQUIRE(counters.codeOwners == 0);

    {
        std::vector<RuntimeDiscoveryContribution> contributions;
        contributions.push_back(std::move(*contribution));
        auto snapshot = RuntimeDiscoverySnapshot::Create(std::move(contributions));
        REQUIRE(snapshot.has_value());
        auto epoch = std::move(*snapshot).BuildEpoch(1);
        REQUIRE(epoch.has_value());
        REQUIRE(epoch->Devices().size() == 1);
        REQUIRE(epoch->Devices()[0].descriptor == Descriptor());
    }
    REQUIRE(counters.services == 1);
    REQUIRE(counters.queues == 1);
    REQUIRE(counters.codeOwners == 1);
}

TEST_CASE("Backend plugin enumeration failures cannot leak or publish partial services",
          "[Sora][Compute][Runtime][Plugin][Failure]") {
    SECTION("incomplete enumeration transfers no payload") {
        LifecycleCounters counters;
        {
            auto plugin = MakePlugin(counters, EnumerationMode::Incomplete);
            REQUIRE(plugin.has_value());
            auto contribution = (*plugin)();
            REQUIRE_FALSE(contribution.has_value());
            REQUIRE(contribution.error().providerError.code ==
                    std::to_underlying(BackendPluginProviderErrorCode::IncompleteEnumeration));
        }
        REQUIRE(counters == LifecycleCounters{.roots = 1, .codeOwners = 1});
    }

    SECTION("invalid bundle rolls back every transferred owning payload") {
        LifecycleCounters counters;
        {
            auto plugin = MakePlugin(counters, EnumerationMode::InvalidBundle);
            REQUIRE(plugin.has_value());
            auto contribution = (*plugin)();
            REQUIRE_FALSE(contribution.has_value());
            REQUIRE(contribution.error().providerError.code ==
                    std::to_underlying(BackendPluginProviderErrorCode::InvalidBundle));
            REQUIRE(counters.services == 1);
            REQUIRE(counters.queues == 1);
        }
        REQUIRE(counters.roots == 1);
        REQUIRE(counters.codeOwners == 1);
    }

    SECTION("invalid service adoption consumes its state and rolls back its queue exactly once") {
        LifecycleCounters counters;
        {
            auto plugin = MakePlugin(counters, EnumerationMode::InvalidService);
            REQUIRE(plugin.has_value());
            auto contribution = (*plugin)();
            REQUIRE_FALSE(contribution.has_value());
            REQUIRE(contribution.error().providerError.code ==
                    std::to_underlying(BackendPluginProviderErrorCode::InvalidService));
            REQUIRE(counters.services == 1);
            REQUIRE(counters.queues == 1);
        }
        REQUIRE(counters.roots == 1);
        REQUIRE(counters.codeOwners == 1);
    }

    SECTION("invalid queue adoption consumes its state and releases the adopted service exactly once") {
        LifecycleCounters counters;
        {
            auto plugin = MakePlugin(counters, EnumerationMode::InvalidQueue);
            REQUIRE(plugin.has_value());
            auto contribution = (*plugin)();
            REQUIRE_FALSE(contribution.has_value());
            REQUIRE(contribution.error().providerError.code ==
                    std::to_underlying(BackendPluginProviderErrorCode::InvalidQueue));
            REQUIRE(counters.services == 1);
            REQUIRE(counters.queues == 1);
        }
        REQUIRE(counters.roots == 1);
        REQUIRE(counters.codeOwners == 1);
    }

    SECTION("provider diagnostics remain opaque and exact") {
        LifecycleCounters counters;
        auto plugin = MakePlugin(counters, EnumerationMode::ProviderFailure);
        REQUIRE(plugin.has_value());
        auto contribution = (*plugin)();
        REQUIRE_FALSE(contribution.has_value());
        REQUIRE(contribution.error().providerError == RuntimeProviderError{.domain = 77, .code = 88, .detail = 99});
    }
}

TEST_CASE("Backend plugin rejects duplicate service and physical device identities before facade construction",
          "[Sora][Compute][Runtime][Plugin][Identity]") {
    LifecycleCounters counters;
    {
        auto plugin = MakePlugin(counters, EnumerationMode::DuplicateIdentity);
        REQUIRE(plugin.has_value());
        auto contribution = (*plugin)();
        REQUIRE_FALSE(contribution.has_value());
        REQUIRE(contribution.error().providerError.code ==
                std::to_underlying(BackendPluginProviderErrorCode::InvalidBundle));
        REQUIRE(counters.services == 2);
        REQUIRE(counters.queues == 2);
    }
    REQUIRE(counters.roots == 1);
    REQUIRE(counters.codeOwners == 1);
}

TEST_CASE("Backend plugin loader rejects modules without the unique negotiation root",
          "[Sora][Compute][Runtime][Plugin][Loader]") {
    auto plugin = LoadRuntimeBackendPlugin(Sora::PAL::CurrentProcessModule());
    REQUIRE_FALSE(plugin.has_value());
    REQUIRE(plugin.error().code == BackendPluginLoadErrorCode::MissingNegotiationExport);
}

TEST_CASE("Dynamic backend plugin keeps callback code mapped after its root and explicit module handle are released",
          "[Sora][Compute][Runtime][Plugin][Loader][Lifetime]") {
    std::expected<RuntimeDiscoveryContribution, RuntimeDiscoveryError> contribution =
        std::unexpected(RuntimeDiscoveryError{});
    {
        constexpr std::string_view path = SORA_BACKEND_PLUGIN_FIXTURE_PATH;
        const Sora::PAL::ModuleLoadOptions options{.nameKind = Sora::PAL::ModuleNameKind::ExactPath,
                                                   .candidatePolicy = Sora::PAL::ModuleCandidatePolicy::ExactOnly,
                                                   .bindMode = Sora::PAL::ModuleBindMode::Now,
                                                   .cachePolicy = Sora::PAL::ModuleCachePolicy::Private};
        auto module = Sora::PAL::LoadModule({path}, options);
        REQUIRE(module.has_value());
        auto plugin = LoadRuntimeBackendPlugin(*module);
        REQUIRE(plugin.has_value());
        contribution = (*plugin)();
        REQUIRE(contribution.has_value());
    }

    std::vector<RuntimeDiscoveryContribution> contributions;
    contributions.push_back(std::move(*contribution));
    auto snapshot = RuntimeDiscoverySnapshot::Create(std::move(contributions));
    REQUIRE(snapshot.has_value());
    auto epoch = std::move(*snapshot).BuildEpoch(1);
    REQUIRE(epoch.has_value());
    REQUIRE(epoch->Devices().size() == 1);
    REQUIRE(epoch->Devices()[0].descriptor.id == DeviceId{0x44594E414D4943ULL, 1});
}

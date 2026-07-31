#include <Sora/Compute/Backend/CpuBackend.h>
#include <Sora/Compute/Backend/VulkanBackend.h>
#include <Sora/Compute/Runtime/RuntimeDiscovery.h>
#include <Sora/Compute/Runtime/RuntimeRecoveryCoordinator.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <memory>
#include <utility>
#include <vector>

namespace {

    using namespace Sora::Compute;

    static_assert(RuntimeBackendBundle<CpuBackend>);
    static_assert(RuntimeBackendBundle<VulkanBackend>);

    struct ObserverState {
        DeviceId device{};
    };

    struct ServiceAndState {
        BackendService service{};
        ObserverState* state = nullptr;
    };

    void DestroyObserverState(void* opaque) noexcept {
        delete static_cast<ObserverState*>(opaque);
    }

    void Observe(void* opaque, DeviceSnapshot* snapshot) noexcept {
        const auto& state = *static_cast<const ObserverState*>(opaque);
        *snapshot = {.id = state.device, .state = DeviceState::Available, .generation = 1};
    }

    ServiceAndState MakeService(BackendServiceId id, DeviceId device) {
        auto* state = new ObserverState{device};
        auto service = BackendService::Adopt({.id = id, .state = state, .destroy = DestroyObserverState},
                                             BackendCodeLease::Retain(std::make_shared<uint32_t>()));
        REQUIRE(service.has_value());
        return {.service = std::move(*service), .state = state};
    }

    DeviceDescriptor Descriptor(DeviceId device, uint64_t architecture = 0) {
        return {.id = device,
                .backend = BackendKind::Vulkan,
                .kind = DeviceKind::DiscreteGpu,
                .architectureId = architecture == 0 ? device.low : architecture,
                .subgroupWidthMinimum = 1,
                .subgroupWidthMaximum = 32,
                .maximumWorkgroupInvocations = 1024};
    }

    TargetFingerprint Target(DeviceDescriptor device, uint64_t driver = 1) {
        return {.device = device.id,
                .backend = device.backend,
                .hostArchitecture = Sora::CpuArchitecture::X64,
                .operatingSystem = Sora::OperatingSystem::Windows,
                .binaryFormat = Sora::BinaryFormat::PE,
                .byteOrder = Sora::ByteOrder::Little,
                .abi = Sora::AbiFamily::MSVC,
                .identityStability = TargetIdentityStability::MachineStable,
                .addressBits = 64,
                .features = device.features,
                .architectureId = device.architectureId,
                .compilerIdentity = {71, 1},
                .driverIdentity = {71, driver}};
    }

    DeviceObserver MakeObserver(const ServiceAndState& service) {
        auto observer = DeviceObserver::Create(service.service, {.device = service.state->device,
                                                                 .backend = BackendKind::Vulkan,
                                                                 .state = service.state,
                                                                 .observe = Observe});
        REQUIRE(observer.has_value());
        return std::move(*observer);
    }

    RuntimeDiscoveryContribution Contribution(RuntimeProviderId provider, const ServiceAndState& service,
                                              uint64_t architecture = 0, uint64_t driver = 1) {
        const DeviceDescriptor descriptor = Descriptor(service.state->device, architecture);
        RuntimeDiscoveryContribution result{.provider = provider};
        result.resources.deviceObservers.push_back(MakeObserver(service));
        result.devices.push_back({.service = service.service.Id(),
                                  .descriptor = descriptor,
                                  .target = RuntimeTarget(Target(descriptor, driver))});
        return result;
    }

    RuntimeEpoch BuildEpoch(uint64_t sequence, std::vector<RuntimeDiscoveryContribution> contributions) {
        auto snapshot = RuntimeDiscoverySnapshot::Create(std::move(contributions));
        REQUIRE(snapshot.has_value());
        auto epoch = std::move(*snapshot).BuildEpoch(sequence);
        REQUIRE(epoch.has_value());
        return std::move(*epoch);
    }

    RuntimeEpochError FailureFor(const RuntimeEpoch& epoch, BackendServiceId service) {
        const auto found = std::ranges::find(epoch.Devices(), service, &RuntimeDeviceBinding::service);
        REQUIRE(found != epoch.Devices().end());
        return {.code = RuntimeEpochErrorCode::DeviceLost,
                .device = static_cast<uint32_t>(found - epoch.Devices().begin()),
                .cause = DeviceObservationError::Lost};
    }

} // namespace

TEST_CASE("Runtime discovery canonicalizes complete provider contributions", "[Sora][Compute][Runtime][Discovery]") {
    auto first = MakeService({1, 101}, {8, 80});
    auto second = MakeService({2, 202}, {3, 30});
    std::vector<RuntimeDiscoveryContribution> contributions;
    contributions.push_back(Contribution({9, 9}, first));
    contributions.push_back(Contribution({4, 4}, second));

    auto snapshot = RuntimeDiscoverySnapshot::Create(std::move(contributions));
    REQUIRE(snapshot.has_value());
    const std::vector<RuntimeProviderId> expectedProviders{{4, 4}, {9, 9}};
    REQUIRE(std::ranges::equal(snapshot->Providers(), expectedProviders));
    REQUIRE(snapshot->Devices().size() == 2);

    auto epoch = std::move(*snapshot).BuildEpoch(11);
    REQUIRE(epoch.has_value());
    REQUIRE(epoch->Sequence() == 11);
    REQUIRE(epoch->Devices()[0].descriptor.id == second.state->device);
    REQUIRE(epoch->Devices()[1].descriptor.id == first.state->device);
}

TEST_CASE("Built-in CPU backend projects into one complete runtime discovery contribution",
          "[Sora][Compute][Runtime][Discovery][Cpu]") {
    auto backend = CpuBackend::Create({.workerCount = 1});
    REQUIRE(backend.has_value());
    auto contribution = MakeRuntimeDiscoveryContribution(RuntimeProviderId{0x435055, 1}, *backend);
    REQUIRE(contribution.has_value());
    std::vector<RuntimeDiscoveryContribution> contributions;
    contributions.push_back(std::move(*contribution));

    auto snapshot = RuntimeDiscoverySnapshot::Create(std::move(contributions));
    REQUIRE(snapshot.has_value());
    auto epoch = std::move(*snapshot).BuildEpoch(1);
    REQUIRE(epoch.has_value());
    REQUIRE(epoch->Devices().size() == 1);
    REQUIRE(epoch->Devices()[0].descriptor == backend->Device());
    REQUIRE(epoch->Devices()[0].target == RuntimeTarget(backend->Target()));
    const PhysicalResourceInventory& inventory = epoch->Resources().Inventory();
    REQUIRE(inventory.Queues().size() == 1);
    REQUIRE(inventory.Memories().size() == 2);
    REQUIRE(inventory.Transfers().size() == 4);
    REQUIRE(inventory.Compilers().size() == 1);
    REQUIRE(inventory.PipelineFactories().empty());
    REQUIRE(inventory.PipelineConfigurations().empty());
}

TEST_CASE("Homogeneous backend ranges project every service without runtime type erasure",
          "[Sora][Compute][Runtime][Discovery][Range]") {
    static_assert(RuntimeBackendRange<std::vector<CpuBackend>>);
    auto first = CpuBackend::Create({.workerCount = 1});
    auto second = CpuBackend::Create({.workerCount = 1});
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    std::vector<CpuBackend> backends;
    backends.push_back(std::move(*first));
    backends.push_back(std::move(*second));

    auto contribution = MakeRuntimeDiscoveryContribution(RuntimeProviderId{0x435055, 2}, backends);
    REQUIRE(contribution.has_value());
    REQUIRE(contribution->resources.deviceObservers.size() == 2);
    REQUIRE(contribution->resources.queues.size() == 2);
    REQUIRE(contribution->resources.allocators.size() == 4);
    REQUIRE(contribution->resources.hostMemoryAccessors.size() == 2);
    REQUIRE(contribution->resources.transferEngines.size() == 8);
    REQUIRE(contribution->resources.compilers.size() == 2);
    REQUIRE(contribution->resources.pipelineFactories.empty());
    REQUIRE(contribution->resources.pipelineConfigurations.empty());
    REQUIRE(contribution->devices.size() == 2);

    std::vector<RuntimeDiscoveryContribution> contributions;
    contributions.push_back(std::move(*contribution));
    auto snapshot = RuntimeDiscoverySnapshot::Create(std::move(contributions));
    REQUIRE(snapshot.has_value());
    auto epoch = std::move(*snapshot).BuildEpoch(1);
    REQUIRE_FALSE(epoch.has_value());
    REQUIRE(epoch.error().code == RuntimeDiscoveryErrorCode::EpochRejected);
    REQUIRE(epoch.error().epoch.code == RuntimeEpochErrorCode::DuplicateBackendDevice);
}

TEST_CASE("Runtime discovery rejects provider and merged-resource conflicts "
          "transactionally",
          "[Sora][Compute][Runtime][Discovery][Failure]") {
    auto first = MakeService({3, 303}, {1, 10});
    auto second = MakeService({4, 404}, {1, 10});

    SECTION("duplicate provider") {
        std::vector<RuntimeDiscoveryContribution> contributions;
        contributions.push_back(Contribution({1, 1}, first));
        contributions.push_back(Contribution({1, 1}, second));
        auto snapshot = RuntimeDiscoverySnapshot::Create(std::move(contributions));
        REQUIRE_FALSE(snapshot.has_value());
        REQUIRE(snapshot.error().code == RuntimeDiscoveryErrorCode::DuplicateProvider);
    }

    SECTION("duplicate physical coordinate") {
        std::vector<RuntimeDiscoveryContribution> contributions;
        contributions.push_back(Contribution({1, 1}, first));
        contributions.push_back(Contribution({2, 2}, second));
        auto snapshot = RuntimeDiscoverySnapshot::Create(std::move(contributions));
        REQUIRE(snapshot.has_value());
        auto epoch = std::move(*snapshot).BuildEpoch(1);
        REQUIRE_FALSE(epoch.has_value());
        REQUIRE(epoch.error().code == RuntimeDiscoveryErrorCode::EpochRejected);
        REQUIRE(epoch.error().epoch.code == RuntimeEpochErrorCode::DuplicateBackendDevice);
    }
}

TEST_CASE("Compile-time provider composition stops at the first discovery failure",
          "[Sora][Compute][Runtime][Discovery][Provider]") {
    auto service = MakeService({5, 505}, {5, 50});
    auto contribution = Contribution({1, 1}, service);
    uint32_t firstCalls = 0;
    uint32_t failedCalls = 0;
    uint32_t skippedCalls = 0;
    auto first = [&]() noexcept -> std::expected<RuntimeDiscoveryContribution, RuntimeDiscoveryError> {
        ++firstCalls;
        return std::move(contribution);
    };
    auto failed = [&]() noexcept -> std::expected<RuntimeDiscoveryContribution, RuntimeDiscoveryError> {
        ++failedCalls;
        return std::unexpected(RuntimeDiscoveryError{.code = RuntimeDiscoveryErrorCode::InvalidProvider});
    };
    auto skipped = [&]() noexcept -> std::expected<RuntimeDiscoveryContribution, RuntimeDiscoveryError> {
        ++skippedCalls;
        return std::unexpected(RuntimeDiscoveryError{.code = RuntimeDiscoveryErrorCode::InvalidProvider});
    };

    auto epoch = DiscoverRuntime(1, first, failed, skipped);
    REQUIRE_FALSE(epoch.has_value());
    REQUIRE(epoch.error().code == RuntimeDiscoveryErrorCode::InvalidProvider);
    REQUIRE(epoch.error().providerIndex == 1);
    REQUIRE(firstCalls == 1);
    REQUIRE(failedCalls == 1);
    REQUIRE(skippedCalls == 0);
}

TEST_CASE("Runtime epoch diff separates retained, removed, added, and replaced "
          "identities",
          "[Sora][Compute][Runtime][Discovery][Diff]") {
    auto removed = MakeService({6, 606}, {1, 10});
    auto retained = MakeService({7, 707}, {2, 20});
    auto replacement = MakeService({8, 808}, {1, 10});
    auto added = MakeService({9, 909}, {3, 30});
    std::vector<RuntimeDiscoveryContribution> previousContributions;
    previousContributions.push_back(Contribution({1, 1}, removed));
    previousContributions.push_back(Contribution({2, 2}, retained));
    RuntimeEpoch previous = BuildEpoch(1, std::move(previousContributions));
    std::vector<RuntimeDiscoveryContribution> replacementContributions;
    replacementContributions.push_back(Contribution({1, 1}, replacement, 77, 9));
    replacementContributions.push_back(Contribution({2, 2}, retained));
    replacementContributions.push_back(Contribution({3, 3}, added));
    RuntimeEpoch current = BuildEpoch(2, std::move(replacementContributions));

    auto diff = DiffRuntimeEpochs(previous, current);
    REQUIRE(diff.has_value());
    REQUIRE(diff->retainedServices == std::vector<BackendServiceId>{retained.service.Id()});
    REQUIRE(diff->removedServices == std::vector<BackendServiceId>{removed.service.Id()});
    REQUIRE(diff->addedServices == std::vector<BackendServiceId>{replacement.service.Id(), added.service.Id()});
    REQUIRE(diff->replacements == std::vector<RuntimeDeviceReplacement>{{.backend = BackendKind::Vulkan,
                                                                         .device = removed.state->device,
                                                                         .previousService = removed.service.Id(),
                                                                         .replacementService = replacement.service.Id(),
                                                                         .targetChanged = true}});
}

TEST_CASE("Runtime recovery retires every service removed by complete "
          "rediscovery before publication",
          "[Sora][Compute][Runtime][Discovery][Recovery]") {
    auto failed = MakeService({10, 1010}, {1, 10});
    auto removed = MakeService({11, 1111}, {2, 20});
    auto replacement = MakeService({12, 1212}, {1, 10});
    std::vector<RuntimeDiscoveryContribution> initialContributions;
    initialContributions.push_back(Contribution({1, 1}, failed));
    initialContributions.push_back(Contribution({2, 2}, removed));
    auto coordinator = RuntimeRecoveryCoordinator::Create(BuildEpoch(1, std::move(initialContributions)));
    REQUIRE(coordinator.has_value());
    const auto observed = (*coordinator)->Current();
    RuntimeEpoch next = BuildEpoch(2, {Contribution({1, 1}, replacement)});
    auto provider =
        [&next](const RuntimeRecoveryRequest&) noexcept -> std::expected<RuntimeEpoch, RuntimeRecoveryError> {
        return std::move(next);
    };

    auto recovered = (*coordinator)->Recover(observed, FailureFor(*observed, failed.service.Id()), provider);
    REQUIRE(recovered.has_value());
    REQUIRE((*coordinator)->InvalidationRegistry().IsServiceRetired(failed.service.Id()));
    REQUIRE((*coordinator)->InvalidationRegistry().IsServiceRetired(removed.service.Id()));
    REQUIRE_FALSE((*coordinator)->InvalidationRegistry().IsServiceRetired(replacement.service.Id()));
    REQUIRE((*coordinator)->Current() == *recovered);
    REQUIRE((*recovered)->Devices().size() == 1);
    REQUIRE((*recovered)->Devices()[0].service == replacement.service.Id());
}

TEST_CASE("Epoch diff conflicts cannot partially publish a reconstructed runtime",
          "[Sora][Compute][Runtime][Discovery][Recovery][Failure]") {
    auto failed = MakeService({13, 1313}, {1, 10});
    auto conflicting = MakeService({14, 1414}, {2, 20});
    RuntimeEpoch initial = BuildEpoch(1, {Contribution({1, 1}, failed), Contribution({2, 2}, conflicting)});
    auto coordinator = RuntimeRecoveryCoordinator::Create(std::move(initial));
    REQUIRE(coordinator.has_value());
    const auto observed = (*coordinator)->Current();
    RuntimeEpoch invalidReplacement = BuildEpoch(2, {Contribution({2, 2}, conflicting, 99, 7)});
    auto provider = [&invalidReplacement](
                        const RuntimeRecoveryRequest&) noexcept -> std::expected<RuntimeEpoch, RuntimeRecoveryError> {
        return std::move(invalidReplacement);
    };

    auto recovered = (*coordinator)->Recover(observed, FailureFor(*observed, failed.service.Id()), provider);
    REQUIRE_FALSE(recovered.has_value());
    REQUIRE(recovered.error().code == RuntimeRecoveryErrorCode::EpochDiffFailed);
    REQUIRE(recovered.error().epochDiff.code == RuntimeEpochDiffErrorCode::ServiceIdentityConflict);
    REQUIRE((*coordinator)->Current() == observed);
    REQUIRE_FALSE((*coordinator)->InvalidationRegistry().IsServiceRetired(conflicting.service.Id()));
}

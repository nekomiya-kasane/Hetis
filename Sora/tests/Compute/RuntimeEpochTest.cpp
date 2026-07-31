#include "BackendTestSupport.h"

#include <Sora/Compute/Runtime/RuntimeEpoch.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace {

    using namespace Sora::Compute;

    constexpr DeviceId kFirstDevice{1, 11};
    constexpr DeviceId kSecondDevice{2, 22};

    struct ObserverState {
        DeviceId device{};
        DeviceState state = DeviceState::Available;
        uint64_t generation = 1;
        uint64_t sequence = 0;
        bool invalid = false;
    };

    void DestroyState(void* opaque) noexcept {
        delete static_cast<ObserverState*>(opaque);
    }

    void Observe(void* opaque, DeviceSnapshot* snapshot) noexcept {
        const auto& state = *static_cast<const ObserverState*>(opaque);
        *snapshot = {.id = state.device,
                     .state = state.state,
                     .generation = state.invalid ? 0 : state.generation,
                     .observationSequence = state.sequence};
    }

    struct ServiceAndState {
        BackendService service;
        ObserverState* state = nullptr;
    };

    ServiceAndState MakeService(BackendServiceId id, DeviceId device) {
        auto* state = new ObserverState{.device = device};
        auto service = BackendService::Adopt({.id = id, .state = state, .destroy = DestroyState},
                                             BackendCodeLease::Retain(std::make_shared<uint32_t>()));
        REQUIRE(service.has_value());
        return {.service = std::move(*service), .state = state};
    }

    DeviceObserver MakeObserver(const ServiceAndState& value, BackendKind backend = BackendKind::Vulkan) {
        auto observer = DeviceObserver::Create(
            value.service,
            {.device = value.state->device, .backend = backend, .state = value.state, .observe = Observe});
        REQUIRE(observer.has_value());
        return std::move(*observer);
    }

    DeviceDescriptor Descriptor(DeviceId device, BackendKind backend = BackendKind::Vulkan) {
        return {.id = device,
                .backend = backend,
                .kind = backend == BackendKind::Cpu ? DeviceKind::Cpu : DeviceKind::DiscreteGpu,
                .architectureId = device.low,
                .subgroupWidthMinimum = 1,
                .subgroupWidthMaximum = 32,
                .maximumWorkgroupInvocations = 1024};
    }

    TargetFingerprint Target(DeviceDescriptor device) {
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
                .compilerIdentity = {61, 1},
                .driverIdentity = {61, 2}};
    }

    RuntimeDeviceBinding Binding(BackendServiceId service, DeviceDescriptor descriptor) {
        return {.service = service, .descriptor = descriptor, .target = RuntimeTarget(Target(descriptor))};
    }

    PhysicalResourceCatalog MakeCatalog(std::vector<DeviceObserver> observers) {
        auto catalog = PhysicalResourceCatalog::Create({.deviceObservers = std::move(observers)});
        REQUIRE(catalog.has_value());
        return std::move(*catalog);
    }

} // namespace

TEST_CASE("RuntimeEpoch publishes one canonical catalog-bound device set", "[Sora][Compute][Runtime][Epoch]") {
    auto first = MakeService({1, 101}, kFirstDevice);
    auto second = MakeService({2, 202}, kSecondDevice);
    first.state->sequence = 7;
    second.state->sequence = 9;
    auto epoch = RuntimeEpoch::Create(3, MakeCatalog({MakeObserver(second), MakeObserver(first)}),
                                      {Binding(second.service.Id(), Descriptor(kSecondDevice)),
                                       Binding(first.service.Id(), Descriptor(kFirstDevice))});

    REQUIRE(epoch.has_value());
    REQUIRE(epoch->Sequence() == 3);
    REQUIRE(epoch->Devices().size() == 2);
    REQUIRE(epoch->Devices()[0].descriptor.id == kFirstDevice);
    REQUIRE(epoch->Devices()[0].target == RuntimeTarget(Target(Descriptor(kFirstDevice))));
    REQUIRE(epoch->Snapshots()[0].observationSequence == 7);
    REQUIRE(epoch->Devices()[1].descriptor.id == kSecondDevice);
    REQUIRE(epoch->Snapshots()[1].observationSequence == 9);
    REQUIRE(epoch->Resources().FindDeviceObserver(first.service.Id()) != nullptr);

    first.state->sequence = 11;
    auto refreshed = epoch->RefreshDeviceSnapshots();
    REQUIRE(refreshed.has_value());
    REQUIRE((*refreshed)[0].generation == epoch->Snapshots()[0].generation);
    REQUIRE(epoch->Devices()[0].target == RuntimeTarget(Target(Descriptor(kFirstDevice))));
    REQUIRE((*refreshed)[0].observationSequence == 11);
    REQUIRE(epoch->Snapshots()[0].observationSequence == 7);
}

TEST_CASE("RuntimeEpoch rejects invalid topology before observing devices",
          "[Sora][Compute][Runtime][Epoch][Validation]") {
    auto first = MakeService({1, 101}, kFirstDevice);
    auto second = MakeService({2, 202}, kSecondDevice);

    SECTION("zero sequence") {
        auto epoch = RuntimeEpoch::Create(0, MakeCatalog({MakeObserver(first)}),
                                          {Binding(first.service.Id(), Descriptor(kFirstDevice))});
        REQUIRE(epoch.error().code == RuntimeEpochErrorCode::InvalidSequence);
    }
    SECTION("empty device set") {
        auto epoch = RuntimeEpoch::Create(1, MakeCatalog({MakeObserver(first)}), {});
        REQUIRE(epoch.error().code == RuntimeEpochErrorCode::EmptyDeviceSet);
    }
    SECTION("invalid binding") {
        auto epoch =
            RuntimeEpoch::Create(1, MakeCatalog({MakeObserver(first)}), {Binding({}, Descriptor(kFirstDevice))});
        REQUIRE(epoch.error().code == RuntimeEpochErrorCode::InvalidDeviceBinding);
    }
    SECTION("invalid descriptor") {
        auto descriptor = Descriptor(kFirstDevice);
        descriptor.subgroupWidthMinimum = 64;
        descriptor.subgroupWidthMaximum = 32;
        auto epoch =
            RuntimeEpoch::Create(1, MakeCatalog({MakeObserver(first)}), {Binding(first.service.Id(), descriptor)});
        REQUIRE(epoch.error().code == RuntimeEpochErrorCode::InvalidDeviceBinding);
    }
    SECTION("incompatible target") {
        const DeviceDescriptor descriptor = Descriptor(kFirstDevice);
        RuntimeDeviceBinding binding = Binding(first.service.Id(), descriptor);
        binding.target.architectureId = descriptor.architectureId + 1;
        auto epoch = RuntimeEpoch::Create(1, MakeCatalog({MakeObserver(first)}), {binding});
        REQUIRE(epoch.error().code == RuntimeEpochErrorCode::InvalidDeviceBinding);
    }
    SECTION("duplicate service") {
        auto epoch = RuntimeEpoch::Create(1, MakeCatalog({MakeObserver(first)}),
                                          {Binding(first.service.Id(), Descriptor(kFirstDevice)),
                                           Binding(first.service.Id(), Descriptor(kSecondDevice))});
        REQUIRE(epoch.error().code == RuntimeEpochErrorCode::DuplicateService);
    }
    SECTION("duplicate backend and device") {
        second.state->device = kFirstDevice;
        auto epoch = RuntimeEpoch::Create(1, MakeCatalog({MakeObserver(first), MakeObserver(second)}),
                                          {Binding(first.service.Id(), Descriptor(kFirstDevice)),
                                           Binding(second.service.Id(), Descriptor(kFirstDevice))});
        REQUIRE(epoch.error().code == RuntimeEpochErrorCode::DuplicateBackendDevice);
    }
}

TEST_CASE("RuntimeEpoch rejects missing, incompatible, invalid, and lost observations",
          "[Sora][Compute][Runtime][Epoch][Observation]") {
    auto first = MakeService({1, 101}, kFirstDevice);

    SECTION("missing observer") {
        auto epoch = RuntimeEpoch::Create(1, MakeCatalog({}), {Binding(first.service.Id(), Descriptor(kFirstDevice))});
        REQUIRE(epoch.error().code == RuntimeEpochErrorCode::MissingDeviceObserver);
    }
    SECTION("incompatible observer") {
        auto epoch = RuntimeEpoch::Create(1, MakeCatalog({MakeObserver(first, BackendKind::Cpu)}),
                                          {Binding(first.service.Id(), Descriptor(kFirstDevice))});
        REQUIRE(epoch.error().code == RuntimeEpochErrorCode::IncompatibleDeviceObserver);
    }
    SECTION("unbound observer") {
        auto second = MakeService({2, 202}, kSecondDevice);
        auto epoch = RuntimeEpoch::Create(1, MakeCatalog({MakeObserver(first), MakeObserver(second)}),
                                          {Binding(first.service.Id(), Descriptor(kFirstDevice))});
        REQUIRE(epoch.error().code == RuntimeEpochErrorCode::DeviceSetMismatch);
    }
    SECTION("invalid snapshot") {
        first.state->invalid = true;
        auto epoch = RuntimeEpoch::Create(1, MakeCatalog({MakeObserver(first)}),
                                          {Binding(first.service.Id(), Descriptor(kFirstDevice))});
        REQUIRE(epoch.error().code == RuntimeEpochErrorCode::DeviceObservationFailed);
        REQUIRE(epoch.error().cause == DeviceObservationError::InvalidSnapshot);
    }
    SECTION("lost device") {
        first.state->state = DeviceState::Lost;
        first.state->generation = 2;
        auto epoch = RuntimeEpoch::Create(1, MakeCatalog({MakeObserver(first)}),
                                          {Binding(first.service.Id(), Descriptor(kFirstDevice))});
        REQUIRE(epoch.error().code == RuntimeEpochErrorCode::DeviceLost);
        REQUIRE(epoch.error().cause == DeviceObservationError::Lost);
    }
}

TEST_CASE("RuntimeEpoch refresh reports loss without mutating the published record",
          "[Sora][Compute][Runtime][Epoch][Recovery]") {
    auto service = MakeService({1, 101}, kFirstDevice);
    auto epoch = RuntimeEpoch::Create(1, MakeCatalog({MakeObserver(service)}),
                                      {Binding(service.service.Id(), Descriptor(kFirstDevice))});
    REQUIRE(epoch.has_value());

    service.state->state = DeviceState::Lost;
    service.state->generation = 2;
    auto refreshed = epoch->RefreshDeviceSnapshots();
    REQUIRE(refreshed.error().code == RuntimeEpochErrorCode::DeviceLost);
    REQUIRE(epoch->Snapshots()[0].state == DeviceState::Available);
    REQUIRE(epoch->Snapshots()[0].generation == 1);
}

TEST_CASE("RuntimeEpoch refresh rejects a replacement generation even when it is available",
          "[Sora][Compute][Runtime][Epoch][Recovery]") {
    auto service = MakeService({1, 101}, kFirstDevice);
    auto epoch = RuntimeEpoch::Create(1, MakeCatalog({MakeObserver(service)}),
                                      {Binding(service.service.Id(), Descriptor(kFirstDevice))});
    REQUIRE(epoch.has_value());

    service.state->generation = 2;
    auto refreshed = epoch->RefreshDeviceSnapshots();
    REQUIRE(refreshed.error().code == RuntimeEpochErrorCode::DeviceGenerationChanged);
    REQUIRE(refreshed.error().cause == DeviceObservationError::GenerationChanged);
    REQUIRE(epoch->Snapshots()[0].generation == 1);
}

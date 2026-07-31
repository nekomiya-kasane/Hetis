#include <Sora/Compute/Runtime/RuntimeRecoveryCoordinator.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <latch>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace {

    using namespace Sora::Compute;

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

    DeviceDescriptor Descriptor(DeviceId device) {
        return {.id = device,
                .backend = BackendKind::Vulkan,
                .kind = DeviceKind::DiscreteGpu,
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
                .compilerIdentity = {41, 1},
                .driverIdentity = {41, 2}};
    }

    DeviceObserver MakeObserver(const ServiceAndState& value) {
        auto observer = DeviceObserver::Create(
            value.service,
            {.device = value.state->device, .backend = BackendKind::Vulkan, .state = value.state, .observe = Observe});
        REQUIRE(observer.has_value());
        return std::move(*observer);
    }

    RuntimeEpoch MakeEpoch(uint64_t sequence, std::span<const ServiceAndState* const> services) {
        PhysicalResourceCatalogStorage storage;
        std::vector<RuntimeDeviceBinding> bindings;
        storage.deviceObservers.reserve(services.size());
        bindings.reserve(services.size());
        for (const ServiceAndState* service : services) {
            storage.deviceObservers.push_back(MakeObserver(*service));
            const DeviceDescriptor descriptor = Descriptor(service->state->device);
            bindings.push_back({.service = service->service.Id(),
                                .descriptor = descriptor,
                                .target = RuntimeTarget(Target(descriptor))});
        }
        auto catalog = PhysicalResourceCatalog::Create(std::move(storage));
        REQUIRE(catalog.has_value());
        auto epoch = RuntimeEpoch::Create(sequence, std::move(*catalog), std::move(bindings));
        REQUIRE(epoch.has_value());
        return std::move(*epoch);
    }

    template<typename... Services>
    RuntimeEpoch MakeEpoch(uint64_t sequence, const Services&... services) {
        const std::array<const ServiceAndState*, sizeof...(Services)> values{&services...};
        return MakeEpoch(sequence, std::span<const ServiceAndState* const>{values});
    }

    RuntimeEpochError FailureFor(const RuntimeEpoch& epoch, BackendServiceId service) {
        const auto found = std::ranges::find(epoch.Devices(), service, &RuntimeDeviceBinding::service);
        REQUIRE(found != epoch.Devices().end());
        return {.code = RuntimeEpochErrorCode::DeviceLost,
                .device = static_cast<uint32_t>(found - epoch.Devices().begin()),
                .cause = DeviceObservationError::Lost};
    }

    std::expected<RuntimeEpoch, RuntimeRecoveryError>
    Rebuild(const RuntimeRecoveryRequest& request, std::span<const ServiceAndState* const> services) noexcept {
        try {
            return MakeEpoch(request.sequence, services);
        } catch (const std::bad_alloc&) {
            return std::unexpected(RuntimeRecoveryError{.code = RuntimeRecoveryErrorCode::OutOfMemory,
                                                        .service = request.failedService,
                                                        .expectedSequence = request.sequence});
        }
    }

    struct AllocationState {
        std::atomic<uint32_t>* destroyCount = nullptr;
    };

    void DestroyAllocation(void* opaque) noexcept {
        std::unique_ptr<AllocationState> state{static_cast<AllocationState*>(opaque)};
        state->destroyCount->fetch_add(1, std::memory_order_relaxed);
    }

    Buffer MakeBuffer(BufferId id) {
        auto buffer = Buffer::Create({.id = id,
                                      .elementType = {ScalarKind::Float32, 1},
                                      .layout = LayoutKind::Contiguous,
                                      .rank = 1,
                                      .sizeBytes = 256,
                                      .alignment = 64});
        REQUIRE(buffer.has_value());
        return *buffer;
    }

    Allocation MakeAllocation(const BackendService& service, DeviceId device, std::atomic<uint32_t>& destroyCount) {
        auto state = std::make_unique<AllocationState>(&destroyCount);
        auto allocation = Allocation::Adopt(
            service, {
                         .location = {.domain = {1, 1}, .device = device, .backend = BackendKind::Vulkan},
                         .memory = {1, 1},
                         .properties = MemoryProperty::DeviceLocal,
                         .sizeBytes = 256,
                         .alignment = 64,
                         .state = state.release(),
                         .destroy = DestroyAllocation,
                     });
        REQUIRE(allocation.has_value());
        return *allocation;
    }

} // namespace

TEST_CASE("RuntimeRecoveryCoordinator publishes one initial immutable epoch", "[Sora][Compute][Runtime][Recovery]") {
    auto first = MakeService({1, 101}, {1, 11});
    auto created = RuntimeRecoveryCoordinator::Create(MakeEpoch(7, first));
    REQUIRE(created.has_value());
    const auto current = (*created)->Current();
    REQUIRE(current);
    REQUIRE(current->Sequence() == 7);
    REQUIRE(current->Devices().size() == 1);
}

TEST_CASE("Concurrent waiters share one successful epoch reconstruction",
          "[Sora][Compute][Runtime][Recovery][SingleFlight]") {
    auto failed = MakeService({2, 202}, {2, 22});
    auto survivor = MakeService({3, 303}, {3, 33});
    auto created = RuntimeRecoveryCoordinator::Create(MakeEpoch(1, failed, survivor));
    REQUIRE(created.has_value());
    auto& coordinator = **created;
    const auto observed = coordinator.Current();
    const RuntimeEpochError failure = FailureFor(*observed, failed.service.Id());
    std::atomic<uint32_t> providerCalls = 0;
    std::latch entered{1};
    std::latch release{1};
    const std::array<const ServiceAndState*, 1> survivors{&survivor};
    auto provider =
        [&](const RuntimeRecoveryRequest& request) noexcept -> std::expected<RuntimeEpoch, RuntimeRecoveryError> {
        providerCalls.fetch_add(1, std::memory_order_relaxed);
        entered.count_down();
        release.wait();
        return Rebuild(request, survivors);
    };

    std::array<std::shared_ptr<const RuntimeEpoch>, 8> results{};
    std::array<std::jthread, 8> threads;
    for (size_t index = 0; index < threads.size(); ++index) {
        threads[index] = std::jthread([&, index] {
            auto recovered = coordinator.Recover(observed, failure, provider);
            if (recovered) {
                results[index] = std::move(*recovered);
            }
        });
    }
    entered.wait();
    release.count_down();
    for (std::jthread& thread : threads) {
        thread.join();
    }

    REQUIRE(providerCalls.load(std::memory_order_relaxed) == 1);
    REQUIRE(results[0]);
    for (const auto& result : results) {
        REQUIRE(result == results[0]);
        REQUIRE(result->Sequence() == 2);
        REQUIRE(result->Devices().size() == 1);
        REQUIRE(result->Devices()[0].service == survivor.service.Id());
    }
    REQUIRE(coordinator.Current() == results[0]);
}

TEST_CASE("Recovery invalidates failed-service resources before invoking the provider",
          "[Sora][Compute][Runtime][Recovery][Invalidation]") {
    auto failed = MakeService({4, 404}, {4, 44});
    auto survivor = MakeService({5, 505}, {5, 55});
    auto created = RuntimeRecoveryCoordinator::Create(MakeEpoch(1, failed, survivor));
    REQUIRE(created.has_value());
    auto& coordinator = **created;
    Buffer buffer = MakeBuffer({10, 1});
    std::atomic<uint32_t> destroyCount = 0;
    Allocation allocation = MakeAllocation(failed.service, failed.state->device, destroyCount);
    REQUIRE(buffer.CommitWholeWrite(allocation).has_value());
    allocation = {};
    REQUIRE(coordinator.InvalidationRegistry().Register(buffer).has_value());
    std::atomic<bool> observedInvalidation = false;
    const std::array<const ServiceAndState*, 1> survivors{&survivor};
    auto provider =
        [&](const RuntimeRecoveryRequest& request) noexcept -> std::expected<RuntimeEpoch, RuntimeRecoveryError> {
        observedInvalidation.store(buffer.ReplicaCount() == 0 && destroyCount.load() == 1, std::memory_order_relaxed);
        return Rebuild(request, survivors);
    };

    const auto observed = coordinator.Current();
    auto recovered = coordinator.Recover(observed, FailureFor(*observed, failed.service.Id()), provider);
    REQUIRE(recovered.has_value());
    REQUIRE(observedInvalidation.load(std::memory_order_relaxed));
    REQUIRE(buffer.ReplicaCount() == 0);
}

TEST_CASE("Different failed services serialize into successive epochs",
          "[Sora][Compute][Runtime][Recovery][MultiDevice]") {
    auto first = MakeService({6, 606}, {6, 66});
    auto second = MakeService({7, 707}, {7, 77});
    auto survivor = MakeService({8, 808}, {8, 88});
    auto created = RuntimeRecoveryCoordinator::Create(MakeEpoch(1, first, second, survivor));
    REQUIRE(created.has_value());
    auto& coordinator = **created;
    const auto observed = coordinator.Current();
    const RuntimeEpochError firstFailure = FailureFor(*observed, first.service.Id());
    const RuntimeEpochError secondFailure = FailureFor(*observed, second.service.Id());
    std::atomic<uint32_t> providerCalls = 0;
    bool firstRetired = false;
    bool secondRetired = false;
    auto provider =
        [&](const RuntimeRecoveryRequest& request) noexcept -> std::expected<RuntimeEpoch, RuntimeRecoveryError> {
        providerCalls.fetch_add(1, std::memory_order_relaxed);
        firstRetired = firstRetired || request.failedService == first.service.Id();
        secondRetired = secondRetired || request.failedService == second.service.Id();
        std::array<const ServiceAndState*, 3> retained{};
        size_t count = 0;
        if (!firstRetired) {
            retained[count++] = &first;
        }
        if (!secondRetired) {
            retained[count++] = &second;
        }
        retained[count++] = &survivor;
        return Rebuild(request, std::span{retained}.first(count));
    };

    std::shared_ptr<const RuntimeEpoch> firstResult;
    std::shared_ptr<const RuntimeEpoch> secondResult;
    std::jthread firstThread([&] {
        auto recovered = coordinator.Recover(observed, firstFailure, provider);
        if (recovered) {
            firstResult = std::move(*recovered);
        }
    });
    std::jthread secondThread([&] {
        auto recovered = coordinator.Recover(observed, secondFailure, provider);
        if (recovered) {
            secondResult = std::move(*recovered);
        }
    });
    firstThread.join();
    secondThread.join();

    REQUIRE(providerCalls.load(std::memory_order_relaxed) == 2);
    REQUIRE(firstResult);
    REQUIRE(secondResult);
    const auto current = coordinator.Current();
    REQUIRE(current->Sequence() == 3);
    REQUIRE(current->Devices().size() == 1);
    REQUIRE(current->Devices()[0].service == survivor.service.Id());
}

TEST_CASE("Waiters share one provider failure and invalid replacements never publish",
          "[Sora][Compute][Runtime][Recovery][Failure]") {
    auto failed = MakeService({9, 909}, {9, 99});
    auto survivor = MakeService({10, 1010}, {10, 110});

    SECTION("provider failure is shared") {
        auto created = RuntimeRecoveryCoordinator::Create(MakeEpoch(1, failed, survivor));
        REQUIRE(created.has_value());
        auto& coordinator = **created;
        const auto observed = coordinator.Current();
        const RuntimeEpochError failure = FailureFor(*observed, failed.service.Id());
        std::atomic<uint32_t> providerCalls = 0;
        std::latch entered{1};
        std::latch release{1};
        auto provider =
            [&](const RuntimeRecoveryRequest& request) noexcept -> std::expected<RuntimeEpoch, RuntimeRecoveryError> {
            providerCalls.fetch_add(1, std::memory_order_relaxed);
            entered.count_down();
            release.wait();
            return std::unexpected(RuntimeRecoveryError{.code = RuntimeRecoveryErrorCode::EpochRebuildFailed,
                                                        .service = request.failedService,
                                                        .expectedSequence = request.sequence});
        };
        std::array<RuntimeRecoveryErrorCode, 4> errors{};
        std::array<std::jthread, 4> threads;
        for (size_t index = 0; index < threads.size(); ++index) {
            threads[index] = std::jthread([&, index] {
                auto recovered = coordinator.Recover(observed, failure, provider);
                if (!recovered) {
                    errors[index] = recovered.error().code;
                }
            });
        }
        entered.wait();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        release.count_down();
        for (std::jthread& thread : threads) {
            thread.join();
        }
        REQUIRE(providerCalls.load(std::memory_order_relaxed) == 1);
        for (RuntimeRecoveryErrorCode error : errors) {
            REQUIRE(error == RuntimeRecoveryErrorCode::EpochRebuildFailed);
        }
        REQUIRE(coordinator.Current() == observed);
    }

    SECTION("wrong sequence is rejected") {
        auto created = RuntimeRecoveryCoordinator::Create(MakeEpoch(1, failed, survivor));
        REQUIRE(created.has_value());
        auto& coordinator = **created;
        const auto observed = coordinator.Current();
        const std::array<const ServiceAndState*, 1> survivors{&survivor};
        auto provider =
            [&](const RuntimeRecoveryRequest& request) noexcept -> std::expected<RuntimeEpoch, RuntimeRecoveryError> {
            RuntimeRecoveryRequest wrong = request;
            ++wrong.sequence;
            return Rebuild(wrong, survivors);
        };
        auto recovered = coordinator.Recover(observed, FailureFor(*observed, failed.service.Id()), provider);
        REQUIRE_FALSE(recovered.has_value());
        REQUIRE(recovered.error().code == RuntimeRecoveryErrorCode::ReplacementSequenceMismatch);
        REQUIRE(coordinator.Current() == observed);
    }

    SECTION("failed service retained is rejected") {
        auto created = RuntimeRecoveryCoordinator::Create(MakeEpoch(1, failed, survivor));
        REQUIRE(created.has_value());
        auto& coordinator = **created;
        const auto observed = coordinator.Current();
        const std::array<const ServiceAndState*, 2> retained{&failed, &survivor};
        auto provider =
            [&](const RuntimeRecoveryRequest& request) noexcept -> std::expected<RuntimeEpoch, RuntimeRecoveryError> {
            return Rebuild(request, retained);
        };
        auto recovered = coordinator.Recover(observed, FailureFor(*observed, failed.service.Id()), provider);
        REQUIRE_FALSE(recovered.has_value());
        REQUIRE(recovered.error().code == RuntimeRecoveryErrorCode::FailedServiceRetained);
        REQUIRE(coordinator.Current() == observed);
    }
}

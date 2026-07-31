/**
 * @file RecoveryBenchmark.cpp
 * @brief Runtime recovery control-plane latency and single-flight scaling
 * baseline.
 */

#include <BenchmarkHarness.h>
#include <Sora/Compute/Runtime/RuntimeRecoveryCoordinator.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <latch>
#include <memory>
#include <mutex>
#include <print>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace {

    using namespace Sora::Compute;

    constexpr size_t kSampleCount = 9;
    constexpr auto kMinimumSampleTime = std::chrono::milliseconds(50);

    std::atomic<bool> gFailed = false;

    template<typename T, typename Error>
    [[nodiscard]] T Take(std::expected<T, Error> result) {
        if (!result) {
            std::exit(2);
        }
        return std::move(*result);
    }

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

    [[nodiscard]] ServiceAndState MakeService(BackendServiceId id, DeviceId device) {
        auto state = std::make_unique<ObserverState>(device);
        auto service = Take(BackendService::Adopt({.id = id, .state = state.get(), .destroy = DestroyObserverState},
                                                  BackendCodeLease::Retain(std::make_shared<uint32_t>())));
        ObserverState* stateView = state.release();
        return {.service = std::move(service), .state = stateView};
    }

    [[nodiscard]] DeviceDescriptor Descriptor(DeviceId device) noexcept {
        return {.id = device,
                .backend = BackendKind::Vulkan,
                .kind = DeviceKind::DiscreteGpu,
                .architectureId = device.low,
                .subgroupWidthMinimum = 1,
                .subgroupWidthMaximum = 32,
                .maximumWorkgroupInvocations = 1024};
    }

    [[nodiscard]] TargetFingerprint Target(DeviceDescriptor device) noexcept {
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
                .compilerIdentity = {31, 1},
                .driverIdentity = {31, 2}};
    }

    [[nodiscard]] DeviceObserver MakeObserver(const ServiceAndState& value) {
        return Take(DeviceObserver::Create(
            value.service,
            {.device = value.state->device, .backend = BackendKind::Vulkan, .state = value.state, .observe = Observe}));
    }

    [[nodiscard]] RuntimeEpoch MakeEpoch(uint64_t sequence, std::span<const ServiceAndState* const> services) {
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
        auto catalog = Take(PhysicalResourceCatalog::Create(std::move(storage)));
        return Take(RuntimeEpoch::Create(sequence, std::move(catalog), std::move(bindings)));
    }

    template<typename... Services>
    [[nodiscard]] RuntimeEpoch MakeEpoch(uint64_t sequence, const Services&... services) {
        const std::array<const ServiceAndState*, sizeof...(Services)> values{&services...};
        return MakeEpoch(sequence, std::span<const ServiceAndState* const>{values});
    }

    [[nodiscard]] RuntimeEpochError FailureFor(const RuntimeEpoch& epoch, BackendServiceId service) {
        const auto found = std::ranges::find(epoch.Devices(), service, &RuntimeDeviceBinding::service);
        if (found == epoch.Devices().end()) {
            std::exit(2);
        }
        return {.code = RuntimeEpochErrorCode::DeviceLost,
                .device = static_cast<uint32_t>(found - epoch.Devices().begin()),
                .cause = DeviceObservationError::Lost};
    }

    [[nodiscard]] std::expected<RuntimeEpoch, RuntimeRecoveryError>
    Rebuild(const RuntimeRecoveryRequest& request, std::span<const ServiceAndState* const> services) noexcept {
        try {
            return MakeEpoch(request.sequence, services);
        } catch (const std::bad_alloc&) {
            return std::unexpected(RuntimeRecoveryError{.code = RuntimeRecoveryErrorCode::OutOfMemory,
                                                        .service = request.failedService,
                                                        .expectedSequence = request.sequence});
        }
    }

    struct RecoveryFixture {
        ServiceAndState failed;
        ServiceAndState survivor;
        std::unique_ptr<RuntimeRecoveryCoordinator> coordinator;
        std::shared_ptr<const RuntimeEpoch> observed;
        RuntimeEpochError failure{};
    };

    [[nodiscard]] RecoveryFixture MakeRecoveryFixture() {
        auto failed = MakeService({1, 101}, {1, 11});
        auto survivor = MakeService({2, 202}, {2, 22});
        auto coordinator = Take(RuntimeRecoveryCoordinator::Create(MakeEpoch(1, failed, survivor)));
        auto observed = coordinator->Current();
        auto failure = FailureFor(*observed, failed.service.Id());
        return {std::move(failed), std::move(survivor), std::move(coordinator), std::move(observed), failure};
    }

    struct EpochDiffFixture {
        std::vector<ServiceAndState> previousServices;
        std::vector<ServiceAndState> replacementServices;
        RuntimeEpoch previous;
        RuntimeEpoch replacement;
    };

    [[nodiscard]] EpochDiffFixture MakeEpochDiffFixture(size_t deviceCount) {
        std::vector<ServiceAndState> previousServices;
        std::vector<ServiceAndState> replacementServices;
        std::vector<const ServiceAndState*> previousViews;
        std::vector<const ServiceAndState*> replacementViews;
        previousServices.reserve(deviceCount);
        replacementServices.reserve(deviceCount);
        previousViews.reserve(deviceCount);
        replacementViews.reserve(deviceCount);
        for (size_t index = 0; index < deviceCount; ++index) {
            const uint64_t identity = static_cast<uint64_t>(index) + 1;
            const DeviceId device{identity, identity * 10};
            previousServices.push_back(MakeService({identity, 0x100}, device));
            replacementServices.push_back(MakeService({identity, 0x200}, device));
        }
        for (size_t index = 0; index < deviceCount; ++index) {
            previousViews.push_back(std::addressof(previousServices[index]));
            replacementViews.push_back(std::addressof(replacementServices[index]));
        }
        RuntimeEpoch previous = MakeEpoch(1, std::span<const ServiceAndState* const>{previousViews});
        RuntimeEpoch replacement = MakeEpoch(2, std::span<const ServiceAndState* const>{replacementViews});
        return {std::move(previousServices), std::move(replacementServices), std::move(previous),
                std::move(replacement)};
    }

    [[nodiscard]] double MeasureUncontendedRecovery() {
        std::array<double, kSampleCount> samples{};
        for (double& sample : samples) {
            auto fixture = MakeRecoveryFixture();
            const std::array<const ServiceAndState*, 1> survivors{&fixture.survivor};
            uint32_t providerCalls = 0;
            auto provider = [&](const RuntimeRecoveryRequest& request) noexcept
                -> std::expected<RuntimeEpoch, RuntimeRecoveryError> {
                ++providerCalls;
                return Rebuild(request, survivors);
            };

            const auto begin = std::chrono::steady_clock::now();
            auto recovered = fixture.coordinator->Recover(fixture.observed, fixture.failure, provider);
            const auto end = std::chrono::steady_clock::now();
            if (!recovered || providerCalls != 1 || (*recovered)->Sequence() != 2) {
                gFailed.store(true, std::memory_order_relaxed);
            }
            sample = std::chrono::duration<double, std::micro>(end - begin).count();
        }
        return Sora::Benchmark::Median(samples);
    }

    template<size_t WaiterCount>
        requires(WaiterCount > 1)
    [[nodiscard]] double RunParkedWakeBurst() {
        std::latch ready{WaiterCount};
        std::atomic<size_t> completed = 0;
        std::atomic<bool> release = false;
        std::mutex mutex;
        std::condition_variable released;
        std::array<std::jthread, WaiterCount> threads{};
        threads[0] = std::jthread([&] {
            ready.count_down();
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            completed.fetch_add(1, std::memory_order_release);
        });
        for (size_t index = 1; index < WaiterCount; ++index) {
            threads[index] = std::jthread([&] {
                std::unique_lock lock{mutex};
                ready.count_down();
                released.wait(lock, [&release] { return release.load(std::memory_order_acquire); });
                completed.fetch_add(1, std::memory_order_release);
            });
        }

        ready.wait();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto begin = std::chrono::steady_clock::now();
        {
            std::scoped_lock lock{mutex};
            release.store(true, std::memory_order_release);
        }
        released.notify_all();
        while (completed.load(std::memory_order_acquire) != WaiterCount) {
            std::this_thread::yield();
        }
        const auto end = std::chrono::steady_clock::now();
        for (std::jthread& thread : threads) {
            thread.join();
        }
        return std::chrono::duration<double, std::micro>(end - begin).count();
    }

    template<size_t WaiterCount>
        requires(WaiterCount > 1)
    [[nodiscard]] double RunSingleFlightRecoveryBurst() {
        auto fixture = MakeRecoveryFixture();
        const std::array<const ServiceAndState*, 1> survivors{&fixture.survivor};
        std::atomic<uint32_t> providerCalls = 0;
        std::latch providerEntered{1};
        std::atomic<bool> releaseProvider = false;
        auto provider =
            [&](const RuntimeRecoveryRequest& request) noexcept -> std::expected<RuntimeEpoch, RuntimeRecoveryError> {
            providerCalls.fetch_add(1, std::memory_order_relaxed);
            providerEntered.count_down();
            while (!releaseProvider.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return Rebuild(request, survivors);
        };

        std::latch ready{WaiterCount};
        std::latch start{1};
        std::atomic<size_t> completed = 0;
        std::array<std::shared_ptr<const RuntimeEpoch>, WaiterCount> results{};
        std::array<std::jthread, WaiterCount> threads{};
        for (size_t index = 0; index < WaiterCount; ++index) {
            threads[index] = std::jthread([&, index] {
                ready.count_down();
                start.wait();
                auto recovered = fixture.coordinator->Recover(fixture.observed, fixture.failure, provider);
                if (recovered) {
                    results[index] = std::move(*recovered);
                } else {
                    gFailed.store(true, std::memory_order_relaxed);
                }
                completed.fetch_add(1, std::memory_order_release);
            });
        }

        ready.wait();
        start.count_down();
        providerEntered.wait();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto begin = std::chrono::steady_clock::now();
        releaseProvider.store(true, std::memory_order_release);
        while (completed.load(std::memory_order_acquire) != WaiterCount) {
            std::this_thread::yield();
        }
        const auto end = std::chrono::steady_clock::now();
        for (std::jthread& thread : threads) {
            thread.join();
        }

        const auto current = fixture.coordinator->Current();
        if (providerCalls.load(std::memory_order_relaxed) != 1 ||
            !std::ranges::all_of(results, [&current](const auto& result) { return result == current; })) {
            gFailed.store(true, std::memory_order_relaxed);
        }
        return std::chrono::duration<double, std::micro>(end - begin).count();
    }

    struct SingleFlightTiming {
        double wakeMicroseconds = 0;
        double recoveryMicroseconds = 0;
        double pairedRatio = 0;
    };

    template<size_t WaiterCount>
        requires(WaiterCount > 1)
    [[nodiscard]] SingleFlightTiming MeasureSingleFlightRecovery() {
        std::array<double, kSampleCount> wakeSamples{};
        std::array<double, kSampleCount> recoverySamples{};
        std::array<double, kSampleCount> ratios{};
        for (size_t sample = 0; sample < kSampleCount; ++sample) {
            if (sample % 2 == 0) {
                wakeSamples[sample] = RunParkedWakeBurst<WaiterCount>();
                recoverySamples[sample] = RunSingleFlightRecoveryBurst<WaiterCount>();
            } else {
                recoverySamples[sample] = RunSingleFlightRecoveryBurst<WaiterCount>();
                wakeSamples[sample] = RunParkedWakeBurst<WaiterCount>();
            }
            ratios[sample] = recoverySamples[sample] / wakeSamples[sample];
        }
        return {.wakeMicroseconds = Sora::Benchmark::Median(wakeSamples),
                .recoveryMicroseconds = Sora::Benchmark::Median(recoverySamples),
                .pairedRatio = Sora::Benchmark::Median(ratios)};
    }

} // namespace

int main() {
    auto healthy = MakeRecoveryFixture();
    auto oneDeviceDiff = MakeEpochDiffFixture(1);
    auto eightDeviceDiff = MakeEpochDiffFixture(8);
    uint64_t checksum = 0;
    auto currentLoad = [&]() noexcept {
        auto current = healthy.coordinator->Current();
        checksum += current->Sequence();
    };
    auto snapshotRefresh = [&]() noexcept {
        auto refreshed = healthy.observed->RefreshDeviceSnapshots();
        if (!refreshed) {
            gFailed.store(true, std::memory_order_relaxed);
            return;
        }
        checksum += refreshed->size();
    };
    auto diffOneDevice = [&]() noexcept {
        auto diff = DiffRuntimeEpochs(oneDeviceDiff.previous, oneDeviceDiff.replacement);
        if (!diff) {
            gFailed.store(true, std::memory_order_relaxed);
            return;
        }
        checksum += diff->removedServices.size() + diff->addedServices.size() + diff->replacements.size();
    };
    auto diffEightDevices = [&]() noexcept {
        auto diff = DiffRuntimeEpochs(eightDeviceDiff.previous, eightDeviceDiff.replacement);
        if (!diff) {
            gFailed.store(true, std::memory_order_relaxed);
            return;
        }
        checksum += diff->removedServices.size() + diff->addedServices.size() + diff->replacements.size();
    };
    const std::array healthyCases{
        Sora::Benchmark::MakeCase("current_epoch_load", currentLoad),
        Sora::Benchmark::MakeCase("healthy_snapshot_refresh", snapshotRefresh),
    };
    const std::array diffCases{
        Sora::Benchmark::MakeCase("epoch_diff_1_device", diffOneDevice),
        Sora::Benchmark::MakeCase("epoch_diff_8_devices", diffEightDevices),
    };
    const auto healthyTiming =
        Sora::Benchmark::MeasureInterleavedIndependent<kSampleCount>(healthyCases, 1, kMinimumSampleTime);
    const auto diffTiming =
        Sora::Benchmark::MeasureInterleavedIndependent<kSampleCount>(diffCases, 1, kMinimumSampleTime);

    const double uncontended = MeasureUncontendedRecovery();
    const SingleFlightTiming eightWaiters = MeasureSingleFlightRecovery<8>();
    const SingleFlightTiming thirtyTwoWaiters = MeasureSingleFlightRecovery<32>();
    if (gFailed.load(std::memory_order_relaxed)) {
        return 2;
    }

    std::println("family,parallelism,metric,value,unit");
    std::println("recovery_healthy,1,current_epoch_load,{:.6f},ns/operation", healthyTiming.MedianTime(0));
    std::println("recovery_healthy,1,healthy_snapshot_refresh,{:.6f},ns/operation", healthyTiming.MedianTime(1));
    std::println("rediscovery_diff,1,full_service_replacement,{:.6f},ns/operation", diffTiming.MedianTime(0));
    std::println("rediscovery_diff,8,full_service_replacement,{:.6f},ns/operation", diffTiming.MedianTime(1));
    std::println("recovery_transaction,1,uncontended_device_recovery,{:.6f},us/recovery", uncontended);
    std::println("recovery_single_flight,8,parked_waiter_burst,{:.6f},us/recovery", eightWaiters.recoveryMicroseconds);
    std::println("scheduler_control,8,parked_wake_burst,{:.6f},us/burst", eightWaiters.wakeMicroseconds);
    std::println("recovery_ratio,8,recovery_over_wake,{:.6f},ratio", eightWaiters.pairedRatio);
    std::println("recovery_single_flight,8,amortized_waiter,{:.6f},ns/waiter",
                 eightWaiters.recoveryMicroseconds * 1'000.0 / 8.0);
    std::println("recovery_single_flight,32,parked_waiter_burst,{:.6f},us/recovery",
                 thirtyTwoWaiters.recoveryMicroseconds);
    std::println("scheduler_control,32,parked_wake_burst,{:.6f},us/burst", thirtyTwoWaiters.wakeMicroseconds);
    std::println("recovery_ratio,32,recovery_over_wake,{:.6f},ratio", thirtyTwoWaiters.pairedRatio);
    std::println("recovery_single_flight,32,amortized_waiter,{:.6f},ns/waiter",
                 thirtyTwoWaiters.recoveryMicroseconds * 1'000.0 / 32.0);
    std::println("checksum,1,control_plane,{},value", checksum);
}

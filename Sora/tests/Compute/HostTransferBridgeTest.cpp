#include <Sora/Compute/Backend/CpuBackend.h>
#include <Sora/Compute/Memory/ResidencyPlanner.h>
#include <Sora/Compute/Runtime/CalibratedTransferTopology.h>
#include <Sora/Compute/Runtime/HostTransferBridge.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace {

    using namespace Sora::Compute;

    struct BackendPair {
        CpuBackend first;
        CpuBackend second;
    };

    [[nodiscard]] BackendPair MakeBackends() {
        auto first = CpuBackend::Create({.workerCount = 1});
        auto second = CpuBackend::Create({.workerCount = 1});
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        return {std::move(*first), std::move(*second)};
    }

    [[nodiscard]] std::vector<TransferEngine> MakeBridges(const BackendPair& backends) {
        const std::array accessors{backends.first.HostAccessor(), backends.second.HostAccessor()};
        auto bridges = CreateHostTransferBridges(accessors);
        REQUIRE(bridges.has_value());
        REQUIRE(bridges->size() == 2);
        return std::move(*bridges);
    }

    [[nodiscard]] const TransferEngine& FindBridge(std::span<const TransferEngine> bridges, BackendServiceId source,
                                                   BackendServiceId destination) {
        const auto found = std::ranges::find_if(bridges, [&](const TransferEngine& bridge) {
            return bridge.SourceService() == source && bridge.Service() == destination;
        });
        REQUIRE(found != bridges.end());
        return *found;
    }

    [[nodiscard]] BackendAllocationRequest HostAllocationRequest(const CpuBackend& backend, uint64_t bytes) {
        return {.location = backend.HostLocation(),
                .requiredProperties = Detail::kCoherentHostMemoryProperties,
                .preferredProperties = Detail::kCoherentHostMemoryProperties,
                .sizeBytes = bytes,
                .alignment = 64};
    }

    [[nodiscard]] PhysicalResourceCatalog MakeCatalog(const BackendPair& backends,
                                                      std::span<const TransferEngine> bridges) {
        PhysicalResourceCatalogStorage storage;
        storage.deviceObservers = {backends.first.Observer(), backends.second.Observer()};
        storage.allocators = {backends.first.DeviceAllocator(), backends.first.HostAllocator(),
                              backends.second.DeviceAllocator(), backends.second.HostAllocator()};
        storage.hostMemoryAccessors = {backends.first.HostAccessor(), backends.second.HostAccessor()};
        const auto appendTransfers = [&](std::span<const TransferEngine> transfers) {
            storage.transferEngines.insert(storage.transferEngines.end(), transfers.begin(), transfers.end());
        };
        appendTransfers(backends.first.TransferEngines());
        appendTransfers(backends.second.TransferEngines());
        appendTransfers(bridges);
        auto catalog = PhysicalResourceCatalog::Create(std::move(storage));
        REQUIRE(catalog.has_value());
        return std::move(*catalog);
    }

    [[nodiscard]] long ServiceOwners(const HostMemoryAccessor& accessor) {
        BackendService retained = accessor.RetainService();
        return retained.UseCount() - 1;
    }

    void ReturnInconsistentMapping(void* opaque, const BackendHostMemoryRequest* request,
                                   BackendHostMemoryResult* result) noexcept {
        result->address = static_cast<std::byte*>(opaque);
        result->sizeBytes = request->sizeBytes;
        result->error = {HostMemoryErrorCode::MappingFailed, 17};
    }

} // namespace

TEST_CASE("Inter-service host bridges copy exact byte ranges in both directions",
          "[Sora][Compute][Runtime][HostBridge][Copy]") {
    BackendPair backends = MakeBackends();
    const std::vector<TransferEngine> bridges = MakeBridges(backends);
    const uint64_t bytes = 4096;
    auto firstAllocation = backends.first.HostAllocator().Allocate(HostAllocationRequest(backends.first, bytes));
    auto secondAllocation = backends.second.HostAllocator().Allocate(HostAllocationRequest(backends.second, bytes));
    REQUIRE(firstAllocation.has_value());
    REQUIRE(secondAllocation.has_value());
    auto firstBytes = backends.first.HostAccessor().Write(*firstAllocation, 0, bytes);
    auto secondBytes = backends.second.HostAccessor().Write(*secondAllocation, 0, bytes);
    REQUIRE(firstBytes.has_value());
    REQUIRE(secondBytes.has_value());
    for (size_t index = 0; index < firstBytes->size(); ++index) {
        (*firstBytes)[index] = std::byte{static_cast<uint8_t>(index * 37U)};
    }
    std::ranges::fill(*secondBytes, std::byte{0});

    const TransferEngine& forward =
        FindBridge(bridges, backends.first.Observer().Service(), backends.second.Observer().Service());
    const std::array forwardRegions{TransferRegion{.sourceOffset = 128, .targetOffset = 256, .sizeBytes = 1024},
                                    TransferRegion{.sourceOffset = 2048, .targetOffset = 2048, .sizeBytes = 512}};
    auto forwardSubmission = forward.Submit(*firstAllocation, *secondAllocation, forwardRegions);
    REQUIRE(forwardSubmission.has_value());
    REQUIRE(forwardSubmission->WaitUntilTerminal().status == EventStatus::Completed);
    REQUIRE(std::ranges::equal(firstBytes->subspan(128, 1024), secondBytes->subspan(256, 1024)));
    REQUIRE(std::ranges::equal(firstBytes->subspan(2048, 512), secondBytes->subspan(2048, 512)));

    std::ranges::fill(firstBytes->subspan(512, 1024), std::byte{0});
    const TransferEngine& reverse =
        FindBridge(bridges, backends.second.Observer().Service(), backends.first.Observer().Service());
    const std::array reverseRegions{TransferRegion{.sourceOffset = 256, .targetOffset = 512, .sizeBytes = 1024}};
    auto reverseSubmission = reverse.Submit(*secondAllocation, *firstAllocation, reverseRegions);
    REQUIRE(reverseSubmission.has_value());
    REQUIRE(reverseSubmission->WaitUntilTerminal().status == EventStatus::Completed);
    REQUIRE(std::ranges::equal(secondBytes->subspan(256, 1024), firstBytes->subspan(512, 1024)));
}

TEST_CASE("Host accessors reject foreign, non-host-location, and out-of-range allocations",
          "[Sora][Compute][Backend][HostMemory][Validation]") {
    BackendPair backends = MakeBackends();
    auto foreign = backends.second.HostAllocator().Allocate(HostAllocationRequest(backends.second, 256));
    REQUIRE(foreign.has_value());
    const auto foreignMap = backends.first.HostAccessor().Read(*foreign, 0, 64);
    REQUIRE_FALSE(foreignMap.has_value());
    REQUIRE(foreignMap.error().code == HostMemoryErrorCode::ForeignAllocation);

    auto device = backends.first.DeviceAllocator().Allocate({.location = backends.first.DeviceLocation(),
                                                             .requiredProperties = MemoryProperty::DeviceLocal,
                                                             .sizeBytes = 256,
                                                             .alignment = 64});
    REQUIRE(device.has_value());
    const auto deviceMap = backends.first.HostAccessor().Write(*device, 0, 64);
    REQUIRE_FALSE(deviceMap.has_value());
    REQUIRE(deviceMap.error().code == HostMemoryErrorCode::ForeignAllocation);

    auto local = backends.first.HostAllocator().Allocate(HostAllocationRequest(backends.first, 256));
    REQUIRE(local.has_value());
    const auto range = backends.first.HostAccessor().Read(*local, 224, 64);
    REQUIRE_FALSE(range.has_value());
    REQUIRE(range.error().code == HostMemoryErrorCode::InvalidRequest);

    std::array<AllocationView, 5> forgedViews{};
    forgedViews.fill(local->View());
    forgedViews[0].memory.low ^= 1;
    forgedViews[1].properties = forgedViews[1].properties | MemoryProperty::DeviceLocal;
    forgedViews[2].memoryOffset = 1;
    --forgedViews[3].sizeBytes;
    ++forgedViews[4].alignment;
    for (const AllocationView forged : forgedViews) {
        const auto forgedMap = backends.first.HostAccessor().Read(forged, 0, 64);
        REQUIRE_FALSE(forgedMap.has_value());
        REQUIRE(forgedMap.error().code == HostMemoryErrorCode::ForeignAllocation);
    }
}

TEST_CASE("Host accessor rejects an error result carrying a borrowed mapping",
          "[Sora][Compute][Backend][HostMemory][Validation]") {
    BackendPair backends = MakeBackends();
    auto allocation = backends.first.HostAllocator().Allocate(HostAllocationRequest(backends.first, 256));
    REQUIRE(allocation.has_value());
    std::byte inconsistentState{};
    auto accessor = HostMemoryAccessor::Create(
        backends.first.HostAccessor().RetainService(),
        {.location = backends.first.HostLocation(), .state = &inconsistentState, .map = ReturnInconsistentMapping});
    REQUIRE(accessor.has_value());
    const auto mapped = accessor->Read(*allocation, 0, 64);
    REQUIRE_FALSE(mapped.has_value());
    REQUIRE(mapped.error().code == HostMemoryErrorCode::InvalidResult);
    REQUIRE(mapped.error().detail == 64);
}

TEST_CASE("Host bridge callback state retains both services without an ownership cycle",
          "[Sora][Compute][Runtime][HostBridge][Lifetime]") {
    BackendPair backends = MakeBackends();
    const long firstBaseline = ServiceOwners(backends.first.HostAccessor());
    const long secondBaseline = ServiceOwners(backends.second.HostAccessor());
    {
        const std::vector<TransferEngine> bridges = MakeBridges(backends);
        REQUIRE(ServiceOwners(backends.first.HostAccessor()) > firstBaseline);
        REQUIRE(ServiceOwners(backends.second.HostAccessor()) > secondBaseline);

        const std::vector<TransferEngine> repeated = MakeBridges(backends);
        for (const TransferEngine& bridge : bridges) {
            const TransferEngine& match = FindBridge(repeated, bridge.SourceService(), bridge.Service());
            REQUIRE(match.Id() == bridge.Id());
        }
    }
    REQUIRE(ServiceOwners(backends.first.HostAccessor()) == firstBaseline);
    REQUIRE(ServiceOwners(backends.second.HostAccessor()) == secondBaseline);
}

TEST_CASE("Bounded host calibration is the only path that makes synthesized bridges plannable",
          "[Sora][Compute][Runtime][HostBridge][Calibration]") {
    BackendPair backends = MakeBackends();
    const std::vector<TransferEngine> bridges = MakeBridges(backends);
    PhysicalResourceCatalog catalog = MakeCatalog(backends, bridges);
    const auto uncalibrated =
        ProjectCalibratedTransferTopology(catalog.Inventory(), PerformanceSnapshot::Empty(), 4096);
    REQUIRE(uncalibrated.has_value());
    REQUIRE(uncalibrated->Links().empty());

    PerformanceModel performance;
    const HostTransferCalibrationConfig config{
        .payloadBytes = {1, 64, 4096, 64U << 10U}, .warmupSamples = 0, .measuredSamples = 3};
    REQUIRE(CalibrateHostTransferBridges(catalog, performance, config).has_value());
    const auto snapshot = performance.Snapshot();
    REQUIRE(snapshot != nullptr);
    const auto calibrated = ProjectCalibratedTransferTopology(catalog.Inventory(), *snapshot, 4096);
    REQUIRE(calibrated.has_value());
    REQUIRE(calibrated->Links().size() == 2);
    REQUIRE(std::ranges::all_of(calibrated->Links(), [](const TransferLink& link) {
        return link.kind == TransferKind::HostBridge && link.bytesPerSecond != 0 && link.latencyNanoseconds != 0;
    }));

    for (const PhysicalTransferDescriptor& descriptor : catalog.Inventory().Transfers()) {
        if (descriptor.kind == TransferKind::HostBridge) {
            continue;
        }
        REQUIRE(performance
                    .Update({.key = {.service = descriptor.service,
                                     .engine = descriptor.engine,
                                     .source = descriptor.source,
                                     .destination = descriptor.destination},
                             .bytes = 4096,
                             .payloadNanoseconds = 100,
                             .latencyNanoseconds = 1})
                    .has_value());
    }
    const auto completeSnapshot = performance.Snapshot();
    const auto completeTopology = ProjectCalibratedTransferTopology(catalog.Inventory(), *completeSnapshot, 4096);
    REQUIRE(completeTopology.has_value());
    const std::array origins{ResidencyOrigin{backends.first.Observer().Service(), backends.first.DeviceLocation()}};
    const auto region = ByteRegion::Make(0, 4096);
    REQUIRE(region.has_value());
    auto route = PlanResidencyPath(origins,
                                   {.service = backends.second.Observer().Service(),
                                    .destination = backends.second.DeviceLocation(),
                                    .region = *region},
                                   completeTopology->Links());
    REQUIRE(route.has_value());
    REQUIRE(route->hops.size() == 3);
    REQUIRE(route->hops[0].kind == TransferKind::CoherentAlias);
    REQUIRE(route->hops[1].kind == TransferKind::HostBridge);
    REQUIRE(route->hops[1].sourceService == backends.first.Observer().Service());
    REQUIRE(route->hops[1].service == backends.second.Observer().Service());
    REQUIRE(route->hops[2].kind == TransferKind::CoherentAlias);
}

TEST_CASE("Host calibration rejects unbounded work and insufficient model capacity before measurement",
          "[Sora][Compute][Runtime][HostBridge][Calibration]") {
    HostTransferCalibrationConfig config{};
    config.warmupSamples = kMaximumHostTransferCalibrationWarmups + 1;
    REQUIRE_FALSE(config.Valid());
    config = {};
    config.measuredSamples = kMaximumHostTransferCalibrationSamples + 1;
    REQUIRE_FALSE(config.Valid());
    config = {};
    config.payloadBytes.back() = kMaximumHostTransferCalibrationPayloadBytes + 1;
    REQUIRE_FALSE(config.Valid());

    BackendPair backends = MakeBackends();
    const std::vector<TransferEngine> bridges = MakeBridges(backends);
    PhysicalResourceCatalog catalog = MakeCatalog(backends, bridges);
    PerformanceModel performance({.maximumTransferKeys = 1});
    const auto calibrated = CalibrateHostTransferBridges(
        catalog, performance, {.payloadBytes = {1, 64, 4096, 64U << 10U}, .warmupSamples = 0, .measuredSamples = 1});
    REQUIRE_FALSE(calibrated.has_value());
    REQUIRE(calibrated.error().code == HostTransferBridgeErrorCode::InsufficientModelCapacity);
    REQUIRE(performance.Snapshot() == nullptr);
}

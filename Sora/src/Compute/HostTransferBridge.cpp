/**
 * @file HostTransferBridge.cpp
 * @brief Portable inter-service host-copy materialization and bounded calibration.
 * @ingroup Compute
 */

#include <Sora/Compute/Runtime/HostTransferBridge.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <ranges>
#include <tuple>

namespace Sora::Compute {

    namespace {

        struct HostBridgeState {
            HostMemoryAccessor source;
            HostMemoryAccessor destination;
        };

        struct CompletedEventSentinel {};

        [[nodiscard]] constexpr auto AccessorKey(const HostMemoryAccessor& accessor) noexcept {
            const BackendServiceId service = accessor.Service();
            return std::tuple{service.low, service.high, accessor.Location()};
        }

        [[nodiscard]] constexpr uint64_t Mix(uint64_t value) noexcept {
            value ^= value >> 30U;
            value *= 0xBF58476D1CE4E5B9ULL;
            value ^= value >> 27U;
            value *= 0x94D049BB133111EBULL;
            return value ^ (value >> 31U);
        }

        [[nodiscard]] constexpr TransferEngineId BridgeId(const HostMemoryAccessor& source,
                                                          const HostMemoryAccessor& destination) noexcept {
            const BackendServiceId sourceService = source.Service();
            const BackendServiceId destinationService = destination.Service();
            const MemoryLocation sourceLocation = source.Location();
            const MemoryLocation destinationLocation = destination.Location();
            uint64_t low = 0x534F52415F484252ULL;
            uint64_t high = 0x494447455F563100ULL;
            const auto append = [&](uint64_t word) {
                low = Mix(low ^ word);
                high = Mix(high ^ std::rotl(word, 23) ^ low);
            };
            append(sourceService.low);
            append(sourceService.high);
            append(destinationService.low);
            append(destinationService.high);
            append(sourceLocation.domain.low);
            append(sourceLocation.domain.high);
            append(sourceLocation.device.low);
            append(sourceLocation.device.high);
            append(std::to_underlying(sourceLocation.backend));
            append(destinationLocation.domain.low);
            append(destinationLocation.domain.high);
            append(destinationLocation.device.low);
            append(destinationLocation.device.high);
            append(std::to_underlying(destinationLocation.backend));
            return {low, high};
        }

        void DestroyCompletedEvent(void*) noexcept {}

        void ObserveCompletedEvent(void*, BackendEventObservation* observation) noexcept {
            *observation = {.status = EventStatus::Completed};
        }

        void WaitCompletedEvent(void*, uint64_t, BackendEventObservation* observation) noexcept {
            *observation = {.status = EventStatus::Completed};
        }

        [[nodiscard]] BackendEventPayload CompletedEvent(MemoryLocation destination) noexcept {
            static CompletedEventSentinel sentinel;
            return {.device = destination.device,
                    .backend = destination.backend,
                    .state = &sentinel,
                    .destroy = DestroyCompletedEvent,
                    .query = ObserveCompletedEvent,
                    .wait = WaitCompletedEvent};
        }

        [[nodiscard]] constexpr TransferError MapHostError(HostMemoryError error) noexcept {
            switch (error.code) {
                case HostMemoryErrorCode::InvalidApi:
                case HostMemoryErrorCode::InvalidRequest:
                case HostMemoryErrorCode::ForeignAllocation:
                case HostMemoryErrorCode::UnsupportedAccess:
                    return {TransferErrorCode::InvalidRequest, error.detail};
                case HostMemoryErrorCode::InvalidResult:
                    return {TransferErrorCode::InvalidResult, error.detail};
                case HostMemoryErrorCode::MappingFailed:
                    return {TransferErrorCode::TransferFailed, error.detail};
                case HostMemoryErrorCode::None:
                    break;
            }
            return {TransferErrorCode::TransferFailed, error.detail};
        }

        void TransferHostBridge(void* opaque, const BackendTransferRequest* request,
                                BackendTransferResult* result) noexcept {
            if (request->waitCount != 0) {
                result->error = {TransferErrorCode::UnsupportedTransfer, request->waitCount};
                return;
            }
            auto& state = *static_cast<HostBridgeState*>(opaque);
            uint64_t sourceBegin = UINT64_MAX;
            uint64_t sourceEnd = 0;
            uint64_t targetBegin = UINT64_MAX;
            uint64_t targetEnd = 0;
            for (const TransferRegion region : request->Regions()) {
                sourceBegin = std::min(sourceBegin, region.sourceOffset);
                sourceEnd = std::max(sourceEnd, region.sourceOffset + region.sizeBytes);
                targetBegin = std::min(targetBegin, region.targetOffset);
                targetEnd = std::max(targetEnd, region.targetOffset + region.sizeBytes);
            }
            auto source = state.source.Read(request->source, sourceBegin, sourceEnd - sourceBegin);
            if (!source) {
                result->error = MapHostError(source.error());
                return;
            }
            auto destination = state.destination.Write(request->target, targetBegin, targetEnd - targetBegin);
            if (!destination) {
                result->error = MapHostError(destination.error());
                return;
            }
            for (const TransferRegion region : request->Regions()) {
                const auto sourceBytes = source->subspan(static_cast<size_t>(region.sourceOffset - sourceBegin),
                                                         static_cast<size_t>(region.sizeBytes));
                const auto destinationBytes = destination->subspan(
                    static_cast<size_t>(region.targetOffset - targetBegin), static_cast<size_t>(region.sizeBytes));
                const auto sourceRange = request->source.PhysicalRange(region.sourceOffset, region.sizeBytes);
                const auto destinationRange = request->target.PhysicalRange(region.targetOffset, region.sizeBytes);
                if (sourceRange && destinationRange && sourceRange->Intersects(*destinationRange)) {
                    std::memmove(destinationBytes.data(), sourceBytes.data(), destinationBytes.size());
                } else {
                    std::memcpy(destinationBytes.data(), sourceBytes.data(), destinationBytes.size());
                }
            }
            result->completion = CompletedEvent(state.destination.Location());
            result->error = {};
        }

        [[nodiscard]] BackendAllocationRequest AllocationRequest(MemoryLocation location, uint64_t bytes) noexcept {
            return {.location = location,
                    .requiredProperties = Detail::kCoherentHostMemoryProperties,
                    .preferredProperties = Detail::kCoherentHostMemoryProperties,
                    .sizeBytes = bytes,
                    .alignment = alignof(std::max_align_t)};
        }

        [[nodiscard]] uint64_t ElapsedNanoseconds(std::chrono::steady_clock::time_point start,
                                                  std::chrono::steady_clock::time_point end) noexcept {
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            return static_cast<uint64_t>(std::max<int64_t>(elapsed, 1));
        }

        [[nodiscard]] std::expected<uint64_t, TransferError>
        MeasureTransfer(const TransferEngine& engine, const Allocation& source, const Allocation& destination,
                        uint64_t bytes, uint32_t samples) noexcept {
            std::array<uint64_t, kMaximumHostTransferCalibrationSamples> elapsed{};
            for (uint32_t sample = 0; sample < samples; ++sample) {
                const auto start = std::chrono::steady_clock::now();
                const std::array regions{TransferRegion{.sizeBytes = bytes}};
                auto submission = engine.Submit(source, destination, regions);
                if (!submission) {
                    return std::unexpected(submission.error());
                }
                const EventObservation observation = submission->WaitUntilTerminal();
                const auto end = std::chrono::steady_clock::now();
                if (observation.status != EventStatus::Completed) {
                    return std::unexpected(TransferError{TransferErrorCode::TransferFailed, observation.error.detail});
                }
                elapsed[sample] = ElapsedNanoseconds(start, end);
            }
            auto measured = std::span{elapsed}.first(samples);
            std::ranges::sort(measured);
            return measured[samples / 2U];
        }

    } // namespace

    std::expected<std::vector<TransferEngine>, HostTransferBridgeError>
    CreateHostTransferBridges(std::span<const HostMemoryAccessor> accessors) noexcept {
        if (accessors.size() > UINT32_MAX) {
            return std::unexpected(HostTransferBridgeError{HostTransferBridgeErrorCode::CapacityExceeded});
        }
        try {
            std::vector<const HostMemoryAccessor*> canonical;
            canonical.reserve(accessors.size());
            for (const HostMemoryAccessor& accessor : accessors) {
                canonical.push_back(std::addressof(accessor));
            }
            std::ranges::sort(canonical, {}, [](const HostMemoryAccessor* accessor) { return AccessorKey(*accessor); });
            if (std::ranges::adjacent_find(canonical, {}, [](const HostMemoryAccessor* accessor) {
                    return AccessorKey(*accessor);
                }) != canonical.end()) {
                return std::unexpected(HostTransferBridgeError{HostTransferBridgeErrorCode::InvalidAccessorSet});
            }
            if (canonical.size() > 1 &&
                canonical.size() > std::numeric_limits<size_t>::max() / (canonical.size() - 1U)) {
                return std::unexpected(HostTransferBridgeError{HostTransferBridgeErrorCode::CapacityExceeded});
            }
            std::vector<TransferEngine> bridges;
            bridges.reserve(canonical.size() * (canonical.size() - std::min<size_t>(canonical.size(), 1)));
            for (const HostMemoryAccessor* source : canonical) {
                for (const HostMemoryAccessor* destination : canonical) {
                    if (source->Service() == destination->Service()) {
                        continue;
                    }
                    auto state = std::make_shared<HostBridgeState>(HostBridgeState{*source, *destination});
                    const TransferEngineId id = BridgeId(*source, *destination);
                    auto bridge =
                        TransferEngine::Create(destination->RetainService(), BackendCallbackStateLease::Retain(state),
                                               {.engine = id,
                                                .sourceService = source->Service(),
                                                .source = source->Location(),
                                                .destination = destination->Location(),
                                                .kind = TransferKind::HostBridge,
                                                .completionDevice = destination->Location().device,
                                                .completionBackend = destination->Location().backend,
                                                .state = state.get(),
                                                .transfer = TransferHostBridge});
                    if (!bridge) {
                        return std::unexpected(
                            HostTransferBridgeError{.code = HostTransferBridgeErrorCode::EngineConstructionFailed,
                                                    .edge = static_cast<uint32_t>(bridges.size()),
                                                    .transfer = bridge.error()});
                    }
                    bridges.push_back(std::move(*bridge));
                }
            }
            return bridges;
        } catch (const std::bad_alloc&) {
            return std::unexpected(HostTransferBridgeError{HostTransferBridgeErrorCode::OutOfMemory});
        } catch (const std::length_error&) {
            return std::unexpected(HostTransferBridgeError{HostTransferBridgeErrorCode::CapacityExceeded});
        }
    }

    std::expected<void, HostTransferBridgeError>
    CalibrateHostTransferBridges(const PhysicalResourceCatalog& resources, PerformanceModel& performance,
                                 HostTransferCalibrationConfig config) noexcept {
        if (!config.Valid()) {
            return std::unexpected(HostTransferBridgeError{HostTransferBridgeErrorCode::InvalidConfiguration});
        }
        const size_t bridgeCount = static_cast<size_t>(
            std::ranges::count_if(resources.Inventory().Transfers(), [](const PhysicalTransferDescriptor& descriptor) {
                return descriptor.kind == TransferKind::HostBridge;
            }));
        if (bridgeCount > performance.Limits().maximumTransferKeys) {
            return std::unexpected(HostTransferBridgeError{HostTransferBridgeErrorCode::InsufficientModelCapacity});
        }
        uint32_t edgeIndex = 0;
        for (const PhysicalTransferDescriptor& descriptor : resources.Inventory().Transfers()) {
            if (descriptor.kind != TransferKind::HostBridge) {
                continue;
            }
            const MemoryAllocator* sourceAllocator =
                resources.FindAllocator(descriptor.sourceService, descriptor.source);
            const MemoryAllocator* destinationAllocator =
                resources.FindAllocator(descriptor.service, descriptor.destination);
            const HostMemoryAccessor* sourceAccessor =
                resources.FindHostMemoryAccessor(descriptor.sourceService, descriptor.source);
            const HostMemoryAccessor* destinationAccessor =
                resources.FindHostMemoryAccessor(descriptor.service, descriptor.destination);
            const TransferEngine* engine = resources.FindTransferEngine(descriptor.service, descriptor.engine);
            if (sourceAllocator == nullptr || destinationAllocator == nullptr || sourceAccessor == nullptr ||
                destinationAccessor == nullptr || engine == nullptr) {
                return std::unexpected(
                    HostTransferBridgeError{.code = HostTransferBridgeErrorCode::MissingResource, .edge = edgeIndex});
            }

            std::array<uint64_t, kHostTransferCalibrationPayloadCount> elapsedNanoseconds{};
            for (uint32_t payloadIndex = 0; payloadIndex < config.payloadBytes.size(); ++payloadIndex) {
                const uint64_t bytes = config.payloadBytes[payloadIndex];
                auto source = sourceAllocator->Allocate(AllocationRequest(descriptor.source, bytes));
                if (!source) {
                    return std::unexpected(
                        HostTransferBridgeError{.code = HostTransferBridgeErrorCode::AllocationFailed,
                                                .edge = edgeIndex,
                                                .payload = payloadIndex,
                                                .allocation = source.error()});
                }
                auto destination = destinationAllocator->Allocate(AllocationRequest(descriptor.destination, bytes));
                if (!destination) {
                    return std::unexpected(
                        HostTransferBridgeError{.code = HostTransferBridgeErrorCode::AllocationFailed,
                                                .edge = edgeIndex,
                                                .payload = payloadIndex,
                                                .allocation = destination.error()});
                }
                auto sourceBytes = sourceAccessor->Write(*source, 0, bytes);
                auto destinationBytes = destinationAccessor->Write(*destination, 0, bytes);
                if (!sourceBytes || !destinationBytes) {
                    return std::unexpected(HostTransferBridgeError{.code = HostTransferBridgeErrorCode::MappingFailed,
                                                                   .edge = edgeIndex,
                                                                   .payload = payloadIndex,
                                                                   .mapping = sourceBytes ? destinationBytes.error()
                                                                                          : sourceBytes.error()});
                }
                std::ranges::fill(*sourceBytes, std::byte{0xA5});
                std::ranges::fill(*destinationBytes, std::byte{0});
                const std::array regions{TransferRegion{.sizeBytes = bytes}};
                for (uint32_t warmup = 0; warmup < config.warmupSamples; ++warmup) {
                    auto submission = engine->Submit(*source, *destination, regions);
                    if (!submission || submission->WaitUntilTerminal().status != EventStatus::Completed) {
                        return std::unexpected(HostTransferBridgeError{
                            .code = HostTransferBridgeErrorCode::TransferFailed,
                            .edge = edgeIndex,
                            .payload = payloadIndex,
                            .transfer =
                                submission ? TransferError{TransferErrorCode::TransferFailed, 0} : submission.error()});
                    }
                }
                auto elapsed = MeasureTransfer(*engine, *source, *destination, bytes, config.measuredSamples);
                if (!elapsed) {
                    return std::unexpected(HostTransferBridgeError{.code = HostTransferBridgeErrorCode::TransferFailed,
                                                                   .edge = edgeIndex,
                                                                   .payload = payloadIndex,
                                                                   .transfer = elapsed.error()});
                }
                if (!std::ranges::equal(*sourceBytes, *destinationBytes)) {
                    return std::unexpected(
                        HostTransferBridgeError{.code = HostTransferBridgeErrorCode::VerificationFailed,
                                                .edge = edgeIndex,
                                                .payload = payloadIndex});
                }
                elapsedNanoseconds[payloadIndex] = *elapsed;
            }
            const uint64_t minimumElapsed = *std::ranges::min_element(elapsedNanoseconds);
            const uint64_t latencyNanoseconds = minimumElapsed > 1 ? minimumElapsed - 1U : 1U;
            for (uint32_t payloadIndex = 0; payloadIndex < config.payloadBytes.size(); ++payloadIndex) {
                const uint64_t elapsed = elapsedNanoseconds[payloadIndex];
                const uint64_t payloadNanoseconds = elapsed > latencyNanoseconds ? elapsed - latencyNanoseconds : 1U;
                auto updated = performance.Update({.key = {.service = descriptor.service,
                                                           .engine = descriptor.engine,
                                                           .source = descriptor.source,
                                                           .destination = descriptor.destination},
                                                   .bytes = config.payloadBytes[payloadIndex],
                                                   .payloadNanoseconds = payloadNanoseconds,
                                                   .latencyNanoseconds = latencyNanoseconds});
                if (!updated) {
                    return std::unexpected(
                        HostTransferBridgeError{.code = HostTransferBridgeErrorCode::CalibrationRejected,
                                                .edge = edgeIndex,
                                                .payload = payloadIndex,
                                                .calibration = updated.error()});
                }
            }
            ++edgeIndex;
        }
        return {};
    }

} // namespace Sora::Compute

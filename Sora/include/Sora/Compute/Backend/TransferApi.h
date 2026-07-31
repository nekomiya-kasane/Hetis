/**
 * @file TransferApi.h
 * @brief Versioned transfer-engine ABI, coherent import, and asynchronous transfer ownership.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Backend/Event.h>
#include <Sora/Compute/Backend/Service.h>
#include <Sora/Compute/Memory/Allocation.h>
#include <Sora/Compute/Memory/Transfer.h>

#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Compute {

    /** @brief Stable transfer failure independent of a concrete backend or transport SDK. */
    enum class TransferErrorCode : uint8_t {
        None,
        InvalidApi,
        InvalidRequest,
        EngineUnavailable,
        UnsupportedTransfer,
        TransferFailed,
        InvalidResult,
        OutOfMemory,
        Stopped,
        DeviceLost,
    };

    /** @brief Transfer failure with an opaque backend diagnostic identity. */
    struct TransferError {
        TransferErrorCode code = TransferErrorCode::None;
        uint64_t detail = 0;

        friend constexpr bool operator==(TransferError, TransferError) noexcept = default;
    };

    /** @brief One source-to-target byte range in a canonical scatter/gather transfer. */
    struct TransferRegion {
        uint64_t sourceOffset = 0;
        uint64_t targetOffset = 0;
        uint64_t sizeBytes = 0;

        friend constexpr bool operator==(TransferRegion, TransferRegion) noexcept = default;
    };

    /** @brief Return whether @p regions form a non-empty, target-ordered transfer without physical write aliasing. */
    [[nodiscard]] constexpr bool ValidTransferRegions(AllocationView source, AllocationView target,
                                                      std::span<const TransferRegion> regions) noexcept {
        if (!source || !target || regions.empty()) {
            return false;
        }
        uint64_t previousTargetEnd = 0;
        for (const TransferRegion& region : regions) {
            if (region.sizeBytes == 0 || !source.Covers(region.sourceOffset, region.sizeBytes) ||
                !target.Covers(region.targetOffset, region.sizeBytes) || region.targetOffset < previousTargetEnd) {
                return false;
            }
            previousTargetEnd = region.targetOffset + region.sizeBytes;
        }

        if (source.service != target.service || source.memory != target.memory) {
            return true;
        }
        for (const TransferRegion& sourceRegion : regions) {
            const auto sourceRange = source.PhysicalRange(sourceRegion.sourceOffset, sourceRegion.sizeBytes);
            size_t first = 0;
            size_t last = regions.size();
            while (first < last) {
                const size_t middle = first + (last - first) / 2;
                const TransferRegion& targetRegion = regions[middle];
                const auto targetRange = target.PhysicalRange(targetRegion.targetOffset, targetRegion.sizeBytes);
                if (targetRange->bytes.End() <= sourceRange->bytes.Offset()) {
                    first = middle + 1;
                } else {
                    last = middle;
                }
            }
            if (first < regions.size()) {
                const TransferRegion& targetRegion = regions[first];
                const auto targetRange = target.PhysicalRange(targetRegion.targetOffset, targetRegion.sizeBytes);
                if (sourceRange->Intersects(*targetRange)) {
                    return false;
                }
            }
        }
        return true;
    }

    /** @brief Allocation-free request for one coherent import or asynchronous scatter/gather transfer. */
    struct BackendTransferRequest {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(BackendTransferRequest);
        TransferEngineId engine{};
        TransferKind kind = TransferKind::Copy;
        MemoryLocation destination{};
        AllocationView source{};
        AllocationView target{};
        const TransferRegion* regions = nullptr;
        uint32_t regionCount = 0;
        const EventView* waits = nullptr;
        uint32_t waitCount = 0;

        /** @brief Return canonical source-to-target regions without copying. */
        [[nodiscard]] constexpr std::span<const TransferRegion> Regions() const noexcept {
            return {regions, regionCount};
        }

        /** @brief Return prerequisite events without copying. */
        [[nodiscard]] constexpr std::span<const EventView> Waits() const noexcept { return {waits, waitCount}; }
    };

    /** @brief Callback result containing either an imported allocation or an asynchronous completion event. */
    struct BackendTransferResult {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(BackendTransferResult);
        AllocationPayload imported{};
        BackendEventPayload completion{};
        TransferError error{TransferErrorCode::TransferFailed, 0};
    };

    using BackendTransfer = void (*)(void* state, const BackendTransferRequest* request,
                                     BackendTransferResult* result) noexcept;

    /** @brief ABI-version-7 POD callback table for one service-transitioning directed transfer edge. */
    struct TransferEngineApiV7 {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(TransferEngineApiV7);
        TransferEngineId engine{};
        BackendServiceId sourceService{};
        MemoryLocation source{};
        MemoryLocation destination{};
        TransferKind kind = TransferKind::Copy;
        DeviceId completionDevice{};
        BackendKind completionBackend = BackendKind::Cpu;
        void* state = nullptr;
        BackendTransfer transfer = nullptr;
    };

    namespace Detail {

        [[nodiscard]] constexpr bool ValidTransferErrorCode(TransferErrorCode code) noexcept {
            return code == TransferErrorCode::None || code == TransferErrorCode::InvalidApi ||
                   code == TransferErrorCode::InvalidRequest || code == TransferErrorCode::EngineUnavailable ||
                   code == TransferErrorCode::UnsupportedTransfer || code == TransferErrorCode::TransferFailed ||
                   code == TransferErrorCode::InvalidResult || code == TransferErrorCode::OutOfMemory ||
                   code == TransferErrorCode::Stopped || code == TransferErrorCode::DeviceLost;
        }

        inline void ReleaseEventPayload(BackendEventPayload payload) noexcept {
            if (payload.state != nullptr && payload.destroy != nullptr) {
                payload.destroy(payload.state);
            }
        }

        inline void ReleaseTransferResult(BackendTransferResult result) noexcept {
            ReleaseAllocationPayload(result.imported);
            ReleaseEventPayload(result.completion);
        }

        [[nodiscard]] constexpr bool ValidTransferApi(const TransferEngineApiV7& api) noexcept {
            const bool validCompletion = api.kind == TransferKind::CoherentAlias ||
                                         (api.completionDevice.Valid() && ValidBackendKind(api.completionBackend));
            const bool validAlias =
                api.kind != TransferKind::CoherentAlias || api.source.domain == api.destination.domain;
            return ValidBackendAbi(api.abiVersion, api.structSize, sizeof(TransferEngineApiV7)) && api.engine.Valid() &&
                   api.sourceService.Valid() && api.source.Valid() && api.destination.Valid() &&
                   ValidTransferKind(api.kind) && validCompletion && validAlias && api.transfer != nullptr;
        }

        [[nodiscard]] constexpr bool ValidTransferRequest(const BackendTransferRequest& request,
                                                          const TransferEngineApiV7& api,
                                                          BackendServiceId service) noexcept {
            if (!ValidBackendAbi(request.abiVersion, request.structSize, sizeof(BackendTransferRequest)) ||
                request.engine != api.engine || request.kind != api.kind || request.destination != api.destination ||
                !request.source || request.source.location != api.source || request.regionCount == 0 ||
                request.regions == nullptr || request.source.service != api.sourceService ||
                (request.waitCount != 0 && request.waits == nullptr)) {
                return false;
            }
            for (const EventView& wait : request.Waits()) {
                if (!wait || wait.service != service) {
                    return false;
                }
            }
            if (request.kind == TransferKind::CoherentAlias) {
                const TransferRegion& region = request.Regions().front();
                return request.regionCount == 1 && !request.target && region.targetOffset == 0 &&
                       request.source.Covers(region.sourceOffset, region.sizeBytes) && request.waitCount == 0;
            }
            return request.target && request.target.location == api.destination && request.target.service == service &&
                   ValidTransferRegions(request.source, request.target, request.Regions());
        }

        [[nodiscard]] constexpr bool ImportedAliasSatisfies(const AllocationPayload& imported,
                                                            const BackendTransferRequest& request) noexcept {
            const TransferRegion& region = request.Regions().front();
            if (region.sourceOffset > UINT64_MAX - request.source.memoryOffset) {
                return false;
            }
            return imported.location == request.destination && imported.memory == request.source.memory &&
                   imported.memoryOffset == request.source.memoryOffset + region.sourceOffset &&
                   imported.sizeBytes >= region.sizeBytes && imported.state != nullptr && imported.destroy != nullptr;
        }

        [[nodiscard]] constexpr TransferError EventAdoptionError(EventError error) noexcept {
            return error == EventError::OutOfMemory
                       ? TransferError{TransferErrorCode::OutOfMemory, static_cast<uint64_t>(error)}
                       : TransferError{TransferErrorCode::InvalidResult, static_cast<uint64_t>(error)};
        }

    } // namespace Detail

    /** @brief Accepted asynchronous transfer retaining all resource and prerequisite leases until completion. */
    class TransferSubmission {
    public:
        TransferSubmission(const TransferSubmission&) = delete;
        TransferSubmission& operator=(const TransferSubmission&) = delete;
        TransferSubmission(TransferSubmission&&) noexcept = default;
        TransferSubmission& operator=(TransferSubmission&& other) noexcept {
            if (this != &other) {
                Drain();
                source_ = std::move(other.source_);
                destination_ = std::move(other.destination_);
                waits_ = std::move(other.waits_);
                completion_ = std::move(other.completion_);
            }
            return *this;
        }

        /** @brief Drain accepted transfer work before releasing any physical resource lease. */
        ~TransferSubmission() noexcept { Drain(); }

        /** @brief Observe the transfer completion without blocking. */
        [[nodiscard]] EventObservation Query() const noexcept { return completion_.Query(); }

        /** @brief Wait without a deadline until the completion event reaches a terminal state. */
        [[nodiscard]] EventObservation WaitUntilTerminal() const noexcept { return completion_.WaitUntilTerminal(); }

        /** @brief Wait for transfer completion or timeout. */
        [[nodiscard]] EventObservation Wait(uint64_t timeoutNanoseconds) const noexcept {
            return completion_.Wait(timeoutNanoseconds);
        }

        /** @brief Return a shared handle to the completion event. */
        [[nodiscard]] Event Completion() const noexcept { return completion_; }

        /** @brief Return the retained destination allocation for later publication or dispatch. */
        [[nodiscard]] Allocation Destination() const noexcept { return destination_; }

    private:
        friend class TransferEngine;

        TransferSubmission(Allocation source, Allocation destination, std::vector<Event> waits,
                           Event completion) noexcept
            : source_(std::move(source)),
              destination_(std::move(destination)),
              waits_(std::move(waits)),
              completion_(std::move(completion)) {}

        void Drain() const noexcept {
            if (!completion_) {
                return;
            }
            if (completion_.Query().status == EventStatus::Pending) {
                (void)completion_.WaitUntilTerminal();
            }
        }

        Allocation source_{};
        Allocation destination_{};
        std::vector<Event> waits_{};
        Event completion_{};
    };

    /** @brief Validated facade over one directed coherent-import or asynchronous-transfer callback table. */
    class TransferEngine final {
    public:
        /** @brief Validate and retain a copy of @p api's immutable callback table. */
        [[nodiscard]] static std::expected<TransferEngine, TransferError> Create(BackendService service,
                                                                                 TransferEngineApiV7 api) noexcept {
            if (!service || !Detail::ValidTransferApi(api) ||
                (api.sourceService == service.Id() && api.source == api.destination)) {
                return std::unexpected(TransferError{TransferErrorCode::InvalidApi, 0});
            }
            return TransferEngine{std::move(service), {}, api};
        }

        /** @brief Validate a transfer whose callback state is independent of both endpoint services. */
        [[nodiscard]] static std::expected<TransferEngine, TransferError>
        Create(BackendService service, BackendCallbackStateLease callbackState, TransferEngineApiV7 api) noexcept {
            if (!service || !callbackState || !Detail::ValidTransferApi(api) || api.state == nullptr ||
                (api.sourceService == service.Id() && api.source == api.destination)) {
                return std::unexpected(TransferError{TransferErrorCode::InvalidApi, 0});
            }
            return TransferEngine{std::move(service), std::move(callbackState), api};
        }

        /** @brief Return the stable engine identity used by residency routes. */
        [[nodiscard]] constexpr TransferEngineId Id() const noexcept { return api_.engine; }

        /** @brief Return the exact source memory location accepted by this engine. */
        [[nodiscard]] constexpr MemoryLocation Source() const noexcept { return api_.source; }

        /** @brief Return the backend service owning source allocations accepted by this engine. */
        [[nodiscard]] constexpr BackendServiceId SourceService() const noexcept { return api_.sourceService; }

        /** @brief Return the exact destination memory location produced by this engine. */
        [[nodiscard]] constexpr MemoryLocation Destination() const noexcept { return api_.destination; }

        /** @brief Return the backend service owning imports, copies, and completion events from this engine. */
        [[nodiscard]] BackendServiceId Service() const noexcept { return service_.Id(); }

        /** @brief Return the transfer mechanism implemented by this engine. */
        [[nodiscard]] constexpr TransferKind Kind() const noexcept { return api_.kind; }

        /** @brief Return the backend identity of asynchronous prerequisite and completion events. */
        [[nodiscard]] constexpr BackendKind CompletionBackend() const noexcept { return api_.completionBackend; }

        /** @brief Return the device identity of asynchronous prerequisite and completion events. */
        [[nodiscard]] constexpr DeviceId CompletionDevice() const noexcept { return api_.completionDevice; }

        /** @brief Import a source subrange as a new backend handle in the same coherence domain. */
        [[nodiscard]] std::expected<Allocation, TransferError> ImportAlias(Allocation source, uint64_t sourceOffset,
                                                                           uint64_t sizeBytes) const noexcept {
            const TransferRegion region{.sourceOffset = sourceOffset, .sizeBytes = sizeBytes};
            BackendTransferRequest request{.engine = api_.engine,
                                           .kind = api_.kind,
                                           .destination = api_.destination,
                                           .source = source.View(),
                                           .regions = &region,
                                           .regionCount = 1};
            if (api_.kind != TransferKind::CoherentAlias ||
                !Detail::ValidTransferRequest(request, api_, service_.Id())) {
                return std::unexpected(TransferError{TransferErrorCode::InvalidRequest, 0});
            }

            BackendTransferResult result{};
            api_.transfer(api_.state, &request, &result);
            if (!ValidBackendAbi(result.abiVersion, result.structSize, sizeof(BackendTransferResult)) ||
                !Detail::ValidTransferErrorCode(result.error.code)) {
                Detail::ReleaseTransferResult(result);
                return std::unexpected(TransferError{TransferErrorCode::InvalidResult, 0});
            }
            if (result.imported.state == nullptr) {
                Detail::ReleaseEventPayload(result.completion);
                return std::unexpected(result.error.code == TransferErrorCode::None
                                           ? TransferError{TransferErrorCode::TransferFailed, 0}
                                           : result.error);
            }
            if (result.error.code != TransferErrorCode::None || result.completion.state != nullptr ||
                !Detail::ImportedAliasSatisfies(result.imported, request)) {
                const uint64_t detail = result.error.detail;
                Detail::ReleaseTransferResult(result);
                return std::unexpected(TransferError{TransferErrorCode::InvalidResult, detail});
            }
            auto imported = Allocation::Adopt(service_, result.imported);
            if (!imported) {
                const TransferErrorCode code = imported.error() == AllocationError::OutOfMemory
                                                   ? TransferErrorCode::OutOfMemory
                                                   : TransferErrorCode::InvalidResult;
                return std::unexpected(TransferError{code, static_cast<uint64_t>(imported.error())});
            }
            return std::move(*imported);
        }

        /** @brief Submit one canonical scatter/gather transfer and retain all leases until completion. */
        [[nodiscard]] std::expected<TransferSubmission, TransferError>
        Submit(Allocation source, Allocation destination, std::span<const TransferRegion> regions,
               std::vector<Event> waits = {}) const noexcept {
            if (api_.kind == TransferKind::CoherentAlias || regions.size() > UINT32_MAX || waits.size() > UINT32_MAX) {
                return std::unexpected(TransferError{TransferErrorCode::InvalidRequest, 0});
            }
            try {
                std::vector<EventView> waitViews;
                waitViews.reserve(waits.size());
                for (const Event& wait : waits) {
                    if (!wait) {
                        return std::unexpected(TransferError{TransferErrorCode::InvalidRequest, 0});
                    }
                    waitViews.push_back(wait.View());
                }
                BackendTransferRequest request{.engine = api_.engine,
                                               .kind = api_.kind,
                                               .destination = api_.destination,
                                               .source = source.View(),
                                               .target = destination.View(),
                                               .regions = regions.data(),
                                               .regionCount = static_cast<uint32_t>(regions.size()),
                                               .waits = waitViews.data(),
                                               .waitCount = static_cast<uint32_t>(waitViews.size())};
                if (!Detail::ValidTransferRequest(request, api_, service_.Id())) {
                    return std::unexpected(TransferError{TransferErrorCode::InvalidRequest, 0});
                }

                BackendTransferResult result{};
                api_.transfer(api_.state, &request, &result);
                if (!ValidBackendAbi(result.abiVersion, result.structSize, sizeof(BackendTransferResult)) ||
                    !Detail::ValidTransferErrorCode(result.error.code)) {
                    Detail::ReleaseTransferResult(result);
                    return std::unexpected(TransferError{TransferErrorCode::InvalidResult, 0});
                }
                if (result.completion.state == nullptr) {
                    const TransferError error = result.error.code == TransferErrorCode::None
                                                    ? TransferError{TransferErrorCode::TransferFailed, 0}
                                                    : result.error;
                    Detail::ReleaseTransferResult(result);
                    return std::unexpected(error);
                }
                if (result.error.code != TransferErrorCode::None || result.imported.state != nullptr) {
                    const uint64_t detail = result.error.detail;
                    Detail::ReleaseTransferResult(result);
                    return std::unexpected(TransferError{TransferErrorCode::InvalidResult, detail});
                }
                auto completion =
                    Event::Adopt(service_, result.completion, api_.completionBackend, api_.completionDevice);
                if (!completion) {
                    return std::unexpected(Detail::EventAdoptionError(completion.error()));
                }
                return TransferSubmission{std::move(source), std::move(destination), std::move(waits),
                                          std::move(*completion)};
            } catch (const std::bad_alloc&) {
                return std::unexpected(TransferError{TransferErrorCode::OutOfMemory, 0});
            }
        }

    private:
        TransferEngine(BackendService service, BackendCallbackStateLease callbackState,
                       TransferEngineApiV7 api) noexcept
            : service_(std::move(service)), callbackState_(std::move(callbackState)), api_(api) {}

        BackendService service_{};
        BackendCallbackStateLease callbackState_{};
        TransferEngineApiV7 api_{};
    };

    static_assert(std::is_trivially_copyable_v<TransferRegion>);
    static_assert(std::is_trivially_copyable_v<BackendTransferRequest>);
    static_assert(std::is_trivially_copyable_v<BackendTransferResult>);
    static_assert(std::is_trivially_copyable_v<TransferEngineApiV7>);
    static_assert(std::is_standard_layout_v<TransferRegion>);
    static_assert(std::is_standard_layout_v<BackendTransferRequest>);
    static_assert(std::is_standard_layout_v<BackendTransferResult>);
    static_assert(std::is_standard_layout_v<TransferEngineApiV7>);

} // namespace Sora::Compute

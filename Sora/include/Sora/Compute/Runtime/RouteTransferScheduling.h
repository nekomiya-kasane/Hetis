/**
 * @file RouteTransferScheduling.h
 * @brief Deterministic service-scoped scheduling of exact residency-route hops.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Runtime/GraphResidency.h>

#include <cstdint>
#include <expected>
#include <span>
#include <type_traits>
#include <vector>

namespace Sora::Compute {

    /** @brief One independent route batch whose dispatch tail determines transfer priority. */
    struct RouteTransferBatch {
        uint32_t operation = UINT32_MAX;
        uint64_t dispatchNanoseconds = 0;
        std::span<const GraphResidencyRoute> routes{};
        std::span<const uint64_t> sourceReadyNanoseconds{};
    };

    /** @brief One pre-existing half-open reservation on an exact service-scoped transfer engine. */
    struct RouteTransferReservation {
        BackendServiceId service{};
        TransferEngineId engine{};
        uint64_t beginNanoseconds = 0;
        uint64_t endNanoseconds = 0;

        /** @brief Return whether the resource identity and half-open interval are valid. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return service.Valid() && engine.Valid() && beginNanoseconds < endNanoseconds;
        }

        friend constexpr bool operator==(RouteTransferReservation, RouteTransferReservation) noexcept = default;
    };

    /** @brief One asynchronous raw route hop placed on a service-scoped transfer-engine timeline. */
    struct ScheduledRouteTransfer {
        uint32_t operation = UINT32_MAX;
        uint32_t route = UINT32_MAX;
        uint32_t hop = UINT32_MAX;
        BackendServiceId service{};
        TransferEngineId engine{};
        uint64_t beginNanoseconds = 0;
        uint64_t endNanoseconds = 0;

        friend constexpr bool operator==(ScheduledRouteTransfer, ScheduledRouteTransfer) noexcept = default;
    };

    /** @brief Exact route readiness and asynchronous-hop order for one bounded planning problem. */
    struct RouteTransferSchedule {
        std::vector<uint64_t> readyNanoseconds{};
        std::vector<std::vector<uint64_t>> routeReadyNanoseconds{};
        std::vector<ScheduledRouteTransfer> transfers{};
        uint64_t makespanNanoseconds = 0;
    };

    /** @brief Stable rejection while validating or scheduling exact route batches. */
    enum class RouteTransferSchedulingError : uint8_t {
        InvalidRequest,
        InvalidRoute,
        CapacityExceeded,
        OutOfMemory,
    };

    /**
     * @brief Schedule route batches with per-route precedence and service-scoped transfer-engine exclusion.
     * @details Batches with longer dispatch tails receive deterministic priority. Optional route source-ready times
     * and committed half-open reservations place the problem on an existing absolute timeline. Coherent aliases
     * consume their calibrated latency without occupying a transfer engine. No transfer duration is inferred from
     * aggregate bytes.
     */
    [[nodiscard]] std::expected<RouteTransferSchedule, RouteTransferSchedulingError>
    ScheduleRouteTransfers(std::span<const RouteTransferBatch> batches,
                           std::span<const RouteTransferReservation> committed = {}) noexcept;

    static_assert(std::is_trivially_copyable_v<RouteTransferBatch>);
    static_assert(std::is_trivially_copyable_v<RouteTransferReservation>);
    static_assert(std::is_trivially_copyable_v<ScheduledRouteTransfer>);

} // namespace Sora::Compute

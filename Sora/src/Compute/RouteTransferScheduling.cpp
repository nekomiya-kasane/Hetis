/**
 * @file RouteTransferScheduling.cpp
 * @brief Deterministic service-scoped scheduling of exact residency-route hops.
 * @ingroup Compute
 */

#include <Sora/Compute/Runtime/RouteTransferScheduling.h>

#include <Sora/Compute/Cost.h>

#include "TransferTimeline.h"

#include <algorithm>
#include <limits>
#include <new>
#include <numeric>
#include <ranges>
#include <tuple>
#include <vector>

namespace Sora::Compute {
    namespace {

        struct TransferProvenance {
            uint32_t operation = UINT32_MAX;
            uint32_t route = UINT32_MAX;
            uint32_t hop = UINT32_MAX;

            friend constexpr auto operator<=>(TransferProvenance, TransferProvenance) noexcept = default;
        };

        using TransferCalendar = Detail::TransferTimelineCalendar<TransferProvenance>;

        [[nodiscard]] bool AppendCommittedReservations(std::span<const RouteTransferReservation> committed,
                                                       std::vector<TransferCalendar>& calendars) {
            for (const RouteTransferReservation reservation : committed) {
                if (!reservation.Valid()) {
                    return false;
                }
                const Detail::TransferTimelineKey key{reservation.service, reservation.engine};
                auto calendar = std::ranges::find(calendars, key, &TransferCalendar::key);
                if (calendar == calendars.end()) {
                    calendars.push_back({.key = key});
                    calendar = std::prev(calendars.end());
                }
                calendar->intervals.push_back({.beginNanoseconds = reservation.beginNanoseconds,
                                               .endNanoseconds = reservation.endNanoseconds,
                                               .provenance = {UINT32_MAX, UINT32_MAX, UINT32_MAX}});
            }
            for (TransferCalendar& calendar : calendars) {
                std::ranges::sort(calendar.intervals, Detail::TransferTimelineIntervalLess<TransferProvenance>);
                for (size_t index = 1; index < calendar.intervals.size(); ++index) {
                    if (calendar.intervals[index - 1].endNanoseconds > calendar.intervals[index].beginNanoseconds) {
                        return false;
                    }
                }
            }
            return true;
        }

    } // namespace

    std::expected<RouteTransferSchedule, RouteTransferSchedulingError>
    ScheduleRouteTransfers(std::span<const RouteTransferBatch> batches,
                           std::span<const RouteTransferReservation> committed) noexcept {
        if (batches.size() > UINT32_MAX) {
            return std::unexpected(RouteTransferSchedulingError::CapacityExceeded);
        }
        try {
            RouteTransferSchedule result;
            result.readyNanoseconds.resize(batches.size());
            result.routeReadyNanoseconds.resize(batches.size());
            std::vector<uint32_t> order(batches.size());
            std::iota(order.begin(), order.end(), uint32_t{0});
            std::ranges::sort(order, [&](uint32_t left, uint32_t right) {
                if (batches[left].dispatchNanoseconds != batches[right].dispatchNanoseconds) {
                    return batches[left].dispatchNanoseconds > batches[right].dispatchNanoseconds;
                }
                return batches[left].operation < batches[right].operation;
            });
            std::vector<uint8_t> seen(batches.size());
            std::vector<TransferCalendar> calendars;
            if (!AppendCommittedReservations(committed, calendars)) {
                return std::unexpected(RouteTransferSchedulingError::InvalidRequest);
            }

            for (const uint32_t orderedIndex : order) {
                const RouteTransferBatch& batch = batches[orderedIndex];
                if (batch.operation >= batches.size() || seen[batch.operation] != 0 ||
                    batch.routes.size() > UINT32_MAX ||
                    (!batch.sourceReadyNanoseconds.empty() &&
                     batch.sourceReadyNanoseconds.size() != batch.routes.size())) {
                    return std::unexpected(RouteTransferSchedulingError::InvalidRequest);
                }
                seen[batch.operation] = 1;
                uint64_t batchReady = 0;
                result.routeReadyNanoseconds[batch.operation].reserve(batch.routes.size());
                for (uint32_t routeIndex = 0; routeIndex < batch.routes.size(); ++routeIndex) {
                    const GraphResidencyRoute& route = batch.routes[routeIndex];
                    if (!route.Valid() || route.hops.size() > UINT32_MAX) {
                        return std::unexpected(RouteTransferSchedulingError::InvalidRoute);
                    }
                    uint64_t routeReady =
                        batch.sourceReadyNanoseconds.empty() ? 0 : batch.sourceReadyNanoseconds[routeIndex];
                    uint64_t estimated = 0;
                    for (uint32_t hopIndex = 0; hopIndex < route.hops.size(); ++hopIndex) {
                        const TransferLink& hop = route.hops[hopIndex];
                        const uint64_t duration = Detail::TransferCost(hop, route.region.Size());
                        estimated = Detail::SaturatingAdd(estimated, duration);
                        if (estimated == std::numeric_limits<uint64_t>::max()) {
                            return std::unexpected(RouteTransferSchedulingError::CapacityExceeded);
                        }
                        if (hop.kind == TransferKind::CoherentAlias) {
                            routeReady = Detail::SaturatingAdd(routeReady, duration);
                            continue;
                        }
                        const Detail::TransferTimelineKey key{hop.service, hop.engine};
                        const auto begin = Detail::EarliestTransferTimelineGap<TransferProvenance>(
                            calendars, {}, key, routeReady, duration);
                        if (!begin) {
                            return std::unexpected(RouteTransferSchedulingError::CapacityExceeded);
                        }
                        const uint64_t end = Detail::SaturatingAdd(*begin, duration);
                        if (end == std::numeric_limits<uint64_t>::max()) {
                            return std::unexpected(RouteTransferSchedulingError::CapacityExceeded);
                        }
                        Detail::InsertTransferTimelineInterval(calendars, key,
                                                               {.beginNanoseconds = *begin,
                                                                .endNanoseconds = end,
                                                                .provenance = {batch.operation, routeIndex, hopIndex}});
                        result.transfers.push_back({.operation = batch.operation,
                                                    .route = routeIndex,
                                                    .hop = hopIndex,
                                                    .service = hop.service,
                                                    .engine = hop.engine,
                                                    .beginNanoseconds = *begin,
                                                    .endNanoseconds = end});
                        routeReady = end;
                    }
                    if (estimated != route.estimatedNanoseconds) {
                        return std::unexpected(RouteTransferSchedulingError::InvalidRoute);
                    }
                    batchReady = std::max(batchReady, routeReady);
                    result.routeReadyNanoseconds[batch.operation].push_back(routeReady);
                }
                result.readyNanoseconds[batch.operation] = batchReady;
                result.makespanNanoseconds = std::max(result.makespanNanoseconds, batchReady);
            }
            std::ranges::sort(result.transfers,
                              [](const ScheduledRouteTransfer& left, const ScheduledRouteTransfer& right) {
                                  return std::tuple{left.beginNanoseconds, left.service, left.engine,
                                                    left.operation,        left.route,   left.hop} <
                                         std::tuple{right.beginNanoseconds, right.service, right.engine,
                                                    right.operation,        right.route,   right.hop};
                              });
            return result;
        } catch (const std::bad_alloc&) {
            return std::unexpected(RouteTransferSchedulingError::OutOfMemory);
        }
    }

} // namespace Sora::Compute

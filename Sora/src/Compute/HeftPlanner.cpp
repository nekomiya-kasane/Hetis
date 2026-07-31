/**
 * @file HeftPlanner.cpp
 * @brief Dynamic exact-calendar HEFT scheduling, bounded local search, and physical lowering.
 * @ingroup Compute
 */

#include <Sora/Compute/Runtime/HeftPlanner.h>

#include <Sora/Compute/Runtime/CalibratedTransferTopology.h>

#include "AutomaticDispatchPlanningInternal.h"
#include "HeftPlanningInternal.h"
#include "TransferTimeline.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <limits>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace Sora::Compute {
    namespace {
        using HeftDetail::Add;
        using HeftDetail::BuildUpwardRanks;
        using HeftDetail::EnumerationError;
        using HeftDetail::Error;
        using HeftDetail::ServiceDuration;

        struct GraphTopology {
            std::vector<std::vector<GraphNodeId>> predecessors{};
            std::vector<std::vector<GraphNodeId>> successors{};
        };

        struct QueueKey {
            BackendServiceId service{};
            BackendKind backend = BackendKind::Cpu;
            DeviceId device{};
            QueueId queue{};

            friend constexpr bool operator==(QueueKey, QueueKey) noexcept = default;
            friend constexpr auto operator<=>(QueueKey, QueueKey) noexcept = default;
        };

        struct QueueInterval {
            uint64_t beginNanoseconds = 0;
            uint64_t endNanoseconds = 0;
            GraphNodeId node{};
            uint32_t dispatch = 0;
        };

        struct QueueCalendar {
            QueueKey key{};
            std::vector<QueueInterval> intervals{};
        };

        using TransferInterval = Detail::TransferTimelineInterval<DispatchTransferRef>;
        using TransferCalendar = Detail::TransferTimelineCalendar<DispatchTransferRef>;

        struct ReplicaReady {
            GraphLocalResidencySource source{};
            uint64_t nanoseconds = 0;
        };

        struct PlacementDecision {
            PlacementCandidate placement{};
            std::optional<PhysicalMemoryDescriptor> destination{};
        };

        struct ScheduledNode {
            bool scheduled = false;
            uint64_t startNanoseconds = 0;
            uint64_t finishNanoseconds = 0;
            DispatchGraphSelection selection{};
        };

        struct ScheduleResult {
            std::vector<ScheduledNode> nodes{};
            std::vector<QueueCalendar> queues{};
            std::vector<TransferCalendar> transfers{};
            std::vector<RouteTransferReservation> transferReservations{};
            std::vector<std::vector<PlacementDecision>> placementAlternatives{};
            uint64_t makespanNanoseconds = 0;
        };

        struct WholeCandidateChoice {
            uint32_t placement = UINT32_MAX;
        };

        struct PartitionCandidateChoice {
            PartitionedDispatchExecution execution{};
        };

        struct CandidateChoice {
            bool valid = false;
            uint64_t startNanoseconds = 0;
            uint64_t finishNanoseconds = 0;
            std::variant<WholeCandidateChoice, PartitionCandidateChoice> execution{};
            std::vector<std::vector<uint64_t>> inputReadyNanoseconds{};
            std::vector<uint64_t> shardFinishNanoseconds{};
            std::vector<QueueCalendar> queues{};
            std::vector<TransferCalendar> transfers{};
        };

        [[nodiscard]] constexpr HeftPlanningError CommonError(const DispatchGraphPlanningError& error) noexcept {
            HeftPlanningErrorCode code = HeftPlanningErrorCode::ResidencyLoweringFailed;
            switch (error.code) {
                case DispatchGraphPlanningErrorCode::InvalidGraph:
                    code = HeftPlanningErrorCode::InvalidGraph;
                    break;
                case DispatchGraphPlanningErrorCode::UnsupportedOperation:
                    code = HeftPlanningErrorCode::UnsupportedOperation;
                    break;
                case DispatchGraphPlanningErrorCode::UnsupportedBinding:
                    code = HeftPlanningErrorCode::UnsupportedBinding;
                    break;
                case DispatchGraphPlanningErrorCode::ProgramUnavailable:
                    code = HeftPlanningErrorCode::ProgramUnavailable;
                    break;
                case DispatchGraphPlanningErrorCode::CapacityExceeded:
                    code = HeftPlanningErrorCode::CapacityExceeded;
                    break;
                case DispatchGraphPlanningErrorCode::OutOfMemory:
                    code = HeftPlanningErrorCode::OutOfMemory;
                    break;
                case DispatchGraphPlanningErrorCode::InvalidSelection:
                case DispatchGraphPlanningErrorCode::ResidencyLoweringFailed:
                    break;
            }
            return Error(code, error.node, error.argument, error.cause);
        }

        [[nodiscard]] std::expected<GraphTopology, HeftPlanningError> BuildTopology(GraphView graph) {
            try {
                GraphTopology result{.predecessors = std::vector<std::vector<GraphNodeId>>(graph.nodes.size()),
                                     .successors = std::vector<std::vector<GraphNodeId>>(graph.nodes.size())};
                for (const GraphEdge edge : graph.edges) {
                    result.predecessors[edge.successor.index].push_back(edge.predecessor);
                    result.successors[edge.predecessor.index].push_back(edge.successor);
                }
                return result;
            } catch (const std::bad_alloc&) {
                return std::unexpected(Error(HeftPlanningErrorCode::OutOfMemory));
            }
        }

        [[nodiscard]] constexpr QueueKey QueueFor(const PlacementCandidate& placement) noexcept {
            return {.service = placement.service,
                    .backend = placement.device.backend,
                    .device = placement.device.id,
                    .queue = placement.queue};
        }

        [[nodiscard]] const QueueCalendar* FindQueue(std::span<const QueueCalendar> queues, QueueKey key) noexcept {
            const auto found = std::ranges::find(queues, key, &QueueCalendar::key);
            return found == queues.end() ? nullptr : std::addressof(*found);
        }

        [[nodiscard]] constexpr bool QueueIntervalLess(const QueueInterval& left, const QueueInterval& right) noexcept {
            return std::tuple{left.beginNanoseconds, left.node, left.dispatch} <
                   std::tuple{right.beginNanoseconds, right.node, right.dispatch};
        }

        [[nodiscard]] std::expected<uint64_t, HeftPlanningError> EarliestGap(const QueueCalendar* committed,
                                                                             const QueueCalendar* speculative,
                                                                             uint64_t ready, uint64_t duration,
                                                                             GraphNodeId node) noexcept {
            uint64_t start = ready;
            size_t committedIndex = 0;
            size_t speculativeIndex = 0;
            while ((committed != nullptr && committedIndex < committed->intervals.size()) ||
                   (speculative != nullptr && speculativeIndex < speculative->intervals.size())) {
                const QueueInterval* interval = nullptr;
                if (committed == nullptr || committedIndex == committed->intervals.size()) {
                    interval = std::addressof(speculative->intervals[speculativeIndex++]);
                } else if (speculative == nullptr || speculativeIndex == speculative->intervals.size()) {
                    interval = std::addressof(committed->intervals[committedIndex++]);
                } else if (QueueIntervalLess(committed->intervals[committedIndex],
                                             speculative->intervals[speculativeIndex])) {
                    interval = std::addressof(committed->intervals[committedIndex++]);
                } else {
                    interval = std::addressof(speculative->intervals[speculativeIndex++]);
                }
                auto finish = Add(start, duration, node, HeftPlanningErrorCode::CapacityExceeded);
                if (!finish) {
                    return std::unexpected(finish.error());
                }
                if (*finish <= interval->beginNanoseconds) {
                    return start;
                }
                start = std::max(start, interval->endNanoseconds);
            }
            if (!Add(start, duration, node, HeftPlanningErrorCode::CapacityExceeded)) {
                return std::unexpected(Error(HeftPlanningErrorCode::CapacityExceeded, node));
            }
            return start;
        }

        [[nodiscard]] bool SameSource(const GraphLocalResidencySource& left,
                                      const GraphLocalResidencySource& right) noexcept {
            return left.buffer == right.buffer && left.coverage == right.coverage &&
                   left.byteFootprint == right.byteFootprint && left.version == right.version &&
                   left.producer == right.producer && left.replica == right.replica;
        }

        [[nodiscard]] const ReplicaReady* FindReplica(std::span<const ReplicaReady> replicas,
                                                      const GraphLocalResidencySource& source) noexcept {
            const auto found = std::ranges::find_if(
                replicas, [&](const ReplicaReady& value) { return SameSource(value.source, source); });
            return found == replicas.end() ? nullptr : std::addressof(*found);
        }

        struct CandidateTransfers {
            uint64_t readyNanoseconds = 0;
            std::vector<uint64_t> inputReadyNanoseconds{};
            std::vector<TransferCalendar> calendars{};
        };

        [[nodiscard]] std::expected<uint64_t, HeftPlanningError> SourceReadyTime(const GraphResidencyRoute& route,
                                                                                 std::span<const ReplicaReady> replicas,
                                                                                 GraphNodeId node) noexcept {
            const auto* source = std::get_if<GraphLocalResidencySource>(&route.source);
            if (source == nullptr) {
                return 0;
            }
            const ReplicaReady* ready = FindReplica(replicas, *source);
            if (ready == nullptr) {
                return std::unexpected(
                    Error(HeftPlanningErrorCode::SchedulingFailed, node, {}, source->replica.materializer.index));
            }
            return ready->nanoseconds;
        }

        [[nodiscard]] std::expected<CandidateTransfers, HeftPlanningError>
        ScheduleTransfers(const DispatchResidencyProjection& projection, std::span<const TransferCalendar> committed,
                          std::span<const ReplicaReady> replicas, GraphNodeId node, uint32_t dispatch = 0) {
            CandidateTransfers result;
            if (projection.inputs.size() > UINT32_MAX) {
                return std::unexpected(Error(HeftPlanningErrorCode::CapacityExceeded, node));
            }
            result.inputReadyNanoseconds.reserve(projection.inputs.size());
            for (uint32_t routeIndex = 0; routeIndex < projection.inputs.size(); ++routeIndex) {
                const GraphResidencyRoute& route = projection.inputs[routeIndex];
                if (!route.Valid() || route.hops.size() > UINT32_MAX) {
                    return std::unexpected(Error(HeftPlanningErrorCode::SchedulingFailed, node));
                }
                auto sourceReady = SourceReadyTime(route, replicas, node);
                if (!sourceReady) {
                    return std::unexpected(sourceReady.error());
                }
                uint64_t routeReady = *sourceReady;
                uint64_t estimated = 0;
                for (uint32_t hopIndex = 0; hopIndex < route.hops.size(); ++hopIndex) {
                    const TransferLink& hop = route.hops[hopIndex];
                    const uint64_t duration = Detail::TransferCost(hop, route.region.Size());
                    auto total = Add(estimated, duration, node, HeftPlanningErrorCode::CapacityExceeded);
                    if (!total) {
                        return std::unexpected(total.error());
                    }
                    estimated = *total;
                    if (hop.kind == TransferKind::CoherentAlias) {
                        auto aliasReady = Add(routeReady, duration, node, HeftPlanningErrorCode::CapacityExceeded);
                        if (!aliasReady) {
                            return std::unexpected(aliasReady.error());
                        }
                        routeReady = *aliasReady;
                        continue;
                    }

                    const Detail::TransferTimelineKey key{hop.service, hop.engine};
                    const auto start = Detail::EarliestTransferTimelineGap<DispatchTransferRef>(
                        committed, result.calendars, key, routeReady, duration);
                    if (!start) {
                        return std::unexpected(Error(HeftPlanningErrorCode::CapacityExceeded, node));
                    }
                    auto finish = Add(*start, duration, node, HeftPlanningErrorCode::CapacityExceeded);
                    if (!finish) {
                        return std::unexpected(finish.error());
                    }
                    Detail::InsertTransferTimelineInterval(result.calendars, key,
                                                           {.beginNanoseconds = *start,
                                                            .endNanoseconds = *finish,
                                                            .provenance = {.consumer = node,
                                                                           .buffer = route.buffer,
                                                                           .dispatch = dispatch,
                                                                           .route = routeIndex,
                                                                           .hop = hopIndex}});
                    routeReady = *finish;
                }
                if (estimated != route.estimatedNanoseconds) {
                    return std::unexpected(
                        Error(HeftPlanningErrorCode::SchedulingFailed, node, {}, route.estimatedNanoseconds));
                }
                result.readyNanoseconds = std::max(result.readyNanoseconds, routeReady);
                result.inputReadyNanoseconds.push_back(routeReady);
            }
            return result;
        }

        [[nodiscard]] bool BetterChoice(const CandidateChoice& candidate, const PlacementCandidate& placement,
                                        const CandidateChoice& incumbent,
                                        const DispatchCandidateSet& candidates) noexcept {
            if (!incumbent.valid || candidate.finishNanoseconds != incumbent.finishNanoseconds) {
                return !incumbent.valid || candidate.finishNanoseconds < incumbent.finishNanoseconds;
            }
            if (candidate.startNanoseconds != incumbent.startNanoseconds) {
                return candidate.startNanoseconds < incumbent.startNanoseconds;
            }
            const auto* whole = std::get_if<WholeCandidateChoice>(&incumbent.execution);
            return whole == nullptr ||
                   StableDispatchPlacementLess(placement, candidates.placements[whole->placement].placement);
        }

        [[nodiscard]] PlacementDecision DecisionFor(const DispatchPlacementCandidate& candidate,
                                                    const DispatchResidencyProjection& projection) {
            return {.placement = candidate.placement, .destination = projection.destination};
        }

        [[nodiscard]] std::optional<PlacementDecision> DecisionFor(const DispatchGraphSelection& selection) {
            const auto* shard = std::get_if<DispatchGraphShardSelection>(&selection.execution);
            if (shard == nullptr) {
                return std::nullopt;
            }
            return PlacementDecision{.placement = shard->placement, .destination = shard->residency.destination};
        }

        template<typename Function>
        void ForEachShard(const DispatchGraphSelection& selection, Function&& function) {
            if (const auto* whole = std::get_if<DispatchGraphShardSelection>(&selection.execution)) {
                function(uint32_t{0}, *whole);
                return;
            }
            const auto& partition = std::get<DispatchGraphPartitionSelection>(selection.execution);
            for (uint32_t index = 0; index < partition.shards.size(); ++index) {
                function(index, partition.shards[index]);
            }
        }

        [[nodiscard]] bool SameDecision(const PlacementDecision& left, const PlacementDecision& right) noexcept {
            return !StableDispatchPlacementLess(left.placement, right.placement) &&
                   !StableDispatchPlacementLess(right.placement, left.placement) &&
                   left.destination == right.destination;
        }

        void RecordAlternative(std::vector<PlacementDecision>& alternatives, PlacementDecision decision) {
            if (std::ranges::find_if(alternatives, [&](const PlacementDecision& value) {
                    return SameDecision(value, decision);
                }) == alternatives.end()) {
                alternatives.push_back(std::move(decision));
            }
        }

        void RecordReplica(std::vector<ReplicaReady>& replicas, GraphLocalResidencySource source,
                           uint64_t readyNanoseconds) {
            const auto found = std::ranges::find_if(
                replicas, [&](const ReplicaReady& value) { return SameSource(value.source, source); });
            if (found == replicas.end()) {
                replicas.push_back({std::move(source), readyNanoseconds});
            } else {
                found->nanoseconds = std::min(found->nanoseconds, readyNanoseconds);
            }
        }

        [[nodiscard]] std::expected<void, HeftPlanningError>
        RecordShardReplicas(GraphNodeId node, const DispatchResidencyProjection& projection,
                            const GraphResidencyState& residency, std::span<const uint64_t> inputReadyNanoseconds,
                            uint64_t finishNanoseconds, std::vector<ReplicaReady>& replicas) {
            if (inputReadyNanoseconds.size() != projection.inputs.size() || !projection.destination) {
                return std::unexpected(Error(HeftPlanningErrorCode::SchedulingFailed, node));
            }
            for (size_t index = 0; index < projection.inputs.size(); ++index) {
                const GraphResidencyRoute& route = projection.inputs[index];
                if (route.AlreadyResident()) {
                    continue;
                }
                const auto* source = std::get_if<GraphLocalResidencySource>(&route.source);
                RecordReplica(replicas,
                              {.buffer = route.buffer,
                               .coverage = route.region,
                               .byteFootprint = route.byteFootprint,
                               .version = route.version,
                               .producer = source != nullptr ? source->producer : std::nullopt,
                               .replica = {.service = route.destinationService,
                                           .location = route.destination,
                                           .materializer = node}},
                              inputReadyNanoseconds[index]);
            }
            for (const DispatchMemoryBinding& binding : projection.bindings) {
                if (binding.access != AccessMode::Write) {
                    continue;
                }
                const auto baseVersion = residency.BaseVersion(binding.buffer.Describe().id);
                if (!baseVersion || *baseVersion == UINT64_MAX) {
                    return std::unexpected(Error(HeftPlanningErrorCode::SchedulingFailed, node));
                }
                RecordReplica(replicas,
                              {.buffer = binding.buffer.Describe().id,
                               .coverage = binding.region,
                               .byteFootprint = binding.byteFootprint,
                               .version = *baseVersion + 1,
                               .producer = node,
                               .replica = {.service = projection.service,
                                           .location = projection.destination->location,
                                           .materializer = node}},
                              finishNanoseconds);
            }
            return {};
        }

        [[nodiscard]] std::expected<void, HeftPlanningError>
        RecordSelectionReplicas(const DispatchGraphSelection& selection, const GraphResidencyState& residency,
                                std::span<const std::vector<uint64_t>> inputReadyNanoseconds,
                                std::span<const uint64_t> shardFinishNanoseconds, std::vector<ReplicaReady>& replicas) {
            try {
                const size_t shardCount =
                    std::holds_alternative<DispatchGraphShardSelection>(selection.execution)
                        ? 1
                        : std::get<DispatchGraphPartitionSelection>(selection.execution).shards.size();
                if (inputReadyNanoseconds.size() != shardCount || shardFinishNanoseconds.size() != shardCount) {
                    return std::unexpected(Error(HeftPlanningErrorCode::SchedulingFailed, selection.node));
                }
                std::expected<void, HeftPlanningError> result;
                ForEachShard(selection, [&](uint32_t index, const DispatchGraphShardSelection& shard) {
                    if (result) {
                        result =
                            RecordShardReplicas(selection.node, shard.residency, residency,
                                                inputReadyNanoseconds[index], shardFinishNanoseconds[index], replicas);
                    }
                });
                return result;
            } catch (const std::bad_alloc&) {
                return std::unexpected(Error(HeftPlanningErrorCode::OutOfMemory, selection.node));
            }
        }

        void InsertQueueInterval(std::vector<QueueCalendar>& queues, QueueKey key, QueueInterval interval) {
            auto queue = std::ranges::find(queues, key, &QueueCalendar::key);
            if (queue == queues.end()) {
                queues.push_back({.key = key});
                queue = std::prev(queues.end());
            }
            const auto position = std::ranges::lower_bound(queue->intervals, interval, QueueIntervalLess);
            queue->intervals.insert(position, interval);
        }

        void CommitCandidateCalendars(const CandidateChoice& choice, ScheduleResult& result) {
            for (const TransferCalendar& calendar : choice.transfers) {
                for (const TransferInterval interval : calendar.intervals) {
                    Detail::InsertTransferTimelineInterval(result.transfers, calendar.key, interval);
                    result.transferReservations.push_back({.service = calendar.key.service,
                                                           .engine = calendar.key.engine,
                                                           .beginNanoseconds = interval.beginNanoseconds,
                                                           .endNanoseconds = interval.endNanoseconds});
                }
            }
            for (const QueueCalendar& calendar : choice.queues) {
                for (const QueueInterval interval : calendar.intervals) {
                    InsertQueueInterval(result.queues, calendar.key, interval);
                }
            }
        }

        struct HeftPartitionScheduler {
            GraphNodeId node{};
            uint64_t predecessorReadyNanoseconds = 0;
            std::span<const QueueCalendar> committedQueues{};
            std::span<const RouteTransferReservation> committedTransfers{};
            std::span<const ReplicaReady> replicas{};
            std::optional<HeftPlanningError> failure{};

            [[nodiscard]] std::expected<bool, AutomaticDispatchPlanningError>
            operator()(PartitionedDispatchExecution& candidate) noexcept {
                try {
                    if (candidate.shards.size() < 2 || candidate.shards.size() > UINT32_MAX) {
                        return false;
                    }
                    std::vector<uint64_t> durations;
                    durations.reserve(candidate.shards.size());
                    size_t routeCount = 0;
                    for (const PartitionShardExecution& shard : candidate.shards) {
                        if (shard.residency.inputs.size() > std::numeric_limits<size_t>::max() - routeCount) {
                            failure = Error(HeftPlanningErrorCode::CapacityExceeded, node);
                            return std::unexpected(AutomaticDispatchPlanningError::InvalidRequest);
                        }
                        routeCount += shard.residency.inputs.size();
                    }
                    std::vector<uint64_t> sourceReady;
                    sourceReady.reserve(routeCount);
                    for (uint32_t index = 0; index < candidate.shards.size(); ++index) {
                        PartitionShardExecution& shard = candidate.shards[index];
                        auto duration = ServiceDuration(shard.execution.cost, node);
                        if (!duration) {
                            failure = duration.error();
                            return std::unexpected(AutomaticDispatchPlanningError::InvalidRequest);
                        }
                        durations.push_back(*duration);
                        for (const GraphResidencyRoute& route : shard.residency.inputs) {
                            auto ready = SourceReadyTime(route, replicas, node);
                            if (!ready) {
                                failure = ready.error();
                                return std::unexpected(AutomaticDispatchPlanningError::InvalidRequest);
                            }
                            sourceReady.push_back(*ready);
                        }
                    }

                    std::vector<RouteTransferBatch> batches;
                    batches.reserve(candidate.shards.size());
                    size_t sourceReadyOffset = 0;
                    for (uint32_t index = 0; index < candidate.shards.size(); ++index) {
                        const PartitionShardExecution& shard = candidate.shards[index];
                        const size_t sourceReadyCount = shard.residency.inputs.size();
                        batches.push_back({.operation = index,
                                           .dispatchNanoseconds = durations[index],
                                           .routes = shard.residency.inputs,
                                           .sourceReadyNanoseconds =
                                               std::span{sourceReady}.subspan(sourceReadyOffset, sourceReadyCount)});
                        sourceReadyOffset += sourceReadyCount;
                    }
                    auto transferSchedule = ScheduleRouteTransfers(batches, committedTransfers);
                    if (!transferSchedule) {
                        if (transferSchedule.error() == RouteTransferSchedulingError::OutOfMemory) {
                            failure = Error(HeftPlanningErrorCode::OutOfMemory, node);
                            return std::unexpected(AutomaticDispatchPlanningError::OutOfMemory);
                        }
                        failure = Error(transferSchedule.error() == RouteTransferSchedulingError::CapacityExceeded
                                            ? HeftPlanningErrorCode::CapacityExceeded
                                            : HeftPlanningErrorCode::SchedulingFailed,
                                        node, {}, static_cast<uint64_t>(transferSchedule.error()));
                        return std::unexpected(AutomaticDispatchPlanningError::InvalidRequest);
                    }

                    std::vector<QueueCalendar> speculativeQueues;
                    candidate.transfers = std::move(transferSchedule->transfers);
                    candidate.transferReadyNanoseconds = transferSchedule->makespanNanoseconds;
                    candidate.parallelNanoseconds = 0;
                    for (uint32_t index = 0; index < candidate.shards.size(); ++index) {
                        PartitionShardExecution& shard = candidate.shards[index];
                        shard.routeReadyNanoseconds = std::move(transferSchedule->routeReadyNanoseconds[index]);
                        shard.inputReadyNanoseconds = transferSchedule->readyNanoseconds[index];
                        const uint64_t ready = std::max({predecessorReadyNanoseconds, shard.inputReadyNanoseconds,
                                                         shard.execution.cost.queueNanoseconds});
                        const QueueKey queue = {.service = shard.execution.service,
                                                .backend = shard.execution.backend,
                                                .device = shard.execution.device,
                                                .queue = shard.execution.queue};
                        auto start = EarliestGap(FindQueue(committedQueues, queue), FindQueue(speculativeQueues, queue),
                                                 ready, durations[index], node);
                        if (!start) {
                            failure = start.error();
                            return std::unexpected(AutomaticDispatchPlanningError::InvalidRequest);
                        }
                        auto finish = Add(*start, durations[index], node, HeftPlanningErrorCode::CapacityExceeded);
                        if (!finish) {
                            failure = finish.error();
                            return std::unexpected(AutomaticDispatchPlanningError::InvalidRequest);
                        }
                        shard.startNanoseconds = *start;
                        shard.finishNanoseconds = *finish;
                        candidate.parallelNanoseconds = std::max(candidate.parallelNanoseconds, *finish);
                        InsertQueueInterval(speculativeQueues, queue, {*start, *finish, node, index});
                    }
                    auto completion = Add(candidate.parallelNanoseconds, candidate.partitionOverheadNanoseconds, node,
                                          HeftPlanningErrorCode::CapacityExceeded);
                    if (completion) {
                        completion = Add(*completion, candidate.publicationNanoseconds, node,
                                         HeftPlanningErrorCode::CapacityExceeded);
                    }
                    if (!completion) {
                        failure = completion.error();
                        return std::unexpected(AutomaticDispatchPlanningError::InvalidRequest);
                    }
                    candidate.totalNanoseconds = *completion;
                    return true;
                } catch (const std::bad_alloc&) {
                    failure = Error(HeftPlanningErrorCode::OutOfMemory, node);
                    return std::unexpected(AutomaticDispatchPlanningError::OutOfMemory);
                }
            }
        };

        [[nodiscard]] bool PartitionSufficientlyImproves(uint64_t releaseNanoseconds, uint64_t wholeFinishNanoseconds,
                                                         uint64_t partitionFinishNanoseconds,
                                                         uint32_t minimumImprovementPermille) noexcept {
            if (partitionFinishNanoseconds >= wholeFinishNanoseconds || wholeFinishNanoseconds < releaseNanoseconds ||
                partitionFinishNanoseconds < releaseNanoseconds) {
                return false;
            }
            const uint64_t wholeElapsed = wholeFinishNanoseconds - releaseNanoseconds;
            const uint64_t partitionElapsed = partitionFinishNanoseconds - releaseNanoseconds;
            const uint64_t retainedPermille = 1000 - minimumImprovementPermille;
            const uint64_t floor =
                (wholeElapsed / 1000) * retainedPermille + (wholeElapsed % 1000) * retainedPermille / 1000;
            const bool exact = (wholeElapsed % 1000) * retainedPermille % 1000 == 0;
            return exact ? partitionElapsed < floor : partitionElapsed <= floor;
        }

        [[nodiscard]] constexpr uint64_t SaturatingMultiply(uint64_t left, uint64_t right) noexcept {
            return left != 0 && right > std::numeric_limits<uint64_t>::max() / left
                       ? std::numeric_limits<uint64_t>::max()
                       : left * right;
        }

        [[nodiscard]] constexpr uint64_t ScaleUp(uint64_t value, uint32_t permille) noexcept {
            const uint64_t whole = value / 1000U;
            const uint64_t remainder = value % 1000U;
            const uint64_t scaledWhole = SaturatingMultiply(whole, permille);
            const uint64_t scaledRemainder = (remainder * permille + 999U) / 1000U;
            return scaledWhole > std::numeric_limits<uint64_t>::max() - scaledRemainder
                       ? std::numeric_limits<uint64_t>::max()
                       : scaledWhole + scaledRemainder;
        }

        [[nodiscard]] std::optional<AutomaticPartitionPlanningShape>
        DescribeAutomaticPartitionPlanning(const KernelInvocation& invocation, const DispatchCandidateSet& candidates,
                                           const AutomaticPartitionPolicy& policy) noexcept {
            if (candidates.placements.size() > std::numeric_limits<uint32_t>::max()) {
                return std::nullopt;
            }
            const DispatchDomain& domain = invocation.Domain();
            const PartitionContract contract = invocation.Partition();
            uint8_t axisMask = static_cast<uint8_t>(contract.splittableAxisMask & policy.allowedAxisMask);
            for (uint8_t axis = 0; axis < kMaximumIndexRank; ++axis) {
                if (axis >= domain.rank || domain.extents[axis] <= 1) {
                    axisMask &= static_cast<uint8_t>(~(uint8_t{1} << axis));
                }
            }
            const uint64_t maximumInputRoutes = std::ranges::fold_left(
                candidates.residencies | std::views::transform([](const DispatchResidencyProjection& projection) {
                    return static_cast<uint64_t>(projection.inputs.size());
                }),
                uint64_t{0}, std::ranges::max);
            std::array<DeviceId, kMaximumAutomaticPartitionDevices> distinctDevices{};
            const uint32_t maximumDeviceCount =
                std::min<uint32_t>(policy.maximumDevices, static_cast<uint32_t>(distinctDevices.size()));
            uint32_t distinctDeviceCount = 0;
            for (const DispatchPlacementCandidate& placement : candidates.placements) {
                if (distinctDeviceCount == maximumDeviceCount) {
                    break;
                }
                const auto known = std::span{distinctDevices}.first(distinctDeviceCount);
                if (std::ranges::contains(known, placement.placement.device.id)) {
                    continue;
                }
                distinctDevices[distinctDeviceCount++] = placement.placement.device.id;
            }
            const auto shape = AutomaticPartitionPlanningShape{
                .axisCount = static_cast<uint8_t>(std::popcount(axisMask)),
                .placementCount = static_cast<uint32_t>(candidates.placements.size()),
                .inputRouteCount = static_cast<uint32_t>(
                    std::min<uint64_t>(SaturatingMultiply(maximumInputRoutes, distinctDeviceCount), UINT32_MAX))};
            return shape.Valid() ? std::optional{shape} : std::nullopt;
        }

        [[nodiscard]] std::optional<GraphNodeId> LatestPartitionedAncestor(GraphNodeId failed,
                                                                           const GraphTopology& topology,
                                                                           const ScheduleResult& schedule,
                                                                           std::span<const uint8_t> partitionDisabled) {
            std::vector<GraphNodeId> pending = topology.predecessors[failed.index];
            std::vector<uint8_t> visited(schedule.nodes.size());
            std::optional<GraphNodeId> best;
            while (!pending.empty()) {
                const GraphNodeId node = pending.back();
                pending.pop_back();
                if (visited[node.index] != 0) {
                    continue;
                }
                visited[node.index] = 1;
                const ScheduledNode& scheduled = schedule.nodes[node.index];
                if (scheduled.scheduled && partitionDisabled[node.index] == 0 &&
                    std::holds_alternative<DispatchGraphPartitionSelection>(scheduled.selection.execution) &&
                    (!best || std::tuple{schedule.nodes[best->index].finishNanoseconds, *best} <
                                  std::tuple{scheduled.finishNanoseconds, node})) {
                    best = node;
                }
                pending.insert(pending.end(), topology.predecessors[node.index].begin(),
                               topology.predecessors[node.index].end());
            }
            return best;
        }

        [[nodiscard]] std::expected<ScheduleResult, HeftPlanningError> BuildScheduleAttempt(
            const ExecutionGraph& graph, const GraphPlanningContext& context, GraphPlanningServices services,
            const HeftPlanningPolicy& policy, const AutomaticPartitionPlanningSnapshot* planningCalibration,
            AutomaticPartitionPlanningCalibration* planningCalibrationModel, const GraphTopology& topology,
            std::span<const uint64_t> ranks, GraphResidencyState residency, std::span<const uint8_t> partitionDisabled,
            GraphNodeId& partitionFallback, HeftPlanningStatistics& statistics,
            std::span<const std::optional<PlacementDecision>> directives = {}, bool collectAlternatives = false) {
            try {
                if (!directives.empty() && directives.size() != graph.NodeCount()) {
                    return std::unexpected(Error(HeftPlanningErrorCode::SchedulingFailed));
                }
                ScheduleResult result{.nodes = std::vector<ScheduledNode>(graph.NodeCount())};
                if (collectAlternatives) {
                    result.placementAlternatives.resize(graph.NodeCount());
                }
                std::vector<GraphNodeId> order;
                order.reserve(graph.NodeCount());
                for (uint32_t node = 0; node < graph.NodeCount(); ++node) {
                    order.push_back({node});
                }
                std::ranges::sort(order, [&](GraphNodeId left, GraphNodeId right) {
                    return ranks[left.index] != ranks[right.index] ? ranks[left.index] > ranks[right.index]
                                                                   : left < right;
                });
                std::vector<ReplicaReady> replicas;
                replicas.reserve(graph.AccessCount());

                for (const GraphNodeId node : order) {
                    uint64_t predecessorReady = 0;
                    for (const GraphNodeId predecessor : topology.predecessors[node.index]) {
                        if (!result.nodes[predecessor.index].scheduled) {
                            return std::unexpected(
                                Error(HeftPlanningErrorCode::SchedulingFailed, node, {}, predecessor.index));
                        }
                        predecessorReady =
                            std::max(predecessorReady, result.nodes[predecessor.index].finishNanoseconds);
                    }
                    auto candidates =
                        EnumerateDispatchCandidates(graph, node, context, services, residency, policy.dispatch);
                    if (!candidates) {
                        return std::unexpected(EnumerationError(candidates.error()));
                    }
                    CandidateChoice best;
                    for (uint32_t index = 0; index < candidates->placements.size(); ++index) {
                        const DispatchPlacementCandidate& candidate = candidates->placements[index];
                        if (candidate.residencyProjection >= candidates->residencies.size()) {
                            return std::unexpected(Error(HeftPlanningErrorCode::CandidateEnumerationFailed, node));
                        }
                        const DispatchResidencyProjection& projection =
                            candidates->residencies[candidate.residencyProjection];
                        const auto execution = PlanExecution(
                            candidates->workload, std::span<const PlacementCandidate>{&candidate.placement, 1},
                            policy.placement);
                        if (!execution) {
                            if (execution.error() == PlanningError::NoCandidate) {
                                continue;
                            }
                            return std::unexpected(Error(HeftPlanningErrorCode::CandidateEnumerationFailed, node, {},
                                                         static_cast<uint64_t>(execution.error())));
                        }
                        const bool directed = !directives.empty() && directives[node.index].has_value();
                        if (collectAlternatives || directed) {
                            const PlacementDecision decision = DecisionFor(candidate, projection);
                            if (collectAlternatives) {
                                RecordAlternative(result.placementAlternatives[node.index], decision);
                            }
                            if (directed && !SameDecision(*directives[node.index], decision)) {
                                continue;
                            }
                        }
                        auto duration = ServiceDuration(execution->cost, node);
                        if (!duration) {
                            return std::unexpected(duration.error());
                        }
                        auto transfers = ScheduleTransfers(candidates->residencies[candidate.residencyProjection],
                                                           result.transfers, replicas, node);
                        if (!transfers) {
                            return std::unexpected(transfers.error());
                        }
                        const uint64_t ready =
                            std::max({predecessorReady, transfers->readyNanoseconds, execution->cost.queueNanoseconds});
                        const QueueKey queue = QueueFor(candidate.placement);
                        auto start = EarliestGap(FindQueue(result.queues, queue), nullptr, ready, *duration, node);
                        if (!start) {
                            return std::unexpected(start.error());
                        }
                        auto finish = Add(*start, *duration, node, HeftPlanningErrorCode::CapacityExceeded);
                        if (!finish) {
                            return std::unexpected(finish.error());
                        }
                        CandidateChoice choice{.valid = true,
                                               .startNanoseconds = *start,
                                               .finishNanoseconds = *finish,
                                               .execution = WholeCandidateChoice{index},
                                               .transfers = std::move(transfers->calendars)};
                        choice.inputReadyNanoseconds.push_back(std::move(transfers->inputReadyNanoseconds));
                        choice.shardFinishNanoseconds.push_back(*finish);
                        InsertQueueInterval(choice.queues, queue, {*start, *finish, node, 0});
                        if (BetterChoice(choice, candidate.placement, best, *candidates)) {
                            best = std::move(choice);
                        }
                    }
                    if (!best.valid) {
                        if (auto fallback = LatestPartitionedAncestor(node, topology, result, partitionDisabled)) {
                            partitionFallback = *fallback;
                        }
                        return std::unexpected(Error(HeftPlanningErrorCode::NoCandidate, node));
                    }

                    const auto& invocation = std::get<KernelInvocation>(graph.Operation(node));
                    const auto wholeBaseline = std::get<WholeCandidateChoice>(best.execution);
                    const DispatchPlacementCandidate& baselinePlacement =
                        candidates->placements[wholeBaseline.placement];
                    const DispatchResidencyProjection& baselineProjection =
                        candidates->residencies[baselinePlacement.residencyProjection];
                    const bool directed = !directives.empty() && directives[node.index].has_value();
                    const auto planningShape =
                        DescribeAutomaticPartitionPlanning(invocation, *candidates, policy.automaticPartition);
                    const auto planningEstimate = planningShape && planningCalibration != nullptr
                                                      ? planningCalibration->Estimate(*planningShape)
                                                      : std::nullopt;
                    const auto partitionResolution =
                        planningShape
                            ? ResolveHeftAutomaticPartitionPolicy(
                                  policy.automaticPartition, policy.automaticPartitionTuning, *planningShape,
                                  planningEstimate,
                                  best.finishNanoseconds >= predecessorReady ? best.finishNanoseconds - predecessorReady
                                                                             : 0,
                                  candidates->workload.expectedReuseCount)
                            : HeftAutomaticPartitionResolution{};
                    if (!directed && partitionDisabled[node.index] == 0 && partitionResolution.search) {
                        AutomaticPartitionPolicy partitionPolicy = partitionResolution.policy;
                        partitionPolicy.placement = policy.placement;
                        // The outer gate used the actual DAG whole elapsed time; the standalone search must not re-gate
                        // it.
                        partitionPolicy.minimumAmortizedBenefitNanoseconds = 0;
                        HeftPartitionScheduler scheduler{.node = node,
                                                         .predecessorReadyNanoseconds = predecessorReady,
                                                         .committedQueues = result.queues,
                                                         .committedTransfers = result.transferReservations,
                                                         .replicas = replicas};
                        ++statistics.partitionSearches;
                        statistics.partitionEvaluationBudget += partitionPolicy.maximumPartitionEvaluations;
                        const auto searchStart = planningCalibrationModel != nullptr
                                                     ? std::chrono::steady_clock::now()
                                                     : std::chrono::steady_clock::time_point{};
                        auto searched = Detail::SearchAutomaticDispatchPartitions(
                            invocation, *candidates, services, residency, partitionPolicy,
                            Detail::AutomaticPartitionSchedulerRef{scheduler});
                        const auto searchElapsed = planningCalibrationModel != nullptr
                                                       ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                             std::chrono::steady_clock::now() - searchStart)
                                                             .count()
                                                       : 0;
                        if (!searched) {
                            if (scheduler.failure) {
                                return std::unexpected(*scheduler.failure);
                            }
                            const auto code =
                                searched.error() == AutomaticDispatchPlanningError::OutOfMemory
                                    ? HeftPlanningErrorCode::OutOfMemory
                                : searched.error() == AutomaticDispatchPlanningError::InsufficientSearchBudget
                                    ? HeftPlanningErrorCode::InvalidPolicy
                                    : HeftPlanningErrorCode::SchedulingFailed;
                            return std::unexpected(Error(code, node, {}, static_cast<uint64_t>(searched.error())));
                        }
                        statistics.partitionEvaluations += searched->partitionEvaluations;
                        if (planningCalibrationModel != nullptr && searched->partitionEvaluations != 0 &&
                            searchElapsed > 0) {
                            static_cast<void>(planningCalibrationModel->Observe(
                                {.shape = *planningShape,
                                 .evaluationCount = searched->partitionEvaluations,
                                 .elapsedNanoseconds = static_cast<uint64_t>(searchElapsed)}));
                        }
                        if (searched->bestPartition &&
                            PartitionSufficientlyImproves(predecessorReady, best.finishNanoseconds,
                                                          searched->bestPartition->totalNanoseconds,
                                                          partitionPolicy.minimumImprovementPermille)) {
                            CandidateChoice partitionChoice{
                                .valid = true,
                                .startNanoseconds = UINT64_MAX,
                                .finishNanoseconds = searched->bestPartition->totalNanoseconds,
                                .execution = PartitionCandidateChoice{std::move(*searched->bestPartition)}};
                            auto& partition = std::get<PartitionCandidateChoice>(partitionChoice.execution).execution;
                            partitionChoice.inputReadyNanoseconds.reserve(partition.shards.size());
                            partitionChoice.shardFinishNanoseconds.reserve(partition.shards.size());
                            partitionChoice.queues.reserve(partition.shards.size());
                            for (uint32_t index = 0; index < partition.shards.size(); ++index) {
                                PartitionShardExecution& shard = partition.shards[index];
                                if (shard.execution.candidateIndex >= candidates->placements.size() ||
                                    shard.routeReadyNanoseconds.size() != shard.residency.inputs.size()) {
                                    return std::unexpected(Error(HeftPlanningErrorCode::SchedulingFailed, node));
                                }
                                partitionChoice.startNanoseconds =
                                    std::min(partitionChoice.startNanoseconds, shard.startNanoseconds);
                                partitionChoice.inputReadyNanoseconds.push_back(std::move(shard.routeReadyNanoseconds));
                                partitionChoice.shardFinishNanoseconds.push_back(shard.finishNanoseconds);
                                const PlacementCandidate& placement =
                                    candidates->placements[shard.execution.candidateIndex].placement;
                                InsertQueueInterval(partitionChoice.queues, QueueFor(placement),
                                                    {shard.startNanoseconds, shard.finishNanoseconds, node, index});
                            }
                            for (const ScheduledRouteTransfer& transfer : partition.transfers) {
                                if (transfer.operation >= partition.shards.size() ||
                                    transfer.route >= partition.shards[transfer.operation].residency.inputs.size()) {
                                    return std::unexpected(Error(HeftPlanningErrorCode::SchedulingFailed, node));
                                }
                                const GraphResidencyRoute& route =
                                    partition.shards[transfer.operation].residency.inputs[transfer.route];
                                if (transfer.hop >= route.hops.size()) {
                                    return std::unexpected(Error(HeftPlanningErrorCode::SchedulingFailed, node));
                                }
                                Detail::InsertTransferTimelineInterval(partitionChoice.transfers,
                                                                       {transfer.service, transfer.engine},
                                                                       {.beginNanoseconds = transfer.beginNanoseconds,
                                                                        .endNanoseconds = transfer.endNanoseconds,
                                                                        .provenance = {.consumer = node,
                                                                                       .buffer = route.buffer,
                                                                                       .dispatch = transfer.operation,
                                                                                       .route = transfer.route,
                                                                                       .hop = transfer.hop}});
                            }
                            best = std::move(partitionChoice);
                            ++statistics.acceptedPartitions;
                        }
                    }

                    DispatchGraphSelection selection{.node = node, .workload = candidates->workload};
                    if (auto* whole = std::get_if<WholeCandidateChoice>(&best.execution)) {
                        const DispatchPlacementCandidate& placement = candidates->placements[whole->placement];
                        const DispatchResidencyProjection& projection =
                            candidates->residencies[placement.residencyProjection];
                        selection.execution =
                            DispatchGraphShardSelection{.slice = DispatchSlice::Whole(invocation.Domain()),
                                                        .placement = placement.placement,
                                                        .residency = projection,
                                                        .geometry = candidates->geometry};
                    } else {
                        auto partition = std::move(std::get<PartitionCandidateChoice>(best.execution).execution);
                        DispatchGraphPartitionSelection selected{.axis = partition.axis,
                                                                 .logicalBindings = baselineProjection.bindings};
                        selected.shards.reserve(partition.shards.size());
                        for (PartitionShardExecution& shard : partition.shards) {
                            const DispatchPlacementCandidate& placement =
                                candidates->placements[shard.execution.candidateIndex];
                            auto geometry = PlanDispatchSliceGeometry(shard.slice);
                            if (!geometry) {
                                return std::unexpected(Error(HeftPlanningErrorCode::SchedulingFailed, node, {},
                                                             static_cast<uint64_t>(geometry.error())));
                            }
                            selected.shards.push_back({.slice = shard.slice,
                                                       .placement = placement.placement,
                                                       .residency = std::move(shard.residency),
                                                       .geometry = *geometry});
                        }
                        selection.execution = std::move(selected);
                    }

                    std::expected<void, GraphResidencyError> committed;
                    if (const auto* whole = std::get_if<DispatchGraphShardSelection>(&selection.execution)) {
                        committed = CommitDispatchResidencySelection(node, whole->residency, residency);
                    } else {
                        const auto& partition = std::get<DispatchGraphPartitionSelection>(selection.execution);
                        std::vector<DispatchResidencyProjection> projections;
                        projections.reserve(partition.shards.size());
                        for (const DispatchGraphShardSelection& shard : partition.shards) {
                            projections.push_back(shard.residency);
                        }
                        committed = CommitPartitionedDispatchResidencySelection(node, partition.logicalBindings,
                                                                                projections, residency);
                    }
                    if (!committed) {
                        const auto code = committed.error() == GraphResidencyError::OutOfMemory
                                              ? HeftPlanningErrorCode::OutOfMemory
                                              : HeftPlanningErrorCode::ResidencyTransitionFailed;
                        return std::unexpected(Error(code, node, {}, static_cast<uint64_t>(committed.error())));
                    }
                    if (auto recorded = RecordSelectionReplicas(selection, residency, best.inputReadyNanoseconds,
                                                                best.shardFinishNanoseconds, replicas);
                        !recorded) {
                        return std::unexpected(recorded.error());
                    }
                    CommitCandidateCalendars(best, result);
                    result.makespanNanoseconds = std::max(result.makespanNanoseconds, best.finishNanoseconds);
                    result.nodes[node.index] = {.scheduled = true,
                                                .startNanoseconds = best.startNanoseconds,
                                                .finishNanoseconds = best.finishNanoseconds,
                                                .selection = std::move(selection)};
                }
                return result;
            } catch (const std::bad_alloc&) {
                return std::unexpected(Error(HeftPlanningErrorCode::OutOfMemory));
            }
        }

        [[nodiscard]] std::expected<ScheduleResult, HeftPlanningError> BuildSchedule(
            const ExecutionGraph& graph, const GraphPlanningContext& context, GraphPlanningServices services,
            const HeftPlanningPolicy& policy, const AutomaticPartitionPlanningSnapshot* planningCalibration,
            AutomaticPartitionPlanningCalibration* planningCalibrationModel, const GraphTopology& topology,
            std::span<const uint64_t> ranks, const GraphResidencyState& residency, HeftPlanningStatistics& statistics,
            std::span<const std::optional<PlacementDecision>> directives = {}, bool collectAlternatives = false) {
            try {
                std::vector<uint8_t> partitionDisabled(graph.NodeCount());
                for (uint32_t attempt = 0; attempt <= graph.NodeCount(); ++attempt) {
                    GraphNodeId fallback{};
                    auto result = BuildScheduleAttempt(
                        graph, context, services, policy, planningCalibration, planningCalibrationModel, topology,
                        ranks, residency, partitionDisabled, fallback, statistics, directives, collectAlternatives);
                    if (result || result.error().code != HeftPlanningErrorCode::NoCandidate || !fallback.Valid()) {
                        return result;
                    }
                    partitionDisabled[fallback.index] = 1;
                }
                return std::unexpected(Error(HeftPlanningErrorCode::SchedulingFailed));
            } catch (const std::bad_alloc&) {
                return std::unexpected(Error(HeftPlanningErrorCode::OutOfMemory));
            }
        }

        [[nodiscard]] bool SufficientImprovement(uint64_t incumbent, uint64_t candidate,
                                                 uint64_t minimumImprovement) noexcept {
            return candidate < incumbent && incumbent - candidate >= minimumImprovement;
        }

        [[nodiscard]] std::expected<ScheduleResult, HeftPlanningError>
        OptimizeSchedule(const ExecutionGraph& graph, const GraphPlanningContext& context,
                         GraphPlanningServices services, const HeftPlanningPolicy& policy,
                         const AutomaticPartitionPlanningSnapshot* planningCalibration,
                         AutomaticPartitionPlanningCalibration* planningCalibrationModel, const GraphTopology& topology,
                         std::span<const uint64_t> ranks, const GraphResidencyState& baseline, ScheduleResult incumbent,
                         HeftPlanningStatistics& statistics) {
            try {
                std::vector<std::optional<PlacementDecision>> directives(graph.NodeCount());
                std::vector<GraphNodeId> nodeOrder(graph.NodeCount());
                for (uint32_t node = 0; node < graph.NodeCount(); ++node) {
                    nodeOrder[node] = {node};
                }

                for (uint32_t pass = 0; pass < policy.localSearch.maximumPasses &&
                                        statistics.neighborEvaluations < policy.localSearch.maximumNeighborEvaluations;
                     ++pass) {
                    std::ranges::sort(nodeOrder, [&](GraphNodeId left, GraphNodeId right) {
                        const uint64_t leftFinish = incumbent.nodes[left.index].finishNanoseconds;
                        const uint64_t rightFinish = incumbent.nodes[right.index].finishNanoseconds;
                        return leftFinish != rightFinish ? leftFinish > rightFinish : left < right;
                    });

                    std::optional<ScheduleResult> bestSchedule;
                    std::optional<PlacementDecision> bestDirective;
                    GraphNodeId bestNode{};
                    const auto evaluate =
                        [&](GraphNodeId node,
                            std::optional<PlacementDecision> proposed) -> std::expected<void, HeftPlanningError> {
                        const std::optional<PlacementDecision> candidateDirective = proposed;
                        const std::optional<PlacementDecision> original = directives[node.index];
                        directives[node.index] = std::move(proposed);
                        auto neighbor = BuildSchedule(graph, context, services, policy, planningCalibration,
                                                      planningCalibrationModel, topology, ranks, baseline, statistics,
                                                      directives, true);
                        directives[node.index] = original;
                        ++statistics.neighborEvaluations;
                        if (!neighbor) {
                            if (neighbor.error().code == HeftPlanningErrorCode::NoCandidate) {
                                return {};
                            }
                            return std::unexpected(neighbor.error());
                        }
                        if (!SufficientImprovement(incumbent.makespanNanoseconds, neighbor->makespanNanoseconds,
                                                   policy.localSearch.minimumImprovementNanoseconds) ||
                            (bestSchedule && bestSchedule->makespanNanoseconds <= neighbor->makespanNanoseconds)) {
                            return {};
                        }
                        bestDirective = candidateDirective;
                        bestNode = node;
                        bestSchedule = std::move(*neighbor);
                        return {};
                    };

                    for (const GraphNodeId node : nodeOrder) {
                        const std::optional<PlacementDecision> selected =
                            DecisionFor(incumbent.nodes[node.index].selection);
                        if (directives[node.index] &&
                            statistics.neighborEvaluations < policy.localSearch.maximumNeighborEvaluations) {
                            if (auto evaluated = evaluate(node, std::nullopt); !evaluated) {
                                return std::unexpected(evaluated.error());
                            }
                        }
                        for (const PlacementDecision& alternative : incumbent.placementAlternatives[node.index]) {
                            if (statistics.neighborEvaluations >= policy.localSearch.maximumNeighborEvaluations) {
                                break;
                            }
                            if (selected && SameDecision(*selected, alternative)) {
                                continue;
                            }
                            if (auto evaluated = evaluate(node, alternative); !evaluated) {
                                return std::unexpected(evaluated.error());
                            }
                        }
                        if (statistics.neighborEvaluations >= policy.localSearch.maximumNeighborEvaluations) {
                            break;
                        }
                    }
                    if (!bestSchedule) {
                        break;
                    }
                    directives[bestNode.index] = std::move(bestDirective);
                    incumbent = std::move(*bestSchedule);
                    ++statistics.acceptedMoves;
                }
                return incumbent;
            } catch (const std::bad_alloc&) {
                return std::unexpected(Error(HeftPlanningErrorCode::OutOfMemory));
            }
        }

        void AddConstraint(std::vector<std::vector<GraphNodeId>>& successors, std::vector<uint32_t>& indegrees,
                           GraphNodeId predecessor, GraphNodeId successor) {
            if (predecessor == successor ||
                std::ranges::find(successors[predecessor.index], successor) != successors[predecessor.index].end()) {
                return;
            }
            successors[predecessor.index].push_back(successor);
            ++indegrees[successor.index];
        }

        [[nodiscard]] std::expected<std::vector<DispatchGraphSelection>, HeftPlanningError>
        BuildEmissionOrder(const ExecutionGraph& graph, const GraphTopology& topology, const ScheduleResult& schedule) {
            try {
                std::vector<std::vector<GraphNodeId>> successors = topology.successors;
                std::vector<uint32_t> indegrees;
                indegrees.reserve(graph.NodeCount());
                for (const auto& predecessors : topology.predecessors) {
                    if (predecessors.size() > UINT32_MAX) {
                        return std::unexpected(Error(HeftPlanningErrorCode::CapacityExceeded));
                    }
                    indegrees.push_back(static_cast<uint32_t>(predecessors.size()));
                }
                for (const ScheduledNode& node : schedule.nodes) {
                    ForEachShard(node.selection, [&](uint32_t, const DispatchGraphShardSelection& shard) {
                        for (const GraphResidencyRoute& route : shard.residency.inputs) {
                            if (const auto* source = std::get_if<GraphLocalResidencySource>(&route.source);
                                source != nullptr && source->replica.materializer != node.selection.node) {
                                AddConstraint(successors, indegrees, source->replica.materializer, node.selection.node);
                            }
                        }
                    });
                }
                for (const QueueCalendar& queue : schedule.queues) {
                    for (size_t index = 1; index < queue.intervals.size(); ++index) {
                        AddConstraint(successors, indegrees, queue.intervals[index - 1].node,
                                      queue.intervals[index].node);
                    }
                }

                std::vector<DispatchGraphSelection> result;
                result.reserve(graph.NodeCount());
                std::vector<uint8_t> emitted(graph.NodeCount());
                while (result.size() != graph.NodeCount()) {
                    GraphNodeId best{};
                    for (uint32_t node = 0; node < graph.NodeCount(); ++node) {
                        if (emitted[node] != 0 || indegrees[node] != 0) {
                            continue;
                        }
                        if (!best.Valid() || std::tuple{schedule.nodes[node].startNanoseconds, node} <
                                                 std::tuple{schedule.nodes[best.index].startNanoseconds, best.index}) {
                            best = {node};
                        }
                    }
                    if (!best.Valid()) {
                        return std::unexpected(Error(HeftPlanningErrorCode::SchedulingFailed));
                    }
                    emitted[best.index] = 1;
                    result.push_back(schedule.nodes[best.index].selection);
                    for (const GraphNodeId successor : successors[best.index]) {
                        if (indegrees[successor.index] == 0) {
                            return std::unexpected(Error(HeftPlanningErrorCode::SchedulingFailed, successor));
                        }
                        --indegrees[successor.index];
                    }
                }
                return result;
            } catch (const std::bad_alloc&) {
                return std::unexpected(Error(HeftPlanningErrorCode::OutOfMemory));
            }
        }

        [[nodiscard]] std::expected<std::vector<DispatchTransferConstraint>, HeftPlanningError>
        BuildTransferConstraints(const ScheduleResult& schedule) {
            try {
                size_t count = 0;
                for (const TransferCalendar& calendar : schedule.transfers) {
                    const size_t additional =
                        calendar.intervals.size() - static_cast<size_t>(!calendar.intervals.empty());
                    if (additional > std::numeric_limits<size_t>::max() - count) {
                        return std::unexpected(Error(HeftPlanningErrorCode::CapacityExceeded));
                    }
                    count += additional;
                }
                std::vector<DispatchTransferConstraint> result;
                if (count > result.max_size()) {
                    return std::unexpected(Error(HeftPlanningErrorCode::CapacityExceeded));
                }
                result.reserve(count);
                for (const TransferCalendar& calendar : schedule.transfers) {
                    for (size_t index = 1; index < calendar.intervals.size(); ++index) {
                        result.push_back({.predecessor = calendar.intervals[index - 1].provenance,
                                          .successor = calendar.intervals[index].provenance});
                    }
                }
                return result;
            } catch (const std::bad_alloc&) {
                return std::unexpected(Error(HeftPlanningErrorCode::OutOfMemory));
            }
        }

    } // namespace

    HeftAutomaticPartitionResolution ResolveHeftAutomaticPartitionPolicy(
        const AutomaticPartitionPolicy& policy, const HeftAutomaticPartitionTuningPolicy& tuning,
        AutomaticPartitionPlanningShape shape, std::optional<AutomaticPartitionPlanningEstimate> estimate,
        uint64_t wholeElapsedNanoseconds, uint64_t expectedReuseCount) noexcept {
        HeftAutomaticPartitionResolution result{.policy = policy};
        if (!policy.enabled || !policy.Valid() || !tuning.Valid() || !shape.Valid() || wholeElapsedNanoseconds == 0 ||
            expectedReuseCount == 0) {
            return result;
        }
        result.amortizedWholeNanoseconds = SaturatingMultiply(wholeElapsedNanoseconds, expectedReuseCount);
        if (!tuning.enabled) {
            result.search = result.amortizedWholeNanoseconds >= policy.minimumAmortizedBenefitNanoseconds;
            return result;
        }

        if (!estimate || !estimate->Valid() || estimate->sampleCount < tuning.minimumCalibrationSamples) {
            result.policy.maximumPartitionEvaluations =
                std::min(policy.maximumPartitionEvaluations, tuning.coldStartMaximumEvaluations);
            const uint64_t minimumBenefit =
                std::max(policy.minimumAmortizedBenefitNanoseconds, tuning.coldStartMinimumBenefitNanoseconds);
            result.search = result.policy.maximumPartitionEvaluations >= shape.axisCount &&
                            result.amortizedWholeNanoseconds >= minimumBenefit;
            return result;
        }

        result.calibrated = true;
        const uint64_t returnedCost =
            ScaleUp(estimate->GuardedNanosecondsPerEvaluation(), tuning.minimumPlanningReturnPermille);
        if (returnedCost == 0) {
            return result;
        }
        const uint64_t affordable = result.amortizedWholeNanoseconds / returnedCost;
        result.policy.maximumPartitionEvaluations =
            static_cast<uint32_t>(std::min<uint64_t>(policy.maximumPartitionEvaluations, affordable));
        result.search = result.policy.maximumPartitionEvaluations >= shape.axisCount &&
                        result.amortizedWholeNanoseconds >= policy.minimumAmortizedBenefitNanoseconds;
        return result;
    }

    std::expected<HeftPlanningResult, HeftPlanningError>
    HeftPlanner::Plan(const ExecutionGraph& graph, const GraphPlanningContext& context,
                      GraphPlanningServices services) const noexcept {
        if (!policy_.localSearch.Valid() || !policy_.automaticPartition.Valid() ||
            !policy_.automaticPartitionTuning.Valid()) {
            return std::unexpected(Error(HeftPlanningErrorCode::InvalidPolicy));
        }
        if (auto valid = ValidateDispatchGraph(graph); !valid) {
            return std::unexpected(CommonError(valid.error()));
        }
        auto baseline = FreezeGraphResidency(graph);
        if (!baseline) {
            const auto code = baseline.error().code == GraphResidencySnapshotErrorCode::OutOfMemory
                                  ? HeftPlanningErrorCode::OutOfMemory
                                  : HeftPlanningErrorCode::ResidencySnapshotFailed;
            return std::unexpected(Error(code, baseline.error().node, {},
                                         baseline.error().cause != 0 ? baseline.error().cause
                                                                     : static_cast<uint64_t>(baseline.error().code)));
        }
        try {
            auto topology = BuildTopology(graph.View());
            if (!topology) {
                return std::unexpected(topology.error());
            }
            auto ranks = BuildUpwardRanks(graph, context, services, policy_);
            if (!ranks) {
                return std::unexpected(ranks.error());
            }
            std::optional<AutomaticPartitionPlanningSnapshot> planningCalibration;
            if (automaticPlanningCalibration_ != nullptr) {
                planningCalibration.emplace(automaticPlanningCalibration_->Snapshot());
            }
            HeftPlanningStatistics statistics;
            auto schedule = BuildSchedule(graph, context, services, policy_,
                                          planningCalibration ? std::addressof(*planningCalibration) : nullptr,
                                          automaticPlanningCalibration_.get(), *topology, *ranks, *baseline, statistics,
                                          {}, policy_.localSearch.Enabled());
            if (!schedule) {
                return std::unexpected(schedule.error());
            }
            statistics.baselineMakespanNanoseconds = schedule->makespanNanoseconds;
            statistics.optimizedMakespanNanoseconds = schedule->makespanNanoseconds;
            if (policy_.localSearch.Enabled()) {
                schedule = OptimizeSchedule(graph, context, services, policy_,
                                            planningCalibration ? std::addressof(*planningCalibration) : nullptr,
                                            automaticPlanningCalibration_.get(), *topology, *ranks, *baseline,
                                            std::move(*schedule), statistics);
                if (!schedule) {
                    return std::unexpected(schedule.error());
                }
                statistics.optimizedMakespanNanoseconds = schedule->makespanNanoseconds;
            }
            auto selections = BuildEmissionOrder(graph, *topology, *schedule);
            if (!selections) {
                return std::unexpected(selections.error());
            }
            auto transferConstraints = BuildTransferConstraints(*schedule);
            if (!transferConstraints) {
                return std::unexpected(transferConstraints.error());
            }
            auto lowered = LowerDispatchGraph(graph, services, policy_.dispatch.memory, *baseline, *selections,
                                              *transferConstraints);
            if (!lowered) {
                return std::unexpected(CommonError(lowered.error()));
            }
            return HeftPlanningResult{.storage = std::move(*lowered), .statistics = statistics};
        } catch (const std::bad_alloc&) {
            return std::unexpected(Error(HeftPlanningErrorCode::OutOfMemory));
        }
    }

    std::expected<PhysicalGraphPlanStorage, HeftPlanningError>
    HeftPlanner::operator()(const ExecutionGraph& graph, const GraphPlanningContext& context,
                            GraphPlanningServices services) const noexcept {
        auto result = Plan(graph, context, services);
        if (!result) {
            return std::unexpected(result.error());
        }
        return std::move(result->storage);
    }

} // namespace Sora::Compute

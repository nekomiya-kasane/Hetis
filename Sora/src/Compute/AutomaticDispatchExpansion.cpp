/**
 * @file AutomaticDispatchExpansion.cpp
 * @brief Automatic-dispatch validation and conversion into shared physical graph lowering.
 * @ingroup Compute
 */

#include <Sora/Compute/Runtime/AutomaticDispatchExpansion.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <new>
#include <ranges>
#include <span>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace Sora::Compute {
    namespace {

        [[nodiscard]] constexpr AutomaticDispatchExpansionError Error(AutomaticDispatchExpansionErrorCode code,
                                                                      GraphNodeId node = {},
                                                                      KernelArgumentId argument = {},
                                                                      uint64_t cause = 0) noexcept {
            return {.code = code, .node = node, .argument = argument, .cause = cause};
        }

        [[nodiscard]] bool SameSource(const GraphResidencySource& left, const GraphResidencySource& right) noexcept {
            if (left.index() != right.index()) {
                return false;
            }
            if (const auto* leftReady = std::get_if<ResidentReplica>(&left)) {
                const auto& rightReady = std::get<ResidentReplica>(right);
                return leftReady->allocation.View() == rightReady.allocation.View() &&
                       leftReady->bufferRegion == rightReady.bufferRegion &&
                       leftReady->allocationOffset == rightReady.allocationOffset &&
                       leftReady->version == rightReady.version && leftReady->Coverage() == rightReady.Coverage();
            }
            const auto& leftGraph = std::get<GraphLocalResidencySource>(left);
            const auto& rightGraph = std::get<GraphLocalResidencySource>(right);
            return leftGraph.buffer == rightGraph.buffer && leftGraph.coverage == rightGraph.coverage &&
                   leftGraph.byteFootprint == rightGraph.byteFootprint && leftGraph.version == rightGraph.version &&
                   leftGraph.producer == rightGraph.producer && leftGraph.replica == rightGraph.replica;
        }

        [[nodiscard]] bool SameBinding(const DispatchMemoryBinding& left, const DispatchMemoryBinding& right) noexcept {
            return left.argument == right.argument && left.access == right.access &&
                   left.buffer.Describe() == right.buffer.Describe() &&
                   left.buffer.Weak().SharesOwnership(right.buffer.Weak()) && left.region == right.region &&
                   left.byteFootprint == right.byteFootprint && left.alignment == right.alignment &&
                   left.publication == right.publication;
        }

        [[nodiscard]] bool SameRoute(const GraphResidencyRoute& left, const GraphResidencyRoute& right) noexcept {
            return left.buffer == right.buffer && SameSource(left.source, right.source) &&
                   left.destinationService == right.destinationService && left.destination == right.destination &&
                   left.region == right.region && left.byteFootprint == right.byteFootprint &&
                   left.version == right.version && left.estimatedNanoseconds == right.estimatedNanoseconds &&
                   left.hops == right.hops;
        }

        [[nodiscard]] bool SameProjection(const DispatchResidencyProjection& left,
                                          const DispatchResidencyProjection& right) noexcept {
            return left.service == right.service && left.device == right.device &&
                   left.destination == right.destination && left.transfer == right.transfer &&
                   left.allowCoherentAlias == right.allowCoherentAlias &&
                   std::ranges::equal(left.bindings, right.bindings, SameBinding) &&
                   std::ranges::equal(left.inputs, right.inputs, SameRoute);
        }

        [[nodiscard]] std::expected<DispatchGraphShardSelection, AutomaticDispatchExpansionError>
        ResolveShardSelection(const DispatchCandidateSet& candidates, const ExecutionPlan& execution,
                              const DispatchSlice& slice) {
            if (execution.candidateIndex >= candidates.placements.size()) {
                return std::unexpected(Error(AutomaticDispatchExpansionErrorCode::CandidateMismatch, candidates.node));
            }
            const DispatchPlacementCandidate& selected = candidates.placements[execution.candidateIndex];
            if (selected.residencyProjection >= candidates.residencies.size() ||
                execution.kernelHash != candidates.workload.kernelHash ||
                !ValidateExecutionPlan(execution, selected.placement)) {
                return std::unexpected(Error(AutomaticDispatchExpansionErrorCode::CandidateMismatch, candidates.node));
            }
            const DispatchResidencyProjection& residency = candidates.residencies[selected.residencyProjection];
            if (residency.service != selected.placement.service || residency.device != selected.placement.device.id ||
                (residency.destination &&
                 (residency.destination->service != residency.service ||
                  residency.destination->location.device != residency.device ||
                  residency.destination->location.backend != selected.placement.device.backend))) {
                return std::unexpected(Error(AutomaticDispatchExpansionErrorCode::CandidateMismatch, candidates.node));
            }
            const auto geometry = PlanDispatchSliceGeometry(slice);
            if (!geometry) {
                return std::unexpected(Error(AutomaticDispatchExpansionErrorCode::InvalidRequest, candidates.node, {},
                                             static_cast<uint64_t>(geometry.error())));
            }
            return DispatchGraphShardSelection{
                .slice = slice, .placement = selected.placement, .residency = residency, .geometry = *geometry};
        }

        [[nodiscard]] std::expected<AutomaticDispatchGraphLowering, AutomaticDispatchExpansionError>
        LowerBaseline(const ExecutionGraph& graph, GraphPlanningServices services,
                      const DestinationMemoryPolicy& memoryPolicy, const GraphResidencyState& baseline,
                      const DispatchCandidateSet& candidates, DispatchGraphShardSelection selection,
                      AutomaticDispatchFallback fallback) {
            const std::array selected{DispatchGraphSelection{
                .node = candidates.node, .workload = candidates.workload, .execution = std::move(selection)}};
            auto lowered = LowerDispatchGraph(graph, services, memoryPolicy, baseline, selected);
            if (!lowered) {
                return std::unexpected(Error(AutomaticDispatchExpansionErrorCode::BaselineLoweringFailed,
                                             lowered.error().node, lowered.error().argument,
                                             static_cast<uint64_t>(lowered.error().code)));
            }
            return AutomaticDispatchGraphLowering{.storage = std::move(*lowered), .fallback = fallback};
        }

        [[nodiscard]] bool IsValidStrictCover(const KernelInvocation& invocation,
                                              const PartitionedDispatchExecution& partitioned) noexcept {
            if (partitioned.shards.size() > kMaximumAutomaticPartitionDevices) {
                return false;
            }
            std::array<DispatchSlice, kMaximumAutomaticPartitionDevices> slices{};
            for (size_t index = 0; index < partitioned.shards.size(); ++index) {
                slices[index] = partitioned.shards[index].slice;
            }
            return ElementwisePartitionPlan::ValidateStrictCover(
                       invocation.Domain(), invocation.Partition(), partitioned.axis,
                       std::span<const DispatchSlice>{slices.data(), partitioned.shards.size()})
                .has_value();
        }

        struct ResolvedPartition {
            std::vector<DispatchGraphShardSelection> shards{};
            std::vector<DispatchTransferConstraint> transferConstraints{};
        };

        [[nodiscard]] std::expected<ResolvedPartition, AutomaticDispatchExpansionError>
        ResolvePartition(const KernelInvocation& invocation, const DispatchCandidateSet& candidates,
                         GraphPlanningServices services, const GraphResidencyState& baseline,
                         const PartitionedDispatchExecution& partitioned) {
            ResolvedPartition result;
            result.shards.reserve(partitioned.shards.size());
            for (const PartitionShardExecution& shard : partitioned.shards) {
                if (!shard.slice.ValidWithin(invocation.Domain())) {
                    return std::unexpected(Error(AutomaticDispatchExpansionErrorCode::InvalidRequest, candidates.node));
                }
                auto selected = ResolveShardSelection(candidates, shard.execution, shard.slice);
                if (!selected) {
                    return std::unexpected(selected.error());
                }
                if (std::ranges::any_of(result.shards, [&](const DispatchGraphShardSelection& prior) {
                        return prior.placement.device.id == selected->placement.device.id;
                    })) {
                    return std::unexpected(Error(AutomaticDispatchExpansionErrorCode::InvalidRequest, candidates.node));
                }
                auto expected = ProjectDispatchSliceResidency(invocation, candidates.node, shard.slice,
                                                              selected->residency, services, baseline);
                if (!expected) {
                    return std::unexpected(
                        Error(expected.error().code == DispatchCandidateEnumerationErrorCode::OutOfMemory
                                  ? AutomaticDispatchExpansionErrorCode::OutOfMemory
                                  : AutomaticDispatchExpansionErrorCode::CandidateMismatch,
                              candidates.node));
                }
                if (!SameProjection(shard.residency, *expected)) {
                    return std::unexpected(
                        Error(AutomaticDispatchExpansionErrorCode::CandidateMismatch, candidates.node));
                }
                selected->residency = shard.residency;
                result.shards.push_back(std::move(*selected));
            }

            std::vector<RouteTransferBatch> batches;
            batches.reserve(partitioned.shards.size());
            for (uint32_t index = 0; index < partitioned.shards.size(); ++index) {
                const PartitionShardExecution& shard = partitioned.shards[index];
                batches.push_back({.operation = index,
                                   .dispatchNanoseconds = shard.execution.cost.totalNanoseconds,
                                   .routes = shard.residency.inputs});
            }
            const auto schedule = ScheduleRouteTransfers(batches);
            if (!schedule || schedule->transfers != partitioned.transfers ||
                schedule->makespanNanoseconds != partitioned.transferReadyNanoseconds) {
                return std::unexpected(Error(AutomaticDispatchExpansionErrorCode::CandidateMismatch, candidates.node));
            }
            for (uint32_t index = 0; index < partitioned.shards.size(); ++index) {
                const PartitionShardExecution& shard = partitioned.shards[index];
                if (shard.routeReadyNanoseconds != schedule->routeReadyNanoseconds[index] ||
                    shard.inputReadyNanoseconds != schedule->readyNanoseconds[index] ||
                    shard.startNanoseconds != shard.inputReadyNanoseconds ||
                    shard.finishNanoseconds !=
                        Detail::SaturatingAdd(shard.startNanoseconds, shard.execution.cost.totalNanoseconds)) {
                    return std::unexpected(
                        Error(AutomaticDispatchExpansionErrorCode::CandidateMismatch, candidates.node));
                }
            }

            std::vector<ScheduledRouteTransfer> ordered = schedule->transfers;
            std::ranges::sort(ordered, [](const ScheduledRouteTransfer& left, const ScheduledRouteTransfer& right) {
                return std::tuple{left.service,   left.engine, left.beginNanoseconds,
                                  left.operation, left.route,  left.hop} <
                       std::tuple{right.service,   right.engine, right.beginNanoseconds,
                                  right.operation, right.route,  right.hop};
            });
            for (size_t index = 1; index < ordered.size(); ++index) {
                const ScheduledRouteTransfer& predecessor = ordered[index - 1];
                const ScheduledRouteTransfer& successor = ordered[index];
                if (predecessor.service != successor.service || predecessor.engine != successor.engine) {
                    continue;
                }
                if (predecessor.endNanoseconds > successor.beginNanoseconds ||
                    predecessor.operation >= result.shards.size() || successor.operation >= result.shards.size() ||
                    predecessor.route >= result.shards[predecessor.operation].residency.inputs.size() ||
                    successor.route >= result.shards[successor.operation].residency.inputs.size()) {
                    return std::unexpected(
                        Error(AutomaticDispatchExpansionErrorCode::CandidateMismatch, candidates.node));
                }
                result.transferConstraints.push_back(
                    {.predecessor =
                         {.consumer = candidates.node,
                          .buffer = result.shards[predecessor.operation].residency.inputs[predecessor.route].buffer,
                          .dispatch = predecessor.operation,
                          .route = predecessor.route,
                          .hop = predecessor.hop},
                     .successor = {.consumer = candidates.node,
                                   .buffer =
                                       result.shards[successor.operation].residency.inputs[successor.route].buffer,
                                   .dispatch = successor.operation,
                                   .route = successor.route,
                                   .hop = successor.hop}});
            }
            return result;
        }

        [[nodiscard]] AutomaticDispatchExpansionError CommonError(const DispatchGraphPlanningError& error) noexcept {
            AutomaticDispatchExpansionErrorCode code = AutomaticDispatchExpansionErrorCode::CandidateMismatch;
            switch (error.code) {
                case DispatchGraphPlanningErrorCode::ProgramUnavailable:
                    code = AutomaticDispatchExpansionErrorCode::ProgramUnavailable;
                    break;
                case DispatchGraphPlanningErrorCode::CapacityExceeded:
                    code = AutomaticDispatchExpansionErrorCode::CapacityExceeded;
                    break;
                case DispatchGraphPlanningErrorCode::OutOfMemory:
                    code = AutomaticDispatchExpansionErrorCode::OutOfMemory;
                    break;
                case DispatchGraphPlanningErrorCode::InvalidGraph:
                case DispatchGraphPlanningErrorCode::UnsupportedOperation:
                case DispatchGraphPlanningErrorCode::UnsupportedBinding:
                case DispatchGraphPlanningErrorCode::InvalidSelection:
                case DispatchGraphPlanningErrorCode::ResidencyLoweringFailed:
                    break;
            }
            return Error(code, error.node, error.argument, static_cast<uint64_t>(error.code));
        }

    } // namespace

    std::expected<AutomaticDispatchGraphLowering, AutomaticDispatchExpansionError>
    LowerAutomaticDispatchGraph(const ExecutionGraph& graph, GraphPlanningServices services,
                                const DestinationMemoryPolicy& memoryPolicy, const GraphResidencyState& baseline,
                                const DispatchCandidateSet& candidates,
                                const AutomaticDispatchExecution& execution) noexcept {
        if (graph.NodeCount() != 1) {
            return std::unexpected(Error(AutomaticDispatchExpansionErrorCode::UnsupportedGraph));
        }
        constexpr GraphNodeId node{0};
        if (const auto valid = ValidateDispatchGraph(graph); !valid) {
            return std::unexpected(Error(AutomaticDispatchExpansionErrorCode::InvalidRequest, valid.error().node,
                                         valid.error().argument, static_cast<uint64_t>(valid.error().code)));
        }
        const auto* invocation = std::get_if<KernelInvocation>(&graph.Operation(node));
        if (invocation == nullptr || candidates.node != node ||
            candidates.workload.kernelHash != invocation->ProgramHash() ||
            candidates.workload.workItemCount != invocation->Domain().workItemCount) {
            return std::unexpected(Error(AutomaticDispatchExpansionErrorCode::InvalidRequest, node));
        }
        try {
            auto baselineSelection = ResolveShardSelection(candidates, execution.singleDeviceBaseline,
                                                           DispatchSlice::Whole(invocation->Domain()));
            if (!baselineSelection) {
                return std::unexpected(baselineSelection.error());
            }
            if (!execution.partitioned) {
                return LowerBaseline(graph, services, memoryPolicy, baseline, candidates, std::move(*baselineSelection),
                                     AutomaticDispatchFallback::SingleDeviceSelected);
            }
            if (execution.partitioned->shards.size() < 2 || !IsValidStrictCover(*invocation, *execution.partitioned)) {
                return std::unexpected(Error(AutomaticDispatchExpansionErrorCode::InvalidRequest, node));
            }
            auto partition = ResolvePartition(*invocation, candidates, services, baseline, *execution.partitioned);
            if (!partition) {
                return std::unexpected(partition.error());
            }
            const std::array selection{DispatchGraphSelection{
                .node = node,
                .workload = candidates.workload,
                .execution = DispatchGraphPartitionSelection{.axis = execution.partitioned->axis,
                                                             .logicalBindings = baselineSelection->residency.bindings,
                                                             .shards = std::move(partition->shards)}}};
            auto lowered =
                LowerDispatchGraph(graph, services, memoryPolicy, baseline, selection, partition->transferConstraints);
            if (lowered) {
                return AutomaticDispatchGraphLowering{.storage = std::move(*lowered)};
            }
            if (lowered.error().code == DispatchGraphPlanningErrorCode::UnsupportedBinding) {
                return LowerBaseline(graph, services, memoryPolicy, baseline, candidates, std::move(*baselineSelection),
                                     AutomaticDispatchFallback::PartitionFootprintUnavailable);
            }
            if (lowered.error().code == DispatchGraphPlanningErrorCode::ResidencyLoweringFailed) {
                return LowerBaseline(graph, services, memoryPolicy, baseline, candidates, std::move(*baselineSelection),
                                     AutomaticDispatchFallback::PartitionInputResidencyUnavailable);
            }
            return std::unexpected(CommonError(lowered.error()));
        } catch (const std::bad_alloc&) {
            return std::unexpected(Error(AutomaticDispatchExpansionErrorCode::OutOfMemory, node));
        }
    }

} // namespace Sora::Compute

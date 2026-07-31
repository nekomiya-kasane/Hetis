/**
 * @file LinearChainPlanner.cpp
 * @brief Exact additive-cost dynamic programming for linear dispatch graphs.
 * @ingroup Compute
 */

#include <Sora/Compute/Runtime/LinearChainPlanner.h>

#include <Sora/Compute/Runtime/DispatchGraphPlanning.h>

#include <algorithm>
#include <limits>
#include <new>
#include <ranges>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace Sora::Compute {
    namespace {

        struct FrontierState {
            GraphResidencyState residency{};
            uint64_t cumulativeNanoseconds = 0;
            uint32_t predecessor = UINT32_MAX;
            DispatchGraphSelection selected{};
        };

        [[nodiscard]] const DispatchGraphShardSelection& WholeShard(const DispatchGraphSelection& selection) noexcept {
            return std::get<DispatchGraphShardSelection>(selection.execution);
        }

        [[nodiscard]] constexpr LinearChainPlanningError Error(LinearChainPlanningErrorCode code, GraphNodeId node = {},
                                                               KernelArgumentId argument = {},
                                                               uint64_t cause = 0) noexcept {
            return {.code = code, .node = node, .argument = argument, .cause = cause};
        }

        [[nodiscard]] constexpr LinearChainPlanningError CommonError(const DispatchGraphPlanningError& error) noexcept {
            LinearChainPlanningErrorCode code = LinearChainPlanningErrorCode::ResidencyLoweringFailed;
            switch (error.code) {
                case DispatchGraphPlanningErrorCode::InvalidGraph:
                    code = LinearChainPlanningErrorCode::InvalidGraph;
                    break;
                case DispatchGraphPlanningErrorCode::UnsupportedOperation:
                    code = LinearChainPlanningErrorCode::UnsupportedOperation;
                    break;
                case DispatchGraphPlanningErrorCode::UnsupportedBinding:
                    code = LinearChainPlanningErrorCode::UnsupportedBinding;
                    break;
                case DispatchGraphPlanningErrorCode::ProgramUnavailable:
                    code = LinearChainPlanningErrorCode::ProgramUnavailable;
                    break;
                case DispatchGraphPlanningErrorCode::CapacityExceeded:
                    code = LinearChainPlanningErrorCode::CapacityExceeded;
                    break;
                case DispatchGraphPlanningErrorCode::OutOfMemory:
                    code = LinearChainPlanningErrorCode::OutOfMemory;
                    break;
                case DispatchGraphPlanningErrorCode::InvalidSelection:
                case DispatchGraphPlanningErrorCode::ResidencyLoweringFailed:
                    break;
            }
            return Error(code, error.node, error.argument, error.cause);
        }

        [[nodiscard]] std::vector<GraphBufferReadLifetime> CollectReadLifetimes(GraphView graph) {
            std::vector<GraphBufferReadLifetime> lifetimes;
            lifetimes.reserve(graph.accesses.size());
            for (uint32_t node = 0; node < graph.nodes.size(); ++node) {
                for (const ResourceAccess& access : Detail::NodeAccesses(graph, graph.nodes[node])) {
                    if (access.mode == AccessMode::Read) {
                        lifetimes.push_back({.buffer = access.buffer, .lastReader = {node}});
                    }
                }
            }
            std::ranges::sort(lifetimes);
            size_t output = 0;
            for (const GraphBufferReadLifetime& lifetime : lifetimes) {
                if (output != 0 && lifetimes[output - 1].buffer == lifetime.buffer) {
                    lifetimes[output - 1].lastReader = lifetime.lastReader;
                } else {
                    lifetimes[output++] = lifetime;
                }
            }
            lifetimes.resize(output);
            return lifetimes;
        }

        [[nodiscard]] std::expected<void, LinearChainPlanningError>
        ValidateSupportedGraph(const ExecutionGraph& graph) noexcept {
            if (auto valid = ValidateDispatchGraph(graph); !valid) {
                return std::unexpected(CommonError(valid.error()));
            }
            const GraphView view = graph.View();
            if (view.edges.size() + 1 != view.nodes.size()) {
                return std::unexpected(Error(LinearChainPlanningErrorCode::NotLinearChain));
            }
            for (uint32_t node = 1; node < view.nodes.size(); ++node) {
                const GraphEdge expected{{node - 1}, {node}};
                if (std::ranges::find(view.edges, expected) == view.edges.end()) {
                    return std::unexpected(Error(LinearChainPlanningErrorCode::NotLinearChain, {node}, {}, node - 1));
                }
            }
            return {};
        }

        [[nodiscard]] LinearChainPlanningError
        EnumerationError(const DispatchCandidateEnumerationError& error) noexcept {
            LinearChainPlanningErrorCode code = LinearChainPlanningErrorCode::CandidateEnumerationFailed;
            if (error.code == DispatchCandidateEnumerationErrorCode::CapacityExceeded) {
                code = LinearChainPlanningErrorCode::CapacityExceeded;
            } else if (error.code == DispatchCandidateEnumerationErrorCode::OutOfMemory) {
                code = LinearChainPlanningErrorCode::OutOfMemory;
            }
            return Error(code, error.node, error.argument,
                         error.cause != 0 ? error.cause : static_cast<uint64_t>(error.code));
        }

        [[nodiscard]] bool BetterTransition(uint64_t cost, uint32_t predecessor, const PlacementCandidate& placement,
                                            const FrontierState& incumbent) noexcept {
            return cost < incumbent.cumulativeNanoseconds ||
                   (cost == incumbent.cumulativeNanoseconds &&
                    (predecessor < incumbent.predecessor ||
                     (predecessor == incumbent.predecessor &&
                      StableDispatchPlacementLess(placement, WholeShard(incumbent.selected).placement))));
        }

        [[nodiscard]] std::expected<std::vector<std::vector<FrontierState>>, LinearChainPlanningError>
        Solve(const ExecutionGraph& graph, const GraphPlanningContext& context, GraphPlanningServices services,
              const LinearChainPlanningPolicy& policy, GraphResidencyState baseline) {
            const std::vector<GraphBufferReadLifetime> readLifetimes = CollectReadLifetimes(graph.View());
            std::vector<std::vector<FrontierState>> layers;
            layers.reserve(graph.NodeCount());
            std::vector<FrontierState> initial;
            initial.push_back({.residency = std::move(baseline)});

            for (uint32_t node = 0; node < graph.NodeCount(); ++node) {
                const std::vector<FrontierState>& current = node == 0 ? initial : layers.back();
                std::vector<FrontierState> next;
                for (uint32_t predecessor = 0; predecessor < current.size(); ++predecessor) {
                    const FrontierState& prior = current[predecessor];
                    auto candidates =
                        EnumerateDispatchCandidates(graph, {node}, context, services, prior.residency, policy.dispatch);
                    if (!candidates) {
                        return std::unexpected(EnumerationError(candidates.error()));
                    }
                    for (const DispatchPlacementCandidate& placement : candidates->placements) {
                        if (placement.residencyProjection >= candidates->residencies.size()) {
                            return std::unexpected(
                                Error(LinearChainPlanningErrorCode::CandidateEnumerationFailed, {node}));
                        }
                        const auto execution = PlanExecution(
                            candidates->workload, std::span<const PlacementCandidate>{&placement.placement, 1},
                            policy.placement);
                        if (!execution) {
                            if (execution.error() == PlanningError::NoCandidate) {
                                continue;
                            }
                            return std::unexpected(Error(LinearChainPlanningErrorCode::CandidateEnumerationFailed,
                                                         {node}, {}, static_cast<uint64_t>(execution.error())));
                        }
                        if (execution->cost.totalNanoseconds >
                            std::numeric_limits<uint64_t>::max() - prior.cumulativeNanoseconds) {
                            return std::unexpected(Error(LinearChainPlanningErrorCode::CapacityExceeded, {node}));
                        }
                        const uint64_t cumulative = prior.cumulativeNanoseconds + execution->cost.totalNanoseconds;
                        const DispatchResidencyProjection& projection =
                            candidates->residencies[placement.residencyProjection];
                        if (!projection.destination) {
                            return std::unexpected(
                                Error(LinearChainPlanningErrorCode::CandidateEnumerationFailed, {node}));
                        }
                        GraphResidencyState residency = prior.residency;
                        auto committed = CommitDispatchResidencySelection({node}, projection, std::move(residency));
                        if (!committed) {
                            const auto code = committed.error() == GraphResidencyError::OutOfMemory
                                                  ? LinearChainPlanningErrorCode::OutOfMemory
                                                  : LinearChainPlanningErrorCode::ResidencyTransitionFailed;
                            return std::unexpected(Error(code, {node}, {}, static_cast<uint64_t>(committed.error())));
                        }
                        residency = std::move(*committed);
                        residency.DiscardProducedBefore({node});
                        residency.DiscardReadDeadBuffers({node}, readLifetimes);
                        auto found = std::ranges::find_if(next, [&](const FrontierState& state) {
                            return state.residency.SamePlanningState(residency);
                        });
                        if (found != next.end() &&
                            !BetterTransition(cumulative, predecessor, placement.placement, *found)) {
                            continue;
                        }
                        const auto& invocation = std::get<KernelInvocation>(graph.Operation({node}));
                        FrontierState replacement{.residency = std::move(residency),
                                                  .cumulativeNanoseconds = cumulative,
                                                  .predecessor = predecessor,
                                                  .selected = {.node = {node},
                                                               .workload = candidates->workload,
                                                               .execution = DispatchGraphShardSelection{
                                                                   .slice = DispatchSlice::Whole(invocation.Domain()),
                                                                   .placement = placement.placement,
                                                                   .residency = projection,
                                                                   .geometry = candidates->geometry}}};
                        if (found != next.end()) {
                            *found = std::move(replacement);
                        } else {
                            if (next.size() >= policy.maximumFrontierStates) {
                                return std::unexpected(Error(LinearChainPlanningErrorCode::CapacityExceeded, {node}, {},
                                                             policy.maximumFrontierStates));
                            }
                            next.push_back(std::move(replacement));
                        }
                    }
                }
                if (next.empty()) {
                    return std::unexpected(Error(LinearChainPlanningErrorCode::NoCandidate, {node}));
                }
                layers.push_back(std::move(next));
            }
            return layers;
        }

    } // namespace

    std::expected<GraphPlanSelection, LinearChainPlanningError>
    LinearChainPlanner::operator()(const ExecutionGraph& graph, const GraphPlanningContext& context,
                                   GraphPlanningServices services) const noexcept {
        if (!policy_.Valid()) {
            return std::unexpected(Error(LinearChainPlanningErrorCode::InvalidPolicy));
        }
        if (auto supported = ValidateSupportedGraph(graph); !supported) {
            return std::unexpected(supported.error());
        }
        auto baseline = FreezeGraphResidency(graph);
        if (!baseline) {
            const auto code = baseline.error().code == GraphResidencySnapshotErrorCode::OutOfMemory
                                  ? LinearChainPlanningErrorCode::OutOfMemory
                                  : LinearChainPlanningErrorCode::ResidencySnapshotFailed;
            return std::unexpected(Error(code, baseline.error().node, {},
                                         baseline.error().cause != 0 ? baseline.error().cause
                                                                     : static_cast<uint64_t>(baseline.error().code)));
        }
        try {
            auto layers = Solve(graph, context, services, policy_, *baseline);
            if (!layers) {
                return std::unexpected(layers.error());
            }
            const auto& final = layers->back();
            const auto best =
                std::ranges::min_element(final, [](const FrontierState& left, const FrontierState& right) {
                    if (left.cumulativeNanoseconds != right.cumulativeNanoseconds) {
                        return left.cumulativeNanoseconds < right.cumulativeNanoseconds;
                    }
                    return StableDispatchPlacementLess(WholeShard(left.selected).placement,
                                                       WholeShard(right.selected).placement);
                });
            const uint64_t predictedMakespanNanoseconds = best->cumulativeNanoseconds;
            std::vector<DispatchGraphSelection> selected(graph.NodeCount());
            uint32_t frontier = static_cast<uint32_t>(best - final.begin());
            for (size_t layer = layers->size(); layer-- != 0;) {
                const FrontierState& state = (*layers)[layer][frontier];
                selected[layer] = state.selected;
                frontier = state.predecessor;
            }
            auto lowered = LowerDispatchGraph(graph, services, policy_.dispatch.memory, *baseline, selected);
            if (!lowered) {
                return std::unexpected(CommonError(lowered.error()));
            }
            return GraphPlanSelection{.storage = std::move(*lowered),
                                      .predictedMakespanNanoseconds = predictedMakespanNanoseconds};
        } catch (const std::bad_alloc&) {
            return std::unexpected(Error(LinearChainPlanningErrorCode::OutOfMemory));
        }
    }

} // namespace Sora::Compute

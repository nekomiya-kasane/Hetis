/**
 * @file HeftRanking.cpp
 * @brief Feasible-domain execution and communication averages for deterministic HEFT upward ranks.
 * @ingroup Compute
 */

#include "HeftPlanningInternal.h"

#include <Sora/Compute/Runtime/CalibratedTransferTopology.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <new>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace Sora::Compute::HeftDetail {
    namespace {

        struct RankedEdge {
            GraphNodeId successor{};
            uint64_t averageCommunicationNanoseconds = 0;
        };

        [[nodiscard]] std::expected<std::vector<uint64_t>, HeftPlanningError>
        BuildCommunicationVolumes(GraphView graph) {
            std::vector<uint64_t> result(graph.edges.size());
            std::vector<BufferId> buffers;
            buffers.reserve(graph.accesses.size());
            for (const ResourceAccess& access : graph.accesses) {
                buffers.push_back(access.buffer);
            }
            std::ranges::sort(buffers);
            const auto duplicateBuffers = std::ranges::unique(buffers);
            buffers.erase(duplicateBuffers.begin(), duplicateBuffers.end());
            std::vector<GraphNodeId> writers(buffers.size());
            for (uint32_t nodeIndex = 0; nodeIndex < graph.nodes.size(); ++nodeIndex) {
                const GraphNodeId node{nodeIndex};
                const GraphNode& graphNode = graph.nodes[nodeIndex];
                const auto accesses = graph.accesses.subspan(graphNode.firstAccess, graphNode.accessCount);
                for (const ResourceAccess& access : accesses) {
                    const auto buffer = std::ranges::lower_bound(buffers, access.buffer);
                    if (buffer == buffers.end() || *buffer != access.buffer) {
                        return std::unexpected(Error(HeftPlanningErrorCode::RankEstimationFailed, node));
                    }
                    const size_t bufferIndex = static_cast<size_t>(buffer - buffers.begin());
                    if (access.mode == AccessMode::Read) {
                        if (!writers[bufferIndex].Valid()) {
                            continue;
                        }
                        const GraphEdge dependency{writers[bufferIndex], node};
                        const auto edge = std::ranges::lower_bound(graph.edges, dependency, GraphEdgeLess);
                        if (edge == graph.edges.end() || *edge != dependency) {
                            return std::unexpected(Error(HeftPlanningErrorCode::RankEstimationFailed, node, {},
                                                         writers[bufferIndex].index));
                        }
                        const size_t edgeIndex = static_cast<size_t>(edge - graph.edges.begin());
                        auto added =
                            Add(result[edgeIndex], access.region.Size(), node, HeftPlanningErrorCode::CapacityExceeded);
                        if (!added) {
                            return std::unexpected(added.error());
                        }
                        result[edgeIndex] = *added;
                        continue;
                    }
                    writers[bufferIndex] = node;
                }
            }
            return result;
        }

        [[nodiscard]] std::vector<ResidencyOrigin>
        CandidateDestinations(const DispatchResourceCandidateSet& candidates) {
            std::vector<ResidencyOrigin> result;
            result.reserve(candidates.candidates.size());
            for (const DispatchResourceCandidate& candidate : candidates.candidates) {
                if (candidate.destination) {
                    result.push_back({candidate.placement.service, candidate.destination->location});
                }
            }
            std::ranges::sort(result);
            const auto duplicates = std::ranges::unique(result);
            result.erase(duplicates.begin(), duplicates.end());
            return result;
        }

        [[nodiscard]] std::expected<uint64_t, HeftPlanningError>
        AverageExecution(const DispatchResourceCandidateSet& candidates, const PlacementPolicy& policy) noexcept {
            uint64_t sum = 0;
            uint64_t count = 0;
            for (const DispatchResourceCandidate& candidate : candidates.candidates) {
                const auto execution = PlanExecution(
                    candidates.workload, std::span<const PlacementCandidate>{&candidate.placement, 1}, policy);
                if (!execution) {
                    if (execution.error() == PlanningError::NoCandidate) {
                        continue;
                    }
                    return std::unexpected(Error(HeftPlanningErrorCode::RankEstimationFailed, candidates.node, {},
                                                 static_cast<uint64_t>(execution.error())));
                }
                auto duration = ServiceDuration(execution->cost, candidates.node);
                if (!duration) {
                    return std::unexpected(duration.error());
                }
                auto added = Add(sum, *duration, candidates.node, HeftPlanningErrorCode::CapacityExceeded);
                if (!added) {
                    return std::unexpected(added.error());
                }
                sum = *added;
                ++count;
            }
            if (count == 0) {
                return std::unexpected(Error(HeftPlanningErrorCode::NoCandidate, candidates.node));
            }
            return sum / count + (sum % count != 0);
        }

        [[nodiscard]] std::expected<uint64_t, HeftPlanningError>
        AverageCommunication(uint64_t bytes, const DispatchResourceCandidateSet& predecessor,
                             const DispatchResourceCandidateSet& successor, GraphPlanningServices services) {
            if (bytes == 0) {
                return 0;
            }
            const auto region = ByteRegion::Make(0, bytes);
            if (!region) {
                return std::unexpected(Error(HeftPlanningErrorCode::RankEstimationFailed, successor.node));
            }
            auto topology = ProjectCalibratedTransferTopology(services.resources, services.performance, bytes);
            if (!topology) {
                const auto code = topology.error() == CalibratedTransferTopologyError::OutOfMemory
                                      ? HeftPlanningErrorCode::OutOfMemory
                                      : HeftPlanningErrorCode::RankEstimationFailed;
                return std::unexpected(Error(code, successor.node, {}, static_cast<uint64_t>(topology.error())));
            }
            std::vector<ResidencyOrigin> origins = CandidateDestinations(predecessor);
            const std::vector<ResidencyOrigin> destinations = CandidateDestinations(successor);
            uint64_t sum = 0;
            uint64_t count = 0;
            for (const ResidencyOrigin origin : origins) {
                for (const ResidencyOrigin destination : destinations) {
                    uint64_t cost = 0;
                    if (origin != destination) {
                        const std::array oneOrigin{origin};
                        auto path = PlanResidencyPath(
                            oneOrigin,
                            {.service = destination.service, .destination = destination.location, .region = *region},
                            topology->Links());
                        if (!path) {
                            if (path.error() == ResidencyPlanningError::OutOfMemory) {
                                return std::unexpected(Error(HeftPlanningErrorCode::OutOfMemory, successor.node));
                            }
                            if (path.error() == ResidencyPlanningError::NoRoute) {
                                continue;
                            }
                            return std::unexpected(Error(HeftPlanningErrorCode::RankEstimationFailed, successor.node,
                                                         {}, static_cast<uint64_t>(path.error())));
                        }
                        cost = path->estimatedNanoseconds;
                    }
                    auto added = Add(sum, cost, successor.node, HeftPlanningErrorCode::CapacityExceeded);
                    if (!added) {
                        return std::unexpected(added.error());
                    }
                    sum = *added;
                    ++count;
                }
            }
            if (count == 0) {
                return std::unexpected(Error(HeftPlanningErrorCode::RankEstimationFailed, successor.node, {}, bytes));
            }
            return sum / count + (sum % count != 0);
        }

    } // namespace

    std::expected<std::vector<uint64_t>, HeftPlanningError> BuildUpwardRanks(const ExecutionGraph& graph,
                                                                             const GraphPlanningContext& context,
                                                                             GraphPlanningServices services,
                                                                             const HeftPlanningPolicy& policy) {
        try {
            std::vector<DispatchResourceCandidateSet> candidates;
            candidates.reserve(graph.NodeCount());
            std::vector<uint64_t> averageExecution;
            averageExecution.reserve(graph.NodeCount());
            for (uint32_t node = 0; node < graph.NodeCount(); ++node) {
                auto resources = EnumerateDispatchResourceCandidates(graph, {node}, context, services, policy.dispatch);
                if (!resources) {
                    return std::unexpected(EnumerationError(resources.error()));
                }
                auto average = AverageExecution(*resources, policy.placement);
                if (!average) {
                    return std::unexpected(average.error());
                }
                averageExecution.push_back(*average);
                candidates.push_back(std::move(*resources));
            }

            const GraphView view = graph.View();
            auto communicationVolumes = BuildCommunicationVolumes(view);
            if (!communicationVolumes) {
                return std::unexpected(communicationVolumes.error());
            }
            std::vector<std::vector<RankedEdge>> successors(graph.NodeCount());
            for (size_t edgeIndex = 0; edgeIndex < view.edges.size(); ++edgeIndex) {
                const GraphEdge edge = view.edges[edgeIndex];
                auto communication =
                    AverageCommunication((*communicationVolumes)[edgeIndex], candidates[edge.predecessor.index],
                                         candidates[edge.successor.index], services);
                if (!communication) {
                    return std::unexpected(communication.error());
                }
                successors[edge.predecessor.index].push_back({edge.successor, *communication});
            }

            std::vector<uint64_t> ranks(graph.NodeCount());
            for (size_t index = graph.NodeCount(); index-- != 0;) {
                uint64_t downstream = 0;
                for (const RankedEdge edge : successors[index]) {
                    auto path = Add(edge.averageCommunicationNanoseconds, ranks[edge.successor.index],
                                    {uint32_t(index)}, HeftPlanningErrorCode::CapacityExceeded);
                    if (!path) {
                        return std::unexpected(path.error());
                    }
                    downstream = std::max(downstream, *path);
                }
                auto rank = Add(averageExecution[index], downstream, {uint32_t(index)},
                                HeftPlanningErrorCode::CapacityExceeded);
                if (!rank) {
                    return std::unexpected(rank.error());
                }
                ranks[index] = *rank;
            }
            return ranks;
        } catch (const std::bad_alloc&) {
            return std::unexpected(Error(HeftPlanningErrorCode::OutOfMemory));
        }
    }

} // namespace Sora::Compute::HeftDetail

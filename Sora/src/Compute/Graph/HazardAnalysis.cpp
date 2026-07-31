/**
 * @file HazardAnalysis.cpp
 * @brief Sparse interval-frontier dependency derivation.
 * @ingroup Compute
 */

#include <Sora/Compute/Graph/HazardAnalysis.h>

#include <algorithm>
#include <iterator>
#include <map>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace Sora::Compute {
    namespace {

        struct HazardState {
            GraphNodeId writer{};
            std::vector<GraphNodeId> readers{};

            friend bool operator==(const HazardState&, const HazardState&) noexcept = default;
        };

        struct HazardSegment {
            uint64_t end = 0;
            std::shared_ptr<const HazardState> state{};
        };

        void AddPredecessor(std::vector<GraphNodeId>& predecessors, GraphNodeId predecessor, GraphNodeId successor) {
            if (predecessor.Valid() && predecessor != successor) {
                predecessors.push_back(predecessor);
            }
        }

        class BufferHazardIndex {
        public:
            void Apply(ByteRegion region, AccessMode mode, GraphNodeId node, std::vector<GraphNodeId>& predecessors) {
                Cover(region.Offset(), region.End());
                for (auto segment = segments_.lower_bound(region.Offset());
                     segment != segments_.end() && segment->first < region.End(); ++segment) {
                    auto updated = std::make_shared<HazardState>(*segment->second.state);
                    if (mode == AccessMode::Read) {
                        AddPredecessor(predecessors, updated->writer, node);
                        if (updated->readers.empty() || updated->readers.back() != node) {
                            updated->readers.push_back(node);
                        }
                    } else {
                        bool hasPriorReader = false;
                        for (const GraphNodeId reader : updated->readers) {
                            if (reader != node) {
                                AddPredecessor(predecessors, reader, node);
                                hasPriorReader = true;
                            }
                        }
                        if (!hasPriorReader && (updated->readers.empty() || updated->readers.back() != node)) {
                            AddPredecessor(predecessors, updated->writer, node);
                        }
                        updated->writer = node;
                        updated->readers.clear();
                    }
                    segment->second.state = std::move(updated);
                }
                MergeAround(region.Offset(), region.End());
            }

        private:
            using SegmentMap = std::map<uint64_t, HazardSegment>;

            void SplitAt(uint64_t position) {
                auto next = segments_.upper_bound(position);
                if (next == segments_.begin()) {
                    return;
                }
                auto current = std::prev(next);
                if (position <= current->first || position >= current->second.end) {
                    return;
                }
                HazardSegment right{.end = current->second.end, .state = current->second.state};
                current->second.end = position;
                segments_.emplace(position, std::move(right));
            }

            void Cover(uint64_t begin, uint64_t end) {
                SplitAt(begin);
                SplitAt(end);
                uint64_t cursor = begin;
                while (cursor < end) {
                    auto segment = segments_.lower_bound(cursor);
                    if (segment == segments_.end() || segment->first > cursor) {
                        const uint64_t gapEnd = segment == segments_.end() ? end : std::min(end, segment->first);
                        segments_.emplace(cursor,
                                          HazardSegment{.end = gapEnd, .state = std::make_shared<HazardState>()});
                        cursor = gapEnd;
                    } else {
                        cursor = segment->second.end;
                    }
                }
            }

            void MergeAround(uint64_t begin, uint64_t end) {
                auto current = segments_.lower_bound(begin);
                if (current != segments_.begin()) {
                    --current;
                }
                while (current != segments_.end() && current->first <= end) {
                    auto next = std::next(current);
                    if (next == segments_.end()) {
                        break;
                    }
                    if (current->second.end == next->first && *current->second.state == *next->second.state) {
                        current->second.end = next->second.end;
                        segments_.erase(next);
                    } else {
                        current = next;
                    }
                }
            }

            SegmentMap segments_{};
        };

        void ApplyAccess(BufferHazardIndex& hazards, GraphView graph, const ResourceAccess& access, GraphNodeId node,
                         std::vector<GraphNodeId>& predecessors) {
            constexpr size_t projectionCapacity = 64;
            const auto regions = AccessCoverage(graph, access).ProjectCanonicalRegions<projectionCapacity>();
            if (!regions) {
                hazards.Apply(access.region, access.mode, node, predecessors);
                return;
            }
            for (const ByteRegion region : regions->Regions()) {
                hazards.Apply(region, access.mode, node, predecessors);
            }
        }

        [[nodiscard]] std::expected<std::vector<GraphEdge>, GraphError>
        DeriveHazardDependenciesUnchecked(GraphView graph) noexcept {
            uint32_t currentNode = kInvalidGraphNodeIndex;
            try {
                std::map<BufferId, BufferHazardIndex> hazards;
                std::vector<GraphEdge> edges;
                std::vector<GraphNodeId> predecessors;
                for (size_t nodeIndex = 0; nodeIndex < graph.nodes.size(); ++nodeIndex) {
                    currentNode = static_cast<uint32_t>(nodeIndex);
                    const GraphNodeId node{currentNode};
                    predecessors.clear();
                    for (const ResourceAccess& access : Detail::NodeAccesses(graph, graph.nodes[nodeIndex])) {
                        ApplyAccess(hazards[access.buffer], graph, access, node, predecessors);
                    }
                    std::ranges::sort(predecessors, {}, &GraphNodeId::index);
                    const auto uniqueEnd = std::ranges::unique(predecessors).begin();
                    predecessors.erase(uniqueEnd, predecessors.end());
                    if (predecessors.size() > edges.max_size() - edges.size()) {
                        return std::unexpected(GraphError{GraphErrorCode::CapacityExceeded, currentNode});
                    }
                    edges.reserve(edges.size() + predecessors.size());
                    for (const GraphNodeId predecessor : predecessors) {
                        edges.push_back({.predecessor = predecessor, .successor = node});
                    }
                }
                return edges;
            } catch (const std::bad_alloc&) {
                return std::unexpected(GraphError{GraphErrorCode::OutOfMemory, currentNode});
            }
        }

    } // namespace

    std::expected<std::vector<GraphEdge>, GraphError> DeriveHazardDependencies(GraphView graph) noexcept {
        if (auto structure = ValidateGraphStructure(graph); !structure) {
            return std::unexpected(structure.error());
        }
        return DeriveHazardDependenciesUnchecked(graph);
    }

    std::expected<void, GraphError> ValidateSparseGraph(GraphView graph) noexcept {
        if (auto structure = ValidateGraphStructure(graph); !structure) {
            return structure;
        }
        try {
            std::vector<GraphEdge> sortedEdges{graph.edges.begin(), graph.edges.end()};
            std::ranges::sort(sortedEdges, GraphEdgeLess);
            for (size_t index = 1; index < sortedEdges.size(); ++index) {
                if (sortedEdges[index] == sortedEdges[index - 1]) {
                    return std::unexpected(GraphError{GraphErrorCode::DuplicateDependency,
                                                      sortedEdges[index].successor.index,
                                                      sortedEdges[index].predecessor.index});
                }
            }

            auto required = DeriveHazardDependenciesUnchecked(graph);
            if (!required) {
                return std::unexpected(required.error());
            }
            for (const GraphEdge& edge : *required) {
                if (!std::ranges::binary_search(sortedEdges, edge, GraphEdgeLess)) {
                    return std::unexpected(GraphError{GraphErrorCode::MissingEffectDependency, edge.successor.index,
                                                      edge.predecessor.index});
                }
            }
            return {};
        } catch (const std::bad_alloc&) {
            return std::unexpected(GraphError{GraphErrorCode::OutOfMemory});
        }
    }

} // namespace Sora::Compute

/**
 * @file DispatchGraphPlanning.cpp
 * @brief Shared dispatch-DAG validation and physical lowering implementation.
 * @ingroup Compute
 */

#include <Sora/Compute/Runtime/DispatchGraphPlanning.h>

#include <Sora/Compute/Runtime/PhysicalGraphPlanner.h>

#include "DispatchInputAssembly.h"

#include <algorithm>
#include <array>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <queue>
#include <ranges>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace Sora::Compute {
    namespace {

        struct LoweredInput {
            BufferId buffer{};
            PlannedAssemblyPart part{};
            std::optional<GraphLocalResidencySource> materializedGraphLocal{};
        };

        struct LoweredAssembly {
            BufferId buffer{};
            PlannedAllocationId allocation{};
            PlannedQueue queue{};
        };

        struct LastWriter {
            BufferId buffer{};
            GraphNodeId node{};
        };

        struct OutputVersion {
            BufferId buffer{};
            uint64_t base = 0;
        };

        struct QueueTail {
            PlannedQueue queue{};
            PhysicalPlanSegmentId segment{};
        };

        struct LoweredTransfer {
            DispatchTransferRef ref{};
            PhysicalPlanSegmentId segment{};
        };

        [[nodiscard]] constexpr DispatchGraphPlanningError Error(DispatchGraphPlanningErrorCode code,
                                                                 GraphNodeId node = {}, KernelArgumentId argument = {},
                                                                 uint64_t cause = 0) noexcept {
            return {.code = code, .node = node, .argument = argument, .cause = cause};
        }

        [[nodiscard]] bool ArtifactLess(const ArtifactKey& left, const ArtifactKey& right) noexcept {
            return std::ranges::lexicographical_compare(left.Words(), right.Words());
        }

        template<typename Fragment>
        [[nodiscard]] std::expected<void, DispatchGraphPlanningError>
        AppendFragment(PhysicalGraphPlanStorage& storage, Fragment& fragment, GraphNodeId node) {
            if (fragment.storage.allocations.size() > UINT32_MAX - storage.allocations.size() ||
                fragment.storage.segments.size() > UINT32_MAX - storage.segments.size() ||
                fragment.storage.edges.size() > UINT32_MAX - storage.edges.size()) {
                return std::unexpected(Error(DispatchGraphPlanningErrorCode::CapacityExceeded, node));
            }
            storage.allocations.insert(storage.allocations.end(),
                                       std::make_move_iterator(fragment.storage.allocations.begin()),
                                       std::make_move_iterator(fragment.storage.allocations.end()));
            storage.segments.insert(storage.segments.end(), std::make_move_iterator(fragment.storage.segments.begin()),
                                    std::make_move_iterator(fragment.storage.segments.end()));
            storage.edges.insert(storage.edges.end(), std::make_move_iterator(fragment.storage.edges.begin()),
                                 std::make_move_iterator(fragment.storage.edges.end()));
            return {};
        }

        [[nodiscard]] const DispatchMemoryBinding* FindBinding(const DispatchResidencyProjection& projection,
                                                               BufferId buffer) noexcept {
            const auto found = std::ranges::find(projection.bindings, buffer, [](const DispatchMemoryBinding& binding) {
                return binding.buffer.Describe().id;
            });
            return found == projection.bindings.end() ? nullptr : std::addressof(*found);
        }

        [[nodiscard]] ByteCoverageView BindingCoverage(const DispatchMemoryBinding& binding) noexcept {
            return binding.byteFootprint ? ByteCoverageView::Affine(*binding.byteFootprint)
                                         : ByteCoverageView::Interval(binding.region);
        }

        [[nodiscard]] bool SameCoverage(ByteCoverageView left, ByteCoverageView right) noexcept {
            return left.Contains(right) == CoverageContainment::Yes &&
                   left.OccupiedByteCount() == right.OccupiedByteCount();
        }

        [[nodiscard]] std::optional<GraphLocalResidencySource>
        MaterializedGraphLocalReplica(const GraphResidencyRoute& route, GraphNodeId materializer) {
            if (route.AlreadyResident()) {
                return std::nullopt;
            }
            const auto* source = std::get_if<GraphLocalResidencySource>(&route.source);
            return GraphLocalResidencySource{.buffer = route.buffer,
                                             .coverage = route.region,
                                             .byteFootprint = route.byteFootprint,
                                             .version = route.version,
                                             .producer = source != nullptr ? source->producer : std::nullopt,
                                             .replica = {.service = route.destinationService,
                                                         .location = route.destination,
                                                         .materializer = materializer}};
        }

        [[nodiscard]] std::optional<PlannedQueue>
        SelectAssemblyQueue(const PhysicalResourceInventory& resources, PlannedQueue dispatch,
                            Detail::InputAssemblyRequirement requirement) noexcept {
            const PhysicalQueueDescriptor* selected = nullptr;
            for (const PhysicalQueueDescriptor& queue : resources.Queues()) {
                if (queue.service != dispatch.service || queue.backend != dispatch.backend ||
                    queue.device != dispatch.device ||
                    (requirement == Detail::InputAssemblyRequirement::Copy &&
                     !queue.capabilities.Supports(BackendCommandKind::Copy))) {
                    continue;
                }
                if (queue.queue == dispatch.queue) {
                    selected = std::addressof(queue);
                    break;
                }
                if (selected == nullptr) {
                    selected = std::addressof(queue);
                }
            }
            return selected != nullptr ? std::optional<PlannedQueue>{{.service = selected->service,
                                                                      .backend = selected->backend,
                                                                      .device = selected->device,
                                                                      .queue = selected->queue}}
                                       : std::nullopt;
        }

        [[nodiscard]] std::vector<LastWriter> CollectLastWriters(const ExecutionGraph& graph) {
            const GraphView view = graph.View();
            std::vector<LastWriter> writers;
            writers.reserve(view.accesses.size());
            for (uint32_t node = 0; node < view.nodes.size(); ++node) {
                const GraphNode& candidate = view.nodes[node];
                const auto accesses = view.accesses.subspan(candidate.firstAccess, candidate.accessCount);
                for (const ResourceAccess& access : accesses) {
                    if (access.mode != AccessMode::Read) {
                        writers.push_back({access.buffer, {node}});
                    }
                }
            }
            std::ranges::sort(writers, [](const LastWriter& left, const LastWriter& right) {
                return left.buffer < right.buffer || (left.buffer == right.buffer && right.node < left.node);
            });
            const auto duplicates =
                std::ranges::unique(writers, {}, [](const LastWriter& writer) { return writer.buffer; });
            writers.erase(duplicates.begin(), duplicates.end());
            return writers;
        }

        [[nodiscard]] bool IsLastWriter(std::span<const LastWriter> writers, GraphNodeId node,
                                        BufferId buffer) noexcept {
            const auto found =
                std::ranges::lower_bound(writers, buffer, {}, [](const LastWriter& writer) { return writer.buffer; });
            return found != writers.end() && found->buffer == buffer && found->node == node;
        }

        [[nodiscard]] std::expected<void, DispatchGraphPlanningError> AddDependency(PhysicalGraphPlanStorage& storage,
                                                                                    PhysicalPlanSegmentId predecessor,
                                                                                    PhysicalPlanSegmentId successor,
                                                                                    GraphNodeId node) {
            if (predecessor.Valid() && predecessor != successor) {
                if (storage.edges.size() >= UINT32_MAX) {
                    return std::unexpected(Error(DispatchGraphPlanningErrorCode::CapacityExceeded, node));
                }
                storage.edges.push_back({predecessor, successor});
            }
            return {};
        }

        [[nodiscard]] std::expected<void, DispatchGraphPlanningError>
        ScheduleQueueSegment(PhysicalGraphPlanStorage& storage, std::vector<QueueTail>& queues, PlannedQueue queue,
                             PhysicalPlanSegmentId segment, GraphNodeId node) {
            auto tail = std::ranges::find(queues, queue, &QueueTail::queue);
            if (tail != queues.end()) {
                if (auto added = AddDependency(storage, tail->segment, segment, node); !added) {
                    return added;
                }
                tail->segment = segment;
            } else {
                queues.push_back({queue, segment});
            }
            return {};
        }

        [[nodiscard]] constexpr bool PhysicalPlanEdgeLess(const PhysicalPlanEdge& left,
                                                          const PhysicalPlanEdge& right) noexcept {
            return std::tuple{left.successor, left.predecessor} < std::tuple{right.successor, right.predecessor};
        }

        void RemapStorageRef(PlannedStorageRef& storage, std::span<const uint32_t> oldToNew) noexcept {
            if (auto* produced = std::get_if<PlannedAllocationRef>(&storage);
                produced != nullptr && produced->readyAfter.Valid()) {
                produced->readyAfter = {oldToNew[produced->readyAfter.index]};
            }
        }

        void RemapSegment(PhysicalPlanSegment& segment, std::span<const uint32_t> oldToNew) noexcept {
            std::visit(
                [&]<typename Segment>(Segment& value) {
                    if constexpr (std::same_as<Segment, PlannedDispatch>) {
                        for (PlannedDispatchBuffer& buffer : value.bufferStorage) {
                            if (buffer.input) {
                                RemapStorageRef(*buffer.input, oldToNew);
                            }
                        }
                    } else if constexpr (std::same_as<Segment, PlannedAssembly>) {
                        for (PlannedAssemblyPart& part : value.parts) {
                            if (part.source) {
                                RemapStorageRef(*part.source, oldToNew);
                            }
                            if (part.transferProducer.Valid()) {
                                part.transferProducer = {oldToNew[part.transferProducer.index]};
                            }
                        }
                    } else if constexpr (std::same_as<Segment, PlannedCopy> || std::same_as<Segment, PlannedTransfer>) {
                        RemapStorageRef(value.source, oldToNew);
                    }
                },
                segment);
        }

        [[nodiscard]] std::expected<void, DispatchGraphPlanningError>
        StableTopologicalOrder(PhysicalGraphPlanStorage& storage) {
            std::ranges::sort(storage.edges, PhysicalPlanEdgeLess);
            const auto unique = std::ranges::unique(storage.edges);
            storage.edges.erase(unique.begin(), unique.end());
            if (storage.segments.size() > UINT32_MAX) {
                return std::unexpected(Error(DispatchGraphPlanningErrorCode::CapacityExceeded));
            }

            bool identity = true;
            for (const PhysicalPlanEdge edge : storage.edges) {
                if (!edge.predecessor.Valid() || !edge.successor.Valid() ||
                    edge.predecessor.index >= storage.segments.size() ||
                    edge.successor.index >= storage.segments.size() || edge.predecessor == edge.successor) {
                    return std::unexpected(Error(DispatchGraphPlanningErrorCode::InvalidSelection));
                }
                identity = identity && edge.predecessor < edge.successor;
            }
            if (identity) {
                return {};
            }

            std::vector<std::vector<uint32_t>> successors(storage.segments.size());
            std::vector<uint32_t> indegrees(storage.segments.size());
            for (const PhysicalPlanEdge edge : storage.edges) {
                if (indegrees[edge.successor.index] == UINT32_MAX) {
                    return std::unexpected(Error(DispatchGraphPlanningErrorCode::InvalidSelection));
                }
                successors[edge.predecessor.index].push_back(edge.successor.index);
                ++indegrees[edge.successor.index];
            }

            std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<>> ready;
            for (uint32_t segment = 0; segment < storage.segments.size(); ++segment) {
                if (indegrees[segment] == 0) {
                    ready.push(segment);
                }
            }
            std::vector<uint32_t> order;
            order.reserve(storage.segments.size());
            while (!ready.empty()) {
                const uint32_t segment = ready.top();
                ready.pop();
                order.push_back(segment);
                for (const uint32_t successor : successors[segment]) {
                    if (--indegrees[successor] == 0) {
                        ready.push(successor);
                    }
                }
            }
            if (order.size() != storage.segments.size()) {
                return std::unexpected(Error(DispatchGraphPlanningErrorCode::InvalidSelection));
            }

            std::vector<uint32_t> oldToNew(order.size());
            std::vector<PhysicalPlanSegment> reordered;
            reordered.reserve(order.size());
            for (uint32_t newIndex = 0; newIndex < order.size(); ++newIndex) {
                oldToNew[order[newIndex]] = newIndex;
                reordered.push_back(std::move(storage.segments[order[newIndex]]));
            }
            storage.segments = std::move(reordered);
            for (PlannedAllocation& allocation : storage.allocations) {
                if (auto* alias = std::get_if<PlannedCoherentAlias>(&allocation.materialization)) {
                    RemapStorageRef(alias->source, oldToNew);
                }
                if (allocation.publication && allocation.publication->producer.Valid()) {
                    allocation.publication->producer = {oldToNew[allocation.publication->producer.index]};
                }
            }
            for (PhysicalPlanSegment& segment : storage.segments) {
                RemapSegment(segment, oldToNew);
            }
            for (PhysicalPlanEdge& edge : storage.edges) {
                edge.predecessor = {oldToNew[edge.predecessor.index]};
                edge.successor = {oldToNew[edge.successor.index]};
            }
            std::ranges::sort(storage.edges, PhysicalPlanEdgeLess);
            return {};
        }

        struct DispatchExecutionView {
            std::span<const DispatchGraphShardSelection> shards{};
            std::span<const DispatchMemoryBinding> logicalBindings{};
            uint8_t axis = 0;
            bool partitioned = false;
        };

        [[nodiscard]] DispatchExecutionView ExecutionView(const DispatchGraphSelection& selection) noexcept {
            if (const auto* whole = std::get_if<DispatchGraphShardSelection>(&selection.execution)) {
                return {.shards = {whole, 1}, .logicalBindings = whole->residency.bindings};
            }
            const auto& partition = std::get<DispatchGraphPartitionSelection>(selection.execution);
            return {.shards = partition.shards,
                    .logicalBindings = partition.logicalBindings,
                    .axis = partition.axis,
                    .partitioned = true};
        }

        [[nodiscard]] bool BindingMatchesView(const DispatchMemoryBinding& binding, KernelArgumentId argument,
                                              AccessMode access, const BufferView& view,
                                              uint64_t minimumAlignment) noexcept {
            const AffineByteFootprint footprint = view.Footprint();
            const bool affineCoverage = !footprint.IsContiguous();
            return binding.argument == argument && binding.access == access &&
                   binding.buffer.Describe() == view.LogicalBuffer().Describe() &&
                   binding.buffer.Weak().SharesOwnership(view.LogicalBuffer().Weak()) &&
                   binding.region == view.Region() && binding.alignment == minimumAlignment &&
                   (affineCoverage ? binding.byteFootprint == std::optional{footprint} : !binding.byteFootprint);
        }

        [[nodiscard]] bool RoutesExactlyCoverReadableBindings(const DispatchResidencyProjection& projection) noexcept {
            constexpr size_t coverageCapacity = 256;
            if (projection.inputs.size() > coverageCapacity) {
                return false;
            }
            std::array<ByteCoverageView, coverageCapacity> coverages{};
            size_t consumedRoutes = 0;
            for (const DispatchMemoryBinding& binding : projection.bindings) {
                if (binding.access != AccessMode::Read) {
                    continue;
                }
                size_t count = 0;
                for (const GraphResidencyRoute& route : projection.inputs) {
                    if (route.buffer == binding.buffer.Describe().id) {
                        coverages[count++] = route.Coverage();
                    }
                }
                const bool exact =
                    count == 1 ? SameCoverage(BindingCoverage(binding), coverages.front())
                               : count != 0 && ExactCoverageUnion<coverageCapacity>(BindingCoverage(binding),
                                                                                    {coverages.data(), count}) ==
                                                   CoverageEquivalence::Equal;
                if (!exact) {
                    return false;
                }
                consumedRoutes += count;
            }
            return consumedRoutes == projection.inputs.size();
        }

        [[nodiscard]] bool SelectionBindingsMatchInvocation(const KernelInvocation& invocation,
                                                            DispatchExecutionView execution) noexcept {
            const auto signature = invocation.Signature();
            const auto invocationBindings = invocation.Bindings();
            size_t bufferIndex = 0;
            for (uint32_t argument = 0; argument < signature.size(); ++argument) {
                if (signature[argument].kind != ParameterKind::Buffer) {
                    continue;
                }
                if (bufferIndex >= execution.logicalBindings.size()) {
                    return false;
                }
                const auto* wholeView = std::get_if<BufferView>(&invocationBindings[argument]);
                if (wholeView == nullptr || !*wholeView) {
                    return false;
                }
                const uint64_t alignment =
                    std::max<uint64_t>(wholeView->BufferDescriptor().alignment, signature[argument].minimumAlignment);
                if (!BindingMatchesView(execution.logicalBindings[bufferIndex], {argument}, signature[argument].access,
                                        *wholeView, alignment)) {
                    return false;
                }
                for (const DispatchGraphShardSelection& shard : execution.shards) {
                    if (bufferIndex >= shard.residency.bindings.size()) {
                        return false;
                    }
                    const std::span<const uint64_t> origin{shard.slice.origin.data(), shard.slice.rank};
                    const std::span<const uint64_t> extents{shard.slice.extents.data(), shard.slice.rank};
                    const auto shardView = wholeView->SubView(origin, extents);
                    if (!shardView || !BindingMatchesView(shard.residency.bindings[bufferIndex], {argument},
                                                          signature[argument].access, *shardView, alignment)) {
                        return false;
                    }
                }
                ++bufferIndex;
            }
            return bufferIndex == execution.logicalBindings.size() &&
                   std::ranges::all_of(execution.shards,
                                       [&](const DispatchGraphShardSelection& shard) {
                                           return shard.residency.bindings.size() == bufferIndex;
                                       }) &&
                   std::ranges::all_of(execution.shards, [](const DispatchGraphShardSelection& shard) {
                       return RoutesExactlyCoverReadableBindings(shard.residency);
                   });
        }

        [[nodiscard]] PlannedQueue QueueFor(const DispatchGraphShardSelection& selection) noexcept {
            return {.service = selection.placement.service,
                    .backend = selection.placement.device.backend,
                    .device = selection.placement.device.id,
                    .queue = selection.placement.queue};
        }

        [[nodiscard]] std::expected<std::vector<std::vector<GraphNodeId>>, DispatchGraphPlanningError>
        CollectPredecessors(GraphView graph) {
            try {
                std::vector<std::vector<GraphNodeId>> result(graph.nodes.size());
                for (const GraphEdge edge : graph.edges) {
                    result[edge.successor.index].push_back(edge.predecessor);
                }
                return result;
            } catch (const std::bad_alloc&) {
                return std::unexpected(Error(DispatchGraphPlanningErrorCode::OutOfMemory));
            }
        }

    } // namespace

    bool StableDispatchPlacementLess(const PlacementCandidate& left, const PlacementCandidate& right) noexcept {
        if (left.service != right.service) {
            return left.service < right.service;
        }
        const ArtifactKey leftExecutable = MakeArtifactKey(
            ExecutableKey{.lowered = {1, 1}, .target = left.target, .specialization = left.specialization});
        const ArtifactKey rightExecutable = MakeArtifactKey(
            ExecutableKey{.lowered = {1, 1}, .target = right.target, .specialization = right.specialization});
        if (leftExecutable != rightExecutable) {
            return ArtifactLess(leftExecutable, rightExecutable);
        }
        if (left.queue != right.queue) {
            return left.queue < right.queue;
        }
        if (left.pipeline.has_value() != right.pipeline.has_value()) {
            return !left.pipeline;
        }
        return left.pipeline && left.pipeline->key != right.pipeline->key &&
               ArtifactLess(MakeArtifactKey(left.pipeline->key), MakeArtifactKey(right.pipeline->key));
    }

    std::expected<void, DispatchGraphPlanningError>
    CanonicalizePhysicalGraphPlanStorage(PhysicalGraphPlanStorage& storage) noexcept {
        try {
            return StableTopologicalOrder(storage);
        } catch (const std::bad_alloc&) {
            return std::unexpected(Error(DispatchGraphPlanningErrorCode::OutOfMemory));
        }
    }

    std::expected<void, DispatchGraphPlanningError> ValidateDispatchGraph(const ExecutionGraph& graph) noexcept {
        const GraphView view = graph.View();
        if (const auto valid = ValidateGraph(view); !valid) {
            return std::unexpected(Error(DispatchGraphPlanningErrorCode::InvalidGraph, GraphNodeId{valid.error().node},
                                         {}, static_cast<uint64_t>(valid.error().code)));
        }
        for (uint32_t node = 0; node < graph.NodeCount(); ++node) {
            const auto* invocation = std::get_if<KernelInvocation>(&graph.Operation({node}));
            if (invocation == nullptr) {
                return std::unexpected(Error(DispatchGraphPlanningErrorCode::UnsupportedOperation, {node}));
            }
            const auto signature = invocation->Signature();
            const auto bindings = invocation->Bindings();
            if (signature.size() != bindings.size()) {
                return std::unexpected(Error(DispatchGraphPlanningErrorCode::UnsupportedBinding, {node}));
            }
            size_t writeCount = 0;
            for (uint32_t argument = 0; argument < signature.size(); ++argument) {
                if (signature[argument].kind != ParameterKind::Buffer) {
                    continue;
                }
                const auto* viewBinding = std::get_if<BufferView>(&bindings[argument]);
                if (viewBinding == nullptr || !*viewBinding || signature[argument].access == AccessMode::ReadWrite) {
                    return std::unexpected(
                        Error(DispatchGraphPlanningErrorCode::UnsupportedBinding, {node}, {argument}));
                }
                const BufferDesc desc = viewBinding->BufferDescriptor();
                const AffineByteFootprint footprint = viewBinding->Footprint();
                bool duplicate = false;
                for (uint32_t prior = 0; prior < argument; ++prior) {
                    const auto* priorView = std::get_if<BufferView>(&bindings[prior]);
                    duplicate = duplicate ||
                                (priorView != nullptr && *priorView && priorView->BufferDescriptor().id == desc.id);
                }
                if (viewBinding->LogicalBuffer().LogicalCoverage().Contains(ByteCoverageView::Affine(footprint)) !=
                        CoverageContainment::Yes ||
                    duplicate) {
                    return std::unexpected(
                        Error(DispatchGraphPlanningErrorCode::UnsupportedBinding, {node}, {argument}));
                }
                writeCount += signature[argument].access == AccessMode::Write;
            }
            if (writeCount == 0) {
                return std::unexpected(Error(DispatchGraphPlanningErrorCode::UnsupportedBinding, {node}));
            }
        }
        return {};
    }

    std::expected<PhysicalGraphPlanStorage, DispatchGraphPlanningError>
    LowerDispatchGraph(const ExecutionGraph& graph, GraphPlanningServices services,
                       const DestinationMemoryPolicy& memoryPolicy, const GraphResidencyState& baseline,
                       std::span<const DispatchGraphSelection> selections,
                       std::span<const DispatchTransferConstraint> transferConstraints) noexcept {
        if (auto valid = ValidateDispatchGraph(graph); !valid) {
            return std::unexpected(valid.error());
        }
        if (selections.size() != graph.NodeCount() || selections.size() > UINT32_MAX) {
            return std::unexpected(Error(DispatchGraphPlanningErrorCode::InvalidSelection));
        }
        try {
            auto predecessors = CollectPredecessors(graph.View());
            if (!predecessors) {
                return std::unexpected(predecessors.error());
            }
            PhysicalGraphPlanStorage storage;
            GraphLocalStorageMap graphLocal;
            GraphResidencyState residency = baseline;
            std::vector<std::vector<PhysicalPlanSegmentId>> logicalCompletions(graph.NodeCount());
            std::vector<QueueTail> queues;
            queues.reserve(services.resources.Queues().size());
            std::vector<LoweredTransfer> loweredTransfers;
            const std::vector<LastWriter> lastWriters = CollectLastWriters(graph);

            for (const DispatchGraphSelection& choice : selections) {
                const GraphNodeId node = choice.node;
                if (!node.Valid() || node.index >= graph.NodeCount() || !logicalCompletions[node.index].empty()) {
                    return std::unexpected(Error(DispatchGraphPlanningErrorCode::InvalidSelection, node));
                }
                for (const GraphNodeId predecessor : (*predecessors)[node.index]) {
                    if (logicalCompletions[predecessor.index].empty()) {
                        return std::unexpected(
                            Error(DispatchGraphPlanningErrorCode::InvalidSelection, node, {}, predecessor.index));
                    }
                }
                const auto* invocation = std::get_if<KernelInvocation>(&graph.Operation(node));
                const DispatchExecutionView execution = ExecutionView(choice);
                if (invocation == nullptr || choice.workload.kernelHash != invocation->ProgramHash() ||
                    choice.workload.workItemCount != invocation->Domain().workItemCount || execution.shards.empty() ||
                    execution.logicalBindings.empty() || execution.shards.size() > UINT32_MAX ||
                    execution.logicalBindings.size() != execution.shards.front().residency.bindings.size()) {
                    return std::unexpected(Error(DispatchGraphPlanningErrorCode::InvalidSelection, node));
                }
                if (!SelectionBindingsMatchInvocation(*invocation, execution)) {
                    return std::unexpected(Error(DispatchGraphPlanningErrorCode::InvalidSelection, node));
                }
                std::vector<OutputVersion> outputVersions;
                for (const DispatchMemoryBinding& binding : execution.logicalBindings) {
                    if (binding.access == AccessMode::Read) {
                        continue;
                    }
                    const auto baseVersion = residency.BaseVersion(binding.buffer.Describe().id);
                    if (!baseVersion || *baseVersion == UINT64_MAX) {
                        return std::unexpected(
                            Error(DispatchGraphPlanningErrorCode::ResidencyLoweringFailed, node, binding.argument));
                    }
                    outputVersions.push_back({binding.buffer.Describe().id, *baseVersion});
                }
                if (execution.partitioned) {
                    if (execution.shards.size() < 2) {
                        return std::unexpected(Error(DispatchGraphPlanningErrorCode::InvalidSelection, node));
                    }
                    std::vector<DispatchSlice> slices;
                    slices.reserve(execution.shards.size());
                    std::vector<DispatchResidencyProjection> projections;
                    projections.reserve(execution.shards.size());
                    for (const DispatchGraphShardSelection& shard : execution.shards) {
                        slices.push_back(shard.slice);
                        projections.push_back(shard.residency);
                    }
                    if (!ElementwisePartitionPlan::ValidateStrictCover(invocation->Domain(), invocation->Partition(),
                                                                       execution.axis, slices) ||
                        !CommitPartitionedDispatchResidencySelection(node, execution.logicalBindings, projections,
                                                                     residency)) {
                        return std::unexpected(Error(DispatchGraphPlanningErrorCode::InvalidSelection, node));
                    }
                } else if (!execution.shards.front().slice.IsWhole(invocation->Domain()) ||
                           !CommitDispatchResidencySelection(node, execution.shards.front().residency, residency)) {
                    return std::unexpected(Error(DispatchGraphPlanningErrorCode::InvalidSelection, node));
                }

                for (uint32_t dispatchIndex = 0; dispatchIndex < execution.shards.size(); ++dispatchIndex) {
                    const DispatchGraphShardSelection& shard = execution.shards[dispatchIndex];
                    if (shard.placement.service != shard.residency.service ||
                        shard.placement.device.id != shard.residency.device || !shard.residency.destination ||
                        shard.residency.inputs.size() > UINT32_MAX) {
                        return std::unexpected(Error(DispatchGraphPlanningErrorCode::InvalidSelection, node));
                    }
                    const PlannedQueue dispatchQueue = QueueFor(shard);
                    const Detail::InputAssemblyRequirement assemblyRequirement =
                        Detail::ClassifyInputAssembly(shard.residency);
                    const auto assemblyQueue =
                        SelectAssemblyQueue(services.resources, dispatchQueue, assemblyRequirement);
                    if (assemblyRequirement != Detail::InputAssemblyRequirement::None && !assemblyQueue) {
                        return std::unexpected(Error(DispatchGraphPlanningErrorCode::ResidencyLoweringFailed, node));
                    }
                    for (uint32_t routeIndex = 0; routeIndex < shard.residency.inputs.size(); ++routeIndex) {
                        const GraphResidencyRoute& route = shard.residency.inputs[routeIndex];
                        if (const auto* source = std::get_if<GraphLocalResidencySource>(&route.source);
                            source != nullptr && (!source->replica.materializer.Valid() ||
                                                  source->replica.materializer.index >= logicalCompletions.size() ||
                                                  logicalCompletions[source->replica.materializer.index].empty())) {
                            return std::unexpected(Error(DispatchGraphPlanningErrorCode::InvalidSelection, node, {},
                                                         source->replica.materializer.index));
                        }
                    }

                    std::vector<LoweredAssembly> assemblies;
                    for (const DispatchMemoryBinding& binding : shard.residency.bindings) {
                        const BufferId buffer = binding.buffer.Describe().id;
                        const auto routeCount =
                            std::ranges::count(shard.residency.inputs, buffer, &GraphResidencyRoute::buffer);
                        if (binding.access != AccessMode::Read || routeCount <= 1) {
                            continue;
                        }
                        if (storage.allocations.size() >= UINT32_MAX) {
                            return std::unexpected(Error(DispatchGraphPlanningErrorCode::CapacityExceeded, node));
                        }
                        const PlannedAllocationId allocationId{static_cast<uint32_t>(storage.allocations.size())};
                        storage.allocations.push_back({
                            .id = allocationId,
                            .purpose = PlannedAllocationPurpose::DispatchInputAssembly,
                            .buffer = binding.buffer,
                            .service = shard.residency.service,
                            .location = shard.residency.destination->location,
                            .bufferRegion = binding.region,
                            .byteFootprint = binding.byteFootprint,
                            .version = 0,
                            .materialization = PlannedAllocate{.requiredProperties = memoryPolicy.requiredProperties,
                                                               .preferredProperties = memoryPolicy.preferredProperties,
                                                               .alignment = binding.alignment},
                        });
                        assemblies.push_back({.buffer = buffer, .allocation = allocationId, .queue = *assemblyQueue});
                    }

                    std::vector<LoweredInput> inputs;
                    inputs.reserve(shard.residency.inputs.size());
                    for (uint32_t routeIndex = 0; routeIndex < shard.residency.inputs.size(); ++routeIndex) {
                        const GraphResidencyRoute& route = shard.residency.inputs[routeIndex];
                        const DispatchMemoryBinding* binding = FindBinding(shard.residency, route.buffer);
                        if (binding == nullptr || binding->access != AccessMode::Read) {
                            return std::unexpected(
                                Error(DispatchGraphPlanningErrorCode::ResidencyLoweringFailed, node));
                        }
                        const auto assembly = std::ranges::find(assemblies, route.buffer, &LoweredAssembly::buffer);
                        const bool directFill = assembly != assemblies.end() && !route.hops.empty() &&
                                                route.hops.back().kind != TransferKind::CoherentAlias;
                        if (directFill) {
                            auto fragment = PlanPhysicalResidencyPrefill(
                                binding->buffer, node, route, storage, graphLocal, assembly->allocation,
                                {.requiredProperties = memoryPolicy.requiredProperties,
                                 .preferredProperties = memoryPolicy.preferredProperties,
                                 .alignment = binding->alignment});
                            if (!fragment) {
                                const auto code = fragment.error() == PhysicalGraphPlanningError::OutOfMemory
                                                      ? DispatchGraphPlanningErrorCode::OutOfMemory
                                                  : fragment.error() == PhysicalGraphPlanningError::CapacityExceeded
                                                      ? DispatchGraphPlanningErrorCode::CapacityExceeded
                                                      : DispatchGraphPlanningErrorCode::ResidencyLoweringFailed;
                                return std::unexpected(Error(code, node, {}, static_cast<uint64_t>(fragment.error())));
                            }
                            if (!transferConstraints.empty()) {
                                for (const PhysicalRouteTransfer transfer : fragment->transfers) {
                                    loweredTransfers.push_back({{.consumer = node,
                                                                 .buffer = route.buffer,
                                                                 .dispatch = dispatchIndex,
                                                                 .route = routeIndex,
                                                                 .hop = transfer.hop},
                                                                transfer.segment});
                                }
                            }
                            const PhysicalPlanSegmentId producer = fragment->producer;
                            if (auto appended = AppendFragment(storage, *fragment, node); !appended) {
                                return std::unexpected(appended.error());
                            }
                            inputs.push_back({.buffer = route.buffer,
                                              .part = {.transferProducer = producer,
                                                       .region = route.region,
                                                       .byteFootprint = route.byteFootprint},
                                              .materializedGraphLocal = MaterializedGraphLocalReplica(route, node)});
                            continue;
                        }
                        auto fragment = PlanPhysicalResidency(binding->buffer, node, route, storage, graphLocal,
                                                              {.requiredProperties = memoryPolicy.requiredProperties,
                                                               .preferredProperties = memoryPolicy.preferredProperties,
                                                               .alignment = binding->alignment});
                        if (!fragment) {
                            const auto code = fragment.error() == PhysicalGraphPlanningError::OutOfMemory
                                                  ? DispatchGraphPlanningErrorCode::OutOfMemory
                                              : fragment.error() == PhysicalGraphPlanningError::CapacityExceeded
                                                  ? DispatchGraphPlanningErrorCode::CapacityExceeded
                                                  : DispatchGraphPlanningErrorCode::ResidencyLoweringFailed;
                            return std::unexpected(Error(code, node, {}, static_cast<uint64_t>(fragment.error())));
                        }
                        if (!transferConstraints.empty()) {
                            for (const PhysicalRouteTransfer transfer : fragment->transfers) {
                                loweredTransfers.push_back({{.consumer = node,
                                                             .buffer = route.buffer,
                                                             .dispatch = dispatchIndex,
                                                             .route = routeIndex,
                                                             .hop = transfer.hop},
                                                            transfer.segment});
                            }
                        }
                        PlannedStorageRef destination = fragment->destination;
                        if (auto appended = AppendFragment(storage, *fragment, node); !appended) {
                            return std::unexpected(appended.error());
                        }
                        if (auto symbolic = MaterializedGraphLocalReplica(route, node)) {
                            const auto* physical = std::get_if<PlannedAllocationRef>(&destination);
                            if (physical == nullptr) {
                                return std::unexpected(
                                    Error(DispatchGraphPlanningErrorCode::ResidencyLoweringFailed, node));
                            }
                            if (auto bound = graphLocal.Bind({std::move(*symbolic), *physical}); !bound) {
                                return std::unexpected(Error(DispatchGraphPlanningErrorCode::ResidencyLoweringFailed,
                                                             node, {}, static_cast<uint64_t>(bound.error())));
                            }
                        }
                        inputs.push_back({.buffer = route.buffer,
                                          .part = {.source = std::move(destination),
                                                   .region = route.region,
                                                   .byteFootprint = route.byteFootprint}});
                    }

                    const size_t assemblyCount = assemblies.size();
                    if (storage.segments.size() >= UINT32_MAX ||
                        assemblyCount > UINT32_MAX - storage.segments.size() - 1 ||
                        storage.allocations.size() >= UINT32_MAX) {
                        return std::unexpected(Error(DispatchGraphPlanningErrorCode::CapacityExceeded, node));
                    }
                    const PhysicalPlanSegmentId dispatchId{
                        static_cast<uint32_t>(storage.segments.size() + assemblyCount)};
                    std::vector<PlannedDispatchBuffer> bufferStorage;
                    bufferStorage.reserve(shard.residency.bindings.size());
                    for (const DispatchMemoryBinding& binding : shard.residency.bindings) {
                        if (binding.access == AccessMode::Read) {
                            std::vector<PlannedAssemblyPart> parts;
                            for (const LoweredInput& input : inputs) {
                                if (input.buffer == binding.buffer.Describe().id) {
                                    parts.push_back(input.part);
                                }
                            }
                            if (parts.empty()) {
                                return std::unexpected(
                                    Error(DispatchGraphPlanningErrorCode::ResidencyLoweringFailed, node));
                            }
                            std::ranges::sort(parts,
                                              [](const PlannedAssemblyPart& left, const PlannedAssemblyPart& right) {
                                                  return std::tie(left.region, left.byteFootprint) <
                                                         std::tie(right.region, right.byteFootprint);
                                              });
                            PlannedStorageRef input;
                            if (parts.size() == 1) {
                                if (!parts.front().source || parts.front().transferProducer.Valid()) {
                                    return std::unexpected(
                                        Error(DispatchGraphPlanningErrorCode::ResidencyLoweringFailed, node));
                                }
                                input = std::move(*parts.front().source);
                            } else {
                                constexpr size_t coverageCapacity = 256;
                                if (parts.size() > coverageCapacity) {
                                    return std::unexpected(
                                        Error(DispatchGraphPlanningErrorCode::CapacityExceeded, node));
                                }
                                std::array<ByteCoverageView, coverageCapacity> coverages{};
                                for (size_t index = 0; index < parts.size(); ++index) {
                                    coverages[index] = parts[index].Coverage();
                                }
                                const CoverageEquivalence cover = ExactCoverageUnion<coverageCapacity>(
                                    BindingCoverage(binding), {coverages.data(), parts.size()});
                                if (cover != CoverageEquivalence::Equal) {
                                    return std::unexpected(
                                        Error(cover == CoverageEquivalence::Unknown
                                                  ? DispatchGraphPlanningErrorCode::CapacityExceeded
                                                  : DispatchGraphPlanningErrorCode::ResidencyLoweringFailed,
                                              node));
                                }
                                const auto assembly = std::ranges::find(assemblies, binding.buffer.Describe().id,
                                                                        &LoweredAssembly::buffer);
                                if (assembly == assemblies.end()) {
                                    return std::unexpected(
                                        Error(DispatchGraphPlanningErrorCode::ResidencyLoweringFailed, node));
                                }
                                const PhysicalPlanSegmentId assemblyId{static_cast<uint32_t>(storage.segments.size())};
                                for (const PlannedAssemblyPart& part : parts) {
                                    PhysicalPlanSegmentId producer = part.transferProducer;
                                    if (part.source) {
                                        if (const auto* source = std::get_if<PlannedAllocationRef>(&*part.source)) {
                                            producer = source->readyAfter;
                                        }
                                    }
                                    if (auto added = AddDependency(storage, producer, assemblyId, node); !added) {
                                        return std::unexpected(added.error());
                                    }
                                }
                                storage.segments.emplace_back(PlannedAssembly{
                                    .logicalNode = node,
                                    .queue = assembly->queue,
                                    .destination = assembly->allocation,
                                    .parts = std::move(parts),
                                });
                                if (auto scheduled =
                                        ScheduleQueueSegment(storage, queues, assembly->queue, assemblyId, node);
                                    !scheduled) {
                                    return std::unexpected(scheduled.error());
                                }
                                for (LoweredInput& lowered : inputs) {
                                    if (lowered.buffer != binding.buffer.Describe().id ||
                                        !lowered.materializedGraphLocal) {
                                        continue;
                                    }
                                    if (auto bound = graphLocal.Bind(
                                            {std::move(*lowered.materializedGraphLocal),
                                             {.allocation = assembly->allocation, .readyAfter = assemblyId}});
                                        !bound) {
                                        return std::unexpected(
                                            Error(DispatchGraphPlanningErrorCode::ResidencyLoweringFailed, node, {},
                                                  static_cast<uint64_t>(bound.error())));
                                    }
                                    lowered.materializedGraphLocal.reset();
                                }
                                input = PlannedAllocationRef{assembly->allocation, assemblyId};
                            }
                            bufferStorage.push_back({.input = input});
                            if (const auto* state = std::get_if<PlannedAllocationRef>(&input)) {
                                if (auto added = AddDependency(storage, state->readyAfter, dispatchId, node); !added) {
                                    return std::unexpected(added.error());
                                }
                            }
                            continue;
                        }

                        const auto version =
                            std::ranges::find(outputVersions, binding.buffer.Describe().id, &OutputVersion::buffer);
                        if (version == outputVersions.end() || storage.allocations.size() >= UINT32_MAX) {
                            return std::unexpected(Error(version == outputVersions.end()
                                                             ? DispatchGraphPlanningErrorCode::ResidencyLoweringFailed
                                                             : DispatchGraphPlanningErrorCode::CapacityExceeded,
                                                         node));
                        }
                        const auto logicalBinding = std::ranges::find(execution.logicalBindings, binding.argument,
                                                                      &DispatchMemoryBinding::argument);
                        if (binding.publication == DestinationPublication::Publish &&
                            IsLastWriter(lastWriters, node, binding.buffer.Describe().id) &&
                            (logicalBinding == execution.logicalBindings.end() ||
                             !SameCoverage(binding.buffer.LogicalCoverage(), BindingCoverage(*logicalBinding)))) {
                            return std::unexpected(
                                Error(DispatchGraphPlanningErrorCode::UnsupportedBinding, node, binding.argument));
                        }
                        const PlannedAllocationId allocationId{static_cast<uint32_t>(storage.allocations.size())};
                        PlannedAllocation allocation{
                            .id = allocationId,
                            .buffer = binding.buffer,
                            .service = shard.residency.service,
                            .location = shard.residency.destination->location,
                            .bufferRegion = binding.region,
                            .byteFootprint = binding.byteFootprint,
                            .version = version->base + 1,
                            .materialization = PlannedAllocate{.requiredProperties = memoryPolicy.requiredProperties,
                                                               .preferredProperties = memoryPolicy.preferredProperties,
                                                               .alignment = binding.alignment}};
                        if (binding.publication == DestinationPublication::Publish &&
                            IsLastWriter(lastWriters, node, binding.buffer.Describe().id)) {
                            allocation.publication = PlannedPublication{
                                .kind = execution.partitioned ? PlannedPublicationKind::PartitionedWrite
                                                              : PlannedPublicationKind::WholeWrite,
                                .expectedVersion = version->base,
                                .producer = dispatchId,
                                .partitionIndex = execution.partitioned ? dispatchIndex : UINT32_MAX,
                                .partitionCount =
                                    execution.partitioned ? static_cast<uint32_t>(execution.shards.size()) : 0};
                        }
                        storage.allocations.push_back(std::move(allocation));
                        bufferStorage.push_back({.output = allocationId});
                        const GraphLocalResidencySource symbolic{
                            .buffer = binding.buffer.Describe().id,
                            .coverage = binding.region,
                            .byteFootprint = binding.byteFootprint,
                            .version = version->base + 1,
                            .producer = node,
                            .replica = {.service = shard.residency.service,
                                        .location = shard.residency.destination->location,
                                        .materializer = node}};
                        if (auto bound = graphLocal.Bind({symbolic, {allocationId, dispatchId}}); !bound) {
                            return std::unexpected(Error(DispatchGraphPlanningErrorCode::ResidencyLoweringFailed, node,
                                                         {}, static_cast<uint64_t>(bound.error())));
                        }
                    }

                    const auto package = services.packages.Resolve(choice.workload.kernelHash);
                    if (!package) {
                        return std::unexpected(Error(DispatchGraphPlanningErrorCode::ProgramUnavailable, node, {},
                                                     static_cast<uint64_t>(package.error().code)));
                    }
                    std::optional<PlannedPipeline> pipeline;
                    if (shard.placement.pipeline) {
                        pipeline = PlannedPipeline{.factory = shard.placement.pipeline->key.builder,
                                                   .state = shard.placement.pipeline->key.state};
                    }
                    for (const GraphNodeId predecessor : (*predecessors)[node.index]) {
                        for (const PhysicalPlanSegmentId predecessorCompletion :
                             logicalCompletions[predecessor.index]) {
                            if (auto added = AddDependency(storage, predecessorCompletion, dispatchId, node); !added) {
                                return std::unexpected(added.error());
                            }
                        }
                    }
                    if (auto scheduled = ScheduleQueueSegment(storage, queues, dispatchQueue, dispatchId, node);
                        !scheduled) {
                        return std::unexpected(scheduled.error());
                    }
                    storage.segments.emplace_back(PlannedDispatch{.logicalNode = node,
                                                                  .queue = dispatchQueue,
                                                                  .program = *package,
                                                                  .invocation = *invocation,
                                                                  .slice = shard.slice,
                                                                  .target = shard.placement.target,
                                                                  .specialization = shard.placement.specialization,
                                                                  .geometry = shard.geometry,
                                                                  .pipeline = pipeline,
                                                                  .bufferStorage = std::move(bufferStorage),
                                                                  .deterministic = shard.placement.deterministic});
                    logicalCompletions[node.index].push_back(dispatchId);
                }
            }

            if (!transferConstraints.empty()) {
                std::ranges::sort(loweredTransfers, {}, &LoweredTransfer::ref);
                if (std::ranges::adjacent_find(loweredTransfers, {}, &LoweredTransfer::ref) != loweredTransfers.end()) {
                    return std::unexpected(Error(DispatchGraphPlanningErrorCode::InvalidSelection));
                }
                for (const DispatchTransferConstraint constraint : transferConstraints) {
                    if (!constraint.Valid()) {
                        return std::unexpected(Error(DispatchGraphPlanningErrorCode::InvalidSelection));
                    }
                    const auto predecessor =
                        std::ranges::lower_bound(loweredTransfers, constraint.predecessor, {}, &LoweredTransfer::ref);
                    const auto successor =
                        std::ranges::lower_bound(loweredTransfers, constraint.successor, {}, &LoweredTransfer::ref);
                    if (predecessor == loweredTransfers.end() || predecessor->ref != constraint.predecessor ||
                        successor == loweredTransfers.end() || successor->ref != constraint.successor) {
                        return std::unexpected(Error(DispatchGraphPlanningErrorCode::InvalidSelection));
                    }
                    if (auto added = AddDependency(storage, predecessor->segment, successor->segment,
                                                   constraint.successor.consumer);
                        !added) {
                        return std::unexpected(added.error());
                    }
                }
            }
            if (auto ordered = CanonicalizePhysicalGraphPlanStorage(storage); !ordered) {
                return std::unexpected(ordered.error());
            }
            return storage;
        } catch (const std::bad_alloc&) {
            return std::unexpected(Error(DispatchGraphPlanningErrorCode::OutOfMemory));
        }
    }

} // namespace Sora::Compute

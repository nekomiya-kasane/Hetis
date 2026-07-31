/**
 * @file FusionOpportunity.cpp
 * @brief Structural elementwise graph-fusion opportunity discovery.
 * @ingroup Compute
 */

#include <Sora/Compute/Graph/FusionOpportunity.h>

#include <algorithm>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace Sora::Compute {
    namespace {

        struct BufferUse {
            BufferId buffer{};
            GraphNodeId node{};
            AccessMode mode = AccessMode::Read;
        };

        [[nodiscard]] constexpr bool BufferUseLess(const BufferUse& left, const BufferUse& right) noexcept {
            if (left.buffer != right.buffer) {
                return left.buffer < right.buffer;
            }
            return left.node < right.node;
        }

        [[nodiscard]] bool SameView(const BufferView& left, const BufferView& right) noexcept {
            return left.BufferDescriptor() == right.BufferDescriptor() && left.Describe() == right.Describe();
        }

        [[nodiscard]] std::optional<KernelArgumentId> WrittenOutput(const KernelInvocation& invocation) noexcept {
            std::optional<KernelArgumentId> output;
            for (uint32_t index = 0; index < invocation.Signature().size(); ++index) {
                const KernelArgumentDesc& argument = invocation.Signature()[index];
                if (argument.kind != ParameterKind::Buffer || argument.access != AccessMode::Write) {
                    continue;
                }
                if (output) {
                    return std::nullopt;
                }
                output = KernelArgumentId{index};
            }
            return output;
        }

        [[nodiscard]] std::optional<KernelArgumentId> MatchingRead(const KernelInvocation& consumer,
                                                                   const BufferView& intermediate) noexcept {
            std::optional<KernelArgumentId> input;
            for (uint32_t index = 0; index < consumer.Signature().size(); ++index) {
                const KernelArgumentDesc& argument = consumer.Signature()[index];
                if (argument.kind != ParameterKind::Buffer || argument.access != AccessMode::Read) {
                    continue;
                }
                const auto* view = std::get_if<BufferView>(&consumer.Bindings()[index]);
                if (view == nullptr || !SameView(*view, intermediate)) {
                    continue;
                }
                if (input) {
                    return std::nullopt;
                }
                input = KernelArgumentId{index};
            }
            return input;
        }

        [[nodiscard]] bool UniqueIntermediate(std::span<const BufferUse> uses, BufferId intermediate,
                                              GraphNodeId producer, GraphNodeId consumer) noexcept {
            const auto first = std::ranges::lower_bound(uses, intermediate, {}, &BufferUse::buffer);
            const auto last = std::ranges::upper_bound(uses, intermediate, {}, &BufferUse::buffer);
            if (last - first != 2) {
                return false;
            }
            return ((first[0].node == producer && first[0].mode == AccessMode::Write && first[1].node == consumer &&
                     first[1].mode == AccessMode::Read) ||
                    (first[1].node == producer && first[1].mode == AccessMode::Write && first[0].node == consumer &&
                     first[0].mode == AccessMode::Read));
        }

    } // namespace

    std::expected<std::vector<ElementwiseFusionOpportunity>, FusionOpportunityError>
    FindElementwiseFusionOpportunities(const ExecutionGraph& graph) noexcept {
        try {
            const GraphView view = graph.View();
            std::vector<BufferUse> uses;
            uses.reserve(view.accesses.size());
            for (uint32_t nodeIndex = 0; nodeIndex < view.nodes.size(); ++nodeIndex) {
                const GraphNode& node = view.nodes[nodeIndex];
                for (const ResourceAccess& access :
                     view.accesses.subspan(node.firstAccess, static_cast<size_t>(node.accessCount))) {
                    uses.push_back({.buffer = access.buffer, .node = {nodeIndex}, .mode = access.mode});
                }
            }
            std::ranges::sort(uses, BufferUseLess);

            std::vector<ElementwiseFusionOpportunity> opportunities;
            opportunities.reserve(view.edges.size());
            for (const GraphEdge edge : view.edges) {
                if (edge.successor.index != edge.predecessor.index + 1) {
                    continue;
                }
                const auto* producer = std::get_if<KernelInvocation>(&graph.Operation(edge.predecessor));
                const auto* consumer = std::get_if<KernelInvocation>(&graph.Operation(edge.successor));
                if (producer == nullptr || consumer == nullptr || producer->Domain() != consumer->Domain()) {
                    continue;
                }
                const auto output = WrittenOutput(*producer);
                if (!output) {
                    continue;
                }
                const auto* intermediate = std::get_if<BufferView>(&producer->Bindings()[output->index]);
                if (intermediate == nullptr) {
                    continue;
                }
                const auto input = MatchingRead(*consumer, *intermediate);
                if (!input) {
                    continue;
                }
                const BufferId buffer = intermediate->BufferDescriptor().id;
                if (!UniqueIntermediate(uses, buffer, edge.predecessor, edge.successor)) {
                    continue;
                }
                opportunities.push_back({.producer = edge.predecessor,
                                         .consumer = edge.successor,
                                         .producerOutput = *output,
                                         .consumerInput = *input,
                                         .intermediate = buffer});
            }
            return opportunities;
        } catch (const std::bad_alloc&) {
            return std::unexpected(FusionOpportunityError::OutOfMemory);
        }
    }

} // namespace Sora::Compute

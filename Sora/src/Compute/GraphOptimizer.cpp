/**
 * @file GraphOptimizer.cpp
 * @brief Runtime selection and transactional publication of canonical graph fusion.
 * @ingroup Compute
 */

#include <Sora/Compute/Runtime/GraphOptimizer.h>

#include <Sora/Compute/Compiler/KernelFusion.h>
#include <Sora/Compute/Cost.h>
#include <Sora/Compute/Graph/FusionOpportunity.h>
#include <Sora/Compute/Graph/GraphBuilder.h>

#include <algorithm>
#include <concepts>
#include <expected>
#include <limits>
#include <new>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace Sora::Compute {
    namespace {

        [[nodiscard]] constexpr uint64_t SavedIntermediateBytes(const KernelInvocation& producer,
                                                                KernelArgumentId output) noexcept {
            if (output.index >= producer.Signature().size()) {
                return 0;
            }
            const uint64_t bytes = producer.Signature()[output.index].elementType.ByteSize();
            return Detail::SaturatingDemandMultiply(
                Detail::SaturatingDemandMultiply(producer.Domain().workItemCount, bytes), 2);
        }

        [[nodiscard]] constexpr GraphOptimizationReport Estimate(const ElementwiseFusionOpportunity& opportunity,
                                                                 const KernelInvocation& producer,
                                                                 GraphFusionPolicy policy) noexcept {
            const uint64_t savedBytes = SavedIntermediateBytes(producer, opportunity.producerOutput);
            const uint64_t trafficNanoseconds = Detail::RateCost(savedBytes, policy.memoryBandwidthBytesPerSecond);
            const uint64_t benefit = Detail::SaturatingAdd(policy.launchLatencyNanoseconds, trafficNanoseconds);
            const uint64_t compilation =
                Detail::CeilDivide(policy.compilationLatencyNanoseconds, policy.expectedReuseCount);
            return {.status = benefit > compilation ? GraphOptimizationStatus::Fused
                                                    : GraphOptimizationStatus::SkippedUnprofitable,
                    .producer = opportunity.producer,
                    .consumer = opportunity.consumer,
                    .intermediateBytesSaved = savedBytes,
                    .estimatedBenefitNanoseconds = benefit,
                    .amortizedCompilationNanoseconds = compilation};
        }

        [[nodiscard]] std::expected<GraphNodeId, GraphError>
        AddOperationCopy(GraphBuilder& builder, const GraphOperation& operation) noexcept {
            try {
                return std::visit(
                    [&]<typename Operation>(const Operation& value) -> std::expected<GraphNodeId, GraphError> {
                        if constexpr (std::same_as<Operation, KernelInvocation>) {
                            return builder.AddDispatch(value);
                        } else if constexpr (std::same_as<Operation, Sora::Compute::CopyOperation>) {
                            return builder.AddCopy(value);
                        } else if constexpr (std::same_as<Operation, FillOperation>) {
                            return builder.AddFill(value);
                        } else {
                            return builder.AddExternal(value);
                        }
                    },
                    operation);
            } catch (const std::bad_alloc&) {
                return std::unexpected(GraphError{GraphErrorCode::OutOfMemory});
            }
        }

        [[nodiscard]] std::expected<ExecutionGraph, GraphError>
        ReplaceAdjacentPair(const ExecutionGraph& graph, const ElementwiseFusionOpportunity& opportunity,
                            KernelInvocation fused) noexcept {
            try {
                GraphBuilder builder;
                std::vector<GraphNodeId> replacements(graph.NodeCount());
                for (uint32_t index = 0; index < graph.NodeCount(); ++index) {
                    if (index == opportunity.producer.index) {
                        auto node = builder.AddDispatch(std::move(fused));
                        if (!node) {
                            return std::unexpected(node.error());
                        }
                        replacements[opportunity.producer.index] = *node;
                        replacements[opportunity.consumer.index] = *node;
                        ++index;
                        continue;
                    }
                    auto node = AddOperationCopy(builder, graph.Operation({index}));
                    if (!node) {
                        return std::unexpected(node.error());
                    }
                    replacements[index] = *node;
                }

                for (const GraphEdge& edge : graph.View().edges) {
                    const GraphNodeId predecessor = replacements[edge.predecessor.index];
                    const GraphNodeId successor = replacements[edge.successor.index];
                    if (predecessor == successor) {
                        continue;
                    }
                    if (auto dependency = builder.AddDependency(predecessor, successor); !dependency) {
                        return std::unexpected(dependency.error());
                    }
                }
                return std::move(builder).Finish();
            } catch (const std::bad_alloc&) {
                return std::unexpected(GraphError{GraphErrorCode::OutOfMemory});
            }
        }

        [[nodiscard]] std::vector<KernelArgumentBinding> FuseBindings(const KernelInvocation& producer,
                                                                      KernelArgumentId producerOutput,
                                                                      const KernelInvocation& consumer,
                                                                      KernelArgumentId consumerInput) {
            std::vector<KernelArgumentBinding> bindings;
            bindings.reserve(producer.Bindings().size() + consumer.Bindings().size() - 2);
            for (uint32_t index = 0; index < producer.Bindings().size(); ++index) {
                if (index != producerOutput.index) {
                    bindings.push_back(producer.Bindings()[index]);
                }
            }
            for (uint32_t index = 0; index < consumer.Bindings().size(); ++index) {
                if (index != consumerInput.index) {
                    bindings.push_back(consumer.Bindings()[index]);
                }
            }
            return bindings;
        }

        [[nodiscard]] GraphOptimizationResult Failed(ExecutionGraph graph, ProgramCatalogSnapshot programs,
                                                     GraphOptimizationReport report,
                                                     GraphOptimizationIssue issue) noexcept {
            report.status = GraphOptimizationStatus::Failed;
            report.issue = issue;
            return {std::move(graph), std::move(programs), report};
        }

    } // namespace

    GraphOptimizationResult OptimizeExecutionGraph(ExecutionGraph graph, ProgramCatalogSnapshot programs,
                                                   GraphFusionPolicy policy) noexcept {
        if (!policy.Valid()) {
            return Failed(std::move(graph), std::move(programs), {}, GraphOptimizationIssue::InvalidPolicy);
        }
        if (!policy.enabled) {
            return {std::move(graph), std::move(programs), {.status = GraphOptimizationStatus::Disabled}};
        }
        auto opportunities = FindElementwiseFusionOpportunities(graph);
        if (!opportunities) {
            return Failed(std::move(graph), std::move(programs), {},
                          opportunities.error() == FusionOpportunityError::OutOfMemory
                              ? GraphOptimizationIssue::OutOfMemory
                              : GraphOptimizationIssue::OpportunityDiscovery);
        }
        if (opportunities->empty()) {
            return {std::move(graph), std::move(programs), {}};
        }

        GraphOptimizationReport best{.status = GraphOptimizationStatus::SkippedUnprofitable};
        for (const ElementwiseFusionOpportunity& opportunity : *opportunities) {
            const auto* producer = std::get_if<KernelInvocation>(&graph.Operation(opportunity.producer));
            if (producer == nullptr) {
                continue;
            }
            const GraphOptimizationReport estimate = Estimate(opportunity, *producer, policy);
            if (estimate.status != GraphOptimizationStatus::Fused) {
                if (estimate.estimatedBenefitNanoseconds > best.estimatedBenefitNanoseconds) {
                    best = estimate;
                }
                continue;
            }
            const uint64_t estimateNet =
                estimate.estimatedBenefitNanoseconds - estimate.amortizedCompilationNanoseconds;
            const uint64_t bestNet = best.status == GraphOptimizationStatus::Fused
                                         ? best.estimatedBenefitNanoseconds - best.amortizedCompilationNanoseconds
                                         : 0;
            if (best.status != GraphOptimizationStatus::Fused || estimateNet > bestNet) {
                best = estimate;
            }
        }
        if (best.status != GraphOptimizationStatus::Fused) {
            return {std::move(graph), std::move(programs), best};
        }

        try {
            const auto selected = std::ranges::find_if(*opportunities, [&](const auto& opportunity) {
                return opportunity.producer == best.producer && opportunity.consumer == best.consumer;
            });
            if (selected == opportunities->end()) {
                return Failed(std::move(graph), std::move(programs), best,
                              GraphOptimizationIssue::OpportunityDiscovery);
            }
            const auto* producer = std::get_if<KernelInvocation>(&graph.Operation(best.producer));
            const auto* consumer = std::get_if<KernelInvocation>(&graph.Operation(best.consumer));
            if (producer == nullptr || consumer == nullptr) {
                return Failed(std::move(graph), std::move(programs), best,
                              GraphOptimizationIssue::OpportunityDiscovery);
            }
            auto producerPackage = programs.Resolve(producer->ProgramHash());
            auto consumerPackage = programs.Resolve(consumer->ProgramHash());
            if (!producerPackage || !consumerPackage) {
                return Failed(std::move(graph), std::move(programs), best, GraphOptimizationIssue::ProgramResolution);
            }
            auto producerImage = KernelProgramImage::Decode(producerPackage->Bytes());
            auto consumerImage = KernelProgramImage::Decode(consumerPackage->Bytes());
            if (!producerImage || !consumerImage) {
                return Failed(std::move(graph), std::move(programs), best, GraphOptimizationIssue::ProgramDecode);
            }

            auto fusedImage =
                FuseElementwiseKernels(producerImage->View(), consumerImage->View(), selected->consumerInput);
            if (!fusedImage) {
                return Failed(std::move(graph), std::move(programs), best, GraphOptimizationIssue::Fusion);
            }
            auto bindings = FuseBindings(*producer, selected->producerOutput, *consumer, selected->consumerInput);
            const auto extents = std::span<const uint64_t>{producer->Domain().extents.data(), producer->Domain().rank};
            auto invocation = BindKernelProgram(fusedImage->View(), fusedImage->Hash(), extents, std::move(bindings));
            if (!invocation) {
                return Failed(std::move(graph), std::move(programs), best, GraphOptimizationIssue::InvocationBinding);
            }
            auto program = ProgramPackage::Create(std::move(*fusedImage));
            if (!program) {
                return Failed(std::move(graph), std::move(programs), best, GraphOptimizationIssue::PackageConstruction);
            }
            auto overlay = programs.WithGenerated(*program);
            if (!overlay) {
                return Failed(std::move(graph), std::move(programs), best, GraphOptimizationIssue::ProgramCatalog);
            }
            auto rebuilt = ReplaceAdjacentPair(graph, *selected, std::move(*invocation));
            if (!rebuilt) {
                return Failed(std::move(graph), std::move(programs), best,
                              rebuilt.error().code == GraphErrorCode::OutOfMemory
                                  ? GraphOptimizationIssue::OutOfMemory
                                  : GraphOptimizationIssue::GraphRebuild);
            }
            return {std::move(*rebuilt), std::move(*overlay), best};
        } catch (const std::bad_alloc&) {
            return Failed(std::move(graph), std::move(programs), best, GraphOptimizationIssue::OutOfMemory);
        }
    }

} // namespace Sora::Compute

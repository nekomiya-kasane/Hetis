/**
 * @file GraphOptimizer.cpp
 * @brief Runtime selection and transactional publication of canonical graph-fusion regions.
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
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace Sora::Compute {
    namespace {

        struct FusionRegionCandidate final {
            GraphNodeId first{};
            GraphNodeId last{};
            KernelInvocation invocation{};
            ProgramPackage program{};
            uint64_t dispatches = 0;
            uint64_t launchesRemoved = 0;
            uint64_t intermediateBytesSaved = 0;
            uint64_t estimatedBenefitNanoseconds = 0;
            uint64_t amortizedCompilationNanoseconds = 0;
        };

        struct FusionAccumulator final {
            GraphNodeId first{};
            GraphNodeId last{};
            KernelInvocation invocation{};
            KernelProgramImage image{};
            uint64_t launchesRemoved = 0;
            uint64_t intermediateBytesSaved = 0;
        };

        struct ProgramGroup final {
            SemanticHash program{};
            size_t representativeCandidate = 0;
            size_t firstCandidate = 0;
            size_t candidateCount = 0;
            size_t activationCount = 0;
            size_t selectedCount = 0;
            uint64_t activationBenefitNanoseconds = 0;
            bool publish = true;
        };

        struct GroupedPrograms final {
            std::vector<ProgramGroup> groups{};
            std::vector<size_t> candidates{};
        };

        enum class FusionSelectionStatus : uint8_t {
            Selected,
            Unprofitable,
            BudgetLimited,
        };

        struct FusionSelection final {
            std::vector<size_t> candidates{};
            std::vector<ProgramPackage> programs{};
            FusionSelectionStatus status = FusionSelectionStatus::Unprofitable;
            bool selectAllCandidates = false;
            uint64_t uniquePrograms = 0;
            uint64_t intermediateBytesSaved = 0;
            uint64_t estimatedBenefitNanoseconds = 0;
            uint64_t amortizedCompilationNanoseconds = 0;
        };

        struct SelectionAction final {
            size_t group = 0;
            size_t count = 0;
            uint64_t netBenefitNanoseconds = 0;
            uint64_t estimatedBenefitNanoseconds = 0;
            uint64_t amortizedCompilationNanoseconds = 0;
            uint64_t publicationBytes = 0;
            size_t publicationPrograms = 0;
        };

        [[nodiscard]] constexpr uint64_t SavedIntermediateBytes(const KernelInvocation& producer,
                                                                KernelArgumentId output) noexcept {
            if (output.index >= producer.Signature().size()) {
                return 0;
            }
            const uint64_t bytes = producer.Signature()[output.index].elementType.ByteSize();
            return Detail::SaturatingDemandMultiply(
                Detail::SaturatingDemandMultiply(producer.Domain().workItemCount, bytes), 2);
        }

        [[nodiscard]] constexpr bool OpportunityLess(const ElementwiseFusionOpportunity& left,
                                                     const ElementwiseFusionOpportunity& right) noexcept {
            if (left.producer != right.producer) {
                return left.producer < right.producer;
            }
            return left.consumer < right.consumer;
        }

        [[nodiscard]] constexpr bool CandidateBetter(const FusionRegionCandidate& left,
                                                     const FusionRegionCandidate& right) noexcept {
            if (left.estimatedBenefitNanoseconds != right.estimatedBenefitNanoseconds) {
                return left.estimatedBenefitNanoseconds > right.estimatedBenefitNanoseconds;
            }
            if (left.first != right.first) {
                return left.first < right.first;
            }
            return left.last < right.last;
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

        [[nodiscard]] std::expected<KernelProgramImage, GraphOptimizationIssue>
        ResolveImage(const ProgramCatalogSnapshot& programs, const KernelInvocation& invocation) noexcept {
            auto package = programs.Resolve(invocation.ProgramHash());
            if (!package) {
                return std::unexpected(GraphOptimizationIssue::ProgramResolution);
            }
            auto image = KernelProgramImage::Decode(package->Bytes());
            if (!image) {
                return std::unexpected(image.error().code == KernelProgramPackageErrorCode::OutOfMemory
                                           ? GraphOptimizationIssue::OutOfMemory
                                           : GraphOptimizationIssue::ProgramDecode);
            }
            return std::move(*image);
        }

        [[nodiscard]] std::expected<FusionAccumulator, GraphOptimizationIssue>
        StartRegion(const ExecutionGraph& graph, const ProgramCatalogSnapshot& programs, GraphNodeId first) noexcept {
            const auto* invocation = std::get_if<KernelInvocation>(&graph.Operation(first));
            if (invocation == nullptr) {
                return std::unexpected(GraphOptimizationIssue::Fusion);
            }
            auto image = ResolveImage(programs, *invocation);
            if (!image) {
                return std::unexpected(image.error());
            }
            return FusionAccumulator{
                .first = first, .last = first, .invocation = *invocation, .image = std::move(*image)};
        }

        [[nodiscard]] std::expected<bool, GraphOptimizationIssue>
        AppendRegion(FusionAccumulator& region, const ExecutionGraph& graph, const ProgramCatalogSnapshot& programs,
                     const ElementwiseFusionOpportunity& opportunity, GraphFusionPolicy policy) noexcept {
            if (opportunity.producer != region.last) {
                return false;
            }
            const auto* producer = std::get_if<KernelInvocation>(&graph.Operation(opportunity.producer));
            const auto* consumer = std::get_if<KernelInvocation>(&graph.Operation(opportunity.consumer));
            if (producer == nullptr || consumer == nullptr) {
                return std::unexpected(GraphOptimizationIssue::Fusion);
            }
            auto consumerImage = ResolveImage(programs, *consumer);
            if (!consumerImage) {
                return std::unexpected(consumerImage.error());
            }
            auto fused = FuseElementwiseKernels(region.image.View(), consumerImage->View(), opportunity.consumerInput,
                                                policy.generatedProgramLimits);
            if (!fused) {
                switch (fused.error().code) {
                    case KernelFusionErrorCode::IncompatibleDomain:
                    case KernelFusionErrorCode::IncompatiblePolicy:
                    case KernelFusionErrorCode::InvalidIntermediate:
                    case KernelFusionErrorCode::CapacityExceeded:
                        return false;
                    case KernelFusionErrorCode::OutOfMemory:
                        return std::unexpected(GraphOptimizationIssue::OutOfMemory);
                    case KernelFusionErrorCode::InvalidProducer:
                    case KernelFusionErrorCode::InvalidConsumer:
                        return std::unexpected(GraphOptimizationIssue::Fusion);
                }
            }

            auto bindings =
                FuseBindings(region.invocation, region.image.View().output, *consumer, opportunity.consumerInput);
            const auto extents =
                std::span<const uint64_t>{region.invocation.Domain().extents.data(), region.invocation.Domain().rank};
            auto invocation = BindKernelProgram(fused->View(), fused->Hash(), extents, std::move(bindings));
            if (!invocation) {
                return std::unexpected(invocation.error().code == KernelInvocationErrorCode::OutOfMemory
                                           ? GraphOptimizationIssue::OutOfMemory
                                           : GraphOptimizationIssue::InvocationBinding);
            }
            region.intermediateBytesSaved = Detail::SaturatingAdd(
                region.intermediateBytesSaved, SavedIntermediateBytes(*producer, opportunity.producerOutput));
            region.invocation = std::move(*invocation);
            region.image = std::move(*fused);
            region.last = opportunity.consumer;
            ++region.launchesRemoved;
            return true;
        }

        [[nodiscard]] std::expected<FusionRegionCandidate, GraphOptimizationIssue>
        FinishRegion(FusionAccumulator region, GraphFusionPolicy policy) noexcept {
            auto program = ProgramPackage::Create(std::move(region.image));
            if (!program) {
                return std::unexpected(program.error().code == ProgramPackageErrorCode::OutOfMemory
                                           ? GraphOptimizationIssue::OutOfMemory
                                           : GraphOptimizationIssue::PackageConstruction);
            }
            const uint64_t launchBenefit =
                Detail::SaturatingDemandMultiply(policy.launchLatencyNanoseconds, region.launchesRemoved);
            const uint64_t trafficBenefit =
                Detail::RateCost(region.intermediateBytesSaved, policy.memoryBandwidthBytesPerSecond);
            return FusionRegionCandidate{
                .first = region.first,
                .last = region.last,
                .invocation = std::move(region.invocation),
                .program = std::move(*program),
                .dispatches = region.launchesRemoved + 1,
                .launchesRemoved = region.launchesRemoved,
                .intermediateBytesSaved = region.intermediateBytesSaved,
                .estimatedBenefitNanoseconds = Detail::SaturatingAdd(launchBenefit, trafficBenefit),
                .amortizedCompilationNanoseconds =
                    Detail::CeilDivide(policy.compilationLatencyNanoseconds, policy.expectedReuseCount),
            };
        }

        [[nodiscard]] std::expected<uint64_t, GraphOptimizationIssue>
        BuildFeasibleRegions(const ExecutionGraph& graph, const ProgramCatalogSnapshot& programs,
                             std::span<const ElementwiseFusionOpportunity> opportunities, GraphFusionPolicy policy,
                             std::vector<FusionRegionCandidate>& candidates) noexcept {
            try {
                uint64_t regionsConsidered = 0;
                for (size_t cursor = 0; cursor < opportunities.size();) {
                    ++regionsConsidered;
                    auto region = StartRegion(graph, programs, opportunities[cursor].producer);
                    if (!region) {
                        return std::unexpected(region.error());
                    }
                    bool appended = false;
                    while (cursor < opportunities.size() && opportunities[cursor].producer == region->last) {
                        auto fused = AppendRegion(*region, graph, programs, opportunities[cursor], policy);
                        if (!fused) {
                            return std::unexpected(fused.error());
                        }
                        ++cursor;
                        if (!*fused) {
                            break;
                        }
                        appended = true;
                    }
                    if (!appended) {
                        continue;
                    }
                    auto candidate = FinishRegion(std::move(*region), policy);
                    if (!candidate) {
                        return std::unexpected(candidate.error());
                    }
                    candidates.push_back(std::move(*candidate));
                }
                return regionsConsidered;
            } catch (const std::length_error&) {
                return std::unexpected(GraphOptimizationIssue::OutOfMemory);
            } catch (const std::bad_alloc&) {
                return std::unexpected(GraphOptimizationIssue::OutOfMemory);
            }
        }

        [[nodiscard]] bool SameProgram(const ProgramPackage& left, const ProgramPackage& right) noexcept {
            return left.Hash() == right.Hash() && std::ranges::equal(left.Bytes(), right.Bytes());
        }

        [[nodiscard]] constexpr bool ActionBetter(const SelectionAction& left, const SelectionAction& right,
                                                  const std::vector<ProgramGroup>& groups) noexcept {
            const uint64_t leftQuotient = left.netBenefitNanoseconds / left.count;
            const uint64_t rightQuotient = right.netBenefitNanoseconds / right.count;
            if (leftQuotient != rightQuotient) {
                return leftQuotient > rightQuotient;
            }
            const uint64_t leftRemainder = left.netBenefitNanoseconds % left.count;
            const uint64_t rightRemainder = right.netBenefitNanoseconds % right.count;
            const uint64_t leftFraction = leftRemainder * right.count;
            const uint64_t rightFraction = rightRemainder * left.count;
            if (leftFraction != rightFraction) {
                return leftFraction > rightFraction;
            }
            if (left.netBenefitNanoseconds != right.netBenefitNanoseconds) {
                return left.netBenefitNanoseconds > right.netBenefitNanoseconds;
            }
            return SemanticHashLess{}(groups[left.group].program, groups[right.group].program);
        }

        [[nodiscard]] std::expected<bool, GraphOptimizationIssue>
        RequiresPublication(const ProgramCatalogSnapshot& programs, const ProgramPackage& program) noexcept {
            auto existing = programs.Resolve(program.Hash());
            if (existing) {
                if (!SameProgram(*existing, program)) {
                    return std::unexpected(GraphOptimizationIssue::ProgramCatalog);
                }
                return false;
            }
            if (existing.error().code != PortablePackageRegistryErrorCode::NotFound) {
                return std::unexpected(GraphOptimizationIssue::ProgramCatalog);
            }
            return true;
        }

        [[nodiscard]] std::expected<GroupedPrograms, GraphOptimizationIssue>
        GroupPrograms(const std::vector<FusionRegionCandidate>& candidates, const ProgramCatalogSnapshot& programs,
                      uint64_t compilationCost) noexcept {
            try {
                GroupedPrograms grouped;
                grouped.groups.reserve(candidates.size());
                grouped.candidates.resize(candidates.size());
                std::iota(grouped.candidates.begin(), grouped.candidates.end(), size_t{0});
                std::ranges::sort(grouped.candidates, [&](size_t left, size_t right) {
                    const FusionRegionCandidate& leftCandidate = candidates[left];
                    const FusionRegionCandidate& rightCandidate = candidates[right];
                    if (leftCandidate.program.Hash() != rightCandidate.program.Hash()) {
                        return SemanticHashLess{}(leftCandidate.program.Hash(), rightCandidate.program.Hash());
                    }
                    return CandidateBetter(leftCandidate, rightCandidate);
                });

                for (size_t first = 0; first < grouped.candidates.size();) {
                    const FusionRegionCandidate& representative = candidates[grouped.candidates[first]];
                    size_t last = first + 1;
                    while (last < grouped.candidates.size() &&
                           candidates[grouped.candidates[last]].program.Hash() == representative.program.Hash()) {
                        if (!SameProgram(representative.program, candidates[grouped.candidates[last]].program)) {
                            return std::unexpected(GraphOptimizationIssue::ProgramCatalog);
                        }
                        ++last;
                    }

                    auto publish = RequiresPublication(programs, representative.program);
                    if (!publish) {
                        return std::unexpected(publish.error());
                    }
                    ProgramGroup group{.program = representative.program.Hash(),
                                       .representativeCandidate = grouped.candidates[first],
                                       .firstCandidate = first,
                                       .candidateCount = last - first,
                                       .publish = *publish};
                    uint64_t benefit = 0;
                    for (size_t index = 0; index < group.candidateCount; ++index) {
                        const size_t candidateIndex = grouped.candidates[group.firstCandidate + index];
                        benefit =
                            Detail::SaturatingAdd(benefit, candidates[candidateIndex].estimatedBenefitNanoseconds);
                        if (benefit > compilationCost) {
                            group.activationCount = index + 1;
                            group.activationBenefitNanoseconds = benefit;
                            break;
                        }
                    }
                    grouped.groups.push_back(std::move(group));
                    first = last;
                }
                return grouped;
            } catch (const std::length_error&) {
                return std::unexpected(GraphOptimizationIssue::OutOfMemory);
            } catch (const std::bad_alloc&) {
                return std::unexpected(GraphOptimizationIssue::OutOfMemory);
            }
        }

        [[nodiscard]] std::expected<FusionSelection, GraphOptimizationIssue>
        SelectSingleRegion(const FusionRegionCandidate& candidate, const ProgramCatalogSnapshot& programs,
                           GraphFusionPolicy policy) noexcept {
            try {
                FusionSelection selection;
                selection.intermediateBytesSaved = candidate.intermediateBytesSaved;
                selection.estimatedBenefitNanoseconds = candidate.estimatedBenefitNanoseconds;
                selection.amortizedCompilationNanoseconds = candidate.amortizedCompilationNanoseconds;
                if (candidate.estimatedBenefitNanoseconds <= candidate.amortizedCompilationNanoseconds) {
                    selection.status = FusionSelectionStatus::Unprofitable;
                    return selection;
                }

                auto publish = RequiresPublication(programs, candidate.program);
                if (!publish) {
                    return std::unexpected(publish.error());
                }
                const uint64_t byteLimit = std::min(policy.maximumGeneratedBytes, programs.AvailableGeneratedBytes());
                if (*publish && (programs.AvailableGeneratedProgramCapacity() == 0 ||
                                 candidate.program.Bytes().size() > byteLimit)) {
                    selection.status = FusionSelectionStatus::BudgetLimited;
                    return selection;
                }

                if (*publish) {
                    selection.programs.push_back(candidate.program);
                }
                selection.status = FusionSelectionStatus::Selected;
                selection.selectAllCandidates = true;
                selection.uniquePrograms = 1;
                return selection;
            } catch (const std::length_error&) {
                return std::unexpected(GraphOptimizationIssue::OutOfMemory);
            } catch (const std::bad_alloc&) {
                return std::unexpected(GraphOptimizationIssue::OutOfMemory);
            }
        }

        [[nodiscard]] std::expected<FusionSelection, GraphOptimizationIssue>
        SelectRegions(const std::vector<FusionRegionCandidate>& candidates, const ProgramCatalogSnapshot& programs,
                      GraphFusionPolicy policy) noexcept {
            try {
                if (candidates.size() == 1) {
                    return SelectSingleRegion(candidates.front(), programs, policy);
                }
                FusionSelection selection;
                const uint64_t compilationCost =
                    Detail::CeilDivide(policy.compilationLatencyNanoseconds, policy.expectedReuseCount);
                auto grouped = GroupPrograms(candidates, programs, compilationCost);
                if (!grouped) {
                    return std::unexpected(grouped.error());
                }
                std::vector<ProgramGroup>& groups = grouped->groups;
                const std::vector<size_t>& groupedCandidates = grouped->candidates;
                const size_t regionLimit =
                    std::min(static_cast<size_t>(policy.maximumFusionRegions), candidates.size());
                const size_t programLimit = programs.AvailableGeneratedProgramCapacity();
                const uint64_t byteLimit = std::min(policy.maximumGeneratedBytes, programs.AvailableGeneratedBytes());
                selection.candidates.reserve(regionLimit);
                selection.programs.reserve(std::min(regionLimit, programLimit));

                bool hasProfitableGroup = false;
                uint64_t bestRejectedBenefit = 0;
                uint64_t bestRejectedCompilation = compilationCost;
                uint64_t bestRejectedBytes = 0;
                for (const ProgramGroup& group : groups) {
                    const size_t diagnosticCount = group.activationCount != 0
                                                       ? group.activationCount
                                                       : std::min(group.candidateCount, regionLimit);
                    uint64_t benefit = 0;
                    uint64_t savedBytes = 0;
                    for (size_t index = 0; index < diagnosticCount; ++index) {
                        const size_t candidateIndex = groupedCandidates[group.firstCandidate + index];
                        const FusionRegionCandidate& candidate = candidates[candidateIndex];
                        benefit = Detail::SaturatingAdd(benefit, candidate.estimatedBenefitNanoseconds);
                        savedBytes = Detail::SaturatingAdd(savedBytes, candidate.intermediateBytesSaved);
                    }
                    if (benefit > bestRejectedBenefit) {
                        bestRejectedBenefit = benefit;
                        bestRejectedBytes = savedBytes;
                    }
                    hasProfitableGroup |= group.activationCount != 0;
                }

                size_t selectedRegions = 0;
                size_t publishedPrograms = 0;
                uint64_t publishedBytes = 0;
                while (selectedRegions < regionLimit) {
                    std::optional<SelectionAction> best;
                    for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
                        const ProgramGroup& group = groups[groupIndex];
                        if (group.selectedCount == group.candidateCount || group.activationCount == 0) {
                            continue;
                        }

                        SelectionAction action{.group = groupIndex};
                        if (group.selectedCount == 0) {
                            action.count = group.activationCount;
                            action.estimatedBenefitNanoseconds = group.activationBenefitNanoseconds;
                            action.amortizedCompilationNanoseconds = compilationCost;
                            action.netBenefitNanoseconds = group.activationBenefitNanoseconds - compilationCost;
                            action.publicationBytes =
                                group.publish ? candidates[group.representativeCandidate].program.Bytes().size() : 0;
                            action.publicationPrograms = group.publish ? 1 : 0;
                        } else {
                            action.count = 1;
                            const size_t candidateIndex = groupedCandidates[group.firstCandidate + group.selectedCount];
                            action.estimatedBenefitNanoseconds = candidates[candidateIndex].estimatedBenefitNanoseconds;
                            action.netBenefitNanoseconds = action.estimatedBenefitNanoseconds;
                        }

                        if (action.count > regionLimit - selectedRegions ||
                            action.publicationPrograms > programLimit - publishedPrograms ||
                            action.publicationBytes > byteLimit - publishedBytes) {
                            continue;
                        }
                        if (!best || ActionBetter(action, *best, groups)) {
                            best = action;
                        }
                    }
                    if (!best) {
                        break;
                    }

                    ProgramGroup& group = groups[best->group];
                    if (group.selectedCount == 0) {
                        ++selection.uniquePrograms;
                        if (group.publish) {
                            selection.programs.push_back(candidates[group.representativeCandidate].program);
                        }
                    }
                    for (size_t index = 0; index < best->count; ++index) {
                        const size_t candidateIndex = groupedCandidates[group.firstCandidate + group.selectedCount++];
                        selection.candidates.push_back(candidateIndex);
                        selection.intermediateBytesSaved = Detail::SaturatingAdd(
                            selection.intermediateBytesSaved, candidates[candidateIndex].intermediateBytesSaved);
                    }
                    selectedRegions += best->count;
                    publishedPrograms += best->publicationPrograms;
                    publishedBytes += best->publicationBytes;
                    selection.estimatedBenefitNanoseconds =
                        Detail::SaturatingAdd(selection.estimatedBenefitNanoseconds, best->estimatedBenefitNanoseconds);
                    selection.amortizedCompilationNanoseconds = Detail::SaturatingAdd(
                        selection.amortizedCompilationNanoseconds, best->amortizedCompilationNanoseconds);
                }

                if (!selection.candidates.empty()) {
                    selection.status = FusionSelectionStatus::Selected;
                } else if (hasProfitableGroup) {
                    selection.status = FusionSelectionStatus::BudgetLimited;
                    selection.intermediateBytesSaved = bestRejectedBytes;
                    selection.estimatedBenefitNanoseconds = bestRejectedBenefit;
                    selection.amortizedCompilationNanoseconds = bestRejectedCompilation;
                } else {
                    selection.status = FusionSelectionStatus::Unprofitable;
                    selection.intermediateBytesSaved = bestRejectedBytes;
                    selection.estimatedBenefitNanoseconds = bestRejectedBenefit;
                    selection.amortizedCompilationNanoseconds = bestRejectedCompilation;
                }
                return selection;
            } catch (const std::length_error&) {
                return std::unexpected(GraphOptimizationIssue::OutOfMemory);
            } catch (const std::bad_alloc&) {
                return std::unexpected(GraphOptimizationIssue::OutOfMemory);
            }
        }

        [[nodiscard]] std::expected<ExecutionGraph, GraphError>
        ReplaceRegions(const ExecutionGraph& graph, std::vector<FusionRegionCandidate>& regions) noexcept {
            try {
                std::ranges::sort(regions, {}, &FusionRegionCandidate::first);
                GraphBuilder builder;
                std::vector<GraphNodeId> replacements(graph.NodeCount());
                size_t regionIndex = 0;
                for (uint32_t index = 0; index < graph.NodeCount();) {
                    if (regionIndex < regions.size() && regions[regionIndex].first.index == index) {
                        FusionRegionCandidate& region = regions[regionIndex++];
                        auto node = builder.AddDispatch(std::move(region.invocation));
                        if (!node) {
                            return std::unexpected(node.error());
                        }
                        for (uint32_t replaced = region.first.index; replaced <= region.last.index; ++replaced) {
                            replacements[replaced] = *node;
                        }
                        index = region.last.index + 1;
                        continue;
                    }
                    auto node = AddOperationCopy(builder, graph.Operation({index}));
                    if (!node) {
                        return std::unexpected(node.error());
                    }
                    replacements[index] = *node;
                    ++index;
                }

                std::vector<GraphEdge> projected;
                for (const GraphEdge& edge : graph.View().edges) {
                    const GraphEdge mapped{.predecessor = replacements[edge.predecessor.index],
                                           .successor = replacements[edge.successor.index]};
                    if (mapped.predecessor != mapped.successor) {
                        if (projected.empty()) {
                            projected.reserve(graph.View().edges.size());
                        }
                        projected.push_back(mapped);
                    }
                }
                std::ranges::sort(projected, GraphEdgeLess);
                projected.erase(std::ranges::unique(projected).begin(), projected.end());
                for (const GraphEdge& edge : projected) {
                    if (auto dependency = builder.AddDependency(edge.predecessor, edge.successor); !dependency) {
                        return std::unexpected(dependency.error());
                    }
                }
                return std::move(builder).Finish();
            } catch (const std::bad_alloc&) {
                return std::unexpected(GraphError{GraphErrorCode::OutOfMemory});
            }
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

        GraphOptimizationReport report{.opportunitiesFound = opportunities->size()};
        if (opportunities->empty()) {
            return {std::move(graph), std::move(programs), report};
        }

        try {
            std::ranges::sort(*opportunities, OpportunityLess);
            std::vector<FusionRegionCandidate> candidates;
            candidates.reserve(opportunities->size());
            for (size_t first = 0; first < opportunities->size();) {
                size_t last = first + 1;
                while (last < opportunities->size() &&
                       (*opportunities)[last].producer == (*opportunities)[last - 1].consumer) {
                    ++last;
                }
                auto built = BuildFeasibleRegions(
                    graph, programs, std::span{*opportunities}.subspan(first, last - first), policy, candidates);
                if (!built) {
                    return Failed(std::move(graph), std::move(programs), report, built.error());
                }
                report.regionsConsidered = Detail::SaturatingAdd(report.regionsConsidered, *built);
                first = last;
            }

            if (candidates.empty()) {
                return {std::move(graph), std::move(programs), report};
            }

            auto selection = SelectRegions(candidates, programs, policy);
            if (!selection) {
                return Failed(std::move(graph), std::move(programs), report, selection.error());
            }
            report.generatedPrograms = selection->uniquePrograms;
            report.intermediateBytesSaved = selection->intermediateBytesSaved;
            report.estimatedBenefitNanoseconds = selection->estimatedBenefitNanoseconds;
            report.amortizedCompilationNanoseconds = selection->amortizedCompilationNanoseconds;
            if (selection->status == FusionSelectionStatus::Unprofitable) {
                report.status = GraphOptimizationStatus::SkippedUnprofitable;
                return {std::move(graph), std::move(programs), report};
            }
            if (selection->status == FusionSelectionStatus::BudgetLimited) {
                report.status = GraphOptimizationStatus::SkippedBudget;
                return {std::move(graph), std::move(programs), report};
            }

            std::vector<FusionRegionCandidate> selected;
            if (selection->selectAllCandidates || selection->candidates.size() == candidates.size()) {
                selected = std::move(candidates);
            } else {
                selected.reserve(selection->candidates.size());
                for (size_t candidateIndex : selection->candidates) {
                    selected.push_back(std::move(candidates[candidateIndex]));
                }
            }

            for (const FusionRegionCandidate& region : selected) {
                ++report.regionsFused;
                report.dispatchesFused = Detail::SaturatingAdd(report.dispatchesFused, region.dispatches);
                report.launchesRemoved = Detail::SaturatingAdd(report.launchesRemoved, region.launchesRemoved);
            }

            auto overlay = programs.WithGenerated(std::move(selection->programs));
            if (!overlay) {
                return Failed(std::move(graph), std::move(programs), report,
                              overlay.error().code == ProgramCatalogErrorCode::OutOfMemory
                                  ? GraphOptimizationIssue::OutOfMemory
                                  : GraphOptimizationIssue::ProgramCatalog);
            }
            auto rebuilt = ReplaceRegions(graph, selected);
            if (!rebuilt) {
                return Failed(std::move(graph), std::move(programs), report,
                              rebuilt.error().code == GraphErrorCode::OutOfMemory
                                  ? GraphOptimizationIssue::OutOfMemory
                                  : GraphOptimizationIssue::GraphRebuild);
            }
            report.status = GraphOptimizationStatus::Fused;
            return {std::move(*rebuilt), std::move(*overlay), report};
        } catch (const std::length_error&) {
            return Failed(std::move(graph), std::move(programs), report, GraphOptimizationIssue::OutOfMemory);
        } catch (const std::bad_alloc&) {
            return Failed(std::move(graph), std::move(programs), report, GraphOptimizationIssue::OutOfMemory);
        }
    }

} // namespace Sora::Compute

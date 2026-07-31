/**
 * @file GraphOptimizer.h
 * @brief Runtime composition of profitable canonical graph transformations.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Graph/ExecutionGraph.h>
#include <Sora/Compute/Runtime/ProgramCatalog.h>

#include <cstdint>
#include <type_traits>

namespace Sora::Compute {

    /** @brief Conservative pre-planning priors and hard budgets for submission-scoped elementwise fusion. */
    struct GraphFusionPolicy {
        bool enabled = true;
        uint64_t launchLatencyNanoseconds = 5'000;
        uint64_t memoryBandwidthBytesPerSecond = 20'000'000'000;
        uint64_t compilationLatencyNanoseconds = 500'000;
        uint32_t expectedReuseCount = 64;
        uint32_t maximumFusionRegions = 64;
        uint64_t maximumGeneratedBytes = uint64_t{256} * 1'024 * 1'024;
        KernelProgramLimits generatedProgramLimits{};

        /** @brief Return whether every divisor, region budget, and generated-program budget is defined. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return !enabled ||
                   (memoryBandwidthBytesPerSecond != 0 && expectedReuseCount != 0 && maximumFusionRegions != 0 &&
                    maximumGeneratedBytes != 0 && generatedProgramLimits.Valid());
        }

        friend constexpr bool operator==(GraphFusionPolicy, GraphFusionPolicy) noexcept = default;
    };

    /** @brief Stable outcome of one optional graph-optimization attempt. */
    enum class GraphOptimizationStatus : uint8_t {
        Unchanged,
        Disabled,
        Fused,
        SkippedUnprofitable,
        SkippedBudget,
        Failed,
    };

    /** @brief Stable non-fatal issue that caused the original graph to be retained. */
    enum class GraphOptimizationIssue : uint8_t {
        None,
        InvalidPolicy,
        OpportunityDiscovery,
        ProgramResolution,
        ProgramDecode,
        Fusion,
        InvocationBinding,
        PackageConstruction,
        GraphRebuild,
        ProgramCatalog,
        OutOfMemory,
    };

    /** @brief Deterministic report for one optional graph transformation. */
    struct GraphOptimizationReport {
        GraphOptimizationStatus status = GraphOptimizationStatus::Unchanged;
        GraphOptimizationIssue issue = GraphOptimizationIssue::None;
        uint64_t opportunitiesFound = 0;
        uint64_t regionsConsidered = 0;
        uint64_t regionsFused = 0;
        uint64_t dispatchesFused = 0;
        uint64_t launchesRemoved = 0;
        uint64_t generatedPrograms = 0;
        uint64_t intermediateBytesSaved = 0;
        uint64_t estimatedBenefitNanoseconds = 0;
        uint64_t amortizedCompilationNanoseconds = 0;

        friend constexpr bool operator==(GraphOptimizationReport, GraphOptimizationReport) noexcept = default;
    };

    /** @brief Monotonic outcomes of submission-scoped graph optimization. */
    struct GraphOptimizationStatistics {
        uint64_t attempts = 0;
        uint64_t unchanged = 0;
        uint64_t disabled = 0;
        uint64_t fused = 0;
        uint64_t skippedUnprofitable = 0;
        uint64_t skippedBudget = 0;
        uint64_t failed = 0;

        friend constexpr bool operator==(GraphOptimizationStatistics, GraphOptimizationStatistics) noexcept = default;
    };

    /** @brief Owning graph and the decision that selected it. */
    struct GraphOptimizationResult {
        ExecutionGraph graph;
        ProgramCatalogSnapshot programs;
        GraphOptimizationReport report{};
    };

    /** @brief Transactionally replace a deterministic bounded set of profitable disjoint elementwise regions. */
    [[nodiscard]] GraphOptimizationResult OptimizeExecutionGraph(ExecutionGraph graph, ProgramCatalogSnapshot programs,
                                                                 GraphFusionPolicy policy = {}) noexcept;

    static_assert(std::is_trivially_copyable_v<GraphFusionPolicy>);
    static_assert(std::is_trivially_copyable_v<GraphOptimizationReport>);
    static_assert(std::is_trivially_copyable_v<GraphOptimizationStatistics>);

} // namespace Sora::Compute

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

    /** @brief Conservative pre-planning priors used to admit one elementwise fusion. */
    struct GraphFusionPolicy {
        bool enabled = true;
        uint64_t launchLatencyNanoseconds = 5'000;
        uint64_t memoryBandwidthBytesPerSecond = 20'000'000'000;
        uint64_t compilationLatencyNanoseconds = 500'000;
        uint32_t expectedReuseCount = 64;

        /** @brief Return whether every divisor and amortization horizon is defined. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return !enabled || (memoryBandwidthBytesPerSecond != 0 && expectedReuseCount != 0);
        }

        friend constexpr bool operator==(GraphFusionPolicy, GraphFusionPolicy) noexcept = default;
    };

    /** @brief Stable outcome of one optional graph-optimization attempt. */
    enum class GraphOptimizationStatus : uint8_t {
        Unchanged,
        Disabled,
        Fused,
        SkippedUnprofitable,
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
        GraphNodeId producer{};
        GraphNodeId consumer{};
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
        uint64_t failed = 0;

        friend constexpr bool operator==(GraphOptimizationStatistics, GraphOptimizationStatistics) noexcept = default;
    };

    /** @brief Owning graph and the decision that selected it. */
    struct GraphOptimizationResult {
        ExecutionGraph graph;
        ProgramCatalogSnapshot programs;
        GraphOptimizationReport report{};
    };

    /** @brief Replace at most one adjacent profitable elementwise producer-consumer pair. */
    [[nodiscard]] GraphOptimizationResult OptimizeExecutionGraph(ExecutionGraph graph, ProgramCatalogSnapshot programs,
                                                                 GraphFusionPolicy policy = {}) noexcept;

    static_assert(std::is_trivially_copyable_v<GraphFusionPolicy>);
    static_assert(std::is_trivially_copyable_v<GraphOptimizationReport>);
    static_assert(std::is_trivially_copyable_v<GraphOptimizationStatistics>);

} // namespace Sora::Compute

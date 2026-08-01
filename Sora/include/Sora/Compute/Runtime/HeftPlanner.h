/**
 * @file HeftPlanner.h
 * @brief Deterministic HEFT placement for dispatch-only affine-buffer DAGs.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Runtime/AutomaticPartitionPlanningCalibration.h>
#include <Sora/Compute/Runtime/AutomaticDispatchPlanning.h>
#include <Sora/Compute/Runtime/DispatchGraphPlanning.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace Sora::Compute {

    /** @brief Explicit deterministic budget for improving one feasible HEFT schedule by placement local search. */
    struct HeftLocalSearchPolicy {
        uint32_t maximumPasses = 0;
        uint32_t maximumNeighborEvaluations = 0;
        uint64_t minimumImprovementNanoseconds = 1;

        /** @brief Return whether this policy requests any local-search work. */
        [[nodiscard]] constexpr bool Enabled() const noexcept {
            return maximumPasses != 0 && maximumNeighborEvaluations != 0;
        }

        /** @brief Return whether pass and evaluation bounds consistently enable or disable local search. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return minimumImprovementNanoseconds != 0 && ((maximumPasses == 0 && maximumNeighborEvaluations == 0) ||
                                                          (maximumPasses != 0 && maximumNeighborEvaluations != 0));
        }

        friend constexpr bool operator==(HeftLocalSearchPolicy, HeftLocalSearchPolicy) noexcept = default;
    };

    /** @brief Runtime calibration controls for resolving one HEFT node's automatic-partition search budget. */
    struct HeftAutomaticPartitionTuningPolicy {
        bool enabled = true;
        uint32_t coldStartMaximumEvaluations = 8;
        uint32_t minimumCalibrationSamples = 4;
        uint32_t minimumPlanningReturnPermille = 4'000;
        uint64_t coldStartMinimumBenefitNanoseconds = 50'000;

        /** @brief Return whether cold-start and calibrated ROI controls are meaningful. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return !enabled || (coldStartMaximumEvaluations != 0 && minimumCalibrationSamples != 0 &&
                                minimumPlanningReturnPermille >= 1'000);
        }

        friend constexpr bool operator==(HeftAutomaticPartitionTuningPolicy,
                                         HeftAutomaticPartitionTuningPolicy) noexcept = default;
    };

    /** @brief Uniform candidate, hard-placement, and bounded-optimization policies for one HEFT attempt. */
    struct HeftPlanningPolicy {
        DispatchEnumerationPolicy dispatch = [] {
            DispatchEnumerationPolicy value;
            value.memory.publication = DestinationPublication::Publish;
            return value;
        }();
        PlacementPolicy placement{};
        AutomaticPartitionPolicy automaticPartition = [] {
            AutomaticPartitionPolicy value;
            value.maximumPartitionEvaluations = 64;
            value.minimumAmortizedBenefitNanoseconds = 0;
            return value;
        }();
        HeftAutomaticPartitionTuningPolicy automaticPartitionTuning{};
        HeftLocalSearchPolicy localSearch{};
    };

    /** @brief One frozen policy resolution for a single logical node on its current absolute DAG timeline. */
    struct HeftAutomaticPartitionResolution {
        AutomaticPartitionPolicy policy{};
        uint64_t amortizedWholeNanoseconds = 0;
        bool search = false;
        bool calibrated = false;
    };

    /**
     * @brief Resolve a hard-bounded partition policy from frozen process-local planning calibration.
     * @details Fixed mode preserves @p policy exactly. Adaptive cold start uses explicit conservative controls. A
     * mature estimate admits only the number of evaluations whose empirically guarded planning cost satisfies the
     * configured return against an upper bound on the node's reusable whole-execution benefit. This is a search-effort
     * bound, not a guarantee that a profitable partition exists.
     */
    [[nodiscard]] HeftAutomaticPartitionResolution ResolveHeftAutomaticPartitionPolicy(
        const AutomaticPartitionPolicy& policy, const HeftAutomaticPartitionTuningPolicy& tuning,
        AutomaticPartitionPlanningShape shape, std::optional<AutomaticPartitionPlanningEstimate> estimate,
        uint64_t wholeElapsedNanoseconds, uint64_t expectedReuseCount) noexcept;

    /**
     * @brief Pure diagnostic evidence produced while constructing one HEFT physical plan.
     * @details Partition counters include every actual search performed by failed transactional replays and
     * local-search neighbor schedules, so they report total planner effort rather than only work retained by the final
     * schedule.
     */
    struct HeftPlanningStatistics {
        uint64_t baselineMakespanNanoseconds = 0;
        uint64_t partitionSearches = 0;
        uint64_t partitionEvaluationBudget = 0;
        uint64_t partitionEvaluations = 0;
        uint64_t acceptedPartitions = 0;
        uint32_t neighborEvaluations = 0;
        uint32_t acceptedMoves = 0;

        friend constexpr bool operator==(HeftPlanningStatistics, HeftPlanningStatistics) noexcept = default;
    };

    /** @brief Pure physical-plan storage and optimization evidence from one HEFT invocation. */
    struct HeftPlanningResult {
        GraphPlanSelection selection{};
        HeftPlanningStatistics statistics{};

        /** @brief Return whether bounded search found a strictly shorter predicted makespan. */
        [[nodiscard]] constexpr bool Improved() const noexcept {
            return selection.predictedMakespanNanoseconds < statistics.baselineMakespanNanoseconds;
        }
    };

    /** @brief Stable stage at which dispatch-DAG HEFT planning failed. */
    enum class HeftPlanningErrorCode : uint8_t {
        InvalidPolicy,
        InvalidGraph,
        UnsupportedOperation,
        UnsupportedBinding,
        ResidencySnapshotFailed,
        CandidateEnumerationFailed,
        RankEstimationFailed,
        NoCandidate,
        SchedulingFailed,
        ResidencyTransitionFailed,
        ResidencyLoweringFailed,
        ProgramUnavailable,
        CapacityExceeded,
        OutOfMemory,
    };

    /** @brief HEFT failure retaining the first logical node, argument, and nested stable cause. */
    struct HeftPlanningError {
        HeftPlanningErrorCode code = HeftPlanningErrorCode::InvalidGraph;
        GraphNodeId node{};
        KernelArgumentId argument{};
        uint64_t cause = 0;

        friend constexpr bool operator==(HeftPlanningError, HeftPlanningError) noexcept = default;
    };

    /**
     * @brief Pure heterogeneous earliest-finish-time planner for the validated dispatch-DAG graph class.
     * @details Upward ranks use average calibrated compute cost and communication cost conditioned on reachable
     * processor-coordinate pairs. Placement uses insertion-based exact-queue calendars and committed symbolic
     * residency. Service-scoped transfer-engine calendars serialize contending asynchronous hops. Optional bounded
     * local search re-evaluates stable resource-placement neighbors from the same frozen residency baseline and only
     * accepts strict predicted-makespan improvements. Buffer bindings may denote exact affine subcoverage; partial
     * final writes must remain graph-local because whole-value publication requires exact logical coverage. The
     * planner invokes no backend callback and performs no compilation, physical allocation, materialization, or
     * submission.
     */
    class HeftPlanner final {
    public:
        explicit HeftPlanner(
            HeftPlanningPolicy policy = {},
            std::shared_ptr<AutomaticPartitionPlanningCalibration> automaticPlanningCalibration = {}) noexcept
            : policy_(policy), automaticPlanningCalibration_(std::move(automaticPlanningCalibration)) {}

        /** @brief Build a deterministic HEFT plan together with bounded-search diagnostics. */
        [[nodiscard]] std::expected<HeftPlanningResult, HeftPlanningError>
        Plan(const ExecutionGraph& graph, const GraphPlanningContext& context,
             GraphPlanningServices services) const noexcept;

        /**
         * @brief Build a plan from @p calibration's frozen view and defer every new search observation into it.
         * @details This overload never reads or publishes through the planner's shared calibration model. Graph
         * portfolio evaluation can therefore give each variant a batch derived from one common snapshot and publish
         * the batches only after all physical objectives have been compared.
         */
        [[nodiscard]] std::expected<HeftPlanningResult, HeftPlanningError>
        Plan(const ExecutionGraph& graph, const GraphPlanningContext& context, GraphPlanningServices services,
             AutomaticPartitionPlanningCalibrationBatch& calibration) const noexcept;

        /** @brief Build a deterministic HEFT schedule and return its selected physical plan and objective. */
        [[nodiscard]] std::expected<GraphPlanSelection, HeftPlanningError>
        operator()(const ExecutionGraph& graph, const GraphPlanningContext& context,
                   GraphPlanningServices services) const noexcept;

        /** @brief Borrow the optional process-local model used to coordinate request-wide deferred observations. */
        [[nodiscard]] const std::shared_ptr<AutomaticPartitionPlanningCalibration>&
        AutomaticPlanningCalibration() const noexcept {
            return automaticPlanningCalibration_;
        }

    private:
        HeftPlanningPolicy policy_{};
        std::shared_ptr<AutomaticPartitionPlanningCalibration> automaticPlanningCalibration_{};
    };

    static_assert(GraphPlanningEngine<HeftPlanner>);
    static_assert(std::is_trivially_copyable_v<HeftLocalSearchPolicy>);
    static_assert(std::is_trivially_copyable_v<HeftAutomaticPartitionTuningPolicy>);
    static_assert(std::is_trivially_copyable_v<HeftAutomaticPartitionResolution>);
    static_assert(std::is_trivially_copyable_v<HeftPlanningStatistics>);
    static_assert(std::is_nothrow_move_constructible_v<HeftPlanningError>);

} // namespace Sora::Compute

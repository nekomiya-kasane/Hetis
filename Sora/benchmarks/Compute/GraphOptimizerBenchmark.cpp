/**
 * @file GraphOptimizerBenchmark.cpp
 * @brief Submission-scoped graph-fusion and bounded logical-portfolio control-plane baseline.
 * @ingroup Compute
 */

#include <BenchmarkHarness.h>
#include <Sora/Compute/Graph/GraphBuilder.h>
#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>
#include <Sora/Compute/Runtime/GraphPortfolio.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <print>
#include <utility>
#include <variant>
#include <vector>

namespace {

    constexpr size_t kSamples = 9;
    constexpr auto kMinimumSampleTime = std::chrono::milliseconds(50);
    constexpr uint64_t kElementCount = uint64_t{1} << 20U;
    constexpr size_t kLongChainDispatches = 8;
    constexpr size_t kIndependentRegions = 4;
    constexpr Sora::Compute::ValueType kFloat{Sora::Compute::ScalarKind::Float32, 1};
    constexpr Sora::Compute::GraphFusionPolicy kFusionPolicy{};
    constexpr Sora::Compute::GraphFusionPolicy kRegionBudgetPolicy{.maximumFusionRegions = 2};

    consteval auto MakeCopyKernel() {
        Sora::Compute::KernelBuilder<1, 1> scalar;
        const auto input = scalar.AddValueParameter(kFloat);
        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<2, 1, 1> program{domain, std::move(scalar).Finish(input),
                                                             Sora::Compute::PartitionContract::Elementwise(1)};
        const auto source = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, 1,
                                                      Sora::Compute::LayoutKind::Contiguous, 64, {1});
        const auto destination = program.AddBufferArgument(Sora::Compute::AccessMode::Write, kFloat, 1,
                                                           Sora::Compute::LayoutKind::Contiguous, 64, {2});
        program.BindArgument(source);
        return std::move(program).Finish(destination);
    }

    constexpr auto kCopyKernel = MakeCopyKernel();

    [[nodiscard]] std::optional<Sora::Compute::Buffer> MakeBuffer(Sora::Compute::BufferId id) noexcept {
        auto buffer = Sora::Compute::Buffer::Create({.id = id,
                                                     .elementType = kFloat,
                                                     .layout = Sora::Compute::LayoutKind::Contiguous,
                                                     .rank = 1,
                                                     .sizeBytes = kElementCount * kFloat.ByteSize(),
                                                     .alignment = 64});
        if (!buffer) {
            return std::nullopt;
        }
        return std::move(*buffer);
    }

    [[nodiscard]] std::optional<Sora::Compute::BufferView> View(Sora::Compute::Buffer buffer) noexcept {
        constexpr std::array extents{kElementCount};
        auto view = Sora::Compute::BufferView::Contiguous(std::move(buffer), extents);
        if (!view) {
            return std::nullopt;
        }
        return std::move(*view);
    }

    [[nodiscard]] std::optional<Sora::Compute::KernelInvocation> Invoke(Sora::Compute::BufferView input,
                                                                        Sora::Compute::BufferView output) noexcept {
        try {
            std::vector<Sora::Compute::KernelArgumentBinding> bindings;
            bindings.reserve(2);
            bindings.emplace_back(std::move(input));
            bindings.emplace_back(std::move(output));
            constexpr std::array extents{kElementCount};
            auto invocation = Sora::Compute::BindKernelProgram(kCopyKernel, extents, std::move(bindings));
            if (!invocation) {
                return std::nullopt;
            }
            return std::move(*invocation);
        } catch (const std::bad_alloc&) {
            return std::nullopt;
        }
    }

    [[nodiscard]] std::optional<Sora::Compute::ExecutionGraph> MakeChainGraph(uint64_t identity,
                                                                              size_t dispatches) noexcept {
        auto source = MakeBuffer({identity, 1});
        if (!source || dispatches < 2) {
            return std::nullopt;
        }
        auto previous = View(std::move(*source));
        if (!previous) {
            return std::nullopt;
        }
        Sora::Compute::GraphBuilder builder;
        for (size_t index = 0; index < dispatches; ++index) {
            auto buffer = MakeBuffer({identity, index + 2});
            if (!buffer) {
                return std::nullopt;
            }
            auto next = View(std::move(*buffer));
            if (!next) {
                return std::nullopt;
            }
            auto invocation = Invoke(*previous, *next);
            if (!invocation || !builder.AddDispatch(std::move(*invocation))) {
                return std::nullopt;
            }
            previous = std::move(next);
        }
        auto graph = std::move(builder).Finish();
        if (!graph) {
            return std::nullopt;
        }
        return std::move(*graph);
    }

    [[nodiscard]] std::optional<Sora::Compute::ExecutionGraph> MakeIndependentRegionGraph(uint64_t identity,
                                                                                          size_t regions) noexcept {
        if (regions == 0) {
            return std::nullopt;
        }
        Sora::Compute::GraphBuilder builder;
        for (size_t region = 0; region < regions; ++region) {
            const uint64_t first = region * 3 + 1;
            auto source = MakeBuffer({identity, first});
            auto intermediate = MakeBuffer({identity, first + 1});
            auto output = MakeBuffer({identity, first + 2});
            if (!source || !intermediate || !output) {
                return std::nullopt;
            }
            auto sourceView = View(std::move(*source));
            auto intermediateView = View(std::move(*intermediate));
            auto outputView = View(std::move(*output));
            if (!sourceView || !intermediateView || !outputView) {
                return std::nullopt;
            }
            auto producer = Invoke(std::move(*sourceView), *intermediateView);
            auto consumer = Invoke(std::move(*intermediateView), std::move(*outputView));
            if (!producer || !consumer || !builder.AddDispatch(std::move(*producer)) ||
                !builder.AddDispatch(std::move(*consumer))) {
                return std::nullopt;
            }
        }
        auto graph = std::move(builder).Finish();
        if (!graph) {
            return std::nullopt;
        }
        return std::move(*graph);
    }

    [[nodiscard]] std::optional<Sora::Compute::ExecutionGraph> MakeProfitableGraph() noexcept {
        return MakeChainGraph(1, 2);
    }

    [[nodiscard]] std::optional<Sora::Compute::ExecutionGraph> MakeNoOpGraph() noexcept {
        auto firstInput = MakeBuffer({2, 1});
        auto firstOutput = MakeBuffer({2, 2});
        auto secondInput = MakeBuffer({2, 3});
        auto secondOutput = MakeBuffer({2, 4});
        if (!firstInput || !firstOutput || !secondInput || !secondOutput) {
            return std::nullopt;
        }
        auto firstInputView = View(std::move(*firstInput));
        auto firstOutputView = View(std::move(*firstOutput));
        auto secondInputView = View(std::move(*secondInput));
        auto secondOutputView = View(std::move(*secondOutput));
        if (!firstInputView || !firstOutputView || !secondInputView || !secondOutputView) {
            return std::nullopt;
        }
        auto first = Invoke(std::move(*firstInputView), std::move(*firstOutputView));
        auto second = Invoke(std::move(*secondInputView), std::move(*secondOutputView));
        if (!first || !second) {
            return std::nullopt;
        }
        Sora::Compute::GraphBuilder builder;
        if (!builder.AddDispatch(std::move(*first)) || !builder.AddDispatch(std::move(*second))) {
            return std::nullopt;
        }
        auto graph = std::move(builder).Finish();
        if (!graph) {
            return std::nullopt;
        }
        return std::move(*graph);
    }

    [[nodiscard]] constexpr uint64_t MinimumProfitableReuseCount(uint64_t compilationNanoseconds,
                                                                 uint64_t benefitNanoseconds) noexcept {
        if (compilationNanoseconds == 0) {
            return 1;
        }
        if (benefitNanoseconds <= 1) {
            return std::numeric_limits<uint64_t>::max();
        }
        const uint64_t maximumAmortizedCost = benefitNanoseconds - 1;
        return compilationNanoseconds / maximumAmortizedCost +
               static_cast<uint64_t>(compilationNanoseconds % maximumAmortizedCost != 0);
    }

    class GraphOptimizerBenchmarkFixture final {
    public:
        GraphOptimizerBenchmarkFixture() {
            if (!packages_.Register(kCopyKernel)) {
                failed_ = true;
                return;
            }
            auto catalog = Sora::Compute::ProgramCatalogSnapshot::Create(packages_.Snapshot());
            auto profitable = MakeProfitableGraph();
            auto noOp = MakeNoOpGraph();
            auto longChain = MakeChainGraph(3, kLongChainDispatches);
            auto independent = MakeIndependentRegionGraph(4, kIndependentRegions);
            if (!catalog || !profitable || !noOp || !longChain || !independent) {
                failed_ = true;
                return;
            }
            catalog_.emplace(std::move(*catalog));
            profitable_.emplace(std::move(*profitable));
            noOp_.emplace(std::move(*noOp));
            longChain_.emplace(std::move(*longChain));
            independent_.emplace(std::move(*independent));
            Validate();
        }

        void Disabled() noexcept { InvokeCase(*profitable_, {.enabled = false}); }

        void NoOp() noexcept { InvokeCase(*noOp_, kFusionPolicy); }

        void ProfitableFusion() noexcept { InvokeCase(*profitable_, kFusionPolicy); }

        void LongChain() noexcept { InvokeCase(*longChain_, kFusionPolicy); }

        void IndependentRegions() noexcept { InvokeCase(*independent_, kFusionPolicy); }

        void RegionBudget() noexcept { InvokeCase(*independent_, kRegionBudgetPolicy); }

        void PortfolioDisabled() noexcept { InvokePortfolio(*profitable_, {.enabled = false}); }

        void PortfolioFused() noexcept { InvokePortfolio(*profitable_, kFusionPolicy); }

        [[nodiscard]] bool Failed() const noexcept { return failed_; }
        [[nodiscard]] uint64_t Checksum() const noexcept { return checksum_; }
        [[nodiscard]] const Sora::Compute::GraphOptimizationReport& FusionReport() const noexcept {
            return fusionReport_;
        }
        [[nodiscard]] const Sora::Compute::GraphOptimizationReport& LongChainReport() const noexcept {
            return longChainReport_;
        }
        [[nodiscard]] const Sora::Compute::GraphOptimizationReport& IndependentReport() const noexcept {
            return independentReport_;
        }
        [[nodiscard]] const Sora::Compute::GraphOptimizationReport& RegionBudgetReport() const noexcept {
            return regionBudgetReport_;
        }

    private:
        void InvokeCase(const Sora::Compute::ExecutionGraph& graph, Sora::Compute::GraphFusionPolicy policy) noexcept {
            try {
                auto result = Sora::Compute::OptimizeExecutionGraph(graph, *catalog_, policy);
                checksum_ += result.graph.NodeCount() + result.programs.PackageCount() +
                             static_cast<uint64_t>(result.report.status) + result.report.intermediateBytesSaved;
            } catch (const std::bad_alloc&) {
                failed_ = true;
            }
        }

        void InvokePortfolio(const Sora::Compute::ExecutionGraph& graph,
                             Sora::Compute::GraphFusionPolicy policy) noexcept {
            try {
                auto portfolio = Sora::Compute::GraphPortfolio::Create(graph, *catalog_, policy);
                checksum_ += portfolio.VariantCount() + portfolio.Original().graph.NodeCount() +
                             static_cast<uint64_t>(portfolio.Optimization().status);
            } catch (const std::bad_alloc&) {
                failed_ = true;
            }
        }

        void Validate() noexcept {
            try {
                auto disabled = Sora::Compute::OptimizeExecutionGraph(*profitable_, *catalog_, {.enabled = false});
                auto noOp = Sora::Compute::OptimizeExecutionGraph(*noOp_, *catalog_, kFusionPolicy);
                auto fused = Sora::Compute::OptimizeExecutionGraph(*profitable_, *catalog_, kFusionPolicy);
                auto longChain = Sora::Compute::OptimizeExecutionGraph(*longChain_, *catalog_, kFusionPolicy);
                auto independent = Sora::Compute::OptimizeExecutionGraph(*independent_, *catalog_, kFusionPolicy);
                auto budget = Sora::Compute::OptimizeExecutionGraph(*independent_, *catalog_, kRegionBudgetPolicy);
                if (disabled.report.status != Sora::Compute::GraphOptimizationStatus::Disabled ||
                    disabled.graph.NodeCount() != 2 ||
                    noOp.report.status != Sora::Compute::GraphOptimizationStatus::Unchanged ||
                    noOp.graph.NodeCount() != 2 ||
                    fused.report.status != Sora::Compute::GraphOptimizationStatus::Fused ||
                    fused.graph.NodeCount() != 1 || fused.programs.GeneratedProgramCount() != 1 ||
                    longChain.report.status != Sora::Compute::GraphOptimizationStatus::Fused ||
                    longChain.graph.NodeCount() != 1 || longChain.report.launchesRemoved != kLongChainDispatches - 1 ||
                    longChain.programs.GeneratedProgramCount() != 1 ||
                    independent.report.status != Sora::Compute::GraphOptimizationStatus::Fused ||
                    independent.graph.NodeCount() != kIndependentRegions ||
                    independent.report.regionsFused != kIndependentRegions ||
                    independent.programs.GeneratedProgramCount() != 1 ||
                    budget.report.status != Sora::Compute::GraphOptimizationStatus::Fused ||
                    budget.graph.NodeCount() != kIndependentRegions * 2 - kRegionBudgetPolicy.maximumFusionRegions ||
                    budget.report.regionsFused != kRegionBudgetPolicy.maximumFusionRegions ||
                    budget.programs.GeneratedProgramCount() != 1) {
                    failed_ = true;
                    return;
                }
                fusionReport_ = fused.report;
                longChainReport_ = longChain.report;
                independentReport_ = independent.report;
                regionBudgetReport_ = budget.report;
            } catch (const std::bad_alloc&) {
                failed_ = true;
            }
        }

        Sora::Compute::PortablePackageRegistry packages_{};
        std::optional<Sora::Compute::ProgramCatalogSnapshot> catalog_{};
        std::optional<Sora::Compute::ExecutionGraph> profitable_{};
        std::optional<Sora::Compute::ExecutionGraph> noOp_{};
        std::optional<Sora::Compute::ExecutionGraph> longChain_{};
        std::optional<Sora::Compute::ExecutionGraph> independent_{};
        Sora::Compute::GraphOptimizationReport fusionReport_{};
        Sora::Compute::GraphOptimizationReport longChainReport_{};
        Sora::Compute::GraphOptimizationReport independentReport_{};
        Sora::Compute::GraphOptimizationReport regionBudgetReport_{};
        uint64_t checksum_ = 0;
        bool failed_ = false;
    };

} // namespace

int main() {
    GraphOptimizerBenchmarkFixture fixture;
    if (fixture.Failed()) {
        return 2;
    }

    auto disabled = [&]() noexcept { fixture.Disabled(); };
    auto noOp = [&]() noexcept { fixture.NoOp(); };
    auto profitableFusion = [&]() noexcept { fixture.ProfitableFusion(); };
    auto longChain = [&]() noexcept { fixture.LongChain(); };
    auto independentRegions = [&]() noexcept { fixture.IndependentRegions(); };
    auto regionBudget = [&]() noexcept { fixture.RegionBudget(); };
    auto portfolioDisabled = [&]() noexcept { fixture.PortfolioDisabled(); };
    auto portfolioFused = [&]() noexcept { fixture.PortfolioFused(); };
    const std::array cases{Sora::Benchmark::MakeCase("disabled", disabled),
                           Sora::Benchmark::MakeCase("enabled_no_op", noOp),
                           Sora::Benchmark::MakeCase("profitable_fusion", profitableFusion),
                           Sora::Benchmark::MakeCase("long_chain", longChain),
                           Sora::Benchmark::MakeCase("independent_regions", independentRegions),
                           Sora::Benchmark::MakeCase("region_budget", regionBudget),
                           Sora::Benchmark::MakeCase("portfolio_disabled", portfolioDisabled),
                           Sora::Benchmark::MakeCase("portfolio_fused", portfolioFused)};
    const auto timing = Sora::Benchmark::MeasureInterleavedIndependent<kSamples>(cases, 1, kMinimumSampleTime);
    if (fixture.Failed() || fixture.Checksum() == 0) {
        return 3;
    }

    const double disabledNanoseconds = timing.MedianTime(0);
    const double noOpNanoseconds = timing.MedianTime(1);
    const double fusionNanoseconds = timing.MedianTime(2);
    const double longChainNanoseconds = timing.MedianTime(3);
    const double independentNanoseconds = timing.MedianTime(4);
    const double budgetNanoseconds = timing.MedianTime(5);
    const double portfolioDisabledNanoseconds = timing.MedianTime(6);
    const double portfolioFusedNanoseconds = timing.MedianTime(7);
    const auto& fusionReport = fixture.FusionReport();
    const auto& longChainReport = fixture.LongChainReport();
    const auto& independentReport = fixture.IndependentReport();
    const auto& budgetReport = fixture.RegionBudgetReport();
    const uint64_t crossover = MinimumProfitableReuseCount(kFusionPolicy.compilationLatencyNanoseconds,
                                                           fusionReport.estimatedBenefitNanoseconds);

    std::println("metric,value,unit");
    std::println("disabled,{:.6f},ns/graph", disabledNanoseconds);
    std::println("enabled_no_op,{:.6f},ns/graph", noOpNanoseconds);
    std::println("profitable_fusion,{:.6f},ns/graph", fusionNanoseconds);
    std::println("long_chain_8,{:.6f},ns/graph", longChainNanoseconds);
    std::println("independent_regions_4,{:.6f},ns/graph", independentNanoseconds);
    std::println("region_budget_2_of_4,{:.6f},ns/graph", budgetNanoseconds);
    std::println("portfolio_disabled,{:.6f},ns/graph", portfolioDisabledNanoseconds);
    std::println("portfolio_fused,{:.6f},ns/graph", portfolioFusedNanoseconds);
    std::println("portfolio_original_retention_increment,{:.6f},ns/graph",
                 std::max(0.0, portfolioFusedNanoseconds - fusionNanoseconds));
    std::println("no_op_discovery_increment,{:.6f},ns/graph", std::max(0.0, noOpNanoseconds - disabledNanoseconds));
    std::println("fusion_increment,{:.6f},ns/graph", std::max(0.0, fusionNanoseconds - disabledNanoseconds));
    std::println("long_chain_increment_per_dispatch,{:.6f},ns/input_dispatch",
                 std::max(0.0, longChainNanoseconds - disabledNanoseconds) / kLongChainDispatches);
    std::println("independent_increment_per_region,{:.6f},ns/input_region",
                 std::max(0.0, independentNanoseconds - disabledNanoseconds) / kIndependentRegions);
    std::println("enabled_no_op_over_disabled,{:.6f},ratio", timing.PairedRatio(1, 0));
    std::println("profitable_fusion_over_disabled,{:.6f},ratio", timing.PairedRatio(2, 0));
    std::println("long_chain_over_disabled,{:.6f},ratio", timing.PairedRatio(3, 0));
    std::println("independent_regions_over_disabled,{:.6f},ratio", timing.PairedRatio(4, 0));
    std::println("region_budget_over_disabled,{:.6f},ratio", timing.PairedRatio(5, 0));
    std::println("portfolio_disabled_over_disabled,{:.6f},ratio", timing.PairedRatio(6, 0));
    std::println("portfolio_fused_over_profitable_fusion,{:.6f},ratio", timing.PairedRatio(7, 2));
    std::println("pair_launches,2 -> 1,count");
    std::println("pair_intermediate_bytes_saved,{},bytes", fusionReport.intermediateBytesSaved);
    std::println("pair_estimated_benefit,{},ns/submission", fusionReport.estimatedBenefitNanoseconds);
    std::println("pair_amortized_compilation,{},ns/submission", fusionReport.amortizedCompilationNanoseconds);
    std::println("long_chain_launches,{} -> {},count", kLongChainDispatches,
                 kLongChainDispatches - longChainReport.launchesRemoved);
    std::println("long_chain_unique_programs,{},count", longChainReport.generatedPrograms);
    std::println("long_chain_intermediate_bytes_saved,{},bytes", longChainReport.intermediateBytesSaved);
    std::println("long_chain_estimated_benefit,{},ns/submission", longChainReport.estimatedBenefitNanoseconds);
    std::println("long_chain_amortized_compilation,{},ns/submission", longChainReport.amortizedCompilationNanoseconds);
    std::println("independent_launches,{} -> {},count", kIndependentRegions * 2,
                 kIndependentRegions * 2 - independentReport.launchesRemoved);
    std::println("independent_regions_fused,{},count", independentReport.regionsFused);
    std::println("independent_unique_programs,{},count", independentReport.generatedPrograms);
    std::println("independent_intermediate_bytes_saved,{},bytes", independentReport.intermediateBytesSaved);
    std::println("independent_estimated_benefit,{},ns/submission", independentReport.estimatedBenefitNanoseconds);
    std::println("independent_amortized_compilation,{},ns/submission",
                 independentReport.amortizedCompilationNanoseconds);
    std::println("budget_regions_fused,{},count", budgetReport.regionsFused);
    std::println("budget_launches_removed,{},count", budgetReport.launchesRemoved);
    std::println("budget_unique_programs,{},count", budgetReport.generatedPrograms);
    std::println("minimum_profitable_reuse,{},submissions", crossover);
    std::println("policy_expected_reuse,{},submissions", kFusionPolicy.expectedReuseCount);
    std::println("disabled_repetitions,{},count", timing.repetitions[0]);
    std::println("enabled_no_op_repetitions,{},count", timing.repetitions[1]);
    std::println("profitable_fusion_repetitions,{},count", timing.repetitions[2]);
    std::println("long_chain_repetitions,{},count", timing.repetitions[3]);
    std::println("independent_regions_repetitions,{},count", timing.repetitions[4]);
    std::println("region_budget_repetitions,{},count", timing.repetitions[5]);
    std::println("portfolio_disabled_repetitions,{},count", timing.repetitions[6]);
    std::println("portfolio_fused_repetitions,{},count", timing.repetitions[7]);
    std::println("checksum,{},value", fixture.Checksum());
    return 0;
}

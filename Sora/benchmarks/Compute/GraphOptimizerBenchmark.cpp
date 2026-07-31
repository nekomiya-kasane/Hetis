/**
 * @file GraphOptimizerBenchmark.cpp
 * @brief Submission-scoped graph-fusion control-plane and profitability baseline.
 * @ingroup Compute
 */

#include <BenchmarkHarness.h>
#include <Sora/Compute/Graph/GraphBuilder.h>
#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>
#include <Sora/Compute/Runtime/GraphOptimizer.h>

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
    constexpr Sora::Compute::ValueType kFloat{Sora::Compute::ScalarKind::Float32, 1};
    constexpr Sora::Compute::GraphFusionPolicy kFusionPolicy{};

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

    [[nodiscard]] std::optional<Sora::Compute::ExecutionGraph> MakeProfitableGraph() noexcept {
        auto source = MakeBuffer({1, 1});
        auto intermediate = MakeBuffer({1, 2});
        auto output = MakeBuffer({1, 3});
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
        if (!producer || !consumer) {
            return std::nullopt;
        }
        Sora::Compute::GraphBuilder builder;
        if (!builder.AddDispatch(std::move(*producer)) || !builder.AddDispatch(std::move(*consumer))) {
            return std::nullopt;
        }
        auto graph = std::move(builder).Finish();
        if (!graph) {
            return std::nullopt;
        }
        return std::move(*graph);
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
            if (!catalog || !profitable || !noOp) {
                failed_ = true;
                return;
            }
            catalog_.emplace(std::move(*catalog));
            profitable_.emplace(std::move(*profitable));
            noOp_.emplace(std::move(*noOp));
            Validate();
        }

        void Disabled() noexcept { InvokeCase(*profitable_, {.enabled = false}); }

        void NoOp() noexcept { InvokeCase(*noOp_, kFusionPolicy); }

        void ProfitableFusion() noexcept { InvokeCase(*profitable_, kFusionPolicy); }

        [[nodiscard]] bool Failed() const noexcept { return failed_; }
        [[nodiscard]] uint64_t Checksum() const noexcept { return checksum_; }
        [[nodiscard]] const Sora::Compute::GraphOptimizationReport& FusionReport() const noexcept {
            return fusionReport_;
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

        void Validate() noexcept {
            try {
                auto disabled = Sora::Compute::OptimizeExecutionGraph(*profitable_, *catalog_, {.enabled = false});
                auto noOp = Sora::Compute::OptimizeExecutionGraph(*noOp_, *catalog_, kFusionPolicy);
                auto fused = Sora::Compute::OptimizeExecutionGraph(*profitable_, *catalog_, kFusionPolicy);
                if (disabled.report.status != Sora::Compute::GraphOptimizationStatus::Disabled ||
                    disabled.graph.NodeCount() != 2 ||
                    noOp.report.status != Sora::Compute::GraphOptimizationStatus::Unchanged ||
                    noOp.graph.NodeCount() != 2 ||
                    fused.report.status != Sora::Compute::GraphOptimizationStatus::Fused ||
                    fused.graph.NodeCount() != 1 || fused.programs.GeneratedProgramCount() != 1) {
                    failed_ = true;
                    return;
                }
                fusionReport_ = fused.report;
            } catch (const std::bad_alloc&) {
                failed_ = true;
            }
        }

        Sora::Compute::PortablePackageRegistry packages_{};
        std::optional<Sora::Compute::ProgramCatalogSnapshot> catalog_{};
        std::optional<Sora::Compute::ExecutionGraph> profitable_{};
        std::optional<Sora::Compute::ExecutionGraph> noOp_{};
        Sora::Compute::GraphOptimizationReport fusionReport_{};
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
    const std::array cases{Sora::Benchmark::MakeCase("disabled", disabled),
                           Sora::Benchmark::MakeCase("enabled_no_op", noOp),
                           Sora::Benchmark::MakeCase("profitable_fusion", profitableFusion)};
    const auto timing = Sora::Benchmark::MeasureInterleavedIndependent<kSamples>(cases, 1, kMinimumSampleTime);
    if (fixture.Failed() || fixture.Checksum() == 0) {
        return 3;
    }

    const double disabledNanoseconds = timing.MedianTime(0);
    const double noOpNanoseconds = timing.MedianTime(1);
    const double fusionNanoseconds = timing.MedianTime(2);
    const auto& report = fixture.FusionReport();
    const uint64_t crossover =
        MinimumProfitableReuseCount(kFusionPolicy.compilationLatencyNanoseconds, report.estimatedBenefitNanoseconds);

    std::println("metric,value,unit");
    std::println("disabled,{:.6f},ns/graph", disabledNanoseconds);
    std::println("enabled_no_op,{:.6f},ns/graph", noOpNanoseconds);
    std::println("profitable_fusion,{:.6f},ns/graph", fusionNanoseconds);
    std::println("no_op_discovery_increment,{:.6f},ns/graph", std::max(0.0, noOpNanoseconds - disabledNanoseconds));
    std::println("fusion_increment,{:.6f},ns/graph", std::max(0.0, fusionNanoseconds - disabledNanoseconds));
    std::println("enabled_no_op_over_disabled,{:.6f},ratio", timing.PairedRatio(1, 0));
    std::println("profitable_fusion_over_disabled,{:.6f},ratio", timing.PairedRatio(2, 0));
    std::println("launches_before,2,count");
    std::println("launches_after,1,count");
    std::println("launches_removed,1,count");
    std::println("intermediate_bytes_saved,{},bytes", report.intermediateBytesSaved);
    std::println("estimated_benefit,{},ns/submission", report.estimatedBenefitNanoseconds);
    std::println("amortized_compilation,{},ns/submission", report.amortizedCompilationNanoseconds);
    std::println("minimum_profitable_reuse,{},submissions", crossover);
    std::println("policy_expected_reuse,{},submissions", kFusionPolicy.expectedReuseCount);
    std::println("disabled_repetitions,{},count", timing.repetitions[0]);
    std::println("enabled_no_op_repetitions,{},count", timing.repetitions[1]);
    std::println("profitable_fusion_repetitions,{},count", timing.repetitions[2]);
    std::println("checksum,{},value", fixture.Checksum());
    return 0;
}

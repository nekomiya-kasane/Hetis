#include <Sora/Compute/Graph/GraphBuilder.h>
#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>
#include <Sora/Compute/Runtime/GraphOptimizer.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <utility>
#include <variant>
#include <vector>

namespace {

    constexpr Sora::Compute::ValueType kFloat{Sora::Compute::ScalarKind::Float32, 1};

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

    consteval auto MakeAbsoluteKernel() {
        Sora::Compute::KernelBuilder<1, 2> scalar;
        const auto input = scalar.AddValueParameter(kFloat);
        const auto result = scalar.Unary(Sora::Compute::OpCode::Abs, input);
        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<2, 1, 2> program{domain, std::move(scalar).Finish(result),
                                                             Sora::Compute::PartitionContract::Elementwise(1)};
        const auto source = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, 1,
                                                      Sora::Compute::LayoutKind::Contiguous, 64, {1});
        const auto destination = program.AddBufferArgument(Sora::Compute::AccessMode::Write, kFloat, 1,
                                                           Sora::Compute::LayoutKind::Contiguous, 64, {2});
        program.BindArgument(source);
        return std::move(program).Finish(destination);
    }

    constexpr auto kAbsoluteKernel = MakeAbsoluteKernel();

    Sora::Compute::Buffer MakeBuffer(Sora::Compute::BufferId id, uint64_t count = 64) {
        auto buffer = Sora::Compute::Buffer::Create({.id = id,
                                                     .elementType = kFloat,
                                                     .layout = Sora::Compute::LayoutKind::Contiguous,
                                                     .rank = 1,
                                                     .sizeBytes = count * kFloat.ByteSize(),
                                                     .alignment = 64});
        REQUIRE(buffer.has_value());
        return std::move(*buffer);
    }

    Sora::Compute::BufferView View(Sora::Compute::Buffer buffer, uint64_t count = 64) {
        const std::array extents{count};
        auto view = Sora::Compute::BufferView::Contiguous(std::move(buffer), extents);
        REQUIRE(view.has_value());
        return std::move(*view);
    }

    Sora::Compute::KernelInvocation InvokeProgram(const auto& program, Sora::Compute::BufferView input,
                                                  Sora::Compute::BufferView output, uint64_t count = 64) {
        std::vector<Sora::Compute::KernelArgumentBinding> bindings;
        bindings.emplace_back(std::move(input));
        bindings.emplace_back(std::move(output));
        const std::array extents{count};
        auto invocation = Sora::Compute::BindKernelProgram(program, extents, std::move(bindings));
        REQUIRE(invocation.has_value());
        return std::move(*invocation);
    }

    Sora::Compute::KernelInvocation Invoke(Sora::Compute::BufferView input, Sora::Compute::BufferView output,
                                           uint64_t count = 64) {
        return InvokeProgram(kCopyKernel, std::move(input), std::move(output), count);
    }

    Sora::Compute::ExecutionGraph TwoNodeGraph(Sora::Compute::Buffer source, Sora::Compute::Buffer intermediate,
                                               Sora::Compute::Buffer output, uint64_t count = 64) {
        const auto intermediateView = View(intermediate, count);
        Sora::Compute::GraphBuilder builder;
        REQUIRE(builder.AddDispatch(Invoke(View(source, count), intermediateView, count)).has_value());
        REQUIRE(builder.AddDispatch(Invoke(intermediateView, View(output, count), count)).has_value());
        auto graph = std::move(builder).Finish();
        REQUIRE(graph.has_value());
        return std::move(*graph);
    }

    Sora::Compute::ExecutionGraph IndependentPairGraph(uint64_t domain) {
        auto firstSource = MakeBuffer({domain, 1});
        auto firstIntermediate = MakeBuffer({domain, 2});
        auto firstOutput = MakeBuffer({domain, 3});
        auto secondSource = MakeBuffer({domain, 4});
        auto secondIntermediate = MakeBuffer({domain, 5});
        auto secondOutput = MakeBuffer({domain, 6});
        const auto firstIntermediateView = View(firstIntermediate);
        const auto secondIntermediateView = View(secondIntermediate);
        Sora::Compute::GraphBuilder builder;
        REQUIRE(builder.AddDispatch(Invoke(View(firstSource), firstIntermediateView)).has_value());
        REQUIRE(builder.AddDispatch(Invoke(firstIntermediateView, View(firstOutput))).has_value());
        REQUIRE(builder.AddDispatch(Invoke(View(secondSource), secondIntermediateView)).has_value());
        REQUIRE(builder.AddDispatch(Invoke(secondIntermediateView, View(secondOutput))).has_value());
        auto graph = std::move(builder).Finish();
        REQUIRE(graph.has_value());
        return std::move(*graph);
    }

    Sora::Compute::ExecutionGraph IndependentMixedPairGraph(uint64_t domain) {
        auto firstSource = MakeBuffer({domain, 1});
        auto firstIntermediate = MakeBuffer({domain, 2});
        auto firstOutput = MakeBuffer({domain, 3});
        auto secondSource = MakeBuffer({domain, 4});
        auto secondIntermediate = MakeBuffer({domain, 5});
        auto secondOutput = MakeBuffer({domain, 6});
        const auto firstIntermediateView = View(firstIntermediate);
        const auto secondIntermediateView = View(secondIntermediate);
        Sora::Compute::GraphBuilder builder;
        REQUIRE(builder.AddDispatch(InvokeProgram(kCopyKernel, View(firstSource), firstIntermediateView)).has_value());
        REQUIRE(builder.AddDispatch(InvokeProgram(kCopyKernel, firstIntermediateView, View(firstOutput))).has_value());
        REQUIRE(builder.AddDispatch(InvokeProgram(kAbsoluteKernel, View(secondSource), secondIntermediateView))
                    .has_value());
        REQUIRE(builder.AddDispatch(InvokeProgram(kAbsoluteKernel, secondIntermediateView, View(secondOutput)))
                    .has_value());
        auto graph = std::move(builder).Finish();
        REQUIRE(graph.has_value());
        return std::move(*graph);
    }

    Sora::Compute::ExecutionGraph ChainGraph(uint64_t domain, size_t dispatches, const auto& program) {
        REQUIRE(dispatches >= 2);
        Sora::Compute::GraphBuilder builder;
        auto previous = View(MakeBuffer({domain, 1}));
        for (size_t index = 0; index < dispatches; ++index) {
            auto next = View(MakeBuffer({domain, index + 2}));
            REQUIRE(builder.AddDispatch(InvokeProgram(program, previous, next)).has_value());
            previous = std::move(next);
        }
        auto graph = std::move(builder).Finish();
        REQUIRE(graph.has_value());
        return std::move(*graph);
    }

    Sora::Compute::GraphFusionPolicy AlwaysFuse() {
        return {.launchLatencyNanoseconds = 1,
                .memoryBandwidthBytesPerSecond = 1,
                .compilationLatencyNanoseconds = 0,
                .expectedReuseCount = 1};
    }

    Sora::Compute::ProgramCatalogSnapshot Catalog(const Sora::Compute::PortablePackageRegistry& packages,
                                                  Sora::Compute::ProgramCatalogLimits limits = {}) {
        auto catalog = Sora::Compute::ProgramCatalogSnapshot::Create(packages.Snapshot(), limits);
        REQUIRE(catalog.has_value());
        return std::move(*catalog);
    }

} // namespace

TEST_CASE("ProgramCatalog transactionally owns a bounded canonical set of generated programs",
          "[Sora][Compute][Runtime][ProgramCatalog][Capacity]") {
    Sora::Compute::PortablePackageRegistry packages;
    auto copy = Sora::Compute::ProgramPackage::Create(kCopyKernel);
    auto absolute = Sora::Compute::ProgramPackage::Create(kAbsoluteKernel);
    REQUIRE(copy.has_value());
    REQUIRE(absolute.has_value());

    auto catalog = Catalog(packages, {.maximumGeneratedPrograms = 2, .maximumGeneratedBytes = 1'024 * 1'024});
    REQUIRE(catalog.GeneratedProgramCapacity() == 2);
    auto generated = catalog.WithGenerated(std::vector{*copy, *absolute});
    REQUIRE(generated.has_value());
    REQUIRE(generated->GeneratedProgramCount() == 2);
    REQUIRE(generated->GeneratedBytes() == copy->Bytes().size() + absolute->Bytes().size());
    REQUIRE(generated->Resolve(copy->Hash()).has_value());
    REQUIRE(generated->Resolve(absolute->Hash()).has_value());

    auto duplicate = generated->WithGenerated(std::vector{*absolute, *copy});
    REQUIRE(duplicate.has_value());
    REQUIRE(duplicate->GeneratedProgramCount() == 2);
    REQUIRE(duplicate->GeneratedBytes() == generated->GeneratedBytes());

    auto overflow = Catalog(packages, {.maximumGeneratedPrograms = 1, .maximumGeneratedBytes = 1'024 * 1'024})
                        .WithGenerated(std::vector{*copy, *absolute});
    REQUIRE_FALSE(overflow.has_value());
    REQUIRE(overflow.error().code == Sora::Compute::ProgramCatalogErrorCode::CapacityExceeded);
    REQUIRE(overflow.error().program == absolute->Hash());
    REQUIRE(catalog.GeneratedProgramCount() == 0);
}

TEST_CASE("ProgramCatalog rejects invalid limits and byte overflow without partial publication",
          "[Sora][Compute][Runtime][ProgramCatalog][Atomicity]") {
    Sora::Compute::PortablePackageRegistry packages;
    auto copy = Sora::Compute::ProgramPackage::Create(kCopyKernel);
    auto absolute = Sora::Compute::ProgramPackage::Create(kAbsoluteKernel);
    REQUIRE(copy.has_value());
    REQUIRE(absolute.has_value());

    auto invalid = Sora::Compute::ProgramCatalogSnapshot::Create(
        packages.Snapshot(),
        {.maximumGeneratedPrograms = static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 1});
    REQUIRE_FALSE(invalid.has_value());
    REQUIRE(invalid.error().code == Sora::Compute::ProgramCatalogErrorCode::InvalidSnapshot);

    const uint64_t byteLimit = copy->Bytes().size() + absolute->Bytes().size() - 1;
    auto catalog = Catalog(packages, {.maximumGeneratedPrograms = 2, .maximumGeneratedBytes = byteLimit});
    auto overflow = catalog.WithGenerated(std::vector{*copy, *absolute});
    REQUIRE_FALSE(overflow.has_value());
    REQUIRE(overflow.error().code == Sora::Compute::ProgramCatalogErrorCode::CapacityExceeded);
    REQUIRE(catalog.GeneratedProgramCount() == 0);
    REQUIRE(catalog.GeneratedBytes() == 0);
    REQUIRE(catalog.AvailableGeneratedBytes() == byteLimit);
}

TEST_CASE("GraphOptimizer fuses one profitable pair into a submission-scoped program catalog",
          "[Sora][Compute][Runtime][GraphOptimizer]") {
    auto source = MakeBuffer({1, 1});
    auto intermediate = MakeBuffer({1, 2});
    auto output = MakeBuffer({1, 3});
    Sora::Compute::PortablePackageRegistry packages;
    REQUIRE(packages.Register(kCopyKernel).has_value());

    auto optimized = Sora::Compute::OptimizeExecutionGraph(TwoNodeGraph(source, intermediate, output),
                                                           Catalog(packages), AlwaysFuse());
    REQUIRE(optimized.report.status == Sora::Compute::GraphOptimizationStatus::Fused);
    REQUIRE(optimized.report.issue == Sora::Compute::GraphOptimizationIssue::None);
    REQUIRE(optimized.report.opportunitiesFound == 1);
    REQUIRE(optimized.report.regionsConsidered == 1);
    REQUIRE(optimized.report.regionsFused == 1);
    REQUIRE(optimized.report.dispatchesFused == 2);
    REQUIRE(optimized.report.launchesRemoved == 1);
    REQUIRE(optimized.report.generatedPrograms == 1);
    REQUIRE(optimized.report.intermediateBytesSaved == 64 * sizeof(float) * 2);
    REQUIRE(optimized.graph.NodeCount() == 1);
    REQUIRE(optimized.graph.EdgeCount() == 0);

    const auto* invocation = std::get_if<Sora::Compute::KernelInvocation>(&optimized.graph.Operation({0}));
    REQUIRE(invocation != nullptr);
    REQUIRE(invocation->Bindings().size() == 2);
    REQUIRE(std::get<Sora::Compute::BufferView>(invocation->Bindings()[0]).BufferDescriptor().id ==
            source.Describe().id);
    REQUIRE(std::get<Sora::Compute::BufferView>(invocation->Bindings()[1]).BufferDescriptor().id ==
            output.Describe().id);
    REQUIRE(invocation->ResourceDemand().bytesRead == sizeof(float));
    REQUIRE(invocation->ResourceDemand().bytesWritten == sizeof(float));
    REQUIRE(optimized.programs.Resolve(invocation->ProgramHash()).has_value());
    REQUIRE(optimized.programs.PackageCount() == 2);
    REQUIRE(optimized.programs.GeneratedProgramCount() == 1);
    REQUIRE_FALSE(packages.Resolve(invocation->ProgramHash()).has_value());
    REQUIRE(packages.Snapshot()->PackageCount() == 1);
}

TEST_CASE("GraphOptimizer retains an unprofitable graph without publishing generated state",
          "[Sora][Compute][Runtime][GraphOptimizer][Profitability]") {
    Sora::Compute::PortablePackageRegistry packages;
    REQUIRE(packages.Register(kCopyKernel).has_value());
    const auto packagesBefore = packages.Snapshot()->PackageCount();

    auto optimized = Sora::Compute::OptimizeExecutionGraph(
        TwoNodeGraph(MakeBuffer({2, 1}), MakeBuffer({2, 2}), MakeBuffer({2, 3})), Catalog(packages),
        {.launchLatencyNanoseconds = 0,
         .memoryBandwidthBytesPerSecond = std::numeric_limits<uint64_t>::max(),
         .compilationLatencyNanoseconds = 1'000'000,
         .expectedReuseCount = 1});
    REQUIRE(optimized.report.status == Sora::Compute::GraphOptimizationStatus::SkippedUnprofitable);
    REQUIRE(optimized.graph.NodeCount() == 2);
    REQUIRE(optimized.programs.GeneratedProgramCount() == 0);
    REQUIRE(packages.Snapshot()->PackageCount() == packagesBefore);
}

TEST_CASE("GraphOptimizer disabled policy bypasses structural discovery and preserves the base catalog",
          "[Sora][Compute][Runtime][GraphOptimizer][Disabled]") {
    Sora::Compute::PortablePackageRegistry packages;
    REQUIRE(packages.Register(kCopyKernel).has_value());

    auto optimized =
        Sora::Compute::OptimizeExecutionGraph(TwoNodeGraph(MakeBuffer({4, 1}), MakeBuffer({4, 2}), MakeBuffer({4, 3})),
                                              Catalog(packages), {.enabled = false});
    REQUIRE(optimized.report.status == Sora::Compute::GraphOptimizationStatus::Disabled);
    REQUIRE(optimized.report.issue == Sora::Compute::GraphOptimizationIssue::None);
    REQUIRE(optimized.graph.NodeCount() == 2);
    REQUIRE(optimized.programs.PackageCount() == 1);
    REQUIRE(optimized.programs.GeneratedProgramCount() == 0);
}

TEST_CASE("GraphOptimizer generated programs cannot bypass parent admission revocation",
          "[Sora][Compute][Runtime][GraphOptimizer][Trust]") {
    auto program = Sora::Compute::ProgramPackage::Create(kCopyKernel);
    REQUIRE(program.has_value());
    auto package = Sora::Compute::PortablePackage::Create(std::move(*program), {});
    REQUIRE(package.has_value());
    constexpr Sora::Compute::PortablePackageTrustGrant grant{
        .scheme = 1,
        .authority = {0xA1, 0xA2},
        .subject = {0xB1, 0xB2},
        .policyRevision = 7,
    };
    Sora::Compute::PortablePackageRegistry packages;
    REQUIRE(
        packages.Register(std::move(*package), Sora::Compute::PortablePackageProvenance::Verified(grant)).has_value());

    auto inFlight = Sora::Compute::OptimizeExecutionGraph(
        TwoNodeGraph(MakeBuffer({5, 1}), MakeBuffer({5, 2}), MakeBuffer({5, 3})), Catalog(packages), AlwaysFuse());
    REQUIRE(inFlight.report.status == Sora::Compute::GraphOptimizationStatus::Fused);
    const auto* invocation = std::get_if<Sora::Compute::KernelInvocation>(&inFlight.graph.Operation({0}));
    REQUIRE(invocation != nullptr);
    const auto fusedProgram = invocation->ProgramHash();

    auto revoked = packages.RevokeAdmissions(
        Sora::Compute::PortablePackageAdmissionRevocation::ForSubject(grant.scheme, grant.authority, grant.subject));
    REQUIRE(revoked.has_value());
    REQUIRE(revoked->packagesErased == 1);
    REQUIRE(packages.Snapshot()->PackageCount() == 0);
    REQUIRE(inFlight.programs.Resolve(kCopyKernel.Hash()).has_value());
    REQUIRE(inFlight.programs.Resolve(fusedProgram).has_value());

    auto afterRevocation = Sora::Compute::OptimizeExecutionGraph(
        TwoNodeGraph(MakeBuffer({6, 1}), MakeBuffer({6, 2}), MakeBuffer({6, 3})), Catalog(packages), AlwaysFuse());
    REQUIRE(afterRevocation.report.status == Sora::Compute::GraphOptimizationStatus::Failed);
    REQUIRE(afterRevocation.report.issue == Sora::Compute::GraphOptimizationIssue::ProgramResolution);
    REQUIRE(afterRevocation.graph.NodeCount() == 2);
    REQUIRE(afterRevocation.programs.GeneratedProgramCount() == 0);
}

TEST_CASE("GraphOptimizer folds one maximal elementwise chain into one final generated program",
          "[Sora][Compute][Runtime][GraphOptimizer][Chain]") {
    auto source = MakeBuffer({3, 1});
    auto first = MakeBuffer({3, 2});
    auto second = MakeBuffer({3, 3});
    auto output = MakeBuffer({3, 4});
    const auto firstView = View(first);
    const auto secondView = View(second);
    Sora::Compute::GraphBuilder builder;
    REQUIRE(builder.AddDispatch(Invoke(View(source), firstView)).has_value());
    REQUIRE(builder.AddDispatch(Invoke(firstView, secondView)).has_value());
    REQUIRE(builder.AddDispatch(Invoke(secondView, View(output))).has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());

    Sora::Compute::PortablePackageRegistry packages;
    REQUIRE(packages.Register(kCopyKernel).has_value());
    auto optimized = Sora::Compute::OptimizeExecutionGraph(std::move(*graph), Catalog(packages), AlwaysFuse());
    REQUIRE(optimized.report.status == Sora::Compute::GraphOptimizationStatus::Fused);
    REQUIRE(optimized.report.opportunitiesFound == 2);
    REQUIRE(optimized.report.regionsFused == 1);
    REQUIRE(optimized.report.dispatchesFused == 3);
    REQUIRE(optimized.report.launchesRemoved == 2);
    REQUIRE(optimized.report.generatedPrograms == 1);
    REQUIRE(optimized.report.intermediateBytesSaved == 2 * 64 * sizeof(float) * 2);
    REQUIRE(optimized.graph.NodeCount() == 1);
    REQUIRE(optimized.graph.EdgeCount() == 0);
    REQUIRE(optimized.programs.GeneratedProgramCount() == 1);
}

TEST_CASE("GraphOptimizer partitions a chain into maximal feasible regions at compiler capacity boundaries",
          "[Sora][Compute][Runtime][GraphOptimizer][Chain][Capacity]") {
    Sora::Compute::PortablePackageRegistry packages;
    REQUIRE(packages.Register(kAbsoluteKernel).has_value());
    auto policy = AlwaysFuse();
    policy.generatedProgramLimits.maximumInstructions = 3;

    auto optimized =
        Sora::Compute::OptimizeExecutionGraph(ChainGraph(9, 4, kAbsoluteKernel), Catalog(packages), policy);
    REQUIRE(optimized.report.status == Sora::Compute::GraphOptimizationStatus::Fused);
    REQUIRE(optimized.report.opportunitiesFound == 3);
    REQUIRE(optimized.report.regionsConsidered == 2);
    REQUIRE(optimized.report.regionsFused == 2);
    REQUIRE(optimized.report.dispatchesFused == 4);
    REQUIRE(optimized.report.launchesRemoved == 2);
    REQUIRE(optimized.report.generatedPrograms == 1);
    REQUIRE(optimized.graph.NodeCount() == 2);
    REQUIRE(optimized.graph.EdgeCount() == 1);
    REQUIRE(optimized.programs.GeneratedProgramCount() == 1);
}

TEST_CASE("GraphOptimizer fuses multiple independent regions and deduplicates identical generated programs",
          "[Sora][Compute][Runtime][GraphOptimizer][Regions]") {
    Sora::Compute::PortablePackageRegistry packages;
    REQUIRE(packages.Register(kCopyKernel).has_value());

    auto optimized = Sora::Compute::OptimizeExecutionGraph(IndependentPairGraph(7), Catalog(packages), AlwaysFuse());
    REQUIRE(optimized.report.status == Sora::Compute::GraphOptimizationStatus::Fused);
    REQUIRE(optimized.report.opportunitiesFound == 2);
    REQUIRE(optimized.report.regionsConsidered == 2);
    REQUIRE(optimized.report.regionsFused == 2);
    REQUIRE(optimized.report.dispatchesFused == 4);
    REQUIRE(optimized.report.launchesRemoved == 2);
    REQUIRE(optimized.report.generatedPrograms == 1);
    REQUIRE(optimized.graph.NodeCount() == 2);
    REQUIRE(optimized.graph.EdgeCount() == 0);
    REQUIRE(optimized.programs.GeneratedProgramCount() == 1);
}

TEST_CASE("GraphOptimizer amortizes one generated program across collectively profitable regions",
          "[Sora][Compute][Runtime][GraphOptimizer][Regions][Amortization]") {
    Sora::Compute::PortablePackageRegistry packages;
    REQUIRE(packages.Register(kCopyKernel).has_value());
    const Sora::Compute::GraphFusionPolicy policy{
        .launchLatencyNanoseconds = 60,
        .memoryBandwidthBytesPerSecond = std::numeric_limits<uint64_t>::max(),
        .compilationLatencyNanoseconds = 100,
        .expectedReuseCount = 1,
    };

    auto optimized = Sora::Compute::OptimizeExecutionGraph(IndependentPairGraph(10), Catalog(packages), policy);
    REQUIRE(optimized.report.status == Sora::Compute::GraphOptimizationStatus::Fused);
    REQUIRE(optimized.report.regionsFused == 2);
    REQUIRE(optimized.report.generatedPrograms == 1);
    REQUIRE(optimized.report.estimatedBenefitNanoseconds > optimized.report.amortizedCompilationNanoseconds);
    REQUIRE(optimized.report.amortizedCompilationNanoseconds == 100);
    REQUIRE(optimized.graph.NodeCount() == 2);
    REQUIRE(optimized.programs.GeneratedProgramCount() == 1);
}

TEST_CASE("GraphOptimizer groups distinct generated identities under the catalog program budget",
          "[Sora][Compute][Runtime][GraphOptimizer][Regions][ProgramBudget]") {
    Sora::Compute::PortablePackageRegistry packages;
    REQUIRE(packages.Register(kCopyKernel).has_value());
    REQUIRE(packages.Register(kAbsoluteKernel).has_value());

    SECTION("both identities are published when the catalog admits both") {
        auto optimized =
            Sora::Compute::OptimizeExecutionGraph(IndependentMixedPairGraph(14), Catalog(packages), AlwaysFuse());
        REQUIRE(optimized.report.status == Sora::Compute::GraphOptimizationStatus::Fused);
        REQUIRE(optimized.report.regionsFused == 2);
        REQUIRE(optimized.report.generatedPrograms == 2);
        REQUIRE(optimized.graph.NodeCount() == 2);
        REQUIRE(optimized.programs.GeneratedProgramCount() == 2);
    }

    SECTION("one remaining catalog slot selects one complete region transactionally") {
        constexpr Sora::Compute::ProgramCatalogLimits limits{.maximumGeneratedPrograms = 1,
                                                             .maximumGeneratedBytes = 1'024 * 1'024};
        auto optimized = Sora::Compute::OptimizeExecutionGraph(IndependentMixedPairGraph(15), Catalog(packages, limits),
                                                               AlwaysFuse());
        REQUIRE(optimized.report.status == Sora::Compute::GraphOptimizationStatus::Fused);
        REQUIRE(optimized.report.regionsFused == 1);
        REQUIRE(optimized.report.generatedPrograms == 1);
        REQUIRE(optimized.graph.NodeCount() == 3);
        REQUIRE(optimized.programs.GeneratedProgramCount() == 1);

        const auto selectedGeneratedHash = [](const Sora::Compute::ExecutionGraph& graph) {
            for (uint32_t index = 0; index < graph.NodeCount(); ++index) {
                const auto* invocation =
                    std::get_if<Sora::Compute::KernelInvocation>(&graph.Operation(Sora::Compute::GraphNodeId{index}));
                if (invocation != nullptr && invocation->ProgramHash() != kCopyKernel.Hash() &&
                    invocation->ProgramHash() != kAbsoluteKernel.Hash()) {
                    return invocation->ProgramHash();
                }
            }
            return Sora::Compute::SemanticHash{};
        };
        const auto selected = selectedGeneratedHash(optimized.graph);
        REQUIRE(Sora::Compute::ValidSemanticHash(selected));

        auto repeated = Sora::Compute::OptimizeExecutionGraph(IndependentMixedPairGraph(16), Catalog(packages, limits),
                                                              AlwaysFuse());
        REQUIRE(repeated.report.status == Sora::Compute::GraphOptimizationStatus::Fused);
        REQUIRE(repeated.report.regionsFused == 1);
        REQUIRE(selectedGeneratedHash(repeated.graph) == selected);
    }
}

TEST_CASE("GraphOptimizer reuses an existing generated identity without consuming publication budget",
          "[Sora][Compute][Runtime][GraphOptimizer][ProgramReuse]") {
    Sora::Compute::PortablePackageRegistry packages;
    REQUIRE(packages.Register(kCopyKernel).has_value());
    auto first = Sora::Compute::OptimizeExecutionGraph(
        TwoNodeGraph(MakeBuffer({11, 1}), MakeBuffer({11, 2}), MakeBuffer({11, 3})), Catalog(packages), AlwaysFuse());
    REQUIRE(first.report.status == Sora::Compute::GraphOptimizationStatus::Fused);
    REQUIRE(first.programs.GeneratedProgramCount() == 1);

    auto second = Sora::Compute::OptimizeExecutionGraph(
        TwoNodeGraph(MakeBuffer({12, 1}), MakeBuffer({12, 2}), MakeBuffer({12, 3})), std::move(first.programs),
        AlwaysFuse());
    REQUIRE(second.report.status == Sora::Compute::GraphOptimizationStatus::Fused);
    REQUIRE(second.report.generatedPrograms == 1);
    REQUIRE(second.programs.GeneratedProgramCount() == 1);
    REQUIRE(second.programs.PackageCount() == 2);
}

TEST_CASE("GraphOptimizer applies the explicit region budget without partially publishing a second region",
          "[Sora][Compute][Runtime][GraphOptimizer][Budget]") {
    Sora::Compute::PortablePackageRegistry packages;
    REQUIRE(packages.Register(kCopyKernel).has_value());
    auto policy = AlwaysFuse();
    policy.maximumFusionRegions = 1;

    auto optimized = Sora::Compute::OptimizeExecutionGraph(IndependentPairGraph(8), Catalog(packages), policy);
    REQUIRE(optimized.report.status == Sora::Compute::GraphOptimizationStatus::Fused);
    REQUIRE(optimized.report.regionsConsidered == 2);
    REQUIRE(optimized.report.regionsFused == 1);
    REQUIRE(optimized.report.dispatchesFused == 2);
    REQUIRE(optimized.graph.NodeCount() == 3);
    REQUIRE(optimized.programs.GeneratedProgramCount() == 1);
}

TEST_CASE("GraphOptimizer reports exhausted generated-program budgets as a normal skip",
          "[Sora][Compute][Runtime][GraphOptimizer][Budget]") {
    Sora::Compute::PortablePackageRegistry packages;
    REQUIRE(packages.Register(kCopyKernel).has_value());
    auto catalog = Catalog(packages, {.maximumGeneratedPrograms = 0, .maximumGeneratedBytes = 0});

    auto optimized = Sora::Compute::OptimizeExecutionGraph(
        TwoNodeGraph(MakeBuffer({13, 1}), MakeBuffer({13, 2}), MakeBuffer({13, 3})), std::move(catalog), AlwaysFuse());
    REQUIRE(optimized.report.status == Sora::Compute::GraphOptimizationStatus::SkippedBudget);
    REQUIRE(optimized.report.issue == Sora::Compute::GraphOptimizationIssue::None);
    REQUIRE(optimized.report.regionsFused == 0);
    REQUIRE(optimized.graph.NodeCount() == 2);
    REQUIRE(optimized.programs.GeneratedProgramCount() == 0);
}

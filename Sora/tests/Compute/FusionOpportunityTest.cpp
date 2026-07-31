#include <Sora/Compute/Graph/FusionOpportunity.h>
#include <Sora/Compute/Graph/GraphBuilder.h>
#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <utility>
#include <vector>

namespace {

    constexpr Sora::Compute::ValueType kFloat{Sora::Compute::ScalarKind::Float32, 1};

    consteval auto MakeCopyKernel() {
        Sora::Compute::KernelBuilder<1, 1> scalar;
        const auto input = scalar.AddValueParameter(kFloat);
        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<2, 1, 1> program{domain, std::move(scalar).Finish(input),
                                                             Sora::Compute::PartitionContract::Elementwise(1)};
        const auto source = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, 1);
        const auto destination = program.AddBufferArgument(Sora::Compute::AccessMode::Write, kFloat, 1);
        program.BindArgument(source);
        return std::move(program).Finish(destination);
    }

    constexpr auto kCopyKernel = MakeCopyKernel();

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

    Sora::Compute::KernelInvocation Invoke(Sora::Compute::BufferView input, Sora::Compute::BufferView output,
                                           uint64_t count = 64) {
        std::vector<Sora::Compute::KernelArgumentBinding> bindings;
        bindings.emplace_back(std::move(input));
        bindings.emplace_back(std::move(output));
        const std::array extents{count};
        auto invocation = Sora::Compute::BindKernelProgram(kCopyKernel, extents, std::move(bindings));
        REQUIRE(invocation.has_value());
        return std::move(*invocation);
    }

} // namespace

TEST_CASE("Fusion opportunity discovery proves one unique exact-view elementwise intermediate",
          "[Sora][Compute][Graph][Fusion]") {
    auto source = MakeBuffer({1, 1});
    auto intermediate = MakeBuffer({1, 2});
    auto output = MakeBuffer({1, 3});
    const auto intermediateView = View(intermediate);

    Sora::Compute::GraphBuilder builder;
    const auto producer = builder.AddDispatch(Invoke(View(source), intermediateView));
    const auto consumer = builder.AddDispatch(Invoke(intermediateView, View(output)));
    REQUIRE(producer.has_value());
    REQUIRE(consumer.has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());

    const auto opportunities = Sora::Compute::FindElementwiseFusionOpportunities(*graph);
    REQUIRE(opportunities.has_value());
    REQUIRE(opportunities->size() == 1);
    REQUIRE(opportunities->front().producer == *producer);
    REQUIRE(opportunities->front().consumer == *consumer);
    REQUIRE(opportunities->front().producerOutput == Sora::Compute::KernelArgumentId{1});
    REQUIRE(opportunities->front().consumerInput == Sora::Compute::KernelArgumentId{0});
    REQUIRE(opportunities->front().intermediate == intermediate.Describe().id);
}

TEST_CASE("Fusion opportunity discovery rejects fanout and non-identical intermediate views",
          "[Sora][Compute][Graph][Fusion][Legality]") {
    SECTION("fanout") {
        auto source = MakeBuffer({2, 1});
        auto intermediate = MakeBuffer({2, 2});
        auto firstOutput = MakeBuffer({2, 3});
        auto secondOutput = MakeBuffer({2, 4});
        const auto intermediateView = View(intermediate);

        Sora::Compute::GraphBuilder builder;
        REQUIRE(builder.AddDispatch(Invoke(View(source), intermediateView)).has_value());
        REQUIRE(builder.AddDispatch(Invoke(intermediateView, View(firstOutput))).has_value());
        REQUIRE(builder.AddDispatch(Invoke(intermediateView, View(secondOutput))).has_value());
        auto graph = std::move(builder).Finish();
        REQUIRE(graph.has_value());
        const auto opportunities = Sora::Compute::FindElementwiseFusionOpportunities(*graph);
        REQUIRE(opportunities.has_value());
        REQUIRE(opportunities->empty());
    }

    SECTION("different views") {
        auto source = MakeBuffer({3, 1}, 128);
        auto intermediate = MakeBuffer({3, 2}, 128);
        auto output = MakeBuffer({3, 3}, 64);
        const auto whole = View(intermediate, 128);
        const std::array origin{0ULL};
        const std::array extents{64ULL};
        auto half = whole.SubView(origin, extents);
        REQUIRE(half.has_value());

        Sora::Compute::GraphBuilder builder;
        REQUIRE(builder.AddDispatch(Invoke(View(source, 128), whole, 128)).has_value());
        REQUIRE(builder.AddDispatch(Invoke(std::move(*half), View(output), 64)).has_value());
        auto graph = std::move(builder).Finish();
        REQUIRE(graph.has_value());
        const auto opportunities = Sora::Compute::FindElementwiseFusionOpportunities(*graph);
        REQUIRE(opportunities.has_value());
        REQUIRE(opportunities->empty());
    }
}

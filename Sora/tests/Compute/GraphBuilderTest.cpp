#include <Sora/Compute/Graph/GraphBuilder.h>
#include <Sora/Compute/Graph/HazardAnalysis.h>
#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <variant>
#include <vector>

namespace {

    constexpr Sora::Compute::ValueType kFloat{Sora::Compute::ScalarKind::Float32, 1};

    Sora::Compute::Buffer MakeBuffer(Sora::Compute::BufferId id, uint64_t size = 256) {
        auto buffer = Sora::Compute::Buffer::Create({
            .id = id,
            .elementType = kFloat,
            .layout = Sora::Compute::LayoutKind::Contiguous,
            .rank = 1,
            .sizeBytes = size,
            .alignment = 64,
        });
        REQUIRE(buffer.has_value());
        return *buffer;
    }

    Sora::Compute::BufferSlice Slice(Sora::Compute::Buffer buffer, uint64_t offset, uint64_t size) {
        auto slice =
            Sora::Compute::BufferSlice::Create(std::move(buffer), *Sora::Compute::ByteRegion::Make(offset, size));
        REQUIRE(slice.has_value());
        return std::move(*slice);
    }

    Sora::Compute::BufferView View(Sora::Compute::Buffer buffer, uint64_t count) {
        const std::array extents{count};
        auto view = Sora::Compute::BufferView::Contiguous(std::move(buffer), extents);
        REQUIRE(view.has_value());
        return std::move(*view);
    }

    constexpr Sora::Compute::FillPattern kPattern = [] consteval {
        constexpr std::array bytes{std::byte{0x3F}, std::byte{0x80}, std::byte{0}, std::byte{0}};
        return *Sora::Compute::FillPattern::Create(bytes);
    }();

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

} // namespace

TEST_CASE("GraphBuilder owns typed fill, copy, and external payloads and derives sparse dependencies",
          "[Sora][Compute][Graph][Builder]") {
    auto first = MakeBuffer({1, 1});
    auto second = MakeBuffer({2, 2});
    auto fillFirst = Sora::Compute::FillOperation::Create(Slice(first, 0, 256), kPattern);
    auto copy = Sora::Compute::CopyOperation::Create(Slice(first, 0, 256), Slice(second, 0, 256));
    auto fillSecond = Sora::Compute::FillOperation::Create(Slice(second, 0, 256), kPattern);
    REQUIRE(fillFirst.has_value());
    REQUIRE(copy.has_value());
    REQUIRE(fillSecond.has_value());
    REQUIRE(copy->RequiresPhysicalOverlapCheck());

    std::vector<Sora::Compute::ExternalResourceBinding> resources;
    resources.push_back({View(second, 64), Sora::Compute::AccessMode::Read});
    auto external = Sora::Compute::ExternalOperation::Create(
        {0xEE, 0x11}, Sora::Compute::ExternalPortability::BackendPortable, std::move(resources));
    REQUIRE(external.has_value());

    Sora::Compute::GraphBuilder builder;
    const auto fillFirstNode = builder.AddFill(std::move(*fillFirst));
    const auto copyNode = builder.AddCopy(std::move(*copy));
    const auto fillSecondNode = builder.AddFill(std::move(*fillSecond));
    const auto externalNode = builder.AddExternal(std::move(*external));
    REQUIRE(fillFirstNode.has_value());
    REQUIRE(copyNode.has_value());
    REQUIRE(fillSecondNode.has_value());
    REQUIRE(externalNode.has_value());
    REQUIRE(builder.AddDependency(*fillFirstNode, *fillSecondNode).has_value());
    REQUIRE(builder.AddDependency(*fillFirstNode, *fillSecondNode).has_value());

    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());
    REQUIRE(graph->NodeCount() == 4);
    REQUIRE(graph->OperationCount() == graph->NodeCount());
    REQUIRE(graph->AccessCount() == 5);
    REQUIRE(graph->EdgeCount() == 4);
    const std::array expectedEdges{
        Sora::Compute::GraphEdge{{0}, {1}},
        Sora::Compute::GraphEdge{{0}, {2}},
        Sora::Compute::GraphEdge{{1}, {2}},
        Sora::Compute::GraphEdge{{2}, {3}},
    };
    REQUIRE(std::ranges::equal(graph->View().edges, expectedEdges));
    REQUIRE(Sora::Compute::ValidateSparseGraph(graph->View()).has_value());
    REQUIRE(std::holds_alternative<Sora::Compute::FillOperation>(graph->Operation(*fillFirstNode)));
    REQUIRE(std::holds_alternative<Sora::Compute::CopyOperation>(graph->Operation(*copyNode)));
    REQUIRE(std::holds_alternative<Sora::Compute::ExternalOperation>(graph->Operation(*externalNode)));

    const auto& retainedCopy = std::get<Sora::Compute::CopyOperation>(graph->Operation(*copyNode));
    REQUIRE(retainedCopy.Source().BufferDescriptor().id == first.Describe().id);
    REQUIRE(retainedCopy.Destination().BufferDescriptor().id == second.Describe().id);
}

TEST_CASE("GraphBuilder consumes KernelInvocation effects without caller-authored access metadata",
          "[Sora][Compute][Graph][Builder][Dispatch]") {
    constexpr uint64_t count = 64;
    auto source = MakeBuffer({3, 1});
    auto destination = MakeBuffer({3, 2});
    std::vector<Sora::Compute::KernelArgumentBinding> bindings;
    bindings.emplace_back(View(source, count));
    bindings.emplace_back(View(destination, count));
    const std::array extents{count};
    auto invocation = Sora::Compute::BindKernelProgram(kCopyKernel, extents, std::move(bindings));
    REQUIRE(invocation.has_value());

    Sora::Compute::GraphBuilder builder;
    const auto node = builder.AddDispatch(std::move(*invocation));
    REQUIRE(node.has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());
    REQUIRE(graph->NodeCount() == 1);
    REQUIRE(graph->AccessCount() == 2);
    REQUIRE(graph->EdgeCount() == 0);
    REQUIRE(std::holds_alternative<Sora::Compute::KernelInvocation>(graph->Operation(*node)));
    REQUIRE(graph->View().nodes[0].operation == kCopyKernel.Hash());
    REQUIRE(graph->View().accesses[0].buffer == source.Describe().id);
    REQUIRE(graph->View().accesses[1].buffer == destination.Describe().id);
}

TEST_CASE("GraphBuilder interns operation-local affine footprints without false bounding hazards",
          "[Sora][Compute][Graph][Builder][Affine]") {
    auto buffer = Sora::Compute::Buffer::Create({.id = {3, 3},
                                                 .elementType = kFloat,
                                                 .layout = Sora::Compute::LayoutKind::Contiguous,
                                                 .rank = 1,
                                                 .sizeBytes = 32,
                                                 .alignment = 4});
    REQUIRE(buffer.has_value());
    const auto makeView = [&](uint64_t offset) {
        Sora::Compute::BufferViewDesc desc{
            .layout = Sora::Compute::LayoutKind::Strided, .rank = 1, .componentStrideBytes = 4, .offsetBytes = offset};
        desc.extents[0] = 4;
        desc.byteStrides[0] = 8;
        auto view = Sora::Compute::BufferView::Create(*buffer, desc);
        REQUIRE(view.has_value());
        return *view;
    };

    std::vector<Sora::Compute::ExternalResourceBinding> evenResources;
    evenResources.push_back({makeView(0), Sora::Compute::AccessMode::Write});
    auto even = Sora::Compute::ExternalOperation::Create(
        {0xEE, 0x20}, Sora::Compute::ExternalPortability::BackendPortable, std::move(evenResources));
    std::vector<Sora::Compute::ExternalResourceBinding> oddResources;
    oddResources.push_back({makeView(4), Sora::Compute::AccessMode::Read});
    auto odd = Sora::Compute::ExternalOperation::Create(
        {0xEE, 0x21}, Sora::Compute::ExternalPortability::BackendPortable, std::move(oddResources));
    REQUIRE(even.has_value());
    REQUIRE(odd.has_value());

    Sora::Compute::GraphBuilder builder;
    REQUIRE(builder.AddExternal(std::move(*even)).has_value());
    REQUIRE(builder.AddExternal(std::move(*odd)).has_value());
    auto graph = std::move(builder).Finish();
    REQUIRE(graph.has_value());
    REQUIRE(graph->FootprintCount() == 2);
    REQUIRE(graph->EdgeCount() == 0);
    REQUIRE(graph->View().accesses[0].footprint == Sora::Compute::ByteFootprintId{0});
    REQUIRE(graph->View().accesses[1].footprint == Sora::Compute::ByteFootprintId{1});
    REQUIRE(Sora::Compute::ValidateSparseGraph(graph->View()).has_value());
}

TEST_CASE("Logical copy and fill operations reject ambiguous byte semantics",
          "[Sora][Compute][Graph][Operation][Validation]") {
    auto buffer = MakeBuffer({4, 1});
    const auto overlap = Sora::Compute::CopyOperation::Create(Slice(buffer, 0, 128), Slice(buffer, 64, 128));
    REQUIRE_FALSE(overlap.has_value());
    REQUIRE(overlap.error() == Sora::Compute::GraphOperationError::OverlappingRegions);

    auto other = MakeBuffer({4, 2});
    const auto mismatch = Sora::Compute::CopyOperation::Create(Slice(buffer, 0, 64), Slice(other, 0, 128));
    REQUIRE_FALSE(mismatch.has_value());
    REQUIRE(mismatch.error() == Sora::Compute::GraphOperationError::SizeMismatch);

    constexpr std::array invalidPatternBytes{std::byte{1}, std::byte{2}, std::byte{3}};
    constexpr auto invalidPattern = Sora::Compute::FillPattern::Create(invalidPatternBytes);
    static_assert(!invalidPattern.has_value());
    REQUIRE(invalidPattern.error() == Sora::Compute::GraphOperationError::InvalidPattern);
}

TEST_CASE("GraphBuilder rejects invalid explicit dependencies and empty finalization transactionally",
          "[Sora][Compute][Graph][Builder][Validation]") {
    Sora::Compute::GraphBuilder empty;
    const auto emptyResult = std::move(empty).Finish();
    REQUIRE_FALSE(emptyResult.has_value());
    REQUIRE(emptyResult.error().code == Sora::Compute::GraphErrorCode::EmptyGraph);

    auto buffer = MakeBuffer({5, 1});
    auto fill = Sora::Compute::FillOperation::Create(Slice(buffer, 0, 256), kPattern);
    REQUIRE(fill.has_value());
    Sora::Compute::GraphBuilder builder;
    const auto node = builder.AddFill(std::move(*fill));
    REQUIRE(node.has_value());
    const auto invalid = builder.AddDependency(*node, *node);
    REQUIRE_FALSE(invalid.has_value());
    REQUIRE(invalid.error().code == Sora::Compute::GraphErrorCode::InvalidDependency);
    REQUIRE(builder.NodeCount() == 1);
}

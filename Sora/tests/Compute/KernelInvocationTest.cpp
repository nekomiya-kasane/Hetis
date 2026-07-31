#include <Sora/Compute/Invocation/KernelInvocation.h>
#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

    constexpr Sora::Compute::ValueType kFloat{Sora::Compute::ScalarKind::Float32, 1};

    consteval auto MakeSaxpyProgram() {
        Sora::Compute::KernelBuilder<3, 4> scalar;
        const auto alpha = scalar.AddValueParameter(kFloat);
        const auto x = scalar.AddValueParameter(kFloat);
        const auto y = scalar.AddValueParameter(kFloat);
        const auto result = scalar.Ternary(Sora::Compute::OpCode::Fma, alpha, x, y);
        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<4, 3, 4> program{domain, std::move(scalar).Finish(result),
                                                             Sora::Compute::PartitionContract::Elementwise(1)};
        const auto alphaArgument = program.AddValueArgument(kFloat);
        const auto xArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, 1,
                                                         Sora::Compute::LayoutKind::Contiguous, 64, {1});
        const auto yArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, 1,
                                                         Sora::Compute::LayoutKind::Contiguous, 64, {2});
        const auto outputArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Write, kFloat, 1,
                                                              Sora::Compute::LayoutKind::Contiguous, 64, {3});
        program.BindArgument(alphaArgument);
        program.BindArgument(xArgument);
        program.BindArgument(yArgument);
        return std::move(program).Finish(outputArgument);
    }

    consteval auto MakeStridedCopyProgram() {
        Sora::Compute::KernelBuilder<1, 1> scalar;
        const auto input = scalar.AddValueParameter(kFloat);
        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<2, 1, 1> program{domain, std::move(scalar).Finish(input),
                                                             Sora::Compute::PartitionContract::Elementwise(1)};
        const auto inputArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, 1,
                                                             Sora::Compute::LayoutKind::Strided, 64, {1});
        const auto outputArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Write, kFloat, 1,
                                                              Sora::Compute::LayoutKind::Strided, 64, {2});
        program.BindArgument(inputArgument);
        return std::move(program).Finish(outputArgument);
    }

    Sora::Compute::Buffer MakeBuffer(Sora::Compute::BufferId id, uint64_t size,
                                     Sora::Compute::LayoutKind layout = Sora::Compute::LayoutKind::Contiguous,
                                     Sora::Compute::ValueType type = kFloat) {
        const Sora::Compute::BufferDesc desc{
            .id = id,
            .elementType = type,
            .layout = layout,
            .rank = 1,
            .sizeBytes = size,
            .alignment = 64,
        };
        if (layout == Sora::Compute::LayoutKind::StructureOfArrays) {
            REQUIRE(size % type.ByteSize() == 0);
            const std::array extents{size / type.ByteSize()};
            const std::array strides{uint64_t{Sora::Compute::ScalarByteSize(type.scalar)}};
            const auto footprint = Sora::Compute::AffineByteFootprint::Create(extents, strides, extents[0] * strides[0],
                                                                              0, strides[0], type.lanes);
            REQUIRE(footprint.has_value());
            auto buffer = Sora::Compute::Buffer::Create(desc, *footprint);
            REQUIRE(buffer.has_value());
            return *buffer;
        }
        auto buffer = Sora::Compute::Buffer::Create(desc);
        REQUIRE(buffer.has_value());
        return *buffer;
    }

    Sora::Compute::ScalarArgument FloatArgument(float value) {
        const auto bits = std::bit_cast<std::array<std::byte, sizeof(float)>>(value);
        auto argument = Sora::Compute::ScalarArgument::Create(kFloat, bits);
        REQUIRE(argument.has_value());
        return std::move(*argument);
    }

    Sora::Compute::BufferView View(Sora::Compute::Buffer buffer, uint64_t count, uint64_t offset = 0) {
        const std::array extents{count};
        auto view = Sora::Compute::BufferView::Contiguous(std::move(buffer), extents, offset);
        REQUIRE(view.has_value());
        return std::move(*view);
    }

    constexpr auto kSaxpy = MakeSaxpyProgram();
    constexpr auto kStridedCopy = MakeStridedCopyProgram();

} // namespace

TEST_CASE("BufferView unifies contiguous, strided, and SoA affine footprints",
          "[Sora][Compute][Invocation][BufferView]") {
    const std::array extents{uint64_t{16}};
    auto contiguous = Sora::Compute::BufferView::Contiguous(MakeBuffer({1, 1}, 64), extents);
    REQUIRE(contiguous.has_value());
    REQUIRE(contiguous->Region() == *Sora::Compute::ByteRegion::Make(0, 64));
    REQUIRE(contiguous->Describe().byteStrides[0] == 4);
    REQUIRE(contiguous->BaseAlignment() == 64);

    constexpr Sora::Compute::ValueType vec3{Sora::Compute::ScalarKind::Float32, 3};
    auto soa = Sora::Compute::BufferView::StructureOfArrays(
        MakeBuffer({2, 2}, 16 * 3 * sizeof(float), Sora::Compute::LayoutKind::StructureOfArrays, vec3), extents);
    REQUIRE(soa.has_value());
    REQUIRE(soa->Describe().byteStrides[0] == 4);
    REQUIRE(soa->Describe().componentStrideBytes == 64);
    REQUIRE(soa->Region().Size() == 192);
    REQUIRE(soa->BaseAlignment() == 64);

    Sora::Compute::BufferViewDesc stridedDesc{
        .layout = Sora::Compute::LayoutKind::Strided,
        .rank = 1,
        .extents = {8},
        .byteStrides = {8},
        .componentStrideBytes = 4,
    };
    auto strided = Sora::Compute::BufferView::Create(MakeBuffer({3, 3}, 64), stridedDesc);
    REQUIRE(strided.has_value());
    REQUIRE(strided->Region().Size() == 60);
}

TEST_CASE("KernelInvocation validates bindings and derives concrete graph effects", "[Sora][Compute][Invocation]") {
    constexpr uint64_t count = 1024;
    auto x = MakeBuffer({10, 1}, count * sizeof(float));
    auto y = MakeBuffer({10, 2}, count * sizeof(float));
    auto output = MakeBuffer({10, 3}, count * sizeof(float));
    std::vector<Sora::Compute::KernelArgumentBinding> bindings;
    bindings.emplace_back(FloatArgument(2.0F));
    bindings.emplace_back(View(x, count));
    bindings.emplace_back(View(y, count));
    bindings.emplace_back(View(output, count));
    const std::array extents{count};

    auto invocation = Sora::Compute::BindKernelProgram(kSaxpy, extents, std::move(bindings));
    REQUIRE(invocation.has_value());
    REQUIRE(invocation->ProgramHash() == kSaxpy.Hash());
    REQUIRE(invocation->Domain().workItemCount == count);
    REQUIRE(invocation->ResourceDemand() == kSaxpy.ResourceDemand());
    REQUIRE(invocation->ResourceAccesses().size() == 3);
    REQUIRE(invocation->ResourceAccesses()[0].buffer == x.Describe().id);
    REQUIRE(invocation->ResourceAccesses()[0].mode == Sora::Compute::AccessMode::Read);
    REQUIRE(invocation->ResourceAccesses()[2].buffer == output.Describe().id);
    REQUIRE(invocation->ResourceAccesses()[2].mode == Sora::Compute::AccessMode::Write);
    REQUIRE(invocation->PhysicalAliasRequirements().size() == 3);
}

TEST_CASE("KernelInvocation fixed and dynamic program representations bind identically",
          "[Sora][Compute][Invocation][Representation]") {
    constexpr uint64_t count = 256;
    const std::array extents{count};
    auto x = MakeBuffer({10, 11}, count * sizeof(float));
    auto y = MakeBuffer({10, 12}, count * sizeof(float));
    auto output = MakeBuffer({10, 13}, count * sizeof(float));
    const auto makeBindings = [&] {
        std::vector<Sora::Compute::KernelArgumentBinding> bindings;
        bindings.emplace_back(FloatArgument(2.0F));
        bindings.emplace_back(View(x, count));
        bindings.emplace_back(View(y, count));
        bindings.emplace_back(View(output, count));
        return bindings;
    };

    const auto fixed = Sora::Compute::BindKernelProgram(kSaxpy, extents, makeBindings());
    const auto dynamic = Sora::Compute::BindKernelProgram(kSaxpy.View(), kSaxpy.Hash(), extents, makeBindings());
    REQUIRE(fixed.has_value());
    REQUIRE(dynamic.has_value());
    REQUIRE(dynamic->ProgramHash() == fixed->ProgramHash());
    REQUIRE(dynamic->Domain() == fixed->Domain());
    REQUIRE(dynamic->Partition() == fixed->Partition());
    REQUIRE(dynamic->RequiredDeviceFeatures() == fixed->RequiredDeviceFeatures());
    REQUIRE(dynamic->ResourceDemand() == fixed->ResourceDemand());
    REQUIRE(std::ranges::equal(dynamic->Signature(), fixed->Signature()));
    REQUIRE(std::ranges::equal(dynamic->ResourceAccesses(), fixed->ResourceAccesses()));
    REQUIRE(std::ranges::equal(dynamic->ByteFootprints(), fixed->ByteFootprints()));
    REQUIRE(std::ranges::equal(dynamic->PhysicalAliasRequirements(), fixed->PhysicalAliasRequirements()));
}

TEST_CASE("KernelInvocation refines static memory demand from actual strided bindings",
          "[Sora][Compute][Invocation][Demand]") {
    constexpr uint64_t count = 8;
    constexpr uint64_t stride = 8;
    constexpr uint64_t footprintBytes = (count - 1) * stride + sizeof(float);
    const Sora::Compute::BufferViewDesc stridedDesc{
        .layout = Sora::Compute::LayoutKind::Strided,
        .rank = 1,
        .extents = {count},
        .byteStrides = {stride},
        .componentStrideBytes = sizeof(float),
    };
    auto input = Sora::Compute::BufferView::Create(MakeBuffer({11, 1}, footprintBytes), stridedDesc);
    auto output = Sora::Compute::BufferView::Create(MakeBuffer({11, 2}, footprintBytes), stridedDesc);
    REQUIRE(input.has_value());
    REQUIRE(output.has_value());

    std::vector<Sora::Compute::KernelArgumentBinding> bindings;
    bindings.emplace_back(std::move(*input));
    bindings.emplace_back(std::move(*output));
    const std::array extents{count};
    const auto invocation = Sora::Compute::BindKernelProgram(kStridedCopy, extents, std::move(bindings));
    REQUIRE(invocation.has_value());
    REQUIRE(kStridedCopy.ResourceDemand().MemoryBytes() == 2 * sizeof(float));
    REQUIRE(kStridedCopy.ResourceDemand().StridedMemoryBytes() == 0);
    REQUIRE(invocation->ResourceDemand().MemoryBytes() == 2 * sizeof(float));
    REQUIRE(invocation->ResourceDemand().StridedMemoryBytes() == 2 * sizeof(float));
    REQUIRE(invocation->ResourceDemand().stridedBytesRead == sizeof(float));
    REQUIRE(invocation->ResourceDemand().stridedBytesWritten == sizeof(float));
}

TEST_CASE("KernelInvocation rejects binding kinds and mismatched extents", "[Sora][Compute][Invocation][Validation]") {
    constexpr uint64_t count = 16;
    const std::array extents{count};
    auto x = MakeBuffer({20, 1}, count * sizeof(float));
    auto y = MakeBuffer({20, 2}, count * sizeof(float));
    auto output = MakeBuffer({20, 3}, count * sizeof(float));

    std::vector<Sora::Compute::KernelArgumentBinding> wrongKind;
    wrongKind.emplace_back(View(x, count));
    wrongKind.emplace_back(View(x, count));
    wrongKind.emplace_back(View(y, count));
    wrongKind.emplace_back(View(output, count));
    const auto kindResult = Sora::Compute::BindKernelProgram(kSaxpy, extents, std::move(wrongKind));
    REQUIRE_FALSE(kindResult.has_value());
    REQUIRE(kindResult.error().code == Sora::Compute::KernelInvocationErrorCode::BindingKindMismatch);

    auto staticProgram = kSaxpy;
    staticProgram.domain.staticExtents[0] = count;
    const std::array wrongDomain{count - 1};
    std::vector<Sora::Compute::KernelArgumentBinding> noBindings;
    const auto domainResult = Sora::Compute::BindKernelProgram(staticProgram, wrongDomain, std::move(noBindings));
    REQUIRE_FALSE(domainResult.has_value());
    REQUIRE(domainResult.error().code == Sora::Compute::KernelInvocationErrorCode::InvalidDomain);

    std::vector<Sora::Compute::KernelArgumentBinding> wrongExtent;
    wrongExtent.emplace_back(FloatArgument(1.0F));
    wrongExtent.emplace_back(View(x, count));
    wrongExtent.emplace_back(View(MakeBuffer({20, 4}, (count - 1) * sizeof(float)), count - 1));
    wrongExtent.emplace_back(View(output, count));
    const auto extentResult = Sora::Compute::BindKernelProgram(kSaxpy, extents, std::move(wrongExtent));
    REQUIRE_FALSE(extentResult.has_value());
    REQUIRE(extentResult.error().code == Sora::Compute::KernelInvocationErrorCode::ExtentMismatch);
}

TEST_CASE("KernelInvocation enforces base alignment and logical noalias contracts",
          "[Sora][Compute][Invocation][Alias]") {
    constexpr uint64_t count = 16;
    const std::array extents{count};
    auto x = MakeBuffer({21, 1}, count * sizeof(float) + 4);
    auto y = MakeBuffer({21, 2}, count * sizeof(float));
    auto output = MakeBuffer({21, 3}, count * sizeof(float));

    std::vector<Sora::Compute::KernelArgumentBinding> misaligned;
    misaligned.emplace_back(FloatArgument(1.0F));
    misaligned.emplace_back(View(x, count, 4));
    misaligned.emplace_back(View(y, count));
    misaligned.emplace_back(View(output, count));
    const auto alignmentResult = Sora::Compute::BindKernelProgram(kSaxpy, extents, std::move(misaligned));
    REQUIRE_FALSE(alignmentResult.has_value());
    REQUIRE(alignmentResult.error().code == Sora::Compute::KernelInvocationErrorCode::AlignmentMismatch);

    auto shared = MakeBuffer({21, 4}, count * sizeof(float));
    std::vector<Sora::Compute::KernelArgumentBinding> aliased;
    aliased.emplace_back(FloatArgument(1.0F));
    aliased.emplace_back(View(shared, count));
    aliased.emplace_back(View(shared, count));
    aliased.emplace_back(View(output, count));
    const auto aliasResult = Sora::Compute::BindKernelProgram(kSaxpy, extents, std::move(aliased));
    REQUIRE_FALSE(aliasResult.has_value());
    REQUIRE(aliasResult.error().code == Sora::Compute::KernelInvocationErrorCode::AliasContractViolation);
    REQUIRE(aliasResult.error().argument == 1);
    REQUIRE(aliasResult.error().related == 2);
}

TEST_CASE("BufferView rejects overflow, incompatible layouts, and out-of-bounds footprints",
          "[Sora][Compute][Invocation][BufferView][Validation]") {
    const std::array tooLarge{std::numeric_limits<uint64_t>::max()};
    auto overflow = Sora::Compute::BufferView::Contiguous(MakeBuffer({30, 1}, 64), tooLarge);
    REQUIRE_FALSE(overflow.has_value());
    REQUIRE(overflow.error() == Sora::Compute::BufferViewError::ArithmeticOverflow);

    const std::array extents{uint64_t{17}};
    auto outOfBounds = Sora::Compute::BufferView::Contiguous(MakeBuffer({30, 2}, 64), extents);
    REQUIRE_FALSE(outOfBounds.has_value());
    REQUIRE(outOfBounds.error() == Sora::Compute::BufferViewError::OutOfBounds);

    auto incompatible = Sora::Compute::BufferView::StructureOfArrays(MakeBuffer({30, 3}, 256), extents);
    REQUIRE_FALSE(incompatible.has_value());
    REQUIRE(incompatible.error() == Sora::Compute::BufferViewError::IncompatibleLayout);

    constexpr Sora::Compute::ValueType vec2{Sora::Compute::ScalarKind::Float32, 2};
    Sora::Compute::BufferViewDesc overlappingDesc{
        .layout = Sora::Compute::LayoutKind::Strided,
        .rank = 1,
        .extents = {4},
        .byteStrides = {4},
        .componentStrideBytes = 4,
    };
    auto overlapping = Sora::Compute::BufferView::Create(
        MakeBuffer({30, 4}, 32, Sora::Compute::LayoutKind::Contiguous, vec2), overlappingDesc);
    REQUIRE_FALSE(overlapping.has_value());
    REQUIRE(overlapping.error() == Sora::Compute::BufferViewError::OverlappingElements);
}

TEST_CASE("ScalarArgument enforces canonical size and bool representation", "[Sora][Compute][Invocation][Scalar]") {
    constexpr std::array shortFloat{std::byte{0}, std::byte{0}};
    const auto wrongSize = Sora::Compute::ScalarArgument::Create(kFloat, shortFloat);
    REQUIRE_FALSE(wrongSize.has_value());
    REQUIRE(wrongSize.error() == Sora::Compute::ScalarArgumentError::InvalidValue);

    constexpr Sora::Compute::ValueType bool2{Sora::Compute::ScalarKind::Bool, 2};
    constexpr std::array invalidBool{std::byte{0}, std::byte{2}};
    const auto nonCanonical = Sora::Compute::ScalarArgument::Create(bool2, invalidBool);
    REQUIRE_FALSE(nonCanonical.has_value());
    REQUIRE(nonCanonical.error() == Sora::Compute::ScalarArgumentError::InvalidValue);
}

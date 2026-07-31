#include <Sora/Compute/Compiler/KernelProgramImage.h>
#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>
#include <Sora/Compute/Runtime/ProgramPackage.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <ranges>
#include <vector>

namespace {

    consteval auto MakeProgram() {
        constexpr Sora::Compute::ValueType type{Sora::Compute::ScalarKind::Float32, 1};
        Sora::Compute::KernelBuilder<2, 3> scalar;
        const auto left = scalar.AddValueParameter(type);
        const auto right = scalar.AddValueParameter(type);
        const auto result = scalar.Binary(Sora::Compute::OpCode::Add, left, right);

        constexpr Sora::Compute::IndexDomain domain{.rank = 1, .staticExtents = {0, 0, 0}};
        Sora::Compute::KernelProgramBuilder<3, 2, 3> program{
            domain, std::move(scalar).Finish(result), Sora::Compute::PartitionContract::Elementwise(domain.rank)};
        const auto first = program.AddBufferArgument(Sora::Compute::AccessMode::Read, type, 1);
        const auto second = program.AddBufferArgument(Sora::Compute::AccessMode::Read, type, 1);
        const auto output = program.AddBufferArgument(Sora::Compute::AccessMode::Write, type, 1);
        program.BindArgument(first);
        program.BindArgument(second);
        return std::move(program).Finish(output);
    }

    constexpr auto kProgram = MakeProgram();
    constexpr auto kBytes = kProgram.Serialize();

    Sora::Compute::KernelProgramImageStorage CopyStorage(Sora::Compute::KernelProgramView view) {
        return {.pattern = view.pattern,
                .domain = view.domain,
                .partition = view.partition,
                .numericalPolicy = view.scalar.numericalPolicy,
                .arguments = {view.arguments.begin(), view.arguments.end()},
                .inputs = {view.inputs.begin(), view.inputs.end()},
                .scalarParameters = {view.scalar.parameters.begin(), view.scalar.parameters.end()},
                .instructions = {view.scalar.instructions.begin(), view.scalar.instructions.end()},
                .result = view.scalar.result,
                .output = view.output};
    }

} // namespace

TEST_CASE("KernelProgramImage dynamically decodes the fixed canonical program representation",
          "[Sora][Compute][Compiler][KernelProgramImage]") {
    const auto image = Sora::Compute::KernelProgramImage::Decode(kBytes.Bytes());
    REQUIRE(image.has_value());
    REQUIRE(image->Hash() == kProgram.Hash());
    REQUIRE(std::ranges::equal(image->Bytes(), kBytes.Bytes()));
    REQUIRE(image->View().pattern == kProgram.pattern);
    REQUIRE(image->View().domain == kProgram.domain);
    REQUIRE(image->View().partition == kProgram.partition);
    REQUIRE(std::ranges::equal(image->View().arguments, kProgram.Arguments()));
    REQUIRE(std::ranges::equal(image->View().inputs, kProgram.Inputs()));
    REQUIRE(std::ranges::equal(image->View().scalar.parameters, kProgram.scalar.Parameters()));
    REQUIRE(std::ranges::equal(image->View().scalar.instructions, kProgram.scalar.Instructions()));
    REQUIRE(image->ResourceDemand() == kProgram.ResourceDemand());
}

TEST_CASE("KernelProgramImage canonicalizes semantic storage and transfers bytes into ProgramPackage",
          "[Sora][Compute][Compiler][KernelProgramImage][Ownership]") {
    auto decoded = Sora::Compute::KernelProgramImage::Decode(kBytes.Bytes());
    REQUIRE(decoded.has_value());
    auto rebuilt = Sora::Compute::KernelProgramImage::Create(CopyStorage(decoded->View()));
    REQUIRE(rebuilt.has_value());
    REQUIRE(std::ranges::equal(rebuilt->Bytes(), kBytes.Bytes()));

    const std::byte* bytes = rebuilt->Bytes().data();
    auto package = Sora::Compute::ProgramPackage::Create(std::move(*rebuilt));
    REQUIRE(package.has_value());
    REQUIRE(package->Hash() == kProgram.Hash());
    REQUIRE(package->Bytes().data() == bytes);
}

TEST_CASE("KernelProgramImage rejects non-canonical bytes and independent resource-limit violations",
          "[Sora][Compute][Compiler][KernelProgramImage][Validation]") {
    std::vector<std::byte> trailing{kBytes.Bytes().begin(), kBytes.Bytes().end()};
    trailing.push_back(std::byte{0});
    const auto nonCanonical = Sora::Compute::KernelProgramImage::Decode(trailing);
    REQUIRE_FALSE(nonCanonical.has_value());
    REQUIRE(nonCanonical.error().code == Sora::Compute::KernelProgramPackageErrorCode::TrailingBytes);

    auto limits = Sora::Compute::KernelProgramLimits{};
    limits.maximumArguments = 2;
    const auto overBudget = Sora::Compute::KernelProgramImage::Decode(kBytes.Bytes(), limits);
    REQUIRE_FALSE(overBudget.has_value());
    REQUIRE(overBudget.error().code == Sora::Compute::KernelProgramPackageErrorCode::CapacityExceeded);
}

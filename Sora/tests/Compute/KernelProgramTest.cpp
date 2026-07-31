#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>
#include <Sora/Compute/KernelProgramPackage.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <algorithm>
#include <cstddef>
#include <span>

namespace {

    constexpr Sora::Compute::ValueType kFloat{Sora::Compute::ScalarKind::Float32, 1};

    consteval auto MakeSaxpyScalar() {
        Sora::Compute::KernelBuilder<3, 4> builder{Sora::Compute::NumericalPolicy{
            .floatingPoint = Sora::Compute::FloatingPointMode::Contract,
            .reduction = Sora::Compute::ReductionMode::Deterministic,
        }};
        const auto alpha = builder.AddValueParameter(kFloat);
        const auto x = builder.AddValueParameter(kFloat);
        const auto y = builder.AddValueParameter(kFloat);
        return std::move(builder).Finish(builder.Ternary(Sora::Compute::OpCode::Fma, alpha, x, y));
    }

    consteval auto MakeSaxpyProgram() {
        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<4, 3, 4> builder{
            domain, MakeSaxpyScalar(), Sora::Compute::PartitionContract::Elementwise(domain.rank)};
        const auto alpha = builder.AddValueArgument(kFloat);
        const auto x = builder.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, domain.rank,
                                                 Sora::Compute::LayoutKind::Contiguous, 64, {1});
        const auto y = builder.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, domain.rank,
                                                 Sora::Compute::LayoutKind::Contiguous, 64, {2});
        const auto output = builder.AddBufferArgument(Sora::Compute::AccessMode::Write, kFloat, domain.rank,
                                                      Sora::Compute::LayoutKind::Contiguous, 64, {3});
        builder.BindArgument(alpha);
        builder.BindArgument(x);
        builder.BindArgument(y);
        return std::move(builder).Finish(output);
    }

    consteval auto MakeIndexProgram() {
        constexpr Sora::Compute::ValueType indexType{Sora::Compute::ScalarKind::UInt64, 1};
        Sora::Compute::KernelBuilder<1, 1> scalar;
        const auto index = scalar.AddValueParameter(indexType);
        constexpr Sora::Compute::IndexDomain domain{.rank = 2};
        Sora::Compute::KernelProgramBuilder<1, 1, 1> builder{
            domain, std::move(scalar).Finish(index), Sora::Compute::PartitionContract::Elementwise(domain.rank)};
        const auto output = builder.AddBufferArgument(Sora::Compute::AccessMode::Write, indexType, domain.rank);
        builder.BindLogicalIndex(1);
        return std::move(builder).Finish(output);
    }

    constexpr auto kSaxpy = MakeSaxpyProgram();
    constexpr auto kIndex = MakeIndexProgram();
    constexpr auto kSaxpyBytes = kSaxpy.Serialize();
    constexpr auto kSaxpyEffects = kSaxpy.EffectSummary();
    constexpr auto kSaxpyDemand = kSaxpy.ResourceDemand();

    static_assert(Sora::Compute::ValidateKernelProgram(kSaxpy).has_value());
    static_assert(Sora::Compute::ValidateKernelProgram(kIndex).has_value());
    static_assert(kSaxpy.argumentCount == 4);
    static_assert(kSaxpy.inputCount == 3);
    static_assert(kSaxpyEffects.size == 3);
    static_assert(kSaxpyEffects.storage[0].access == Sora::Compute::AccessMode::Read);
    static_assert(kSaxpyEffects.storage[2].access == Sora::Compute::AccessMode::Write);
    static_assert(kSaxpyDemand.floatingPointOperations == 2);
    static_assert(kSaxpyDemand.bytesRead == 8);
    static_assert(kSaxpyDemand.bytesWritten == 4);
    static_assert(kSaxpyDemand.Valid());
    static_assert(kSaxpyBytes.storage[0] == std::byte{'S'});
    static_assert(kSaxpyBytes.storage[1] == std::byte{'P'});
    static_assert(kSaxpyBytes.storage[2] == std::byte{'R'});
    static_assert(kSaxpyBytes.storage[3] == std::byte{'G'});
    constexpr auto kDecodedSaxpy = Sora::Compute::DecodeKernelProgramPackage<4, 3, 4>(kSaxpyBytes.Bytes());
    static_assert(kDecodedSaxpy.has_value());
    static_assert(kDecodedSaxpy->Hash() == kSaxpy.Hash());

} // namespace

TEST_CASE("KernelProgram closes scalar, domain, signature, effect, and partition semantics at compile time",
          "[Sora][Compute][KernelProgram]") {
    REQUIRE(kSaxpy.pattern == Sora::Compute::KernelPattern::Elementwise);
    REQUIRE(kSaxpy.domain.rank == 1);
    REQUIRE(kSaxpy.partition.kind == Sora::Compute::PartitionKind::Elementwise);
    REQUIRE(kSaxpy.partition.splittableAxisMask == 1);
    REQUIRE(kSaxpy.scalar.Hash() == MakeSaxpyScalar().Hash());
    REQUIRE(kSaxpy.Hash() == MakeSaxpyProgram().Hash());
}

TEST_CASE("KernelProgram semantic identity includes domain and partition contract",
          "[Sora][Compute][KernelProgram][Hash]") {
    auto changedDomain = kSaxpy;
    changedDomain.domain.staticExtents[0] = 4096;
    REQUIRE(changedDomain.Hash() != kSaxpy.Hash());

    auto changedPartition = kSaxpy;
    changedPartition.partition = Sora::Compute::PartitionContract::Indivisible();
    REQUIRE(Sora::Compute::ValidateKernelProgram(changedPartition).has_value());
    REQUIRE(changedPartition.Hash() != kSaxpy.Hash());

    auto changedAlias = kSaxpy;
    changedAlias.arguments[1].aliasClass = {7};
    REQUIRE(Sora::Compute::ValidateKernelProgram(changedAlias).has_value());
    REQUIRE(changedAlias.Hash() != kSaxpy.Hash());
    REQUIRE_FALSE(changedAlias.arguments[1].MayAlias(changedAlias.arguments[2]));
}

TEST_CASE("KernelProgram accepts logical-index inputs only as UInt64 values",
          "[Sora][Compute][KernelProgram][Validation]") {
    REQUIRE(kIndex.inputs[0].kind == Sora::Compute::ScalarInputKind::LogicalIndex);
    REQUIRE(kIndex.inputs[0].sourceIndex == 1);

    auto invalid = kIndex;
    invalid.scalar.parameters[0].elementType = kFloat;
    invalid.scalar.instructions[0].resultType = kFloat;
    invalid.arguments[0].elementType = kFloat;
    const auto result = Sora::Compute::ValidateKernelProgram(invalid);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == Sora::Compute::KernelProgramErrorCode::InvalidInput);
}

TEST_CASE("KernelProgram rejects duplicate scalar loads and unused signature arguments",
          "[Sora][Compute][KernelProgram][Validation]") {
    auto duplicate = kSaxpy;
    duplicate.inputs[2] = duplicate.inputs[1];
    const auto duplicateResult = Sora::Compute::ValidateKernelProgram(duplicate);
    REQUIRE_FALSE(duplicateResult.has_value());
    REQUIRE(duplicateResult.error().code == Sora::Compute::KernelProgramErrorCode::DuplicateInput);
    REQUIRE(duplicateResult.error().related == 1);

    constexpr Sora::Compute::IndexDomain domain{.rank = 1};
    Sora::Compute::KernelProgramBuilder<5, 3, 4> builder{domain, MakeSaxpyScalar(),
                                                         Sora::Compute::PartitionContract::Elementwise(domain.rank)};
    static_cast<void>(builder.AddValueArgument(kFloat));
    const auto alpha = builder.AddValueArgument(kFloat);
    const auto x = builder.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, domain.rank);
    const auto y = builder.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, domain.rank);
    const auto output = builder.AddBufferArgument(Sora::Compute::AccessMode::Write, kFloat, domain.rank);
    builder.BindArgument(alpha);
    builder.BindArgument(x);
    builder.BindArgument(y);
    const auto unusedResult = std::move(builder).TryFinish(output);
    REQUIRE_FALSE(unusedResult.has_value());
    REQUIRE(unusedResult.error().code == Sora::Compute::KernelProgramErrorCode::UnusedArgument);
    REQUIRE(unusedResult.error().index == 0);
}

TEST_CASE("KernelProgram rejects writable non-output arguments and output type mismatches",
          "[Sora][Compute][KernelProgram][Validation]") {
    auto extraWrite = kSaxpy;
    extraWrite.arguments[1].access = Sora::Compute::AccessMode::ReadWrite;
    const auto extraWriteResult = Sora::Compute::ValidateKernelProgram(extraWrite);
    REQUIRE_FALSE(extraWriteResult.has_value());
    REQUIRE(extraWriteResult.error().code == Sora::Compute::KernelProgramErrorCode::InvalidOutput);
    REQUIRE(extraWriteResult.error().index == 1);

    auto wrongOutput = kSaxpy;
    wrongOutput.arguments[3].elementType = {Sora::Compute::ScalarKind::Float64, 1};
    const auto wrongOutputResult = Sora::Compute::ValidateKernelProgram(wrongOutput);
    REQUIRE_FALSE(wrongOutputResult.has_value());
    REQUIRE(wrongOutputResult.error().code == Sora::Compute::KernelProgramErrorCode::InvalidOutput);
}

TEST_CASE("KernelProgram rejects inactive extents and partition axes", "[Sora][Compute][KernelProgram][Validation]") {
    auto invalidDomain = kSaxpy;
    invalidDomain.domain.staticExtents[1] = 1;
    const auto domainResult = Sora::Compute::ValidateKernelProgram(invalidDomain);
    REQUIRE_FALSE(domainResult.has_value());
    REQUIRE(domainResult.error().code == Sora::Compute::KernelProgramErrorCode::InvalidDomain);

    auto invalidPartition = kSaxpy;
    invalidPartition.partition.splittableAxisMask = 2;
    const auto partitionResult = Sora::Compute::ValidateKernelProgram(invalidPartition);
    REQUIRE_FALSE(partitionResult.has_value());
    REQUIRE(partitionResult.error().code == Sora::Compute::KernelProgramErrorCode::InvalidPartition);
}

TEST_CASE("KernelProgram packages round-trip and separate wire from semantic failures",
          "[Sora][Compute][KernelProgram][Package]") {
    const auto decoded = Sora::Compute::DecodeKernelProgramPackage<4, 3, 4>(kSaxpyBytes.Bytes());
    REQUIRE(decoded.has_value());
    REQUIRE(std::ranges::equal(decoded->Serialize().Bytes(), kSaxpyBytes.Bytes()));

    std::array<std::byte, kSaxpyBytes.storage.size() + 1> extended{};
    std::ranges::copy(kSaxpyBytes.Bytes(), extended.begin());
    const auto trailing = Sora::Compute::DecodeKernelProgramPackage<4, 3, 4>(std::span<const std::byte>{extended});
    REQUIRE_FALSE(trailing.has_value());
    REQUIRE(trailing.error().code == Sora::Compute::KernelProgramPackageErrorCode::TrailingBytes);

    const auto truncated =
        Sora::Compute::DecodeKernelProgramPackage<4, 3, 4>(kSaxpyBytes.Bytes().first(kSaxpyBytes.Bytes().size() - 1));
    REQUIRE_FALSE(truncated.has_value());
    REQUIRE(truncated.error().code == Sora::Compute::KernelProgramPackageErrorCode::Truncated);

    auto invalidMagicBytes = kSaxpyBytes;
    invalidMagicBytes.storage[0] = std::byte{0};
    const auto invalidMagic = Sora::Compute::DecodeKernelProgramPackage<4, 3, 4>(invalidMagicBytes.Bytes());
    REQUIRE_FALSE(invalidMagic.has_value());
    REQUIRE(invalidMagic.error().code == Sora::Compute::KernelProgramPackageErrorCode::InvalidMagic);

    auto invalidSemanticsProgram = kSaxpy;
    invalidSemanticsProgram.arguments[1].minimumAlignment = 3;
    const auto invalidSemanticsBytes = invalidSemanticsProgram.Serialize();
    const auto invalidSemantics = Sora::Compute::DecodeKernelProgramPackage<4, 3, 4>(invalidSemanticsBytes.Bytes());
    REQUIRE_FALSE(invalidSemantics.has_value());
    REQUIRE(invalidSemantics.error().code == Sora::Compute::KernelProgramPackageErrorCode::InvalidSemantics);
    REQUIRE(invalidSemantics.error().detail ==
            static_cast<uint32_t>(Sora::Compute::KernelProgramErrorCode::InvalidArgument));

    auto nonCanonicalImmediateProgram = kSaxpy;
    nonCanonicalImmediateProgram.scalar.instructions[3].immediate = 1;
    const auto nonCanonicalImmediateBytes = nonCanonicalImmediateProgram.Serialize();
    const auto nonCanonicalImmediate =
        Sora::Compute::DecodeKernelProgramPackage<4, 3, 4>(nonCanonicalImmediateBytes.Bytes());
    REQUIRE_FALSE(nonCanonicalImmediate.has_value());
    REQUIRE(nonCanonicalImmediate.error().code == Sora::Compute::KernelProgramPackageErrorCode::InvalidScalarModule);
    REQUIRE(nonCanonicalImmediate.error().detail ==
            static_cast<uint32_t>(Sora::Compute::KernelPackageErrorCode::InvalidEncoding));
}

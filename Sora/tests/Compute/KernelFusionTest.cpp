#include <Sora/Compute/Compiler/KernelFusion.h>
#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>
#include <Sora/Compute/KernelProgramPackage.h>
#include <Sora/Compute/ReferenceEvaluator.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <ranges>
#include <span>

namespace {

    constexpr Sora::Compute::ValueType kFloat{Sora::Compute::ScalarKind::Float32, 1};
    constexpr Sora::Compute::IndexDomain kDomain{.rank = 1, .staticExtents = {0, 0, 0}};

    consteval auto MakeProducer() {
        Sora::Compute::KernelBuilder<1, 3> scalar;
        const auto input = scalar.AddValueParameter(kFloat);
        const auto two = scalar.Constant(2.0F);
        const auto result = scalar.Binary(Sora::Compute::OpCode::Mul, input, two);
        Sora::Compute::KernelProgramBuilder<2, 1, 3> program{
            kDomain, std::move(scalar).Finish(result), Sora::Compute::PartitionContract::Elementwise(kDomain.rank)};
        const auto source = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, 1,
                                                      Sora::Compute::LayoutKind::Contiguous, 4, {11});
        const auto intermediate = program.AddBufferArgument(Sora::Compute::AccessMode::Write, kFloat, 1,
                                                            Sora::Compute::LayoutKind::Contiguous, 4, {22});
        program.BindArgument(source);
        return std::move(program).Finish(intermediate);
    }

    consteval auto MakeConsumer(Sora::Compute::NumericalPolicy policy = {},
                                Sora::Compute::IndexDomain domain = kDomain) {
        Sora::Compute::KernelBuilder<1, 3> scalar{policy};
        const auto input = scalar.AddValueParameter(kFloat);
        const auto one = scalar.Constant(1.0F);
        const auto result = scalar.Binary(Sora::Compute::OpCode::Add, input, one);
        Sora::Compute::KernelProgramBuilder<2, 1, 3> program{
            domain, std::move(scalar).Finish(result), Sora::Compute::PartitionContract::Elementwise(domain.rank)};
        const auto intermediate = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, domain.rank,
                                                            Sora::Compute::LayoutKind::Contiguous, 4, {33});
        const auto output = program.AddBufferArgument(Sora::Compute::AccessMode::Write, kFloat, domain.rank,
                                                      Sora::Compute::LayoutKind::Contiguous, 4, {44});
        program.BindArgument(intermediate);
        return std::move(program).Finish(output);
    }

    constexpr auto kProducer = MakeProducer();
    constexpr auto kConsumer = MakeConsumer();

} // namespace

TEST_CASE("Kernel fusion eliminates one elementwise intermediate while preserving scalar semantics",
          "[Sora][Compute][Compiler][Fusion]") {
    const auto fused = Sora::Compute::FuseElementwiseKernels(kProducer.View(), kConsumer.View(), {0});
    REQUIRE(fused.has_value());
    REQUIRE(fused->View().arguments.size() == 2);
    REQUIRE(fused->View().scalar.parameters.size() == 1);
    REQUIRE(fused->View().scalar.instructions.size() == 5);
    REQUIRE(fused->View().arguments[0].access == Sora::Compute::AccessMode::Read);
    REQUIRE_FALSE(fused->View().arguments[0].aliasClass.Known());
    REQUIRE(fused->View().arguments[1].access == Sora::Compute::AccessMode::Write);
    REQUIRE(fused->View().arguments[1].aliasClass == Sora::Compute::AliasClassId{44});

    const auto demand = fused->ResourceDemand();
    REQUIRE(demand.bytesRead == kFloat.ByteSize());
    REQUIRE(demand.bytesWritten == kFloat.ByteSize());
    REQUIRE(demand.floatingPointOperations == 2);
    REQUIRE(demand.bytesRead + kFloat.ByteSize() ==
            kProducer.ResourceDemand().bytesRead + kConsumer.ResourceDemand().bytesRead);
    REQUIRE(demand.bytesWritten + kFloat.ByteSize() ==
            kProducer.ResourceDemand().bytesWritten + kConsumer.ResourceDemand().bytesWritten);

    const auto decoded = Sora::Compute::DecodeKernelProgramPackage<2, 1, 5>(fused->Bytes());
    REQUIRE(decoded.has_value());
    constexpr std::array inputs{3.5F};
    const auto actual = Sora::Compute::EvaluateReference(decoded->scalar, std::span<const float>{inputs});
    REQUIRE(actual.has_value());
    REQUIRE(*actual == 8.0F);
}

TEST_CASE("Kernel fusion has deterministic canonical identity and intersects partition contracts",
          "[Sora][Compute][Compiler][Fusion][Identity]") {
    auto consumer = kConsumer;
    consumer.partition = Sora::Compute::PartitionContract::Indivisible();
    REQUIRE(Sora::Compute::ValidateKernelProgram(consumer).has_value());

    const auto first = Sora::Compute::FuseElementwiseKernels(kProducer.View(), consumer.View(), {0});
    const auto second = Sora::Compute::FuseElementwiseKernels(kProducer.View(), consumer.View(), {0});
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(first->Hash() == second->Hash());
    REQUIRE(first->Bytes().data() != second->Bytes().data());
    REQUIRE(std::ranges::equal(first->Bytes(), second->Bytes()));
    REQUIRE(first->View().partition == Sora::Compute::PartitionContract::Indivisible());
}

TEST_CASE("Kernel fusion rejects domain, numerical-policy, and intermediate-contract mismatches",
          "[Sora][Compute][Compiler][Fusion][Legality]") {
    constexpr Sora::Compute::IndexDomain fixedDomain{.rank = 1, .staticExtents = {64, 0, 0}};
    constexpr auto fixedConsumer = MakeConsumer({}, fixedDomain);
    const auto domainMismatch = Sora::Compute::FuseElementwiseKernels(kProducer.View(), fixedConsumer.View(), {0});
    REQUIRE_FALSE(domainMismatch.has_value());
    REQUIRE(domainMismatch.error().code == Sora::Compute::KernelFusionErrorCode::IncompatibleDomain);

    constexpr Sora::Compute::NumericalPolicy fastPolicy{.floatingPoint = Sora::Compute::FloatingPointMode::Fast,
                                                        .reduction = Sora::Compute::ReductionMode::Deterministic};
    constexpr auto fastConsumer = MakeConsumer(fastPolicy);
    const auto policyMismatch = Sora::Compute::FuseElementwiseKernels(kProducer.View(), fastConsumer.View(), {0});
    REQUIRE_FALSE(policyMismatch.has_value());
    REQUIRE(policyMismatch.error().code == Sora::Compute::KernelFusionErrorCode::IncompatiblePolicy);

    const auto invalidIntermediate = Sora::Compute::FuseElementwiseKernels(kProducer.View(), kConsumer.View(), {1});
    REQUIRE_FALSE(invalidIntermediate.has_value());
    REQUIRE(invalidIntermediate.error().code == Sora::Compute::KernelFusionErrorCode::InvalidIntermediate);
}

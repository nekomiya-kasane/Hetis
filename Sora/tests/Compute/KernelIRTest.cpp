#include <Sora/Compute/ElementwiseKernel.h>
#include <Sora/Compute/KernelPackage.h>
#include <Sora/Compute/Planner.h>
#include <Sora/Compute/ReferenceEvaluator.h>
#include <Sora/Math/PortableCompile.h>
#include <Sora/Math/Simd.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <algorithm>
#include <cstddef>
#include <span>

namespace {

    inline constexpr Sora::Compute::NumericalPolicy kStrictPolicy{
        .floatingPoint = Sora::Compute::FloatingPointMode::Strict,
        .reduction = Sora::Compute::ReductionMode::Deterministic,
    };

    inline constexpr auto kContracted = Sora::Math::CompilePortable<float, 3>(
        [](auto a, auto b, auto c) { return Sora::Math::Add(Sora::Math::Mul(a, b), c); });

    inline constexpr auto kStrict = Sora::Math::CompilePortable<float, 3, kStrictPolicy>(
        [](auto a, auto b, auto c) { return Sora::Math::Add(Sora::Math::Mul(a, b), c); });

    inline constexpr auto kContractedAgain = Sora::Math::CompilePortable<float, 3>(
        [](auto a, auto b, auto c) { return Sora::Math::Add(Sora::Math::Mul(a, b), c); });

    inline constexpr auto kAdd =
        Sora::Math::CompilePortable<float, 2>([](auto first, auto second) { return Sora::Math::Add(first, second); });
    inline constexpr auto kOffset =
        Sora::Math::CompilePortable<float, 1>([](auto value) { return Sora::Math::Add(value, 2.0F); });

    constexpr Sora::Compute::IndexDomain kVectorDomain{.rank = 1};
    constexpr auto kVec3Aos = Sora::Compute::LiftElementwise<3>(
        kAdd, kVectorDomain,
        {.layout = Sora::Compute::LayoutKind::Contiguous, .minimumAlignment = 64, .aliasClass = {3}},
        Sora::Compute::ElementwiseInputSpec::Buffer(Sora::Compute::LayoutKind::Contiguous, 64, {1}),
        Sora::Compute::ElementwiseInputSpec::Buffer(Sora::Compute::LayoutKind::Contiguous, 64, {2}));
    constexpr auto kVec3Soa = Sora::Compute::LiftElementwise<3>(
        kAdd, kVectorDomain,
        {.layout = Sora::Compute::LayoutKind::StructureOfArrays, .minimumAlignment = 64, .aliasClass = {3}},
        Sora::Compute::ElementwiseInputSpec::Buffer(Sora::Compute::LayoutKind::StructureOfArrays, 64, {1}),
        Sora::Compute::ElementwiseInputSpec::Buffer(Sora::Compute::LayoutKind::StructureOfArrays, 64, {2}));
    constexpr auto kVec3Offset = Sora::Compute::LiftElementwise<3>(
        kOffset, kVectorDomain, {.layout = Sora::Compute::LayoutKind::Contiguous},
        Sora::Compute::ElementwiseInputSpec::Buffer(Sora::Compute::LayoutKind::Contiguous));

    constexpr auto kContractedModule = kContracted.PortableModule();
    constexpr auto kStrictModule = kStrict.PortableModule();
    constexpr auto kContractedBytes = kContractedModule.Serialize();

    static_assert(kContractedModule.parameterCount == 3);
    static_assert(kContractedModule.instructionCount == 4);
    static_assert(kContractedModule.instructions[3].opcode == Sora::Compute::OpCode::Fma);
    static_assert(kStrictModule.instructionCount == 5);
    static_assert(kStrictModule.instructions[3].opcode == Sora::Compute::OpCode::Mul);
    static_assert(kStrictModule.instructions[4].opcode == Sora::Compute::OpCode::Add);
    static_assert(kContracted.Hash() == kContractedAgain.Hash());
    static_assert(kContracted.Hash() != kStrict.Hash());
    static_assert(kContractedBytes.storage[0] == std::byte{'S'});
    static_assert(kContractedBytes.storage[1] == std::byte{'K'});
    static_assert(kContractedBytes.storage[2] == std::byte{'I'});
    static_assert(kContractedBytes.storage[3] == std::byte{'R'});
    static_assert(kAdd(2.0F, 3.0F) == 5.0F);
    static_assert(kVec3Aos.scalar.parameters[0].elementType ==
                  Sora::Compute::ValueType{Sora::Compute::ScalarKind::Float32, 3});
    static_assert(kVec3Aos.scalar.instructions[2].resultType.lanes == 3);
    static_assert(kVec3Aos.partition == Sora::Compute::PartitionContract::Elementwise(1));
    static_assert(kVec3Aos.Hash() != kVec3Soa.Hash());
    static_assert(kVec3Aos.scalar.numericalPolicy == kAdd.PortableModule().numericalPolicy);
    static_assert(kVec3Offset.scalar.instructions[1].opcode == Sora::Compute::OpCode::Constant);
    static_assert(kVec3Offset.scalar.instructions[1].resultType.lanes == 3);
    static_assert(Sora::Compute::Validate(kContractedModule).has_value());
    static_assert(Sora::Compute::LayoutSatisfies(Sora::Compute::LayoutKind::Contiguous,
                                                 Sora::Compute::LayoutKind::Strided));
    static_assert(Sora::Compute::LayoutSatisfies(Sora::Compute::LayoutKind::StructureOfArrays,
                                                 Sora::Compute::LayoutKind::Strided));
    static_assert(!Sora::Compute::LayoutSatisfies(Sora::Compute::LayoutKind::Strided,
                                                  Sora::Compute::LayoutKind::Contiguous));
    static_assert(!Sora::Compute::LayoutSatisfies(static_cast<Sora::Compute::LayoutKind>(0xFF),
                                                  static_cast<Sora::Compute::LayoutKind>(0xFF)));

    constexpr auto kDecodedContracted = Sora::Compute::DecodeKernelPackage<3, 4>(kContractedBytes.Bytes());
    static_assert(kDecodedContracted.has_value());
    static_assert(kDecodedContracted->Hash() == kContracted.Hash());

} // namespace

TEST_CASE("portable Math compilation preserves direct and reference semantics", "[Sora][Compute][IR]") {
    const std::array args{1.25F, -3.5F, 0.75F};
    const auto interpreted =
        Sora::Compute::EvaluateReference(kContracted.PortableModule(), std::span<const float>{args});

    REQUIRE(interpreted.has_value());
    REQUIRE(*interpreted == Catch::Approx(kContracted(args[0], args[1], args[2])));
}

TEST_CASE("one portable Math definition directly invokes fixed SIMD carriers", "[Sora][Compute][IR][Simd]") {
    const Sora::Simd::F32<4> first(2.0F);
    const Sora::Simd::F32<4> second(3.0F);
    const auto result = kAdd(first, second);
    for (size_t lane = 0; lane < 4; ++lane) {
        REQUIRE(result[lane] == 5.0F);
    }
}

TEST_CASE("strict and contracted portable kernels retain distinct operation graphs", "[Sora][Compute][IR]") {
    REQUIRE(kContracted.PortableModule().numericalPolicy.floatingPoint == Sora::Compute::FloatingPointMode::Contract);
    REQUIRE(kStrict.PortableModule().numericalPolicy.floatingPoint == Sora::Compute::FloatingPointMode::Strict);
    REQUIRE(kContracted.Hash() != kStrict.Hash());
}

TEST_CASE("Kernel IR validation rejects non-dominating operands", "[Sora][Compute][IR][Validation]") {
    auto invalid = kStrict.PortableModule();
    invalid.instructions[3].operands[0] = Sora::Compute::ValueId{4};

    const auto result = Sora::Compute::Validate(invalid);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == Sora::Compute::KernelIrError{Sora::Compute::KernelIrErrorCode::InvalidOperand, 3});
}

TEST_CASE("Kernel IR validation rejects non-canonical instruction immediates",
          "[Sora][Compute][IR][Validation][Canonical]") {
    auto unusedImmediate = kContractedModule;
    unusedImmediate.instructions[3].immediate = 1;
    const auto unusedResult = Sora::Compute::Validate(unusedImmediate);
    REQUIRE_FALSE(unusedResult.has_value());
    REQUIRE(unusedResult.error() ==
            Sora::Compute::KernelIrError{Sora::Compute::KernelIrErrorCode::InvalidInstruction, 3});

    auto excessiveConstantBits = kContractedModule;
    excessiveConstantBits.instructions[0].opcode = Sora::Compute::OpCode::Constant;
    excessiveConstantBits.instructions[0].immediate = uint64_t{1} << 32U;
    const auto constantResult = Sora::Compute::Validate(excessiveConstantBits);
    REQUIRE_FALSE(constantResult.has_value());
    REQUIRE(constantResult.error() ==
            Sora::Compute::KernelIrError{Sora::Compute::KernelIrErrorCode::InvalidInstruction, 0});
}

TEST_CASE("Kernel IR validation rejects out-of-domain enum values", "[Sora][Compute][IR][Validation]") {
    auto invalidPolicy = kContractedModule;
    invalidPolicy.numericalPolicy.floatingPoint = static_cast<Sora::Compute::FloatingPointMode>(0xFF);
    const auto policyResult = Sora::Compute::Validate(invalidPolicy);
    REQUIRE_FALSE(policyResult.has_value());
    REQUIRE(policyResult.error().code == Sora::Compute::KernelIrErrorCode::InvalidPolicy);

    auto invalidParameter = kContractedModule;
    invalidParameter.parameters[0].kind = static_cast<Sora::Compute::ParameterKind>(0xFF);
    const auto parameterResult = Sora::Compute::Validate(invalidParameter);
    REQUIRE_FALSE(parameterResult.has_value());
    REQUIRE(parameterResult.error().code == Sora::Compute::KernelIrErrorCode::InvalidParameter);
    REQUIRE(parameterResult.error().index == 0);

    auto invalidOpcode = kContractedModule;
    invalidOpcode.instructions[3].opcode = static_cast<Sora::Compute::OpCode>(0xFF);
    invalidOpcode.instructions[3].operandCount = 0;
    invalidOpcode.instructions[3].operands = {};
    const auto opcodeResult = Sora::Compute::Validate(invalidOpcode);
    REQUIRE_FALSE(opcodeResult.has_value());
    REQUIRE(opcodeResult.error().code == Sora::Compute::KernelIrErrorCode::InvalidInstruction);
    REQUIRE(opcodeResult.error().index == 3);

    auto invalidScalar = kContractedModule;
    invalidScalar.parameters[0].elementType.scalar = static_cast<Sora::Compute::ScalarKind>(0xFF);
    invalidScalar.instructions[0].resultType.scalar = static_cast<Sora::Compute::ScalarKind>(0xFF);
    const auto scalarResult = Sora::Compute::Validate(invalidScalar);
    REQUIRE_FALSE(scalarResult.has_value());
    REQUIRE(scalarResult.error().code == Sora::Compute::KernelIrErrorCode::InvalidParameter);
}

TEST_CASE("canonical serialization is the semantic hash input", "[Sora][Compute][IR][Wire]") {
    const auto bytes = kContracted.PortableModule().Serialize();
    REQUIRE(bytes.size == kContracted.PortableModule().kWireCapacity);
    REQUIRE(Sora::Compute::Detail::HashBytes(bytes.Bytes()) == kContracted.Hash());
}

TEST_CASE("malformed active counts remain bounded before validation", "[Sora][Compute][IR][Validation]") {
    auto malformed = kContractedModule;
    malformed.instructionCount = 1000;
    REQUIRE(malformed.Instructions().size() == malformed.kInstructionCapacity);
    REQUIRE(malformed.Serialize().Bytes().size() <= malformed.kWireCapacity);
    REQUIRE(Sora::Compute::Validate(malformed).error().code == Sora::Compute::KernelIrErrorCode::InvalidCount);
}

TEST_CASE("canonical packages round-trip and reject trailing bytes", "[Sora][Compute][Package]") {
    const auto decoded = Sora::Compute::DecodeKernelPackage<3, 4>(kContractedBytes.Bytes());
    REQUIRE(decoded.has_value());
    REQUIRE(std::ranges::equal(decoded->Serialize().Bytes(), kContractedBytes.Bytes()));

    std::array<std::byte, kContractedBytes.storage.size() + 1> extended{};
    std::ranges::copy(kContractedBytes.Bytes(), extended.begin());
    const auto invalid = Sora::Compute::DecodeKernelPackage<3, 4>(std::span<const std::byte>{extended});
    REQUIRE_FALSE(invalid.has_value());
    REQUIRE(invalid.error().code == Sora::Compute::KernelPackageErrorCode::TrailingBytes);
    const auto truncated =
        Sora::Compute::DecodeKernelPackage<3, 4>(kContractedBytes.Bytes().first(kContractedBytes.Bytes().size() - 1));
    REQUIRE_FALSE(truncated.has_value());
    REQUIRE(truncated.error().code == Sora::Compute::KernelPackageErrorCode::Truncated);
}

TEST_CASE("planner accounts for transfer crossover instead of selecting by device class", "[Sora][Compute][Planner]") {
    constexpr Sora::Compute::DeviceId cpuId{1, 0};
    constexpr Sora::Compute::DeviceId gpuId{2, 0};
    const std::array candidates{
        Sora::Compute::PlacementCandidate{
            .service = {10, 1},
            .device = {.id = cpuId,
                       .backend = Sora::Compute::BackendKind::Cpu,
                       .kind = Sora::Compute::DeviceKind::Cpu,
                       .features = Sora::Compute::DeviceFeature::FusedMultiplyAdd,
                       .totalMemoryBytes = uint64_t{32} << 30U,
                       .maximumAllocationBytes = uint64_t{16} << 30U},
            .snapshot = {.id = cpuId,
                         .state = Sora::Compute::DeviceState::Available,
                         .generation = 1,
                         .availableMemoryBytes = uint64_t{24} << 30U},
            .executable = {.mode = Sora::Compute::ArtifactResolutionMode::Live},
            .workItemsPerSecond = 100'000'000,
            .transfer = Sora::Compute::CandidateTransferCost{512, 6},
            .launchNanoseconds = 200,
            .deterministic = true,
        },
        Sora::Compute::PlacementCandidate{
            .service = {20, 1},
            .device = {.id = gpuId,
                       .backend = Sora::Compute::BackendKind::Vulkan,
                       .kind = Sora::Compute::DeviceKind::DiscreteGpu,
                       .features = Sora::Compute::DeviceFeature::FusedMultiplyAdd,
                       .totalMemoryBytes = uint64_t{16} << 30U,
                       .maximumAllocationBytes = uint64_t{12} << 30U},
            .snapshot = {.id = gpuId,
                         .state = Sora::Compute::DeviceState::Available,
                         .generation = 1,
                         .availableMemoryBytes = uint64_t{12} << 30U},
            .executable = {.mode = Sora::Compute::ArtifactResolutionMode::Live},
            .workItemsPerSecond = 2'000'000'000,
            .transfer = Sora::Compute::CandidateTransferCost{512, 32},
            .launchNanoseconds = 20'000,
            .deterministic = true,
        },
    };

    const Sora::Compute::WorkloadDesc small{
        .kernelHash = {1, 1},
        .resourceDemand = {.floatingPointOperations = 2, .bytesRead = 8, .bytesWritten = 4},
        .workItemCount = 32,
        .requiredDeviceMemoryBytes = 512,
        .largestAllocationBytes = 384};
    const auto smallPlan =
        Sora::Compute::PlanExecution(small, std::span<const Sora::Compute::PlacementCandidate>{candidates});
    REQUIRE(smallPlan.has_value());
    REQUIRE(smallPlan->backend == Sora::Compute::BackendKind::Cpu);

    const Sora::Compute::WorkloadDesc residentLarge{
        .kernelHash = {1, 1},
        .resourceDemand = {.floatingPointOperations = 2, .bytesRead = 8, .bytesWritten = 4},
        .workItemCount = 100'000'000,
        .requiredDeviceMemoryBytes = 1'000'000'000,
        .largestAllocationBytes = 1'000'000'000,
        .expectedReuseCount = 100};
    const auto largePlan =
        Sora::Compute::PlanExecution(residentLarge, std::span<const Sora::Compute::PlacementCandidate>{candidates});
    REQUIRE(largePlan.has_value());
    REQUIRE(largePlan->backend == Sora::Compute::BackendKind::Vulkan);
    REQUIRE(Sora::Compute::Detail::RateCost(1'000'000'000'000, 1'000'000'000'000) == 1'000'000'000);
}

TEST_CASE("planner prices and independently gates cold pipeline creation", "[Sora][Compute][Planner][Pipeline]") {
    constexpr Sora::Compute::DeviceId device{2, 0};
    constexpr Sora::Compute::TargetFingerprint target{.device = device,
                                                      .backend = Sora::Compute::BackendKind::Vulkan,
                                                      .hostArchitecture = Sora::CpuArchitecture::X64,
                                                      .operatingSystem = Sora::OperatingSystem::Windows,
                                                      .binaryFormat = Sora::BinaryFormat::PE,
                                                      .byteOrder = Sora::ByteOrder::Little,
                                                      .abi = Sora::AbiFamily::MSVC,
                                                      .identityStability =
                                                          Sora::Compute::TargetIdentityStability::MachineStable,
                                                      .addressBits = 64,
                                                      .architectureId = 7,
                                                      .compilerIdentity = {8, 9},
                                                      .driverIdentity = {10, 11}};
    constexpr Sora::Compute::PipelineKey pipelineKey{
        .executable = {12, 13}, .target = target, .builder = {14, 15}, .state = {16, 17}, .bindingLayout = {18, 19}};
    constexpr Sora::Compute::PlacementCandidate cold{
        .service = {20, 21},
        .device = {.id = device,
                   .backend = Sora::Compute::BackendKind::Vulkan,
                   .kind = Sora::Compute::DeviceKind::DiscreteGpu,
                   .maximumAllocationBytes = 1'024},
        .target = target,
        .queue = {22, 23},
        .snapshot = {.id = device,
                     .state = Sora::Compute::DeviceState::Available,
                     .generation = 1,
                     .availableMemoryBytes = 1'024},
        .executable = {.mode = Sora::Compute::ArtifactResolutionMode::Live},
        .pipeline =
            Sora::Compute::PipelinePlacement{
                .key = pipelineKey,
                .resolution = {.mode = Sora::Compute::ArtifactResolutionMode::TransientProduction,
                               .productionNanoseconds = 400}},
        .workItemsPerSecond = 1'000'000'000};
    constexpr Sora::Compute::WorkloadDesc workload{.kernelHash = {24, 25},
                                                   .resourceDemand = {.bytesRead = 4, .bytesWritten = 4},
                                                   .workItemCount = 128,
                                                   .expectedReuseCount = 4};
    constexpr std::array coldCandidates{cold};
    constexpr auto allowed = Sora::Compute::PlanExecution(workload, coldCandidates);
    static_assert(allowed.has_value());
    static_assert(allowed->cost.amortizedPipelineCreationNanoseconds == 100);
    constexpr auto blocked =
        Sora::Compute::PlanExecution(workload, coldCandidates, {.allowRuntimePipelineCreation = false});
    static_assert(blocked.error() == Sora::Compute::PlanningError::NoCandidate);

    constexpr auto live = [cold] {
        auto candidate = cold;
        candidate.pipeline->resolution = {.mode = Sora::Compute::ArtifactResolutionMode::Live};
        return candidate;
    }();
    constexpr std::array liveCandidates{live};
    constexpr auto livePlan =
        Sora::Compute::PlanExecution(workload, liveCandidates, {.allowRuntimePipelineCreation = false});
    static_assert(livePlan.has_value());
    static_assert(livePlan->cost.amortizedPipelineCreationNanoseconds == 0);
}

TEST_CASE("planner selects compiler targets from import and production forecasts",
          "[Sora][Compute][Planner][ArtifactResolution]") {
    constexpr Sora::Compute::DeviceId device{30, 0};
    constexpr Sora::Compute::TargetFingerprint importTarget{
        .device = device,
        .backend = Sora::Compute::BackendKind::Cpu,
        .hostArchitecture = Sora::CpuArchitecture::X64,
        .operatingSystem = Sora::OperatingSystem::Windows,
        .binaryFormat = Sora::BinaryFormat::PE,
        .byteOrder = Sora::ByteOrder::Little,
        .abi = Sora::AbiFamily::MSVC,
        .identityStability = Sora::Compute::TargetIdentityStability::MachineStable,
        .addressBits = 64,
        .architectureId = 31,
        .compilerIdentity = {32, 33},
    };
    constexpr auto makeCandidate = [device](Sora::Compute::TargetFingerprint target,
                                            Sora::Compute::ArtifactResolutionForecast resolution) {
        return Sora::Compute::PlacementCandidate{
            .service = {34, 35},
            .device = {.id = device,
                       .backend = Sora::Compute::BackendKind::Cpu,
                       .kind = Sora::Compute::DeviceKind::Cpu,
                       .maximumAllocationBytes = 1'024},
            .target = target,
            .queue = {36, 37},
            .snapshot = {.id = device,
                         .state = Sora::Compute::DeviceState::Available,
                         .generation = 1,
                         .availableMemoryBytes = 1'024},
            .executable = resolution,
            .workItemsPerSecond = 1'000'000'000,
        };
    };
    constexpr auto productionTarget = [importTarget] {
        auto target = importTarget;
        target.compilerIdentity = {38, 39};
        return target;
    }();
    constexpr Sora::Compute::WorkloadDesc workload{.kernelHash = {40, 41},
                                                   .resourceDemand = {.bytesRead = 4, .bytesWritten = 4},
                                                   .workItemCount = 1'000,
                                                   .expectedReuseCount = 1};
    constexpr std::array productionPreferred{
        makeCandidate(importTarget,
                      {.mode = Sora::Compute::ArtifactResolutionMode::PackageImport, .importNanoseconds = 900}),
        makeCandidate(productionTarget, {.mode = Sora::Compute::ArtifactResolutionMode::TransientProduction,
                                         .productionNanoseconds = 300}),
    };
    constexpr auto productionPlan = Sora::Compute::PlanExecution(workload, productionPreferred);
    static_assert(productionPlan.has_value());
    static_assert(productionPlan->target == productionTarget);
    static_assert(productionPlan->cost.amortizedExecutableCompilationNanoseconds == 300);

    constexpr std::array importPreferred{
        makeCandidate(importTarget,
                      {.mode = Sora::Compute::ArtifactResolutionMode::PackageImport, .importNanoseconds = 100}),
        makeCandidate(productionTarget, {.mode = Sora::Compute::ArtifactResolutionMode::TransientProduction,
                                         .productionNanoseconds = 700}),
    };
    constexpr auto importPlan = Sora::Compute::PlanExecution(workload, importPreferred);
    static_assert(importPlan.has_value());
    static_assert(importPlan->target == importTarget);
    static_assert(importPlan->cost.amortizedExecutableImportNanoseconds == 100);
}

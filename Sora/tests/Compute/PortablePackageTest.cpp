#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>
#include <Sora/Compute/Runtime/PortablePackage.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>
#include <vector>

namespace {

    consteval auto MakeIndexProgram() {
        constexpr Sora::Compute::ValueType indexType{Sora::Compute::ScalarKind::UInt64, 1};
        Sora::Compute::KernelBuilder<1, 1> scalar;
        const auto index = scalar.AddValueParameter(indexType);
        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<1, 1, 1> builder{
            domain, std::move(scalar).Finish(index), Sora::Compute::PartitionContract::Elementwise(domain.rank)};
        const auto output = builder.AddBufferArgument(Sora::Compute::AccessMode::Write, indexType, domain.rank);
        builder.BindLogicalIndex(0);
        return std::move(builder).Finish(output);
    }

    constexpr auto kProgram = MakeIndexProgram();
    constexpr Sora::Compute::TargetFingerprint kTargetA{
        .device = {0xA001, 0xA002},
        .backend = Sora::Compute::BackendKind::Cpu,
        .hostArchitecture = Sora::CpuArchitecture::X64,
        .operatingSystem = Sora::OperatingSystem::Windows,
        .binaryFormat = Sora::BinaryFormat::PE,
        .byteOrder = Sora::ByteOrder::Little,
        .abi = Sora::AbiFamily::MSVC,
        .identityStability = Sora::Compute::TargetIdentityStability::MachineStable,
        .addressBits = 64,
        .features = Sora::Compute::DeviceFeature::FusedMultiplyAdd,
        .architectureId = 0x7838365F3634,
        .fixedVectorBits = 256,
        .compilerIdentity = {0xC001, 0xC002},
        .driverIdentity = {0xD001, 0xD002},
    };
    constexpr Sora::Compute::TargetFingerprint kTargetB = [] {
        auto target = kTargetA;
        target.device = {0xB001, 0xB002};
        target.architectureId = 0x7838365F3635;
        return target;
    }();
    constexpr Sora::Compute::SpecializationKey kSpecialization{
        .shape = {1, 2},
        .layout = {3, 4},
        .tuning = {5, 6},
    };
    constexpr auto kLowering = Sora::Compute::MakeArtifactKey(
        Sora::Compute::LoweringKey{.module = kProgram.Hash(), .lowerer = {0x100, 0x101}});
    constexpr auto kExecutable = Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
        .lowered = Sora::Compute::ArtifactIdentity(kLowering), .target = kTargetA, .specialization = kSpecialization});
    constexpr auto kPipeline = Sora::Compute::MakeArtifactKey(
        Sora::Compute::PipelineKey{.executable = Sora::Compute::ArtifactIdentity(kExecutable),
                                   .target = kTargetA,
                                   .builder = {0x200, 0x201},
                                   .state = {0x300, 0x301},
                                   .bindingLayout = {0x400, 0x401}});

    Sora::Compute::ProgramPackage Program() {
        auto package = Sora::Compute::ProgramPackage::Create(kProgram);
        REQUIRE(package.has_value());
        return *package;
    }

    Sora::Compute::PackagedArtifact Seed(Sora::Compute::ArtifactKey key, std::byte value) {
        return {.key = key, .payload = {value, std::byte{0x5A}}};
    }

} // namespace

TEST_CASE("PortablePackage canonicalizes one coherent program-to-pipeline "
          "artifact DAG",
          "[Sora][Compute][PortablePackage][Validation]") {
    std::vector artifacts{Seed(kPipeline, std::byte{3}), Seed(kExecutable, std::byte{2}),
                          Seed(kLowering, std::byte{1})};
    auto package = Sora::Compute::PortablePackage::Create(Program(), std::move(artifacts));
    REQUIRE(package.has_value());
    REQUIRE(package->Program().Hash() == kProgram.Hash());
    REQUIRE(package->Artifacts().Program() == kProgram.Hash());
    REQUIRE(package->Artifacts().Entries().size() == 3);
    REQUIRE(package->Artifacts().Entries()[0].key.Kind() == Sora::Compute::ArtifactKind::PortableLowering);
    REQUIRE(package->Artifacts().Entries()[1].key.Kind() == Sora::Compute::ArtifactKind::DeviceExecutable);
    REQUIRE(package->Artifacts().Entries()[2].key.Kind() == Sora::Compute::ArtifactKind::DevicePipeline);

    const auto executable = package->Artifacts().Find(kExecutable);
    REQUIRE(executable.has_value());
    REQUIRE((*executable)[0] == std::byte{2});
    const auto otherTarget = Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
        .lowered = Sora::Compute::ArtifactIdentity(kLowering), .target = kTargetB, .specialization = kSpecialization});
    REQUIRE_FALSE(package->Artifacts().Find(otherTarget).has_value());
}

TEST_CASE("PortablePackage rejects duplicate, nonpersistent, and orphaned "
          "artifact seeds",
          "[Sora][Compute][PortablePackage][Validation]") {
    SECTION("duplicate exact key") {
        auto package = Sora::Compute::PortablePackage::Create(
            Program(), {Seed(kLowering, std::byte{1}), Seed(kLowering, std::byte{2})});
        REQUIRE_FALSE(package.has_value());
        REQUIRE(package.error().code == Sora::Compute::PortablePackageErrorCode::DuplicateArtifact);
    }

    SECTION("process-only target") {
        auto processTarget = kTargetA;
        processTarget.identityStability = Sora::Compute::TargetIdentityStability::ProcessStable;
        const auto executable = Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
            .lowered = kProgram.Hash(), .target = processTarget, .specialization = kSpecialization});
        auto package = Sora::Compute::PortablePackage::Create(Program(), {Seed(executable, std::byte{1})});
        REQUIRE_FALSE(package.has_value());
        REQUIRE(package.error().code == Sora::Compute::PortablePackageErrorCode::NonPersistentArtifact);
    }

    SECTION("orphan lowering") {
        const auto orphan = Sora::Compute::MakeArtifactKey(
            Sora::Compute::LoweringKey{.module = {0xBAD, 0xF00D}, .lowerer = {0x100, 0x101}});
        auto package = Sora::Compute::PortablePackage::Create(Program(), {Seed(orphan, std::byte{1})});
        REQUIRE_FALSE(package.has_value());
        REQUIRE(package.error().code == Sora::Compute::PortablePackageErrorCode::IncoherentArtifactGraph);
    }

    SECTION("pipeline executable is absent") {
        const auto otherExecutable = Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
            .lowered = Sora::Compute::ArtifactIdentity(kLowering),
            .target = kTargetB,
            .specialization = kSpecialization,
        });
        const auto orphanedPipeline = Sora::Compute::MakeArtifactKey(
            Sora::Compute::PipelineKey{.executable = Sora::Compute::ArtifactIdentity(otherExecutable),
                                       .target = kTargetB,
                                       .builder = {0x200, 0x201},
                                       .state = {0x300, 0x301},
                                       .bindingLayout = {0x400, 0x401}});
        auto package = Sora::Compute::PortablePackage::Create(
            Program(),
            {Seed(kLowering, std::byte{1}), Seed(kExecutable, std::byte{2}), Seed(orphanedPipeline, std::byte{3})});
        REQUIRE_FALSE(package.has_value());
        REQUIRE(package.error().code == Sora::Compute::PortablePackageErrorCode::IncoherentArtifactGraph);
    }
}

TEST_CASE("PortablePackage rejects an empty program lease and empty backend payload",
          "[Sora][Compute][PortablePackage][Validation]") {
    auto invalidProgram = Sora::Compute::PortablePackage::Create({}, {});
    REQUIRE_FALSE(invalidProgram.has_value());
    REQUIRE(invalidProgram.error().code == Sora::Compute::PortablePackageErrorCode::InvalidProgram);

    auto emptyPayload =
        Sora::Compute::PortablePackage::Create(Program(), {Sora::Compute::PackagedArtifact{.key = kLowering}});
    REQUIRE_FALSE(emptyPayload.has_value());
    REQUIRE(emptyPayload.error().code == Sora::Compute::PortablePackageErrorCode::EmptyArtifactPayload);
}

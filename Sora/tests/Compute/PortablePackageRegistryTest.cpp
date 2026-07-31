/**
 * @file PortablePackageRegistryTest.cpp
 * @brief Transaction, snapshot, ownership, budget, and concurrency tests for
 * portable packages.
 */

#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>
#include <Sora/Compute/Runtime/PortablePackageRegistry.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

    consteval auto MakeIndexProgram(uint8_t rank) {
        constexpr Sora::Compute::ValueType indexType{Sora::Compute::ScalarKind::UInt64, 1};
        Sora::Compute::KernelBuilder<1, 1> scalar;
        const auto index = scalar.AddValueParameter(indexType);
        const Sora::Compute::IndexDomain domain{.rank = rank};
        Sora::Compute::KernelProgramBuilder<1, 1, 1> builder{
            domain, std::move(scalar).Finish(index), Sora::Compute::PartitionContract::Elementwise(domain.rank)};
        const auto output = builder.AddBufferArgument(Sora::Compute::AccessMode::Write, indexType, domain.rank);
        builder.BindLogicalIndex(0);
        return std::move(builder).Finish(output);
    }

    constexpr auto kProgramA = MakeIndexProgram(1);
    constexpr auto kProgramB = MakeIndexProgram(2);
    constexpr auto kBytesA = kProgramA.Serialize();
    constexpr Sora::Compute::TargetFingerprint kTarget{
        .device = {0xA001, 0xA002},
        .backend = Sora::Compute::BackendKind::Cpu,
        .hostArchitecture = Sora::CpuArchitecture::X64,
        .operatingSystem = Sora::OperatingSystem::Windows,
        .binaryFormat = Sora::BinaryFormat::PE,
        .byteOrder = Sora::ByteOrder::Little,
        .abi = Sora::AbiFamily::MSVC,
        .identityStability = Sora::Compute::TargetIdentityStability::MachineStable,
        .addressBits = 64,
        .architectureId = 0x7838365F3634,
        .fixedVectorBits = 256,
        .compilerIdentity = {0xC001, 0xC002},
        .driverIdentity = {0xD001, 0xD002},
    };
    constexpr Sora::Compute::SpecializationKey kSpecialization{
        .shape = {1, 2},
        .layout = {3, 4},
        .tuning = {5, 6},
    };
    constexpr auto kExecutableA = Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
        .lowered = kProgramA.Hash(), .target = kTarget, .specialization = kSpecialization});
    constexpr auto kExecutableB = Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
        .lowered = kProgramB.Hash(), .target = kTarget, .specialization = kSpecialization});
    constexpr Sora::Compute::SemanticHash kAuthorityA{0xA1, 0xA2};
    constexpr Sora::Compute::SemanticHash kAuthorityB{0xA3, 0xA4};
    constexpr Sora::Compute::SemanticHash kSubjectA{0xB1, 0xB2};
    constexpr Sora::Compute::SemanticHash kSubjectB{0xB3, 0xB4};
    constexpr Sora::Compute::PortablePackageProvenance kVerifiedProvenance =
        Sora::Compute::PortablePackageProvenance::Verified({
            .scheme = 7,
            .authority = kAuthorityA,
            .subject = kSubjectA,
            .policyRevision = 11,
        });
    constexpr auto kNewerPolicyProvenance = Sora::Compute::PortablePackageProvenance::Verified(
        {.scheme = 7, .authority = kAuthorityA, .subject = kSubjectA, .policyRevision = 13});
    constexpr auto kOtherSubjectProvenance = Sora::Compute::PortablePackageProvenance::Verified(
        {.scheme = 7, .authority = kAuthorityA, .subject = kSubjectB, .policyRevision = 9});
    constexpr auto kOtherAuthorityProvenance = Sora::Compute::PortablePackageProvenance::Verified(
        {.scheme = 9, .authority = kAuthorityB, .subject = kSubjectA, .policyRevision = 5});

    template<typename Program>
    Sora::Compute::PortablePackage Package(const Program& program,
                                           std::vector<Sora::Compute::PackagedArtifact> artifacts = {}) {
        auto canonical = Sora::Compute::ProgramPackage::Create(program);
        REQUIRE(canonical.has_value());
        auto package = Sora::Compute::PortablePackage::Create(std::move(*canonical), std::move(artifacts));
        REQUIRE(package.has_value());
        return std::move(*package);
    }

    Sora::Compute::PackagedArtifact Seed(Sora::Compute::ArtifactKey key, std::byte value) {
        return {.key = key, .payload = {value, std::byte{0x5A}}};
    }

    [[nodiscard]] bool Contains(std::span<const Sora::Compute::PortablePackageProvenance> provenance,
                                Sora::Compute::PortablePackageProvenance expected) {
        return std::ranges::find(provenance, expected) != provenance.end();
    }

} // namespace

TEST_CASE("PortablePackageRegistry publishes multiple programs in one "
          "immutable generation",
          "[Sora][Compute][Runtime][PortablePackageRegistry]") {
    Sora::Compute::PortablePackageRegistry registry;
    const auto first = registry.Register(kProgramA);
    const auto second = registry.Register(kProgramB);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    const auto snapshot = registry.Snapshot();
    REQUIRE(snapshot->PackageCount() == 2);
    REQUIRE(snapshot->ArtifactCount() == 0);
    REQUIRE(snapshot->ProvenanceCount() == 2);
    REQUIRE(snapshot->OwnedBytes() == kProgramA.Serialize().Bytes().size() + kProgramB.Serialize().Bytes().size());
    REQUIRE(snapshot->Resolve(kProgramA.Hash())->Program().Bytes().data() == first->Program().Bytes().data());
    REQUIRE(snapshot->Resolve(kProgramB.Hash())->Program().Bytes().data() == second->Program().Bytes().data());
    REQUIRE(first->Provenance().size() == 1);
    REQUIRE(first->Provenance().front() == Sora::Compute::PortablePackageProvenance::Direct());

    const auto duplicate = registry.Register(kProgramA);
    REQUIRE(duplicate.has_value());
    REQUIRE(duplicate->Program().Bytes().data() == first->Program().Bytes().data());
    REQUIRE(registry.Snapshot().get() == snapshot.get());
}

TEST_CASE("PortablePackageRegistry enriches admission provenance without copying package payloads",
          "[Sora][Compute][Runtime][PortablePackageRegistry][Provenance][COW]") {
    Sora::Compute::PortablePackageRegistry registry;
    const auto initial = registry.Register(Package(kProgramA, {Seed(kExecutableA, std::byte{7})}));
    REQUIRE(initial.has_value());
    const auto initialSnapshot = registry.Snapshot();
    const std::byte* programBytes = initial->Program().Bytes().data();
    const auto initialArtifacts = initial->Package().Artifacts().Entries();
    const std::byte* artifactBytes = initialArtifacts.front().payload.data();
    REQUIRE(initial->Provenance().size() == 1);
    REQUIRE(initial->Provenance().front() == Sora::Compute::PortablePackageProvenance::Direct());

    const auto duplicate = registry.Register(Package(kProgramA, {Seed(kExecutableA, std::byte{7})}));
    REQUIRE(duplicate.has_value());
    REQUIRE(registry.Snapshot().get() == initialSnapshot.get());
    REQUIRE(duplicate->Program().Bytes().data() == programBytes);
    REQUIRE(duplicate->Provenance().size() == 1);

    const auto enriched =
        registry.Register(Package(kProgramA, {Seed(kExecutableA, std::byte{7})}), kVerifiedProvenance);
    REQUIRE(enriched.has_value());
    const auto enrichedSnapshot = registry.Snapshot();
    REQUIRE(enrichedSnapshot.get() != initialSnapshot.get());
    REQUIRE(enrichedSnapshot->PackageCount() == 1);
    REQUIRE(enrichedSnapshot->ArtifactCount() == 1);
    REQUIRE(enrichedSnapshot->ProvenanceCount() == 2);
    REQUIRE(enrichedSnapshot->OwnedBytes() == initialSnapshot->OwnedBytes());
    REQUIRE(enriched->Program().Bytes().data() == programBytes);
    const auto enrichedArtifacts = enriched->Package().Artifacts().Entries();
    REQUIRE(enrichedArtifacts.front().payload.data() == artifactBytes);
    REQUIRE(enriched->Provenance().size() == 2);
    REQUIRE(Contains(enriched->Provenance(), Sora::Compute::PortablePackageProvenance::Direct()));
    REQUIRE(Contains(enriched->Provenance(), kVerifiedProvenance));

    REQUIRE(initial->Provenance().size() == 1);
    REQUIRE(initialSnapshot->ProvenanceCount() == 1);
    REQUIRE(initialSnapshot->Resolve(kProgramA.Hash())->Provenance().size() == 1);
    const auto resolved = registry.Resolve(kProgramA.Hash());
    REQUIRE(resolved.has_value());
    REQUIRE(resolved->Provenance().size() == 2);
    const auto seed = registry.FindArtifact(kExecutableA);
    REQUIRE(seed.has_value());
    REQUIRE(seed->Owner().Provenance().size() == 2);
    REQUIRE(Contains(seed->Owner().Provenance(), kVerifiedProvenance));

    const auto erased = registry.Erase(kProgramA.Hash());
    REQUIRE(erased.has_value());
    REQUIRE(*erased);
    REQUIRE(registry.Snapshot()->ProvenanceCount() == 0);
    REQUIRE(enriched->Provenance().size() == 2);
    REQUIRE(seed->Owner().Provenance().size() == 2);
    REQUIRE(seed->Bytes().data() == artifactBytes);
}

TEST_CASE("PortablePackageRegistry atomically revokes current detached admissions",
          "[Sora][Compute][Runtime][PortablePackageRegistry][Provenance][Revocation]") {
    using Sora::Compute::PortablePackageAdmissionRevocation;
    constexpr auto oldSubject = PortablePackageAdmissionRevocation::ForSubject(7, kAuthorityA, kSubjectA, 11);
    static_assert(oldSubject.Valid());
    static_assert(oldSubject.Matches(kVerifiedProvenance));
    static_assert(!oldSubject.Matches(kNewerPolicyProvenance));
    static_assert(!oldSubject.Matches(Sora::Compute::PortablePackageProvenance::Direct()));

    Sora::Compute::PortablePackageRegistry registry;
    const auto empty = registry.Snapshot();
    const auto invalid = registry.RevokeAdmissions({});
    REQUIRE_FALSE(invalid.has_value());
    REQUIRE(invalid.error().code == Sora::Compute::PortablePackageRegistryErrorCode::InvalidRevocation);
    REQUIRE(registry.Snapshot().get() == empty.get());

    const auto first = registry.Register(Package(kProgramA, {Seed(kExecutableA, std::byte{7})}), kVerifiedProvenance);
    REQUIRE(first.has_value());
    REQUIRE(registry.Register(Package(kProgramA, {Seed(kExecutableA, std::byte{7})}), kNewerPolicyProvenance));
    REQUIRE(registry.Register(Package(kProgramA, {Seed(kExecutableA, std::byte{7})}), kOtherSubjectProvenance));
    REQUIRE(registry.Register(Package(kProgramA, {Seed(kExecutableA, std::byte{7})}), kOtherAuthorityProvenance));
    const auto second = registry.Register(Package(kProgramB, {Seed(kExecutableB, std::byte{8})}), kVerifiedProvenance);
    REQUIRE(second.has_value());
    const auto before = registry.Snapshot();
    const auto latestFirst = before->Resolve(kProgramA.Hash());
    const auto oldSecondSeed = before->FindArtifact(kExecutableB);
    REQUIRE(latestFirst.has_value());
    REQUIRE(oldSecondSeed.has_value());
    REQUIRE(before->PackageCount() == 2);
    REQUIRE(before->ProvenanceCount() == 5);
    REQUIRE(latestFirst->Provenance().size() == 4);
    const std::byte* firstProgramBytes = latestFirst->Program().Bytes().data();
    const std::byte* firstArtifactBytes = before->FindArtifact(kExecutableA)->Bytes().data();

    const auto revokedOldSubject = registry.RevokeAdmissions(oldSubject);
    REQUIRE(revokedOldSubject.has_value());
    REQUIRE(revokedOldSubject->provenanceRemoved == 2);
    REQUIRE(revokedOldSubject->packagesUpdated == 1);
    REQUIRE(revokedOldSubject->packagesErased == 1);
    const auto afterSubject = registry.Snapshot();
    REQUIRE(afterSubject.get() != before.get());
    REQUIRE(afterSubject->PackageCount() == 1);
    REQUIRE(afterSubject->ArtifactCount() == 1);
    REQUIRE(afterSubject->ProvenanceCount() == 3);
    REQUIRE_FALSE(afterSubject->Resolve(kProgramB.Hash()).has_value());
    REQUIRE_FALSE(afterSubject->FindArtifact(kExecutableB).has_value());
    const auto retainedFirst = afterSubject->Resolve(kProgramA.Hash());
    REQUIRE(retainedFirst.has_value());
    REQUIRE(retainedFirst->Program().Bytes().data() == firstProgramBytes);
    REQUIRE(afterSubject->FindArtifact(kExecutableA)->Bytes().data() == firstArtifactBytes);
    REQUIRE(Contains(retainedFirst->Provenance(), kNewerPolicyProvenance));
    REQUIRE(Contains(retainedFirst->Provenance(), kOtherSubjectProvenance));
    REQUIRE(Contains(retainedFirst->Provenance(), kOtherAuthorityProvenance));
    REQUIRE_FALSE(Contains(retainedFirst->Provenance(), kVerifiedProvenance));

    REQUIRE(before->PackageCount() == 2);
    REQUIRE(before->ProvenanceCount() == 5);
    REQUIRE(latestFirst->Provenance().size() == 4);
    REQUIRE(oldSecondSeed->Bytes()[0] == std::byte{8});
    REQUIRE(second->Provenance().size() == 1);

    const auto unchangedGeneration = registry.Snapshot();
    const auto duplicateRevocation = registry.RevokeAdmissions(oldSubject);
    REQUIRE(duplicateRevocation.has_value());
    REQUIRE_FALSE(duplicateRevocation->Changed());
    REQUIRE(registry.Snapshot().get() == unchangedGeneration.get());

    const auto revokedAuthority =
        registry.RevokeAdmissions(PortablePackageAdmissionRevocation::ForAuthority(7, kAuthorityA));
    REQUIRE(revokedAuthority.has_value());
    REQUIRE(revokedAuthority->provenanceRemoved == 2);
    REQUIRE(revokedAuthority->packagesUpdated == 1);
    REQUIRE(revokedAuthority->packagesErased == 0);
    REQUIRE(registry.Resolve(kProgramA.Hash())->Provenance().size() == 1);
    REQUIRE(registry.Resolve(kProgramA.Hash())->Provenance().front() == kOtherAuthorityProvenance);

    const auto revokedLast =
        registry.RevokeAdmissions(PortablePackageAdmissionRevocation::ForAuthority(9, kAuthorityB));
    REQUIRE(revokedLast.has_value());
    REQUIRE(revokedLast->provenanceRemoved == 1);
    REQUIRE(revokedLast->packagesUpdated == 0);
    REQUIRE(revokedLast->packagesErased == 1);
    REQUIRE(registry.Snapshot()->PackageCount() == 0);
    REQUIRE(latestFirst->Program().Bytes().data() == firstProgramBytes);
}

TEST_CASE("ProgramPackage accepts only canonical externally supplied bytes",
          "[Sora][Compute][Runtime][PortablePackageRegistry][ProgramPackage]") {
    const auto loaded = Sora::Compute::ProgramPackage::Load(kBytesA.Bytes());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->Hash() == kProgramA.Hash());
    REQUIRE(std::ranges::equal(loaded->Bytes(), kBytesA.Bytes()));

    auto corrupted = kBytesA;
    corrupted.storage[0] ^= std::byte{0xFF};
    const auto invalid = Sora::Compute::ProgramPackage::Load(corrupted.Bytes());
    REQUIRE_FALSE(invalid.has_value());
    REQUIRE(invalid.error().code == Sora::Compute::ProgramPackageErrorCode::InvalidProgram);
    REQUIRE(invalid.error().detail ==
            static_cast<uint64_t>(Sora::Compute::KernelProgramPackageErrorCode::InvalidMagic));
    REQUIRE(invalid.error().offset == 0);
}

TEST_CASE("Backend-independent ProgramPackage validation agrees with the fixed-capacity decoder",
          "[Sora][Compute][Runtime][PortablePackageRegistry][ProgramPackage][Differential]") {
    constexpr std::array masks{std::byte{0x01}, std::byte{0x80}};
    for (size_t offset = 0; offset < kBytesA.Bytes().size(); ++offset) {
        for (const std::byte mask : masks) {
            std::vector<std::byte> bytes{kBytesA.Bytes().begin(), kBytesA.Bytes().end()};
            bytes[offset] ^= mask;

            const auto fixed = Sora::Compute::DecodeKernelProgramPackage<1, 1, 1>(bytes);
            bool fixedCanonical = false;
            if (fixed) {
                const auto encoded = fixed->Serialize();
                fixedCanonical = std::ranges::equal(encoded.Bytes(), bytes);
            }
            const auto dynamic = Sora::Compute::ProgramPackage::Validate(bytes);
            CAPTURE(offset, std::to_integer<uint8_t>(mask));
            REQUIRE(dynamic.has_value() == fixedCanonical);
        }
    }
}

TEST_CASE("PortablePackageRegistry rejects conflicting same-program packages "
          "without publication",
          "[Sora][Compute][Runtime][PortablePackageRegistry][Conflict]") {
    Sora::Compute::PortablePackageRegistry registry;
    const auto original = registry.Register(Package(kProgramA, {Seed(kExecutableA, std::byte{1})}));
    REQUIRE(original.has_value());
    const auto generation = registry.Snapshot();

    const auto conflict = registry.Register(Package(kProgramA, {Seed(kExecutableA, std::byte{2})}));
    REQUIRE_FALSE(conflict.has_value());
    REQUIRE(conflict.error().code == Sora::Compute::PortablePackageRegistryErrorCode::PackageConflict);
    REQUIRE(registry.Snapshot().get() == generation.get());
    REQUIRE(registry.FindArtifact(kExecutableA)->Bytes()[0] == std::byte{1});
}

TEST_CASE("PortablePackageRegistry preserves artifact owners across program-index insertion and erase",
          "[Sora][Compute][Runtime][PortablePackageRegistry][Index]") {
    const bool aBeforeB = Sora::Compute::SemanticHashLess{}(kProgramA.Hash(), kProgramB.Hash());
    const auto* lowerProgram = aBeforeB ? &kProgramA : &kProgramB;
    const auto* higherProgram = aBeforeB ? &kProgramB : &kProgramA;
    const auto* lowerExecutable = aBeforeB ? &kExecutableA : &kExecutableB;
    const auto* higherExecutable = aBeforeB ? &kExecutableB : &kExecutableA;
    const std::byte lowerValue = aBeforeB ? std::byte{1} : std::byte{2};
    const std::byte higherValue = aBeforeB ? std::byte{2} : std::byte{1};

    Sora::Compute::PortablePackageRegistry registry;
    REQUIRE(registry.Register(Package(*higherProgram, {Seed(*higherExecutable, higherValue)})).has_value());
    REQUIRE(registry.Register(Package(*lowerProgram, {Seed(*lowerExecutable, lowerValue)})).has_value());
    const auto erased = registry.Erase(lowerProgram->Hash());
    REQUIRE(erased.has_value());
    REQUIRE(*erased);

    const auto remaining = registry.FindArtifact(*higherExecutable);
    REQUIRE(remaining.has_value());
    REQUIRE(remaining->Bytes()[0] == higherValue);

    REQUIRE(registry.Register(Package(*lowerProgram, {Seed(*lowerExecutable, lowerValue)})).has_value());
    REQUIRE(registry.FindArtifact(*lowerExecutable)->Bytes()[0] == lowerValue);
    REQUIRE(registry.FindArtifact(*higherExecutable)->Bytes()[0] == higherValue);
}

TEST_CASE("PortablePackageRegistry enforces package, artifact, and byte "
          "budgets transactionally",
          "[Sora][Compute][Runtime][PortablePackageRegistry][Capacity]") {
    SECTION("invalid limits cannot create a registry") {
        REQUIRE_THROWS_AS((Sora::Compute::PortablePackageRegistry{
                              {.maximumPackages = 0, .maximumArtifacts = 0, .maximumOwnedBytes = 1}}),
                          std::invalid_argument);
        REQUIRE_THROWS_AS((Sora::Compute::PortablePackageRegistry{
                              {.maximumPackages = 1, .maximumArtifacts = 0, .maximumOwnedBytes = 0}}),
                          std::invalid_argument);
        REQUIRE_THROWS_AS(
            (Sora::Compute::PortablePackageRegistry{
                {.maximumPackages = 1, .maximumArtifacts = 0, .maximumProvenanceRecords = 0, .maximumOwnedBytes = 1}}),
            std::invalid_argument);
    }

    SECTION("zero artifact budget still admits canonical programs for JIT") {
        Sora::Compute::PortablePackageRegistry registry{
            {.maximumPackages = 1, .maximumArtifacts = 0, .maximumOwnedBytes = 1'024 * 1'024}};
        REQUIRE(registry.Register(kProgramA).has_value());
        REQUIRE(registry.Snapshot()->PackageCount() == 1);
        REQUIRE(registry.Snapshot()->ArtifactCount() == 0);
    }

    SECTION("package count") {
        Sora::Compute::PortablePackageRegistry registry{
            {.maximumPackages = 1, .maximumArtifacts = 8, .maximumOwnedBytes = 1'024 * 1'024}};
        REQUIRE(registry.Register(kProgramA).has_value());
        const auto rejected = registry.Register(kProgramB);
        REQUIRE_FALSE(rejected.has_value());
        REQUIRE(rejected.error().code == Sora::Compute::PortablePackageRegistryErrorCode::PackageCapacityExceeded);
        REQUIRE(registry.Snapshot()->PackageCount() == 1);
    }

    SECTION("artifact count") {
        Sora::Compute::PortablePackageRegistry registry{
            {.maximumPackages = 1, .maximumArtifacts = 1, .maximumOwnedBytes = 1'024 * 1'024}};
        constexpr auto secondTarget = [] {
            auto target = kTarget;
            target.device = {0xB001, 0xB002};
            ++target.architectureId;
            return target;
        }();
        constexpr auto secondExecutable = Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
            .lowered = kProgramA.Hash(), .target = secondTarget, .specialization = kSpecialization});
        const auto rejected = registry.Register(
            Package(kProgramA, {Seed(kExecutableA, std::byte{1}), Seed(secondExecutable, std::byte{2})}));
        REQUIRE_FALSE(rejected.has_value());
        REQUIRE(rejected.error().code == Sora::Compute::PortablePackageRegistryErrorCode::ArtifactCapacityExceeded);
        REQUIRE(registry.Snapshot()->PackageCount() == 0);
    }

    SECTION("owned bytes") {
        Sora::Compute::PortablePackageRegistry registry{
            {.maximumPackages = 1, .maximumArtifacts = 1, .maximumOwnedBytes = kBytesA.Bytes().size() - 1}};
        const auto rejected = registry.Register(kProgramA);
        REQUIRE_FALSE(rejected.has_value());
        REQUIRE(rejected.error().code == Sora::Compute::PortablePackageRegistryErrorCode::ByteCapacityExceeded);
        REQUIRE(registry.Snapshot()->OwnedBytes() == 0);
    }

    SECTION("provenance count") {
        Sora::Compute::PortablePackageRegistry registry{{.maximumPackages = 2,
                                                         .maximumArtifacts = 1,
                                                         .maximumProvenanceRecords = 1,
                                                         .maximumOwnedBytes = 1'024 * 1'024}};
        REQUIRE(registry.Register(Package(kProgramA, {Seed(kExecutableA, std::byte{1})})).has_value());
        const auto generation = registry.Snapshot();
        const auto rejected =
            registry.Register(Package(kProgramA, {Seed(kExecutableA, std::byte{1})}), kVerifiedProvenance);
        REQUIRE_FALSE(rejected.has_value());
        REQUIRE(rejected.error().code == Sora::Compute::PortablePackageRegistryErrorCode::ProvenanceCapacityExceeded);
        REQUIRE(registry.Snapshot().get() == generation.get());
        REQUIRE(registry.Resolve(kProgramA.Hash())->Provenance().size() == 1);
    }

    SECTION("invalid provenance") {
        Sora::Compute::PortablePackageRegistry registry;
        const auto generation = registry.Snapshot();
        auto invalid = Sora::Compute::PortablePackageProvenance::Direct();
        invalid.subject = {0xBAD, 0xBAD};
        const auto rejected = registry.Register(Package(kProgramA), invalid);
        REQUIRE_FALSE(rejected.has_value());
        REQUIRE(rejected.error().code == Sora::Compute::PortablePackageRegistryErrorCode::InvalidProvenance);
        REQUIRE(registry.Snapshot().get() == generation.get());
    }
}

TEST_CASE("Registry snapshots and artifact seeds retain their generation across "
          "erase and clear",
          "[Sora][Compute][Runtime][PortablePackageRegistry][Snapshot][Lease]") {
    Sora::Compute::PortablePackageRegistry registry;
    REQUIRE(registry.Register(Package(kProgramA, {Seed(kExecutableA, std::byte{7})})).has_value());
    const auto beforeErase = registry.Snapshot();
    const auto seed = beforeErase->FindArtifact(kExecutableA);
    REQUIRE(seed.has_value());
    const std::byte* payload = seed->Bytes().data();

    const auto erased = registry.Erase(kProgramA.Hash());
    REQUIRE(erased.has_value());
    REQUIRE(*erased);
    REQUIRE(registry.Snapshot()->PackageCount() == 0);
    REQUIRE(beforeErase->PackageCount() == 1);
    REQUIRE(beforeErase->Resolve(kProgramA.Hash()).has_value());
    REQUIRE(seed->Bytes().data() == payload);
    REQUIRE(seed->Bytes()[0] == std::byte{7});

    REQUIRE(registry.Register(kProgramB).has_value());
    REQUIRE(registry.Clear().has_value());
    const auto empty = registry.Snapshot();
    REQUIRE(empty->PackageCount() == 0);
    REQUIRE(registry.Clear().has_value());
    REQUIRE(registry.Snapshot().get() == empty.get());
    REQUIRE(seed->Owner().Program().Hash() == kProgramA.Hash());
}

TEST_CASE("PortablePackageRegistry canonicalizes concurrent duplicate registration",
          "[Sora][Compute][Runtime][PortablePackageRegistry][Concurrent]") {
    Sora::Compute::PortablePackageRegistry registry;
    constexpr size_t threadCount = 8;
    std::array<const std::byte*, threadCount> identities{};
    std::array<std::jthread, threadCount> threads;
    std::atomic<bool> failed = false;
    for (size_t index = 0; index < threadCount; ++index) {
        threads[index] = std::jthread([&, index] {
            const auto registered = registry.Register(kProgramA);
            if (!registered) {
                failed.store(true, std::memory_order_relaxed);
                return;
            }
            identities[index] = registered->Program().Bytes().data();
        });
    }
    for (std::jthread& thread : threads) {
        thread.join();
    }

    REQUIRE_FALSE(failed.load(std::memory_order_relaxed));
    REQUIRE(registry.Snapshot()->PackageCount() == 1);
    for (const std::byte* identity : identities) {
        REQUIRE(identity == identities.front());
    }
}

TEST_CASE("PortablePackageRegistry readers observe coherent generations during register and erase",
          "[Sora][Compute][Runtime][PortablePackageRegistry][Concurrent][Snapshot]") {
    Sora::Compute::PortablePackageRegistry registry;
    REQUIRE(registry.Register(Package(kProgramA, {Seed(kExecutableA, std::byte{9})})).has_value());

    constexpr size_t readerCount = 4;
    constexpr size_t updateCount = 256;
    std::atomic<bool> stop = false;
    std::atomic<bool> failed = false;
    std::array<std::jthread, readerCount> readers;
    for (std::jthread& reader : readers) {
        reader = std::jthread([&] {
            while (!stop.load(std::memory_order_acquire)) {
                const auto snapshot = registry.Snapshot();
                const auto stable = snapshot->Resolve(kProgramA.Hash());
                const auto optional = snapshot->Resolve(kProgramB.Hash());
                const auto seed = snapshot->FindArtifact(kExecutableA);
                const bool coherent = stable && seed && seed->Bytes()[0] == std::byte{9} &&
                                      snapshot->PackageCount() == (optional ? 2 : 1) &&
                                      snapshot->ProvenanceCount() == snapshot->PackageCount() &&
                                      snapshot->ArtifactCount() == 1;
                if (!coherent) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }

    for (size_t update = 0; update < updateCount; ++update) {
        if (!registry.Register(kProgramB)) {
            failed.store(true, std::memory_order_relaxed);
            break;
        }
        const auto erased = registry.Erase(kProgramB.Hash());
        if (!erased || !*erased) {
            failed.store(true, std::memory_order_relaxed);
            break;
        }
    }
    stop.store(true, std::memory_order_release);
    for (std::jthread& reader : readers) {
        reader.join();
    }

    REQUIRE_FALSE(failed.load(std::memory_order_relaxed));
    REQUIRE(registry.Snapshot()->PackageCount() == 1);
    REQUIRE(registry.Resolve(kProgramA.Hash()).has_value());
}

TEST_CASE("PortablePackageRegistry readers observe coherent admission generations during revocation",
          "[Sora][Compute][Runtime][PortablePackageRegistry][Concurrent][Revocation]") {
    constexpr auto revocation =
        Sora::Compute::PortablePackageAdmissionRevocation::ForSubject(7, kAuthorityA, kSubjectA, 11);
    Sora::Compute::PortablePackageRegistry registry;
    REQUIRE(registry.Register(kProgramA).has_value());
    REQUIRE(registry.Register(Package(kProgramB, {Seed(kExecutableB, std::byte{9})})).has_value());
    REQUIRE(registry.Register(Package(kProgramB, {Seed(kExecutableB, std::byte{9})}), kVerifiedProvenance).has_value());

    constexpr size_t readerCount = 4;
    constexpr size_t updateCount = 128;
    std::atomic<bool> stop = false;
    std::atomic<bool> failed = false;
    std::array<std::jthread, readerCount> readers;
    for (std::jthread& reader : readers) {
        reader = std::jthread([&] {
            while (!stop.load(std::memory_order_acquire)) {
                const auto snapshot = registry.Snapshot();
                const auto first = snapshot->Resolve(kProgramA.Hash());
                const auto second = snapshot->Resolve(kProgramB.Hash());
                const auto seed = snapshot->FindArtifact(kExecutableB);
                if (!first || !second || !seed) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
                const auto provenance = second->Provenance();
                const bool canonical = (provenance.size() == 1 &&
                                        provenance.front() == Sora::Compute::PortablePackageProvenance::Direct()) ||
                                       (provenance.size() == 2 &&
                                        Contains(provenance, Sora::Compute::PortablePackageProvenance::Direct()) &&
                                        Contains(provenance, kVerifiedProvenance));
                const bool coherent = canonical && seed->Owner().Provenance().size() == provenance.size() &&
                                      snapshot->PackageCount() == 2 && snapshot->ArtifactCount() == 1 &&
                                      snapshot->ProvenanceCount() == 1 + provenance.size() &&
                                      seed->Bytes()[0] == std::byte{9};
                if (!coherent) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }

    for (size_t update = 0; update < updateCount; ++update) {
        const auto revoked = registry.RevokeAdmissions(revocation);
        if (!revoked || revoked->provenanceRemoved != 1 || revoked->packagesUpdated != 1 ||
            revoked->packagesErased != 0 ||
            !registry.Register(Package(kProgramB, {Seed(kExecutableB, std::byte{9})}), kVerifiedProvenance)) {
            failed.store(true, std::memory_order_relaxed);
            break;
        }
    }
    stop.store(true, std::memory_order_release);
    for (std::jthread& reader : readers) {
        reader.join();
    }

    REQUIRE_FALSE(failed.load(std::memory_order_relaxed));
    REQUIRE(registry.Snapshot()->PackageCount() == 2);
    REQUIRE(registry.Snapshot()->ProvenanceCount() == 3);
}

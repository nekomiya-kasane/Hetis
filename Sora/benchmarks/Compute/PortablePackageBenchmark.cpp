/**
 * @file PortablePackageBenchmark.cpp
 * @brief Immutable package-snapshot publication and exact-artifact lookup
 * baseline.
 */

#include <BenchmarkHarness.h>
#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>
#include <Sora/Compute/Runtime/PortablePackageArchive.h>
#include <Sora/Compute/Runtime/PortablePackageRegistry.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <print>
#include <utility>
#include <vector>

namespace {

    constexpr size_t kSamples = 9;
    constexpr auto kMinimumSampleTime = std::chrono::milliseconds(50);
    constexpr Sora::Compute::SpecializationKey kSpecialization{
        .shape = {3, 4},
        .layout = {5, 6},
        .tuning = {7, 8},
    };

    consteval auto MakeProgram(uint8_t rank) {
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

    constexpr auto kProgram = MakeProgram(1);
    constexpr auto kUpdateProgram = MakeProgram(2);
    constexpr auto kProgramIdentity = kProgram.Hash();
    constexpr auto kUpdateProgramIdentity = kUpdateProgram.Hash();
    constexpr auto kBenchmarkProvenance = Sora::Compute::PortablePackageProvenance::Verified({
        .scheme = 1,
        .authority = {0xA1, 0xA2},
        .subject = {0xB1, 0xB2},
        .policyRevision = 7,
    });
    constexpr auto kBenchmarkRevocation =
        Sora::Compute::PortablePackageAdmissionRevocation::ForSubject(1, {0xA1, 0xA2}, {0xB1, 0xB2});

    [[nodiscard]] constexpr Sora::Compute::TargetFingerprint Target(uint64_t device) noexcept {
        return {
            .device = {device, device + 1},
            .backend = Sora::Compute::BackendKind::Cpu,
            .hostArchitecture = Sora::CpuArchitecture::X64,
            .operatingSystem = Sora::OperatingSystem::Windows,
            .binaryFormat = Sora::BinaryFormat::PE,
            .byteOrder = Sora::ByteOrder::Little,
            .abi = Sora::AbiFamily::MSVC,
            .identityStability = Sora::Compute::TargetIdentityStability::MachineStable,
            .addressBits = 64,
            .architectureId = 0x1000 + device,
            .fixedVectorBits = 256,
            .compilerIdentity = {9, 10},
            .driverIdentity = {11, 12},
        };
    }

    [[nodiscard]] constexpr Sora::Compute::ArtifactKey Executable(uint64_t device) noexcept {
        return Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
            .lowered = kProgramIdentity, .target = Target(device), .specialization = kSpecialization});
    }

    class PortablePackageFixture final {
    public:
        PortablePackageFixture() {
            std::vector<Sora::Compute::PackagedArtifact> artifacts;
            artifacts.reserve(8);
            for (uint64_t device = 1; device <= 8; ++device) {
                artifacts.push_back(
                    {.key = Executable(device), .payload = {std::byte{static_cast<uint8_t>(device)}, std::byte{0x5A}}});
            }
            auto program = Sora::Compute::ProgramPackage::Create(kProgram);
            if (!program) {
                failed_ = true;
                return;
            }
            auto package = Sora::Compute::PortablePackage::Create(std::move(*program), std::move(artifacts));
            if (!package) {
                failed_ = true;
                return;
            }
            auto archive = Sora::Compute::PortablePackageArchive::Create(*package);
            if (!archive) {
                failed_ = true;
                return;
            }
            archiveBytes_.assign(archive->Bytes().begin(), archive->Bytes().end());
            auto archiveView = Sora::Compute::PortablePackageArchiveView::OpenTrusted(archiveBytes_);
            if (!archiveView) {
                failed_ = true;
                return;
            }
            archiveView_.emplace(std::move(*archiveView));
            if (!registry_.Register(std::move(*package))) {
                failed_ = true;
                return;
            }
            snapshot_ = registry_.Snapshot();
            const auto packageLease = snapshot_->Resolve(kProgram.Hash());
            if (!packageLease) {
                failed_ = true;
                return;
            }
            package_ = *packageLease;
        }

        void SnapshotLoad() noexcept {
            const auto snapshot = registry_.Snapshot();
            checksum_ += snapshot->ArtifactCount();
        }

        void FrozenExactHit() noexcept {
            const auto seed = snapshot_->FindArtifact(Executable(8));
            checksum_ += seed ? std::to_integer<uint8_t>(seed->Bytes()[0]) : 0;
        }

        void PackageExactHit() noexcept {
            const auto bytes = package_.Package().Artifacts().Find(Executable(8));
            checksum_ += bytes ? std::to_integer<uint8_t>((*bytes)[0]) : 0;
        }

        void PackageTargetMismatch() noexcept {
            const auto bytes = package_.Package().Artifacts().Find(Executable(9));
            checksum_ += bytes ? std::to_integer<uint8_t>((*bytes)[0]) : 1;
        }

        void RegistryExactHit() noexcept {
            const auto seed = registry_.FindArtifact(Executable(8));
            checksum_ += seed ? std::to_integer<uint8_t>(seed->Bytes()[0]) : 0;
        }

        void TargetMismatch() noexcept {
            const auto seed = snapshot_->FindArtifact(Executable(9));
            checksum_ += seed ? std::to_integer<uint8_t>(seed->Bytes()[0]) : 1;
        }

        void UpdateCycle() noexcept {
            const auto registered = registry_.Register(kUpdateProgram);
            if (!registered) {
                failed_ = true;
                return;
            }
            const auto erased = registry_.Erase(kUpdateProgramIdentity);
            if (!erased || !*erased) {
                failed_ = true;
                return;
            }
            checksum_ += registered->Program().Bytes().size();
        }

        void ExactDuplicateCycle() noexcept {
            const auto registered = registry_.Register(kUpdateProgram);
            const auto duplicate = registry_.Register(kUpdateProgram);
            if (!registered || !duplicate) {
                failed_ = true;
                return;
            }
            const auto erased = registry_.Erase(kUpdateProgramIdentity);
            if (!erased || !*erased) {
                failed_ = true;
                return;
            }
            checksum_ += duplicate->Provenance().size();
        }

        void ProvenanceEnrichmentCycle() noexcept {
            const auto registered = registry_.Register(kUpdateProgram);
            const auto enriched = registry_.Register(kUpdateProgram, kBenchmarkProvenance);
            if (!registered || !enriched) {
                failed_ = true;
                return;
            }
            const auto erased = registry_.Erase(kUpdateProgramIdentity);
            if (!erased || !*erased) {
                failed_ = true;
                return;
            }
            checksum_ += enriched->Provenance().size();
        }

        void AdmissionRevocationCycle() noexcept {
            const auto registered = registry_.Register(kUpdateProgram, kBenchmarkProvenance);
            if (!registered) {
                failed_ = true;
                return;
            }
            const auto revoked = registry_.RevokeAdmissions(kBenchmarkRevocation);
            if (!revoked || revoked->provenanceRemoved != 1 || revoked->packagesErased != 1) {
                failed_ = true;
                return;
            }
            checksum_ += revoked->provenanceRemoved;
        }

        void ArchiveOpenTrusted() noexcept {
            const auto archive = Sora::Compute::PortablePackageArchiveView::OpenTrusted(archiveBytes_);
            if (!archive) {
                failed_ = true;
                return;
            }
            checksum_ += archive->Artifacts().size() + archive->ProgramBytes().size();
        }

        void ArchiveMaterialize() noexcept {
            const auto package = archiveView_->Materialize();
            if (!package) {
                failed_ = true;
                return;
            }
            checksum_ += package->Program().Bytes().size() + package->Artifacts().Entries().size();
        }

        [[nodiscard]] bool Failed() const noexcept { return failed_ || snapshot_ == nullptr; }
        [[nodiscard]] uint64_t Checksum() const noexcept { return checksum_; }

    private:
        Sora::Compute::PortablePackageRegistry registry_{};
        std::shared_ptr<const Sora::Compute::PortablePackageRegistrySnapshot> snapshot_{};
        Sora::Compute::PortablePackageLease package_{};
        std::vector<std::byte> archiveBytes_{};
        std::optional<Sora::Compute::PortablePackageArchiveView> archiveView_{};
        uint64_t checksum_ = 0;
        bool failed_ = false;
    };

} // namespace

int main() {
    PortablePackageFixture fixture;
    if (fixture.Failed()) {
        return 2;
    }
    auto snapshotLoad = [&fixture]() noexcept { fixture.SnapshotLoad(); };
    auto frozenExactHit = [&fixture]() noexcept { fixture.FrozenExactHit(); };
    auto packageExactHit = [&fixture]() noexcept { fixture.PackageExactHit(); };
    auto packageTargetMismatch = [&fixture]() noexcept { fixture.PackageTargetMismatch(); };
    auto registryExactHit = [&fixture]() noexcept { fixture.RegistryExactHit(); };
    auto targetMismatch = [&fixture]() noexcept { fixture.TargetMismatch(); };
    auto updateCycle = [&fixture]() noexcept { fixture.UpdateCycle(); };
    auto exactDuplicateCycle = [&fixture]() noexcept { fixture.ExactDuplicateCycle(); };
    auto provenanceEnrichmentCycle = [&fixture]() noexcept { fixture.ProvenanceEnrichmentCycle(); };
    auto admissionRevocationCycle = [&fixture]() noexcept { fixture.AdmissionRevocationCycle(); };
    auto archiveOpenTrusted = [&fixture]() noexcept { fixture.ArchiveOpenTrusted(); };
    auto archiveMaterialize = [&fixture]() noexcept { fixture.ArchiveMaterialize(); };
    const std::array cases{Sora::Benchmark::MakeCase("snapshot_load", snapshotLoad),
                           Sora::Benchmark::MakeCase("package_exact_hit", packageExactHit),
                           Sora::Benchmark::MakeCase("package_target_mismatch", packageTargetMismatch),
                           Sora::Benchmark::MakeCase("frozen_exact_hit", frozenExactHit),
                           Sora::Benchmark::MakeCase("registry_exact_hit", registryExactHit),
                           Sora::Benchmark::MakeCase("target_mismatch", targetMismatch),
                           Sora::Benchmark::MakeCase("update_cycle", updateCycle),
                           Sora::Benchmark::MakeCase("exact_duplicate_cycle", exactDuplicateCycle),
                           Sora::Benchmark::MakeCase("provenance_enrichment_cycle", provenanceEnrichmentCycle),
                           Sora::Benchmark::MakeCase("admission_revocation_cycle", admissionRevocationCycle),
                           Sora::Benchmark::MakeCase("archive_open_trusted", archiveOpenTrusted),
                           Sora::Benchmark::MakeCase("archive_materialize", archiveMaterialize)};
    const auto timing = Sora::Benchmark::MeasureInterleavedIndependent<kSamples>(cases, 1, kMinimumSampleTime);
    if (fixture.Checksum() == 0 || fixture.Checksum() == std::numeric_limits<uint64_t>::max()) {
        return 3;
    }

    std::println("metric,value,unit");
    std::println("portable_package_snapshot_load,{:.6f},ns/operation", timing.MedianTime(0));
    std::println("portable_package_local_exact_hit,{:.6f},ns/operation", timing.MedianTime(1));
    std::println("portable_package_local_target_mismatch,{:.6f},ns/operation", timing.MedianTime(2));
    std::println("portable_package_frozen_exact_hit,{:.6f},ns/operation", timing.MedianTime(3));
    std::println("portable_package_registry_exact_hit,{:.6f},ns/operation", timing.MedianTime(4));
    std::println("portable_package_target_mismatch,{:.6f},ns/operation", timing.MedianTime(5));
    std::println("portable_package_register_erase_update_cycle,{:.6f},ns/operation", timing.MedianTime(6));
    std::println("portable_package_exact_duplicate_cycle,{:.6f},ns/operation", timing.MedianTime(7));
    std::println("portable_package_provenance_enrichment_cycle,{:.6f},ns/operation", timing.MedianTime(8));
    std::println("portable_package_provenance_enrichment_over_duplicate,{:.6f},ratio", timing.PairedRatio(8, 7));
    std::println("portable_package_admission_revocation_cycle,{:.6f},ns/operation", timing.MedianTime(9));
    std::println("portable_package_revocation_over_erase,{:.6f},ratio", timing.PairedRatio(9, 6));
    std::println("portable_package_archive_open_trusted,{:.6f},ns/operation", timing.MedianTime(10));
    std::println("portable_package_archive_materialize,{:.6f},ns/operation", timing.MedianTime(11));
    std::println("snapshot_load_repetitions,{},count", timing.repetitions[0]);
    std::println("package_exact_hit_repetitions,{},count", timing.repetitions[1]);
    std::println("package_target_mismatch_repetitions,{},count", timing.repetitions[2]);
    std::println("frozen_exact_hit_repetitions,{},count", timing.repetitions[3]);
    std::println("registry_exact_hit_repetitions,{},count", timing.repetitions[4]);
    std::println("target_mismatch_repetitions,{},count", timing.repetitions[5]);
    std::println("update_cycle_repetitions,{},count", timing.repetitions[6]);
    std::println("exact_duplicate_cycle_repetitions,{},count", timing.repetitions[7]);
    std::println("provenance_enrichment_cycle_repetitions,{},count", timing.repetitions[8]);
    std::println("admission_revocation_cycle_repetitions,{},count", timing.repetitions[9]);
    std::println("archive_open_trusted_repetitions,{},count", timing.repetitions[10]);
    std::println("archive_materialize_repetitions,{},count", timing.repetitions[11]);
    std::println("checksum,{},value", fixture.Checksum());
}

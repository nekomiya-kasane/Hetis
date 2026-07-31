/**
 * @file PortablePackageArchiveTest.cpp
 * @brief Canonical wire, trust-boundary, budget, and semantic-load tests for portable package archives.
 * @ingroup Testing
 */

#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>
#include <Sora/Compute/Runtime/PortablePackageArchive.h>
#include <Sora/Core/Hash.h>
#include <Sora/Core/Wire.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
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

    consteval auto MakeWidePortableProgram() {
        constexpr size_t inputCount = 32;
        constexpr Sora::Compute::ValueType type{Sora::Compute::ScalarKind::UInt64, 1};
        Sora::Compute::KernelBuilder<inputCount, inputCount> scalar;
        std::array<Sora::Compute::ValueId, inputCount> values{};
        for (Sora::Compute::ValueId& value : values) {
            value = scalar.AddValueParameter(type);
        }
        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<inputCount + 1, inputCount, inputCount> builder{
            domain, std::move(scalar).Finish(values.front()),
            Sora::Compute::PartitionContract::Elementwise(domain.rank)};
        for (size_t index = 0; index < inputCount; ++index) {
            const auto argument = builder.AddValueArgument(type);
            builder.BindArgument(argument);
        }
        const auto output = builder.AddBufferArgument(Sora::Compute::AccessMode::Write, type, domain.rank);
        return std::move(builder).Finish(output);
    }

    constexpr auto kWidePortableProgram = MakeWidePortableProgram();
    static_assert(kWidePortableProgram.argumentCount == 33);
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
        .features = Sora::Compute::DeviceFeature::FusedMultiplyAdd,
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
    constexpr auto kLowering = Sora::Compute::MakeArtifactKey(
        Sora::Compute::LoweringKey{.module = kProgram.Hash(), .lowerer = {0x100, 0x101}});
    constexpr auto kExecutable = Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
        .lowered = Sora::Compute::ArtifactIdentity(kLowering),
        .target = kTarget,
        .specialization = kSpecialization,
    });
    constexpr auto kPipeline = Sora::Compute::MakeArtifactKey(Sora::Compute::PipelineKey{
        .executable = Sora::Compute::ArtifactIdentity(kExecutable),
        .target = kTarget,
        .builder = {0x200, 0x201},
        .state = {0x300, 0x301},
        .bindingLayout = {0x400, 0x401},
    });

    Sora::Compute::ProgramPackage Program() {
        auto program = Sora::Compute::ProgramPackage::Create(kProgram);
        REQUIRE(program.has_value());
        return *program;
    }

    Sora::Compute::PackagedArtifact Seed(Sora::Compute::ArtifactKey key, std::byte value) {
        return {.key = key, .payload = {value, std::byte{0x5A}}};
    }

    Sora::Compute::PortablePackage Package(bool withArtifacts = true) {
        std::vector<Sora::Compute::PackagedArtifact> artifacts;
        if (withArtifacts) {
            artifacts = {Seed(kPipeline, std::byte{3}), Seed(kExecutable, std::byte{2}), Seed(kLowering, std::byte{1})};
        }
        auto package = Sora::Compute::PortablePackage::Create(Program(), std::move(artifacts));
        REQUIRE(package.has_value());
        return std::move(*package);
    }

    using Header = Sora::Compute::PortablePackageWire::Header;
    using Section = Sora::Compute::PortablePackageWire::Section;

    Header ReadHeader(std::span<const std::byte> bytes) {
        size_t offset = 0;
        auto header = Sora::Wire::Read<Header>(bytes, offset);
        REQUIRE(header.has_value());
        return *header;
    }

    std::vector<Section> ReadSections(std::span<const std::byte> bytes, const Header& header) {
        size_t offset = static_cast<size_t>(header.sectionTableOffset);
        std::vector<Section> sections;
        sections.reserve(header.sectionCount);
        for (uint32_t index = 0; index < header.sectionCount; ++index) {
            auto section = Sora::Wire::Read<Section>(bytes, offset);
            REQUIRE(section.has_value());
            sections.push_back(*section);
        }
        return sections;
    }

    void WritePreamble(std::vector<std::byte>& bytes, Header header, std::span<const Section> sections) {
        header.preambleChecksum = 0;
        std::array<std::byte, Sora::Compute::PortablePackageWire::kHeaderSize +
                                  Sora::Compute::PortablePackageWire::kMaximumSections *
                                      Sora::Compute::PortablePackageWire::kSectionSize>
            preamble{};
        Sora::Wire::WriteUnchecked(preamble, 0, header);
        size_t preambleSize = Sora::Compute::PortablePackageWire::kHeaderSize;
        for (const Section& section : sections) {
            Sora::Wire::WriteUnchecked(preamble, preambleSize, section);
            preambleSize += Sora::Compute::PortablePackageWire::kSectionSize;
        }
        header.preambleChecksum =
            Sora::Hashing::HashByteRange(std::span<const std::byte>{preamble}.first(preambleSize));
        REQUIRE(Sora::Wire::WriteAt(bytes, 0, header).has_value());
        for (size_t index = 0; index < sections.size(); ++index) {
            REQUIRE(Sora::Wire::WriteAt(bytes,
                                        Sora::Compute::PortablePackageWire::kHeaderSize +
                                            index * Sora::Compute::PortablePackageWire::kSectionSize,
                                        sections[index])
                        .has_value());
        }
    }

    struct TestVerifier {
        int* calls = nullptr;
        bool accept = true;
        bool validGrant = true;
        bool matchingScheme = true;

        std::expected<Sora::Compute::PortablePackageTrustGrant, Sora::Compute::PortablePackageTrustError>
        Verify(const Sora::Compute::PortablePackageVerificationRequest& request) const noexcept {
            ++*calls;
            if (request.context != Sora::Compute::kPortablePackageSignatureContext || request.archive.empty() ||
                !request.signature.Valid()) {
                return std::unexpected(Sora::Compute::PortablePackageTrustError::ProviderFailure);
            }
            if (!accept) {
                return std::unexpected(Sora::Compute::PortablePackageTrustError::InvalidSignature);
            }
            return Sora::Compute::PortablePackageTrustGrant{
                .scheme = request.signature.scheme + !matchingScheme,
                .authority = validGrant ? Sora::Compute::SemanticHash{0xA1, 0xA2} : Sora::Compute::SemanticHash{},
                .subject = {0xB1, 0xB2},
                .policyRevision = 7,
            };
        }
    };

    static_assert(Sora::Compute::PortablePackageSignatureVerifier<TestVerifier>);

} // namespace

TEST_CASE("PortablePackageArchive round-trips canonical program, artifacts, and diagnostics",
          "[Sora][Compute][PortablePackage][Archive]") {
    constexpr std::array sourceMap{std::byte{0x10}, std::byte{0x11}, std::byte{0x12}};
    constexpr std::array diagnostics{std::byte{0x20}, std::byte{0x21}};
    const auto package = Package();
    const Sora::Compute::PortablePackageArchiveMetadata metadata{
        .sourceMap = sourceMap,
        .diagnostics = diagnostics,
    };
    auto archive = Sora::Compute::PortablePackageArchive::Create(package, metadata);
    REQUIRE(archive.has_value());

    auto view = Sora::Compute::PortablePackageArchiveView::OpenTrusted(archive->Bytes());
    REQUIRE(view.has_value());
    REQUIRE_FALSE(view->VerifiedProvenance().has_value());
    REQUIRE(view->ProgramIdentity() == kProgram.Hash());
    REQUIRE(view->MinimumRuntime() == Sora::Compute::kPortablePackageRuntimeVersion);
    REQUIRE(std::ranges::equal(view->SourceMap(), sourceMap));
    REQUIRE(std::ranges::equal(view->Diagnostics(), diagnostics));
    REQUIRE(view->Artifacts().size() == 3);
    REQUIRE(view->Artifacts()[0].key == kLowering);
    REQUIRE(view->Artifacts()[1].key == kExecutable);
    REQUIRE(view->Artifacts()[2].key == kPipeline);

    auto materialized = view->Materialize();
    REQUIRE(materialized.has_value());
    REQUIRE(materialized->Program().Hash() == package.Program().Hash());
    REQUIRE(materialized->Artifacts().Entries().size() == package.Artifacts().Entries().size());
    for (size_t index = 0; index < package.Artifacts().Entries().size(); ++index) {
        REQUIRE(materialized->Artifacts().Entries()[index].key == package.Artifacts().Entries()[index].key);
        REQUIRE(materialized->Artifacts().Entries()[index].payload == package.Artifacts().Entries()[index].payload);
    }

    auto repeated = Sora::Compute::PortablePackageArchive::Create(package, metadata);
    REQUIRE(repeated.has_value());
    REQUIRE(std::ranges::equal(archive->Bytes(), repeated->Bytes()));
}

TEST_CASE("PortablePackageArchive keeps trusted and verified loading explicit",
          "[Sora][Compute][PortablePackage][Archive][Trust]") {
    const auto package = Package(false);
    auto archive = Sora::Compute::PortablePackageArchive::Create(package);
    REQUIRE(archive.has_value());
    constexpr std::array keyIdentity{std::byte{0x11}};
    constexpr std::array signatureBytes{std::byte{0x22}, std::byte{0x33}};
    const Sora::Compute::PortablePackageDetachedSignature signature{
        .scheme = 1,
        .keyIdentity = keyIdentity,
        .signature = signatureBytes,
    };

    int calls = 0;
    auto verified = Sora::Compute::PortablePackageArchiveView::OpenVerified(archive->Bytes(), signature,
                                                                            TestVerifier{.calls = &calls});
    REQUIRE(verified.has_value());
    REQUIRE(calls == 1);
    REQUIRE(verified->VerifiedProvenance() == Sora::Compute::PortablePackageProvenance::Verified({
                                                  .scheme = signature.scheme,
                                                  .authority = {0xA1, 0xA2},
                                                  .subject = {0xB1, 0xB2},
                                                  .policyRevision = 7,
                                              }));

    auto rejected = Sora::Compute::PortablePackageArchiveView::OpenVerified(
        archive->Bytes(), signature, TestVerifier{.calls = &calls, .accept = false});
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error().code == Sora::Compute::PortablePackageArchiveErrorCode::SignatureRejected);
    REQUIRE(rejected.error().detail ==
            static_cast<uint64_t>(Sora::Compute::PortablePackageTrustError::InvalidSignature));
    REQUIRE(calls == 2);

    auto invalidGrant = Sora::Compute::PortablePackageArchiveView::OpenVerified(
        archive->Bytes(), signature, TestVerifier{.calls = &calls, .validGrant = false});
    REQUIRE_FALSE(invalidGrant.has_value());
    REQUIRE(invalidGrant.error().code == Sora::Compute::PortablePackageArchiveErrorCode::SignatureRejected);
    REQUIRE(invalidGrant.error().detail ==
            static_cast<uint64_t>(Sora::Compute::PortablePackageTrustError::ProviderFailure));
    REQUIRE(calls == 3);

    auto mismatchedScheme = Sora::Compute::PortablePackageArchiveView::OpenVerified(
        archive->Bytes(), signature, TestVerifier{.calls = &calls, .matchingScheme = false});
    REQUIRE_FALSE(mismatchedScheme.has_value());
    REQUIRE(mismatchedScheme.error().code == Sora::Compute::PortablePackageArchiveErrorCode::SignatureRejected);
    REQUIRE(mismatchedScheme.error().detail ==
            static_cast<uint64_t>(Sora::Compute::PortablePackageTrustError::ProviderFailure));
    REQUIRE(calls == 4);

    auto malformed =
        Sora::Compute::PortablePackageArchiveView::OpenVerified(archive->Bytes(), {}, TestVerifier{.calls = &calls});
    REQUIRE_FALSE(malformed.has_value());
    REQUIRE(malformed.error().code == Sora::Compute::PortablePackageArchiveErrorCode::SignatureRejected);
    REQUIRE(calls == 4);

    auto limits = Sora::Compute::PortablePackageArchiveLimits{};
    limits.maximumArchiveBytes = archive->Bytes().size() - 1;
    limits.program.maximumBytes = limits.maximumArchiveBytes;
    limits.maximumArtifactBytes = limits.maximumArchiveBytes;
    limits.maximumMetadataBytes = limits.maximumArchiveBytes;
    auto oversized = Sora::Compute::PortablePackageArchiveView::OpenVerified(archive->Bytes(), signature,
                                                                             TestVerifier{.calls = &calls}, limits);
    REQUIRE_FALSE(oversized.has_value());
    REQUIRE(oversized.error().code == Sora::Compute::PortablePackageArchiveErrorCode::ArchiveTooLarge);
    REQUIRE(calls == 4);
}

TEST_CASE("PortablePackageArchive rejects corruption, incompatible versions, and malformed sections",
          "[Sora][Compute][PortablePackage][Archive][Validation]") {
    auto archive = Sora::Compute::PortablePackageArchive::Create(Package());
    REQUIRE(archive.has_value());

    SECTION("corrupted program section") {
        std::vector<std::byte> bytes(archive->Bytes().begin(), archive->Bytes().end());
        const Header header = ReadHeader(bytes);
        const auto sections = ReadSections(bytes, header);
        bytes[static_cast<size_t>(sections[0].offset)] ^= std::byte{0x01};
        auto opened = Sora::Compute::PortablePackageArchiveView::OpenTrusted(bytes);
        REQUIRE_FALSE(opened.has_value());
        REQUIRE(opened.error().code == Sora::Compute::PortablePackageArchiveErrorCode::ChecksumMismatch);
    }

    SECTION("future format minor") {
        std::vector<std::byte> bytes(archive->Bytes().begin(), archive->Bytes().end());
        constexpr size_t minorOffset = Sora::Wire::OffsetOf<Header, ^^Header::minor>();
        size_t offset = minorOffset;
        Sora::Wire::WriteLittleEndianUnchecked(std::span<std::byte>{bytes}, offset, uint16_t{1});
        auto opened = Sora::Compute::PortablePackageArchiveView::OpenTrusted(bytes);
        REQUIRE_FALSE(opened.has_value());
        REQUIRE(opened.error().code == Sora::Compute::PortablePackageArchiveErrorCode::UnsupportedFormat);
    }

    SECTION("overlapping canonical sections") {
        std::vector<std::byte> bytes(archive->Bytes().begin(), archive->Bytes().end());
        Header header = ReadHeader(bytes);
        auto sections = ReadSections(bytes, header);
        sections[1].offset = sections[0].offset;
        WritePreamble(bytes, header, sections);
        auto opened = Sora::Compute::PortablePackageArchiveView::OpenTrusted(bytes);
        REQUIRE_FALSE(opened.has_value());
        REQUIRE(opened.error().code == Sora::Compute::PortablePackageArchiveErrorCode::InvalidSectionTable);
    }

    SECTION("future minimum runtime") {
        auto future =
            Sora::Compute::PortablePackageArchive::Create(Package(), {.minimumRuntime = {.major = 1, .minor = 1}});
        REQUIRE(future.has_value());
        auto opened = Sora::Compute::PortablePackageArchiveView::OpenTrusted(future->Bytes());
        REQUIRE_FALSE(opened.has_value());
        REQUIRE(opened.error().code == Sora::Compute::PortablePackageArchiveErrorCode::RuntimeTooOld);
    }
}

TEST_CASE("PortablePackageArchive separates structural decoding from semantic DAG validation",
          "[Sora][Compute][PortablePackage][Archive][Semantics]") {
    auto archive = Sora::Compute::PortablePackageArchive::Create(Package());
    REQUIRE(archive.has_value());
    std::vector<std::byte> bytes(archive->Bytes().begin(), archive->Bytes().end());
    Header header = ReadHeader(bytes);
    auto sections = ReadSections(bytes, header);

    const size_t firstWordOffset =
        static_cast<size_t>(sections[1].offset) + Sora::Compute::PortablePackageWire::kArtifactSize;
    size_t writeOffset = firstWordOffset;
    Sora::Wire::WriteLittleEndianUnchecked(std::span<std::byte>{bytes}, writeOffset, uint64_t{0xBAD0BAD0});
    const auto indexBytes = std::span<const std::byte>{bytes}.subspan(static_cast<size_t>(sections[1].offset),
                                                                      static_cast<size_t>(sections[1].size));
    sections[1].checksum = Sora::Hashing::HashByteRange(indexBytes);
    WritePreamble(bytes, header, sections);

    auto opened = Sora::Compute::PortablePackageArchiveView::OpenTrusted(bytes);
    REQUIRE(opened.has_value());
    auto materialized = opened->Materialize();
    REQUIRE_FALSE(materialized.has_value());
    REQUIRE(materialized.error().code == Sora::Compute::PortablePackageArchiveErrorCode::InvalidPackage);
}

TEST_CASE("PortablePackageArchive enforces byte and artifact budgets before materialization",
          "[Sora][Compute][PortablePackage][Archive][Budget]") {
    auto archive = Sora::Compute::PortablePackageArchive::Create(Package());
    REQUIRE(archive.has_value());

    SECTION("artifact count") {
        auto limits = Sora::Compute::PortablePackageArchiveLimits{};
        limits.maximumArtifacts = 2;
        auto opened = Sora::Compute::PortablePackageArchiveView::OpenTrusted(archive->Bytes(), limits);
        REQUIRE_FALSE(opened.has_value());
        REQUIRE(opened.error().code == Sora::Compute::PortablePackageArchiveErrorCode::ArtifactCapacityExceeded);
    }

    SECTION("program-only archives permit a zero artifact budget") {
        auto limits = Sora::Compute::PortablePackageArchiveLimits{};
        limits.maximumArtifacts = 0;
        limits.maximumArtifactBytes = 0;
        auto programOnly = Sora::Compute::PortablePackageArchive::Create(Package(false), {}, limits);
        REQUIRE(programOnly.has_value());
        auto opened = Sora::Compute::PortablePackageArchiveView::OpenTrusted(programOnly->Bytes(), limits);
        REQUIRE(opened.has_value());
        REQUIRE(opened->Artifacts().empty());
        REQUIRE(opened->Materialize().has_value());
    }

    SECTION("metadata bytes") {
        constexpr std::array sourceMap{std::byte{1}, std::byte{2}};
        auto limits = Sora::Compute::PortablePackageArchiveLimits{};
        limits.maximumMetadataBytes = 1;
        auto encoded = Sora::Compute::PortablePackageArchive::Create(Package(false), {.sourceMap = sourceMap}, limits);
        REQUIRE_FALSE(encoded.has_value());
        REQUIRE(encoded.error().code == Sora::Compute::PortablePackageArchiveErrorCode::MetadataBytesExceeded);
    }
}

TEST_CASE("PortablePackageArchive validation budgets are independent of backend decoder capacities",
          "[Sora][Compute][PortablePackage][Archive][CapacityBoundary]") {
    auto canonical = Sora::Compute::ProgramPackage::Create(kWidePortableProgram);
    REQUIRE(canonical.has_value());

    auto loaded = Sora::Compute::ProgramPackage::Load(canonical->Bytes());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->Hash() == kWidePortableProgram.Hash());

    auto restrictedLimits = Sora::Compute::KernelProgramLimits{};
    restrictedLimits.maximumArguments = 32;
    const auto restricted = Sora::Compute::ProgramPackage::Load(canonical->Bytes(), restrictedLimits);
    REQUIRE_FALSE(restricted.has_value());
    REQUIRE(restricted.error().code == Sora::Compute::ProgramPackageErrorCode::CapacityExceeded);
    REQUIRE(restricted.error().detail ==
            static_cast<uint64_t>(Sora::Compute::KernelProgramPackageErrorCode::CapacityExceeded));

    auto package = Sora::Compute::PortablePackage::Create(std::move(*loaded), {});
    REQUIRE(package.has_value());
    auto archiveLimits = Sora::Compute::PortablePackageArchiveLimits{};
    archiveLimits.program.maximumArguments = 32;
    const auto restrictedArchive = Sora::Compute::PortablePackageArchive::Create(*package, {}, archiveLimits);
    REQUIRE_FALSE(restrictedArchive.has_value());
    REQUIRE(restrictedArchive.error().code == Sora::Compute::PortablePackageArchiveErrorCode::ProgramCapacityExceeded);

    auto archive = Sora::Compute::PortablePackageArchive::Create(*package);
    REQUIRE(archive.has_value());
    auto view = Sora::Compute::PortablePackageArchiveView::OpenTrusted(archive->Bytes());
    REQUIRE(view.has_value());
    auto materialized = view->Materialize();
    REQUIRE(materialized.has_value());
    REQUIRE(materialized->Program().Hash() == kWidePortableProgram.Hash());
}

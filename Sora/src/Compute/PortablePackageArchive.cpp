/**
 * @file PortablePackageArchive.cpp
 * @brief Canonical portable-package archive encoding and validation.
 * @ingroup Compute
 */

#include <Sora/Compute/Runtime/PortablePackageArchive.h>

#include <Sora/Core/Hash.h>
#include <Sora/Core/Memory/MemoryLayout.h>
#include <Sora/Core/Wire.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace Sora::Compute {

    namespace {

        using ArchiveHeaderWire = PortablePackageWire::Header;
        using ArchiveSectionWire = PortablePackageWire::Section;
        using ArchiveArtifactWire = PortablePackageWire::Artifact;
        using ArchiveSectionKind = PortablePackageWire::SectionKind;

        inline constexpr auto kPortablePackageArchiveMagic = PortablePackageWire::kMagic;
        inline constexpr auto kPortablePackageArchiveMajorVersion = PortablePackageWire::kMajorVersion;
        inline constexpr auto kPortablePackageArchiveMinorVersion = PortablePackageWire::kMinorVersion;
        inline constexpr auto kPortablePackageArchiveFlags = PortablePackageWire::kArchiveFlags;
        inline constexpr auto kPortablePackageSectionFlags = PortablePackageWire::kSectionFlags;
        inline constexpr auto kPortablePackageSectionAlignment = PortablePackageWire::kSectionAlignment;
        inline constexpr auto kMaximumArtifactKeyWords = PortablePackageWire::kMaximumArtifactKeyWords;
        inline constexpr auto kMaximumSections = PortablePackageWire::kMaximumSections;

        struct ArchiveLayout {
            std::array<ArchiveSectionWire, kMaximumSections> sections{};
            size_t sectionCount = 0;
            uint64_t fileSize = 0;
        };

        [[nodiscard]] constexpr std::optional<uint64_t> CheckedAdd(uint64_t left, uint64_t right) noexcept {
            if (right > std::numeric_limits<uint64_t>::max() - left) {
                return std::nullopt;
            }
            return left + right;
        }

        [[nodiscard]] constexpr std::optional<uint64_t> CheckedMul(uint64_t left, uint64_t right) noexcept {
            if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left) {
                return std::nullopt;
            }
            return left * right;
        }

        [[nodiscard]] constexpr std::optional<uint64_t> Aligned(uint64_t value) noexcept {
            return Sora::TryAlignUp(value, kPortablePackageSectionAlignment);
        }

        [[nodiscard]] constexpr bool RuntimeSupported(PortablePackageRuntimeVersion minimum) noexcept {
            return minimum.Valid() && (minimum.major < kPortablePackageRuntimeVersion.major ||
                                       (minimum.major == kPortablePackageRuntimeVersion.major &&
                                        minimum.minor <= kPortablePackageRuntimeVersion.minor));
        }

        [[nodiscard]] bool AllZero(std::span<const std::byte> bytes) noexcept {
            return std::ranges::all_of(bytes, [](std::byte value) noexcept { return value == std::byte{0}; });
        }

        [[nodiscard]] uint64_t Checksum(std::span<const std::byte> bytes) noexcept {
            return Sora::Hashing::HashByteRange(bytes);
        }

        [[nodiscard]] uint64_t PreambleChecksum(ArchiveHeaderWire header,
                                                std::span<const ArchiveSectionWire> sections) noexcept {
            header.preambleChecksum = 0;
            std::array<std::byte,
                       Wire::SizeOf<ArchiveHeaderWire>() + kMaximumSections * Wire::SizeOf<ArchiveSectionWire>()>
                bytes{};
            Wire::WriteUnchecked(bytes, 0, header);
            size_t offset = Wire::SizeOf<ArchiveHeaderWire>();
            for (const ArchiveSectionWire& section : sections) {
                Wire::WriteUnchecked(bytes, offset, section);
                offset += Wire::SizeOf<ArchiveSectionWire>();
            }
            return Checksum(std::span<const std::byte>{bytes}.first(offset));
        }

        [[nodiscard]] std::optional<std::span<const std::byte>>
        SectionBytes(std::span<const std::byte> bytes, const ArchiveSectionWire& section) noexcept {
            if (!Wire::HasRange(bytes, section.offset, section.size) ||
                section.offset > std::numeric_limits<size_t>::max() ||
                section.size > std::numeric_limits<size_t>::max()) {
                return std::nullopt;
            }
            return bytes.subspan(static_cast<size_t>(section.offset), static_cast<size_t>(section.size));
        }

        template<typename E>
            requires std::is_enum_v<E>
        [[nodiscard]] constexpr std::optional<E> DecodeEnum(uint64_t value) noexcept {
            using Raw = std::underlying_type_t<E>;
            if (!std::in_range<Raw>(value)) {
                return std::nullopt;
            }
            return static_cast<E>(static_cast<Raw>(value));
        }

        [[nodiscard]] std::optional<ExecutableTargetIdentity> DecodeExecutableTarget(std::span<const uint64_t> words,
                                                                                     size_t start) noexcept {
            constexpr size_t kTargetWords = 18;
            if (start > words.size() || kTargetWords > words.size() - start) {
                return std::nullopt;
            }
            auto backend = DecodeEnum<BackendKind>(words[start]);
            auto domain = DecodeEnum<ExecutableTargetDomain>(words[start + 1]);
            auto stability = DecodeEnum<TargetIdentityStability>(words[start + 11]);
            auto host = DecodeEnum<CpuArchitecture>(words[start + 12]);
            auto operatingSystem = DecodeEnum<OperatingSystem>(words[start + 13]);
            auto binaryFormat = DecodeEnum<BinaryFormat>(words[start + 14]);
            auto byteOrder = DecodeEnum<ByteOrder>(words[start + 15]);
            auto abi = DecodeEnum<AbiFamily>(words[start + 16]);
            if (!backend || !domain || !host || !operatingSystem || !binaryFormat || !byteOrder || !abi || !stability ||
                !std::in_range<uint32_t>(words[start + 4]) || !std::in_range<uint32_t>(words[start + 5]) ||
                !std::in_range<uint32_t>(words[start + 10]) || !std::in_range<uint8_t>(words[start + 17])) {
                return std::nullopt;
            }
            ExecutableTargetIdentity target;
            target.backend = *backend;
            target.executableDomain = *domain;
            target.identityStability = *stability;
            target.hostArchitecture = *host;
            target.operatingSystem = *operatingSystem;
            target.binaryFormat = *binaryFormat;
            target.byteOrder = *byteOrder;
            target.abi = *abi;
            target.addressBits = static_cast<uint8_t>(words[start + 17]);
            target.features = DeviceFeatureMask{words[start + 3]};
            target.architectureId = words[start + 2];
            target.fixedVectorBits = static_cast<uint32_t>(words[start + 4]);
            target.scalableVectorBits = static_cast<uint32_t>(words[start + 5]);
            target.backendAbiVersion = static_cast<uint32_t>(words[start + 10]);
            target.runtimeIdentity = {words[start + 8], words[start + 9]};
            target.compilerIdentity = {words[start + 6], words[start + 7]};
            return target.Valid() ? std::optional{target} : std::nullopt;
        }

        [[nodiscard]] std::optional<PipelineTargetIdentity> DecodePipelineTarget(std::span<const uint64_t> words,
                                                                                 size_t start) noexcept {
            constexpr size_t kTargetWords = 10;
            if (start > words.size() || kTargetWords > words.size() - start) {
                return std::nullopt;
            }
            auto backend = DecodeEnum<BackendKind>(words[start + 2]);
            auto stability = DecodeEnum<TargetIdentityStability>(words[start + 3]);
            if (!backend || !stability || !std::in_range<uint32_t>(words[start + 5])) {
                return std::nullopt;
            }
            PipelineTargetIdentity target;
            target.device = {words[start], words[start + 1]};
            target.backend = *backend;
            target.identityStability = *stability;
            target.architectureId = words[start + 4];
            target.backendAbiVersion = static_cast<uint32_t>(words[start + 5]);
            target.runtimeIdentity = {words[start + 6], words[start + 7]};
            target.driverIdentity = {words[start + 8], words[start + 9]};
            return target.Valid() ? std::optional{target} : std::nullopt;
        }

        [[nodiscard]] std::optional<ArtifactKey> DecodeArtifactKey(ArtifactKind kind,
                                                                   std::span<const uint64_t> words) noexcept {
            ArtifactKey key;
            switch (kind) {
                case ArtifactKind::PortableLowering:
                    if (words.size() != 7) {
                        return std::nullopt;
                    }
                    key = MakeArtifactKey(LoweringKey{
                        .module = {words[0], words[1]},
                        .lowerer = {words[2], words[3]},
                        .options = {words[4], words[5]},
                        .flags = words[6],
                    });
                    break;
                case ArtifactKind::DeviceExecutable: {
                    if (words.size() != 26) {
                        return std::nullopt;
                    }
                    auto target = DecodeExecutableTarget(words, 2);
                    if (!target) {
                        return std::nullopt;
                    }
                    key = MakeArtifactKey(ExecutableKey{
                        .lowered = {words[0], words[1]},
                        .target = *target,
                        .specialization =
                            {
                                .shape = {words[20], words[21]},
                                .layout = {words[22], words[23]},
                                .tuning = {words[24], words[25]},
                            },
                    });
                    break;
                }
                case ArtifactKind::DevicePipeline: {
                    if (words.size() != 18) {
                        return std::nullopt;
                    }
                    auto target = DecodePipelineTarget(words, 2);
                    if (!target) {
                        return std::nullopt;
                    }
                    key = MakeArtifactKey(PipelineKey{
                        .executable = {words[0], words[1]},
                        .target = *target,
                        .builder = {words[12], words[13]},
                        .state = {words[14], words[15]},
                        .bindingLayout = {words[16], words[17]},
                    });
                    break;
                }
            }
            if (!key.Valid() || !std::ranges::equal(key.Words(), words)) {
                return std::nullopt;
            }
            return key;
        }

        [[nodiscard]] PortablePackageArchiveError Failure(PortablePackageArchiveErrorCode code, uint64_t offset = 0,
                                                          uint64_t detail = 0, SemanticHash program = {},
                                                          SemanticHash artifact = {}) noexcept {
            return {.code = code, .offset = offset, .detail = detail, .program = program, .artifact = artifact};
        }

        [[nodiscard]] std::optional<ArchiveLayout> MakeLayout(uint64_t programSize, uint64_t indexSize,
                                                              uint64_t dataSize,
                                                              const PortablePackageArchiveMetadata& metadata) noexcept {
            ArchiveLayout layout;
            const auto append = [&layout](ArchiveSectionKind kind, uint64_t size) -> bool {
                auto offset = Aligned(layout.fileSize);
                if (!offset) {
                    return false;
                }
                auto end = CheckedAdd(*offset, size);
                if (!end) {
                    return false;
                }
                layout.sections[layout.sectionCount++] = ArchiveSectionWire{
                    .kind = kind,
                    .flags = kPortablePackageSectionFlags,
                    .alignment = kPortablePackageSectionAlignment,
                    .offset = *offset,
                    .size = size,
                };
                layout.fileSize = *end;
                return true;
            };

            const size_t sectionCount = 3 + !metadata.sourceMap.empty() + !metadata.diagnostics.empty();
            auto tableBytes = CheckedMul(sectionCount, Wire::SizeOf<ArchiveSectionWire>());
            auto preambleBytes = tableBytes ? CheckedAdd(Wire::SizeOf<ArchiveHeaderWire>(), *tableBytes) : std::nullopt;
            if (!preambleBytes) {
                return std::nullopt;
            }
            layout.fileSize = *preambleBytes;
            if (!append(ArchiveSectionKind::Program, programSize) ||
                !append(ArchiveSectionKind::ArtifactIndex, indexSize) ||
                !append(ArchiveSectionKind::ArtifactData, dataSize)) {
                return std::nullopt;
            }
            if (!metadata.sourceMap.empty() && !append(ArchiveSectionKind::SourceMap, metadata.sourceMap.size())) {
                return std::nullopt;
            }
            if (!metadata.diagnostics.empty() &&
                !append(ArchiveSectionKind::Diagnostics, metadata.diagnostics.size())) {
                return std::nullopt;
            }
            return layout;
        }

    } // namespace

    std::expected<PortablePackageArchive, PortablePackageArchiveError>
    PortablePackageArchive::Create(const PortablePackage& package, PortablePackageArchiveMetadata metadata,
                                   PortablePackageArchiveLimits limits) noexcept {
        const SemanticHash programIdentity = package.Program().Hash();
        const auto artifacts = package.Artifacts().Entries();
        if (!limits.Valid() || !metadata.minimumRuntime.Valid()) {
            return std::unexpected(Failure(PortablePackageArchiveErrorCode::InvalidConfiguration));
        }
        if (!package.Program() || !ValidSemanticHash(programIdentity) ||
            package.Artifacts().Program() != programIdentity) {
            return std::unexpected(Failure(PortablePackageArchiveErrorCode::InvalidPackage, 0, 0, programIdentity));
        }
        auto validatedProgram = ProgramPackage::Validate(package.Program().Bytes(), limits.program);
        if (!validatedProgram) {
            PortablePackageArchiveErrorCode code = PortablePackageArchiveErrorCode::InvalidPackage;
            if (validatedProgram.error().code == ProgramPackageErrorCode::CapacityExceeded) {
                code = package.Program().Bytes().size() > limits.program.maximumBytes
                           ? PortablePackageArchiveErrorCode::ProgramTooLarge
                           : PortablePackageArchiveErrorCode::ProgramCapacityExceeded;
            } else if (validatedProgram.error().code == ProgramPackageErrorCode::OutOfMemory) {
                code = PortablePackageArchiveErrorCode::OutOfMemory;
            }
            return std::unexpected(
                Failure(code, validatedProgram.error().offset, validatedProgram.error().detail, programIdentity));
        }
        if (*validatedProgram != programIdentity) {
            return std::unexpected(Failure(PortablePackageArchiveErrorCode::InvalidPackage, 0, 0, programIdentity));
        }
        if (artifacts.size() > limits.maximumArtifacts || !std::in_range<uint32_t>(artifacts.size())) {
            return std::unexpected(Failure(PortablePackageArchiveErrorCode::ArtifactCapacityExceeded, 0,
                                           artifacts.size(), programIdentity));
        }

        try {
            uint64_t indexSize = 0;
            uint64_t dataSize = 0;
            uint64_t artifactBytes = 0;
            std::vector<uint64_t> payloadOffsets;
            payloadOffsets.reserve(artifacts.size());
            for (const PackagedArtifact& artifact : artifacts) {
                if (!artifact.key.Valid() || artifact.key.Words().empty() ||
                    artifact.key.Words().size() > kMaximumArtifactKeyWords || artifact.payload.empty()) {
                    return std::unexpected(Failure(PortablePackageArchiveErrorCode::InvalidPackage, 0, 0,
                                                   programIdentity, ArtifactIdentity(artifact.key)));
                }
                auto wordsSize = CheckedMul(artifact.key.Words().size(), sizeof(uint64_t));
                auto recordSize =
                    wordsSize ? CheckedAdd(Wire::SizeOf<ArchiveArtifactWire>(), *wordsSize) : std::nullopt;
                auto nextIndex = recordSize ? CheckedAdd(indexSize, *recordSize) : std::nullopt;
                auto payloadOffset = Aligned(dataSize);
                auto nextData = payloadOffset ? CheckedAdd(*payloadOffset, artifact.payload.size()) : std::nullopt;
                auto nextArtifactBytes = CheckedAdd(artifactBytes, artifact.payload.size());
                if (!nextIndex || !nextData || !nextArtifactBytes) {
                    return std::unexpected(
                        Failure(PortablePackageArchiveErrorCode::ArchiveTooLarge, 0, 0, programIdentity));
                }
                indexSize = *nextIndex;
                dataSize = *nextData;
                artifactBytes = *nextArtifactBytes;
                payloadOffsets.push_back(*payloadOffset);
            }
            if (artifactBytes > limits.maximumArtifactBytes) {
                return std::unexpected(
                    Failure(PortablePackageArchiveErrorCode::ArtifactBytesExceeded, 0, artifactBytes, programIdentity));
            }
            auto metadataBytes = CheckedAdd(metadata.sourceMap.size(), metadata.diagnostics.size());
            if (!metadataBytes || *metadataBytes > limits.maximumMetadataBytes) {
                return std::unexpected(Failure(PortablePackageArchiveErrorCode::MetadataBytesExceeded, 0,
                                               metadataBytes.value_or(UINT64_MAX), programIdentity));
            }

            auto layout = MakeLayout(package.Program().Bytes().size(), indexSize, dataSize, metadata);
            if (!layout || layout->fileSize > limits.maximumArchiveBytes ||
                layout->fileSize > std::numeric_limits<size_t>::max()) {
                return std::unexpected(Failure(PortablePackageArchiveErrorCode::ArchiveTooLarge, 0,
                                               layout ? layout->fileSize : UINT64_MAX, programIdentity));
            }

            std::vector<std::byte> bytes(static_cast<size_t>(layout->fileSize), std::byte{0});
            auto mutableBytes = std::span<std::byte>{bytes};
            const auto& programSection = layout->sections[0];
            std::memcpy(mutableBytes.data() + programSection.offset, package.Program().Bytes().data(),
                        package.Program().Bytes().size());

            const auto& indexSection = layout->sections[1];
            const auto& dataSection = layout->sections[2];
            size_t indexOffset = static_cast<size_t>(indexSection.offset);
            for (size_t artifactIndex = 0; artifactIndex < artifacts.size(); ++artifactIndex) {
                const PackagedArtifact& artifact = artifacts[artifactIndex];
                const uint64_t payloadOffset = payloadOffsets[artifactIndex];
                const ArchiveArtifactWire record{
                    .kind = artifact.key.Kind(),
                    .wordCount = static_cast<uint8_t>(artifact.key.Words().size()),
                    .payloadOffset = payloadOffset,
                    .payloadSize = artifact.payload.size(),
                };
                Wire::WriteUnchecked(mutableBytes, indexOffset, record);
                indexOffset += Wire::SizeOf<ArchiveArtifactWire>();
                for (uint64_t word : artifact.key.Words()) {
                    Wire::WriteLittleEndianUnchecked(mutableBytes, indexOffset, word);
                }
                std::memcpy(mutableBytes.data() + dataSection.offset + payloadOffset, artifact.payload.data(),
                            artifact.payload.size());
            }

            size_t optionalSection = 3;
            if (!metadata.sourceMap.empty()) {
                const auto& section = layout->sections[optionalSection++];
                std::memcpy(mutableBytes.data() + section.offset, metadata.sourceMap.data(), metadata.sourceMap.size());
            }
            if (!metadata.diagnostics.empty()) {
                const auto& section = layout->sections[optionalSection];
                std::memcpy(mutableBytes.data() + section.offset, metadata.diagnostics.data(),
                            metadata.diagnostics.size());
            }

            for (size_t index = 0; index < layout->sectionCount; ++index) {
                ArchiveSectionWire& section = layout->sections[index];
                section.checksum = Checksum(std::span<const std::byte>{bytes}.subspan(
                    static_cast<size_t>(section.offset), static_cast<size_t>(section.size)));
                Wire::WriteUnchecked(mutableBytes,
                                     Wire::SizeOf<ArchiveHeaderWire>() + index * Wire::SizeOf<ArchiveSectionWire>(),
                                     section);
            }

            ArchiveHeaderWire header{
                .headerSize = static_cast<uint16_t>(Wire::SizeOf<ArchiveHeaderWire>()),
                .sectionSize = static_cast<uint16_t>(Wire::SizeOf<ArchiveSectionWire>()),
                .sectionCount = static_cast<uint32_t>(layout->sectionCount),
                .artifactCount = static_cast<uint32_t>(artifacts.size()),
                .fileSize = layout->fileSize,
                .sectionTableOffset = Wire::SizeOf<ArchiveHeaderWire>(),
                .minimumRuntimeMajor = metadata.minimumRuntime.major,
                .minimumRuntimeMinor = metadata.minimumRuntime.minor,
                .programIdentityLow = programIdentity.low,
                .programIdentityHigh = programIdentity.high,
            };
            header.preambleChecksum = PreambleChecksum(header, std::span{layout->sections}.first(layout->sectionCount));
            Wire::WriteUnchecked(mutableBytes, 0, header);
            return PortablePackageArchive{std::move(bytes)};
        } catch (const std::length_error&) {
            return std::unexpected(Failure(PortablePackageArchiveErrorCode::ArchiveTooLarge, 0, 0, programIdentity));
        } catch (const std::bad_alloc&) {
            return std::unexpected(Failure(PortablePackageArchiveErrorCode::OutOfMemory, 0, 0, programIdentity));
        }
    }

    SemanticHash PortablePackageArchiveView::PeekProgramIdentity(std::span<const std::byte> bytes) noexcept {
        constexpr size_t lowOffset = Wire::OffsetOf<ArchiveHeaderWire, ^^ArchiveHeaderWire::programIdentityLow>();
        constexpr size_t highOffset = Wire::OffsetOf<ArchiveHeaderWire, ^^ArchiveHeaderWire::programIdentityHigh>();
        if (!Wire::HasRange(bytes, lowOffset, sizeof(uint64_t)) ||
            !Wire::HasRange(bytes, highOffset, sizeof(uint64_t))) {
            return {};
        }
        return {Wire::ReadLittleEndianUnchecked<uint64_t>(bytes, lowOffset),
                Wire::ReadLittleEndianUnchecked<uint64_t>(bytes, highOffset)};
    }

    std::expected<PortablePackage, PortablePackageArchiveError>
    PortablePackageArchiveView::Materialize() const noexcept {
        auto program = ProgramPackage::Load(programBytes_, programLimits_);
        if (!program) {
            PortablePackageArchiveErrorCode code = PortablePackageArchiveErrorCode::InvalidPackage;
            if (program.error().code == ProgramPackageErrorCode::CapacityExceeded) {
                code = PortablePackageArchiveErrorCode::ProgramCapacityExceeded;
            } else if (program.error().code == ProgramPackageErrorCode::OutOfMemory) {
                code = PortablePackageArchiveErrorCode::OutOfMemory;
            }
            return std::unexpected(PortablePackageArchiveError{
                .code = code,
                .offset = programOffset_ + program.error().offset,
                .detail = program.error().detail,
                .program = programIdentity_,
            });
        }
        try {
            std::vector<PackagedArtifact> artifacts;
            artifacts.reserve(artifacts_.size());
            for (const PortablePackageArchiveArtifactView& artifact : artifacts_) {
                artifacts.push_back(PackagedArtifact{
                    .key = artifact.key,
                    .payload = std::vector<std::byte>{artifact.payload.begin(), artifact.payload.end()},
                });
            }
            auto package = PortablePackage::Create(std::move(*program), std::move(artifacts));
            if (!package) {
                return std::unexpected(PortablePackageArchiveError{
                    .code = PortablePackageArchiveErrorCode::InvalidPackage,
                    .detail = static_cast<uint64_t>(package.error().code),
                    .program = programIdentity_,
                    .artifact = package.error().identity,
                });
            }
            return std::move(*package);
        } catch (const std::length_error&) {
            return std::unexpected(PortablePackageArchiveError{
                .code = PortablePackageArchiveErrorCode::ArtifactBytesExceeded,
                .program = programIdentity_,
            });
        } catch (const std::bad_alloc&) {
            return std::unexpected(PortablePackageArchiveError{
                .code = PortablePackageArchiveErrorCode::OutOfMemory,
                .program = programIdentity_,
            });
        }
    }

    std::expected<void, PortablePackageArchiveError>
    PortablePackageArchiveView::ValidateExtent(std::span<const std::byte> bytes,
                                               PortablePackageArchiveLimits limits) noexcept {
        if (!limits.Valid()) {
            return std::unexpected(Failure(PortablePackageArchiveErrorCode::InvalidConfiguration));
        }
        if (bytes.size() < Wire::SizeOf<ArchiveHeaderWire>()) {
            return std::unexpected(Failure(PortablePackageArchiveErrorCode::Truncated, bytes.size()));
        }
        if (bytes.size() > limits.maximumArchiveBytes) {
            return std::unexpected(
                Failure(PortablePackageArchiveErrorCode::ArchiveTooLarge, 0, bytes.size(), PeekProgramIdentity(bytes)));
        }
        size_t offset = 0;
        auto header = Wire::Read<ArchiveHeaderWire>(bytes, offset);
        if (!header) {
            return std::unexpected(Failure(PortablePackageArchiveErrorCode::Truncated, offset));
        }
        const SemanticHash program{header->programIdentityLow, header->programIdentityHigh};
        if (header->magic != kPortablePackageArchiveMagic) {
            return std::unexpected(Failure(PortablePackageArchiveErrorCode::InvalidMagic, 0, header->magic, program));
        }
        if (header->major != kPortablePackageArchiveMajorVersion ||
            header->minor > kPortablePackageArchiveMinorVersion) {
            return std::unexpected(
                Failure(PortablePackageArchiveErrorCode::UnsupportedFormat, sizeof(uint64_t), header->major, program));
        }
        const PortablePackageRuntimeVersion minimum{header->minimumRuntimeMajor, header->minimumRuntimeMinor};
        if (!RuntimeSupported(minimum)) {
            return std::unexpected(
                Failure(PortablePackageArchiveErrorCode::RuntimeTooOld,
                        Wire::OffsetOf<ArchiveHeaderWire, ^^ArchiveHeaderWire::minimumRuntimeMajor>(),
                        (uint64_t{minimum.major} << 16U) | minimum.minor, program));
        }
        if (header->artifactCount > limits.maximumArtifacts) {
            return std::unexpected(Failure(PortablePackageArchiveErrorCode::ArtifactCapacityExceeded,
                                           Wire::OffsetOf<ArchiveHeaderWire, ^^ArchiveHeaderWire::artifactCount>(),
                                           header->artifactCount, program));
        }
        if (header->headerSize != Wire::SizeOf<ArchiveHeaderWire>() ||
            header->sectionSize != Wire::SizeOf<ArchiveSectionWire>() ||
            header->flags != kPortablePackageArchiveFlags || header->reserved0 != 0 || header->reserved1 != 0 ||
            header->sectionCount < 3 || header->sectionCount > kMaximumSections || header->fileSize != bytes.size() ||
            header->sectionTableOffset != Wire::SizeOf<ArchiveHeaderWire>() || !ValidSemanticHash(program)) {
            return std::unexpected(Failure(PortablePackageArchiveErrorCode::InvalidHeader, 0, 0, program));
        }
        auto tableSize = CheckedMul(header->sectionCount, header->sectionSize);
        if (!tableSize || !Wire::HasRange(bytes, header->sectionTableOffset, *tableSize)) {
            return std::unexpected(Failure(PortablePackageArchiveErrorCode::InvalidSectionTable,
                                           header->sectionTableOffset, tableSize.value_or(UINT64_MAX), program));
        }
        return {};
    }

    std::expected<PortablePackageArchiveView, PortablePackageArchiveError>
    PortablePackageArchiveView::OpenTrusted(std::span<const std::byte> bytes,
                                            PortablePackageArchiveLimits limits) noexcept {
        if (auto extent = ValidateExtent(bytes, limits); !extent) {
            return std::unexpected(extent.error());
        }
        size_t offset = 0;
        auto headerResult = Wire::Read<ArchiveHeaderWire>(bytes, offset);
        if (!headerResult) {
            return std::unexpected(Failure(PortablePackageArchiveErrorCode::Truncated, offset));
        }
        const ArchiveHeaderWire header = *headerResult;
        const SemanticHash programIdentity{header.programIdentityLow, header.programIdentityHigh};

        try {
            std::array<ArchiveSectionWire, kMaximumSections> sections{};
            offset = static_cast<size_t>(header.sectionTableOffset);
            for (size_t index = 0; index < header.sectionCount; ++index) {
                auto section = Wire::Read<ArchiveSectionWire>(bytes, offset);
                if (!section) {
                    return std::unexpected(
                        Failure(PortablePackageArchiveErrorCode::Truncated, offset, 0, programIdentity));
                }
                sections[index] = *section;
            }
            if (PreambleChecksum(header, std::span{sections}.first(header.sectionCount)) != header.preambleChecksum) {
                return std::unexpected(
                    Failure(PortablePackageArchiveErrorCode::ChecksumMismatch, 0, 0, programIdentity));
            }

            constexpr std::array requiredKinds{ArchiveSectionKind::Program, ArchiveSectionKind::ArtifactIndex,
                                               ArchiveSectionKind::ArtifactData};
            for (size_t index = 0; index < requiredKinds.size(); ++index) {
                if (sections[index].kind != requiredKinds[index]) {
                    return std::unexpected(
                        Failure(PortablePackageArchiveErrorCode::InvalidSectionTable,
                                header.sectionTableOffset + index * Wire::SizeOf<ArchiveSectionWire>(),
                                std::to_underlying(sections[index].kind), programIdentity));
                }
            }
            for (size_t index = 3; index < header.sectionCount; ++index) {
                const ArchiveSectionKind expected = index == 3 && sections[index].kind == ArchiveSectionKind::SourceMap
                                                        ? ArchiveSectionKind::SourceMap
                                                        : ArchiveSectionKind::Diagnostics;
                if (sections[index].kind != expected ||
                    (index == 4 && sections[3].kind != ArchiveSectionKind::SourceMap)) {
                    return std::unexpected(
                        Failure(PortablePackageArchiveErrorCode::InvalidSectionTable,
                                header.sectionTableOffset + index * Wire::SizeOf<ArchiveSectionWire>(),
                                std::to_underlying(sections[index].kind), programIdentity));
                }
            }

            auto tableBytes = CheckedMul(header.sectionCount, header.sectionSize);
            auto cursor = tableBytes ? CheckedAdd(header.sectionTableOffset, *tableBytes) : std::nullopt;
            uint64_t metadataBytes = 0;
            if (!cursor) {
                return std::unexpected(
                    Failure(PortablePackageArchiveErrorCode::InvalidSectionTable, 0, 0, programIdentity));
            }
            for (size_t index = 0; index < header.sectionCount; ++index) {
                const ArchiveSectionWire& section = sections[index];
                auto expectedOffset = Aligned(*cursor);
                auto sectionEnd = expectedOffset ? CheckedAdd(*expectedOffset, section.size) : std::nullopt;
                if (!expectedOffset || !sectionEnd || section.flags != kPortablePackageSectionFlags ||
                    section.alignment != kPortablePackageSectionAlignment || section.reserved != 0 ||
                    section.offset != *expectedOffset || !Wire::HasRange(bytes, section.offset, section.size) ||
                    !AllZero(
                        bytes.subspan(static_cast<size_t>(*cursor), static_cast<size_t>(*expectedOffset - *cursor)))) {
                    return std::unexpected(
                        Failure(PortablePackageArchiveErrorCode::InvalidSectionTable,
                                header.sectionTableOffset + index * Wire::SizeOf<ArchiveSectionWire>(), section.offset,
                                programIdentity));
                }
                const auto sectionBytes = SectionBytes(bytes, section);
                if (!sectionBytes || Checksum(*sectionBytes) != section.checksum) {
                    return std::unexpected(Failure(PortablePackageArchiveErrorCode::ChecksumMismatch, section.offset,
                                                   section.size, programIdentity));
                }
                if (section.kind == ArchiveSectionKind::Program && section.size > limits.program.maximumBytes) {
                    return std::unexpected(Failure(PortablePackageArchiveErrorCode::ProgramTooLarge, section.offset,
                                                   section.size, programIdentity));
                }
                if (section.kind == ArchiveSectionKind::SourceMap || section.kind == ArchiveSectionKind::Diagnostics) {
                    auto total = CheckedAdd(metadataBytes, section.size);
                    if (!total || *total > limits.maximumMetadataBytes) {
                        return std::unexpected(Failure(PortablePackageArchiveErrorCode::MetadataBytesExceeded,
                                                       section.offset, section.size, programIdentity));
                    }
                    metadataBytes = *total;
                }
                cursor = sectionEnd;
            }
            if (*cursor != bytes.size()) {
                return std::unexpected(Failure(PortablePackageArchiveErrorCode::NonCanonicalEncoding, *cursor,
                                               bytes.size() - *cursor, programIdentity));
            }

            PortablePackageArchiveView view;
            view.bytes_ = bytes;
            view.minimumRuntime_ = {header.minimumRuntimeMajor, header.minimumRuntimeMinor};
            view.programIdentity_ = programIdentity;
            view.programOffset_ = sections[0].offset;
            view.programLimits_ = limits.program;
            view.programBytes_ = *SectionBytes(bytes, sections[0]);
            if (Detail::HashBytes(view.programBytes_) != programIdentity) {
                return std::unexpected(Failure(PortablePackageArchiveErrorCode::ChecksumMismatch, sections[0].offset,
                                               sections[0].size, programIdentity));
            }

            const auto indexBytes = *SectionBytes(bytes, sections[1]);
            const auto dataBytes = *SectionBytes(bytes, sections[2]);
            view.artifacts_.reserve(header.artifactCount);
            size_t indexOffset = 0;
            uint64_t dataCursor = 0;
            uint64_t artifactBytes = 0;
            for (uint32_t artifactIndex = 0; artifactIndex < header.artifactCount; ++artifactIndex) {
                const size_t recordOffset = indexOffset;
                auto record = Wire::Read<ArchiveArtifactWire>(indexBytes, indexOffset);
                if (!record) {
                    return std::unexpected(Failure(PortablePackageArchiveErrorCode::Truncated,
                                                   sections[1].offset + recordOffset, artifactIndex, programIdentity));
                }
                if (!ValidArtifactKind(record->kind) || record->wordCount == 0 ||
                    record->wordCount > kMaximumArtifactKeyWords || record->reserved16 != 0 ||
                    record->reserved32 != 0 || record->payloadSize == 0) {
                    return std::unexpected(Failure(PortablePackageArchiveErrorCode::InvalidArtifactKey,
                                                   sections[1].offset + recordOffset, artifactIndex, programIdentity));
                }
                std::array<uint64_t, kMaximumArtifactKeyWords> words{};
                for (size_t word = 0; word < record->wordCount; ++word) {
                    auto value = Wire::ReadLittleEndian<uint64_t>(indexBytes, indexOffset);
                    if (!value) {
                        return std::unexpected(Failure(PortablePackageArchiveErrorCode::Truncated,
                                                       sections[1].offset + indexOffset, artifactIndex,
                                                       programIdentity));
                    }
                    words[word] = *value;
                }
                auto key = DecodeArtifactKey(record->kind, std::span{words}.first(record->wordCount));
                if (!key || !key->PersistentCacheable()) {
                    return std::unexpected(Failure(PortablePackageArchiveErrorCode::InvalidArtifactKey,
                                                   sections[1].offset + recordOffset, artifactIndex, programIdentity));
                }
                if (!view.artifacts_.empty() && !ArtifactKeyLess{}(view.artifacts_.back().key, *key)) {
                    return std::unexpected(Failure(PortablePackageArchiveErrorCode::NonCanonicalEncoding,
                                                   sections[1].offset + recordOffset, artifactIndex, programIdentity,
                                                   ArtifactIdentity(*key)));
                }

                auto expectedPayloadOffset = Aligned(dataCursor);
                auto payloadEnd =
                    expectedPayloadOffset ? CheckedAdd(*expectedPayloadOffset, record->payloadSize) : std::nullopt;
                auto nextArtifactBytes = CheckedAdd(artifactBytes, record->payloadSize);
                if (!expectedPayloadOffset || !payloadEnd || !nextArtifactBytes ||
                    record->payloadOffset != *expectedPayloadOffset ||
                    !Wire::HasRange(dataBytes, record->payloadOffset, record->payloadSize) ||
                    !AllZero(dataBytes.subspan(static_cast<size_t>(dataCursor),
                                               static_cast<size_t>(*expectedPayloadOffset - dataCursor)))) {
                    return std::unexpected(Failure(PortablePackageArchiveErrorCode::NonCanonicalEncoding,
                                                   sections[2].offset + dataCursor, artifactIndex, programIdentity,
                                                   ArtifactIdentity(*key)));
                }
                if (*nextArtifactBytes > limits.maximumArtifactBytes) {
                    return std::unexpected(Failure(PortablePackageArchiveErrorCode::ArtifactBytesExceeded,
                                                   sections[2].offset + record->payloadOffset, *nextArtifactBytes,
                                                   programIdentity, ArtifactIdentity(*key)));
                }
                const auto payload = dataBytes.subspan(static_cast<size_t>(record->payloadOffset),
                                                       static_cast<size_t>(record->payloadSize));
                view.artifacts_.push_back({.key = *key, .payload = payload});
                dataCursor = *payloadEnd;
                artifactBytes = *nextArtifactBytes;
            }
            if (indexOffset != indexBytes.size() || dataCursor != dataBytes.size()) {
                return std::unexpected(Failure(PortablePackageArchiveErrorCode::NonCanonicalEncoding,
                                               indexOffset != indexBytes.size() ? sections[1].offset + indexOffset
                                                                                : sections[2].offset + dataCursor,
                                               0, programIdentity));
            }

            for (size_t index = 3; index < header.sectionCount; ++index) {
                const auto sectionBytes = *SectionBytes(bytes, sections[index]);
                if (sections[index].kind == ArchiveSectionKind::SourceMap) {
                    view.sourceMap_ = sectionBytes;
                } else {
                    view.diagnostics_ = sectionBytes;
                }
            }
            return view;
        } catch (const std::length_error&) {
            return std::unexpected(
                Failure(PortablePackageArchiveErrorCode::ArtifactCapacityExceeded, 0, 0, programIdentity));
        } catch (const std::bad_alloc&) {
            return std::unexpected(Failure(PortablePackageArchiveErrorCode::OutOfMemory, 0, 0, programIdentity));
        }
    }

} // namespace Sora::Compute

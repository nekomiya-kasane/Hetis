/**
 * @file PortablePackageArchiveFormat.h
 * @brief Versioned POD wire schema for canonical portable-package archives.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Artifact.h>
#include <Sora/Core/Wire.h>

#include <cstddef>
#include <cstdint>

namespace Sora::Compute::PortablePackageWire {

    inline constexpr uint64_t kMagic = 0x00474B5041524F53ULL; // "SORAPKG\0" in little-endian byte order.
    inline constexpr uint16_t kMajorVersion = 2;
    inline constexpr uint16_t kMinorVersion = 0;
    inline constexpr uint32_t kArchiveFlags = 0;
    inline constexpr uint32_t kSectionFlags = 0;
    inline constexpr uint32_t kSectionAlignment = 8;
    inline constexpr size_t kMaximumArtifactKeyWords = 32;
    inline constexpr size_t kMaximumSections = 5;

    /** @brief Canonical archive section identity in required serialization order. */
    enum class SectionKind : uint32_t {
        Program = 1,
        ArtifactIndex = 2,
        ArtifactData = 3,
        SourceMap = 4,
        Diagnostics = 5,
    };

    /** @brief Fixed archive preamble; serialized field-by-field without object padding. */
    struct Header {
        uint64_t magic = kMagic;
        uint16_t major = kMajorVersion;
        uint16_t minor = kMinorVersion;
        uint16_t headerSize = 0;
        uint16_t sectionSize = 0;
        uint32_t flags = kArchiveFlags;
        uint32_t sectionCount = 0;
        uint32_t artifactCount = 0;
        uint32_t reserved0 = 0;
        uint64_t fileSize = 0;
        uint64_t sectionTableOffset = 0;
        uint16_t minimumRuntimeMajor = 0;
        uint16_t minimumRuntimeMinor = 0;
        uint32_t reserved1 = 0;
        uint64_t programIdentityLow = 0;
        uint64_t programIdentityHigh = 0;
        uint64_t preambleChecksum = 0;
    };

    /** @brief One aligned, checksummed archive section. */
    struct Section {
        SectionKind kind = SectionKind::Program;
        uint32_t flags = 0;
        uint32_t alignment = 0;
        uint32_t reserved = 0;
        uint64_t offset = 0;
        uint64_t size = 0;
        uint64_t checksum = 0;
    };

    /** @brief Variable-key artifact record prefix stored in the artifact-index section. */
    struct Artifact {
        ArtifactKind kind = ArtifactKind::PortableLowering;
        uint8_t wordCount = 0;
        uint16_t reserved16 = 0;
        uint32_t reserved32 = 0;
        uint64_t payloadOffset = 0;
        uint64_t payloadSize = 0;
    };

    inline constexpr size_t kHeaderSize = Wire::SizeOf<Header>();
    inline constexpr size_t kSectionSize = Wire::SizeOf<Section>();
    inline constexpr size_t kArtifactSize = Wire::SizeOf<Artifact>();

    static_assert(kHeaderSize == 80);
    static_assert(kSectionSize == 40);
    static_assert(kArtifactSize == 24);

} // namespace Sora::Compute::PortablePackageWire

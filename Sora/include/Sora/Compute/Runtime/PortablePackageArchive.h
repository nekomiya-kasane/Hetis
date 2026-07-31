/**
 * @file PortablePackageArchive.h
 * @brief Canonical sectioned distribution archive for portable compute packages.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Runtime/PortablePackage.h>
#include <Sora/Compute/Runtime/PortablePackageArchiveFormat.h>
#include <Sora/Compute/Runtime/PortablePackageTrust.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Compute {

    /** @brief Runtime format level required by one portable package archive. */
    struct PortablePackageRuntimeVersion {
        uint16_t major = 1;
        uint16_t minor = 0;

        /** @brief Return whether this is a representable runtime requirement. */
        [[nodiscard]] constexpr bool Valid() const noexcept { return major != 0; }

        friend constexpr bool operator==(PortablePackageRuntimeVersion,
                                         PortablePackageRuntimeVersion) noexcept = default;
    };

    inline constexpr PortablePackageRuntimeVersion kPortablePackageRuntimeVersion{};

    /** @brief Hard resource limits applied before archive-owned allocation. */
    struct PortablePackageArchiveLimits {
        uint64_t maximumArchiveBytes = uint64_t{4} * 1'024 * 1'024 * 1'024;
        uint64_t maximumArtifactBytes = uint64_t{4} * 1'024 * 1'024 * 1'024;
        uint64_t maximumMetadataBytes = uint64_t{64} * 1'024 * 1'024;
        uint32_t maximumArtifacts = 65'536;
        KernelProgramLimits program{};

        /** @brief Return whether every byte budget fits the archive and host address space. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return maximumArchiveBytes != 0 && maximumArchiveBytes <= std::numeric_limits<size_t>::max() &&
                   maximumArtifactBytes <= maximumArchiveBytes && maximumMetadataBytes <= maximumArchiveBytes &&
                   program.Valid() && program.maximumBytes <= maximumArchiveBytes;
        }

        friend constexpr bool operator==(PortablePackageArchiveLimits, PortablePackageArchiveLimits) noexcept = default;
    };

    /** @brief Optional diagnostic sections that do not participate in program identity. */
    struct PortablePackageArchiveMetadata {
        std::span<const std::byte> sourceMap{};
        std::span<const std::byte> diagnostics{};
        PortablePackageRuntimeVersion minimumRuntime = kPortablePackageRuntimeVersion;
    };

    /** @brief Stable archive construction or validation failure. */
    enum class PortablePackageArchiveErrorCode : uint8_t {
        InvalidConfiguration,
        InvalidPackage,
        ArchiveTooLarge,
        ProgramTooLarge,
        ProgramCapacityExceeded,
        ArtifactCapacityExceeded,
        ArtifactBytesExceeded,
        MetadataBytesExceeded,
        Truncated,
        InvalidMagic,
        UnsupportedFormat,
        RuntimeTooOld,
        InvalidHeader,
        InvalidSectionTable,
        ChecksumMismatch,
        InvalidArtifactKey,
        NonCanonicalEncoding,
        SignatureRejected,
        OutOfMemory,
    };

    /** @brief Archive failure located at one byte offset and optional semantic identity. */
    struct PortablePackageArchiveError {
        PortablePackageArchiveErrorCode code = PortablePackageArchiveErrorCode::InvalidPackage;
        uint64_t offset = 0;
        uint64_t detail = 0;
        SemanticHash program{};
        SemanticHash artifact{};

        friend constexpr bool operator==(PortablePackageArchiveError, PortablePackageArchiveError) noexcept = default;
    };

    /** @brief Borrowed exact artifact bytes decoded from one validated archive. */
    struct PortablePackageArchiveArtifactView {
        ArtifactKey key{};
        std::span<const std::byte> payload{};
    };

    /**
     * @brief Validated zero-copy view of one canonical portable package archive.
     * @details The source byte image must outlive this view and every span returned by it.
     */
    class PortablePackageArchiveView final {
    public:
        /** @brief Validate bytes whose trust was established by an enclosing transport or local policy. */
        [[nodiscard]] static std::expected<PortablePackageArchiveView, PortablePackageArchiveError>
        OpenTrusted(std::span<const std::byte> bytes, PortablePackageArchiveLimits limits = {}) noexcept;

        /** @brief Verify detached trust evidence before decoding untrusted cross-machine bytes. */
        template<PortablePackageSignatureVerifier Verifier>
        [[nodiscard]] static std::expected<PortablePackageArchiveView, PortablePackageArchiveError>
        OpenVerified(std::span<const std::byte> bytes, PortablePackageDetachedSignature signature,
                     const Verifier& verifier, PortablePackageArchiveLimits limits = {}) noexcept {
            if (auto extent = ValidateExtent(bytes, limits); !extent) {
                return std::unexpected(extent.error());
            }
            if (!signature.Valid()) {
                return std::unexpected(PortablePackageArchiveError{
                    .code = PortablePackageArchiveErrorCode::SignatureRejected,
                    .program = PeekProgramIdentity(bytes),
                });
            }
            const auto trust = verifier.Verify(PortablePackageVerificationRequest{
                .context = kPortablePackageSignatureContext,
                .archive = bytes,
                .signature = signature,
            });
            if (!trust) {
                return std::unexpected(PortablePackageArchiveError{
                    .code = PortablePackageArchiveErrorCode::SignatureRejected,
                    .detail = static_cast<uint64_t>(trust.error()),
                    .program = PeekProgramIdentity(bytes),
                });
            }
            if (!trust->Valid() || trust->scheme != signature.scheme) {
                return std::unexpected(PortablePackageArchiveError{
                    .code = PortablePackageArchiveErrorCode::SignatureRejected,
                    .detail = static_cast<uint64_t>(PortablePackageTrustError::ProviderFailure),
                    .program = PeekProgramIdentity(bytes),
                });
            }
            auto archive = OpenTrusted(bytes, limits);
            if (archive) {
                archive->verifiedProvenance_ = PortablePackageProvenance::Verified(*trust);
            }
            return archive;
        }

        /** @brief Validate portable program semantics and copy payloads into one owning runtime projection. */
        [[nodiscard]] std::expected<PortablePackage, PortablePackageArchiveError> Materialize() const noexcept;

        /** @brief Return the exact canonical archive image retained by the caller. */
        [[nodiscard]] std::span<const std::byte> Bytes() const noexcept { return bytes_; }

        /** @brief Return the canonical program bytes without materializing runtime ownership. */
        [[nodiscard]] std::span<const std::byte> ProgramBytes() const noexcept { return programBytes_; }

        /** @brief Return artifact keys and borrowed payload bytes in canonical order. */
        [[nodiscard]] std::span<const PortablePackageArchiveArtifactView> Artifacts() const noexcept {
            return artifacts_;
        }

        /** @brief Return optional source-map bytes. */
        [[nodiscard]] std::span<const std::byte> SourceMap() const noexcept { return sourceMap_; }

        /** @brief Return optional diagnostic-name bytes. */
        [[nodiscard]] std::span<const std::byte> Diagnostics() const noexcept { return diagnostics_; }

        /** @brief Return the minimum runtime level required by this archive. */
        [[nodiscard]] PortablePackageRuntimeVersion MinimumRuntime() const noexcept { return minimumRuntime_; }

        /** @brief Return the canonical root program identity recorded by the archive. */
        [[nodiscard]] SemanticHash ProgramIdentity() const noexcept { return programIdentity_; }

        /** @brief Return authenticated admission provenance, or no value for a transport-trusted view. */
        [[nodiscard]] std::optional<PortablePackageProvenance> VerifiedProvenance() const noexcept {
            return verifiedProvenance_;
        }

    private:
        [[nodiscard]] static std::expected<void, PortablePackageArchiveError>
        ValidateExtent(std::span<const std::byte> bytes, PortablePackageArchiveLimits limits) noexcept;

        [[nodiscard]] static SemanticHash PeekProgramIdentity(std::span<const std::byte> bytes) noexcept;

        std::span<const std::byte> bytes_{};
        std::span<const std::byte> programBytes_{};
        std::vector<PortablePackageArchiveArtifactView> artifacts_{};
        std::span<const std::byte> sourceMap_{};
        std::span<const std::byte> diagnostics_{};
        PortablePackageRuntimeVersion minimumRuntime_{};
        SemanticHash programIdentity_{};
        std::optional<PortablePackageProvenance> verifiedProvenance_{};
        uint64_t programOffset_ = 0;
        KernelProgramLimits programLimits_{};
    };

    /** @brief Owning canonical bytes suitable for content addressing and detached signing. */
    class PortablePackageArchive final {
    public:
        PortablePackageArchive() = delete;

        /** @brief Encode one validated runtime package and optional diagnostic sections canonically. */
        [[nodiscard]] static std::expected<PortablePackageArchive, PortablePackageArchiveError>
        Create(const PortablePackage& package, PortablePackageArchiveMetadata metadata = {},
               PortablePackageArchiveLimits limits = {}) noexcept;

        /** @brief Return the canonical byte image signed by external trust providers. */
        [[nodiscard]] std::span<const std::byte> Bytes() const noexcept { return bytes_; }

    private:
        explicit PortablePackageArchive(std::vector<std::byte> bytes) noexcept : bytes_(std::move(bytes)) {}

        std::vector<std::byte> bytes_{};
    };

    static_assert(std::is_trivially_copyable_v<PortablePackageRuntimeVersion>);
    static_assert(std::is_trivially_copyable_v<PortablePackageArchiveLimits>);
    static_assert(std::is_trivially_copyable_v<PortablePackageArchiveError>);

} // namespace Sora::Compute

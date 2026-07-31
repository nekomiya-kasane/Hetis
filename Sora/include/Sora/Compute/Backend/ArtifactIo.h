/**
 * @file ArtifactIo.h
 * @brief Shared backend ABI values for persistent artifact import and export.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Artifact.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace Sora::Compute {

    /** @brief Versioned request for exporting one live artifact into backend-private reconstructible bytes. */
    struct BackendArtifactExportRequest {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(BackendArtifactExportRequest);
        ArtifactKey key{};
        ArtifactView artifact{};
    };

    using BackendArtifactBytesDestroy = void (*)(void* state) noexcept;

    /** @brief ABI payload transferring temporary ownership of exported bytes to the runtime. */
    struct BackendArtifactBytes {
        const std::byte* data = nullptr;
        uint64_t size = 0;
        void* state = nullptr;
        BackendArtifactBytesDestroy destroy = nullptr;

        /** @brief Return the exported bytes without copying. */
        [[nodiscard]] constexpr std::span<const std::byte> Bytes() const noexcept {
            if (size > std::numeric_limits<size_t>::max()) {
                return {};
            }
            return {data, static_cast<size_t>(size)};
        }
    };

    namespace Detail {

        inline void ReleaseArtifactBytes(BackendArtifactBytes payload) noexcept {
            if (payload.state != nullptr && payload.destroy != nullptr) {
                payload.destroy(payload.state);
            }
        }

    } // namespace Detail

    static_assert(std::is_trivially_copyable_v<BackendArtifactExportRequest>);
    static_assert(std::is_trivially_copyable_v<BackendArtifactBytes>);
    static_assert(std::is_standard_layout_v<BackendArtifactExportRequest>);
    static_assert(std::is_standard_layout_v<BackendArtifactBytes>);

} // namespace Sora::Compute

/**
 * @file SystemArtifactStore.h
 * @brief Platform cache-root policy for automatic target artifact persistence.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/ArtifactStore.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

namespace Sora::Compute {

    /** @brief Process-environment override for the automatic artifact-cache namespace. */
    inline constexpr std::string_view kSystemArtifactStoreRootEnvironment = "SORA_COMPUTE_ARTIFACT_CACHE_DIR";

    /** @brief Availability policy for the automatically resolved system artifact store. */
    enum class SystemArtifactStoreMode : uint8_t {
        Disabled,
        Opportunistic,
        Required,
    };

    /** @brief Automatic platform cache-root and bounded-retention policy. */
    struct SystemArtifactStorePolicy {
        SystemArtifactStoreMode mode = SystemArtifactStoreMode::Opportunistic;
        uint64_t maximumPayloadBytes = uint64_t{1} << 30U;
        uint64_t maximumStorageBytes = uint64_t{8} << 30U;
        uint64_t maximumEntryCount = 4096;
        uint64_t reconciliationPublicationInterval = 256;
        uint64_t reconciliationByteInterval = uint64_t{64} << 20U;
        bool durableWrites = false;

        /** @brief Return whether the closed mode and every retention budget are valid. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            const bool validMode = mode == SystemArtifactStoreMode::Disabled ||
                                   mode == SystemArtifactStoreMode::Opportunistic ||
                                   mode == SystemArtifactStoreMode::Required;
            const bool validRetention = maximumPayloadBytes != 0 && maximumStorageBytes != 0 &&
                                        maximumEntryCount != 0 && reconciliationPublicationInterval != 0 &&
                                        reconciliationByteInterval != 0;
            return validMode && (mode == SystemArtifactStoreMode::Disabled || validRetention);
        }
    };

    /** @brief Resolve the current platform cache namespace without creating filesystem state. */
    [[nodiscard]] std::expected<std::optional<ArtifactStoreConfig>, ArtifactStoreError>
    ResolveSystemArtifactStoreConfig(SystemArtifactStorePolicy policy = {}) noexcept;

} // namespace Sora::Compute

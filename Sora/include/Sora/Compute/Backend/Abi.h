/**
 * @file Abi.h
 * @brief Stable version identity shared by compute backend callback tables.
 * @ingroup Compute
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace Sora::Compute {

    inline constexpr uint32_t kComputeBackendAbiVersion = 10;

    /** @brief Return whether an ABI structure exposes the complete current-version prefix. */
    [[nodiscard]] constexpr bool ValidBackendAbi(uint32_t version, uint32_t size, size_t requiredSize) noexcept {
        return version == kComputeBackendAbiVersion && size >= requiredSize;
    }

} // namespace Sora::Compute

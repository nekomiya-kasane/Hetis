/**
 * @file DisplayMetrics.h
 * @brief Platform-independent display metric conversions.
 * @ingroup Platform
 */
#pragma once

#include <cstdint>

namespace Mashiro::Platform {

    inline constexpr float kDesktopDpiBase = 96.0f;

    /** @brief Convert physical DPI to desktop scale factor. */
    [[nodiscard]] constexpr float DpiScaleFromDpi(std::uint32_t dpi) noexcept {
        return static_cast<float>(dpi) / kDesktopDpiBase;
    }

} // namespace Mashiro::Platform
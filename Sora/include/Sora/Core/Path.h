/**
 * @file Path.h
 * @brief Allocation-free lexical queries over narrow path spellings.
 * @details These helpers deliberately operate on @c std::string_view rather than @c std::filesystem::path. They are
 * constexpr, preserve the caller's byte spelling, and accept both slash families for diagnostics and portable module
 * names. Use @c std::filesystem::path for native path semantics, filesystem access, and canonicalization.
 * @ingroup Core
 */
#pragma once

#include <string_view>
#include <utility>

#include <Sora/Platform.h>

namespace Sora {

    /** @brief Return whether @p path contains either common directory separator. */
    [[nodiscard]] constexpr bool HasPathSeparator(std::string_view path) noexcept {
        return path.contains('/') || path.contains('\\');
    }

    /** @brief Split @p path into its directory prefix and final filename view. */
    [[nodiscard]] constexpr std::pair<std::string_view, std::string_view>
    SplitDirectory(std::string_view path) noexcept {
        const size_t separator = path.find_last_of("/\\");
        if (separator == std::string_view::npos) {
            return {{}, path};
        }
        return {path.substr(0, separator + 1), path.substr(separator + 1)};
    }

    /** @brief Return the final lexical filename component of @p path without allocating. */
    [[nodiscard]] constexpr std::string_view FileName(std::string_view path) noexcept {
        return SplitDirectory(path).second;
    }

} // namespace Sora

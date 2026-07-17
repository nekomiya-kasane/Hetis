/**
 * @file Path.h
 * @brief Allocation-free lexical path queries and shared-library name manipulation.
 * @ingroup PAL
 */
#pragma once

#include <Sora/Platform.h>

#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace Sora::PAL {

    /** @brief Native shared-library suffixes worth probing across supported toolchains. */
    constexpr std::array kKnownSharedLibrarySuffixes{std::string_view{".dll"}, std::string_view{".so"},
                                                     std::string_view{".dylib"}};

    /** @brief Return true when @p name contains an explicit directory component. */
    [[nodiscard]] constexpr bool HasPathSeparator(std::string_view name) noexcept {
        return name.contains(Sora::Platform::kPathSeparator_Windows) ||
               name.contains(Sora::Platform::kPathSeparator_Unix);
    }

    /** @brief Return true when @p name has a suffix associated with a known shared-library format. */
    [[nodiscard]] inline bool HasSharedLibrarySuffix(std::string_view name) {
        for (std::string_view suffix : kKnownSharedLibrarySuffixes) {
            if (name.ends_with(suffix) || name.contains(std::string{suffix} + '.')) {
                return true;
            }
        }
        return false;
    }

    /** @brief Split @p path into directory prefix and final filename. */
    [[nodiscard]] constexpr std::pair<std::string_view, std::string_view>
    SplitDirectory(std::string_view path) noexcept {
        const size_t slash = path.find_last_of("/\\");
        if (slash == std::string_view::npos) {
            return {{}, path};
        }
        return {path.substr(0, slash + 1), path.substr(slash + 1)};
    }

    /** @brief Return the final filename component of @p path without allocating. */
    [[nodiscard]] constexpr std::string_view FileName(std::string_view path) noexcept {
        return SplitDirectory(path).second;
    }

    /** @brief Return @p name without a known shared-library suffix. */
    [[nodiscard]] inline std::string RemoveSharedLibrarySuffix(std::string_view name) {
        for (std::string_view suffix : kKnownSharedLibrarySuffixes) {
            if (name.ends_with(suffix)) {
                return std::string{name.substr(0, name.size() - suffix.size())};
            }
            if (const size_t position = name.find(std::string{suffix} + '.'); position != std::string_view::npos) {
                return std::string{name.substr(0, position)};
            }
        }
        return std::string{name};
    }

} // namespace Sora::PAL

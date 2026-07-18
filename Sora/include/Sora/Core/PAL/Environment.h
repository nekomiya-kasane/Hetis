/**
 * @file Environment.h
 * @brief Native process-environment access, immutable snapshots, and transactional bulk mutation.
 * @ingroup PAL
 *
 * @details PAL models the process environment as a flat set of UTF-8 @c (name,value) pairs. A name must be non-empty,
 * contain neither @c '=' nor NUL, and be valid UTF-8. A value may be empty and may contain @c '=', but must contain no
 * NUL and must be valid UTF-8. An existing empty value is therefore distinct from an absent variable:
 *
 * - @c NAME= represents the present pair @c ("NAME","").
 * - No @c NAME entry represents absence and is returned as @c std::nullopt.
 *
 * PAL accepts names and values, not shell assignment syntax. It does not parse @c export, quoting, @c $NAME,
 * @c %NAME%, or command-line expansion. Conceptually, each pair is translated to the native @c NAME=VALUE form:
 *
 * - On Windows, UTF-8 names and values are converted to UTF-16 and passed to the wide environment APIs. Snapshots are
 *   decoded from the double-NUL-terminated UTF-16 environment block. Names compare case-insensitively; per-drive
 *   current-directory pseudo-entries whose native names begin with @c '=' are intentionally omitted.
 * - On POSIX systems, validated UTF-8 bytes are passed unchanged to @c getenv, @c setenv, and @c unsetenv. Snapshots
 *   decode the NUL-terminated @c char* entries in @c environ. Names compare case-sensitively.
 *
 * Hierarchical configuration paths are encoded above PAL by @ref Sora::Configuration::EncodeEnvironmentName: every
 * segment becomes upper-snake case and levels are separated by two underscores. For example, scope @c sora and path
 * @c renderer.backend.device become the native pair
 * @c SORA__RENDERER__BACKEND__DEVICE=discrete. PAL receives that encoded name unchanged:
 *
 * @code{.cpp}
 * auto written = Sora::PAL::WriteEnvironmentVariable("SORA__RENDERER__BACKEND__DEVICE", "discrete");
 * @endcode
 */
#pragma once

#include <Sora/Core/StringUtils.h>
#include <Sora/ErrorCode.h>
#include <Sora/Platform.h>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Sora::PAL {

    /** @brief Compare native names using the target platform's environment-name case semantics. */
    [[nodiscard]] constexpr std::strong_ordering CompareEnvironmentNames(std::string_view lhs,
                                                                         std::string_view rhs) noexcept {
        return kIsWindows ? Ascii::CompareIgnoreCase(lhs, rhs) : lhs <=> rhs;
    }

    /** @brief Return whether @p name begins with @p prefix under platform-native name comparison. */
    [[nodiscard]] constexpr bool EnvironmentNameStartsWith(std::string_view name, std::string_view prefix) noexcept {
        return kIsWindows ? Ascii::EqualsIgnoreCase(name.substr(0, prefix.size()), prefix) : name.starts_with(prefix);
    }

    /** @brief Non-owning environment entry returned by an immutable snapshot. */
    struct EnvironmentEntryView {
        std::string_view name;  /**< Native variable name. */
        std::string_view value; /**< UTF-8 variable value, which may be empty. */
    };

    /** @brief Half-open index range in a sorted environment snapshot. */
    struct EnvironmentIndexRange {
        size_t begin = 0; /**< First matching entry index. */
        size_t end = 0;   /**< One-past-last matching entry index. */

        /** @brief Return the number of entries in this range. */
        [[nodiscard]] constexpr size_t Size() const noexcept { return end - begin; }

        /** @brief Return whether this range contains no entries. */
        [[nodiscard]] constexpr bool Empty() const noexcept { return begin == end; }
    };

    /** @brief Immutable, contiguous, name-sorted snapshot of the process environment. */
    class EnvironmentSnapshot {
    public:
        /** @brief Return the number of captured variables. */
        [[nodiscard]] size_t Size() const noexcept { return entries_.size(); }

        /** @brief Return whether no variables were captured. */
        [[nodiscard]] bool Empty() const noexcept { return entries_.empty(); }

        /** @brief Return entry @p index. */
        [[nodiscard]] EnvironmentEntryView operator[](size_t index) const noexcept;

        /** @brief Find @p name using platform-native case semantics. */
        [[nodiscard]] std::optional<std::string_view> Find(std::string_view name) const noexcept;

        /** @brief Return the contiguous range of names beginning with @p prefix. */
        [[nodiscard]] EnvironmentIndexRange PrefixRange(std::string_view prefix) const noexcept;

    private:
        struct StoredEntry {
            uint32_t nameOffset = 0;
            uint32_t nameSize = 0;
            uint32_t valueOffset = 0;
            uint32_t valueSize = 0;
        };

        friend Result<EnvironmentSnapshot> CaptureEnvironment();

        std::string storage_;
        std::vector<StoredEntry> entries_;
    };

    /** @brief One requested process-environment mutation; @c nullopt removes the variable. */
    struct EnvironmentMutation {
        std::string_view name;                 /**< Native variable name. */
        std::optional<std::string_view> value; /**< Replacement UTF-8 value, or @c nullopt to unset. */
    };

    /** @brief Read one variable, distinguishing absence from an existing empty value. */
    [[nodiscard]] Result<std::optional<std::string>> ReadEnvironmentVariable(std::string_view name);

    /** @brief Return whether @p name exists without materializing its value. */
    [[nodiscard]] Result<bool> HasEnvironmentVariable(std::string_view name);

    /** @brief Set @p name to UTF-8 @p value. */
    [[nodiscard]] Result<void> WriteEnvironmentVariable(std::string_view name, std::string_view value);

    /** @brief Remove @p name; removing an absent variable succeeds. */
    [[nodiscard]] Result<void> RemoveEnvironmentVariable(std::string_view name);

    /** @brief Capture one immutable process-environment snapshot. */
    [[nodiscard]] Result<EnvironmentSnapshot> CaptureEnvironment();

    /** @brief Apply @p mutations under one Sora lock and roll back earlier changes when a native operation fails. */
    [[nodiscard]] Result<void> ApplyEnvironmentMutations(std::span<const EnvironmentMutation> mutations);

} // namespace Sora::PAL

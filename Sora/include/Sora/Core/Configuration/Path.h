/**
 * @file Path.h
 * @brief Validated dotted configuration paths and portable environment-variable name encoding.
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/StringUtils.h>
#include <Sora/ErrorCode.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <ranges>
#include <utility>

namespace Sora::Configuration {

    /** @brief Return whether @p segment is a portable configuration-path segment. */
    [[nodiscard]] constexpr bool IsPathSegment(std::string_view segment) noexcept {
        if (segment.empty() || !Ascii::IsAlpha(segment.front())) {
            return false;
        }
        return std::ranges::all_of(segment | std::views::drop(1), [](char c) {
            return Ascii::IsAlpha(c) || Ascii::IsDigit(c) || c == '_' || c == '-';
        });
    }

    /** @brief Validate dotted configuration path @p path. */
    [[nodiscard]] constexpr Result<void> ValidatePath(std::string_view path) noexcept {
        if (path.empty()) {
            return std::unexpected(ErrorCode::EmptyConfigurationPath);
        }
        size_t begin = 0;
        while (begin <= path.size()) {
            const size_t end = path.find('.', begin);
            const std::string_view segment = path.substr(begin, end == std::string_view::npos ? end : end - begin);
            if (segment.empty()) {
                return std::unexpected(ErrorCode::EmptyConfigurationPathSegment);
            }
            if (!IsPathSegment(segment)) {
                return std::unexpected(ErrorCode::InvalidConfigurationPathCharacter);
            }
            if (end == std::string_view::npos) {
                break;
            }
            begin = end + 1;
        }
        return {};
    }

    /** @brief Non-owning validated dotted configuration path. */
    class PathView {
    public:
        /** @brief Validate @p text and return a non-owning path view. */
        [[nodiscard]] static constexpr Result<PathView> Parse(std::string_view text) noexcept {
            if (auto valid = ValidatePath(text); !valid) {
                return std::unexpected(valid.error());
            }
            return PathView{text};
        }

        /** @brief Return the original dotted path text. */
        [[nodiscard]] constexpr std::string_view Text() const noexcept { return text_; }

        /** @brief Return the number of path segments. */
        [[nodiscard]] constexpr size_t Depth() const noexcept { return 1 + std::ranges::count(text_, '.'); }

    private:
        explicit constexpr PathView(std::string_view text) noexcept : text_{text} {}
        std::string_view text_;
    };

    /** @brief Owning validated dotted configuration path. */
    class Path {
    public:
        /** @brief Validate and copy @p text into an owning path. */
        [[nodiscard]] static Result<Path> Parse(std::string_view text) {
            if (auto valid = ValidatePath(text); !valid) {
                return std::unexpected(valid.error());
            }
            return Path{std::string{text}};
        }

        /** @brief Return a non-owning view over this path. */
        [[nodiscard]] constexpr PathView View() const noexcept { return *PathView::Parse(text_); }

        /** @brief Return the original dotted path text. */
        [[nodiscard]] constexpr std::string_view Text() const noexcept { return text_; }

    private:
        explicit Path(std::string text) : text_{std::move(text)} {}
        std::string text_;
    };

    /** @brief Join two validated path fragments with one dot. */
    [[nodiscard]] constexpr Result<std::string> JoinPath(std::string_view prefix, std::string_view suffix) {
        if (prefix.empty()) {
            if (auto valid = ValidatePath(suffix); !valid) {
                return std::unexpected(valid.error());
            }
            return std::string{suffix};
        }
        if (suffix.empty()) {
            if (auto valid = ValidatePath(prefix); !valid) {
                return std::unexpected(valid.error());
            }
            return std::string{prefix};
        }
        if (auto valid = ValidatePath(prefix); !valid) {
            return std::unexpected(valid.error());
        }
        if (auto valid = ValidatePath(suffix); !valid) {
            return std::unexpected(valid.error());
        }
        std::string result{prefix};
        result.push_back('.');
        result += suffix;
        return result;
    }

    /**
     * @brief Encode @p path under @p scope as a portable native environment-variable name.
     * @details Every segment is converted to upper-snake spelling and levels are separated by two underscores.
     */
    [[nodiscard]] constexpr Result<std::string> EncodeEnvironmentName(std::string_view scope, std::string_view path) {
        if (!scope.empty() && !IsPathSegment(scope)) {
            return std::unexpected(ErrorCode::InvalidEnvironmentName);
        }
        if (auto valid = ValidatePath(path); !valid) {
            return std::unexpected(valid.error());
        }

        std::string result;
        if (!scope.empty()) {
            result = Ascii::ToUpperSnake(scope);
        }
        size_t begin = 0;
        while (begin <= path.size()) {
            const size_t end = path.find('.', begin);
            const std::string_view segment = path.substr(begin, end == std::string_view::npos ? end : end - begin);
            if (!result.empty()) {
                result += "__";
            }
            result += Ascii::ToUpperSnake(segment);
            if (end == std::string_view::npos) {
                break;
            }
            begin = end + 1;
        }
        return result;
    }

} // namespace Sora::Configuration

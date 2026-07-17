/**
 * @file StringUtils.h
 * @brief Small constexpr ASCII and string-normalisation utilities shared by core facilities.
 * @ingroup Core
 */
#pragma once

#include <algorithm>
#include <compare>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <ranges>

namespace Sora {

    namespace Concept {

        template<typename T>
        concept NarrowStringLike = requires(T&& value) { std::string_view{std::forward<T>(value)}; };

        template<typename T>
        concept Utf8StringLike = requires(T&& value) { std::u8string_view{std::forward<T>(value)}; };

        template<typename T>
        concept Utf16StringLike = requires(T&& value) { std::u16string_view{std::forward<T>(value)}; };

        template<typename T>
        concept Utf32StringLike = requires(T&& value) { std::u32string_view{std::forward<T>(value)}; };

        template<typename T>
        concept WideStringLike = requires(T&& value) { std::wstring_view{std::forward<T>(value)}; };

    } // namespace Concept

    /** @brief ASCII character classification and case-folding helpers. */
    namespace Ascii {

        /** @brief Return true when @p c is an ASCII alphabetic character. */
        [[nodiscard]] constexpr bool IsAlpha(char c) noexcept {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        }

        /** @brief Return true when @p c is an ASCII decimal digit. */
        [[nodiscard]] constexpr bool IsDigit(char c) noexcept {
            return c >= '0' && c <= '9';
        }

        /** @brief Return true when @p c is an ASCII hexadecimal digit. */
        [[nodiscard]] constexpr bool IsHexDigit(char c) noexcept {
            return IsDigit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        }

        /** @brief Return the hexadecimal value of @p c, or -1 when @p c is not hexadecimal. */
        [[nodiscard]] constexpr int HexValue(char c) noexcept {
            if (c >= '0' && c <= '9') {
                return c - '0';
            }
            if (c >= 'A' && c <= 'F') {
                return c - 'A' + 10;
            }
            if (c >= 'a' && c <= 'f') {
                return c - 'a' + 10;
            }
            return -1;
        }

        /** @brief Return true when @p c is an ASCII whitespace character. */
        [[nodiscard]] constexpr bool IsWhitespace(char c) noexcept {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
        }

        /** @brief Convert an ASCII uppercase letter to lowercase; other bytes pass through. */
        [[nodiscard]] constexpr char ToLower(char c) noexcept {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
        }

        /** @brief Convert an ASCII lowercase letter to uppercase; other bytes pass through. */
        [[nodiscard]] constexpr char ToUpper(char c) noexcept {
            return c >= 'a' && c <= 'z' ? static_cast<char>(c - ('a' - 'A')) : c;
        }

        /** @brief Uppercase a hexadecimal digit while leaving other bytes unchanged. */
        [[nodiscard]] constexpr char ToUpperHex(char c) noexcept {
            return c >= 'a' && c <= 'f' ? ToUpper(c) : c;
        }

        /** @brief Lowercase a hexadecimal digit while leaving other bytes unchanged. */
        [[nodiscard]] constexpr char ToLowerHex(char c) noexcept {
            return c >= 'A' && c <= 'F' ? ToLower(c) : c;
        }

        /** @brief Convert a character to a std::byte. */
        [[nodiscard]] constexpr std::byte ToByte(char c) noexcept {
            return std::byte{static_cast<unsigned char>(c)};
        }

        /** @brief Convert a std::byte to a character. */
        [[nodiscard]] constexpr char FromByte(std::byte b) noexcept {
            return static_cast<char>(std::to_integer<unsigned char>(b));
        }

        /** @brief Return true when @p lhs and @p rhs are equal after ASCII case folding. */
        [[nodiscard]] constexpr bool EqualsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept {
            return std::ranges::equal(lhs, rhs, std::equal_to<>{}, ToLower, ToLower);
        }

        /** @brief General string algorithms used by compile-time metadata generators and runtime helpers. */
        inline namespace String {

            /** @brief Compare @p lhs and @p rhs lexicographically after ASCII case folding. */
            [[nodiscard]] constexpr std::strong_ordering CompareIgnoreCase(std::string_view lhs,
                                                                           std::string_view rhs) noexcept {
                for (auto [a, b] : std::views::zip(lhs, rhs)) {
                    if (const auto order = ToLower(a) <=> ToLower(b); order != 0) {
                        return order;
                    }
                }
                return lhs.size() <=> rhs.size();
            }

            /** @brief Return @p text without leading ASCII whitespace. */
            [[nodiscard]] constexpr std::string_view TrimStart(std::string_view text) noexcept {
                while (!text.empty() && IsWhitespace(text.front())) {
                    text.remove_prefix(1);
                }
                return text;
            }

            /** @brief Return @p text without trailing ASCII whitespace. */
            [[nodiscard]] constexpr std::string_view TrimEnd(std::string_view text) noexcept {
                while (!text.empty() && IsWhitespace(text.back())) {
                    text.remove_suffix(1);
                }
                return text;
            }

            /** @brief Return @p text without leading or trailing ASCII whitespace. */
            [[nodiscard]] constexpr std::string_view Trim(std::string_view text) noexcept {
                return TrimEnd(TrimStart(text));
            }

            /**
             * @brief Truncate @p text to at most @p maxBytes, appending @p marker when truncation is required.
             * @details This function operates on bytes. It does not preserve UTF-8 code-point boundaries. When
             * @p marker itself exceeds @p maxBytes, its prefix is returned so the result always satisfies the bound.
             * @param[in] text Source bytes.
             * @param[in] maxBytes Maximum result size in bytes.
             * @param[in] marker Truncation marker, conventionally @c "...".
             * @return A copy of @p text when it fits, otherwise a bounded prefix followed by @p marker.
             */
            [[nodiscard]] constexpr std::string Truncate(std::string_view text, size_t maxBytes,
                                                         std::string_view marker = "...") {
                if (text.size() <= maxBytes) {
                    return std::string{text};
                }
                if (marker.size() >= maxBytes) {
                    return std::string{marker.substr(0, maxBytes)};
                }
                std::string result;
                result.reserve(maxBytes);
                result.append(text.substr(0, maxBytes - marker.size()));
                result.append(marker);
                return result;
            }

            /**
             * @brief Append @p source converted to lower-kebab spelling into @p out.
             * @details Accepts ASCII letters, digits, dot, underscore, space, and hyphen. Word separators collapse to a
             * single hyphen; uppercase letters introduce a word boundary when they follow another segment character.
             */
            constexpr std::string ToLowerKebab(std::string_view source) {
                std::string out;
                for (size_t i = 0; i < source.size(); ++i) {
                    const char c = source[i];
                    const bool upper = c >= 'A' && c <= 'Z', lower = c >= 'a' && c <= 'z';
                    const bool digit = Ascii::IsDigit(c);
                    if (c == '_' || c == ' ' || c == '-') {
                        if (!out.empty() && out.back() != '-' && out.back() != '/') {
                            out.push_back('-');
                        }
                    } else if (upper) {
                        if (i != 0 && !out.empty() && out.back() != '-' && out.back() != '/') {
                            out.push_back('-');
                        }
                        out.push_back(Ascii::ToLower(c));
                    } else if (lower || digit || c == '.') {
                        out.push_back(c);
                    } else {
                        throw "Sora string segment contains a non URI-safe character.";
                    }
                }
                if (!out.empty() && out.back() == '-') {
                    out.pop_back();
                }
                return out;
            }

            /**
             * @brief Convert @p source to upper-snake spelling.
             * @details Underscore, hyphen, and ASCII whitespace separate words. A lowercase-to-uppercase transition
             * starts a new word, as does the final uppercase letter of an acronym before a lowercase word.
             */
            constexpr std::string ToUpperSnake(std::string_view source) {
                std::string out;
                out.reserve(source.size());

                bool pendingSeparator = false;
                for (size_t index = 0; index < source.size(); ++index) {
                    const char c = source[index];
                    if (c == '_' || c == '-' || IsWhitespace(c)) {
                        pendingSeparator = !out.empty();
                        continue;
                    }
                    if (!IsAlpha(c) && !IsDigit(c)) {
                        throw "Sora upper-snake source contains an unsupported character.";
                    }

                    const bool upper = c >= 'A' && c <= 'Z';
                    const bool previousLowerOrDigit =
                        index != 0 &&
                        ((source[index - 1] >= 'a' && source[index - 1] <= 'z') || IsDigit(source[index - 1]));
                    const bool acronymBoundary = upper && index != 0 && index + 1 < source.size() &&
                                                 source[index - 1] >= 'A' && source[index - 1] <= 'Z' &&
                                                 source[index + 1] >= 'a' && source[index + 1] <= 'z';
                    if (!out.empty() && out.back() != '_' &&
                        (pendingSeparator || (upper && previousLowerOrDigit) || acronymBoundary)) {
                        out.push_back('_');
                    }
                    pendingSeparator = false;
                    out.push_back(ToUpper(c));
                }

                if (!out.empty() && out.back() == '_') {
                    out.pop_back();
                }
                return out;
            }

        } // namespace String

    } // namespace Ascii

} // namespace Sora

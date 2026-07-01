/**
 * @file Unicode.h
 * @brief Unicode scalar, UTF-16 decoding, and UTF-8 encoding helpers.
 * @ingroup Core
 */
#pragma once

#include <array>
#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>

namespace Mashiro::Unicode {

    inline constexpr char32_t kReplacementCharacter = 0xFFFDu;
    inline constexpr char32_t kMaxScalarValue = 0x10FFFFu;
    inline constexpr char32_t kHighSurrogateFirst = 0xD800u;
    inline constexpr char32_t kHighSurrogateLast = 0xDBFFu;
    inline constexpr char32_t kLowSurrogateFirst = 0xDC00u;
    inline constexpr char32_t kLowSurrogateLast = 0xDFFFu;

    template<class T>
    concept Utf16CodeUnit = std::integral<T> && sizeof(T) == sizeof(std::uint16_t);

    /** @brief True iff @p codepoint is a UTF-16 high surrogate. */
    [[nodiscard]] constexpr bool IsHighSurrogate(char32_t codepoint) noexcept {
        return codepoint >= kHighSurrogateFirst && codepoint <= kHighSurrogateLast;
    }

    /** @brief True iff @p codepoint is a UTF-16 low surrogate. */
    [[nodiscard]] constexpr bool IsLowSurrogate(char32_t codepoint) noexcept {
        return codepoint >= kLowSurrogateFirst && codepoint <= kLowSurrogateLast;
    }

    /** @brief True iff @p codepoint is a Unicode scalar value. */
    [[nodiscard]] constexpr bool IsScalarValue(char32_t codepoint) noexcept {
        return codepoint <= kMaxScalarValue && !IsHighSurrogate(codepoint) && !IsLowSurrogate(codepoint);
    }

    /** @brief Return @p codepoint, or U+FFFD when it is not a scalar value. */
    [[nodiscard]] constexpr char32_t SanitizeScalarValue(char32_t codepoint) noexcept {
        return IsScalarValue(codepoint) ? codepoint : kReplacementCharacter;
    }

    /** @brief Decode one well-formed UTF-16 surrogate pair. */
    [[nodiscard]] constexpr char32_t DecodeSurrogatePair(std::uint16_t high, std::uint16_t low) noexcept {
        return 0x10000u + (((static_cast<char32_t>(high) - kHighSurrogateFirst) << 10u) |
                           (static_cast<char32_t>(low) - kLowSurrogateFirst));
    }

    /** @brief Append @p codepoint encoded as UTF-8. */
    constexpr void AppendUtf8Scalar(std::string& out, char32_t codepoint) {
        const char32_t value = SanitizeScalarValue(codepoint);
        if (value <= 0x7Fu) {
            out.push_back(static_cast<char>(value));
        } else if (value <= 0x7FFu) {
            out.push_back(static_cast<char>(0xC0u | (value >> 6u)));
            out.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
        } else if (value <= 0xFFFFu) {
            out.push_back(static_cast<char>(0xE0u | (value >> 12u)));
            out.push_back(static_cast<char>(0x80u | ((value >> 6u) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xF0u | (value >> 18u)));
            out.push_back(static_cast<char>(0x80u | ((value >> 12u) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((value >> 6u) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
        }
    }

    /** @brief Zero, one, or two scalar values emitted by one UTF-16 input unit. */
    struct Utf16DecodeResult {
        std::array<char32_t, 2> codepoints{};
        std::uint8_t count = 0;

        /** @brief Append one scalar value after validity normalization. */
        constexpr void Append(char32_t codepoint) noexcept {
            codepoints[count++] = SanitizeScalarValue(codepoint);
        }
    };

    /** @brief Stateful UTF-16 stream decoder with replacement-character recovery. */
    class Utf16StreamDecoder final {
    public:
        /** @brief Consume one UTF-16 code unit. */
        [[nodiscard]] constexpr Utf16DecodeResult Push(std::uint16_t codeUnit) noexcept {
            Utf16DecodeResult out{};
            const auto value = static_cast<char32_t>(codeUnit);

            if (IsHighSurrogate(value)) {
                if (pendingHighSurrogate_ != 0) {
                    out.Append(kReplacementCharacter);
                }
                pendingHighSurrogate_ = codeUnit;
                return out;
            }

            if (IsLowSurrogate(value)) {
                if (pendingHighSurrogate_ != 0) {
                    out.Append(DecodeSurrogatePair(pendingHighSurrogate_, codeUnit));
                    pendingHighSurrogate_ = 0;
                } else {
                    out.Append(kReplacementCharacter);
                }
                return out;
            }

            if (pendingHighSurrogate_ != 0) {
                pendingHighSurrogate_ = 0;
                out.Append(kReplacementCharacter);
            }
            out.Append(value);
            return out;
        }

        /** @brief Emit U+FFFD for an unterminated high surrogate, if any. */
        [[nodiscard]] constexpr Utf16DecodeResult Flush() noexcept {
            Utf16DecodeResult out{};
            if (pendingHighSurrogate_ != 0) {
                pendingHighSurrogate_ = 0;
                out.Append(kReplacementCharacter);
            }
            return out;
        }

        /** @brief Clear any pending high surrogate without emission. */
        constexpr void Reset() noexcept { pendingHighSurrogate_ = 0; }

    private:
        std::uint16_t pendingHighSurrogate_ = 0;
    };

    /** @brief Convert a UTF-16 code-unit sequence to UTF-8 with replacement-character recovery. */
    template<Utf16CodeUnit CodeUnit>
    [[nodiscard]] constexpr std::string Utf16ToUtf8(std::basic_string_view<CodeUnit> text) {
        Utf16StreamDecoder decoder{};
        std::string out;
        out.reserve(text.size());

        for (CodeUnit codeUnit : text) {
            const auto decoded = decoder.Push(static_cast<std::uint16_t>(codeUnit));
            for (std::uint8_t i = 0; i < decoded.count; ++i) {
                AppendUtf8Scalar(out, decoded.codepoints[i]);
            }
        }

        const auto tail = decoder.Flush();
        for (std::uint8_t i = 0; i < tail.count; ++i) {
            AppendUtf8Scalar(out, tail.codepoints[i]);
        }
        return out;
    }

} // namespace Mashiro::Unicode
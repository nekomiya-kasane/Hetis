/**
 * @file Unicode.h
 * @brief Constexpr Unicode scalar, UTF-16 decoding, and UTF-8 encoding utilities.
 * @ingroup Core
 *
 * @details This header deliberately stays at the encoding boundary: it validates Unicode scalar values, decodes
 * UTF-16 streams, and emits UTF-8 bytes. It does not attempt normalization, collation, grapheme segmentation, or
 * locale-sensitive transforms; those are different abstractions with larger Unicode data-table requirements.
 */
#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace Sora::Unicode {

    /** @brief Unicode replacement character U+FFFD, used by recovery-oriented decoders. */
    inline constexpr char32_t kReplacementCharacter = 0xFFFDu;

    /** @brief Largest valid Unicode scalar value. */
    inline constexpr char32_t kMaxScalarValue = 0x10FFFFu;

    /** @brief First UTF-16 high-surrogate code unit. */
    inline constexpr char32_t kHighSurrogateFirst = 0xD800u;

    /** @brief Last UTF-16 high-surrogate code unit. */
    inline constexpr char32_t kHighSurrogateLast = 0xDBFFu;

    /** @brief First UTF-16 low-surrogate code unit. */
    inline constexpr char32_t kLowSurrogateFirst = 0xDC00u;

    /** @brief Last UTF-16 low-surrogate code unit. */
    inline constexpr char32_t kLowSurrogateLast = 0xDFFFu;

    /** @brief Error reported by strict Unicode encoders and decoders. */
    enum class Error : uint8_t {
        InvalidScalar,         /**< Input code point is not a Unicode scalar value. */
        IsolatedHighSurrogate, /**< UTF-16 stream ended or changed direction after a high surrogate. */
        IsolatedLowSurrogate,  /**< UTF-16 stream contained a low surrogate without a preceding high surrogate. */
    };

    /** @brief Integral 16-bit code-unit type accepted by UTF-16 helpers. */
    template<typename T>
    concept Utf16CodeUnit = std::integral<std::remove_cv_t<T>> && sizeof(std::remove_cv_t<T>) == sizeof(uint16_t);

    /** @brief Return true when @p codepoint is a UTF-16 high surrogate. */
    [[nodiscard]] constexpr bool IsHighSurrogate(char32_t codepoint) noexcept {
        return codepoint >= kHighSurrogateFirst && codepoint <= kHighSurrogateLast;
    }

    /** @brief Return true when @p codepoint is a UTF-16 low surrogate. */
    [[nodiscard]] constexpr bool IsLowSurrogate(char32_t codepoint) noexcept {
        return codepoint >= kLowSurrogateFirst && codepoint <= kLowSurrogateLast;
    }

    /** @brief Return true when @p codepoint is one of the UTF-16 surrogate code points. */
    [[nodiscard]] constexpr bool IsSurrogate(char32_t codepoint) noexcept {
        return IsHighSurrogate(codepoint) || IsLowSurrogate(codepoint);
    }

    /** @brief Return true when @p codepoint is a valid Unicode scalar value. */
    [[nodiscard]] constexpr bool IsScalarValue(char32_t codepoint) noexcept {
        return codepoint <= kMaxScalarValue && !IsSurrogate(codepoint);
    }

    /** @brief Return @p codepoint if it is scalar, otherwise return U+FFFD. */
    [[nodiscard]] constexpr char32_t SanitizeScalarValue(char32_t codepoint) noexcept {
        return IsScalarValue(codepoint) ? codepoint : kReplacementCharacter;
    }

    /** @brief Decode one well-formed UTF-16 surrogate pair into a Unicode scalar value. */
    [[nodiscard]] constexpr char32_t DecodeSurrogatePair(uint16_t high, uint16_t low) noexcept {
        return 0x10000u + (((static_cast<char32_t>(high) - kHighSurrogateFirst) << 10u) |
                           (static_cast<char32_t>(low) - kLowSurrogateFirst));
    }

    /** @brief UTF-8 byte sequence for a single scalar value. */
    struct Utf8Scalar {
        std::array<char, 4> bytes{}; /**< Encoded bytes. */
        uint8_t size = 0;            /**< Number of bytes used in @ref bytes. */

        /** @brief View over the encoded byte sequence. */
        [[nodiscard]] constexpr std::string_view View() const noexcept { return {bytes.data(), size}; }
    };

    /**
     * @brief Encode @p codepoint as UTF-8, rejecting non-scalar values.
     * @param[in] codepoint Unicode scalar value to encode.
     * @return Encoded UTF-8 bytes, or @ref Error::InvalidScalar.
     */
    [[nodiscard]] constexpr std::expected<Utf8Scalar, Error> EncodeUtf8Scalar(char32_t codepoint) noexcept {
        if (!IsScalarValue(codepoint)) {
            return std::unexpected(Error::InvalidScalar);
        }

        Utf8Scalar out{};
        if (codepoint <= 0x7Fu) {
            out.bytes[0] = static_cast<char>(codepoint);
            out.size = 1;
        } else if (codepoint <= 0x7FFu) {
            out.bytes[0] = static_cast<char>(0xC0u | (codepoint >> 6u));
            out.bytes[1] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
            out.size = 2;
        } else if (codepoint <= 0xFFFFu) {
            out.bytes[0] = static_cast<char>(0xE0u | (codepoint >> 12u));
            out.bytes[1] = static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu));
            out.bytes[2] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
            out.size = 3;
        } else {
            out.bytes[0] = static_cast<char>(0xF0u | (codepoint >> 18u));
            out.bytes[1] = static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3Fu));
            out.bytes[2] = static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu));
            out.bytes[3] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
            out.size = 4;
        }
        return out;
    }

    /** @brief Append @p codepoint encoded as UTF-8, replacing invalid scalar values with U+FFFD. */
    constexpr void AppendUtf8Scalar(std::string& out, char32_t codepoint) {
        auto encoded = EncodeUtf8Scalar(SanitizeScalarValue(codepoint));
        out.append(encoded->bytes.data(), encoded->size);
    }

    /** @brief Strictly append @p codepoint encoded as UTF-8. */
    constexpr std::expected<void, Error> AppendUtf8ScalarStrict(std::string& out, char32_t codepoint) {
        auto encoded = EncodeUtf8Scalar(codepoint);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        out.append(encoded->bytes.data(), encoded->size);
        return {};
    }

    /** @brief Zero, one, or two scalar values emitted by one UTF-16 input unit. */
    struct Utf16DecodeResult {
        std::array<char32_t, 2> scalars{};  /**< Decoded scalar values. */
        uint8_t count = 0;                  /**< Number of values stored in @ref scalars. */
        Error error = Error::InvalidScalar; /**< Error kind when @ref ok is false. */
        bool ok = true;                     /**< Whether decoding completed without recovery. */

        /** @brief Append a scalar value after replacement-oriented validity normalization. */
        constexpr void Append(char32_t codepoint) noexcept { scalars[count++] = SanitizeScalarValue(codepoint); }

        /** @brief Mark this result as recovered from @p e and append U+FFFD. */
        constexpr void Recover(Error e) noexcept {
            error = e;
            ok = false;
            Append(kReplacementCharacter);
        }
    };

    /** @brief Stateful UTF-16 stream decoder with replacement-character recovery. */
    class Utf16StreamDecoder {
    public:
        /**
         * @brief Consume one UTF-16 code unit.
         * @param[in] codeUnit Input code unit.
         * @return Decoded scalar output plus recovery status.
         */
        [[nodiscard]] constexpr Utf16DecodeResult Push(uint16_t codeUnit) noexcept {
            Utf16DecodeResult out{};
            const char32_t value = static_cast<char32_t>(codeUnit);

            if (IsHighSurrogate(value)) {
                if (pendingHighSurrogate_ != 0) {
                    out.Recover(Error::IsolatedHighSurrogate);
                }
                pendingHighSurrogate_ = codeUnit;
                return out;
            }

            if (IsLowSurrogate(value)) {
                if (pendingHighSurrogate_ == 0) {
                    out.Recover(Error::IsolatedLowSurrogate);
                    return out;
                }
                out.Append(DecodeSurrogatePair(pendingHighSurrogate_, codeUnit));
                pendingHighSurrogate_ = 0;
                return out;
            }

            if (pendingHighSurrogate_ != 0) {
                pendingHighSurrogate_ = 0;
                out.Recover(Error::IsolatedHighSurrogate);
            }
            out.Append(value);
            return out;
        }

        /** @brief Emit U+FFFD for an unterminated high surrogate, if any. */
        [[nodiscard]] constexpr Utf16DecodeResult Flush() noexcept {
            Utf16DecodeResult out{};
            if (pendingHighSurrogate_ != 0) {
                pendingHighSurrogate_ = 0;
                out.Recover(Error::IsolatedHighSurrogate);
            }
            return out;
        }

        /** @brief Clear any pending high surrogate without emission. */
        constexpr void Reset() noexcept { pendingHighSurrogate_ = 0; }

        /** @brief Return true when a high surrogate is waiting for its matching low surrogate. */
        [[nodiscard]] constexpr bool HasPendingHighSurrogate() const noexcept { return pendingHighSurrogate_ != 0; }

    private:
        uint16_t pendingHighSurrogate_ = 0;
    };

    /** @brief Convert a UTF-16 code-unit sequence to UTF-8 with replacement-character recovery. */
    template<Utf16CodeUnit CodeUnit>
    [[nodiscard]] constexpr std::string Utf16ToUtf8(std::basic_string_view<CodeUnit> text) {
        Utf16StreamDecoder decoder{};
        std::string out;
        out.reserve(text.size());

        for (CodeUnit codeUnit : text) {
            const Utf16DecodeResult decoded = decoder.Push(static_cast<uint16_t>(codeUnit));
            for (uint8_t i = 0; i < decoded.count; ++i) {
                AppendUtf8Scalar(out, decoded.scalars[i]);
            }
        }

        const Utf16DecodeResult tail = decoder.Flush();
        for (uint8_t i = 0; i < tail.count; ++i) {
            AppendUtf8Scalar(out, tail.scalars[i]);
        }
        return out;
    }

    /** @brief Convert a UTF-16 span to UTF-8 with replacement-character recovery. */
    template<Utf16CodeUnit CodeUnit>
    [[nodiscard]] constexpr std::string Utf16ToUtf8(std::span<const CodeUnit> text) {
        return Utf16ToUtf8(std::basic_string_view<CodeUnit>{text.data(), text.size()});
    }

    /** @brief Convert a platform wide string to UTF-8 when @c wchar_t is a UTF-16 code unit. */
    template<typename WChar = wchar_t>
        requires std::same_as<WChar, wchar_t> && (sizeof(WChar) == sizeof(uint16_t))
    [[nodiscard]] constexpr std::string WideToUtf8(std::basic_string_view<WChar> text) {
        return Utf16ToUtf8(text);
    }

} // namespace Sora::Unicode

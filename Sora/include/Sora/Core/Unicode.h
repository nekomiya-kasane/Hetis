/**
 * @file Unicode.h
 * @brief Constexpr Unicode scalar validation and UTF-8, UTF-16, UTF-32, and wide-string transcoding.
 * @ingroup Core
 *
 * @details This header stays at the encoding boundary. It does not provide normalization, collation, grapheme
 * segmentation, or locale-sensitive transforms; those require Unicode data tables and belong to a higher layer.
 */
#pragma once

#include <Sora/ErrorCode.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace Sora {

    namespace Unicode {

        /** @brief Compile-time policy applied when a source sequence is malformed. */
        enum class InvalidSequencePolicy : uint8_t {
            Reject,  /**< Stop at the first malformed sequence and return its @ref ErrorCode. */
            Replace, /**< Emit U+FFFD for each malformed subsequence and continue. */
        };

        namespace Concept {

            /** @brief Character or unsigned storage type that semantically represents one UTF-16 code unit. */
            template<typename T>
            concept Utf16CodeUnit =
                std::same_as<std::remove_cv_t<T>, char16_t> || std::same_as<std::remove_cv_t<T>, uint16_t> ||
                (std::same_as<std::remove_cv_t<T>, wchar_t> && sizeof(wchar_t) == sizeof(uint16_t));

        } // namespace Concept

        /** @brief Unicode replacement character U+FFFD. */
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

        /** @brief UTF-8 byte sequence for a single scalar value. */
        struct Utf8Scalar {
            std::array<char, 4> bytes{}; /**< Encoded bytes. */
            uint8_t size = 0;            /**< Number of bytes used in @ref bytes. */

            /** @brief View over the encoded byte sequence. */
            [[nodiscard]] constexpr std::string_view View() const noexcept { return {bytes.data(), size}; }
        };

        /** @brief One Unicode scalar decoded from the beginning of a UTF-8 byte sequence. */
        struct DecodedUtf8Scalar {
            char32_t value = 0; /**< Decoded Unicode scalar value. */
            uint8_t size = 0;   /**< Number of input bytes consumed. */
        };

        /** @brief UTF-16 code-unit sequence for a single scalar value. */
        struct Utf16Scalar {
            std::array<uint16_t, 2> units{}; /**< Encoded UTF-16 code units. */
            uint8_t size = 0;                /**< Number of code units used in @ref units. */
        };

        namespace Detail {

            /** @brief Encode a known Unicode scalar as UTF-8. */
            [[nodiscard]] constexpr Utf8Scalar EncodeUtf8ScalarUnchecked(char32_t codepoint) noexcept {
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

            /** @brief Append a known Unicode scalar as UTF-8. */
            constexpr void AppendUtf8ScalarUnchecked(std::string& out, char32_t codepoint) {
                const Utf8Scalar encoded = EncodeUtf8ScalarUnchecked(codepoint);
                out.append(encoded.bytes.data(), encoded.size);
            }

            /** @brief Encode a known Unicode scalar as UTF-16. */
            [[nodiscard]] constexpr Utf16Scalar EncodeUtf16ScalarUnchecked(char32_t codepoint) noexcept {
                Utf16Scalar out{};
                if (codepoint <= 0xFFFFu) {
                    out.units[0] = static_cast<uint16_t>(codepoint);
                    out.size = 1;
                } else {
                    const char32_t offset = codepoint - 0x10000u;
                    out.units[0] = static_cast<uint16_t>(kHighSurrogateFirst + (offset >> 10u));
                    out.units[1] = static_cast<uint16_t>(kLowSurrogateFirst + (offset & 0x3FFu));
                    out.size = 2;
                }
                return out;
            }

            /** @brief Decode a surrogate pair whose preconditions were already checked. */
            [[nodiscard]] constexpr char32_t DecodeSurrogatePairUnchecked(uint16_t high, uint16_t low) noexcept {
                return 0x10000u + (((static_cast<char32_t>(high) - kHighSurrogateFirst) << 10u) |
                                   (static_cast<char32_t>(low) - kLowSurrogateFirst));
            }

            template<InvalidSequencePolicy Policy, typename Visitor>
            [[nodiscard]] constexpr Result<void> VisitUtf8Scalars(std::string_view text, Visitor&& visitor);

        } // namespace Detail

        /**
         * @brief Encode @p codepoint as UTF-8 under @p Policy.
         * @tparam Policy Reject invalid scalars or replace them with U+FFFD.
         */
        template<InvalidSequencePolicy Policy = InvalidSequencePolicy::Reject>
        [[nodiscard]] constexpr Result<Utf8Scalar> EncodeUtf8Scalar(char32_t codepoint) noexcept {
            if (!IsScalarValue(codepoint)) {
                if constexpr (Policy == InvalidSequencePolicy::Reject) {
                    return std::unexpected(ErrorCode::InvalidScalar);
                } else {
                    codepoint = kReplacementCharacter;
                }
            }
            return Detail::EncodeUtf8ScalarUnchecked(codepoint);
        }

        /**
         * @brief Decode the first Unicode scalar in @p text under @p Policy.
         * @tparam Policy Reject malformed input or emit U+FFFD and report the consumed malformed prefix.
         */
        template<InvalidSequencePolicy Policy = InvalidSequencePolicy::Reject>
        [[nodiscard]] constexpr Result<DecodedUtf8Scalar> DecodeUtf8Scalar(std::string_view text) noexcept {
            if (text.empty()) {
                return std::unexpected(ErrorCode::TruncatedUtf8Sequence);
            }

            const auto invalid = [](ErrorCode error, uint8_t consumed) -> Result<DecodedUtf8Scalar> {
                if constexpr (Policy == InvalidSequencePolicy::Reject) {
                    return std::unexpected(error);
                } else {
                    return DecodedUtf8Scalar{.value = kReplacementCharacter, .size = consumed};
                }
            };

            const uint8_t lead = static_cast<uint8_t>(text.front());
            if (lead <= 0x7Fu) {
                return DecodedUtf8Scalar{.value = lead, .size = 1};
            }

            uint8_t size = 0;
            char32_t value = 0;
            if (lead >= 0xC2u && lead <= 0xDFu) {
                size = 2;
                value = lead & 0x1Fu;
            } else if (lead >= 0xE0u && lead <= 0xEFu) {
                size = 3;
                value = lead & 0x0Fu;
            } else if (lead >= 0xF0u && lead <= 0xF4u) {
                size = 4;
                value = lead & 0x07u;
            } else if (lead == 0xC0u || lead == 0xC1u) {
                return invalid(ErrorCode::OverlongUtf8Sequence, 1);
            } else {
                return invalid(ErrorCode::InvalidUtf8LeadingByte, 1);
            }

            for (uint8_t index = 1; index < size; ++index) {
                if (index == text.size()) {
                    return invalid(ErrorCode::TruncatedUtf8Sequence, index);
                }
                const uint8_t continuation = static_cast<uint8_t>(text[index]);
                if ((continuation & 0xC0u) != 0x80u) {
                    return invalid(ErrorCode::InvalidUtf8Continuation, index);
                }
                if (index == 1) {
                    if ((lead == 0xE0u && continuation < 0xA0u) || (lead == 0xF0u && continuation < 0x90u)) {
                        return invalid(ErrorCode::OverlongUtf8Sequence, 1);
                    }
                    if ((lead == 0xEDu && continuation > 0x9Fu) || (lead == 0xF4u && continuation > 0x8Fu)) {
                        return invalid(ErrorCode::InvalidScalar, 1);
                    }
                }
                value = (value << 6u) | (continuation & 0x3Fu);
            }

            if ((size == 2 && value <= 0x7Fu) || (size == 3 && value <= 0x7FFu) || (size == 4 && value <= 0xFFFFu)) {
                return invalid(ErrorCode::OverlongUtf8Sequence, size);
            }
            if (!IsScalarValue(value)) {
                return invalid(ErrorCode::InvalidScalar, size);
            }
            return DecodedUtf8Scalar{.value = value, .size = size};
        }

        namespace Detail {

            template<InvalidSequencePolicy Policy, typename Visitor>
            [[nodiscard]] constexpr Result<void> VisitUtf8Scalars(std::string_view text, Visitor&& visitor) {
                size_t offset = 0;
                while (offset < text.size()) {
                    auto decoded = DecodeUtf8Scalar<Policy>(text.substr(offset));
                    if (!decoded) {
                        return std::unexpected(decoded.error());
                    }
                    visitor(decoded->value);
                    offset += decoded->size;
                }
                return {};
            }

        } // namespace Detail

        /** @brief Validate every scalar in @p text as strict UTF-8. */
        [[nodiscard]] constexpr Result<void> ValidateUtf8(std::string_view text) noexcept {
            return Detail::VisitUtf8Scalars<InvalidSequencePolicy::Reject>(text, [](char32_t) {});
        }

        /** @brief Copy UTF-8 @c char8_t code units into Sora's canonical @c char byte storage. */
        [[nodiscard]] constexpr std::string Utf8BytesToString(std::u8string_view text) {
            if consteval {
                std::string result;
                result.reserve(text.size());
                for (const char8_t codeUnit : text) {
                    result.push_back(static_cast<char>(codeUnit));
                }
                return result;
            } else {
                return std::string(reinterpret_cast<const char*>(text.data()), text.size());
            }
        }

        /**
         * @brief Encode @p codepoint as UTF-16 under @p Policy.
         * @tparam Policy Reject invalid scalars or replace them with U+FFFD.
         */
        template<InvalidSequencePolicy Policy = InvalidSequencePolicy::Reject>
        [[nodiscard]] constexpr Result<Utf16Scalar> EncodeUtf16Scalar(char32_t codepoint) noexcept {
            if (!IsScalarValue(codepoint)) {
                if constexpr (Policy == InvalidSequencePolicy::Reject) {
                    return std::unexpected(ErrorCode::InvalidScalar);
                } else {
                    codepoint = kReplacementCharacter;
                }
            }
            return Detail::EncodeUtf16ScalarUnchecked(codepoint);
        }

        /** @brief Append one scalar as UTF-8 under @p Policy. */
        template<InvalidSequencePolicy Policy = InvalidSequencePolicy::Reject>
        [[nodiscard]] constexpr Result<void> AppendUtf8Scalar(std::string& out, char32_t codepoint) {
            auto encoded = EncodeUtf8Scalar<Policy>(codepoint);
            if (!encoded) {
                return std::unexpected(encoded.error());
            }
            out.append(encoded->bytes.data(), encoded->size);
            return {};
        }

        template<InvalidSequencePolicy Policy>
        class Utf16StreamDecoder;

        /** @brief Immutable zero-, one-, or two-scalar output produced from one UTF-16 input unit. */
        class Utf16DecodeResult {
        public:
            /** @brief Return the number of decoded scalar values. */
            [[nodiscard]] constexpr size_t Size() const noexcept { return size_; }

            /** @brief Return whether this result contains no scalar values. */
            [[nodiscard]] constexpr bool Empty() const noexcept { return size_ == 0; }

            /** @brief Return decoded scalar @p index. */
            [[nodiscard]] constexpr char32_t operator[](size_t index) const noexcept { return scalars_[index]; }

            /** @brief Return a view over all decoded scalar values. */
            [[nodiscard]] constexpr std::span<const char32_t> Scalars() const noexcept {
                return {scalars_.data(), size_};
            }

        private:
            template<InvalidSequencePolicy>
            friend class Utf16StreamDecoder;

            constexpr void Append(char32_t codepoint) noexcept { scalars_[size_++] = codepoint; }

            std::array<char32_t, 2> scalars_{};
            uint8_t size_ = 0;
        };

        /** @brief Stateful UTF-16 stream decoder parameterized by malformed-sequence policy. */
        template<InvalidSequencePolicy Policy = InvalidSequencePolicy::Reject>
        class Utf16StreamDecoder {
        public:
            /** @brief Consume one UTF-16 code unit. */
            [[nodiscard]] constexpr Result<Utf16DecodeResult> Push(uint16_t codeUnit) noexcept {
                Utf16DecodeResult out{};
                const char32_t value = static_cast<char32_t>(codeUnit);

                if (IsHighSurrogate(value)) {
                    if (pendingHighSurrogate_ != 0) {
                        if constexpr (Policy == InvalidSequencePolicy::Reject) {
                            pendingHighSurrogate_ = 0;
                            return std::unexpected(ErrorCode::IsolatedHighSurrogate);
                        } else {
                            out.Append(kReplacementCharacter);
                        }
                    }
                    pendingHighSurrogate_ = codeUnit;
                    return out;
                }

                if (IsLowSurrogate(value)) {
                    if (pendingHighSurrogate_ == 0) {
                        if constexpr (Policy == InvalidSequencePolicy::Reject) {
                            return std::unexpected(ErrorCode::IsolatedLowSurrogate);
                        } else {
                            out.Append(kReplacementCharacter);
                            return out;
                        }
                    }
                    out.Append(Detail::DecodeSurrogatePairUnchecked(pendingHighSurrogate_, codeUnit));
                    pendingHighSurrogate_ = 0;
                    return out;
                }

                if (pendingHighSurrogate_ != 0) {
                    pendingHighSurrogate_ = 0;
                    if constexpr (Policy == InvalidSequencePolicy::Reject) {
                        return std::unexpected(ErrorCode::IsolatedHighSurrogate);
                    } else {
                        out.Append(kReplacementCharacter);
                    }
                }
                out.Append(value);
                return out;
            }

            /** @brief Finish the stream, rejecting or replacing an unterminated high surrogate. */
            [[nodiscard]] constexpr Result<Utf16DecodeResult> Flush() noexcept {
                Utf16DecodeResult out{};
                if (pendingHighSurrogate_ == 0) {
                    return out;
                }
                pendingHighSurrogate_ = 0;
                if constexpr (Policy == InvalidSequencePolicy::Reject) {
                    return std::unexpected(ErrorCode::IsolatedHighSurrogate);
                } else {
                    out.Append(kReplacementCharacter);
                    return out;
                }
            }

            /** @brief Clear any pending high surrogate without emission. */
            constexpr void Reset() noexcept { pendingHighSurrogate_ = 0; }

            /** @brief Return true when a high surrogate is waiting for its matching low surrogate. */
            [[nodiscard]] constexpr bool HasPendingHighSurrogate() const noexcept { return pendingHighSurrogate_ != 0; }

        private:
            uint16_t pendingHighSurrogate_ = 0;
        };

        /**
         * @brief Append UTF-16 input to @p out as UTF-8 without allocating a second output string.
         * @details On rejection, @p out is restored to its original size.
         */
        template<InvalidSequencePolicy Policy = InvalidSequencePolicy::Reject, Concept::Utf16CodeUnit CodeUnit,
                 size_t Extent>
        [[nodiscard]] constexpr Result<void> AppendUtf16ToUtf8(std::string& out, std::span<CodeUnit, Extent> text) {
            const size_t originalSize = out.size();
            Utf16StreamDecoder<Policy> decoder;
            for (CodeUnit codeUnit : text) {
                auto decoded = decoder.Push(static_cast<uint16_t>(codeUnit));
                if (!decoded) {
                    out.resize(originalSize);
                    return std::unexpected(decoded.error());
                }
                for (char32_t codepoint : decoded->Scalars()) {
                    Detail::AppendUtf8ScalarUnchecked(out, codepoint);
                }
            }

            auto tail = decoder.Flush();
            if (!tail) {
                out.resize(originalSize);
                return std::unexpected(tail.error());
            }
            for (char32_t codepoint : tail->Scalars()) {
                Detail::AppendUtf8ScalarUnchecked(out, codepoint);
            }
            return {};
        }

        /** @brief Append UTF-16 text to @p out as UTF-8. */
        template<InvalidSequencePolicy Policy = InvalidSequencePolicy::Reject>
        [[nodiscard]] constexpr Result<void> AppendUtf16ToUtf8(std::string& out, std::u16string_view text) {
            return AppendUtf16ToUtf8<Policy>(out, std::span<const char16_t>{text.data(), text.size()});
        }

        /** @brief Convert a UTF-16 code-unit span to UTF-8 under @p Policy. */
        template<InvalidSequencePolicy Policy = InvalidSequencePolicy::Reject, Concept::Utf16CodeUnit CodeUnit,
                 size_t Extent>
        [[nodiscard]] constexpr Result<std::string> Utf16ToUtf8(std::span<CodeUnit, Extent> text) {
            std::string out;
            out.reserve(text.size());
            if (auto converted = AppendUtf16ToUtf8<Policy>(out, text); !converted) {
                return std::unexpected(converted.error());
            }
            return out;
        }

        /** @brief Convert UTF-16 text to UTF-8 under @p Policy. */
        template<InvalidSequencePolicy Policy = InvalidSequencePolicy::Reject>
        [[nodiscard]] constexpr Result<std::string> Utf16ToUtf8(std::u16string_view text) {
            return Utf16ToUtf8<Policy>(std::span<const char16_t>{text.data(), text.size()});
        }

        /**
         * @brief Append UTF-8 input to @p out as UTF-16 without allocating a second output string.
         * @details On rejection, @p out is restored to its original size.
         */
        template<InvalidSequencePolicy Policy = InvalidSequencePolicy::Reject>
        [[nodiscard]] constexpr Result<void> AppendUtf8ToUtf16(std::u16string& out, std::string_view text) {
            const size_t originalSize = out.size();
            auto converted = Detail::VisitUtf8Scalars<Policy>(text, [&out](char32_t codepoint) {
                const Utf16Scalar encoded = Detail::EncodeUtf16ScalarUnchecked(codepoint);
                for (uint8_t index = 0; index < encoded.size; ++index) {
                    out.push_back(static_cast<char16_t>(encoded.units[index]));
                }
            });
            if (!converted) {
                out.resize(originalSize);
                return std::unexpected(converted.error());
            }
            return {};
        }

        /** @brief Convert UTF-8 to UTF-16 under @p Policy. */
        template<InvalidSequencePolicy Policy = InvalidSequencePolicy::Reject>
        [[nodiscard]] constexpr Result<std::u16string> Utf8ToUtf16(std::string_view text) {
            std::u16string out;
            out.reserve(text.size());
            if (auto converted = AppendUtf8ToUtf16<Policy>(out, text); !converted) {
                return std::unexpected(converted.error());
            }
            return out;
        }

        /**
         * @brief Append UTF-32 input to @p out as UTF-8 without allocating a second output string.
         * @details On rejection, @p out is restored to its original size.
         */
        template<InvalidSequencePolicy Policy = InvalidSequencePolicy::Reject>
        [[nodiscard]] constexpr Result<void> AppendUtf32ToUtf8(std::string& out, std::u32string_view text) {
            const size_t originalSize = out.size();
            for (char32_t codepoint : text) {
                if (auto appended = AppendUtf8Scalar<Policy>(out, codepoint); !appended) {
                    out.resize(originalSize);
                    return std::unexpected(appended.error());
                }
            }
            return {};
        }

        /** @brief Convert UTF-32 to UTF-8 under @p Policy. */
        template<InvalidSequencePolicy Policy = InvalidSequencePolicy::Reject>
        [[nodiscard]] constexpr Result<std::string> Utf32ToUtf8(std::u32string_view text) {
            std::string out;
            out.reserve(text.size());
            if (auto converted = AppendUtf32ToUtf8<Policy>(out, text); !converted) {
                return std::unexpected(converted.error());
            }
            return out;
        }

        /**
         * @brief Append UTF-8 input to @p out as UTF-32 without allocating a second output string.
         * @details On rejection, @p out is restored to its original size.
         */
        template<InvalidSequencePolicy Policy = InvalidSequencePolicy::Reject>
        [[nodiscard]] constexpr Result<void> AppendUtf8ToUtf32(std::u32string& out, std::string_view text) {
            const size_t originalSize = out.size();
            auto converted =
                Detail::VisitUtf8Scalars<Policy>(text, [&out](char32_t codepoint) { out.push_back(codepoint); });
            if (!converted) {
                out.resize(originalSize);
                return std::unexpected(converted.error());
            }
            return {};
        }

        /** @brief Convert UTF-8 to UTF-32 under @p Policy. */
        template<InvalidSequencePolicy Policy = InvalidSequencePolicy::Reject>
        [[nodiscard]] constexpr Result<std::u32string> Utf8ToUtf32(std::string_view text) {
            std::u32string out;
            out.reserve(text.size());
            if (auto converted = AppendUtf8ToUtf32<Policy>(out, text); !converted) {
                return std::unexpected(converted.error());
            }
            return out;
        }

        /** @brief Append a platform wide string to @p out as UTF-8 under @p Policy. */
        template<InvalidSequencePolicy Policy = InvalidSequencePolicy::Reject>
        [[nodiscard]] constexpr Result<void> AppendWideToUtf8(std::string& out, std::wstring_view text) {
            if constexpr (sizeof(wchar_t) == sizeof(uint16_t)) {
                return AppendUtf16ToUtf8<Policy>(out, std::span<const wchar_t>{text.data(), text.size()});
            } else {
                static_assert(sizeof(wchar_t) == sizeof(char32_t));
                const size_t originalSize = out.size();
                for (wchar_t codepoint : text) {
                    if (auto appended = AppendUtf8Scalar<Policy>(out, static_cast<char32_t>(codepoint)); !appended) {
                        out.resize(originalSize);
                        return std::unexpected(appended.error());
                    }
                }
                return {};
            }
        }

        /** @brief Convert a platform wide string to UTF-8 under @p Policy. */
        template<InvalidSequencePolicy Policy = InvalidSequencePolicy::Reject>
        [[nodiscard]] constexpr Result<std::string> WideToUtf8(std::wstring_view text) {
            std::string out;
            out.reserve(text.size());
            if (auto converted = AppendWideToUtf8<Policy>(out, text); !converted) {
                return std::unexpected(converted.error());
            }
            return out;
        }

        /** @brief Append UTF-8 input directly to a platform wide string under @p Policy. */
        template<InvalidSequencePolicy Policy = InvalidSequencePolicy::Reject>
        [[nodiscard]] constexpr Result<void> AppendUtf8ToWide(std::wstring& out, std::string_view text) {
            const size_t originalSize = out.size();
            auto converted = Detail::VisitUtf8Scalars<Policy>(text, [&out](char32_t codepoint) {
                if constexpr (sizeof(wchar_t) == sizeof(uint16_t)) {
                    const Utf16Scalar encoded = Detail::EncodeUtf16ScalarUnchecked(codepoint);
                    for (uint8_t index = 0; index < encoded.size; ++index) {
                        out.push_back(static_cast<wchar_t>(encoded.units[index]));
                    }
                } else {
                    static_assert(sizeof(wchar_t) == sizeof(char32_t));
                    out.push_back(static_cast<wchar_t>(codepoint));
                }
            });
            if (!converted) {
                out.resize(originalSize);
                return std::unexpected(converted.error());
            }
            return {};
        }

        /** @brief Convert UTF-8 directly to a platform wide string under @p Policy. */
        template<InvalidSequencePolicy Policy = InvalidSequencePolicy::Reject>
        [[nodiscard]] constexpr Result<std::wstring> Utf8ToWide(std::string_view text) {
            std::wstring out;
            out.reserve(text.size());
            if (auto converted = AppendUtf8ToWide<Policy>(out, text); !converted) {
                return std::unexpected(converted.error());
            }
            return out;
        }

    } // namespace Unicode

    namespace Concept {

        /** @brief Character or unsigned storage type that semantically represents one UTF-16 code unit. */
        template<typename T>
        concept Utf16CodeUnit = Unicode::Concept::Utf16CodeUnit<T>;

    } // namespace Concept

} // namespace Sora

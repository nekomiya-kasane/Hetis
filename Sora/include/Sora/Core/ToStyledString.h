/**
 * @file ToStyledString.h
 * @brief Tapioca-backed rich string conversion for diagnostics and terminal output.
 * @ingroup Core
 *
 * @details Provides @ref Sora::ToStyledString, a styled counterpart to @ref Sora::ToString. The object model and
 * dispatch order intentionally mirror @c ToString: explicit hooks and ADL have priority, then built-in scalar cases,
 * enums, ranges, tuple-like values, variants, pointers, and reflection-based class member dumps. Styling is generated
 * only through tapioca's @c style and @c ansi_emitter types; user text is escaped before it is appended so hostile or
 * accidental terminal control sequences cannot be injected through data values.
 */
#pragma once

#include "Sora/Core/ToString.h"
#include "Sora/Core/Traits.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <meta>
#include <ostream>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include <tapioca/ansi_emitter.h>
#include <tapioca/style.h>
#include <tapioca/terminal.h>

namespace Sora {

    /** @brief Rendering policy used by automatic formatting integration. */
    enum class RenderMode : uint8_t {
        Plain,       /**< Use @ref ToString. */
        Styled,      /**< Use @ref ToStyledString. */
        Placeholder, /**< Use a shallow placeholder in the form @c <TypeName @c at @c address>. */
    };

    /** @brief Default automatic formatting policy used by @c std::format and @c std::println. */
    inline constexpr RenderMode kDefaultRenderMode = RenderMode::Styled;

    namespace Styled {

        /** @brief Escaping mode used when appending data-owned text to styled output. */
        enum class StyledEscapePolicy : uint8_t {
            /** @brief Append text unchanged. Use only for trusted text that is not interpreted by a terminal. */
            None,

            /** @brief Render terminal control bytes and common escapes as visible ASCII sequences. */
            TerminalSafe,
        };

        /** @brief Semantic style role used by the default styled string theme. */
        enum class StyledRole : uint8_t {
            Plain,
            TypeName,
            FieldName,
            EnumName,
            Number,
            String,
            Boolean,
            Null,
            Punctuation,
            Address,
            Escape,
            Error,
        };

        /** @brief Color theme used by @ref StyledStringBuilder. */
        struct StyledStringTheme {
            /** @brief Return the tapioca style used for semantic role @p role. */
            [[nodiscard]] constexpr tapioca::style operator[](StyledRole role) const noexcept {
                using namespace tapioca;
                switch (role) {
                    case StyledRole::Plain:
                        return {};
                    case StyledRole::TypeName:
                        return {.fg = colors::bright_cyan, .attrs = attr::bold};
                    case StyledRole::FieldName:
                        return {.fg = colors::bright_blue};
                    case StyledRole::EnumName:
                        return {.fg = colors::bright_magenta, .attrs = attr::bold};
                    case StyledRole::Number:
                        return {.fg = colors::yellow};
                    case StyledRole::String:
                        return {.fg = colors::green};
                    case StyledRole::Boolean:
                        return {.fg = colors::magenta, .attrs = attr::bold};
                    case StyledRole::Null:
                        return {.fg = colors::bright_black, .attrs = attr::dim | attr::italic};
                    case StyledRole::Punctuation:
                        return {.fg = colors::bright_yellow};
                    case StyledRole::Address:
                        return {.fg = colors::bright_black, .attrs = attr::dim};
                    case StyledRole::Escape:
                        return {.fg = colors::bright_yellow, .attrs = attr::dim};
                    case StyledRole::Error:
                        return {.fg = colors::bright_red, .attrs = attr::bold};
                }
                return {};
            }
        };

        /** @brief Runtime options controlling styled string emission. */
        struct StyledStringOptions {
            /** @brief Enable ANSI style emission. When false, only escaped plain text is emitted. */
            bool color = true;

            /** @brief Escape user-owned text before appending it to the output string. */
            StyledEscapePolicy escapePolicy = StyledEscapePolicy::TerminalSafe;

            /** @brief Terminal capability model used by tapioca when lowering styles to ANSI SGR sequences. */
            tapioca::terminal_caps caps = tapioca::terminal_caps::modern();

            /** @brief Theme mapping semantic roles to tapioca styles. */
            StyledStringTheme theme{};

            /** @brief Text attributes composited over every semantic style emitted by the builder. */
            tapioca::attr attributes = tapioca::attr::none;
        };

        /** @brief Stateful builder for ANSI-rich strings generated through tapioca. */
        class StyledStringBuilder {
        public:
            /** @brief Construct a builder with explicit options. */
            explicit StyledStringBuilder(StyledStringOptions options = {})
                : options_(options), emitter_(options.caps) {}

            /** @brief Append trusted structural text without escaping. */
            constexpr void Raw(std::string_view text) { out_.append(text); }

            /** @brief Append trusted structural text under semantic role @p role. */
            void Raw(StyledRole role, std::string_view text) { AppendStyled(role, text); }

            /** @brief Append data-owned text under semantic role @p role, escaping it according to current options. */
            void Text(StyledRole role, std::string_view text) {
                if (options_.escapePolicy == StyledEscapePolicy::None) {
                    AppendStyled(role, text);
                    return;
                }
                for (unsigned char ch : text) {
                    switch (ch) {
                        case '\\':
                            AppendStyled(StyledRole::Escape, "\\\\");
                            break;
                        case '\n':
                            AppendStyled(StyledRole::Escape, "\\n");
                            break;
                        case '\r':
                            AppendStyled(StyledRole::Escape, "\\r");
                            break;
                        case '\t':
                            AppendStyled(StyledRole::Escape, "\\t");
                            break;
                        case '\033':
                            AppendHexEscape(ch);
                            break;
                        default:
                            if (ch < 0x20 || ch == 0x7F) {
                                AppendHexEscape(ch);
                            } else {
                                char c = static_cast<char>(ch);
                                AppendStyled(role, std::string_view{&c, 1});
                            }
                            break;
                    }
                }
            }

            /** @brief Append a quoted string literal with escaped contents. */
            void QuotedString(std::string_view text) {
                Raw(StyledRole::Punctuation, "\"");
                Text(StyledRole::String, text);
                Raw(StyledRole::Punctuation, "\"");
            }

            /** @brief Return the accumulated rich string and reset terminal style to default. */
            [[nodiscard]] std::string Finish() && {
                Reset();
                return std::move(out_);
            }

            /** @brief Access the accumulated rich string without changing the builder. */
            [[nodiscard]] constexpr const std::string& String() const noexcept { return out_; }

            /** @brief Access active styled string options. */
            [[nodiscard]] constexpr const StyledStringOptions& Options() const noexcept { return options_; }

        private:
            void AppendStyled(StyledRole role, std::string_view text) {
                if (options_.color) {
                    auto style = options_.theme[role];
                    style.attrs |= options_.attributes;
                    emitter_.transition(style, out_);
                }
                out_.append(text);
            }

            void AppendHexEscape(unsigned char ch) {
                char buf[5];
                auto n = std::format_to_n(buf, sizeof(buf), "\\x{:02X}", static_cast<unsigned>(ch));
                AppendStyled(StyledRole::Escape, std::string_view{buf, static_cast<size_t>(n.size)});
            }

            void Reset() {
                if (options_.color) {
                    emitter_.reset(out_);
                }
            }

            StyledStringOptions options_{};
            tapioca::ansi_emitter emitter_{};
            std::string out_{};
        };

    } // namespace Styled

    namespace Hook {

        /**
         * @brief Open customisation point for @ref ToStyledString.
         *
         * @details Specialise this hook when a type needs styled output that cannot be derived from its structural
         * representation. A specialisation provides @c static @c void @c Render(StyledStringBuilder&, @c const @c T&).
         * The primary template is intentionally undefined; lack of a specialisation means dispatch falls through.
         *
         * @tparam T Cv-unqualified type to customise.
         */
        template<typename T>
        struct ToStyledStringHook;

    } // namespace Hook

    /** @cond INTERNAL */
    namespace Detail {

        /** @name ADL hook detection @{ */
        namespace ADL {

            namespace FreeToStyledString {
                /** @brief Poison pill that keeps ordinary lookup from finding a fallback overload. */
                void ToStyledString() = delete;

                /** @brief True if @c ToStyledString(builder, value) is found via ADL. */
                template<typename T>
                concept Available = requires(Styled::StyledStringBuilder& builder, T&& value) {
                    { ToStyledString(builder, std::forward<T>(value)) } -> std::same_as<void>;
                };

                template<typename T>
                    requires Available<T>
                void Invoke(Styled::StyledStringBuilder& builder, T&& value) {
                    ToStyledString(builder, std::forward<T>(value));
                }
            } // namespace FreeToStyledString

        } // namespace ADL
        /** @} */

        /** @brief Detects a member @c value.ToStyledString(builder). */
        template<typename T>
        concept HasMemberToStyledString = requires(T&& value, Styled::StyledStringBuilder& builder) {
            { std::forward<T>(value).ToStyledString(builder) } -> std::same_as<void>;
        };

        /** @brief Detects a usable @ref Sora::Hook::ToStyledStringHook specialisation for @p T. */
        template<typename T>
        concept HasHookToStyledString = requires(const T& value, Styled::StyledStringBuilder& builder) {
            { Hook::ToStyledStringHook<T>::Render(builder, value) } -> std::same_as<void>;
        };

    } // namespace Detail
    /** @endcond */

    namespace Concept {

        /** @brief Type explicitly customizing styled conversion through a hook, ADL, or member function. */
        template<typename T>
        concept CustomStyledStringFormattable =
            Detail::HasHookToStyledString<std::remove_cvref_t<T>> || Detail::ADL::FreeToStyledString::Available<T> ||
            Detail::HasMemberToStyledString<T>;

    } // namespace Concept

    /** @cond INTERNAL */
    namespace Detail {

        /** @brief Core dispatch from a value to styled terminal text. */
        template<typename T>
        void ToStyledStringImpl(Styled::StyledStringBuilder& builder, T&& value) {
            using U = std::remove_cvref_t<T>;
            namespace $$ = Styled;

            if constexpr (HasHookToStyledString<U>) {
                Hook::ToStyledStringHook<U>::Render(builder, value);
            } else if constexpr (ADL::FreeToStyledString::Available<T>) {
                ADL::FreeToStyledString::Invoke(builder, std::forward<T>(value));
            } else if constexpr (HasMemberToStyledString<T>) {
                std::forward<T>(value).ToStyledString(builder);
            } else if constexpr (Concept::CustomStringFormattable<U>) {
                builder.Text($$::StyledRole::Plain, ToString(std::forward<T>(value)));
            } else if constexpr (std::is_null_pointer_v<U>) {
                builder.Text($$::StyledRole::Null, "nullptr");
            } else if constexpr (std::same_as<U, bool>) {
                builder.Text($$::StyledRole::Boolean, value ? "true" : "false");
            } else if constexpr (std::same_as<U, char>) {
                char text[] = {value, '\0'};
                builder.QuotedString(std::string_view{text, 1});
            } else if constexpr (Concept::NarrowStringLike<T> || Concept::Utf8StringLike<T> ||
                                 Concept::Utf16StringLike<T> || Concept::Utf32StringLike<T> ||
                                 Concept::WideStringLike<T> || std::same_as<U, std::filesystem::path>) {
                builder.QuotedString(ToString(std::forward<T>(value)));
            } else if constexpr (std::is_enum_v<U>) {
                builder.Text($$::StyledRole::EnumName, Traits::EnumToString(value));
            } else if constexpr (std::ranges::range<U>) {
                builder.Raw($$::StyledRole::Punctuation, "[");
                bool first = true;
                for (auto&& item : value) {
                    if (!first) {
                        builder.Raw($$::StyledRole::Punctuation, ", ");
                    }
                    first = false;
                    ToStyledStringImpl(builder, item);
                }
                builder.Raw($$::StyledRole::Punctuation, "]");
            } else if constexpr (Concept::TupleLikeClass<U>) {
                builder.Raw($$::StyledRole::Punctuation, "(");
                bool first = true;
                std::apply(
                    [&]<typename... Ts>(Ts&&... args) {
                        ((first ? void(first = false) : builder.Raw($$::StyledRole::Punctuation, ", "),
                          ToStyledStringImpl(builder, std::forward<Ts>(args))),
                         ...);
                    },
                    std::forward<T>(value));
                builder.Raw($$::StyledRole::Punctuation, ")");
            } else if constexpr (Concept::VariantLikeClass<U>) {
                if (value.valueless_by_exception()) {
                    builder.Text($$::StyledRole::TypeName, Traits::TypeName<U>);
                    builder.Raw($$::StyledRole::Punctuation, "(");
                    builder.Text($$::StyledRole::Error, "valueless");
                    builder.Raw($$::StyledRole::Punctuation, ")");
                } else {
                    std::visit(
                        [&builder](auto&& alternative) {
                            ToStyledStringImpl(builder, std::forward<decltype(alternative)>(alternative));
                        },
                        std::forward<T>(value));
                }
            } else if constexpr (std::is_pointer_v<U>) {
                if (value == nullptr) {
                    builder.Text($$::StyledRole::TypeName, Traits::TypeName<std::remove_pointer_t<U>>);
                    builder.Raw($$::StyledRole::Punctuation, "*(");
                    builder.Text($$::StyledRole::Null, "nullptr");
                    builder.Raw($$::StyledRole::Punctuation, ")");
                } else if constexpr (std::is_object_v<std::remove_pointer_t<U>>) {
                    builder.Text($$::StyledRole::TypeName, Traits::TypeName<std::remove_pointer_t<U>>);
                    builder.Raw($$::StyledRole::Punctuation, "*(");
                    builder.Text($$::StyledRole::Address, std::format("{:p}", static_cast<const void*>(value)));
                    builder.Raw($$::StyledRole::Punctuation, ")");
                } else {
                    builder.Text($$::StyledRole::TypeName, Traits::TypeName<std::remove_pointer_t<U>>);
                    builder.Raw($$::StyledRole::Punctuation, "*(function)");
                }
            } else if constexpr (std::is_arithmetic_v<U>) {
                builder.Text($$::StyledRole::Number, std::format("{}", value));
            } else if constexpr (!Concept::AutoDisplayable<U> &&
                                 requires(std::ostream& os, T&& v) { os << std::forward<T>(v); }) {
                std::stringstream ss;
                ss << std::forward<T>(value);
                builder.Text($$::StyledRole::Plain, ss.str());
            } else if constexpr (std::is_class_v<U> && !std::is_union_v<U> && requires { sizeof(U); }) {
                builder.Text($$::StyledRole::TypeName, Traits::TypeName<U>);
                builder.Raw($$::StyledRole::Punctuation, "{");

                template for (bool first = true; constexpr auto m : Traits::DataMembers<U>) {
                    if constexpr (!$::Has<$::Serialization::Ignore>(m)) {
                        if (!first) {
                            builder.Raw($$::StyledRole::Punctuation, ", ");
                        }
                        first = false;

                        builder.Text($$::StyledRole::FieldName, Meta::SerializationFieldNameOf<m>());
                        builder.Raw($$::StyledRole::Punctuation, "=");

                        if constexpr (std::meta::is_bit_field(m)) {
                            ToStyledStringImpl(builder, auto(value.[:m:]));
                        } else if constexpr (std::is_pointer_v<typename [:std::meta::type_of(m):]> &&
                                             $::Has<$::Serialization::DerefPrint>(m)) {
                            auto* ptr = value.[:m:];
                            if (ptr == nullptr) {
                                builder.Text($$::StyledRole::Null, "nullptr");
                            } else {
                                ToStyledStringImpl(builder, *ptr);
                            }
                        } else {
                            ToStyledStringImpl(builder, value.[:m:]);
                        }
                    }
                }

                builder.Raw($$::StyledRole::Punctuation, "}");
            } else if constexpr (std::is_object_v<U>) {
                builder.Raw($$::StyledRole::Punctuation, "<");
                builder.Text($$::StyledRole::TypeName, Traits::TypeName<U>);
                builder.Raw($$::StyledRole::Plain, " at ");
                builder.Text($$::StyledRole::Address,
                             std::format("{:p}", static_cast<const void*>(std::addressof(value))));
                builder.Raw($$::StyledRole::Punctuation, ">");
            } else {
                builder.Text($$::StyledRole::TypeName, Traits::TypeName<U>);
            }
        }

    } // namespace Detail
    /** @endcond */

    namespace CPO {

        /** @brief Function object implementing the @ref Sora::ToStyledString customization point. */
        struct ToStyledStringFn {
            /** @brief Convert @p value to a styled string with default options. */
            template<typename T>
            [[nodiscard]] std::string operator()(T&& value) const {
                return (*this)(std::forward<T>(value), Styled::StyledStringOptions{});
            }

            /** @brief Convert @p value to a styled string with explicit options. */
            template<typename T>
            [[nodiscard]] std::string operator()(T&& value, Styled::StyledStringOptions options) const {
                Styled::StyledStringBuilder builder{options};
                Detail::ToStyledStringImpl(builder, std::forward<T>(value));
                return std::move(builder).Finish();
            }
        };

    } // namespace CPO

    /** @brief Customisation-point object that converts a supported value to tapioca-styled terminal text. */
    inline constexpr CPO::ToStyledStringFn ToStyledString{};

} // namespace Sora

/**
 * @brief Auto-specialise @c std::formatter for Sora-displayable types.
 *
 * @details Empty format specifiers use @ref Sora::kDefaultRenderMode. @c {:s} forces @ref Sora::ToStyledString,
 * @c {:p} forces @ref Sora::ToString, and @c {:?} emits a shallow placeholder. Styled mode additionally accepts the
 * composable flags @c h (highlight/bold), @c i (italic), @c u (underline), and @c r (reverse), for example @c {:shiu}.
 */
template<typename T>
    requires Sora::Concept::AutoDisplayable<T>
struct std::formatter<T> {
    Sora::RenderMode mode = Sora::kDefaultRenderMode;
    tapioca::attr attributes = tapioca::attr::none;

    constexpr auto parse(std::format_parse_context& ctx) {
        auto it = ctx.begin();
        if (it == ctx.end() || *it == '}') {
            return it;
        }

        if (*it == 's' || *it == 'p' || *it == '?') {
            switch (*it) {
                case 's':
                    mode = Sora::RenderMode::Styled;
                    break;
                case 'p':
                    mode = Sora::RenderMode::Plain;
                    break;
                case '?':
                    mode = Sora::RenderMode::Placeholder;
                    break;
            }
            ++it;
        }

        while (it != ctx.end() && *it != '}') {
            switch (*it) {
                case 'h':
                    attributes |= tapioca::attr::bold;
                    break;
                case 'i':
                    attributes |= tapioca::attr::italic;
                    break;
                case 'u':
                    attributes |= tapioca::attr::underline;
                    break;
                case 'r':
                    attributes |= tapioca::attr::reverse;
                    break;
                default:
                    throw std::format_error("invalid Sora display attribute flag");
            }
            ++it;
        }
        if (attributes != tapioca::attr::none && mode != Sora::RenderMode::Styled) {
            throw std::format_error("Sora display attribute flags require styled mode");
        }
        return it;
    }

    auto format(const T& value, std::format_context& ctx) const {
        switch (mode) {
            case Sora::RenderMode::Plain:
                return std::format_to(ctx.out(), "{}", Sora::ToString(value));
            case Sora::RenderMode::Styled:
                return std::format_to(ctx.out(), "{}", Sora::ToStyledString(value, {.attributes = attributes}));
            case Sora::RenderMode::Placeholder:
                return std::format_to(ctx.out(), "<{} at {:p}>", Sora::Traits::TypeName<T>,
                                      static_cast<const void*>(std::addressof(value)));
        }
        return std::format_to(ctx.out(), "<{} at {:p}>", Sora::Traits::TypeName<T>,
                              static_cast<const void*>(std::addressof(value)));
    }
};

/**
 * @brief Insert Sora-displayable values into an output stream using @ref Sora::kDefaultRenderMode.
 */
template<typename T>
    requires Sora::Concept::AutoDisplayable<T>
std::ostream& operator<<(std::ostream& os, const T& value) {
    switch (Sora::kDefaultRenderMode) {
        case Sora::RenderMode::Plain:
            return os << Sora::ToString(value);
        case Sora::RenderMode::Styled:
            return os << Sora::ToStyledString(value);
        default:
            return os << std::format("<{} at {:p}>", Sora::Traits::TypeName<T>,
                                     static_cast<const void*>(std::addressof(value)));
    }
}

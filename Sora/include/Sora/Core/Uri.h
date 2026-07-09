/**
 * @file Uri.h
 * @brief RFC 3986-oriented URI parsing, query views, normalization, hashing, and NTTP carriers.
 * @ingroup Core
 *
 * @details This header implements a strict ASCII URI layer based on RFC 3986 generic syntax. It is deliberately not a
 * browser WHATWG URL implementation: no IDNA processing, no special-scheme path state machine, and no implicit
 * percent-decoding during identity comparison. Query parameters are exposed as zero-allocation views over the raw query
 * component, with optional explicit percent decoding for callers that need form-style values.
 */
#pragma once

#include "Sora/Core/Traits/EnumTraits.h"
#include <Sora/Core/FixedString.h>
#include <Sora/Core/Hash.h>
#include <Sora/Core/StringUtils.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <meta>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>

namespace Sora {

    /** @brief URI parse and validation failure reason. */
    enum class UriParseError : uint8_t {
        Ok [[= Sora::$::Description{"URI parsed successfully."}]],
        Empty [[= Sora::$::Description{"Input is empty."}]],
        MissingScheme [[= Sora::$::Description{"Absolute URI requires a non-empty scheme and ':'."}]],
        BadSchemeStart [[= Sora::$::Description{"Scheme does not start with an ASCII letter."}]],
        BadSchemeCharacter
        [[= Sora::$::Description{"Scheme contains a character outside ALPHA / DIGIT / '+' / '-' / '.'."}]],
        BadCharacter
        [[= Sora::$::Description{"URI contains a control, space, DEL, non-ASCII byte, or disallowed delimiter."}]],
        BadPercentEncoding [[= Sora::$::Description{"A '%' is not followed by two hexadecimal digits."}]],
        BadAuthority
        [[= Sora::$::Description{"Authority is present but malformed for the generic parser's constraints."}]],
        BadPath [[= Sora::$::Description{"Path violates RFC 3986 path grammar for the parsed hierarchy form."}]],
    };

    /** @brief Return a stable diagnostic string for @p error. */
    [[nodiscard]] constexpr std::string_view UriParseErrorMessage(UriParseError error) noexcept {
        switch (error) {
        case UriParseError::Ok:
            return "ok";
        case UriParseError::Empty:
            return "empty URI";
        case UriParseError::MissingScheme:
            return "absolute URI requires a scheme followed by ':'";
        case UriParseError::BadSchemeStart:
            return "URI scheme must start with an ASCII letter";
        case UriParseError::BadSchemeCharacter:
            return "URI scheme contains a character outside ALPHA / DIGIT / '+' / '-' / '.'";
        case UriParseError::BadCharacter:
            return "URI contains a byte outside RFC 3986 generic syntax";
        case UriParseError::BadPercentEncoding:
            return "URI percent encoding must be '%' followed by two hex digits";
        case UriParseError::BadAuthority:
            return "URI authority is malformed";
        case UriParseError::BadPath:
            return "URI path is not valid for its hierarchy form";
        }
        return "unknown URI parse error";
    }

    /** @brief Parsed RFC 3986 URI components as views into the original URI text.
     *
     * Example: in @c "https://user@example.com:443/docs/index.html?lang=en#intro",
     * @ref scheme is @c "https", @ref authority is @c "user@example.com:443",
     * @ref path is @c "/docs/index.html", @ref query is @c "lang=en", and
     * @ref fragment (anchor) is @c "intro".
     */
    struct UriParts {
        std::string_view scheme{};    /**< Scheme before @c :. */
        std::string_view authority{}; /**< Authority after @c //, if present. */
        std::string_view path{};      /**< Path component. */
        std::string_view query{};     /**< Query component after @c ?, if present. */
        std::string_view fragment{};  /**< Fragment component after @c #, if present. */
        bool hasAuthority = false;    /**< Whether @ref authority is syntactically present. */
        bool hasQuery = false;        /**< Whether @ref query is syntactically present. */
        bool hasFragment = false;     /**< Whether @ref fragment is syntactically present. */
    };

    /** @brief One raw query parameter split from a URI query component. */
    struct UriQueryParam {
        std::string_view name{};  /**< Raw parameter name. */
        std::string_view value{}; /**< Raw parameter value, empty when missing or explicitly empty. */
        bool hasEquals = false;   /**< Whether the source parameter contained an @c = delimiter. */
    };

    namespace Detail::Uri {

        /** @brief Cursor over an immutable URI string; parser primitives advance by slicing views. */
        struct Cursor {
            std::string_view text{}; /**< Complete source text. */
            size_t offset = 0;       /**< Current parse offset. */

            /** @brief Return true when the cursor has consumed the whole source. */
            [[nodiscard]] constexpr bool Done() const noexcept { return offset >= text.size(); }

            /** @brief Return the unconsumed suffix. */
            [[nodiscard]] constexpr std::string_view Remaining() const noexcept { return text.substr(offset); }

            /** @brief Return true when the unconsumed suffix starts with @p prefix. */
            [[nodiscard]] constexpr bool StartsWith(std::string_view prefix) const noexcept {
                return Remaining().starts_with(prefix);
            }

            /** @brief Consume @p token when it appears at the cursor. */
            constexpr bool Consume(std::string_view token) noexcept {
                if (!StartsWith(token)) {
                    return false;
                }
                offset += token.size();
                return true;
            }

            /** @brief Consume @p token when it appears at the cursor. */
            constexpr bool Consume(char token) noexcept {
                if (Done() || text[offset] != token) {
                    return false;
                }
                ++offset;
                return true;
            }

            /** @brief Return the source slice up to one of @p delimiters, leaving the delimiter unconsumed. */
            [[nodiscard]] constexpr std::string_view TakeUntilAny(std::string_view delimiters) noexcept {
                const size_t begin = offset;
                const size_t end = text.find_first_of(delimiters, offset);
                offset = end == std::string_view::npos ? text.size() : end;
                return text.substr(begin, offset - begin);
            }

            /** @brief Return the remaining source slice and move the cursor to EOF. */
            [[nodiscard]] constexpr std::string_view TakeRest() noexcept {
                const size_t begin = offset;
                offset = text.size();
                return text.substr(begin);
            }
        };

        /** @brief Optional URI component returned by parsers for authority, query, and fragment. */
        struct OptionalComponent {
            std::string_view text{}; /**< Component bytes without their syntax marker. */
            bool present = false;    /**< Whether the syntax marker for this component was present. */
        };

        [[nodiscard]] constexpr bool IsAsciiUriByte(char c) noexcept {
            const auto u = static_cast<unsigned char>(c);
            return u > 0x20u && u < 0x7Fu;
        }

        [[nodiscard]] constexpr bool IsUnreserved(char c) noexcept {
            return Ascii::IsAlpha(c) || Ascii::IsDigit(c) || c == '-' || c == '.' || c == '_' || c == '~';
        }

        [[nodiscard]] constexpr bool IsSubDelimiter(char c) noexcept {
            switch (c) {
            case '!':
            case '$':
            case '&':
            case '\'':
            case '(':
            case ')':
            case '*':
            case '+':
            case ',':
            case ';':
            case '=':
                return true;
            default:
                return false;
            }
        }

        [[nodiscard]] constexpr bool IsSchemeCharacter(char c) noexcept {
            return Ascii::IsAlpha(c) || Ascii::IsDigit(c) || c == '+' || c == '-' || c == '.';
        }

        [[nodiscard]] constexpr bool IsPcharPlain(char c) noexcept {
            return IsUnreserved(c) || IsSubDelimiter(c) || c == ':' || c == '@';
        }

        [[nodiscard]] constexpr bool IsRegNamePlain(char c) noexcept {
            return IsUnreserved(c) || IsSubDelimiter(c);
        }

        [[nodiscard]] constexpr bool IsQueryPlain(char c) noexcept {
            return IsPcharPlain(c) || c == '/' || c == '?';
        }

        [[nodiscard]] constexpr bool IsFragmentPlain(char c) noexcept {
            return IsQueryPlain(c);
        }

        [[nodiscard]] constexpr bool IsPathPlain(char c) noexcept {
            return IsPcharPlain(c) || c == '/';
        }

        template<auto Plain>
        [[nodiscard]] constexpr UriParseError ValidateEncodedRange(std::string_view text) noexcept {
            for (size_t i = 0; i < text.size(); ++i) {
                const char c = text[i];
                if (!IsAsciiUriByte(c)) {
                    return UriParseError::BadCharacter;
                }
                if (c == '%') {
                    if (i + 2 >= text.size() || !Ascii::IsHexDigit(text[i + 1]) || !Ascii::IsHexDigit(text[i + 2])) {
                        return UriParseError::BadPercentEncoding;
                    }
                    i += 2;
                } else if (!Plain(c)) {
                    return UriParseError::BadCharacter;
                }
            }
            return UriParseError::Ok;
        }

        template<auto Plain>
        [[nodiscard]] constexpr bool IsValidEncodedRange(std::string_view text) noexcept {
            return ValidateEncodedRange<Plain>(text) == UriParseError::Ok;
        }

        [[nodiscard]] constexpr UriParseError ValidateAuthority(std::string_view authority) noexcept {
            for (size_t i = 0; i < authority.size(); ++i) {
                const char c = authority[i];
                if (!IsAsciiUriByte(c)) {
                    return UriParseError::BadCharacter;
                }
                if (c == '%') {
                    if (i + 2 >= authority.size() || !Ascii::IsHexDigit(authority[i + 1]) ||
                        !Ascii::IsHexDigit(authority[i + 2])) {
                        return UriParseError::BadPercentEncoding;
                    }
                    i += 2;
                } else if (!(IsRegNamePlain(c) || c == ':' || c == '@' || c == '[' || c == ']')) {
                    return UriParseError::BadAuthority;
                }
            }
            return UriParseError::Ok;
        }

        [[nodiscard]] constexpr bool IsValidAuthority(std::string_view authority) noexcept {
            return ValidateAuthority(authority) == UriParseError::Ok;
        }

        /** @brief Parse `scheme:` and leave the cursor immediately after the colon. */
        [[nodiscard]] constexpr auto ParseScheme(Cursor& cursor) noexcept
            -> std::expected<std::string_view, UriParseError> {
            const std::string_view scheme = cursor.TakeUntilAny(":");
            if (!cursor.Consume(':') || scheme.empty()) {
                return std::unexpected(UriParseError::MissingScheme);
            }
            if (!Ascii::IsAlpha(scheme.front())) {
                return std::unexpected(UriParseError::BadSchemeStart);
            }
            if (std::ranges::any_of(scheme | std::views::drop(1), std::not_fn(IsSchemeCharacter))) {
                return std::unexpected(UriParseError::BadSchemeCharacter);
            }
            return scheme;
        }

        /** @brief Parse optional `//authority` and leave the cursor at path, query, fragment, or EOF. */
        [[nodiscard]] constexpr auto ParseAuthority(Cursor& cursor) noexcept
            -> std::expected<OptionalComponent, UriParseError> {
            if (!cursor.Consume("//")) {
                return OptionalComponent{};
            }
            const std::string_view authority = cursor.TakeUntilAny("/?#");
            if (const auto error = ValidateAuthority(authority); error != UriParseError::Ok) {
                return std::unexpected(error);
            }
            return OptionalComponent{.text = authority, .present = true};
        }

        /** @brief Parse the path component and enforce RFC 3986 generic hierarchy constraints. */
        [[nodiscard]] constexpr auto ParsePath(Cursor& cursor, bool hasAuthority) noexcept
            -> std::expected<std::string_view, UriParseError> {
            const std::string_view path = cursor.TakeUntilAny("?#");
            if (hasAuthority && !path.empty() && path.front() != '/') {
                return std::unexpected(UriParseError::BadPath);
            }
            if (!hasAuthority && path.starts_with("//")) {
                return std::unexpected(UriParseError::BadPath);
            }
            if (const auto error = ValidateEncodedRange<IsPathPlain>(path); error != UriParseError::Ok) {
                return std::unexpected(error);
            }
            return path;
        }

        /** @brief Parse optional `?query` and leave the cursor at fragment or EOF. */
        [[nodiscard]] constexpr auto ParseQuery(Cursor& cursor) noexcept
            -> std::expected<OptionalComponent, UriParseError> {
            if (!cursor.Consume('?')) {
                return OptionalComponent{};
            }
            const std::string_view query = cursor.TakeUntilAny("#");
            if (const auto error = ValidateEncodedRange<IsQueryPlain>(query); error != UriParseError::Ok) {
                return std::unexpected(error);
            }
            return OptionalComponent{.text = query, .present = true};
        }

        /** @brief Parse optional `#fragment` and consume the remaining URI text. */
        [[nodiscard]] constexpr auto ParseFragment(Cursor& cursor) noexcept
            -> std::expected<OptionalComponent, UriParseError> {
            if (!cursor.Consume('#')) {
                return OptionalComponent{};
            }
            const std::string_view fragment = cursor.TakeRest();
            if (const auto error = ValidateEncodedRange<IsFragmentPlain>(fragment); error != UriParseError::Ok) {
                return std::unexpected(error);
            }
            return OptionalComponent{.text = fragment, .present = true};
        }

    } // namespace Detail::Uri

    /** @brief Return true when @p c is an RFC 3986 unreserved character. */
    [[nodiscard]] constexpr bool IsUriUnreserved(char c) noexcept {
        return Detail::Uri::IsUnreserved(c);
    }

    /** @brief Return true when @p c is an RFC 3986 sub-delimiter. */
    [[nodiscard]] constexpr bool IsUriSubDelimiter(char c) noexcept {
        return Detail::Uri::IsSubDelimiter(c);
    }

    /** @brief Return true when @p c can appear literally in an RFC 3986 query component. */
    [[nodiscard]] constexpr bool IsUriQueryCharacter(char c) noexcept {
        return Detail::Uri::IsQueryPlain(c);
    }

    /** @brief Return true when @p c can appear literally in an RFC 3986 fragment/anchor component. */
    [[nodiscard]] constexpr bool IsUriAnchorCharacter(char c) noexcept {
        return Detail::Uri::IsFragmentPlain(c);
    }

    /** @brief Parse an absolute RFC 3986 URI into component views. */
    [[nodiscard]] constexpr auto ParseUri(std::string_view text) noexcept -> std::expected<UriParts, UriParseError> {
        if (text.empty()) {
            return std::unexpected(UriParseError::Empty);
        }

        Detail::Uri::Cursor cursor{.text = text};
        UriParts parts{};

        auto scheme = Detail::Uri::ParseScheme(cursor);
        if (!scheme) {
            return std::unexpected(scheme.error());
        }
        parts.scheme = *scheme;

        auto authority = Detail::Uri::ParseAuthority(cursor);
        if (!authority) {
            return std::unexpected(authority.error());
        }
        parts.authority = authority->text;
        parts.hasAuthority = authority->present;

        auto path = Detail::Uri::ParsePath(cursor, parts.hasAuthority);
        if (!path) {
            return std::unexpected(path.error());
        }
        parts.path = *path;

        auto query = Detail::Uri::ParseQuery(cursor);
        if (!query) {
            return std::unexpected(query.error());
        }
        parts.query = query->text;
        parts.hasQuery = query->present;

        auto fragment = Detail::Uri::ParseFragment(cursor);
        if (!fragment) {
            return std::unexpected(fragment.error());
        }
        parts.fragment = fragment->text;
        parts.hasFragment = fragment->present;

        if (!cursor.Done()) {
            return std::unexpected(UriParseError::BadCharacter);
        }
        return parts;
    }

    /** @brief Return true when @p text is a syntactically valid absolute RFC 3986 URI. */
    [[nodiscard]] constexpr bool IsUri(std::string_view text) noexcept {
        return ParseUri(text).has_value();
    }

    /** @brief Stable FNV-1a hash of the exact URI bytes. */
    [[nodiscard]] constexpr uint64_t UriHash(std::string_view text) noexcept {
        return Hashing::HashByteRange(std::span<const char>{text.data(), text.size()});
    }

    /** @brief Zero-allocation iterable view over raw @c a=b&c query parameters. */
    class UriQueryView {
    public:
        /** @brief Iterator over query parameter views. */
        class iterator {
        public:
            using value_type = UriQueryParam;
            using difference_type = ptrdiff_t;

            constexpr iterator() = default;
            constexpr explicit iterator(std::string_view query, size_t position = 0) : query_(query), pos_(position) {
                SkipEmpty();
                ReadCurrent();
            }

            [[nodiscard]] constexpr UriQueryParam operator*() const noexcept { return current_; }

            constexpr iterator& operator++() noexcept {
                if (pos_ == npos) {
                    return *this;
                }
                const size_t amp = query_.find('&', pos_);
                pos_ = amp == std::string_view::npos ? npos : amp + 1;
                SkipEmpty();
                ReadCurrent();
                return *this;
            }

            [[nodiscard]] constexpr bool operator==(const iterator& other) const noexcept {
                return query_.data() == other.query_.data() && query_.size() == other.query_.size() &&
                       pos_ == other.pos_;
            }

        private:
            static constexpr size_t npos = std::string_view::npos;
            std::string_view query_{};
            size_t pos_ = npos;
            UriQueryParam current_{};

            constexpr void SkipEmpty() noexcept {
                while (pos_ != npos && pos_ < query_.size() && query_[pos_] == '&') {
                    ++pos_;
                }
                if (pos_ >= query_.size()) {
                    pos_ = npos;
                }
            }

            constexpr void ReadCurrent() noexcept {
                current_ = {};
                if (pos_ == npos) {
                    return;
                }
                const size_t amp = query_.find('&', pos_);
                const size_t end = amp == std::string_view::npos ? query_.size() : amp;
                const std::string_view item = query_.substr(pos_, end - pos_);
                const size_t eq = item.find('=');
                if (eq == std::string_view::npos) {
                    current_ = UriQueryParam{.name = item, .value = {}, .hasEquals = false};
                } else {
                    current_ = UriQueryParam{
                        .name = item.substr(0, eq),
                        .value = item.substr(eq + 1),
                        .hasEquals = true,
                    };
                }
            }
        };

        /** @brief Construct an empty query view. */
        constexpr UriQueryView() = default;

        /** @brief Construct from a raw query component without the leading @c ?. */
        constexpr explicit UriQueryView(std::string_view query) noexcept : query_(query) {}

        /** @brief Return an iterator to the first non-empty parameter. */
        [[nodiscard]] constexpr iterator begin() const noexcept { return iterator{query_, 0}; }

        /** @brief Return the sentinel iterator. */
        [[nodiscard]] constexpr iterator end() const noexcept { return iterator{query_, std::string_view::npos}; }

        /** @brief Return the raw query component. */
        [[nodiscard]] constexpr std::string_view Raw() const noexcept { return query_; }

        /** @brief Return the first raw parameter whose name equals @p name, if present. */
        [[nodiscard]] constexpr std::optional<UriQueryParam> Find(std::string_view name) const noexcept {
            for (UriQueryParam param : *this) {
                if (param.name == name) {
                    return param;
                }
            }
            return std::nullopt;
        }

        /** @brief Return the first raw value whose name equals @p name, or an empty view when absent. */
        [[nodiscard]] constexpr std::string_view Get(std::string_view name) const noexcept {
            if (auto param = Find(name); param.has_value()) {
                return param->value;
            }
            return {};
        }

        /** @brief Return the first raw value whose name equals @p name, or @p fallback when absent. */
        [[nodiscard]] constexpr std::string_view GetOr(std::string_view name,
                                                       std::string_view fallback) const noexcept {
            if (auto param = Find(name); param.has_value()) {
                return param->value;
            }
            return fallback;
        }

        /** @brief Return true when a parameter named @p name exists. */
        [[nodiscard]] constexpr bool Contains(std::string_view name) const noexcept {
            for (UriQueryParam param : *this) {
                if (param.name == name) {
                    return true;
                }
            }
            return false;
        }

    private:
        std::string_view query_{};
    };

    /** @brief Non-owning validated URI view with component and query helpers. */
    struct UriView {
        std::string_view text{}; /**< Original URI text. */

        /** @brief Construct an empty view. */
        constexpr UriView() = default;

        /** @brief Construct a URI view over @p value. */
        constexpr explicit UriView(std::string_view value) noexcept : text(value) {}

        /** @brief Return the original URI bytes. */
        [[nodiscard]] constexpr std::string_view view() const noexcept { return text; }

        /** @brief Return the original URI bytes. */
        [[nodiscard]] constexpr operator std::string_view() const noexcept { return text; }

        /** @brief Return whether this view is syntactically valid. */
        [[nodiscard]] constexpr bool Valid() const noexcept { return IsUri(text); }

        /** @brief Parse this URI into component views. */
        [[nodiscard]] constexpr auto Parts() const noexcept -> std::expected<UriParts, UriParseError> {
            return ParseUri(text);
        }

        /** @brief Return the URI scheme, or empty when invalid. */
        [[nodiscard]] constexpr std::string_view Scheme() const noexcept {
            auto parts = Parts();
            return parts ? parts->scheme : std::string_view{};
        }

        /** @brief Return the URI authority, or empty when absent or invalid. */
        [[nodiscard]] constexpr std::string_view Authority() const noexcept {
            auto parts = Parts();
            return parts ? parts->authority : std::string_view{};
        }

        /** @brief Return the URI path, or empty when invalid. */
        [[nodiscard]] constexpr std::string_view Path() const noexcept {
            auto parts = Parts();
            return parts ? parts->path : std::string_view{};
        }

        /** @brief Return the URI query, or empty when absent or invalid. */
        [[nodiscard]] constexpr std::string_view Query() const noexcept {
            auto parts = Parts();
            return parts ? parts->query : std::string_view{};
        }

        /** @brief Return the URI fragment, or empty when absent or invalid. */
        [[nodiscard]] constexpr std::string_view Fragment() const noexcept {
            auto parts = Parts();
            return parts ? parts->fragment : std::string_view{};
        }

        /** @brief Return the URI anchor after @c #, or empty when absent or invalid. */
        [[nodiscard]] constexpr std::string_view Anchor() const noexcept { return Fragment(); }

        /** @brief Return whether an anchor marker @c # is syntactically present. */
        [[nodiscard]] constexpr bool HasAnchor() const noexcept {
            auto parts = Parts();
            return parts ? parts->hasFragment : false;
        }

        /** @brief Return an iterable raw query parameter view. */
        [[nodiscard]] constexpr UriQueryView QueryParams() const noexcept { return UriQueryView{Query()}; }

        /** @brief Return the stable exact-byte URI hash. */
        [[nodiscard]] constexpr uint64_t Hash() const noexcept { return UriHash(text); }
    };

    /** @brief Decode a percent-encoded component into a fixed-capacity string. */
    template<size_t Capacity = 1024>
    [[nodiscard]] constexpr FixedString<Capacity> PercentDecode(std::string_view text, bool plusAsSpace = false) {
        FixedString<Capacity> out;
        for (size_t i = 0; i < text.size(); ++i) {
            char c = text[i];
            if (plusAsSpace && c == '+') {
                c = ' ';
            } else if (c == '%') {
                if (i + 2 >= text.size() || !Ascii::IsHexDigit(text[i + 1]) || !Ascii::IsHexDigit(text[i + 2])) {
                    throw "Bad URI percent encoding.";
                }
                c = static_cast<char>((Ascii::HexValue(text[i + 1]) << 4) | Ascii::HexValue(text[i + 2]));
                i += 2;
            }
            if (out.size() == out.capacity()) {
                throw "PercentDecode output exceeds capacity.";
            }
            out.push_back(c);
        }
        return out;
    }

    /** @brief Decode an application/x-www-form-urlencoded component, where @c + denotes a space. */
    template<size_t Capacity = 1024>
    [[nodiscard]] constexpr FixedString<Capacity> PercentDecodeFormComponent(std::string_view text) {
        return PercentDecode<Capacity>(text, true);
    }

    /** @brief Normalize syntax-preserving URI spelling by lowercasing the scheme and uppercasing percent hex digits. */
    template<size_t Capacity = 1024>
    [[nodiscard]] constexpr FixedString<Capacity> NormalizeUriSyntax(std::string_view text) {
        auto parsed = ParseUri(text);
        if (!parsed) {
            throw "Cannot normalize an invalid URI.";
        }
        FixedString<Capacity> out;
        const UriParts parts = *parsed;
        for (char c : parts.scheme) {
            out.push_back(Ascii::ToLower(c));
        }
        out.push_back(':');
        const auto appendEncoded = [&](std::string_view component) constexpr {
            for (size_t i = 0; i < component.size(); ++i) {
                out.push_back(component[i]);
                if (component[i] == '%') {
                    out.push_back(Ascii::ToUpperHex(component[++i]));
                    out.push_back(Ascii::ToUpperHex(component[++i]));
                }
            }
        };
        if (parts.hasAuthority) {
            out.append("//");
            appendEncoded(parts.authority);
        }
        appendEncoded(parts.path);
        if (parts.hasQuery) {
            out.push_back('?');
            appendEncoded(parts.query);
        }
        if (parts.hasFragment) {
            out.push_back('#');
            appendEncoded(parts.fragment);
        }
        return out;
    }

    /** @brief Owning compile-time URI carrier suitable for non-type template parameters. */
    template<size_t N>
    struct Uri {
        FixedString<N> text{}; /**< Canonical or caller-provided URI bytes. */

        /** @brief Construct an empty URI carrier. */
        constexpr Uri() = default;

        /** @brief Construct from a string literal and validate RFC 3986 syntax at compile time. */
        template<size_t M>
        consteval Uri(const char (&value)[M]) : text(value) {
            if (!IsUri(text.view())) {
                throw std::define_static_string("Sora::Uri literal is not a valid absolute RFC 3986 URI.");
            }
        }

        /** @brief Construct from a fixed string and validate RFC 3986 syntax. */
        consteval explicit Uri(FixedString<N> value) : text(value) {
            if (!IsUri(text.view())) {
                throw std::define_static_string("Sora::Uri value is not a valid absolute RFC 3986 URI.");
            }
        }

        /** @brief Return the URI bytes. */
        [[nodiscard]] constexpr std::string_view view() const noexcept { return text.view(); }

        /** @brief Return the URI bytes. */
        [[nodiscard]] constexpr const char* data() const noexcept { return text.data(); }

        /** @brief Return the URI byte length. */
        [[nodiscard]] constexpr size_t size() const noexcept { return text.size(); }

        /** @brief Return a non-owning URI view. */
        [[nodiscard]] constexpr UriView View() const noexcept { return UriView{view()}; }

        /** @brief Parse this URI into component views. */
        [[nodiscard]] constexpr auto Parts() const noexcept -> std::expected<UriParts, UriParseError> {
            return ParseUri(view());
        }

        /** @brief Return the URI scheme. */
        [[nodiscard]] constexpr std::string_view Scheme() const noexcept { return View().Scheme(); }

        /** @brief Return the URI authority, if any. */
        [[nodiscard]] constexpr std::string_view Authority() const noexcept { return View().Authority(); }

        /** @brief Return the URI path. */
        [[nodiscard]] constexpr std::string_view Path() const noexcept { return View().Path(); }

        /** @brief Return the URI query, if any. */
        [[nodiscard]] constexpr std::string_view Query() const noexcept { return View().Query(); }

        /** @brief Return the URI fragment, if any. */
        [[nodiscard]] constexpr std::string_view Fragment() const noexcept { return View().Fragment(); }

        /** @brief Return the URI anchor after @c #, if any. */
        [[nodiscard]] constexpr std::string_view Anchor() const noexcept { return View().Anchor(); }

        /** @brief Return whether an anchor marker @c # is syntactically present. */
        [[nodiscard]] constexpr bool HasAnchor() const noexcept { return View().HasAnchor(); }

        /** @brief Return an iterable raw query parameter view. */
        [[nodiscard]] constexpr UriQueryView QueryParams() const noexcept { return View().QueryParams(); }

        /** @brief Return the stable exact-byte URI hash. */
        [[nodiscard]] constexpr uint64_t Hash() const noexcept { return UriHash(view()); }

        /** @brief Convert to @c std::string_view. */
        [[nodiscard]] constexpr operator std::string_view() const noexcept { return view(); }

        /** @brief Compare URI spelling exactly. */
        constexpr bool operator==(const Uri&) const noexcept = default;
    };

    /** @brief Deduction guide for string literals. */
    template<size_t M>
    Uri(const char (&)[M]) -> Uri<M - 1>;

    namespace Literals {

        /** @brief Return a compile-time URI carrier for @p text. */
        [[nodiscard]] consteval Uri<256> operator""_URI(const char* text, size_t length) {
            if (length > 256) {
                throw std::define_static_string("Sora URI literal _URI exceeds its 256-character capacity.");
            }
            FixedString<256> value;
            for (size_t i = 0; i < length; ++i) {
                value.push_back(text[i]);
            }
            return Uri<256>{value};
        }

        /** @brief Return a compile-time URI carrier for @p text. */
        [[nodiscard]] consteval Uri<256> operator""_uri(const char* text, size_t length) {
            return operator""_URI(text, length);
        }

    } // namespace Literals

} // namespace Sora

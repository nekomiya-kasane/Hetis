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
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <meta>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>

namespace Sora {

    /** @brief URI parse, validation, construction, and decoding failure reason. */
    enum class UriError : uint8_t {
        Ok //
        [[= Sora::$::Description{"URI operation completed successfully."}]],
        Empty //
        [[= Sora::$::Description{"Input is empty."}]],
        MissingScheme //
        [[= Sora::$::Description{"Absolute URI requires a non-empty scheme and ':'."}]],
        BadSchemeStart //
        [[= Sora::$::Description{"Scheme does not start with an ASCII letter."}]],
        BadSchemeCharacter //
        [[= Sora::$::Description{"Scheme contains a character outside ALPHA / DIGIT / '+' / '-' / '.'."}]],
        BadCharacter //
        [[= Sora::$::Description{"URI contains a control, space, DEL, non-ASCII byte, or disallowed delimiter."}]],
        BadPercentEncoding //
        [[= Sora::$::Description{"A '%' is not followed by two hexadecimal digits."}]],
        BadAuthority //
        [[= Sora::$::Description{"Authority is present but malformed for the generic parser's constraints."}]],
        BadPath //
        [[= Sora::$::Description{"Path violates RFC 3986 path grammar for the parsed hierarchy form."}]],
        TooLong //
        [[= Sora::$::Description{"URI construction exceeds the fixed-capacity URI carrier."}]],
    };

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

        /** @brief Tag selecting construction from already-validated URI text. */
        struct UncheckedTag {};

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
        [[nodiscard]] constexpr UriError ValidateEncodedRange(std::string_view text) noexcept {
            for (size_t i = 0; i < text.size(); ++i) {
                const char c = text[i];
                if (!IsAsciiUriByte(c)) {
                    return UriError::BadCharacter;
                }
                if (c == '%') {
                    if (i + 2 >= text.size() || !Ascii::IsHexDigit(text[i + 1]) || !Ascii::IsHexDigit(text[i + 2])) {
                        return UriError::BadPercentEncoding;
                    }
                    i += 2;
                } else if (!Plain(c)) {
                    return UriError::BadCharacter;
                }
            }
            return UriError::Ok;
        }

        template<auto Plain>
        [[nodiscard]] constexpr bool IsValidEncodedRange(std::string_view text) noexcept {
            return ValidateEncodedRange<Plain>(text) == UriError::Ok;
        }

        [[nodiscard]] constexpr UriError ValidateAuthority(std::string_view authority) noexcept {
            for (size_t i = 0; i < authority.size(); ++i) {
                const char c = authority[i];
                if (!IsAsciiUriByte(c)) {
                    return UriError::BadCharacter;
                }
                if (c == '%') {
                    if (i + 2 >= authority.size() || !Ascii::IsHexDigit(authority[i + 1]) ||
                        !Ascii::IsHexDigit(authority[i + 2])) {
                        return UriError::BadPercentEncoding;
                    }
                    i += 2;
                } else if (!(IsRegNamePlain(c) || c == ':' || c == '@' || c == '[' || c == ']')) {
                    return UriError::BadAuthority;
                }
            }
            return UriError::Ok;
        }

        [[nodiscard]] constexpr bool IsValidAuthority(std::string_view authority) noexcept {
            return ValidateAuthority(authority) == UriError::Ok;
        }

        template<size_t Capacity>
        [[nodiscard]] constexpr bool Append(FixedString<Capacity>& out, std::string_view text) noexcept {
            if (text.size() > out.capacity() - out.size()) {
                return false;
            }
            out.append(text);
            return true;
        }

        template<size_t Capacity>
        [[nodiscard]] constexpr bool Append(FixedString<Capacity>& out, char c) noexcept {
            if (out.size() == out.capacity()) {
                return false;
            }
            out.push_back(c);
            return true;
        }

        template<size_t Capacity>
        [[nodiscard]] constexpr auto ToFixedString(std::string_view text) noexcept
            -> std::expected<FixedString<Capacity>, UriError> {
            if (text.size() > Capacity) {
                return std::unexpected(UriError::TooLong);
            }
            return FixedString<Capacity>{text};
        }

        /** @brief Parse `scheme:` and leave the cursor immediately after the colon. */
        [[nodiscard]] constexpr auto ParseScheme(Cursor& cursor) noexcept -> std::expected<std::string_view, UriError> {
            const std::string_view scheme = cursor.TakeUntilAny(":");
            if (!cursor.Consume(':') || scheme.empty()) {
                return std::unexpected(UriError::MissingScheme);
            }
            if (!Ascii::IsAlpha(scheme.front())) {
                return std::unexpected(UriError::BadSchemeStart);
            }
            if (std::ranges::any_of(scheme | std::views::drop(1), std::not_fn(IsSchemeCharacter))) {
                return std::unexpected(UriError::BadSchemeCharacter);
            }
            return scheme;
        }

        /** @brief Parse optional `//authority` and leave the cursor at path, query, fragment, or EOF. */
        [[nodiscard]] constexpr auto ParseAuthority(Cursor& cursor) noexcept
            -> std::expected<OptionalComponent, UriError> {
            if (!cursor.Consume("//")) {
                return OptionalComponent{};
            }
            const std::string_view authority = cursor.TakeUntilAny("/?#");
            if (const auto error = ValidateAuthority(authority); error != UriError::Ok) {
                return std::unexpected(error);
            }
            return OptionalComponent{.text = authority, .present = true};
        }

        /** @brief Parse the path component and enforce RFC 3986 generic hierarchy constraints. */
        [[nodiscard]] constexpr auto ParsePath(Cursor& cursor, bool hasAuthority) noexcept
            -> std::expected<std::string_view, UriError> {
            const std::string_view path = cursor.TakeUntilAny("?#");
            if (hasAuthority && !path.empty() && path.front() != '/') {
                return std::unexpected(UriError::BadPath);
            }
            if (!hasAuthority && path.starts_with("//")) {
                return std::unexpected(UriError::BadPath);
            }
            if (const auto error = ValidateEncodedRange<IsPathPlain>(path); error != UriError::Ok) {
                return std::unexpected(error);
            }
            return path;
        }

        /** @brief Parse optional `?query` and leave the cursor at fragment or EOF. */
        [[nodiscard]] constexpr auto ParseQuery(Cursor& cursor) noexcept -> std::expected<OptionalComponent, UriError> {
            if (!cursor.Consume('?')) {
                return OptionalComponent{};
            }
            const std::string_view query = cursor.TakeUntilAny("#");
            if (const auto error = ValidateEncodedRange<IsQueryPlain>(query); error != UriError::Ok) {
                return std::unexpected(error);
            }
            return OptionalComponent{.text = query, .present = true};
        }

        /** @brief Parse optional `#fragment` and consume the remaining URI text. */
        [[nodiscard]] constexpr auto ParseFragment(Cursor& cursor) noexcept
            -> std::expected<OptionalComponent, UriError> {
            if (!cursor.Consume('#')) {
                return OptionalComponent{};
            }
            const std::string_view fragment = cursor.TakeRest();
            if (const auto error = ValidateEncodedRange<IsFragmentPlain>(fragment); error != UriError::Ok) {
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
    [[nodiscard]] constexpr auto ParseUri(std::string_view text) noexcept -> std::expected<UriParts, UriError> {
        if (text.empty()) {
            return std::unexpected(UriError::Empty);
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
            return std::unexpected(UriError::BadCharacter);
        }
        return parts;
    }

    /** @brief Return true when @p text is a syntactically valid absolute RFC 3986 URI. */
    [[nodiscard]] constexpr bool IsUri(std::string_view text) noexcept {
        return ParseUri(text).has_value();
    }

    /**
     * @brief Return true when @p path has no empty, dot, or dot-dot path segments.
     *
     * @details This is stricter than RFC 3986 syntax and is intended for URI strings used as stable identity keys.
     * Empty paths are accepted; a root-only @c "/" path is rejected because it contains no identity segment.
     */
    [[nodiscard]] constexpr bool IsCanonicalUriIdentityPath(std::string_view path) noexcept {
        if (Detail::Uri::ValidateEncodedRange<Detail::Uri::IsPathPlain>(path) != UriError::Ok) {
            return false;
        }
        if (path.empty()) {
            return true;
        }
        if (path.ends_with('/')) {
            return false;
        }
        if (path.starts_with('/')) {
            path.remove_prefix(1);
        }
        while (!path.empty()) {
            const size_t slash = path.find('/');
            const std::string_view segment = slash == std::string_view::npos ? path : path.substr(0, slash);
            if (segment.empty() || segment == "." || segment == "..") {
                return false;
            }
            if (slash == std::string_view::npos) {
                return true;
            }
            path.remove_prefix(slash + 1u);
        }
        return true;
    }

    /**
     * @brief Return true when @p path is a canonical relative identity path.
     *
     * @details The path must contain at least one segment, must not start or end with @c /, must not contain raw
     * query/fragment delimiters, and must not contain @c : so callers do not accidentally accept scheme-like input.
     */
    [[nodiscard]] constexpr bool IsCanonicalRelativeUriIdentityPath(std::string_view path) noexcept {
        return !path.empty() && !path.starts_with('/') && !path.ends_with('/') &&
               path.find(':') == std::string_view::npos && IsCanonicalUriIdentityPath(path);
    }

    /** @brief Return true when @p suffix is a dot-prefixed filename suffix usable inside a URI path segment. */
    [[nodiscard]] constexpr bool IsUriPathFilenameSuffix(std::string_view suffix) noexcept {
        return !suffix.empty() && suffix.front() == '.' && suffix != "." && suffix != ".." &&
               suffix.find('/') == std::string_view::npos && suffix.find(':') == std::string_view::npos &&
               Detail::Uri::ValidateEncodedRange<Detail::Uri::IsPathPlain>(suffix) == UriError::Ok;
    }

    /** @brief Return true when @p text is an absolute URI usable as a canonical identity key. */
    [[nodiscard]] constexpr bool IsUriIdentity(std::string_view text) noexcept {
        auto parsed = ParseUri(text);
        return parsed.has_value() && !parsed->hasQuery && !parsed->hasFragment &&
               IsCanonicalUriIdentityPath(parsed->path);
    }

    /** @brief Normalize a URI identity base by removing trailing path separators and validating identity spelling. */
    template<size_t Capacity = 1024>
    [[nodiscard]] constexpr auto NormalizeUriIdentityBase(std::string_view uri) noexcept
        -> std::expected<FixedString<Capacity>, UriError> {
        auto parsed = ParseUri(uri);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        if (parsed->hasQuery || parsed->hasFragment) {
            return std::unexpected(UriError::BadCharacter);
        }

        size_t normalizedSize = uri.size();
        std::string_view path = parsed->path;
        while (!path.empty() && path.back() == '/') {
            path.remove_suffix(1);
            --normalizedSize;
        }

        const std::string_view normalized = uri.substr(0, normalizedSize);
        parsed = ParseUri(normalized);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        if (parsed->hasQuery || parsed->hasFragment || !IsCanonicalUriIdentityPath(parsed->path)) {
            return std::unexpected(UriError::BadPath);
        }
        return Detail::Uri::ToFixedString<Capacity>(normalized);
    }

    /** @brief Return @p base with canonical relative identity path @p relative appended to its path component. */
    template<size_t Capacity = 1024>
    [[nodiscard]] constexpr auto JoinUriIdentityPath(std::string_view base, std::string_view relative) noexcept
        -> std::expected<FixedString<Capacity>, UriError> {
        if (!IsCanonicalRelativeUriIdentityPath(relative)) {
            return std::unexpected(UriError::BadPath);
        }

        auto normalized = NormalizeUriIdentityBase<Capacity>(base);
        if (!normalized) {
            return std::unexpected(normalized.error());
        }

        FixedString<Capacity> out = *normalized;
        if (!Detail::Uri::Append(out, '/') || !Detail::Uri::Append(out, relative)) {
            return std::unexpected(UriError::TooLong);
        }
        if (!IsUriIdentity(out.view())) {
            return std::unexpected(UriError::BadPath);
        }
        return out;
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

    /** @brief Materialized parse result for repeated zero-allocation access to one URI. */
    struct ParsedUri {
        std::string_view text{}; /**< Original URI text. */
        UriParts parts{};        /**< Parsed component views into @ref text. */

        /** @brief Return the original URI bytes. */
        [[nodiscard]] constexpr std::string_view view() const noexcept { return text; }

        /** @brief Return the parsed component record. */
        [[nodiscard]] constexpr const UriParts& Parts() const noexcept { return parts; }

        /** @brief Return the URI scheme. */
        [[nodiscard]] constexpr std::string_view Scheme() const noexcept { return parts.scheme; }

        /** @brief Return the URI authority, or an empty view when absent. */
        [[nodiscard]] constexpr std::string_view Authority() const noexcept { return parts.authority; }

        /** @brief Return the URI path. */
        [[nodiscard]] constexpr std::string_view Path() const noexcept { return parts.path; }

        /** @brief Return the URI query, or an empty view when absent. */
        [[nodiscard]] constexpr std::string_view Query() const noexcept { return parts.query; }

        /** @brief Return the URI fragment, or an empty view when absent. */
        [[nodiscard]] constexpr std::string_view Fragment() const noexcept { return parts.fragment; }

        /** @brief Return the URI anchor after @c #, or an empty view when absent. */
        [[nodiscard]] constexpr std::string_view Anchor() const noexcept { return Fragment(); }

        /** @brief Return whether an authority marker @c // is syntactically present. */
        [[nodiscard]] constexpr bool HasAuthority() const noexcept { return parts.hasAuthority; }

        /** @brief Return whether a query marker @c ? is syntactically present. */
        [[nodiscard]] constexpr bool HasQuery() const noexcept { return parts.hasQuery; }

        /** @brief Return whether an anchor marker @c # is syntactically present. */
        [[nodiscard]] constexpr bool HasAnchor() const noexcept { return parts.hasFragment; }

        /** @brief Return an iterable raw query parameter view. */
        [[nodiscard]] constexpr UriQueryView QueryParams() const noexcept { return UriQueryView{parts.query}; }

        /** @brief Return the stable exact-byte URI hash. */
        [[nodiscard]] constexpr uint64_t Hash() const noexcept { return UriHash(text); }
    };

    /** @brief Parse an absolute URI and keep the parsed components for repeated access. */
    [[nodiscard]] constexpr auto ParseUriView(std::string_view text) noexcept -> std::expected<ParsedUri, UriError> {
        auto parts = ParseUri(text);
        if (!parts) {
            return std::unexpected(parts.error());
        }
        return ParsedUri{.text = text, .parts = *parts};
    }

    /** @brief Structured query argument used by URI construction helpers.
     *
     * @details The name must not contain raw @c & or @c = delimiters, and the value must not contain raw @c &. Encode
     * these bytes explicitly when they are part of data rather than query syntax.
     */
    struct UriQueryArgument {
        std::string_view name{};  /**< Raw query name without percent decoding. */
        std::string_view value{}; /**< Raw query value without percent decoding. */
        bool hasEquals = false;   /**< Whether the argument should be rendered as @c name=value. */

        /** @brief Construct a raw @c name=value query argument. */
        [[nodiscard]] static constexpr UriQueryArgument Pair(std::string_view name, std::string_view value) noexcept {
            return UriQueryArgument{.name = name, .value = value, .hasEquals = true};
        }

        /** @brief Construct a raw flag query argument rendered as @c name. */
        [[nodiscard]] static constexpr UriQueryArgument Flag(std::string_view name) noexcept {
            return UriQueryArgument{.name = name, .value = {}, .hasEquals = false};
        }
    };

    /** @brief Fragment/anchor argument used by URI construction helpers. */
    struct UriAnchorArgument {
        std::string_view text{}; /**< Raw anchor text without the leading @c #. */

        /** @brief Construct a raw URI anchor argument without the leading @c # marker. */
        [[nodiscard]] static constexpr UriAnchorArgument From(std::string_view text) noexcept {
            return UriAnchorArgument{.text = text};
        }
    };

    namespace Detail::Uri {

        /** @brief Compose URI text from parsed component views. */
        template<size_t Capacity>
        [[nodiscard]] constexpr auto ComposeText(const UriParts& parts) noexcept
            -> std::expected<FixedString<Capacity>, UriError> {
            FixedString<Capacity> out;
            if (!Append(out, parts.scheme) || !Append(out, ':')) {
                return std::unexpected(UriError::TooLong);
            }
            if (parts.hasAuthority && (!Append(out, "//") || !Append(out, parts.authority))) {
                return std::unexpected(UriError::TooLong);
            }
            if (!Append(out, parts.path)) {
                return std::unexpected(UriError::TooLong);
            }
            if (parts.hasQuery && (!Append(out, '?') || !Append(out, parts.query))) {
                return std::unexpected(UriError::TooLong);
            }
            if (parts.hasFragment && (!Append(out, '#') || !Append(out, parts.fragment))) {
                return std::unexpected(UriError::TooLong);
            }
            if (auto parsed = ParseUri(out.view()); !parsed) {
                return std::unexpected(parsed.error());
            }
            return out;
        }

        /** @brief Return URI text with @p segment appended to the path component. */
        template<size_t Capacity>
        [[nodiscard]] constexpr auto AppendPathText(std::string_view uri, std::string_view segment) noexcept
            -> std::expected<FixedString<Capacity>, UriError> {
            auto parsed = ParseUri(uri);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            while (segment.starts_with('/')) {
                segment.remove_prefix(1);
            }
            if (segment.empty()) {
                return ToFixedString<Capacity>(uri);
            }
            if (segment.contains('/')) {
                return std::unexpected(UriError::BadPath);
            }
            if (const auto error = ValidateEncodedRange<IsPathPlain>(segment); error != UriError::Ok) {
                return std::unexpected(error);
            }

            FixedString<Capacity> path;
            if (!Append(path, parsed->path)) {
                return std::unexpected(UriError::TooLong);
            }
            if (path.empty()) {
                if (parsed->hasAuthority && !Append(path, '/')) {
                    return std::unexpected(UriError::TooLong);
                }
            } else if (!path.ends_with("/") && !Append(path, '/')) {
                return std::unexpected(UriError::TooLong);
            }
            if (!Append(path, segment)) {
                return std::unexpected(UriError::TooLong);
            }

            auto parts = *parsed;
            parts.path = path.view();
            return ComposeText<Capacity>(parts);
        }

        /** @brief Return URI text with its query component replaced by @p query without the leading @c ?. */
        template<size_t Capacity>
        [[nodiscard]] constexpr auto WithQueryText(std::string_view uri, std::string_view query) noexcept
            -> std::expected<FixedString<Capacity>, UriError> {
            auto parsed = ParseUri(uri);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            if (const auto error = ValidateEncodedRange<IsQueryPlain>(query); error != UriError::Ok) {
                return std::unexpected(error);
            }
            auto parts = *parsed;
            parts.query = query;
            parts.hasQuery = true;
            return ComposeText<Capacity>(parts);
        }

        /** @brief Return URI text with @p argument appended to its query component. */
        template<size_t Capacity>
        [[nodiscard]] constexpr auto AppendQueryText(std::string_view uri, UriQueryArgument argument) noexcept
            -> std::expected<FixedString<Capacity>, UriError> {
            auto parsed = ParseUri(uri);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            if (argument.name.contains('&') || argument.name.contains('=') || argument.value.contains('&')) {
                return std::unexpected(UriError::BadCharacter);
            }
            if (const auto error = ValidateEncodedRange<IsQueryPlain>(argument.name); error != UriError::Ok) {
                return std::unexpected(error);
            }
            if (const auto error = ValidateEncodedRange<IsQueryPlain>(argument.value); error != UriError::Ok) {
                return std::unexpected(error);
            }

            FixedString<Capacity> query;
            if (parsed->hasQuery && !Append(query, parsed->query)) {
                return std::unexpected(UriError::TooLong);
            }
            if (!query.empty() && !Append(query, '&')) {
                return std::unexpected(UriError::TooLong);
            }
            if (!Append(query, argument.name)) {
                return std::unexpected(UriError::TooLong);
            }
            if (argument.hasEquals && (!Append(query, '=') || !Append(query, argument.value))) {
                return std::unexpected(UriError::TooLong);
            }

            auto parts = *parsed;
            parts.query = query.view();
            parts.hasQuery = true;
            return ComposeText<Capacity>(parts);
        }

        /** @brief Return URI text with its fragment/anchor component replaced by @p anchor. */
        template<size_t Capacity>
        [[nodiscard]] constexpr auto WithFragmentText(std::string_view uri, std::string_view fragment) noexcept
            -> std::expected<FixedString<Capacity>, UriError> {
            auto parsed = ParseUri(uri);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            if (const auto error = ValidateEncodedRange<IsFragmentPlain>(fragment); error != UriError::Ok) {
                return std::unexpected(error);
            }
            auto parts = *parsed;
            parts.fragment = fragment;
            parts.hasFragment = true;
            return ComposeText<Capacity>(parts);
        }

    } // namespace Detail::Uri

    /** @brief Non-owning raw URI text view with parsing convenience methods. */
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
        [[nodiscard]] constexpr auto Parts() const noexcept -> std::expected<UriParts, UriError> {
            return ParseUri(text);
        }

        /** @brief Parse this URI and keep the result in a materialized parsed view. */
        [[nodiscard]] constexpr auto Parsed() const noexcept -> std::expected<ParsedUri, UriError> {
            return ParseUriView(text);
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
    [[nodiscard]] constexpr auto PercentDecode(std::string_view text, bool plusAsSpace = false) noexcept
        -> std::expected<FixedString<Capacity>, UriError> {
        FixedString<Capacity> out;
        for (size_t i = 0; i < text.size(); ++i) {
            char c = text[i];
            if (plusAsSpace && c == '+') {
                c = ' ';
            } else if (c == '%') {
                if (i + 2 >= text.size() || !Ascii::IsHexDigit(text[i + 1]) || !Ascii::IsHexDigit(text[i + 2])) {
                    return std::unexpected(UriError::BadPercentEncoding);
                }
                c = static_cast<char>((Ascii::HexValue(text[i + 1]) << 4) | Ascii::HexValue(text[i + 2]));
                i += 2;
            }
            if (out.size() == out.capacity()) {
                return std::unexpected(UriError::TooLong);
            }
            out.push_back(c);
        }
        return out;
    }

    /** @brief Decode an application/x-www-form-urlencoded component, where @c + denotes a space. */
    template<size_t Capacity = 1024>
    [[nodiscard]] constexpr auto PercentDecodeFormComponent(std::string_view text) noexcept
        -> std::expected<FixedString<Capacity>, UriError> {
        return PercentDecode<Capacity>(text, true);
    }

    /** @brief Normalize syntax-preserving URI spelling by lowercasing the scheme and uppercasing percent hex digits. */
    template<size_t Capacity = 1024>
    [[nodiscard]] constexpr auto NormalizeUriSyntax(std::string_view text) noexcept
        -> std::expected<FixedString<Capacity>, UriError> {
        auto parsed = ParseUri(text);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        FixedString<Capacity> out;
        const UriParts parts = *parsed;
        for (char c : parts.scheme) {
            if (!Detail::Uri::Append(out, Ascii::ToLower(c))) {
                return std::unexpected(UriError::TooLong);
            }
        }
        if (!Detail::Uri::Append(out, ':')) {
            return std::unexpected(UriError::TooLong);
        }
        const auto appendEncoded = [&](std::string_view component) constexpr {
            for (size_t i = 0; i < component.size(); ++i) {
                if (!Detail::Uri::Append(out, component[i])) {
                    return false;
                }
                if (component[i] == '%') {
                    auto c1 = component[i + 1], c2 = component[i + 2];
                    if (!Ascii::IsHexDigit(c1) || !Ascii::IsHexDigit(c2)) {
                        return false;
                    }
                    if (!Detail::Uri::Append(out, Ascii::ToUpperHex(c1)) ||
                        !Detail::Uri::Append(out, Ascii::ToUpperHex(c2))) {
                        return false;
                    }
                    i += 2;
                }
            }
            return true;
        };
        if (parts.hasAuthority) {
            if (!Detail::Uri::Append(out, "//") || !appendEncoded(parts.authority)) {
                return std::unexpected(UriError::TooLong);
            }
        }
        if (!appendEncoded(parts.path)) {
            return std::unexpected(UriError::TooLong);
        }
        if (parts.hasQuery) {
            if (!Detail::Uri::Append(out, '?') || !appendEncoded(parts.query)) {
                return std::unexpected(UriError::TooLong);
            }
        }
        if (parts.hasFragment) {
            if (!Detail::Uri::Append(out, '#') || !appendEncoded(parts.fragment)) {
                return std::unexpected(UriError::TooLong);
            }
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

        /** @brief Construct from already-validated URI bytes. */
        constexpr Uri(FixedString<N> value, Detail::Uri::UncheckedTag) noexcept : text(value) {}

        /** @brief Try to construct a URI carrier from runtime text without throwing. */
        [[nodiscard]] static constexpr auto Try(std::string_view value) noexcept -> std::expected<Uri, UriError> {
            if (value.size() > N) {
                return std::unexpected(UriError::TooLong);
            }
            if (auto parsed = ParseUri(value); !parsed) {
                return std::unexpected(parsed.error());
            }
            return Uri{FixedString<N>{value}, Detail::Uri::UncheckedTag{}};
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
        [[nodiscard]] constexpr auto Parts() const noexcept -> std::expected<UriParts, UriError> {
            return ParseUri(view());
        }

        /** @brief Parse this URI and keep the result in a materialized parsed view. */
        [[nodiscard]] constexpr auto Parsed() const noexcept -> std::expected<ParsedUri, UriError> {
            return ParseUriView(view());
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

        /** @brief Return a URI with @p segment appended to the path component. */
        template<size_t Extra = 256>
        [[nodiscard]] constexpr auto AppendPath(std::string_view segment) const noexcept
            -> std::expected<Uri<N + Extra>, UriError> {
            auto result = Detail::Uri::AppendPathText<N + Extra>(view(), segment);
            if (!result) {
                return std::unexpected(result.error());
            }
            return Uri<N + Extra>{*result, Detail::Uri::UncheckedTag{}};
        }

        /** @brief Return a URI with its query component replaced by @p query without the leading @c ?. */
        template<size_t Extra = 256>
        [[nodiscard]] constexpr auto WithQuery(std::string_view query) const noexcept
            -> std::expected<Uri<N + Extra>, UriError> {
            auto result = Detail::Uri::WithQueryText<N + Extra>(view(), query);
            if (!result) {
                return std::unexpected(result.error());
            }
            return Uri<N + Extra>{*result, Detail::Uri::UncheckedTag{}};
        }

        /** @brief Return a URI with @p argument appended to the query component. */
        template<size_t Extra = 256>
        [[nodiscard]] constexpr auto AppendQuery(UriQueryArgument argument) const noexcept
            -> std::expected<Uri<N + Extra>, UriError> {
            auto result = Detail::Uri::AppendQueryText<N + Extra>(view(), argument);
            if (!result) {
                return std::unexpected(result.error());
            }
            return Uri<N + Extra>{*result, Detail::Uri::UncheckedTag{}};
        }

        /** @brief Return a URI with @p name and @p value appended as @c name=value in the query component. */
        template<size_t Extra = 256>
        [[nodiscard]] constexpr auto AppendQuery(std::string_view name, std::string_view value) const noexcept
            -> std::expected<Uri<N + Extra>, UriError> {
            return AppendQuery<Extra>(UriQueryArgument::Pair(name, value));
        }

        /** @brief Return a URI with @p name appended as a flag query argument. */
        template<size_t Extra = 256>
        [[nodiscard]] constexpr auto AppendFlag(std::string_view name) const noexcept
            -> std::expected<Uri<N + Extra>, UriError> {
            return AppendQuery<Extra>(UriQueryArgument::Flag(name));
        }

        /** @brief Return a URI with its fragment/anchor component replaced by @p anchor. */
        template<size_t Extra = 256>
        [[nodiscard]] constexpr auto WithAnchor(UriAnchorArgument anchor) const noexcept
            -> std::expected<Uri<N + Extra>, UriError> {
            auto result = Detail::Uri::WithFragmentText<N + Extra>(view(), anchor.text);
            if (!result) {
                return std::unexpected(result.error());
            }
            return Uri<N + Extra>{*result, Detail::Uri::UncheckedTag{}};
        }

        /** @brief Return a URI with its fragment/anchor component replaced by @p anchor. */
        template<size_t Extra = 256>
        [[nodiscard]] constexpr auto WithAnchor(std::string_view anchor) const noexcept
            -> std::expected<Uri<N + Extra>, UriError> {
            return WithAnchor<Extra>(UriAnchorArgument::From(anchor));
        }

        /** @brief Convert to @c std::string_view. */
        [[nodiscard]] constexpr operator std::string_view() const noexcept { return view(); }

        /** @brief Compare exact URI spelling with another URI carrier. */
        template<size_t M>
        [[nodiscard]] constexpr bool operator==(const Uri<M>& other) const noexcept {
            return view() == other.view();
        }

        /** @brief Compare exact URI spelling with a URI view. */
        [[nodiscard]] constexpr bool operator==(UriView other) const noexcept { return view() == other.view(); }

        /** @brief Compare exact URI spelling with raw text. */
        [[nodiscard]] constexpr bool operator==(std::string_view other) const noexcept { return view() == other; }

        /** @brief Order by exact URI spelling. */
        template<size_t M>
        [[nodiscard]] constexpr std::strong_ordering operator<=>(const Uri<M>& other) const noexcept {
            return view() <=> other.view();
        }

        /** @brief Order by exact URI spelling against a URI view. */
        [[nodiscard]] constexpr std::strong_ordering operator<=>(UriView other) const noexcept {
            return view() <=> other.view();
        }

        /** @brief Order by exact URI spelling against raw text. */
        [[nodiscard]] constexpr std::strong_ordering operator<=>(std::string_view other) const noexcept {
            return view() <=> other;
        }
    };

    /** @brief Deduction guide for string literals. */
    template<size_t M>
    Uri(const char (&)[M]) -> Uri<M - 1>;

    /** @brief Try to construct a fixed-capacity URI carrier from runtime text. */
    template<size_t Capacity = 256>
    [[nodiscard]] constexpr auto MakeUri(std::string_view text) noexcept -> std::expected<Uri<Capacity>, UriError> {
        return Uri<Capacity>::Try(text);
    }

    /** @brief Compose a fixed-capacity URI carrier from component views. */
    template<size_t Capacity = 256>
    [[nodiscard]] constexpr auto ComposeUri(const UriParts& parts) noexcept -> std::expected<Uri<Capacity>, UriError> {
        auto text = Detail::Uri::ComposeText<Capacity>(parts);
        if (!text) {
            return std::unexpected(text.error());
        }
        return Uri<Capacity>{*text, Detail::Uri::UncheckedTag{}};
    }

    /** @brief Compare exact URI spelling between a URI view and a URI carrier. */
    template<size_t N>
    [[nodiscard]] constexpr bool operator==(UriView lhs, const Uri<N>& rhs) noexcept {
        return lhs.view() == rhs.view();
    }

    /** @brief Order exact URI spelling between a URI view and a URI carrier. */
    template<size_t N>
    [[nodiscard]] constexpr std::strong_ordering operator<=>(UriView lhs, const Uri<N>& rhs) noexcept {
        return lhs.view() <=> rhs.view();
    }

    /** @brief Compare exact URI spelling between two URI views. */
    [[nodiscard]] constexpr bool operator==(UriView lhs, UriView rhs) noexcept {
        return lhs.view() == rhs.view();
    }

    /** @brief Order exact URI spelling between two URI views. */
    [[nodiscard]] constexpr std::strong_ordering operator<=>(UriView lhs, UriView rhs) noexcept {
        return lhs.view() <=> rhs.view();
    }

    /** @brief Fixed-capacity structured URI builder over RFC 3986 components. */
    template<size_t Capacity = 256>
    class UriBuilder {
        FixedString<Capacity> scheme_{};
        FixedString<Capacity> authority_{};
        FixedString<Capacity> path_{};
        FixedString<Capacity> query_{};
        FixedString<Capacity> fragment_{};
        UriError error_ = UriError::Ok;
        bool hasAuthority_ = false;
        bool hasQuery_ = false;
        bool hasFragment_ = false;

        constexpr void SetError(UriError error) noexcept {
            if (error_ == UriError::Ok) {
                error_ = error;
            }
        }

        constexpr bool Assign(FixedString<Capacity>& target, std::string_view value) noexcept {
            target.clear();
            if (!Detail::Uri::Append(target, value)) {
                SetError(UriError::TooLong);
                return false;
            }
            return true;
        }

        constexpr bool AppendPathSegment(std::string_view segment) noexcept {
            while (segment.starts_with('/')) {
                segment.remove_prefix(1);
            }
            if (segment.empty()) {
                return true;
            }
            if (segment.contains('/')) {
                SetError(UriError::BadPath);
                return false;
            }
            if (const auto error = Detail::Uri::ValidateEncodedRange<Detail::Uri::IsPathPlain>(segment);
                error != UriError::Ok) {
                SetError(error);
                return false;
            }
            if (path_.empty()) {
                if (hasAuthority_ && !Detail::Uri::Append(path_, '/')) {
                    SetError(UriError::TooLong);
                    return false;
                }
            } else if (!path_.ends_with("/") && !Detail::Uri::Append(path_, '/')) {
                SetError(UriError::TooLong);
                return false;
            }
            if (!Detail::Uri::Append(path_, segment)) {
                SetError(UriError::TooLong);
                return false;
            }
            return true;
        }

    public:
        /** @brief Construct an empty builder. */
        constexpr UriBuilder() = default;

        /** @brief Construct a builder initialized from an existing absolute URI. */
        [[nodiscard]] static constexpr UriBuilder From(std::string_view uri) noexcept {
            UriBuilder builder;
            auto parsed = ParseUri(uri);
            if (!parsed) {
                builder.SetError(parsed.error());
                return builder;
            }
            builder.Scheme(parsed->scheme);
            if (parsed->hasAuthority) {
                builder.Authority(parsed->authority);
            }
            builder.Path(parsed->path);
            if (parsed->hasQuery) {
                builder.RawQuery(parsed->query);
            }
            if (parsed->hasFragment) {
                builder.Anchor(parsed->fragment);
            }
            return builder;
        }

        /** @brief Replace the URI scheme without the trailing @c :. */
        constexpr UriBuilder& Scheme(std::string_view scheme) noexcept {
            if (error_ == UriError::Ok) {
                Assign(scheme_, scheme);
            }
            return *this;
        }

        /** @brief Replace the authority without the leading @c //. */
        constexpr UriBuilder& Authority(std::string_view authority) noexcept {
            if (error_ == UriError::Ok) {
                hasAuthority_ = true;
                Assign(authority_, authority);
                if (!path_.empty() && !path_.starts_with("/")) {
                    FixedString<Capacity> normalized;
                    if (!Detail::Uri::Append(normalized, '/') || !Detail::Uri::Append(normalized, path_.view())) {
                        SetError(UriError::TooLong);
                    } else {
                        path_ = normalized;
                    }
                }
            }
            return *this;
        }

        /** @brief Remove the authority component. */
        constexpr UriBuilder& NoAuthority() noexcept {
            if (error_ == UriError::Ok) {
                hasAuthority_ = false;
                authority_.clear();
            }
            return *this;
        }

        /** @brief Replace the full path component. A leading slash is inserted when an authority is present. */
        constexpr UriBuilder& Path(std::string_view path) noexcept {
            if (error_ != UriError::Ok) {
                return *this;
            }
            if (path.empty()) {
                path_.clear();
                return *this;
            }
            if (const auto error = Detail::Uri::ValidateEncodedRange<Detail::Uri::IsPathPlain>(path);
                error != UriError::Ok) {
                SetError(error);
                return *this;
            }
            path_.clear();
            if (hasAuthority_ && !path.starts_with("/") && !Detail::Uri::Append(path_, '/')) {
                SetError(UriError::TooLong);
                return *this;
            }
            if (!Detail::Uri::Append(path_, path)) {
                SetError(UriError::TooLong);
            }
            return *this;
        }

        /** @brief Append @p segment as one path segment. */
        constexpr UriBuilder& Segment(std::string_view segment) noexcept {
            if (error_ == UriError::Ok) {
                AppendPathSegment(segment);
            }
            return *this;
        }

        /** @brief Replace the raw query component without the leading @c ?. */
        constexpr UriBuilder& RawQuery(std::string_view query) noexcept {
            if (error_ != UriError::Ok) {
                return *this;
            }
            if (const auto error = Detail::Uri::ValidateEncodedRange<Detail::Uri::IsQueryPlain>(query);
                error != UriError::Ok) {
                SetError(error);
                return *this;
            }
            hasQuery_ = true;
            Assign(query_, query);
            return *this;
        }

        /** @brief Append @p argument to the query component. */
        constexpr UriBuilder& Query(UriQueryArgument argument) noexcept {
            if (error_ != UriError::Ok) {
                return *this;
            }
            if (argument.name.contains('&') || argument.name.contains('=') || argument.value.contains('&')) {
                SetError(UriError::BadCharacter);
                return *this;
            }
            if (const auto error = Detail::Uri::ValidateEncodedRange<Detail::Uri::IsQueryPlain>(argument.name);
                error != UriError::Ok) {
                SetError(error);
                return *this;
            }
            if (const auto error = Detail::Uri::ValidateEncodedRange<Detail::Uri::IsQueryPlain>(argument.value);
                error != UriError::Ok) {
                SetError(error);
                return *this;
            }
            if (hasQuery_ && !query_.empty() && !Detail::Uri::Append(query_, '&')) {
                SetError(UriError::TooLong);
                return *this;
            }
            hasQuery_ = true;
            if (!Detail::Uri::Append(query_, argument.name)) {
                SetError(UriError::TooLong);
                return *this;
            }
            if (argument.hasEquals &&
                (!Detail::Uri::Append(query_, '=') || !Detail::Uri::Append(query_, argument.value))) {
                SetError(UriError::TooLong);
            }
            return *this;
        }

        /** @brief Append @p name and @p value as @c name=value to the query component. */
        constexpr UriBuilder& Query(std::string_view name, std::string_view value) noexcept {
            return Query(UriQueryArgument::Pair(name, value));
        }

        /** @brief Append @p name as a flag query argument. */
        constexpr UriBuilder& Flag(std::string_view name) noexcept { return Query(UriQueryArgument::Flag(name)); }

        /** @brief Remove the query component. */
        constexpr UriBuilder& NoQuery() noexcept {
            if (error_ == UriError::Ok) {
                hasQuery_ = false;
                query_.clear();
            }
            return *this;
        }

        /** @brief Replace the fragment/anchor component without the leading @c #. */
        constexpr UriBuilder& Anchor(std::string_view anchor) noexcept {
            if (error_ != UriError::Ok) {
                return *this;
            }
            if (const auto error = Detail::Uri::ValidateEncodedRange<Detail::Uri::IsFragmentPlain>(anchor);
                error != UriError::Ok) {
                SetError(error);
                return *this;
            }
            hasFragment_ = true;
            Assign(fragment_, anchor);
            return *this;
        }

        /** @brief Remove the fragment/anchor component. */
        constexpr UriBuilder& NoAnchor() noexcept {
            if (error_ == UriError::Ok) {
                hasFragment_ = false;
                fragment_.clear();
            }
            return *this;
        }

        /** @brief Return the first accumulated construction error, if any. */
        [[nodiscard]] constexpr UriError Error() const noexcept { return error_; }

        /** @brief Compose the current components into fixed-capacity URI text. */
        [[nodiscard]] constexpr auto BuildText() const noexcept -> std::expected<FixedString<Capacity>, UriError> {
            if (error_ != UriError::Ok) {
                return std::unexpected(error_);
            }
            return Detail::Uri::ComposeText<Capacity>(UriParts{
                .scheme = scheme_.view(),
                .authority = authority_.view(),
                .path = path_.view(),
                .query = query_.view(),
                .fragment = fragment_.view(),
                .hasAuthority = hasAuthority_,
                .hasQuery = hasQuery_,
                .hasFragment = hasFragment_,
            });
        }

        /** @brief Compose the current components into a validated URI value. */
        [[nodiscard]] constexpr auto Build() const noexcept -> std::expected<Uri<Capacity>, UriError> {
            auto text = BuildText();
            if (!text) {
                return std::unexpected(text.error());
            }
            return Uri<Capacity>{*text, Detail::Uri::UncheckedTag{}};
        }
    };

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

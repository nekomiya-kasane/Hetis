/**
 * @file FixedString.h
 * @brief Compile-time fixed-capacity string suitable for non-type template parameters.
 * @ingroup Core
 *
 * @details @c FixedString<N> is a structural type suitable for use as a non-type template parameter. It stores up to
 * @c N characters, excluding the implicit NUL terminator, in a @c char[N + 1] buffer with constant-time size access.
 * Operations are @c constexpr or @c consteval oriented and are intended to fold away in compile-time use.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <meta>
#include <string_view>
#include <utility>

namespace Sora {

    /**
     * @brief Fixed-capacity compile-time string.
     *
     * @details The internal storage is @c char[N + 1] and is always NUL-terminated. The @c len_ member tracks the
     * current logical length, so a larger-capacity string can represent shorter values without changing type.
     *
     * @tparam N Maximum number of characters, excluding the NUL terminator.
     */
    template<size_t N>
    struct FixedString {
        /** @brief Raw NUL-terminated character storage. */
        char buf_[N + 1]{};

        /** @brief Current logical string length. */
        size_t len_ = 0;

        /** @name Constructors */
        /** @{ */

        /** @brief Construct an empty string. */
        constexpr FixedString() = default;

        /**
         * @brief Construct from a string literal.
         * @param[in] s String literal including its trailing NUL terminator.
         */
        template<size_t M>
        consteval FixedString(const char (&s)[M]) : len_(M - 1) {
            static_assert(M - 1 <= N, "String literal exceeds FixedString capacity");
            for (size_t i = 0; i < M; ++i) {
                buf_[i] = s[i];
            }
        }

        /**
         * @brief Construct from a string view.
         * @param[in] sv Source characters. The caller must ensure @p sv fits within this string's capacity.
         */
        constexpr FixedString(std::string_view sv) : len_(sv.size()) {
            for (size_t i = 0; i < sv.size(); ++i) {
                buf_[i] = sv[i];
            }
            buf_[sv.size()] = '\0';
        }

        /**
         * @brief Construct from a repeated character.
         * @param[in] count Number of characters to write. The caller must ensure @p count fits within capacity.
         * @param[in] c Character to repeat.
         */
        constexpr FixedString(size_t count, char c) : len_(count) {
            for (size_t i = 0; i < count; ++i) {
                buf_[i] = c;
            }
            buf_[count] = '\0';
        }

        /** @} */

        /** @name Observers */
        /** @{ */

        /** @brief Return the current logical string length. */
        [[nodiscard]] constexpr size_t size() const { return len_; }

        /** @brief Return the maximum number of characters this string can hold. */
        [[nodiscard]] constexpr size_t capacity() const { return N; }

        /** @brief Return true when this string is empty. */
        [[nodiscard]] constexpr bool empty() const { return len_ == 0; }

        /** @brief Return the raw NUL-terminated character buffer. */
        [[nodiscard]] constexpr const char* data() const { return buf_; }

        /** @brief Return the raw NUL-terminated character buffer. */
        [[nodiscard]] constexpr const char* c_str() const { return buf_; }

        /** @brief Return the character at @p i. */
        [[nodiscard]] constexpr char operator[](size_t i) const { return buf_[i]; }

        /** @brief Return the character at @p i. */
        [[nodiscard]] constexpr char& operator[](size_t i) { return buf_[i]; }

        /** @brief Return the first character. The string must be non-empty. */
        [[nodiscard]] constexpr char front() const { return buf_[0]; }

        /** @brief Return the last character. The string must be non-empty. */
        [[nodiscard]] constexpr char back() const { return buf_[len_ - 1]; }

        /** @brief Convert to @c std::string_view. */
        [[nodiscard]] constexpr operator std::string_view() const { return {buf_, len_}; }

        /** @brief Return this string as @c std::string_view. */
        [[nodiscard]] constexpr std::string_view view() const { return {buf_, len_}; }

        /** @} */

        /** @name Iterators */
        /** @{ */

        /** @brief Return a const iterator to the first character. */
        [[nodiscard]] constexpr const char* begin() const { return buf_; }

        /** @brief Return a const iterator one past the last character. */
        [[nodiscard]] constexpr const char* end() const { return buf_ + len_; }

        /** @brief Return a mutable iterator to the first character. */
        [[nodiscard]] constexpr char* begin() { return buf_; }

        /** @brief Return a mutable iterator one past the last character. */
        [[nodiscard]] constexpr char* end() { return buf_ + len_; }

        /** @} */

        /** @name Comparison */
        /** @{ */

        /** @brief Three-way compare with another fixed string. */
        template<size_t M>
        [[nodiscard]] constexpr auto operator<=>(const FixedString<M>& o) const {
            return view() <=> o.view();
        }

        /** @brief Compare equality with another fixed string. */
        template<size_t M>
        [[nodiscard]] constexpr bool operator==(const FixedString<M>& o) const {
            return view() == o.view();
        }

        /** @brief Three-way compare with a string view. */
        [[nodiscard]] constexpr auto operator<=>(std::string_view sv) const { return view() <=> sv; }

        /** @brief Compare equality with a string view. */
        [[nodiscard]] constexpr bool operator==(std::string_view sv) const { return view() == sv; }

        /** @} */

        /** @name Search */
        /** @{ */

        /** @brief Sentinel returned when a search fails. */
        static constexpr size_t npos = std::string_view::npos;

        /**
         * @brief Find character @p c at or after @p pos.
         * @param[in] c Character to find.
         * @param[in] pos First position to inspect.
         * @return Matching index, or @ref npos when no match exists.
         */
        [[nodiscard]] constexpr size_t find(char c, size_t pos = 0) const {
            for (size_t i = pos; i < len_; ++i) {
                if (buf_[i] == c) {
                    return i;
                }
            }
            return npos;
        }

        /** @brief Find string @p s at or after @p pos. */
        [[nodiscard]] constexpr size_t find(std::string_view s, size_t pos = 0) const { return view().find(s, pos); }

        /** @brief Find the last occurrence of character @p c at or before @p pos. */
        [[nodiscard]] constexpr size_t rfind(char c, size_t pos = npos) const {
            size_t start = (pos < len_) ? pos : (len_ > 0 ? len_ - 1 : 0);
            for (size_t i = start + 1; i > 0; --i) {
                if (buf_[i - 1] == c) {
                    return i - 1;
                }
            }
            return npos;
        }

        /** @brief Find the last occurrence of string @p s at or before @p pos. */
        [[nodiscard]] constexpr size_t rfind(std::string_view s, size_t pos = npos) const {
            return view().rfind(s, pos);
        }

        /** @brief Return true when this string contains character @p c. */
        [[nodiscard]] constexpr bool contains(char c) const { return find(c) != npos; }

        /** @brief Return true when this string contains substring @p s. */
        [[nodiscard]] constexpr bool contains(std::string_view s) const { return find(s) != npos; }

        /** @brief Return true when this string starts with @p s. */
        [[nodiscard]] constexpr bool starts_with(std::string_view s) const { return view().starts_with(s); }

        /** @brief Return true when this string ends with @p s. */
        [[nodiscard]] constexpr bool ends_with(std::string_view s) const { return view().ends_with(s); }

        /** @brief Count occurrences of character @p c. */
        [[nodiscard]] constexpr size_t count(char c) const {
            size_t n = 0;
            for (size_t i = 0; i < len_; ++i) {
                if (buf_[i] == c) {
                    ++n;
                }
            }
            return n;
        }

        /** @} */

        /** @name Substring extraction */
        /** @{ */

        /**
         * @brief Return a substring starting at @p pos.
         * @param[in] pos First character position to include.
         * @param[in] count Maximum number of characters to copy.
         * @return Substring with the same fixed capacity as this string.
         */
        [[nodiscard]] constexpr FixedString substr(size_t pos, size_t count = npos) const {
            size_t actual = (count == npos || pos + count > len_) ? (len_ - pos) : count;
            FixedString r;
            r.len_ = actual;
            for (size_t i = 0; i < actual; ++i) {
                r.buf_[i] = buf_[pos + i];
            }
            r.buf_[actual] = '\0';
            return r;
        }

        /** @brief Return everything before the first occurrence of @p delim, or the whole string. */
        [[nodiscard]] constexpr FixedString before(char delim) const {
            size_t p = find(delim);
            return (p == npos) ? *this : substr(0, p);
        }

        /** @brief Return everything after the first occurrence of @p delim, or an empty string. */
        [[nodiscard]] constexpr FixedString after(char delim) const {
            size_t p = find(delim);
            return (p == npos) ? FixedString{} : substr(p + 1);
        }

        /** @brief Return everything before the last occurrence of @p delim, or the whole string. */
        [[nodiscard]] constexpr FixedString before_last(char delim) const {
            size_t p = rfind(delim);
            return (p == npos) ? *this : substr(0, p);
        }

        /** @brief Return everything after the last occurrence of @p delim, or an empty string. */
        [[nodiscard]] constexpr FixedString after_last(char delim) const {
            size_t p = rfind(delim);
            return (p == npos) ? FixedString{} : substr(p + 1);
        }

        /** @} */

        /** @name Transformation */
        /** @{ */

        /** @brief Remove @p prefix from the front if it is present. */
        [[nodiscard]] constexpr FixedString strip_prefix(std::string_view prefix) const {
            if (starts_with(prefix)) {
                return substr(prefix.size());
            }
            return *this;
        }

        /** @brief Remove @p suffix from the back if it is present. */
        [[nodiscard]] constexpr FixedString strip_suffix(std::string_view suffix) const {
            if (ends_with(suffix)) {
                return substr(0, len_ - suffix.size());
            }
            return *this;
        }

        /** @brief Replace every occurrence of character @p from with @p to. */
        [[nodiscard]] constexpr FixedString replace_char(char from, char to) const {
            FixedString r = *this;
            for (size_t i = 0; i < r.len_; ++i) {
                if (r.buf_[i] == from) {
                    r.buf_[i] = to;
                }
            }
            return r;
        }

        /** @brief Apply a per-character transform function. */
        [[nodiscard]] constexpr FixedString transform(auto&& fn) const {
            FixedString r = *this;
            for (size_t i = 0; i < r.len_; ++i) {
                r.buf_[i] = fn(r.buf_[i]);
            }
            return r;
        }

        /** @brief Convert ASCII lowercase letters to uppercase. */
        [[nodiscard]] constexpr FixedString to_upper() const {
            return transform([](char c) -> char { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c; });
        }

        /** @brief Convert ASCII uppercase letters to lowercase. */
        [[nodiscard]] constexpr FixedString to_lower() const {
            return transform([](char c) -> char { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; });
        }

        /** @brief Trim leading and trailing ASCII whitespace. */
        [[nodiscard]] constexpr FixedString trim() const {
            size_t l = 0;
            size_t r = len_;
            while (l < r && (buf_[l] == ' ' || buf_[l] == '\t' || buf_[l] == '\n' || buf_[l] == '\r')) {
                ++l;
            }
            while (r > l && (buf_[r - 1] == ' ' || buf_[r - 1] == '\t' || buf_[r - 1] == '\n' || buf_[r - 1] == '\r')) {
                --r;
            }
            return substr(l, r - l);
        }

        /** @brief Return a reversed copy of this string. */
        [[nodiscard]] constexpr FixedString reverse() const {
            FixedString r = *this;
            for (size_t i = 0; i < r.len_ / 2; ++i) {
                char tmp = r.buf_[i];
                r.buf_[i] = r.buf_[r.len_ - 1 - i];
                r.buf_[r.len_ - 1 - i] = tmp;
            }
            return r;
        }

        /** @} */

        /** @name Path operations */
        /** @{ */

        /** @brief Return the filename stem, such as @c baz from @c foo/bar/baz.cpp. */
        [[nodiscard]] constexpr FixedString stem() const {
            auto name = after_last_sep();
            size_t dot = name.rfind('.');
            return (dot == npos) ? name : name.substr(0, dot);
        }

        /** @brief Return the file extension with dot, such as @c .cpp from @c foo.cpp. */
        [[nodiscard]] constexpr FixedString extension() const {
            auto name = after_last_sep();
            size_t dot = name.rfind('.');
            return (dot == npos) ? FixedString{} : name.substr(dot);
        }

        /** @brief Return the last path component. */
        [[nodiscard]] constexpr FixedString filename() const { return after_last_sep(); }

        /** @brief Return the parent directory path. */
        [[nodiscard]] constexpr FixedString parent_path() const {
            size_t p = rfind_sep();
            return (p == npos) ? FixedString{} : substr(0, p);
        }

        /** @brief Return the immediate parent directory name. */
        [[nodiscard]] constexpr FixedString parent_name() const {
            auto pp = parent_path();
            return pp.after_last_sep().empty() ? pp : pp.after_last_sep();
        }

        /** @} */

    private:
        /** @brief Return true when @p c is a supported path separator. */
        [[nodiscard]] constexpr bool is_sep(char c) const { return c == '/' || c == '\\'; }

        /** @brief Return the position of the last path separator, or @ref npos. */
        [[nodiscard]] constexpr size_t rfind_sep() const {
            for (size_t i = len_; i > 0; --i) {
                if (is_sep(buf_[i - 1])) {
                    return i - 1;
                }
            }
            return npos;
        }

        /** @brief Return everything after the last path separator. */
        [[nodiscard]] constexpr FixedString after_last_sep() const {
            size_t p = rfind_sep();
            return (p == npos) ? *this : substr(p + 1);
        }

    public:
        /** @name Append and mutation */
        /** @{ */

        /** @brief Append one character. */
        constexpr FixedString& push_back(char c) {
            buf_[len_++] = c;
            buf_[len_] = '\0';
            return *this;
        }

        /** @brief Append all characters in @p sv. */
        constexpr FixedString& append(std::string_view sv) {
            for (char c : sv) {
                buf_[len_++] = c;
            }
            buf_[len_] = '\0';
            return *this;
        }

        /** @brief Append character @p c repeated @p count times. */
        constexpr FixedString& append(char c, size_t count = 1) {
            for (size_t i = 0; i < count; ++i) {
                buf_[len_++] = c;
            }
            buf_[len_] = '\0';
            return *this;
        }

        /** @brief Clear all contents. */
        constexpr void clear() {
            len_ = 0;
            buf_[0] = '\0';
        }

        /** @} */
    };

    /** @brief Deduction guide for string literals, where the array size includes the NUL terminator. */
    template<size_t M>
    FixedString(const char (&)[M]) -> FixedString<M - 1>;

    /** @name Concatenation */
    /** @{ */

    /** @brief Concatenate two fixed strings. */
    template<size_t A, size_t B>
    [[nodiscard]] constexpr FixedString<A + B> operator+(const FixedString<A>& a, const FixedString<B>& b) {
        FixedString<A + B> r;
        r.len_ = a.size() + b.size();
        for (size_t i = 0; i < a.size(); ++i) {
            r.buf_[i] = a[i];
        }
        for (size_t i = 0; i < b.size(); ++i) {
            r.buf_[a.size() + i] = b[i];
        }
        r.buf_[r.len_] = '\0';
        return r;
    }

    /** @brief Concatenate a fixed string with a string view using a conservative extra capacity. */
    template<size_t A>
    [[nodiscard]] constexpr auto operator+(const FixedString<A>& a, std::string_view b) {
        FixedString<A + 256> r;
        r.append(a.view());
        r.append(b);
        return r;
    }

    /** @} */

    /** @brief Free-function algorithms operating on @ref FixedString. */
    namespace String {

        /**
         * @brief Repeat string @p s @p n times.
         * @tparam Cap Result capacity.
         */
        template<size_t Cap = 1024>
        [[nodiscard]] constexpr FixedString<Cap> Repeat(std::string_view s, size_t n) {
            FixedString<Cap> r;
            for (size_t i = 0; i < n; ++i) {
                r.append(s);
            }
            return r;
        }

        /**
         * @brief Join string views with separator @p sep.
         * @tparam Cap Result capacity.
         * @tparam Count Number of input parts.
         */
        template<size_t Cap = 1024, size_t Count>
        [[nodiscard]] constexpr FixedString<Cap> Join(std::string_view sep,
                                                      const std::array<std::string_view, Count>& parts) {
            FixedString<Cap> r;
            for (size_t i = 0; i < Count; ++i) {
                if (i > 0) {
                    r.append(sep);
                }
                r.append(parts[i]);
            }
            return r;
        }

        /** @brief Join two string views with separator @p sep. */
        template<size_t Cap = 256>
        [[nodiscard]] constexpr FixedString<Cap> Join(std::string_view sep, std::string_view a, std::string_view b) {
            FixedString<Cap> r;
            r.append(a);
            r.append(sep);
            r.append(b);
            return r;
        }

        /** @brief Convert a signed integer to a decimal string. */
        template<size_t Cap = 32>
        [[nodiscard]] constexpr FixedString<Cap> FromInt(long long value) {
            if (value == 0) {
                return FixedString<Cap>(std::string_view("0"));
            }
            FixedString<Cap> r;
            bool neg = value < 0;
            unsigned long long v =
                neg ? static_cast<unsigned long long>(-value) : static_cast<unsigned long long>(value);
            while (v > 0) {
                r.push_back(static_cast<char>('0' + v % 10));
                v /= 10;
            }
            if (neg) {
                r.push_back('-');
            }
            return r.reverse();
        }

        /** @brief Convert an unsigned integer to a lowercase hexadecimal string without prefix. */
        template<size_t Cap = 32>
        [[nodiscard]] constexpr FixedString<Cap> ToHex(unsigned long long value) {
            if (value == 0) {
                return FixedString<Cap>(std::string_view("0"));
            }
            constexpr char digits[] = "0123456789abcdef";
            FixedString<Cap> r;
            while (value > 0) {
                r.push_back(digits[value & 0xF]);
                value >>= 4;
            }
            return r.reverse();
        }

        /** @brief Wrap @p content between @p open and @p close. */
        template<size_t Cap = 256>
        [[nodiscard]] constexpr FixedString<Cap> Wrap(std::string_view open, std::string_view close,
                                                      std::string_view content) {
            FixedString<Cap> r;
            r.append(open);
            r.append(content);
            r.append(close);
            return r;
        }

        /** @brief Count occurrences of @p needle in @p haystack. */
        [[nodiscard]] constexpr size_t Count(std::string_view haystack, std::string_view needle) {
            if (needle.empty()) {
                return 0;
            }
            size_t n = 0;
            size_t pos = 0;
            while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
                ++n;
                pos += needle.size();
            }
            return n;
        }

    } // namespace String

    /**
     * @brief Promote a compile-time fixed string to static storage.
     *
     * @tparam Str Fixed-string-like value exposing @c view().
     * @return Pointer to a NUL-terminated string with static storage duration.
     *
     * @code{.cpp}
     * constexpr auto tag = FixedString("hello");
     * constexpr const char* ptr = StaticStr<tag>();
     * @endcode
     */
    template<auto Str>
        requires requires { Str.view(); }
    consteval const char* StaticStr() {
        return std::define_static_string(Str.view());
    }

    /** @name Reflection integration */
    /** @{ */

    /**
     * @brief Return the identifier of a reflected entity as a fixed string.
     * @tparam R Reflected entity.
     * @tparam Cap Result capacity.
     */
    template<std::meta::info R, size_t Cap = 256>
    consteval FixedString<Cap> NameOf() {
        return FixedString<Cap>(std::meta::identifier_of(R));
    }

    /**
     * @brief Return the display name of @p T as a fixed string.
     * @tparam T Reflected type.
     * @tparam Cap Result capacity.
     */
    template<typename T, size_t Cap = 256>
    consteval FixedString<Cap> TypeNameStr() {
        return FixedString<Cap>(std::meta::display_string_of(^^T));
    }

    /** @} */

    /** @brief User-defined literals for @ref FixedString. */
    namespace Literals {

        /** @brief Return the fixed string represented by the literal suffix @c _fs. */
        template<FixedString S>
        [[nodiscard]] consteval auto operator""_fs() {
            return S;
        }

    } // namespace Literals

} // namespace Sora

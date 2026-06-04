/**
 * @file FixedString.h
 * @brief Compile-time fixed-capacity string — the canonical NTTP-friendly string type.
 *
 * `FixedString<N>` is a structural type suitable for use as a non-type template parameter
 * (NTTP). It stores up to N characters (excluding the implicit NUL terminator) in a
 * `char[N+1]` buffer with O(1) `.size()`. All operations are `consteval`/`constexpr`
 * and designed for zero runtime cost.
 *
 * Design goals:
 * - **Structural**: trivially copyable, no pointers/references, valid NTTP.
 * - **Compile-time complete**: all algorithms (`find`, `substr`, `split`, `concat`,
 *   `replace`, `transform`, etc.) are `constexpr` and preferred `consteval`.
 * - **Zero overhead at runtime**: when used as NTTP or in constexpr contexts, the
 *   compiler folds everything; when instantiated at runtime, it's just a flat array.
 * - **Interop**: implicit conversion from string literals via CTAD, explicit
 *   conversions to `std::string_view`, and a `define_static_string` bridge for
 *   obtaining `const char*` with static storage duration.
 * - **Composable**: `operator+` for concatenation, format-like `Join`, `Repeat`.
 *
 * @ingroup Core
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <meta>
#include <string_view>
#include <utility>

namespace Mashiro {

    /**
     * @brief Fixed-capacity compile-time string.
     * @tparam N  Maximum number of characters (not counting NUL terminator).
     *
     * The internal storage is `char[N+1]`, always NUL-terminated. `len_` tracks the
     * actual string length (≤ N). This allows a FixedString<128> to hold shorter strings
     * without wasting the capacity semantics — important for NTTP deduction from
     * `consteval` results.
     */
    template<size_t N>
    struct FixedString {
        char buf_[N + 1]{}; ///< Raw character storage (NUL-terminated).
        size_t len_ = 0;    ///< Actual string length (≤ N).

        /** @name Constructors */
        /** @{ */

        /** @brief Default: empty string. */
        constexpr FixedString() = default;

        /** @brief From a string literal (CTAD deduces N from array size minus 1). */
        consteval FixedString(const char (&s)[N + 1]) : len_(N) {
            for (size_t i = 0; i <= N; ++i) {
                buf_[i] = s[i];
            }
        }

        /** @brief From a `string_view` (must fit within capacity N). */
        constexpr FixedString(std::string_view sv) : len_(sv.size()) {
            for (size_t i = 0; i < sv.size(); ++i) {
                buf_[i] = sv[i];
            }
            buf_[sv.size()] = '\0';
        }

        /** @brief From a char repeated `count` times. */
        constexpr FixedString(size_t count, char c) : len_(count) {
            for (size_t i = 0; i < count; ++i) {
                buf_[i] = c;
            }
            buf_[count] = '\0';
        }

        /** @} */

        /** @name Observers */
        /** @{ */

        [[nodiscard]] constexpr size_t size() const { return len_; }
        [[nodiscard]] constexpr size_t capacity() const { return N; }
        [[nodiscard]] constexpr bool empty() const { return len_ == 0; }
        [[nodiscard]] constexpr const char* data() const { return buf_; }
        [[nodiscard]] constexpr const char* c_str() const { return buf_; }
        [[nodiscard]] constexpr char operator[](size_t i) const { return buf_[i]; }
        [[nodiscard]] constexpr char& operator[](size_t i) { return buf_[i]; }
        [[nodiscard]] constexpr char front() const { return buf_[0]; }
        [[nodiscard]] constexpr char back() const { return buf_[len_ - 1]; }

        /** @brief Implicit conversion to string_view. */
        [[nodiscard]] constexpr operator std::string_view() const { return {buf_, len_}; }

        /** @brief Explicit view accessor. */
        [[nodiscard]] constexpr std::string_view view() const { return {buf_, len_}; }

        /** @} */

        /** @name Iterators */
        /** @{ */

        [[nodiscard]] constexpr const char* begin() const { return buf_; }
        [[nodiscard]] constexpr const char* end() const { return buf_ + len_; }
        [[nodiscard]] constexpr char* begin() { return buf_; }
        [[nodiscard]] constexpr char* end() { return buf_ + len_; }

        /** @} */

        /** @name Comparison */
        /** @{ */

        template<size_t M>
        [[nodiscard]] constexpr auto operator<=>(const FixedString<M>& o) const {
            return view() <=> o.view();
        }

        template<size_t M>
        [[nodiscard]] constexpr bool operator==(const FixedString<M>& o) const {
            return view() == o.view();
        }

        [[nodiscard]] constexpr auto operator<=>(std::string_view sv) const {
            return view() <=> sv;
        }

        [[nodiscard]] constexpr bool operator==(std::string_view sv) const { return view() == sv; }

        /** @} */

        /** @name Search */
        /** @{ */

        static constexpr size_t npos = std::string_view::npos;

        [[nodiscard]] constexpr size_t find(char c, size_t pos = 0) const {
            for (size_t i = pos; i < len_; ++i) {
                if (buf_[i] == c) {
                    return i;
                }
            }
            return npos;
        }

        [[nodiscard]] constexpr size_t find(std::string_view s, size_t pos = 0) const {
            return view().find(s, pos);
        }

        [[nodiscard]] constexpr size_t rfind(char c, size_t pos = npos) const {
            size_t start = (pos < len_) ? pos : (len_ > 0 ? len_ - 1 : 0);
            for (size_t i = start + 1; i > 0; --i) {
                if (buf_[i - 1] == c) {
                    return i - 1;
                }
            }
            return npos;
        }

        [[nodiscard]] constexpr size_t rfind(std::string_view s, size_t pos = npos) const {
            return view().rfind(s, pos);
        }

        [[nodiscard]] constexpr bool contains(char c) const { return find(c) != npos; }
        [[nodiscard]] constexpr bool contains(std::string_view s) const { return find(s) != npos; }

        [[nodiscard]] constexpr bool starts_with(std::string_view s) const {
            return view().starts_with(s);
        }

        [[nodiscard]] constexpr bool ends_with(std::string_view s) const {
            return view().ends_with(s);
        }

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

        /** @brief Everything before the first occurrence of @p delim (or the whole string). */
        [[nodiscard]] constexpr FixedString before(char delim) const {
            size_t p = find(delim);
            return (p == npos) ? *this : substr(0, p);
        }

        /** @brief Everything after the first occurrence of @p delim (or empty). */
        [[nodiscard]] constexpr FixedString after(char delim) const {
            size_t p = find(delim);
            return (p == npos) ? FixedString{} : substr(p + 1);
        }

        /** @brief Everything before the last occurrence of @p delim. */
        [[nodiscard]] constexpr FixedString before_last(char delim) const {
            size_t p = rfind(delim);
            return (p == npos) ? *this : substr(0, p);
        }

        /** @brief Everything after the last occurrence of @p delim. */
        [[nodiscard]] constexpr FixedString after_last(char delim) const {
            size_t p = rfind(delim);
            return (p == npos) ? FixedString{} : substr(p + 1);
        }

        /** @} */

        /** @name Transformation (returns new FixedString) */
        /** @{ */

        /** @brief Remove @p prefix from the front if present. */
        [[nodiscard]] constexpr FixedString strip_prefix(std::string_view prefix) const {
            if (starts_with(prefix)) {
                return substr(prefix.size());
            }
            return *this;
        }

        /** @brief Remove @p suffix from the back if present. */
        [[nodiscard]] constexpr FixedString strip_suffix(std::string_view suffix) const {
            if (ends_with(suffix)) {
                return substr(0, len_ - suffix.size());
            }
            return *this;
        }

        /** @brief Replace all occurrences of char @p from with @p to. */
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

        /** @brief Convert to uppercase (ASCII only). */
        [[nodiscard]] constexpr FixedString to_upper() const {
            return transform([](char c) -> char {
                return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
            });
        }

        /** @brief Convert to lowercase (ASCII only). */
        [[nodiscard]] constexpr FixedString to_lower() const {
            return transform([](char c) -> char {
                return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
            });
        }

        /** @brief Trim leading and trailing whitespace. */
        [[nodiscard]] constexpr FixedString trim() const {
            size_t l = 0, r = len_;
            while (l < r &&
                   (buf_[l] == ' ' || buf_[l] == '\t' || buf_[l] == '\n' || buf_[l] == '\r')) {
                ++l;
            }
            while (r > l && (buf_[r - 1] == ' ' || buf_[r - 1] == '\t' || buf_[r - 1] == '\n' ||
                             buf_[r - 1] == '\r')) {
                --r;
            }
            return substr(l, r - l);
        }

        /** @brief Reverse the string. */
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

        /** @name Path operations (both `/` and `\` separators) */
        /** @{ */

        /** @brief Filename stem: `"foo/bar/baz.cpp"` → `"baz"`. */
        [[nodiscard]] constexpr FixedString stem() const {
            auto name = after_last_sep();
            size_t dot = name.rfind('.');
            return (dot == npos) ? name : name.substr(0, dot);
        }

        /** @brief File extension (with dot): `"foo.cpp"` → `".cpp"`. */
        [[nodiscard]] constexpr FixedString extension() const {
            auto name = after_last_sep();
            size_t dot = name.rfind('.');
            return (dot == npos) ? FixedString{} : name.substr(dot);
        }

        /** @brief Filename (last component): `"foo/bar/baz.cpp"` → `"baz.cpp"`. */
        [[nodiscard]] constexpr FixedString filename() const { return after_last_sep(); }

        /** @brief Parent directory path: `"foo/bar/baz.cpp"` → `"foo/bar"`. */
        [[nodiscard]] constexpr FixedString parent_path() const {
            size_t p = rfind_sep();
            return (p == npos) ? FixedString{} : substr(0, p);
        }

        /** @brief Parent directory name (immediate): `"foo/bar/baz.cpp"` → `"bar"`. */
        [[nodiscard]] constexpr FixedString parent_name() const {
            auto pp = parent_path();
            return pp.after_last_sep().empty() ? pp : pp.after_last_sep();
        }

        /** @} */

    private:
        [[nodiscard]] constexpr bool is_sep(char c) const { return c == '/' || c == '\\'; }

        [[nodiscard]] constexpr size_t rfind_sep() const {
            for (size_t i = len_; i > 0; --i) {
                if (is_sep(buf_[i - 1])) {
                    return i - 1;
                }
            }
            return npos;
        }

        [[nodiscard]] constexpr FixedString after_last_sep() const {
            size_t p = rfind_sep();
            return (p == npos) ? *this : substr(p + 1);
        }

    public:
        /** @name Append / Push (in-place mutation) */
        /** @{ */

        /** @brief Append a single character. */
        constexpr FixedString& push_back(char c) {
            buf_[len_++] = c;
            buf_[len_] = '\0';
            return *this;
        }

        /** @brief Append a string_view. */
        constexpr FixedString& append(std::string_view sv) {
            for (char c : sv) {
                buf_[len_++] = c;
            }
            buf_[len_] = '\0';
            return *this;
        }

        /** @brief Append a char repeated @p count times. */
        constexpr FixedString& append(char c, size_t count = 1) {
            for (size_t i = 0; i < count; ++i) {
                buf_[len_++] = c;
            }
            buf_[len_] = '\0';
            return *this;
        }

        /** @brief Clear contents. */
        constexpr void clear() {
            len_ = 0;
            buf_[0] = '\0';
        }

        /** @} */
    };

    /** @brief CTAD: deduce N from string literal (array includes NUL → N = array_size - 1). */
    template<size_t M>
    FixedString(const char (&)[M]) -> FixedString<M - 1>;

    /** @name Concatenation */
    /** @{ */

    /** @brief Concatenate two FixedStrings at compile time. */
    template<size_t A, size_t B>
    [[nodiscard]] constexpr FixedString<A + B> operator+(const FixedString<A>& a,
                                                         const FixedString<B>& b) {
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

    /** @brief Concatenate FixedString + string_view (result capacity = A + 256). */
    template<size_t A>
    [[nodiscard]] constexpr auto operator+(const FixedString<A>& a, std::string_view b) {
        FixedString<A + 256> r;
        r.append(a.view());
        r.append(b);
        return r;
    }

    /** @} */

    /**
     * @brief Free-function algorithms operating on FixedString.
     */
    namespace String {

        /** @brief Repeat a string @p n times. */
        template<size_t Cap = 1024>
        [[nodiscard]] constexpr FixedString<Cap> Repeat(std::string_view s, size_t n) {
            FixedString<Cap> r;
            for (size_t i = 0; i < n; ++i) {
                r.append(s);
            }
            return r;
        }

        /** @brief Join multiple string_views with a separator. */
        template<size_t Cap = 1024, size_t Count>
        [[nodiscard]] constexpr FixedString<Cap>
        Join(std::string_view sep, const std::array<std::string_view, Count>& parts) {
            FixedString<Cap> r;
            for (size_t i = 0; i < Count; ++i) {
                if (i > 0) {
                    r.append(sep);
                }
                r.append(parts[i]);
            }
            return r;
        }

        /** @brief Join two string_views with a separator. */
        template<size_t Cap = 256>
        [[nodiscard]] constexpr FixedString<Cap> Join(std::string_view sep, std::string_view a,
                                                      std::string_view b) {
            FixedString<Cap> r;
            r.append(a);
            r.append(sep);
            r.append(b);
            return r;
        }

        /** @brief Compile-time integer to decimal string. */
        template<size_t Cap = 32>
        [[nodiscard]] constexpr FixedString<Cap> FromInt(long long value) {
            if (value == 0) {
                return FixedString<Cap>(std::string_view("0"));
            }
            FixedString<Cap> r;
            bool neg = value < 0;
            unsigned long long v = neg ? static_cast<unsigned long long>(-value)
                                       : static_cast<unsigned long long>(value);
            while (v > 0) {
                r.push_back(static_cast<char>('0' + v % 10));
                v /= 10;
            }
            if (neg) {
                r.push_back('-');
            }
            return r.reverse();
        }

        /** @brief Compile-time unsigned integer to hexadecimal string (no prefix). */
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

        /** @brief Wrap a string in delimiters: `Wrap("[", "]", "tag")` → `"[tag]"`. */
        template<size_t Cap = 256>
        [[nodiscard]] constexpr FixedString<Cap> Wrap(std::string_view open, std::string_view close,
                                                      std::string_view content) {
            FixedString<Cap> r;
            r.append(open);
            r.append(content);
            r.append(close);
            return r;
        }

        /** @brief Count occurrences of a substring. */
        [[nodiscard]] constexpr size_t Count(std::string_view haystack, std::string_view needle) {
            if (needle.empty()) {
                return 0;
            }
            size_t n = 0, pos = 0;
            while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
                ++n;
                pos += needle.size();
            }
            return n;
        }

    } // namespace Str

    /**
     * @brief Promote a compile-time FixedString to static storage, returning `const char*`.
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

    /** @brief Get the identifier of a reflected entity as a FixedString. */
    template<std::meta::info R, size_t Cap = 256>
    consteval FixedString<Cap> NameOf() {
        return FixedString<Cap>(std::meta::identifier_of(R));
    }

    /** @brief Get the display name of a type as a FixedString. */
    template<typename T, size_t Cap = 256>
    consteval FixedString<Cap> TypeNameStr() {
        return FixedString<Cap>(std::meta::display_string_of(^^T));
    }

    /** @} */

    /** @brief User-defined literals for FixedString. */
    namespace Literals {

        /** @brief `"hello"_fs` → `FixedString<5>{"hello"}`. */
        template<FixedString S>
        [[nodiscard]] consteval auto operator""_fs() {
            return S;
        }

    } // namespace Literals

} // namespace Mashiro

/**
 * @file FixedStringTest.cpp
 * @brief Comprehensive tests for Mashiro::FixedString — compile-time fixed-capacity string.
 *
 * Covers: construction, observers, comparison, search, substring extraction,
 * transformation, path operations, append/push, concatenation, FixedStr:: algorithms,
 * static storage promotion, reflection integration, and UDL.
 */
#include "Mashiro/Core/FixedString.h"

#include "Support/Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <string_view>

using namespace Mashiro;
using namespace std::string_view_literals;

// =============================================================================
// §1  Construction & Observers
// =============================================================================

TEST_CASE("FixedString default construction", AUTO_TAG) {
    constexpr FixedString<32> s;
    STATIC_REQUIRE(s.size() == 0);
    STATIC_REQUIRE(s.empty());
    STATIC_REQUIRE(s.capacity() == 32);
    STATIC_REQUIRE(s.view() == "");
}

TEST_CASE("FixedString from string literal via CTAD", AUTO_TAG) {
    constexpr FixedString hello("hello");
    STATIC_REQUIRE(hello.size() == 5);
    STATIC_REQUIRE(!hello.empty());
    STATIC_REQUIRE(hello.capacity() == 5);
    STATIC_REQUIRE(hello.view() == "hello");
    STATIC_REQUIRE(hello.front() == 'h');
    STATIC_REQUIRE(hello.back() == 'o');
    STATIC_REQUIRE(hello[2] == 'l');
}

TEST_CASE("FixedString from string_view", AUTO_TAG) {
    constexpr FixedString<64> s(std::string_view("world"));
    STATIC_REQUIRE(s.size() == 5);
    STATIC_REQUIRE(s.capacity() == 64);
    STATIC_REQUIRE(s.view() == "world");
}

TEST_CASE("FixedString from repeated char", AUTO_TAG) {
    constexpr FixedString<10> s(4, 'x');
    STATIC_REQUIRE(s.size() == 4);
    STATIC_REQUIRE(s.view() == "xxxx");
}

TEST_CASE("FixedString c_str is NUL-terminated", AUTO_TAG) {
    constexpr FixedString abc("abc");
    STATIC_REQUIRE(abc.c_str()[3] == '\0');
    STATIC_REQUIRE(abc.data()[3] == '\0');
}

// =============================================================================
// §2  Iterators
// =============================================================================

TEST_CASE("FixedString range-for iteration", AUTO_TAG) {
    constexpr FixedString s("abc");
    constexpr auto sum = [&]() consteval {
        int total = 0;
        for (char c : s) total += c;
        return total;
    }();
    STATIC_REQUIRE(sum == 'a' + 'b' + 'c');
}

// =============================================================================
// §3  Comparison
// =============================================================================

TEST_CASE("FixedString equality across different capacities", AUTO_TAG) {
    constexpr FixedString<5>  a("hello");
    constexpr FixedString<64> b(std::string_view("hello"));
    STATIC_REQUIRE(a == b);
    STATIC_REQUIRE(!(a != b));
}

TEST_CASE("FixedString ordering", AUTO_TAG) {
    constexpr FixedString a("apple");
    constexpr FixedString b("banana");
    STATIC_REQUIRE(a < b);
    STATIC_REQUIRE(b > a);
    STATIC_REQUIRE(a <= a);
}

TEST_CASE("FixedString comparison with string_view", AUTO_TAG) {
    constexpr FixedString s("test");
    STATIC_REQUIRE(s == "test"sv);
    STATIC_REQUIRE(s != "other"sv);
    STATIC_REQUIRE(s < "xyz"sv);
}

// =============================================================================
// §4  Search
// =============================================================================

TEST_CASE("FixedString find char", AUTO_TAG) {
    constexpr FixedString s("hello world");
    STATIC_REQUIRE(s.find('o') == 4);
    STATIC_REQUIRE(s.find('o', 5) == 7);
    STATIC_REQUIRE(s.find('z') == FixedString<11>::npos);
}

TEST_CASE("FixedString find string_view", AUTO_TAG) {
    constexpr FixedString s("abcdefg");
    STATIC_REQUIRE(s.find("cde") == 2);
    STATIC_REQUIRE(s.find("xyz") == FixedString<7>::npos);
}

TEST_CASE("FixedString rfind char", AUTO_TAG) {
    constexpr FixedString s("foo/bar/baz");
    STATIC_REQUIRE(s.rfind('/') == 7);
    STATIC_REQUIRE(s.rfind('/', 6) == 3);
    STATIC_REQUIRE(s.rfind('z') == 10);
}

TEST_CASE("FixedString rfind string_view", AUTO_TAG) {
    constexpr FixedString s("aababab");
    STATIC_REQUIRE(s.rfind("ab") == 5);
}

TEST_CASE("FixedString contains", AUTO_TAG) {
    constexpr FixedString s("hello world");
    STATIC_REQUIRE(s.contains('w'));
    STATIC_REQUIRE(!s.contains('z'));
    STATIC_REQUIRE(s.contains("world"));
    STATIC_REQUIRE(!s.contains("xyz"));
}

TEST_CASE("FixedString starts_with / ends_with", AUTO_TAG) {
    constexpr FixedString s("prefix_suffix");
    STATIC_REQUIRE(s.starts_with("prefix"));
    STATIC_REQUIRE(!s.starts_with("suffix"));
    STATIC_REQUIRE(s.ends_with("suffix"));
    STATIC_REQUIRE(!s.ends_with("prefix"));
}

TEST_CASE("FixedString count char", AUTO_TAG) {
    constexpr FixedString s("banana");
    STATIC_REQUIRE(s.count('a') == 3);
    STATIC_REQUIRE(s.count('n') == 2);
    STATIC_REQUIRE(s.count('z') == 0);
}

// =============================================================================
// §5  Substring extraction
// =============================================================================

TEST_CASE("FixedString substr", AUTO_TAG) {
    constexpr FixedString s("hello world");
    STATIC_REQUIRE(s.substr(0, 5) == "hello"sv);
    STATIC_REQUIRE(s.substr(6) == "world"sv);
    STATIC_REQUIRE(s.substr(6, 100) == "world"sv); // clamped
}

TEST_CASE("FixedString before / after", AUTO_TAG) {
    constexpr FixedString s("key=value");
    STATIC_REQUIRE(s.before('=') == "key"sv);
    STATIC_REQUIRE(s.after('=') == "value"sv);
    STATIC_REQUIRE(s.before('z') == s.view()); // not found → whole string
    STATIC_REQUIRE(s.after('z') == ""sv);       // not found → empty
}

TEST_CASE("FixedString before_last / after_last", AUTO_TAG) {
    constexpr FixedString s("a.b.c.d");
    STATIC_REQUIRE(s.before_last('.') == "a.b.c"sv);
    STATIC_REQUIRE(s.after_last('.') == "d"sv);
}

// =============================================================================
// §6  Transformation
// =============================================================================

TEST_CASE("FixedString strip_prefix / strip_suffix", AUTO_TAG) {
    constexpr FixedString s("TestFoo");
    STATIC_REQUIRE(s.strip_prefix("Test") == "Foo"sv);
    STATIC_REQUIRE(s.strip_suffix("Foo") == "Test"sv);
    STATIC_REQUIRE(s.strip_prefix("Xyz") == s.view()); // no match → unchanged
    STATIC_REQUIRE(s.strip_suffix("Xyz") == s.view());
}

TEST_CASE("FixedString replace_char", AUTO_TAG) {
    constexpr FixedString s("a-b-c");
    STATIC_REQUIRE(s.replace_char('-', '_') == "a_b_c"sv);
}

TEST_CASE("FixedString to_upper / to_lower", AUTO_TAG) {
    constexpr FixedString s("Hello World");
    STATIC_REQUIRE(s.to_upper() == "HELLO WORLD"sv);
    STATIC_REQUIRE(s.to_lower() == "hello world"sv);
}

TEST_CASE("FixedString trim", AUTO_TAG) {
    constexpr FixedString<32> s(std::string_view("  \t hello \n "));
    STATIC_REQUIRE(s.trim() == "hello"sv);
}

TEST_CASE("FixedString trim empty result", AUTO_TAG) {
    constexpr FixedString<16> s(std::string_view("   "));
    STATIC_REQUIRE(s.trim() == ""sv);
    STATIC_REQUIRE(s.trim().empty());
}

TEST_CASE("FixedString reverse", AUTO_TAG) {
    constexpr FixedString s("abcde");
    STATIC_REQUIRE(s.reverse() == "edcba"sv);
}

TEST_CASE("FixedString reverse single char", AUTO_TAG) {
    constexpr FixedString s("x");
    STATIC_REQUIRE(s.reverse() == "x"sv);
}

TEST_CASE("FixedString transform custom", AUTO_TAG) {
    constexpr FixedString s("abc");
    constexpr auto result = s.transform([](char c) -> char { return static_cast<char>(c + 1); });
    STATIC_REQUIRE(result == "bcd"sv);
}

// =============================================================================
// §7  Path operations
// =============================================================================

TEST_CASE("FixedString stem (Unix path)", AUTO_TAG) {
    constexpr FixedString<64> s(std::string_view("foo/bar/baz.cpp"));
    STATIC_REQUIRE(s.stem() == "baz"sv);
}

TEST_CASE("FixedString stem (Windows path)", AUTO_TAG) {
    constexpr FixedString<64> s(std::string_view("C:\\Users\\file.txt"));
    STATIC_REQUIRE(s.stem() == "file"sv);
}

TEST_CASE("FixedString stem (no extension)", AUTO_TAG) {
    constexpr FixedString<32> s(std::string_view("foo/bar/Makefile"));
    STATIC_REQUIRE(s.stem() == "Makefile"sv);
}

TEST_CASE("FixedString extension", AUTO_TAG) {
    constexpr FixedString<64> s(std::string_view("archive.tar.gz"));
    STATIC_REQUIRE(s.extension() == ".gz"sv);
}

TEST_CASE("FixedString extension (none)", AUTO_TAG) {
    constexpr FixedString<32> s(std::string_view("noext"));
    STATIC_REQUIRE(s.extension() == ""sv);
    STATIC_REQUIRE(s.extension().empty());
}

TEST_CASE("FixedString filename", AUTO_TAG) {
    constexpr FixedString<64> s(std::string_view("/usr/local/bin/clang"));
    STATIC_REQUIRE(s.filename() == "clang"sv);
}

TEST_CASE("FixedString parent_path", AUTO_TAG) {
    constexpr FixedString<64> s(std::string_view("a/b/c.txt"));
    STATIC_REQUIRE(s.parent_path() == "a/b"sv);
}

TEST_CASE("FixedString parent_path (no separator)", AUTO_TAG) {
    constexpr FixedString<32> s(std::string_view("justfile"));
    STATIC_REQUIRE(s.parent_path() == ""sv);
}

TEST_CASE("FixedString parent_name", AUTO_TAG) {
    constexpr FixedString<64> s(std::string_view("tests/Core/HashTest.cpp"));
    STATIC_REQUIRE(s.parent_name() == "Core"sv);
}

TEST_CASE("FixedString parent_name (Windows)", AUTO_TAG) {
    constexpr FixedString<64> s(std::string_view("G:\\Teaching\\Vulkan\\file.h"));
    STATIC_REQUIRE(s.parent_name() == "Vulkan"sv);
}

// =============================================================================
// §8  Append / Push
// =============================================================================

TEST_CASE("FixedString push_back", AUTO_TAG) {
    constexpr auto result = []() consteval {
        FixedString<8> s;
        s.push_back('a');
        s.push_back('b');
        s.push_back('c');
        return s;
    }();
    STATIC_REQUIRE(result.size() == 3);
    STATIC_REQUIRE(result == "abc"sv);
}

TEST_CASE("FixedString append string_view", AUTO_TAG) {
    constexpr auto result = []() consteval {
        FixedString<32> s;
        s.append("hello");
        s.append(" ");
        s.append("world");
        return s;
    }();
    STATIC_REQUIRE(result == "hello world"sv);
}

TEST_CASE("FixedString append char repeated", AUTO_TAG) {
    constexpr auto result = []() consteval {
        FixedString<16> s(std::string_view("hi"));
        s.append('!', 3);
        return s;
    }();
    STATIC_REQUIRE(result == "hi!!!"sv);
}

TEST_CASE("FixedString clear", AUTO_TAG) {
    constexpr auto result = []() consteval {
        FixedString<16> s(std::string_view("data"));
        s.clear();
        return s;
    }();
    STATIC_REQUIRE(result.empty());
    STATIC_REQUIRE(result.size() == 0);
}

// =============================================================================
// §9  Concatenation (operator+)
// =============================================================================

TEST_CASE("FixedString concat two FixedStrings", AUTO_TAG) {
    constexpr FixedString a("hello");
    constexpr FixedString b(" world");
    constexpr auto c = a + b;
    STATIC_REQUIRE(c == "hello world"sv);
    STATIC_REQUIRE(c.capacity() == 5 + 6);
}

TEST_CASE("FixedString concat with string_view", AUTO_TAG) {
    constexpr FixedString a("foo");
    constexpr auto b = a + std::string_view("bar");
    STATIC_REQUIRE(b == "foobar"sv);
}

// =============================================================================
// §10  Str:: free-function algorithms
// =============================================================================

TEST_CASE("Str::Repeat", AUTO_TAG) {
    constexpr auto result = String::Repeat("ab", 4);
    STATIC_REQUIRE(result == "abababab"sv);
}

TEST_CASE("Str::Repeat zero times", AUTO_TAG) {
    constexpr auto result = String::Repeat("x", 0);
    STATIC_REQUIRE(result.empty());
}

TEST_CASE("Str::Join array", AUTO_TAG) {
    constexpr std::array<std::string_view, 3> parts = {"a", "b", "c"};
    constexpr auto result = String::Join<64>(", ", parts);
    STATIC_REQUIRE(result == "a, b, c"sv);
}

TEST_CASE("Str::Join two strings", AUTO_TAG) {
    constexpr auto result = String::Join(".", "Core", "Hash");
    STATIC_REQUIRE(result == "Core.Hash"sv);
}

TEST_CASE("Str::FromInt positive", AUTO_TAG) {
    STATIC_REQUIRE(String::FromInt(12345) == "12345"sv);
}

TEST_CASE("Str::FromInt negative", AUTO_TAG) {
    STATIC_REQUIRE(String::FromInt(-42) == "-42"sv);
}

TEST_CASE("Str::FromInt zero", AUTO_TAG) {
    STATIC_REQUIRE(String::FromInt(0) == "0"sv);
}

TEST_CASE("Str::ToHex", AUTO_TAG) {
    STATIC_REQUIRE(String::ToHex(0xFF) == "ff"sv);
    STATIC_REQUIRE(String::ToHex(0) == "0"sv);
    STATIC_REQUIRE(String::ToHex(0xDEAD) == "dead"sv);
}

TEST_CASE("Str::Wrap", AUTO_TAG) {
    constexpr auto result = String::Wrap("[", "]", "tag");
    STATIC_REQUIRE(result == "[tag]"sv);
}

TEST_CASE("Str::Count", AUTO_TAG) {
    STATIC_REQUIRE(String::Count("abcabcabc", "abc") == 3);
    STATIC_REQUIRE(String::Count("aaa", "aa") == 1); // non-overlapping
    STATIC_REQUIRE(String::Count("hello", "xyz") == 0);
    STATIC_REQUIRE(String::Count("test", "") == 0); // empty needle
}

// =============================================================================
// §11  StaticStr (static storage promotion)
// =============================================================================

TEST_CASE("StaticStr promotes to const char*", AUTO_TAG) {
    constexpr auto tag = FixedString<16>(std::string_view("[Core.Test]"));
    constexpr const char* ptr = StaticStr<tag>();
    REQUIRE(std::string_view(ptr) == "[Core.Test]"sv);
}

// =============================================================================
// §12  Reflection integration
// =============================================================================

namespace {
    struct ReflectMe {
        int x;
    };
}

TEST_CASE("NameOf reflected entity", AUTO_TAG) {
    constexpr auto name = NameOf<^^ReflectMe>();
    STATIC_REQUIRE(name == "ReflectMe"sv);
}

TEST_CASE("TypeNameStr", AUTO_TAG) {
    constexpr auto name = TypeNameStr<int>();
    REQUIRE(name.contains("int"));
}

// =============================================================================
// §13  User-defined literal
// =============================================================================

TEST_CASE("UDL _fs", AUTO_TAG) {
    using namespace Mashiro::Literals;
    constexpr auto s = "compile-time"_fs;
    STATIC_REQUIRE(s.size() == 12);
    STATIC_REQUIRE(s == "compile-time"sv);
}

// =============================================================================
// §14  Edge cases
// =============================================================================

TEST_CASE("FixedString empty string literal", AUTO_TAG) {
    constexpr FixedString s("");
    STATIC_REQUIRE(s.size() == 0);
    STATIC_REQUIRE(s.empty());
    STATIC_REQUIRE(s.view() == ""sv);
}

TEST_CASE("FixedString single char literal", AUTO_TAG) {
    constexpr FixedString s("x");
    STATIC_REQUIRE(s.size() == 1);
    STATIC_REQUIRE(s.front() == 'x');
    STATIC_REQUIRE(s.back() == 'x');
}

TEST_CASE("FixedString substr at boundary", AUTO_TAG) {
    constexpr FixedString s("abc");
    STATIC_REQUIRE(s.substr(3) == ""sv);    // pos == size → empty
    STATIC_REQUIRE(s.substr(0, 0) == ""sv); // count == 0 → empty
}

TEST_CASE("FixedString path with no directory", AUTO_TAG) {
    constexpr FixedString<32> s(std::string_view("file.txt"));
    STATIC_REQUIRE(s.filename() == "file.txt"sv);
    STATIC_REQUIRE(s.stem() == "file"sv);
    STATIC_REQUIRE(s.extension() == ".txt"sv);
    STATIC_REQUIRE(s.parent_path() == ""sv);
}

TEST_CASE("FixedString implicit string_view conversion", AUTO_TAG) {
    static constexpr FixedString s("test");
    constexpr std::string_view sv = s;
    STATIC_REQUIRE(sv == "test");
}

TEST_CASE("FixedString NUL-termination after mutation", AUTO_TAG) {
    constexpr auto result = []() consteval {
        FixedString<16> s;
        s.append("hi");
        s.push_back('!');
        // Verify NUL at correct position
        return s.c_str()[3] == '\0';
    }();
    STATIC_REQUIRE(result);
}

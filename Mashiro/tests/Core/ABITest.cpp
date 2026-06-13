/**
 * @file ABITest.cpp
 * @brief Tests for reflection-driven compile-time name mangling (Core/ABI.h).
 *
 * Three tiers, mirroring the design spec:
 *  1. **Compile-time golden assertions** (`STATIC_REQUIRE`) — curated entities
 *     mapped to expected mangled strings, for both ABIs. MSVC strings are the
 *     compiler's real output (captured via `llvm-nm` / `typeid().raw_name()`);
 *     Itanium strings are hand-derived from the ABI spec.
 *  2. **Self-consistency** — for the type matrix, `MSVC::Mangle(^^T)` equals
 *     `typeid(T).raw_name()`, with the compiler itself as oracle.
 *  3. **Round-trip** (runtime) — `Runtime::Demangle(MSVC::Mangle(^^T))` recovers
 *     a human signature, exercising the DbgHelp / libc++abi path.
 *
 * Each mangling sub-rule is its own `TEST_CASE` so a regression localises.
 */
#include "Mashiro/Core/ABI.h"

#include "Support/Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <typeinfo>

namespace mabi = Mashiro::ABI;

// =============================================================================
// Fixtures — types/functions/variables exercised across the matrix.
// =============================================================================

namespace abitest {

    struct Plain { int a; };                 // struct → MSVC 'U'
    class  Klass { int a; public: int f(); };// class  → MSVC 'V'
    union  Onion { int a; float b; };        // union  → MSVC 'T'
    enum class Color { Red, Green };         // enum   → MSVC 'W4'

    namespace ns { struct Inner { int x; }; } // nested scope

    // Free functions / variables for the symbol direction.
    int  free_two(int, double);
    void free_none();

    // Repeated-type parameters drive the compression engines.
    void dup_ptr(const char*, const char*);
    void dup_named(Plain*, Plain*);

} // namespace abitest

// Namespace-scope variables (must have external linkage to mangle predictably).
int g_int;
const int g_cint = 1;
int* g_ptr;
const int* g_cptr;

// =============================================================================
// §1  MSVC — type direction (oracle: typeid raw_name(), see tier 2 below)
// =============================================================================

TEST_CASE("MSVC mangles builtin types to raw_name letters", AUTO_TAG) {
    STATIC_REQUIRE(mabi::MSVC::Mangle(^^int)    == std::string_view{".H"});
    STATIC_REQUIRE(mabi::MSVC::Mangle(^^double) == std::string_view{".N"});
    STATIC_REQUIRE(mabi::MSVC::Mangle(^^char)   == std::string_view{".D"});
    STATIC_REQUIRE(mabi::MSVC::Mangle(^^bool)   == std::string_view{"._N"});
}

TEST_CASE("MSVC __int128 letters avoid the bool collision", AUTO_TAG) {
    // Verified via clang-cl + llvm-nm: __int128 → _L, unsigned __int128 → _M.
    STATIC_REQUIRE(mabi::MSVC::Mangle(^^__int128)          == std::string_view{"._L"});
    STATIC_REQUIRE(mabi::MSVC::Mangle(^^unsigned __int128) == std::string_view{"._M"});
}

TEST_CASE("MSVC class-key tags: struct U, class V, union T, enum W4", AUTO_TAG) {
    STATIC_REQUIRE(mabi::MSVC::Mangle(^^abitest::Plain) == std::string_view{".?AUPlain@abitest@@"});
    STATIC_REQUIRE(mabi::MSVC::Mangle(^^abitest::Klass) == std::string_view{".?AVKlass@abitest@@"});
    STATIC_REQUIRE(mabi::MSVC::Mangle(^^abitest::Onion) == std::string_view{".?ATOnion@abitest@@"});
    STATIC_REQUIRE(mabi::MSVC::Mangle(^^abitest::Color) == std::string_view{".?AW4Color@abitest@@"});
}

TEST_CASE("MSVC pointer/const-pointer indirection PEA/PEB", AUTO_TAG) {
    STATIC_REQUIRE(mabi::MSVC::Mangle(^^int*)       == std::string_view{".PEAH"});
    STATIC_REQUIRE(mabi::MSVC::Mangle(^^const int*) == std::string_view{".PEBH"});
}

// =============================================================================
// §2  MSVC — variable direction (oracle: llvm-nm on real .obj)
// =============================================================================

TEST_CASE("MSVC variables: data code 3, storage A/B, ptr E-suffix", AUTO_TAG) {
    STATIC_REQUIRE(mabi::MSVC::Mangle(^^g_int)  == std::string_view{"?g_int@@3HA"});
    STATIC_REQUIRE(mabi::MSVC::Mangle(^^g_cint) == std::string_view{"?g_cint@@3HB"});
    // Pointer variables carry a trailing E<pointee-cv> storage modifier.
    STATIC_REQUIRE(mabi::MSVC::Mangle(^^g_ptr)  == std::string_view{"?g_ptr@@3PEAHEA"});
    STATIC_REQUIRE(mabi::MSVC::Mangle(^^g_cptr) == std::string_view{"?g_cptr@@3PEBHEB"});
}

// =============================================================================
// §3  MSVC — function direction (oracle: llvm-nm on real .obj)
// =============================================================================

TEST_CASE("MSVC free functions: Y storage, A cc, param list, void X", AUTO_TAG) {
    STATIC_REQUIRE(mabi::MSVC::Mangle(^^abitest::free_two)  == std::string_view{"?free_two@abitest@@YAHHN@Z"});
    STATIC_REQUIRE(mabi::MSVC::Mangle(^^abitest::free_none) == std::string_view{"?free_none@abitest@@YAXXZ"});
}

// =============================================================================
// §4  Self-consistency — MSVC type mangling == typeid raw_name (compile time)
// =============================================================================

TEST_CASE("MSVC type mangling matches typeid raw_name (compiler oracle)", AUTO_TAG) {
    CHECK(mabi::MSVC::Mangle(^^int)            == std::string_view{typeid(int).raw_name()});
    CHECK(mabi::MSVC::Mangle(^^double)         == std::string_view{typeid(double).raw_name()});
    CHECK(mabi::MSVC::Mangle(^^abitest::Plain) == std::string_view{typeid(abitest::Plain).raw_name()});
    CHECK(mabi::MSVC::Mangle(^^abitest::Klass) == std::string_view{typeid(abitest::Klass).raw_name()});
    CHECK(mabi::MSVC::Mangle(^^abitest::Onion) == std::string_view{typeid(abitest::Onion).raw_name()});
    CHECK(mabi::MSVC::Mangle(^^abitest::Color) == std::string_view{typeid(abitest::Color).raw_name()});
    CHECK(mabi::MSVC::Mangle(^^int*)           == std::string_view{typeid(int*).raw_name()});
    CHECK(mabi::MSVC::Mangle(^^const int*)     == std::string_view{typeid(const int*).raw_name()});
}

// =============================================================================
// §5  Itanium — type direction (golden strings from the ABI spec §5.1.5)
// =============================================================================

TEST_CASE("Itanium builtin type codes", AUTO_TAG) {
    STATIC_REQUIRE(mabi::Itanium::Mangle(^^void)               == std::string_view{"v"});
    STATIC_REQUIRE(mabi::Itanium::Mangle(^^bool)               == std::string_view{"b"});
    STATIC_REQUIRE(mabi::Itanium::Mangle(^^char)               == std::string_view{"c"});
    STATIC_REQUIRE(mabi::Itanium::Mangle(^^signed char)        == std::string_view{"a"});
    STATIC_REQUIRE(mabi::Itanium::Mangle(^^unsigned char)      == std::string_view{"h"});
    STATIC_REQUIRE(mabi::Itanium::Mangle(^^int)                == std::string_view{"i"});
    STATIC_REQUIRE(mabi::Itanium::Mangle(^^unsigned int)       == std::string_view{"j"});
    STATIC_REQUIRE(mabi::Itanium::Mangle(^^long)               == std::string_view{"l"});
    STATIC_REQUIRE(mabi::Itanium::Mangle(^^unsigned long long) == std::string_view{"y"});
    STATIC_REQUIRE(mabi::Itanium::Mangle(^^__int128)           == std::string_view{"n"});
    STATIC_REQUIRE(mabi::Itanium::Mangle(^^unsigned __int128)  == std::string_view{"o"});
    STATIC_REQUIRE(mabi::Itanium::Mangle(^^double)             == std::string_view{"d"});
}

TEST_CASE("Itanium cv-qualified pointer: const char* -> PKc", AUTO_TAG) {
    STATIC_REQUIRE(mabi::Itanium::Mangle(^^const char*) == std::string_view{"PKc"});
    STATIC_REQUIRE(mabi::Itanium::Mangle(^^int*)        == std::string_view{"Pi"});
    STATIC_REQUIRE(mabi::Itanium::Mangle(^^int&)        == std::string_view{"Ri"});
    STATIC_REQUIRE(mabi::Itanium::Mangle(^^int&&)       == std::string_view{"Oi"});
}

TEST_CASE("Itanium named types: global 1A, nested N...E", AUTO_TAG) {
    STATIC_REQUIRE(mabi::Itanium::Mangle(^^abitest::ns::Inner)
                   == std::string_view{"N7abitest2ns5InnerE"});
}

// =============================================================================
// §6  Itanium — symbol direction (golden strings)
// =============================================================================

TEST_CASE("Itanium variables: _Z prefix + name", AUTO_TAG) {
    STATIC_REQUIRE(mabi::Itanium::Mangle(^^g_int) == std::string_view{"_Z5g_int"});
}

TEST_CASE("Itanium functions: _Z name + param types, void list = v", AUTO_TAG) {
    STATIC_REQUIRE(mabi::Itanium::Mangle(^^abitest::free_two)
                   == std::string_view{"_ZN7abitest8free_twoEid"});
    STATIC_REQUIRE(mabi::Itanium::Mangle(^^abitest::free_none)
                   == std::string_view{"_ZN7abitest9free_noneEv"});
}

// =============================================================================
// §7  Compression engines (the #1 correctness risk on each side)
// =============================================================================

TEST_CASE("Itanium substitution: repeated PKc collapses to a back-ref", AUTO_TAG) {
    // Name prefix `abitest` = S_, then `Kc` = S0_, `PKc` = S1_; the second
    // const char* reuses S1_. (Prefix components share the dictionary with
    // parameter types — verified against the spec's compression rules.)
    STATIC_REQUIRE(mabi::Itanium::Mangle(^^abitest::dup_ptr)
                   == std::string_view{"_ZN7abitest7dup_ptrEPKcS1_"});
}

TEST_CASE("Itanium substitution: named type and its pointer get distinct slots", AUTO_TAG) {
    // `abitest` = S_; in Plain* the nested name reuses the S_ prefix
    // (NS_5PlainE), Plain registers as S0_, Plain* as S1_; the repeat is S1_.
    STATIC_REQUIRE(mabi::Itanium::Mangle(^^abitest::dup_named)
                   == std::string_view{"_ZN7abitest9dup_namedEPNS_5PlainES1_"});
}

// =============================================================================
// §8  Runtime round-trip (DbgHelp on Windows, libc++abi on POSIX)
// =============================================================================

TEST_CASE("Runtime demangles an MSVC function symbol", AUTO_TAG) {
    constexpr auto sym = mabi::MSVC::Mangle(^^abitest::free_two);
    auto human = mabi::Runtime::Demangle(sym);
    CHECK(human.find("free_two") != std::string::npos);
}

TEST_CASE("Runtime TryDemangle reports failure on garbage", AUTO_TAG) {
    auto r = mabi::Runtime::TryDemangle("not a mangled name");
    CHECK_FALSE(r.has_value());
}

TEST_CASE("Runtime Demangle returns input unchanged on failure", AUTO_TAG) {
    CHECK(mabi::Runtime::Demangle("zzz-not-mangled") == std::string{"zzz-not-mangled"});
}



# =============================================================================
# ReflectionFeatureProbes.cmake
# -----------------------------------------------------------------------------
# Compile-probes for the C++26 language and library features this project relies on.
# Each probe compiles a tiny translation unit with the project's real compile
# flags (the toolchain's -freflection-latest, -std=gnu++26, libc++ -isystem,
# sysroot, ...) and records the result in a cache variable:
#
#   HAVE_REFLECTION            P2996  Reflection for C++26       (MANDATORY)
#   HAVE_DEFINE_STATIC_ARRAY   P3491  std::define_static_array   (MANDATORY)
#   HAVE_ANNOTATIONS           P3394  Annotations for Reflection (MANDATORY)
#   HAVE_CONSTEVAL_BLOCKS       P3289  consteval { } blocks       (MANDATORY)
#   HAVE_EXPANSION_STATEMENTS   P1306  template for               (MANDATORY)
#   HAVE_EMBED                  P1967  #embed                     (MANDATORY)
#   HAVE_INT128                 ----  __int128 / __uint128_t      (MANDATORY)
#
# (*) P3394 was plenary-approved into the C++26 IS working draft (Sofia, 2025),
#     so it is a standard feature. Per project policy it is therefore required;
#     the probe gates that requirement against the actual compiler. If a future
#     compiler lacks it the configure step fails loudly rather than silently
#     degrading.
# =============================================================================

include_guard(GLOBAL)
include(CheckCXXSourceCompiles)

# The Check* modules drive try_compile, which inherits CMAKE_CXX_FLAGS (carrying
# -freflection-latest and the libc++/sysroot -isystem flags) and, under CMP0067
# (NEW since our cmake_minimum_required), CMAKE_CXX_STANDARD. We still pin the
# standard explicitly so the probes are independent of caller state.
set(CMAKE_REQUIRED_QUIET FALSE)

set(_hetis_saved_cxx_standard "${CMAKE_CXX_STANDARD}")
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

function(hetis_check_cxx_source_compiles name description source)
    unset("${name}" CACHE)
    check_cxx_source_compiles("${source}" "${name}")
    set("${name}" "${${name}}" CACHE BOOL "${description}" FORCE)
endfunction()

# --- P2996: core reflection (^^, std::meta::info, metafunctions) -------------
hetis_check_cxx_source_compiles(HAVE_REFLECTION "P2996 core reflection support" "
#include <meta>
#include <vector>
struct HetisProbe { int a; int b; };
consteval int hetisCountMembers() {
    return static_cast<int>(
        std::meta::nonstatic_data_members_of(
            ^^HetisProbe, std::meta::access_context::unchecked()).size());
}
static_assert(hetisCountMembers() == 2);
int main() { return 0; }
")

# --- P3491: std::define_static_array (promote consteval data to static) ------
hetis_check_cxx_source_compiles(HAVE_DEFINE_STATIC_ARRAY "P3491 std::define_static_array support" "
#include <meta>
#include <vector>
int main() {
    constexpr auto span = std::define_static_array(std::vector<int>{1, 2, 3});
    static_assert(span.size() == 3);
    static_assert(span[1] == 2);
    return 0;
}
")

# --- P3394: annotations ([[=value]], annotations_of) -------------------------
# Note: the standardized (and libc++/clang-p2996) spelling is the two-argument
# std::meta::annotations_of(entity, type); early drafts/blog posts used the name
# annotations_of_with_type, which this toolchain does not provide. The probe
# exercises both the [[=value]] grammar and the library metafunction.
hetis_check_cxx_source_compiles(HAVE_ANNOTATIONS "P3394 annotation syntax and annotations_of support" "
#include <meta>
struct HetisProbeTag {};
constexpr HetisProbeTag hetisProbeTag;
struct [[=hetisProbeTag]] HetisAnnotated {};
int main() {
    static_assert(
        std::meta::annotations_of(^^HetisAnnotated, ^^HetisProbeTag).size() == 1);
    return 0;
}
")

# --- P3385: attribute reflection (^^[[attr]], is_attribute) -------------------
hetis_check_cxx_source_compiles(HAVE_ATTRIBUTE_REFLECTION "P3385 attribute reflection support" "
#include <meta>
int main() {
    static_assert(std::meta::is_attribute(^^[[nodiscard]]));
    static_assert(!std::meta::is_attribute(^^int));
    return 0;
}
")

# --- P3385: attributes_of(entity) --------------------------------------------
hetis_check_cxx_source_compiles(HAVE_ATTRIBUTES_OF "P3385 attributes_of support" "
#include <meta>
enum class [[nodiscard]] HetisProbeError { ok };
int main() {
    constexpr auto attrs = std::meta::attributes_of(^^HetisProbeError);
    static_assert(attrs.size() == 1);
    static_assert(std::meta::is_attribute(attrs[0]));
    return 0;
}
")

# --- P3385: has_attribute(entity, attribute) ---------------------------------
hetis_check_cxx_source_compiles(HAVE_HAS_ATTRIBUTE "P3385 has_attribute support" "
#include <meta>
struct [[deprecated]] HetisDeprecated {};
int main() {
    static_assert(std::meta::has_attribute(^^HetisDeprecated, ^^[[deprecated]]));
    return 0;
}
")

# --- P3385: attribute_comparison::ignore_argument ----------------------------
hetis_check_cxx_source_compiles(
    HAVE_ATTRIBUTE_COMPARISON_IGNORE_ARGUMENT
    "P3385 attribute_comparison::ignore_argument support"
"
#include <meta>
[[deprecated(\"hetis\")]] void hetisDeprecated();
int main() {
    static_assert(std::meta::has_attribute(
        ^^hetisDeprecated,
        ^^[[deprecated]],
        std::meta::attribute_comparison::ignore_argument));
    return 0;
}
")

# --- P3385: identifier_of(attribute) -----------------------------------------
hetis_check_cxx_source_compiles(HAVE_ATTRIBUTE_IDENTIFIER "P3385 identifier_of(attribute) support" "
#include <meta>
#include <string_view>
using namespace std::literals;
int main() {
    static_assert(std::meta::identifier_of(^^[[nodiscard]]) == \"nodiscard\"sv);
    return 0;
}
")

# --- P3385: data_member_spec(... .attributes = {...}) ------------------------
hetis_check_cxx_source_compiles(
    HAVE_DATA_MEMBER_SPEC_ATTRIBUTES
    "P3385 data_member_spec attribute-list support"
"
#include <meta>
struct HetisEmpty {};
struct HetisGenerated;
consteval {
    std::meta::define_aggregate(^^HetisGenerated, {
        std::meta::data_member_spec(^^int, {.name = \"x\"}),
        std::meta::data_member_spec(
            ^^HetisEmpty,
            {.name = \"tag\", .attributes = {^^[[maybe_unused]]}})
    });
}
int main() {
    HetisGenerated obj{};
    (void)obj;
    return 0;
}
")

# --- P3289: consteval blocks (namespace/class scope immediate evaluation) -----
hetis_check_cxx_source_compiles(HAVE_CONSTEVAL_BLOCKS "P3289 consteval block support" "
#include <meta>
struct HetisConstevalBlockProbe;
consteval {
    std::meta::define_aggregate(^^HetisConstevalBlockProbe, {
        std::meta::data_member_spec(^^int,   {.name = \"x\"}),
        std::meta::data_member_spec(^^float, {.name = \"y\"}),
    });
}
static_assert(sizeof(HetisConstevalBlockProbe) == 8);
int main() { return 0; }
")

# --- P1306: expansion statements (template for) ------------------------------
hetis_check_cxx_source_compiles(HAVE_EXPANSION_STATEMENTS "P1306 expansion statement support" "
#include <meta>
#include <ranges>
struct HetisExpansionProbe { int a; float b; double c; };
consteval int hetisCountViaExpansion() {
    int n = 0;
    template for (constexpr auto m : std::define_static_array(
            std::meta::nonstatic_data_members_of(
                ^^HetisExpansionProbe,
                std::meta::access_context::unchecked()))) {
        ++n;
    }
    return n;
}
static_assert(hetisCountViaExpansion() == 3);
int main() { return 0; }
")

# --- P1967: #embed -----------------------------------------------------------
hetis_check_cxx_source_compiles(HAVE_EMBED "P1967 #embed support" [=[
#ifndef __has_embed
#error "__has_embed is unavailable"
#endif

#if !__has_embed(__FILE__)
#error "compiler reports that the current source file cannot be embedded"
#endif

constexpr unsigned char hetisEmbeddedSource[] = {
#embed __FILE__
};

static_assert(sizeof(hetisEmbeddedSource) > 0);

int main() {
    return static_cast<int>(hetisEmbeddedSource[0]);
}
]=])

# --- 128-bit integers (__int128 / __uint128_t compiler extension) ------------
# Not an ISO type, but the project's 128-bit hash tier (Hashing::Uuid) and the
# Mashiro::uint128_t / int128_t aliases depend on it. The probe verifies the
# extension exists, has the expected 16-byte width, and supports the arithmetic
# the codebase relies on. Treated as mandatory: configure fails loudly on a
# toolchain that lacks it rather than silently dropping the 128-bit facilities.
hetis_check_cxx_source_compiles(HAVE_INT128 "__int128 / __uint128_t compiler extension support" "
int main() {
    __uint128_t u = 0;
    __int128 s = 0;
    static_assert(sizeof(__uint128_t) == 16, \"expected 16-byte __uint128_t\");
    static_assert(sizeof(__int128) == 16, \"expected 16-byte __int128\");
    u = (static_cast<__uint128_t>(0x0123456789abcdefULL) << 64) | 0xfedcba9876543210ULL;
    u *= 3u;
    u ^= 1u;
    s = -s;
    return static_cast<int>((u >> 120) + s);
}
")

# --- P3394: annotation structural value --------------------------------------
hetis_check_cxx_source_compiles(HAVE_STRUCTURAL_ANNOTATION_VALUE "P3394 structural annotation value support" "
#include <meta>
struct HetisConfig { int value; };
constexpr HetisConfig hetisConfig{2};
struct [[=hetisConfig]] HetisAnnotated {};
int main() {
    static_assert(
        std::meta::annotations_of(^^HetisAnnotated, ^^HetisConfig).size() == 1);
    return 0;
}
")

set(CMAKE_CXX_STANDARD "${_hetis_saved_cxx_standard}")
unset(_hetis_saved_cxx_standard)

# --- Enforcement -------------------------------------------------------------
if(NOT HAVE_REFLECTION)
    message(FATAL_ERROR
        "[Hetis] C++26 reflection (P2996) is required but the compiler probe failed.\n"
        "        Verify the toolchain enables -freflection-latest (COCA_ENABLE_REFLECTION=ON).")
endif()

if(NOT HAVE_DEFINE_STATIC_ARRAY)
    message(FATAL_ERROR
        "[Hetis] std::define_static_array (P3491) is required but the compiler probe failed.")
endif()

if(NOT HAVE_ANNOTATIONS OR NOT HAVE_STRUCTURAL_ANNOTATION_VALUE)
    message(FATAL_ERROR
        "[Hetis] C++26 annotations (P3394) are required but the compiler probe failed.\n"
        "        P3394 is part of the C++26 IS; upgrade or repair the toolchain\n"
        "        (-freflection-latest must implement annotations).")
endif()

if(NOT HAVE_CONSTEVAL_BLOCKS)
    message(FATAL_ERROR
        "[Hetis] consteval blocks (P3289) are required but the compiler probe failed.\n"
        "        P3289 is the preferred mechanism for compile-time code generation.\n"
        "        Ensure -freflection-latest is enabled (COCA clang-p2996 implements P3289).")
endif()

if(NOT HAVE_EXPANSION_STATEMENTS)
    message(FATAL_ERROR
        "[Hetis] expansion statements / template for (P1306) are required but the compiler probe failed.\n"
        "        P1306 is essential for compile-time iteration over reflected members.\n"
        "        Ensure -freflection-latest is enabled.")
endif()

if(NOT HAVE_EMBED)
    message(FATAL_ERROR
        "[Hetis] #embed (P1967) is required but the compiler probe failed.\n"
        "        P1967 is required for compile-time binary resource embedding.\n"
        "        Use a C++26 toolchain that implements __has_embed and #embed.")
endif()

if(NOT HAVE_INT128)
    message(FATAL_ERROR
        "[Hetis] 128-bit integers (__int128 / __uint128_t) are required but the compiler probe failed.\n"
        "        The project's 128-bit hash tier (Hashing::Uuid) and the\n"
        "        Mashiro::uint128_t / int128_t aliases depend on this compiler extension.\n"
        "        Use a toolchain that provides 16-byte __int128 / __uint128_t.")
endif()

message(STATUS "[Hetis] C++26 required features: P2996, P3491, P3394, P3289, P1306, P1967 -> all present.")
message(STATUS "[Hetis] 128-bit integer extension (__int128 / __uint128_t) -> present (required).")

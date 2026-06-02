# =============================================================================
# ReflectionFeatureProbes.cmake
# -----------------------------------------------------------------------------
# Compile-probes for the C++26 reflection features this project relies on.
# Each probe compiles a tiny translation unit with the project's real compile
# flags (the toolchain's -freflection-latest, -std=gnu++26, libc++ -isystem,
# sysroot, ...) and records the result in a cache variable:
#
#   HAVE_REFLECTION            P2996  Reflection for C++26       (MANDATORY)
#   HAVE_DEFINE_STATIC_ARRAY   P3491  std::define_static_array   (MANDATORY)
#   HAVE_ANNOTATIONS           P3394  Annotations for Reflection (MANDATORY*)
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

# --- P2996: core reflection (^^, std::meta::info, metafunctions) -------------
check_cxx_source_compiles("
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
" HAVE_REFLECTION)

# --- P3491: std::define_static_array (promote consteval data to static) ------
check_cxx_source_compiles("
#include <meta>
#include <vector>
int main() {
    constexpr auto span = std::define_static_array(std::vector<int>{1, 2, 3});
    static_assert(span.size() == 3);
    static_assert(span[1] == 2);
    return 0;
}
" HAVE_DEFINE_STATIC_ARRAY)

# --- P3394: annotations ([[=value]], annotations_of) -------------------------
# Note: the standardized (and libc++/clang-p2996) spelling is the two-argument
# std::meta::annotations_of(entity, type); early drafts/blog posts used the name
# annotations_of_with_type, which this toolchain does not provide. The probe
# exercises both the [[=value]] grammar and the library metafunction.
check_cxx_source_compiles("
#include <meta>
struct HetisProbeTag {};
constexpr HetisProbeTag hetisProbeTag;
struct [[=hetisProbeTag]] HetisAnnotated {};
int main() {
    static_assert(
        std::meta::annotations_of(^^HetisAnnotated, ^^HetisProbeTag).size() == 1);
    return 0;
}
" HAVE_ANNOTATIONS)

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

if(NOT HAVE_ANNOTATIONS)
    message(FATAL_ERROR
        "[Hetis] C++26 annotations (P3394) are required but the compiler probe failed.\n"
        "        P3394 is part of the C++26 IS; upgrade or repair the toolchain\n"
        "        (-freflection-latest must implement annotations).")
endif()

message(STATUS "[Hetis] C++26 reflection features: P2996 reflection, "
               "P3491 define_static_array, P3394 annotations -> all present (required).")

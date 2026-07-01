cmake_minimum_required(VERSION 3.25)

# Pin defaults BEFORE project() so they reach every add_subdirectory we pull in.
if(NOT DEFINED CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build all dependencies as static libs" FORCE)

project(vulkan_thirdparty_probe LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Headers come from the SDK via find_package; the loader is volk (do NOT link
# Vulkan::Vulkan or vulkan-1.lib — it duplicates symbols volk owns).
find_package(Vulkan REQUIRED)

# Pull the shared thirdparty collection from the sibling directory.
# CMAKE_CURRENT_SOURCE_DIR (not CMAKE_SOURCE_DIR) keeps this correct whether
# _probe is the top-level project or a subdirectory of the repo-root umbrella;
# the guard avoids a duplicate add when a parent already pulled thirdparty in.
if(NOT TARGET vma)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty thirdparty)
endif()

add_executable(probe main.cpp stdexec_probe.cpp)

# stdexec on MSVC needs the conformant preprocessor (/Zc:preprocessor) AND an
# accurate __cplusplus (/Zc:__cplusplus) — its C++20 guard reads the latter.
# Scope both flags to this TU so the rest of main.cpp's includes are unaffected.
if(MSVC)
    set_source_files_properties(stdexec_probe.cpp PROPERTIES
        COMPILE_OPTIONS "/Zc:preprocessor;/Zc:__cplusplus")
endif()

# Link a representative subset that exercises every category.
target_link_libraries(probe PRIVATE
    Vulkan::Headers
    vma
    volk
    vk-bootstrap
    spirv-reflect
    spirv-cross-core
    glfw
    imgui
    glm
    stb
    tinyobjloader
    tinygltf
    cgltf
    fastgltf
    meshoptimizer
    mikktspace
    tinyexr
    TracyClient
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    entt
    tinyfiledialogs
    renderdoc_app
    cli11
    taskflow
    miniaudio
    imguizmo
    implot
)
# Header-only/static extras always present in this build.
target_compile_definitions(probe PRIVATE
    VK_TP_PROBE_CLI11 VK_TP_PROBE_TASKFLOW VK_TP_PROBE_MINIAUDIO
    VK_TP_PROBE_IMGUIZMO VK_TP_PROBE_IMPLOT)

# stdexec (P2300) — header-only but MSVC support is experimental upstream, so it
# gets its own switch: flip VK_TP_PROBE_STDEXEC off if a compiler update regresses
# without losing coverage of the other libraries.
if(VK_TP_BUILD_PERFETTO)
    target_link_libraries(probe PRIVATE perfetto)
    target_compile_definitions(probe PRIVATE VK_TP_PROBE_PERFETTO)
endif()

target_link_libraries(probe PRIVATE stdexec)
target_compile_definitions(probe PRIVATE VK_TP_PROBE_STDEXEC)
if(VK_TP_BUILD_KTX)
    target_link_libraries(probe PRIVATE ktx)
    target_compile_definitions(probe PRIVATE VK_TP_PROBE_KTX)
endif()
if(VK_TP_BUILD_ASSIMP)
    target_link_libraries(probe PRIVATE assimp)
    target_compile_definitions(probe PRIVATE VK_TP_PROBE_ASSIMP)
endif()
if(VK_TP_BUILD_SLANG)
    target_link_libraries(probe PRIVATE slang)
    target_compile_definitions(probe PRIVATE VK_TP_PROBE_SLANG)
    vk_tp_deploy_slang(probe)   # copy slang-compiler/glslang/glsl-module DLLs next to probe.exe
endif()
if(VK_TP_BUILD_CATCH2)
    target_link_libraries(probe PRIVATE Catch2::Catch2)
    target_compile_definitions(probe PRIVATE VK_TP_PROBE_CATCH2)
endif()
if(VK_TP_BUILD_EFSW)
    target_link_libraries(probe PRIVATE efsw)
    target_compile_definitions(probe PRIVATE VK_TP_PROBE_EFSW)
endif()
if(VK_TP_BUILD_MSDFGEN)
    target_link_libraries(probe PRIVATE msdfgen::msdfgen-core)
    target_compile_definitions(probe PRIVATE VK_TP_PROBE_MSDFGEN)
endif()
if(VK_TP_BUILD_JOLT)
    target_link_libraries(probe PRIVATE Jolt)
    target_compile_definitions(probe PRIVATE VK_TP_PROBE_JOLT)
endif()
if(VK_TP_BUILD_TBB)
    target_link_libraries(probe PRIVATE TBB::tbb)
    target_compile_definitions(probe PRIVATE VK_TP_PROBE_TBB)
endif()
if(VK_TP_BUILD_METIS)
    target_link_libraries(probe PRIVATE metis)
    target_compile_definitions(probe PRIVATE VK_TP_PROBE_METIS)
endif()

# KTX ships an older bundled vulkan_core.h under external/dfdutils/vulkan/.
# Its FILE_SET BASE_DIRS for KHR/khr_df.h leaks `external/dfdutils` into ktx's
# INTERFACE_INCLUDE_DIRECTORIES, which then shadows the SDK's vulkan_core.h
# (missing 1.4 features).
#
# Strategy: copy only KHR/khr_df.h into a clean shim directory, then strip the
# offending dfdutils path from ktx's INTERFACE_INCLUDE_DIRECTORIES and add the
# shim directory in its place. Public ktx headers (ktx.h / ktxvulkan.h) live in
# ktx/lib/include and are untouched.
if(VK_TP_BUILD_KTX)
    set(_ktx_dfd_src "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/ktx/external/dfdutils")
    set(_ktx_shim    "${CMAKE_BINARY_DIR}/ktx_khr_shim")
    configure_file(
        "${_ktx_dfd_src}/KHR/khr_df.h"
        "${_ktx_shim}/KHR/khr_df.h"
        COPYONLY)
    foreach(_t ktx ktx_read)
        if(TARGET ${_t})
            get_target_property(_inc ${_t} INTERFACE_INCLUDE_DIRECTORIES)
            if(_inc)
                list(FILTER _inc EXCLUDE REGEX "external/dfdutils/?\\$?>?$")
                list(APPEND _inc "$<BUILD_INTERFACE:${_ktx_shim}>")
                set_target_properties(${_t} PROPERTIES
                    INTERFACE_INCLUDE_DIRECTORIES "${_inc}")
            endif()
        endif()
    endforeach()
endif()

if(MSVC)
    target_compile_options(probe PRIVATE /utf-8 /MP)
endif()

# Copy every shared dependency's DLL (assimp-*/glfw3/ktx/meshoptimizer/spdlog/
# tbb12) next to probe.exe so it runs straight from the build tree. Slang keeps
# its own payload helper (vk_tp_deploy_slang) above.
vk_tp_deploy_runtime(probe)

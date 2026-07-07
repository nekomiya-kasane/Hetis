# =============================================================================
#  Vulkan thirdparty aggregator
#  -----------------------------------------------------------------------------
#  Each library is exposed as a CMake target. Heavy libraries are guarded by
#  options so a parent project only pays for what it links.
#
#  Usage from parent CMakeLists:
#       add_subdirectory(thirdparty)
#       target_link_libraries(my_app PRIVATE
#           Vulkan::Vulkan vma volk vk-bootstrap glfw glm stb imgui
#           spirv-reflect spirv-cross-core spdlog::spdlog ...)
# =============================================================================
cmake_minimum_required(VERSION 3.25)

if(NOT TARGET vulkan_thirdparty_marker)
    add_custom_target(vulkan_thirdparty_marker)
endif()

# -----------------------------------------------------------------------------
# 0. Vulkan SDK (loader, headers, glslangValidator/glslc, validation layers)
# -----------------------------------------------------------------------------
find_package(Vulkan REQUIRED COMPONENTS glslc)
message(STATUS "[vulkan/thirdparty] Vulkan ${Vulkan_VERSION} @ ${Vulkan_INCLUDE_DIR}")
if(Vulkan_glslc_FOUND)
    message(STATUS "[vulkan/thirdparty] glslc:           ${Vulkan_GLSLC_EXECUTABLE}")
endif()

# -----------------------------------------------------------------------------
# 0b. Vulkan-Headers (KhronosGroup/Vulkan-Headers, vulkan-sdk-1.4.350.0)
#
#     `find_package(Vulkan)` exposes `Vulkan::Headers` backed by whatever the
#     system SDK ships. We override its INTERFACE_INCLUDE_DIRECTORIES so the
#     codebase tracks the latest headers (new extensions, vulkan.hpp /
#     vulkan.cppm features) independently of whichever SDK the developer has
#     installed locally. Every existing consumer (vma, spirv-reflect, imgui)
#     keeps linking against `Vulkan::Headers` unchanged — they just see the
#     newer headers.
# -----------------------------------------------------------------------------
set(VK_TP_VENDORED_HEADERS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vulkan-headers/include)

if(TARGET Vulkan::Headers)
    # `find_package(Vulkan)` (CMake ≥ 3.21) creates `Vulkan::Headers` as an
    # IMPORTED INTERFACE target — directly editable via set_property. Older
    # toolchains expose it as an ALIAS to an unnamed imported target; the
    # ALIASED_TARGET branch covers that case.
    get_target_property(_vk_headers_aliased Vulkan::Headers ALIASED_TARGET)
    if(_vk_headers_aliased)
        set(_vk_headers_target ${_vk_headers_aliased})
    else()
        set(_vk_headers_target Vulkan::Headers)
    endif()
    set_property(TARGET ${_vk_headers_target}
        PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${VK_TP_VENDORED_HEADERS_DIR}")
    message(STATUS "[vulkan/thirdparty] Vulkan-Headers   (vendored, overrides SDK): "
            "${VK_TP_VENDORED_HEADERS_DIR}")
else()
    # No SDK Vulkan::Headers — bring the upstream CMakeLists in directly.
    add_subdirectory(vulkan-headers)
    message(STATUS "[vulkan/thirdparty] Vulkan-Headers   (vendored, sole source): "
            "${VK_TP_VENDORED_HEADERS_DIR}")
endif()

# -----------------------------------------------------------------------------
# Build option toggles for heavy libraries (default ON, switch off if unused)
# -----------------------------------------------------------------------------
option(VK_TP_BUILD_KTX     "Build KTX-Software (~30s)"   ON)
option(VK_TP_BUILD_ASSIMP  "Build Assimp model loader"   ON)
option(VK_TP_BUILD_TRACY   "Enable Tracy profiler client (sources only when off)" OFF)
option(VK_TP_BUILD_SLANG   "Use Slang as the shading language (prebuilt SDK, runtime compilation)" ON)
option(VK_TP_BUILD_TBB     "Build oneTBB (shared tbb12.dll, deploy via vk_tp_deploy_tbb())" ON)
option(VK_TP_BUILD_JOLT    "Build Jolt Physics"          ON)
option(VK_TP_BUILD_MSDFGEN "Build msdfgen (core-only, zero external deps)" ON)
option(VK_TP_BUILD_METIS   "Build METIS + GKlib graph partitioner (custom static build)" ON)
option(VK_TP_BUILD_EFSW    "Build efsw filesystem watcher" ON)
option(VK_TP_BUILD_CATCH2  "Build Catch2 test framework"   ON)
option(VK_TP_BUILD_PERFETTO "Build Perfetto tracing SDK (amalgamated static library)" ON)
option(VK_TP_ENABLE_PHYSX  "Expose NVIDIA PhysX SDK headers (native library build is isolated for now)" ON)

# -----------------------------------------------------------------------------
# 1. VulkanMemoryAllocator (header-only)
#     We deliberately link Vulkan::Headers (not Vulkan::Vulkan) — volk is the
#     loader of record, so pulling in vulkan-1.lib would create duplicate
#     symbols for vkGetInstanceProcAddr & friends.
# -----------------------------------------------------------------------------
add_library(vma INTERFACE)
target_include_directories(vma INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/vma/include)
target_link_libraries(vma INTERFACE Vulkan::Headers)

# -----------------------------------------------------------------------------
# 2. volk — meta-loader (single .c + .h)
# -----------------------------------------------------------------------------
set(VOLK_STATIC_DEFINES "" CACHE INTERNAL "")
set(VOLK_PULL_IN_VULKAN ON CACHE BOOL "" FORCE)
add_subdirectory(volk)
# volk provides target `volk` and `volk_headers`

# -----------------------------------------------------------------------------
# 3. vk-bootstrap
# -----------------------------------------------------------------------------
set(VK_BOOTSTRAP_TEST OFF CACHE BOOL "" FORCE)
set(VK_BOOTSTRAP_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(vk-bootstrap)

# -----------------------------------------------------------------------------
# 4. SPIRV-Cross — reflection + reverse compilation
# -----------------------------------------------------------------------------
set(SPIRV_CROSS_CLI         OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_STATIC      ON  CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SHARED      OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(spirv-cross)
# targets: spirv-cross-core, spirv-cross-glsl, spirv-cross-hlsl,
#          spirv-cross-msl, spirv-cross-cpp, spirv-cross-reflect, spirv-cross-util

# -----------------------------------------------------------------------------
# 5. SPIRV-Reflect — lightweight runtime reflection (single .c+.h)
# -----------------------------------------------------------------------------
add_library(spirv-reflect STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/spirv-reflect/spirv_reflect.c
    ${CMAKE_CURRENT_SOURCE_DIR}/spirv-reflect/spirv_reflect.h
)
target_include_directories(spirv-reflect PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/spirv-reflect)
# Headers only — see vma comment above for why we avoid Vulkan::Vulkan.
target_link_libraries(spirv-reflect PUBLIC Vulkan::Headers)

# -----------------------------------------------------------------------------
# 5b. Slang — primary (and only) shading language for this renderer.
#     Prebuilt SDK; all shaders are authored in Slang and compiled at runtime
#     through slang::IGlobalSession. A single session is configured with several
#     targets so the same module is emitted as SPIR-V (for execution on Vulkan)
#     and, for inspection/debugging, cross-compiled to GLSL / HLSL / Metal / WGSL.
#
#     We link slang-compiler.lib directly — NOT the legacy slang.lib proxy, which
#     Khronos removes at the end of 2026. slang-compiler.dll resolves
#     slang-glslang.dll and slang-glsl-module.dll lazily via LoadLibrary when a
#     GLSL / SPIR-V-via-GLSL target is requested, so they are absent from its
#     import table and $<TARGET_RUNTIME_DLLS> cannot see them; they must be
#     copied next to every consumer. Use vk_tp_deploy_slang(<target>).
# -----------------------------------------------------------------------------
if(VK_TP_BUILD_SLANG)
    set(SLANG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/slang)
    if(NOT EXISTS "${SLANG_ROOT}/lib/slang-compiler.lib")
        message(FATAL_ERROR "[vulkan/thirdparty] Slang SDK missing at ${SLANG_ROOT}. "
                            "Expected prebuilt slang-VERSION-windows-x86_64 layout "
                            "(bin/ lib/ include/).")
    endif()

    add_library(slang SHARED IMPORTED GLOBAL)
    set_target_properties(slang PROPERTIES
        IMPORTED_IMPLIB               "${SLANG_ROOT}/lib/slang-compiler.lib"
        IMPORTED_LOCATION             "${SLANG_ROOT}/bin/slang-compiler.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${SLANG_ROOT}/include"
    )

    # Runtime payload deployed beside any executable that compiles Slang.
    set(SLANG_RUNTIME_DLLS
        "${SLANG_ROOT}/bin/slang-compiler.dll"
        "${SLANG_ROOT}/bin/slang-glslang.dll"
        "${SLANG_ROOT}/bin/slang-glsl-module.dll"
        CACHE INTERNAL "Slang runtime DLLs to copy next to consumer executables")
    set(SLANG_SLANGC_EXECUTABLE "${SLANG_ROOT}/bin/slangc.exe"
        CACHE INTERNAL "Slang offline/CLI compiler")

    message(STATUS "[vulkan/thirdparty] Slang SDK:        ${SLANG_ROOT}")
endif()

# vk_tp_deploy_slang(<target>): copy the Slang runtime DLLs into the directory of
# <target> after it is built. No-op (with a warning) when Slang is disabled.
function(vk_tp_deploy_slang tgt)
    if(NOT TARGET slang)
        message(WARNING "vk_tp_deploy_slang(${tgt}) called but VK_TP_BUILD_SLANG is OFF.")
        return()
    endif()
    add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${SLANG_RUNTIME_DLLS} "$<TARGET_FILE_DIR:${tgt}>"
        COMMENT "Deploying Slang runtime -> $<TARGET_FILE_DIR:${tgt}>"
        VERBATIM)
endfunction()

# -----------------------------------------------------------------------------
# 6. KTX-Software (KTX/KTX2 textures, Basis Universal transcoding)
# -----------------------------------------------------------------------------
if(VK_TP_BUILD_KTX)
    set(KTX_FEATURE_TESTS     OFF CACHE BOOL "" FORCE)
    set(KTX_FEATURE_TOOLS     OFF CACHE BOOL "" FORCE)
    set(KTX_FEATURE_DOC       OFF CACHE BOOL "" FORCE)
    set(KTX_FEATURE_LOADTEST_APPS OFF CACHE BOOL "" FORCE)
    set(KTX_FEATURE_GL_UPLOAD  OFF CACHE BOOL "" FORCE)
    set(KTX_FEATURE_VK_UPLOAD  ON  CACHE BOOL "" FORCE)
    set(KTX_FEATURE_STATIC_LIBRARY OFF CACHE BOOL "" FORCE)  # build ktx.dll — deploy via vk_tp_deploy_runtime()
    # The vendored tree carries no git tags, so KTX's git_describe emits
    # `fatal: No names found` and falls back to 0.0.0-noversion. Pin the version
    # (see RELEASE_NOTES.md) so version.cmake skips the git probe entirely.
    set(KTX_GIT_VERSION_FULL "v5.0.0-rc1")
    add_subdirectory(ktx)
    # exposes target `ktx`
endif()

# -----------------------------------------------------------------------------
# 7. GLFW (with Vulkan support compiled in)
# -----------------------------------------------------------------------------
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
set(GLFW_VULKAN_STATIC  OFF CACHE BOOL "" FORCE)
set(GLFW_LIBRARY_TYPE   SHARED CACHE STRING "" FORCE)  # glfw3.dll — deploy via vk_tp_deploy_runtime()
add_subdirectory(glfw)

# -----------------------------------------------------------------------------
# 7b. glad — OpenGL loader (GLAD 2.0.8, gl:core=4.6 with all extensions)
# -----------------------------------------------------------------------------
add_subdirectory(glad)

# -----------------------------------------------------------------------------
# 8. ImGui (docking branch) — Vulkan + GLFW backends bundled
# -----------------------------------------------------------------------------
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
# Headers only — backends call Vulkan via volk-loaded pointers in user code.
target_link_libraries(imgui PUBLIC glfw Vulkan::Headers)

# -----------------------------------------------------------------------------
# 9. glm (header-only)
# -----------------------------------------------------------------------------
add_library(glm INTERFACE)
target_include_directories(glm INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/glm)
target_compile_definitions(glm INTERFACE
    GLM_FORCE_DEPTH_ZERO_TO_ONE      # Vulkan-style clip space
    GLM_FORCE_RADIANS
    GLM_ENABLE_EXPERIMENTAL
)

# -----------------------------------------------------------------------------
# 10. stb (header-only multi-library)
# -----------------------------------------------------------------------------
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/stb)

# -----------------------------------------------------------------------------
# 11. Assimp (50+ model formats)
# -----------------------------------------------------------------------------
if(VK_TP_BUILD_ASSIMP)
    set(BUILD_SHARED_LIBS         OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_TESTS        OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_SAMPLES      OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_DOCS         OFF CACHE BOOL "" FORCE)
    set(ASSIMP_INSTALL            OFF CACHE BOOL "" FORCE)
    set(ASSIMP_INSTALL_PDB        OFF CACHE BOOL "" FORCE)
    set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT ON CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
    set(ASSIMP_INJECT_DEBUG_POSTFIX OFF CACHE BOOL "" FORCE)
    # Build assimp shared (it ships a complete dllexport surface) -> assimp-*.dll.
    # The cached BUILD_SHARED_LIBS=OFF above remains the tree-wide static default;
    # shadow it with a normal var for assimp only, then drop it.
    set(BUILD_SHARED_LIBS ON)
    add_subdirectory(assimp)
    unset(BUILD_SHARED_LIBS)
endif()

# -----------------------------------------------------------------------------
# 12. tinyobjloader (static library)
#     Compiled in C++20 mode to dodge a constexpr/fast_float regression that
#     surfaces in MSVC 19.41 + /std:c++23. Consumers can still build with
#     C++23 — only this TU is pinned.
# -----------------------------------------------------------------------------
add_library(tinyobjloader STATIC ${CMAKE_CURRENT_SOURCE_DIR}/tinyobjloader/tiny_obj_loader.cc)
target_include_directories(tinyobjloader PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tinyobjloader)
set_target_properties(tinyobjloader PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# -----------------------------------------------------------------------------
# 13. tinygltf (header-only, depends on stb + json)
# -----------------------------------------------------------------------------
add_library(tinygltf INTERFACE)
target_include_directories(tinygltf INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/tinygltf)

# -----------------------------------------------------------------------------
# 14. cgltf (header-only C)
# -----------------------------------------------------------------------------
add_library(cgltf INTERFACE)
target_include_directories(cgltf INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/cgltf)

# -----------------------------------------------------------------------------
# 15. fastgltf (modern C++ glTF parser)
# -----------------------------------------------------------------------------
set(FASTGLTF_COMPILE_AS_CPP20 OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_TESTS     OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_DOCS      OFF CACHE BOOL "" FORCE)
add_subdirectory(fastgltf)

# -----------------------------------------------------------------------------
# 16. meshoptimizer
# -----------------------------------------------------------------------------
set(MESHOPT_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)  # meshoptimizer.dll — deploy via vk_tp_deploy_runtime()
add_subdirectory(meshoptimizer)

# -----------------------------------------------------------------------------
# 17. MikkTSpace (single .c+.h)
# -----------------------------------------------------------------------------
add_library(mikktspace STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/mikktspace/mikktspace.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mikktspace/mikktspace.h
)
target_include_directories(mikktspace PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/mikktspace)

# -----------------------------------------------------------------------------
# 18. TinyEXR (HDR loader) + bundled miniz
#     The miniz.c TU must be compiled and linked when TINYEXR_IMPLEMENTATION
#     is activated in the consumer; we ship it as a small static library.
# -----------------------------------------------------------------------------
add_library(tinyexr STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/tinyexr/deps/miniz/miniz.c
)
target_include_directories(tinyexr PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/tinyexr
    ${CMAKE_CURRENT_SOURCE_DIR}/tinyexr/deps/miniz)

# -----------------------------------------------------------------------------
# 19. Tracy profiler — client only, opt-in
# -----------------------------------------------------------------------------
if(VK_TP_BUILD_TRACY)
    set(TRACY_ENABLE   ON  CACHE BOOL "" FORCE)
    set(TRACY_ON_DEMAND OFF CACHE BOOL "" FORCE)
    add_subdirectory(tracy)
else()
    # Always create an INTERFACE shim so user code can include <tracy/Tracy.hpp>
    # even when profiling is disabled (the macros become no-ops).
    add_library(TracyClient INTERFACE)
    target_include_directories(TracyClient INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/tracy/public)
endif()

# -----------------------------------------------------------------------------
# 20. spdlog (uses bundled fmt)
# -----------------------------------------------------------------------------
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL       OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_SHARED  ON CACHE BOOL "" FORCE)  # spdlog.dll — deploy via vk_tp_deploy_runtime()
add_subdirectory(spdlog)

# -----------------------------------------------------------------------------
# 21. nlohmann_json (header-only)
# -----------------------------------------------------------------------------
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install    OFF CACHE BOOL "" FORCE)
add_subdirectory(nlohmann_json)

# -----------------------------------------------------------------------------
# 22. EnTT (header-only ECS)
# -----------------------------------------------------------------------------
add_library(entt INTERFACE)
target_include_directories(entt SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/entt/src)
target_compile_features(entt INTERFACE cxx_std_17)
add_library(EnTT::EnTT ALIAS entt)

# -----------------------------------------------------------------------------
# 23. tinyfiledialogs (single .c + .h)
# -----------------------------------------------------------------------------
add_library(tinyfiledialogs STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/tinyfiledialogs/tinyfiledialogs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/tinyfiledialogs/tinyfiledialogs.h
)
target_include_directories(tinyfiledialogs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tinyfiledialogs)
if(WIN32)
    target_link_libraries(tinyfiledialogs PRIVATE comdlg32 ole32)
endif()

# -----------------------------------------------------------------------------
# 24. RenderDoc API (single header)
# -----------------------------------------------------------------------------
add_library(renderdoc_app INTERFACE)
target_include_directories(renderdoc_app INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/renderdoc_app)

# -----------------------------------------------------------------------------
# 24b. Perfetto tracing SDK (amalgamated perfetto.h + perfetto.cc).
#      This is the official C++ SDK release layout, not the full GN-built
#      upstream tree. Target: perfetto.
# -----------------------------------------------------------------------------
if(VK_TP_BUILD_PERFETTO)
    set(PERFETTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/perfetto)
    if(NOT EXISTS "${PERFETTO_DIR}/perfetto.h" OR NOT EXISTS "${PERFETTO_DIR}/perfetto.cc")
        message(FATAL_ERROR "[vulkan/thirdparty] Perfetto SDK missing at ${PERFETTO_DIR}. "
                            "Expected perfetto-cpp-sdk-src release layout with perfetto.h/perfetto.cc.")
    endif()

    add_library(perfetto STATIC ${PERFETTO_DIR}/perfetto.cc)
    target_include_directories(perfetto PUBLIC ${PERFETTO_DIR})
    target_compile_definitions(perfetto PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_features(perfetto PUBLIC cxx_std_17)
    if(WIN32)
        target_link_libraries(perfetto PUBLIC ws2_32)
    endif()
    if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        target_compile_options(perfetto PRIVATE /bigobj)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
        target_compile_options(perfetto PRIVATE -Wa,-mbig-obj)
    endif()
endif()

# =============================================================================
#  Frontier-renderer additions (2026)
# =============================================================================

# -----------------------------------------------------------------------------
# 25. CLI11 — header-only command-line parser
# -----------------------------------------------------------------------------
add_library(cli11 INTERFACE)
target_include_directories(cli11 INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/cli11/include)

# -----------------------------------------------------------------------------
# 26. stdexec — reference impl of std::execution (P2300 senders/receivers).
#     Header-only; needs C++20. (MSVC support is best-effort upstream.)
# -----------------------------------------------------------------------------
add_library(stdexec INTERFACE)
target_include_directories(stdexec INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/stdexec/include)
target_compile_features(stdexec INTERFACE cxx_std_20)

# -----------------------------------------------------------------------------
# 27. Taskflow — header-only task-graph parallelism (#include <taskflow/taskflow.hpp>)
# -----------------------------------------------------------------------------
add_library(taskflow INTERFACE)
target_include_directories(taskflow INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/taskflow)
target_compile_features(taskflow INTERFACE cxx_std_17)
if(WIN32)
    target_link_libraries(taskflow INTERFACE) # pthread not needed on Win32
endif()

# -----------------------------------------------------------------------------
# 28. miniaudio — single-file audio (split build: compile miniaudio.c once)
# -----------------------------------------------------------------------------
add_library(miniaudio STATIC ${CMAKE_CURRENT_SOURCE_DIR}/miniaudio/miniaudio.c)
target_include_directories(miniaudio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/miniaudio)

# -----------------------------------------------------------------------------
# 29. ImGuizmo — transform gizmos + editor widgets (compiled against imgui)
# -----------------------------------------------------------------------------
set(IMGUIZMO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/imguizmo/src)
add_library(imguizmo STATIC
    ${IMGUIZMO_DIR}/ImGuizmo.cpp
    ${IMGUIZMO_DIR}/GraphEditor.cpp
    ${IMGUIZMO_DIR}/ImCurveEdit.cpp
    ${IMGUIZMO_DIR}/ImGradient.cpp
    ${IMGUIZMO_DIR}/ImSequencer.cpp
    ${IMGUIZMO_DIR}/ImVectorEditor.cpp
)
target_include_directories(imguizmo PUBLIC ${IMGUIZMO_DIR})
target_link_libraries(imguizmo PUBLIC imgui)

# -----------------------------------------------------------------------------
# 30. ImPlot — plotting for debug/profiler UI (compiled against imgui)
# -----------------------------------------------------------------------------
set(IMPLOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/implot)
add_library(implot STATIC
    ${IMPLOT_DIR}/implot.cpp
    ${IMPLOT_DIR}/implot_items.cpp
)
target_include_directories(implot PUBLIC ${IMPLOT_DIR})
target_link_libraries(implot PUBLIC imgui)

# -----------------------------------------------------------------------------
# 31. Catch2 v3 — unit-test framework (targets Catch2::Catch2[/WithMain])
# -----------------------------------------------------------------------------
if(VK_TP_BUILD_CATCH2)
    set(CATCH_INSTALL_DOCS    OFF CACHE BOOL "" FORCE)
    set(CATCH_INSTALL_EXTRAS  OFF CACHE BOOL "" FORCE)
    set(CATCH_DEVELOPMENT_BUILD OFF CACHE BOOL "" FORCE)
    set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH};${CMAKE_CURRENT_SOURCE_DIR}/catch2/extras" CACHE STRING "" FORCE)
    add_subdirectory(catch2)
endif()

# -----------------------------------------------------------------------------
# 31b. icetea + tapioca (Mashiro infrastructure)
# -----------------------------------------------------------------------------
add_subdirectory(icetea)
add_subdirectory(tapioca)

# -----------------------------------------------------------------------------
# 31c. Eigen — header-only linear algebra library (used as oracle in math tests)
# -----------------------------------------------------------------------------
add_library(eigen INTERFACE)
target_include_directories(eigen SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/eigen)

# -----------------------------------------------------------------------------
# 32. efsw — cross-platform filesystem watcher.
#     With BUILD_SHARED_LIBS OFF the unconditional add_library(efsw) becomes a
#     STATIC lib; we disable BUILD_STATIC_LIBS to avoid the duplicate
#     `efsw-static` target. Consume target `efsw`.
# -----------------------------------------------------------------------------
if(VK_TP_BUILD_EFSW)
    set(BUILD_STATIC_LIBS OFF CACHE BOOL "" FORCE)  # no duplicate efsw-static
    set(BUILD_TEST_APP    OFF CACHE BOOL "" FORCE)
    set(EFSW_INSTALL      OFF CACHE BOOL "" FORCE)
    add_subdirectory(efsw)
endif()

# -----------------------------------------------------------------------------
# 33. msdfgen — (multi-channel) signed-distance-field generation.
#     CORE_ONLY drops every external dependency (no FreeType / Skia / PNG / SVG),
#     so it builds standalone. Font-file rasterisation would need the -ext lib
#     plus FreeType; add that later if glyph loading is required.
#     Consume target msdfgen::msdfgen-core.
# -----------------------------------------------------------------------------
if(VK_TP_BUILD_MSDFGEN)
    set(MSDFGEN_CORE_ONLY        ON  CACHE BOOL "" FORCE)
    set(MSDFGEN_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
    set(MSDFGEN_USE_VCPKG        OFF CACHE BOOL "" FORCE)
    set(MSDFGEN_USE_SKIA         OFF CACHE BOOL "" FORCE)
    set(MSDFGEN_USE_OPENMP       OFF CACHE BOOL "" FORCE)
    set(MSDFGEN_INSTALL          OFF CACHE BOOL "" FORCE)
    set(MSDFGEN_DYNAMIC_RUNTIME  ON  CACHE BOOL "" FORCE)  # match project /MD (avoid LNK2038)
    add_subdirectory(msdfgen)
endif()

# -----------------------------------------------------------------------------
# 34. Jolt Physics (CMake lives in jolt/Build, target `Jolt`).
#       * OVERRIDE_CXX_FLAGS OFF  -> don't clobber our global /O2 etc.
#       * ENABLE_ALL_WARNINGS OFF -> upstream turns on -Werror otherwise.
#       * USE_STATIC_MSVC_RUNTIME_LIBRARY OFF -> match the project-wide /MD;
#         the default ON would emit /MT and collide at link time.
#     Jolt propagates its JPH_* config defines as PUBLIC on the target, so
#     consumers need no manual macro matching.
# -----------------------------------------------------------------------------
if(VK_TP_BUILD_JOLT)
    set(OVERRIDE_CXX_FLAGS                 OFF CACHE BOOL "" FORCE)
    set(INTERPROCEDURAL_OPTIMIZATION       OFF CACHE BOOL "" FORCE)
    set(ENABLE_ALL_WARNINGS                OFF CACHE BOOL "" FORCE)
    set(FLOATING_POINT_EXCEPTIONS_ENABLED  OFF CACHE BOOL "" FORCE)
    set(USE_STATIC_MSVC_RUNTIME_LIBRARY    OFF CACHE BOOL "" FORCE)
    set(ENABLE_INSTALL                     OFF CACHE BOOL "" FORCE)
    set(TARGET_UNIT_TESTS                  OFF CACHE BOOL "" FORCE)
    set(TARGET_HELLO_WORLD                 OFF CACHE BOOL "" FORCE)
    set(TARGET_PERFORMANCE_TEST            OFF CACHE BOOL "" FORCE)
    set(TARGET_SAMPLES                     OFF CACHE BOOL "" FORCE)
    set(TARGET_VIEWER                      OFF CACHE BOOL "" FORCE)
    add_subdirectory(jolt/Build)
endif()

# -----------------------------------------------------------------------------
# 34b. NVIDIA PhysX SDK 5.x.
#      The upstream Windows CMake files still assume an MSVC/cl frontend and
#      globally rewrite CMAKE_CXX_FLAGS with /std:c++14, /W4, /WX, /GR-, etc.
#      This repository uses the COCA clang-p2996 GNU-like frontend, so directly
#      adding PhysX's native build would pollute the whole thirdparty directory.
#      For now we expose a stable, warning-free public-header target. A real
#      runtime target should be added as a separate ExternalProject or imported
#      prebuilt SDK once we decide on clang-cl vs upstream binaries.
# -----------------------------------------------------------------------------
if(VK_TP_ENABLE_PHYSX)
    set(PHYSX_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/physx/physx)
    if(NOT EXISTS "${PHYSX_SDK_ROOT}/include/PxPhysicsAPI.h")
        message(FATAL_ERROR "[vulkan/thirdparty] PhysX SDK missing at ${PHYSX_SDK_ROOT}. "
                            "Expected NVIDIA-Omniverse/PhysX checkout under thirdparty/physx.")
    endif()

    add_library(physx_headers INTERFACE)
    target_include_directories(physx_headers SYSTEM INTERFACE ${PHYSX_SDK_ROOT}/include)
    target_compile_features(physx_headers INTERFACE cxx_std_17)
    target_compile_definitions(physx_headers INTERFACE
        PX_PHYSX_STATIC_LIB
        PX_SIMD_DISABLED
        DISABLE_CUDA_PHYSX
    )
    add_library(PhysX::Headers ALIAS physx_headers)

    message(STATUS "[vulkan/thirdparty] PhysX SDK:       ${PHYSX_SDK_ROOT} (headers only)")
endif()

# -----------------------------------------------------------------------------
# 35. oneTBB — work-stealing task scheduler.
#     oneTBB only supports a shared runtime, so it is built as tbb12.dll even
#     though the rest of this tree is static (upstream emits an unconditional
#     warning for static TBB). Deploy the DLL beside consumers with
#     vk_tp_deploy_tbb().
#     Target: TBB::tbb (+ TBB::tbbmalloc).
# -----------------------------------------------------------------------------
if(VK_TP_BUILD_TBB)
    set(TBB_TEST     OFF CACHE BOOL "" FORCE)
    set(TBB_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(TBB_STRICT   OFF CACHE BOOL "" FORCE)  # no -Werror
    set(TBB_INSTALL  OFF CACHE BOOL "" FORCE)
    set(TBBMALLOC_PROXY_BUILD OFF CACHE BOOL "" FORCE)  # proxy needs CRT symbols unavailable with our sysroot
    # oneTBB only supports a shared runtime; a static build trips an
    # unconditional upstream warning (CMakeLists.txt:160) and is unsupported.
    # Shadow the cached BUILD_SHARED_LIBS=OFF with a normal var for TBB only
    # (emits tbb12.dll), then drop it so the rest of the tree stays static.
    # Deploy the runtime DLL beside consumers with vk_tp_deploy_tbb().
    set(BUILD_SHARED_LIBS ON)
    add_subdirectory(tbb)
    unset(BUILD_SHARED_LIBS)
endif()

# vk_tp_deploy_runtime(<target>): copy every shared-library DLL that <target>
# transitively links (assimp-*, glfw3, ktx, meshoptimizer, spdlog, tbb12, ...)
# next to its executable. Statically-linked libraries contribute nothing.
# Uses $<TARGET_RUNTIME_DLLS> so the set always tracks the real link graph.
#
# Additionally copies COCA toolchain runtime DLLs (c++.dll, and the ASan
# dynamic runtime when sanitizers are enabled) which are not tracked by
# CMake's target graph but are required at process start.
function(vk_tp_deploy_runtime tgt)
    add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_RUNTIME_DLLS:${tgt}>" "$<TARGET_FILE_DIR:${tgt}>"
        COMMAND_EXPAND_LISTS
        COMMENT "Deploying runtime DLLs -> $<TARGET_FILE_DIR:${tgt}>")

    # -- COCA toolchain runtime DLLs ------------------------------------------
    # COCA_RUNTIME_DLLS is populated by the toolchain (c++.dll, etc.).
    # COCA_ASAN_RUNTIME_DLL is conditionally appended when ASan is active.
    set(_deploy_dlls ${COCA_RUNTIME_DLLS})
    if(DEFINED COCA_ASAN_RUNTIME_DLL)
        if(CMAKE_CXX_FLAGS_DEBUG MATCHES "-fsanitize=address"
           OR CMAKE_CXX_FLAGS MATCHES "-fsanitize=address")
            list(APPEND _deploy_dlls "${COCA_ASAN_RUNTIME_DLL}")
        endif()
    endif()
    foreach(_dll IN LISTS _deploy_dlls)
        if(EXISTS "${_dll}")
            cmake_path(GET _dll FILENAME _dll_name)
            add_custom_command(TARGET ${tgt} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_dll}" "$<TARGET_FILE_DIR:${tgt}>"
                COMMENT "Deploying ${_dll_name} -> $<TARGET_FILE_DIR:${tgt}>")
        endif()
    endforeach()
endfunction()

# Backwards-compatible alias: runtime deployment now covers TBB and every other
# shared dependency in one shot.
function(vk_tp_deploy_tbb tgt)
    vk_tp_deploy_runtime(${tgt})
endfunction()

# -----------------------------------------------------------------------------
# 36. METIS + GKlib — graph/mesh partitioner.
#     We bypass the upstream build systems and compile both as static libs:
#       * GKlib uses its bundled regex on MSVC (USE_GKREGEX), and trips the CRT
#         secure-warning wall otherwise.
#       * metis.h leaves IDXTYPEWIDTH / REALTYPEWIDTH #defined-out, so they MUST
#         be supplied as PUBLIC defs or the header fails to preprocess. 32/32 is
#         the standard (32-bit indices, single-precision reals) configuration.
#     Target: metis (links gklib transitively).
# -----------------------------------------------------------------------------
if(VK_TP_BUILD_METIS)
    file(GLOB GKLIB_SOURCES CONFIGURE_DEPENDS
         ${CMAKE_CURRENT_SOURCE_DIR}/gklib/src/*.c)
    add_library(gklib STATIC ${GKLIB_SOURCES})
    target_include_directories(gklib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/gklib/include)
    if(MSVC)
        target_compile_definitions(gklib PUBLIC USE_GKREGEX __thread=__declspec\(thread\))
        target_compile_definitions(gklib PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(gklib PRIVATE /wd4101 /wd4102 /wd4244 /wd4267 /wd4996)
    endif()

    file(GLOB METIS_SOURCES CONFIGURE_DEPENDS
         ${CMAKE_CURRENT_SOURCE_DIR}/metis/libmetis/*.c)
    add_library(metis STATIC ${METIS_SOURCES})
    target_include_directories(metis
        PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/metis/include
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/metis/libmetis)
    target_compile_definitions(metis PUBLIC IDXTYPEWIDTH=32 REALTYPEWIDTH=32)
    target_link_libraries(metis PUBLIC gklib)
    if(MSVC)
        target_compile_definitions(metis PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(metis PRIVATE /wd4101 /wd4244 /wd4267 /wd4996)
    endif()
endif()

# -----------------------------------------------------------------------------
# Convenience: shaderc / glslang from SDK (NOT vendored, comes from find_package)
# -----------------------------------------------------------------------------
# Parent projects can use:
#   find_package(Vulkan REQUIRED COMPONENTS glslc)
# and invoke ${Vulkan_GLSLC_EXECUTABLE} to compile shaders at build time.

# -----------------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------------
message(STATUS "[vulkan/thirdparty] Targets exposed:")
message(STATUS "  Vulkan        : Vulkan::Vulkan vma volk volk_headers")
message(STATUS "  Bootstrap     : vk-bootstrap")
message(STATUS "  Shading       : $<$<BOOL:${VK_TP_BUILD_SLANG}>:slang (runtime compiler, vk_tp_deploy_slang())>")
message(STATUS "  Reflection    : spirv-cross-core/-glsl/-hlsl/-msl/-cpp/-reflect/-util  spirv-reflect")
message(STATUS "  Textures      : $<$<BOOL:${VK_TP_BUILD_KTX}>:ktx> stb tinyexr")
message(STATUS "  Window/UI     : glfw glad imgui")
message(STATUS "  Math          : glm")
message(STATUS "  Assets        : $<$<BOOL:${VK_TP_BUILD_ASSIMP}>:assimp> tinyobjloader tinygltf cgltf fastgltf meshoptimizer mikktspace")
message(STATUS "  Utility       : spdlog::spdlog nlohmann_json::nlohmann_json entt/EnTT::EnTT tinyfiledialogs cli11")
message(STATUS "  Profiling     : TracyClient (active=${VK_TP_BUILD_TRACY})  renderdoc_app $<$<BOOL:${VK_TP_BUILD_PERFETTO}>:perfetto>")
message(STATUS "  Concurrency   : $<$<BOOL:${VK_TP_BUILD_TBB}>:TBB::tbb (vk_tp_deploy_tbb())> taskflow stdexec")
message(STATUS "  UI extras     : imguizmo implot")
message(STATUS "  Physics       : $<$<BOOL:${VK_TP_BUILD_JOLT}>:Jolt> $<$<BOOL:${VK_TP_ENABLE_PHYSX}>:PhysX::Headers>")
message(STATUS "  Geometry      : $<$<BOOL:${VK_TP_BUILD_MSDFGEN}>:msdfgen::msdfgen-core> $<$<BOOL:${VK_TP_BUILD_METIS}>:metis (gklib)>")
message(STATUS "  Audio/IO      : miniaudio $<$<BOOL:${VK_TP_BUILD_EFSW}>:efsw>")
message(STATUS "  Testing       : $<$<BOOL:${VK_TP_BUILD_CATCH2}>:Catch2::Catch2WithMain>")

# -----------------------------------------------------------------------------
# IDE organisation — nest every target created anywhere under this directory
# into a "thirdparty/<top-level-lib>" folder (Visual Studio CMake Targets View,
# Visual Studio / Xcode generators, etc.). Targets declared directly here land
# in "thirdparty". Runs last so all add_subdirectory() targets already exist.
# -----------------------------------------------------------------------------
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(_VK_TP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
function(_vk_tp_folderize dir)
    file(RELATIVE_PATH _rel "${_VK_TP_ROOT}" "${dir}")
    if(_rel STREQUAL "")
        set(_folder "thirdparty")
    else()
        # Collapse to the top-level library directory (e.g. jolt/Build -> jolt,
        # assimp/contrib/zlib -> assimp) so the tree stays one level deep.
        string(REGEX REPLACE "/.*$" "" _top "${_rel}")
        set(_folder "thirdparty/${_top}")
    endif()
    get_property(_tgts DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t IN LISTS _tgts)
        set_target_properties(${_t} PROPERTIES FOLDER "${_folder}")
    endforeach()
    get_property(_subs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_s IN LISTS _subs)
        _vk_tp_folderize("${_s}")
    endforeach()
endfunction()
_vk_tp_folderize("${CMAKE_CURRENT_SOURCE_DIR}")

# -----------------------------------------------------------------------------
# Warning suppression — disable ALL compiler warnings for third-party code.
# Two mechanisms:
#   1. Inject -w (Clang/GCC) or /W0 (MSVC) into every compilable target so
#      warnings are silenced when building those TUs.
#   2. Mark include directories as SYSTEM so consumers never see warnings from
#      third-party headers either (CMake >= 3.25 SYSTEM property).
# -----------------------------------------------------------------------------
function(_vk_tp_silence_warnings dir)
    get_property(_tgts DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t IN LISTS _tgts)
        get_target_property(_type ${_t} TYPE)
        if(_type STREQUAL "INTERFACE_LIBRARY" OR _type STREQUAL "UTILITY")
            continue()
        endif()
        # Suppress warnings in the library's own source files.
        target_compile_options(${_t} PRIVATE
            $<$<C_COMPILER_ID:Clang,AppleClang,GNU>:-w>
            $<$<C_COMPILER_ID:MSVC>:/W0>
            $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-w>
            $<$<CXX_COMPILER_ID:MSVC>:/W0>)
        # Mark as SYSTEM so consumers' builds stay warning-free from these headers.
        set_target_properties(${_t} PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
                "$<TARGET_PROPERTY:${_t},INTERFACE_INCLUDE_DIRECTORIES>")
    endforeach()
    get_property(_subs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_s IN LISTS _subs)
        _vk_tp_silence_warnings("${_s}")
    endforeach()
endfunction()
_vk_tp_silence_warnings("${CMAKE_CURRENT_SOURCE_DIR}")

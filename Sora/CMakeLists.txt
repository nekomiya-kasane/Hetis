cmake_minimum_required(VERSION 4.0)

project(Sora LANGUAGES C CXX)

file(GLOB_RECURSE Sora_SOURCES CONFIGURE_DEPENDS
    "src/*.cpp"
    "src/*.h"
)
message(STATUS "${Sora_SOURCES}")

add_library(Sora STATIC)
target_sources(Sora PRIVATE ${Sora_SOURCES})
target_include_directories(Sora PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(Sora PUBLIC tapioca)
target_link_libraries(Sora PUBLIC eigen nlohmann_json::nlohmann_json stdexec)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND DEFINED COCA_TOOLCHAIN_ROOT)
    set(SORA_INTEL_RUNTIME_DIR "${COCA_TOOLCHAIN_ROOT}/tools/ifort/lib")
    set(SORA_SVML_LIBRARY "${SORA_INTEL_RUNTIME_DIR}/svml_dispmt.lib")
    set(SORA_IRC_LIBRARY "${SORA_INTEL_RUNTIME_DIR}/libirc.lib")
    if(EXISTS "${SORA_SVML_LIBRARY}" AND EXISTS "${SORA_IRC_LIBRARY}")
        target_compile_options(Sora PUBLIC "$<$<COMPILE_LANGUAGE:CXX>:-fveclib=SVML>")
        target_link_directories(Sora PUBLIC "${SORA_INTEL_RUNTIME_DIR}")
        target_link_libraries(Sora PUBLIC "${SORA_SVML_LIBRARY}" "${SORA_IRC_LIBRARY}")
        message(STATUS "Sora SIMD transcendental backend: Intel SVML")
    endif()
endif()

if(WIN32)
    target_link_libraries(Sora PRIVATE advapi32 dwmapi imm32 ole32 shell32 user32)
endif()
set_target_properties(Sora PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    FOLDER "Sora"

    COMPILE_PDB_NAME "Sora.Comp"
)

set_property(GLOBAL PROPERTY SORA_DEMO_TARGETS "")
set_property(GLOBAL PROPERTY SORA_TEST_TARGETS "")

add_subdirectory(demos)
add_subdirectory(tests)
add_subdirectory(benchmarks)

get_property(SORA_DEMO_TARGETS GLOBAL PROPERTY SORA_DEMO_TARGETS)
get_property(SORA_TEST_TARGETS GLOBAL PROPERTY SORA_TEST_TARGETS)

add_custom_target(Sora.BuildAll)
add_dependencies(Sora.BuildAll
    Sora
    ${SORA_DEMO_TARGETS}
    ${SORA_TEST_TARGETS}
)
set_target_properties(Sora.BuildAll PROPERTIES FOLDER "Sora")

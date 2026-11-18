cmake_minimum_required(VERSION 4.0)

# Gate for the experimental build-database export used below
# (CMAKE_EXPORT_BUILD_DATABASE). The UUID is specific to CMake 4.2.x and must be
# set before project()/toolchain discovery; update it when upgrading CMake.
set(CMAKE_EXPERIMENTAL_EXPORT_BUILD_DATABASE "73194a1d-c0b5-41b9-9190-a4512925e192")

project(Hetis LANGUAGES C CXX)

enable_testing()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(ReflectionFeatureProbes)

if(NOT DEFINED CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# The bundled libc++ is built against the release UCRT for every configuration. Mixing it with CMake's default
# Debug /MDd runtime crosses allocator and thread-local-state domains at DLL boundaries.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

add_compile_definitions(
    "$<$<CONFIG:Debug>:DEBUG>"
    "$<$<NOT:$<CONFIG:Debug>>:NDEBUG>"
)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build dependencies as static libraries")

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_EXPORT_BUILD_DATABASE ON)

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_COMPILE_PDB_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_PDB_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(CTest)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/misc" misc)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/thirdparty" thirdparty)

add_subdirectory(Sora)
add_subdirectory(Kirara)

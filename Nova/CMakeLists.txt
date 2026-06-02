cmake_minimum_required(VERSION 4.0)

project(Nova LANGUAGES C CXX)

file(GLOB_RECURSE NOVA_SOURCES CONFIGURE_DEPENDS
    "src/*.cpp"
    "src/*.h"
)

add_library(Nova SHARED)
target_sources(Nova PRIVATE ${NOVA_SOURCES})
target_include_directories(Nova PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
set_target_properties(Nova PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    FOLDER "Nova"

    COMPILE_PDB_NAME "Nova.Comp"
)

add_subdirectory(tests)

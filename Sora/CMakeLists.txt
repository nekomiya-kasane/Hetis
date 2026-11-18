cmake_minimum_required(VERSION 4.0)

project(Sora LANGUAGES C CXX)

file(GLOB_RECURSE Sora_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/**.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/**.h"
)

add_library(Sora STATIC)
target_sources(Sora PRIVATE ${Sora_SOURCES})
target_include_directories(Sora PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(Sora PUBLIC simdjson::simdjson stdexec)

set_target_properties(Sora PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    FOLDER "Sora"

    COMPILE_PDB_NAME "Sora.Comp"
)

add_custom_target(Sora.BuildAll)
set_target_properties(Sora.BuildAll PROPERTIES FOLDER "Sora")

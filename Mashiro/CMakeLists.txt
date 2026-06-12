cmake_minimum_required(VERSION 4.0)

project(Mashiro LANGUAGES C CXX)

file(GLOB_RECURSE MASHIRO_SOURCES CONFIGURE_DEPENDS
    "src/*.cpp"
    "src/*.h"
)

add_library(Mashiro SHARED)
target_sources(Mashiro PRIVATE ${MASHIRO_SOURCES})
target_include_directories(Mashiro PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(Mashiro PRIVATE tapioca)
target_link_libraries(Mashiro PUBLIC nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(Mashiro PRIVATE dbghelp)
endif()
set_target_properties(Mashiro PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    FOLDER "Mashiro"

    COMPILE_PDB_NAME "Mashiro.Comp"
)

add_subdirectory(demos)
add_subdirectory(tests)

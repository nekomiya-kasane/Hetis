cmake_minimum_required(VERSION 4.0)

project(Yuki LANGUAGES C CXX)

file(GLOB_RECURSE YUKI_SOURCES CONFIGURE_DEPENDS
    "src/*.cpp"
    "src/*.h"
)

add_library(Yuki SHARED)
target_sources(Yuki PRIVATE ${YUKI_SOURCES})
target_include_directories(Yuki PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(Yuki PRIVATE Mashiro)

target_link_libraries(Yuki PRIVATE tapioca)
target_link_libraries(Yuki PUBLIC nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(Yuki PRIVATE dbghelp)
endif()
set_target_properties(Yuki PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    FOLDER "Yuki"

    COMPILE_PDB_NAME "Yuki.Comp"
)

add_subdirectory(demos)
add_subdirectory(tests)

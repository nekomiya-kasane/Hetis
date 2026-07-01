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
target_link_libraries(Sora PRIVATE tapioca)
target_link_libraries(Sora PUBLIC nlohmann_json::nlohmann_json stdexec)
if(WIN32)
    target_link_libraries(Sora PRIVATE advapi32 dbghelp dwmapi imm32 ole32 shell32 user32)
endif()
set_target_properties(Sora PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    FOLDER "Sora"

    COMPILE_PDB_NAME "Sora.Comp"
)

add_subdirectory(demos)
add_subdirectory(tests)

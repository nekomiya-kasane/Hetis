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
target_link_libraries(Sora PUBLIC nlohmann_json::nlohmann_json stdexec)
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

get_property(SORA_DEMO_TARGETS GLOBAL PROPERTY SORA_DEMO_TARGETS)
get_property(SORA_TEST_TARGETS GLOBAL PROPERTY SORA_TEST_TARGETS)

add_custom_target(Sora.BuildAll)
add_dependencies(Sora.BuildAll
    Sora
    ${SORA_DEMO_TARGETS}
    ${SORA_TEST_TARGETS}
)
set_target_properties(Sora.BuildAll PROPERTIES FOLDER "Sora")

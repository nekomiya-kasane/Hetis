cmake_minimum_required(VERSION 4.0)

project(SoraEventPortBench LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(VULKAN_WORKSPACE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(SORA_ROOT "${VULKAN_WORKSPACE_ROOT}/Sora")
set(THIRDPARTY_ROOT "${VULKAN_WORKSPACE_ROOT}/thirdparty")

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

add_library(nlohmann_json INTERFACE)
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
target_include_directories(nlohmann_json INTERFACE "${THIRDPARTY_ROOT}/nlohmann_json/single_include")

add_library(stdexec INTERFACE)
target_include_directories(stdexec INTERFACE "${THIRDPARTY_ROOT}/stdexec/include")
target_compile_features(stdexec INTERFACE cxx_std_20)

add_subdirectory("${THIRDPARTY_ROOT}/icetea" "${CMAKE_BINARY_DIR}/thirdparty/icetea")
add_subdirectory("${THIRDPARTY_ROOT}/tapioca" "${CMAKE_BINARY_DIR}/thirdparty/tapioca")

file(GLOB_RECURSE SORA_BENCH_SOURCES CONFIGURE_DEPENDS
    "${SORA_ROOT}/src/*.cpp"
    "${SORA_ROOT}/src/*.h"
)

add_library(Sora.EventPortBench.Sora STATIC ${SORA_BENCH_SOURCES})
target_include_directories(Sora.EventPortBench.Sora PUBLIC
    "${SORA_ROOT}/src"
    "${SORA_ROOT}/include"
)
target_link_libraries(Sora.EventPortBench.Sora PUBLIC tapioca nlohmann_json::nlohmann_json stdexec)
if(WIN32)
    target_link_libraries(Sora.EventPortBench.Sora PRIVATE advapi32 dwmapi imm32 ole32 shell32 user32)
endif()
set_target_properties(Sora.EventPortBench.Sora PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

add_executable(Sora.EventPortBench EventPortBench.cpp)
target_link_libraries(Sora.EventPortBench PRIVATE Sora.EventPortBench.Sora)
set_target_properties(Sora.EventPortBench PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
)

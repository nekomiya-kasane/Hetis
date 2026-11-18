cmake_minimum_required(VERSION 4.2)

project(Kirara)

add_library(Kirara SHARED)
target_include_directories(Kirara PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
file(GLOB_RECURSE _SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/**.h" "${CMAKE_CURRENT_SOURCE_DIR}/src/**.cpp")
target_sources(Kirara PRIVATE "${_SOURCES}")
target_link_libraries(Kirara PRIVATE Impeller::ImpellerShared)
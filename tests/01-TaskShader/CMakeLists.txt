cmake_minimum_required(VERSION 3.25)

project(TaskShaderCube LANGUAGES CXX)

find_package(Vulkan REQUIRED)

add_executable(task_shader_cube main.cpp)

target_link_libraries(task_shader_cube PRIVATE
    Vulkan::Headers
    volk
    vk-bootstrap
    glfw
    glm
    spdlog::spdlog
)

target_compile_definitions(task_shader_cube PRIVATE
    VK_NO_PROTOTYPES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_LEFT_HANDED
)

# ── Compile Slang shaders → SPIR-V ──────────────────────────────────────────
if(NOT DEFINED SLANG_SLANGC_EXECUTABLE)
    message(FATAL_ERROR "SLANG_SLANGC_EXECUTABLE not set. Enable VK_TP_BUILD_SLANG.")
endif()

set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})

# slangc compiles each entry-point from a single .slang module
set(SLANG_SOURCE ${SHADER_DIR}/cube.slang)

# Task shader
add_custom_command(
    OUTPUT ${SHADER_OUT}/cube.task.spv
    COMMAND ${SLANG_SLANGC_EXECUTABLE}
        ${SLANG_SOURCE}
        -entry taskMain -stage amplification
        -target spirv -profile spirv_1_4
        -o ${SHADER_OUT}/cube.task.spv
    DEPENDS ${SLANG_SOURCE}
    COMMENT "slangc: cube.slang [taskMain -> task.spv]"
)
# Mesh shader
add_custom_command(
    OUTPUT ${SHADER_OUT}/cube.mesh.spv
    COMMAND ${SLANG_SLANGC_EXECUTABLE}
        ${SLANG_SOURCE}
        -entry meshMain -stage mesh
        -target spirv -profile spirv_1_4
        -o ${SHADER_OUT}/cube.mesh.spv
    DEPENDS ${SLANG_SOURCE}
    COMMENT "slangc: cube.slang [meshMain -> mesh.spv]"
)
# Fragment shader
add_custom_command(
    OUTPUT ${SHADER_OUT}/cube.frag.spv
    COMMAND ${SLANG_SLANGC_EXECUTABLE}
        ${SLANG_SOURCE}
        -entry fragMain -stage fragment
        -target spirv -profile spirv_1_4
        -o ${SHADER_OUT}/cube.frag.spv
    DEPENDS ${SLANG_SOURCE}
    COMMENT "slangc: cube.slang [fragMain -> frag.spv]"
)

add_custom_target(task_shader_cube_shaders ALL DEPENDS
    ${SHADER_OUT}/cube.task.spv
    ${SHADER_OUT}/cube.mesh.spv
    ${SHADER_OUT}/cube.frag.spv
)
add_dependencies(task_shader_cube task_shader_cube_shaders)

target_compile_definitions(task_shader_cube PRIVATE
    SHADER_DIR="${SHADER_OUT}"
)

vk_tp_deploy_runtime(task_shader_cube)
vk_tp_deploy_slang(task_shader_cube)

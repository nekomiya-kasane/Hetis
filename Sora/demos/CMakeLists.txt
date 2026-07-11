function(add_sora_demo CATEGORY DEMO_NAME)
    set(DEMO_TARGET Demo.Sora.${CATEGORY}.${DEMO_NAME})

    add_executable(${DEMO_TARGET} ${CATEGORY}/${DEMO_NAME}.cpp)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${CATEGORY}/${DEMO_NAME}.Providers.cpp")
        target_sources(${DEMO_TARGET} PRIVATE "${CATEGORY}/${DEMO_NAME}.Providers.cpp")
    endif()
    target_include_directories(${DEMO_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${DEMO_TARGET} PRIVATE Sora Catch2::Catch2WithMain TBB::tbb)
    set_target_properties(${DEMO_TARGET} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        FOLDER "Sora/Demos/${CATEGORY}"
    )
    vk_tp_deploy_runtime(${DEMO_TARGET})
    set_property(GLOBAL APPEND PROPERTY SORA_DEMO_TARGETS ${DEMO_TARGET})
endfunction()

function(add_sora_demo_module CATEGORY MODULE_NAME)
    set(MODULE_TARGET Demo.Sora.${CATEGORY}.${MODULE_NAME})

    add_library(${MODULE_TARGET} SHARED ${CATEGORY}/${MODULE_NAME}.cpp)
    target_include_directories(${MODULE_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${MODULE_TARGET} PRIVATE Sora TBB::tbb)
    set_target_properties(${MODULE_TARGET} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        FOLDER "Sora/Demos/${CATEGORY}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    )
    vk_tp_deploy_runtime(${MODULE_TARGET})
    set_property(GLOBAL APPEND PROPERTY SORA_DEMO_TARGETS ${MODULE_TARGET})
endfunction()

function(add_sora_asset_resource_library TARGET)
    add_library(${TARGET} SHARED ${ARGN})
    target_include_directories(${TARGET} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../src"
        "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    )
    target_compile_options(${TARGET} PRIVATE
        -fno-sanitize=address
        -fno-sanitize=undefined
        -Wno-c23-extensions
    )
    target_link_options(${TARGET} PRIVATE
        -fno-sanitize=address
        -fno-sanitize=undefined
    )
    if(WIN32)
        target_link_options(${TARGET} PRIVATE
            LINKER:/NOENTRY
            LINKER:/NODEFAULTLIB
        )
    endif()
    set_target_properties(${TARGET} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        PREFIX ""
        SUFFIX ".lpak"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}"
    )
endfunction()

function(add_sora_demo_asset_resource_library CATEGORY MODULE_NAME)
    set(MODULE_TARGET Demo.Sora.${CATEGORY}.${MODULE_NAME})

    add_sora_asset_resource_library(${MODULE_TARGET} ${CATEGORY}/${MODULE_NAME}.cpp)
    target_include_directories(${MODULE_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    set_target_properties(${MODULE_TARGET} PROPERTIES FOLDER "Sora/Demos/${CATEGORY}")
    set_property(GLOBAL APPEND PROPERTY SORA_DEMO_TARGETS ${MODULE_TARGET})
endfunction()

include(CTest)
include(Catch)

add_sora_demo(Core ABIDemo)
add_sora_demo(Core TraitsDemo)
add_sora_demo(Core PolymorphismDemo)
add_sora_demo(Core HashDemo)
add_sora_demo(Core ToStringDemo)
add_sora_demo(Core GetSetDemo)
add_sora_demo(Core CLIDemo)

add_sora_demo(PAL ModuleDemo)

add_sora_demo(Kernel IIDDemo)
add_sora_demo(Kernel BaseObjectDemo)

function(add_sora_demo CATEGORY DEMO_NAME)
    set(DEMO_TARGET Demo.Sora.${CATEGORY}.${DEMO_NAME})

    add_executable(${DEMO_TARGET} ${CATEGORY}/${DEMO_NAME}.cpp)
    target_include_directories(${DEMO_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${DEMO_TARGET} PRIVATE Sora Catch2::Catch2WithMain TBB::tbb)
    set_target_properties(${DEMO_TARGET} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        FOLDER "Sora/Demos/${CATEGORY}"
    )
    vk_tp_deploy_runtime(${DEMO_TARGET})
endfunction()

include(CTest)
include(Catch)

add_sora_demo(Core ABIDemo)
add_sora_demo(Core TraitsDemo)
add_sora_demo(Core PolymorphismDemo)
add_sora_demo(Core HashDemo)
add_sora_demo(Core ToStringDemo)

add_sora_demo(PAL ModuleDemo)

add_sora_demo(Kernel IIDDemo)
add_sora_demo(Kernel BaseObjectDemo)
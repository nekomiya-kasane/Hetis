function(add_sora_demo CATEGORY DEMO_NAME)
    set(DEMO_TARGET Demo.Sora.${CATEGORY}.${DEMO_NAME})

    add_executable(${DEMO_TARGET} ${CATEGORY}/${DEMO_NAME}.cpp)
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
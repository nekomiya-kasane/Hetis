function(add_mashiro_demo CATEGORY DEMO_NAME)
    set(DEMO_TARGET Demo.${CATEGORY}.${DEMO_NAME})

    add_executable(${DEMO_TARGET} ${CATEGORY}/${DEMO_NAME}.cpp)
    target_link_libraries(${DEMO_TARGET} PRIVATE Mashiro Catch2::Catch2WithMain)
    set_target_properties(${DEMO_TARGET} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        FOLDER "Mashiro/Demos/${CATEGORY}"
    )
    vk_tp_deploy_runtime(${DEMO_TARGET})
endfunction()

include(CTest)
include(Catch)

add_mashiro_demo(Core ToStringDemo)
add_mashiro_demo(Core ToJsonDemo)

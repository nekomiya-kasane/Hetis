function(add_yuki_demo CATEGORY DEMO_NAME)
    set(DEMO_TARGET Demo.${CATEGORY}.${DEMO_NAME})

    add_executable(${DEMO_TARGET} ${CATEGORY}/${DEMO_NAME}.cpp)
    target_link_libraries(${DEMO_TARGET} PRIVATE Yuki Catch2::Catch2WithMain)
    set_target_properties(${DEMO_TARGET} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        FOLDER "Yuki/Demos/${CATEGORY}"
    )
    vk_tp_deploy_runtime(${DEMO_TARGET})
endfunction()

include(CTest)
include(Catch)

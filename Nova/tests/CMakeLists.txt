function(add_nova_test CATEGORY TEST_NAME)
    add_executable(${CATEGORY}.${TEST_NAME} ${CATEGORY}/${TEST_NAME}.cpp)
    target_link_libraries(${CATEGORY}.${TEST_NAME} PRIVATE Nova Catch2::Catch2WithMain)
    set_target_properties(${CATEGORY}.${TEST_NAME} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        FOLDER "Nova/Tests/${CATEGORY}"
    )
    vk_tp_deploy_runtime(${CATEGORY}.${TEST_NAME})
    catch_discover_tests(${CATEGORY}.${TEST_NAME})
endfunction()

include(CTest)
include(Catch)

add_nova_test(core ErrorCodeTest)

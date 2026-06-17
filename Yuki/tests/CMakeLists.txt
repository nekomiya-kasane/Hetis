function(add_yuki_test CATEGORY TEST_NAME)
    set(TEST_TARGET Test.${CATEGORY}.${TEST_NAME})

    add_executable(${TEST_TARGET} ${CATEGORY}/${TEST_NAME}.cpp)
    target_include_directories(${TEST_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${TEST_TARGET} PRIVATE
        Yuki Mashiro
        TestSupport
        Catch2::Catch2WithMain ${ARGN}
    )
    set_target_properties(${TEST_TARGET} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        FOLDER "Yuki/Tests/${CATEGORY}"
    )
    vk_tp_deploy_runtime(${TEST_TARGET})
    catch_discover_tests(${TEST_TARGET})
endfunction()

add_yuki_test(Core ClassTypeTest)

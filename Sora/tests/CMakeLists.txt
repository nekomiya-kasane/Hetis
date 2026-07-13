function(add_sora_test CATEGORY TEST_NAME)
    set(TEST_TARGET Test.Sora.${CATEGORY}.${TEST_NAME})

    add_executable(${TEST_TARGET} ${CATEGORY}/${TEST_NAME}.cpp)
    target_include_directories(${TEST_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${TEST_TARGET} PRIVATE Sora Catch2::Catch2WithMain)
    set_target_properties(${TEST_TARGET} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        FOLDER "Sora/Tests/${CATEGORY}"
    )
    vk_tp_deploy_runtime(${TEST_TARGET})
    catch_discover_tests(${TEST_TARGET})
    set_property(GLOBAL APPEND PROPERTY SORA_TEST_TARGETS ${TEST_TARGET})
endfunction()

include(CTest)
include(Catch)

add_sora_test(Resources ResourcePackageTest)
add_sora_test(Core CLITest)
add_sora_test(Core CLIFragmentTest)
add_sora_test(Core ComAdaptorTest)
add_sora_test(Core RefPtrTest)
add_sora_test(Core SOATest)
add_sora_test(Core StructuredLoggerTest)
add_sora_test(Core HiveTest)
add_sora_test(Core MemoryLayoutTest)
add_sora_test(Core WireTest)
add_sora_test(Core FunctionalTest)
add_sora_test(Core UriTest)

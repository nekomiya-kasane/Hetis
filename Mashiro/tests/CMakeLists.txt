add_library(TestSupport INTERFACE)
target_include_directories(TestSupport INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/Support")

function(add_mashiro_test CATEGORY TEST_NAME)
    set(TEST_TARGET Test.${CATEGORY}.${TEST_NAME})

    add_executable(${TEST_TARGET} ${CATEGORY}/${TEST_NAME}.cpp)
    target_include_directories(${TEST_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${TEST_TARGET} PRIVATE Mashiro Catch2::Catch2WithMain ${ARGN})
    set_target_properties(${TEST_TARGET} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        FOLDER "Mashiro/Tests/${CATEGORY}"
    )
    vk_tp_deploy_runtime(${TEST_TARGET})
    catch_discover_tests(${TEST_TARGET})
endfunction()

include(CTest)
include(Catch)

add_mashiro_test(Core ToStringTest)
add_mashiro_test(Core QuanterionTest)
add_mashiro_test(Core MathUtilsTest)
add_mashiro_test(Core AlgebraTest)
add_mashiro_test(Core ScalarMathTest)
add_mashiro_test(Core GeometryTest)
add_mashiro_test(Core GeomOpsTest)
add_mashiro_test(Core AlgebraApproxTest)
add_mashiro_test(Core HashTest)
add_mashiro_test(Core Int128Test)
add_mashiro_test(Core FlagsTest)
add_mashiro_test(Core InlineFunctionTest)
add_mashiro_test(Core ChunkedSlotMapTest)
add_mashiro_test(Core LinearAllocatorTest)
add_mashiro_test(Core SpscRingBufferTest)
add_mashiro_test(Core SeqLockTest)
add_mashiro_test(Core ConcurrentObjectPoolTest)
add_mashiro_test(Core StructuredLoggerTest)
add_mashiro_test(Core FixedStringTest)
add_mashiro_test(Core SOATest)
add_mashiro_test(Core ToJsonTest)
add_mashiro_test(Core TypeTraitsTest)
add_mashiro_test(Core DumbPtrTest)
add_mashiro_test(Core ABITest)

add_mashiro_test(Math FunctionalTest)
add_mashiro_test(Math VecTest eigen)
add_mashiro_test(Math MatTest eigen)
add_mashiro_test(Math TypesTest)
add_mashiro_test(Math MathUtilsXform3DTest eigen)
add_mashiro_test(Math MathUtilsXform2DTest eigen)
add_mashiro_test(Math MathStressTest eigen)

add_mashiro_test(Schedular GeneratorTest)

add_mashiro_test(Surface ColorSpaceTest)
add_mashiro_test(Surface WindowTest)

add_mashiro_test(Platform SystemEventTest)
add_mashiro_test(Platform ThreadContractTest)

function(add_mashiro_test CATEGORY TEST_NAME)
    set(TEST_TARGET Test.${CATEGORY}.${TEST_NAME})

    add_executable(${TEST_TARGET} ${CATEGORY}/${TEST_NAME}.cpp)
    target_link_libraries(${TEST_TARGET} PRIVATE Mashiro Catch2::Catch2WithMain)
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
add_mashiro_test(Core TypesTest)
add_mashiro_test(Core QuanterionTest)
add_mashiro_test(Core MathUtilsTest)
add_mashiro_test(Core AlgebraTest)
add_mashiro_test(Core ScalarMathTest)
add_mashiro_test(Core GeometryTest)
add_mashiro_test(Core AlgebraApproxTest)

# Math tests with Eigen reference validation.
function(add_math_test TEST_NAME)
    set(_TGT Test.Math.${TEST_NAME})
    add_executable(${_TGT} Math/${TEST_NAME}.cpp)
    target_include_directories(${_TGT} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${_TGT} PRIVATE Mashiro Catch2::Catch2WithMain eigen)
    set_target_properties(${_TGT} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        FOLDER "Mashiro/Tests/Math"
    )
    vk_tp_deploy_runtime(${_TGT})
    catch_discover_tests(${_TGT})
endfunction()

add_math_test(VecTest)
add_math_test(MatTest)
add_math_test(MathUtilsXform3DTest)
add_math_test(MathUtilsXform2DTest)
add_math_test(MathStressTest)

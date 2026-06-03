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

# Eigen-oracle test: additional link to Eigen (header-only)
set(_EIGEN_TEST Test.Core.EigenOracleTest)
add_executable(${_EIGEN_TEST} Core/EigenOracleTest.cpp)
target_link_libraries(${_EIGEN_TEST} PRIVATE Mashiro Catch2::Catch2WithMain eigen)
set_target_properties(${_EIGEN_TEST} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Mashiro/Tests/Core"
)
vk_tp_deploy_runtime(${_EIGEN_TEST})
catch_discover_tests(${_EIGEN_TEST})

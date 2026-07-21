function(add_sora_test_main TARGET_NAME INSTALL_CRASH_HANDLER)
    add_library(${TARGET_NAME} OBJECT TestMain.cpp)
    target_compile_definitions(${TARGET_NAME} PRIVATE SORA_TEST_INSTALL_CRASH_HANDLER=${INSTALL_CRASH_HANDLER})
    target_link_libraries(${TARGET_NAME} PRIVATE Sora Catch2::Catch2)
    set_target_properties(${TARGET_NAME} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        FOLDER "Sora/Tests"
    )
endfunction()

add_sora_test_main(Sora.TestMain 1)
add_sora_test_main(Sora.TestMain.NoCrashHandler 0)

function(add_sora_test CATEGORY TEST_NAME)
    set(TEST_TARGET Test.Sora.${CATEGORY}.${TEST_NAME})
    set(TEST_MAIN Sora.TestMain)
    if(ARGN)
        if(NOT "${ARGN}" STREQUAL "MANAGES_CRASH_HANDLER")
            message(FATAL_ERROR "Unsupported add_sora_test option: ${ARGN}")
        endif()
        set(TEST_MAIN Sora.TestMain.NoCrashHandler)
    endif()

    add_executable(${TEST_TARGET} ${CATEGORY}/${TEST_NAME}.cpp)
    target_include_directories(${TEST_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${TEST_TARGET} PRIVATE Sora Catch2::Catch2 ${TEST_MAIN})
    set_target_properties(${TEST_TARGET} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        FOLDER "Sora/Tests/${CATEGORY}"
    )
    vk_tp_deploy_runtime(${TEST_TARGET})
    catch_discover_tests(${TEST_TARGET})
    set_property(GLOBAL APPEND PROPERTY SORA_TEST_TARGETS ${TEST_TARGET})
endfunction()

function(add_sora_standalone_test CATEGORY TEST_NAME)
    set(TEST_TARGET Test.Sora.${CATEGORY}.${TEST_NAME})

    add_executable(${TEST_TARGET} ${CATEGORY}/${TEST_NAME}.cpp)
    target_include_directories(${TEST_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${TEST_TARGET} PRIVATE Sora)
    set_target_properties(${TEST_TARGET} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        FOLDER "Sora/Tests/${CATEGORY}"
    )
    vk_tp_deploy_runtime(${TEST_TARGET})
    add_test(NAME ${TEST_TARGET} COMMAND ${TEST_TARGET})
    set_property(GLOBAL APPEND PROPERTY SORA_TEST_TARGETS ${TEST_TARGET})
endfunction()

function(add_sora_math_test TEST_NAME MATH_TARGET)
    set(TEST_TARGET Test.Sora.Math.${TEST_NAME})

    add_executable(${TEST_TARGET} Math/${TEST_NAME}.cpp)
    target_include_directories(${TEST_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${TEST_TARGET} PRIVATE ${MATH_TARGET} Catch2::Catch2WithMain)
    set_target_properties(${TEST_TARGET} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        FOLDER "Sora/Tests/Math"
    )
    vk_tp_deploy_runtime(${TEST_TARGET})
    catch_discover_tests(${TEST_TARGET})
    set_property(GLOBAL APPEND PROPERTY SORA_TEST_TARGETS ${TEST_TARGET})
endfunction()

function(add_sora_compute_test TEST_NAME COMPUTE_TARGET)
    set(TEST_TARGET Test.Sora.Compute.${TEST_NAME})

    add_executable(${TEST_TARGET} Compute/${TEST_NAME}.cpp)
    target_include_directories(${TEST_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${TEST_TARGET} PRIVATE ${COMPUTE_TARGET} Catch2::Catch2WithMain)
    set_target_properties(${TEST_TARGET} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        FOLDER "Sora/Tests/Compute"
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
add_sora_test(Core CpuProfilerTest)
add_sora_test(Core DiagnosticsTest MANAGES_CRASH_HANDLER)
add_sora_test(Core DirectBufferTest)
add_sora_standalone_test(Core CrashHandlerProcessTest)
add_sora_test(Core RefPtrTest)
add_sora_test(Core SOATest)
add_sora_test(Core StructuredLoggerTest)
add_sora_test(Core StringUtilsTest)
add_sora_test(Core StopwatchTest)
add_sora_test(Core UnicodeTest)
add_sora_test(Core ToStringTest)
add_sora_test(Core ToJsonTest)
add_sora_test(Core ToStyledStringTest)
add_sora_test(Core ConfigurationPathTest)
add_sora_test(Core EnvironmentTest)
add_sora_test(Core HiveTest)
add_sora_test(Core ChunkedSlotMapTest)
add_sora_test(Core MemoryLayoutTest)
add_sora_test(Core SequenceLockTest)
add_sora_test(Core WireTest)
add_sora_test(Core FunctionalTest)
add_sora_test(Core GuardTest)
add_sora_test(Core UniqueResourceTest)
add_sora_test(Core UriTest)
add_sora_test(Core PathTest)
add_sora_test(PAL ClipboardTest)
add_sora_test(PAL GlobalMemoryTest)
add_sora_test(PAL EnvironmentTest)
add_sora_test(PAL FileTest)
add_sora_test(PAL NativeCrashStreamTest)
add_sora_test(PAL NativeErrorTest)
add_sora_test(PAL ProcessTest)
add_sora_test(PAL SystemAPITest)
add_sora_test(PAL ThreadTest)
add_sora_compute_test(BackendExecutionTest Sora.Compute.Execution)
add_sora_compute_test(GraphTest Sora.Compute.Graph)
add_sora_compute_test(HazardAnalysisTest Sora.Compute.Graph)
add_sora_compute_test(GraphBuilderTest Sora.Compute.Graph)
add_sora_compute_test(KernelIRTest Sora.Math.Core)
add_sora_compute_test(KernelProgramTest Sora.Compute.Core)
add_sora_compute_test(KernelInvocationTest Sora.Compute.Core)
add_sora_compute_test(MemoryTest Sora.Compute.Runtime)
add_sora_compute_test(ResidencyPlannerTest Sora.Compute.Runtime)
add_sora_compute_test(RuntimeTest Sora.Compute.Runtime)
add_sora_math_test(EigenAdaptorTest Sora.Math.Eigen)
add_sora_math_test(ValueTypesTest Sora.Math.Core)
add_sora_math_test(KernelContextTest Sora.Math.Runtime)
add_sora_math_test(SimdTest Sora.Math.Core)
add_sora_test(Render HandleTest)
add_sora_standalone_test(Experimental CATCommandStandaloneTest)

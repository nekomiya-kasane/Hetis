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
    catch_discover_tests(${TEST_TARGET} PROPERTIES LABELS "Sora.${CATEGORY}")
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

function(add_sora_core_test TEST_NAME CORE_TARGET)
    set(TEST_TARGET Test.Sora.Core.${TEST_NAME})

    add_executable(${TEST_TARGET} Core/${TEST_NAME}.cpp)
    target_include_directories(${TEST_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${TEST_TARGET} PRIVATE ${CORE_TARGET} Catch2::Catch2WithMain)
    set_target_properties(${TEST_TARGET} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        FOLDER "Sora/Tests/Core"
    )
    vk_tp_deploy_runtime(${TEST_TARGET})
    catch_discover_tests(${TEST_TARGET} PROPERTIES LABELS "Sora.Core")
    set_property(GLOBAL APPEND PROPERTY SORA_TEST_TARGETS ${TEST_TARGET})
endfunction()

function(add_sora_pal_test TEST_NAME PAL_TARGET)
    set(TEST_TARGET Test.Sora.PAL.${TEST_NAME})

    add_executable(${TEST_TARGET} PAL/${TEST_NAME}.cpp)
    target_include_directories(${TEST_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${TEST_TARGET} PRIVATE ${PAL_TARGET} Catch2::Catch2WithMain)
    set_target_properties(${TEST_TARGET} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        FOLDER "Sora/Tests/PAL"
    )
    vk_tp_deploy_runtime(${TEST_TARGET})
    catch_discover_tests(${TEST_TARGET} PROPERTIES LABELS "Sora.PAL")
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
    catch_discover_tests(${TEST_TARGET} PROPERTIES LABELS "Sora.Math")
    set_property(GLOBAL APPEND PROPERTY SORA_TEST_TARGETS ${TEST_TARGET})
endfunction()

function(add_sora_compute_test TEST_NAME COMPUTE_TARGET)
    set(TEST_TARGET Test.Sora.Compute.${TEST_NAME})
    set(TEST_SOURCE ${TEST_NAME})
    if(ARGC GREATER 2)
        set(TEST_SOURCE ${ARGV2})
    endif()

    add_executable(${TEST_TARGET} Compute/${TEST_SOURCE}.cpp)
    target_include_directories(${TEST_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${TEST_TARGET} PRIVATE ${COMPUTE_TARGET} Catch2::Catch2WithMain)
    set_target_properties(${TEST_TARGET} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        FOLDER "Sora/Tests/Compute"
    )
    vk_tp_deploy_runtime(${TEST_TARGET})
    catch_discover_tests(${TEST_TARGET} PROPERTIES LABELS "Sora.Compute")
    set_property(GLOBAL APPEND PROPERTY SORA_TEST_TARGETS ${TEST_TARGET})
endfunction()

include(CTest)
include(Catch)

add_sora_test(Resources ResourcePackageTest)
add_sora_test(Resources SettingsTest)
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
add_sora_test(Core FalseSharingCheckerTest)
add_sora_core_test(AllocatorTest Sora.Core.Memory)
add_sora_test(Core MemoryLayoutTest)
add_sora_test(Core SequenceLockTest)
add_sora_test(Core ReclamationTest)
add_sora_test(Core GeneratorTest)
add_sora_test(Core SpscRingBufferTest)
add_sora_test(Core RecordRingBufferTest)
add_sora_test(Core BoundedStackTest)
add_sora_test(Core BoundedStackStressTest)
add_sora_test(Core MichaelScottQueueTest)
add_sora_test(Core ConcurrentMapTest)
add_sora_test(Core ConcurrentObjectPoolTest)
add_sora_test(Core ConcurrentSlabArenaTest)
add_sora_test(Core WireTest)
add_sora_test(Core FlagsTest)
add_sora_test(Core FunctionalTest)
add_sora_test(Core GuardTest)
add_sora_test(Core TypeTraitsTest)
add_sora_test(Core UniqueResourceTest)
add_sora_test(Core UriTest)
add_sora_test(Core PathTest)
add_sora_test(Async AsyncQueueTest)
add_sora_test(Async AsyncQueueCoroutineTest)
add_sora_test(Async AsyncQueueStressTest)
add_sora_test(PAL ClipboardTest)
add_sora_pal_test(CpuInfoTest Sora.PAL.Cpu)
add_sora_test(PAL GlobalMemoryTest)
add_sora_test(PAL EnvironmentTest)
add_sora_test(PAL FileTest)
add_sora_pal_test(InterprocessFileLockTest Sora.PAL.Storage)
add_executable(Test.Sora.PAL.InterprocessFileLockProcessTest PAL/InterprocessFileLockProcessTest.cpp)
target_link_libraries(Test.Sora.PAL.InterprocessFileLockProcessTest PRIVATE Sora.PAL.Storage)
set_target_properties(Test.Sora.PAL.InterprocessFileLockProcessTest PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Tests/PAL"
)
vk_tp_deploy_runtime(Test.Sora.PAL.InterprocessFileLockProcessTest)
add_test(NAME Test.Sora.PAL.InterprocessFileLockProcessTest COMMAND Test.Sora.PAL.InterprocessFileLockProcessTest)
set_tests_properties(Test.Sora.PAL.InterprocessFileLockProcessTest PROPERTIES LABELS "Sora.PAL")
set_property(GLOBAL APPEND PROPERTY SORA_TEST_TARGETS Test.Sora.PAL.InterprocessFileLockProcessTest)
add_sora_test(PAL NativeCrashStreamTest)
add_sora_test(PAL NativeErrorTest)
add_sora_pal_test(D3DAPITest Sora.PAL.Storage)
add_sora_pal_test(D3DNativeABITest Sora.PAL.Storage)
target_sources(Test.Sora.PAL.D3DNativeABITest PRIVATE PAL/D3DPALABIProbe.cpp)
add_sora_pal_test(OpenGLAPITest Sora.PAL.Storage)
add_sora_pal_test(OpenGLNativeABITest Sora.PAL.Storage)
add_sora_pal_test(VulkanAPITest Sora.PAL.Storage)
add_sora_pal_test(VulkanNativeABITest Sora.PAL.Storage)
target_include_directories(Test.Sora.PAL.VulkanNativeABITest PRIVATE
    "${CMAKE_SOURCE_DIR}/thirdparty/vulkan-headers/include"
)
add_sora_test(PAL ProcessTest)
add_sora_pal_test(SchedulerTraceTest Sora.PAL.Tracing)
target_include_directories(Test.Sora.PAL.SchedulerTraceTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
add_sora_pal_test(SystemAPITest Sora.PAL.Storage)
add_sora_pal_test(SocketSystemAPITest Sora.PAL.Storage)
add_sora_test(PAL SystemEventTest)
add_sora_test(PAL ThreadTest)
add_sora_compute_test(BackendExecutionTest Sora.Compute.Execution)
add_sora_compute_test(SyncWaitTest Sora.Compute.Execution)
add_sora_compute_test(BackendServiceTest Sora.Compute.Runtime)
add_sora_compute_test(AutomaticDispatchExpansionTest Sora.Compute.Runtime)
add_sora_compute_test(AutomaticDispatchPlanningTest Sora.Compute.Runtime)
add_sora_compute_test(RouteTransferSchedulingTest Sora.Compute.Runtime)
add_sora_compute_test(DispatchCandidateEnumerationTest Sora.Compute.Runtime)
add_sora_compute_test(GraphResidencyTest Sora.Compute.Runtime)
add_sora_compute_test(HostTransferBridgeTest Sora.Compute.Runtime)
target_link_libraries(Test.Sora.Compute.HostTransferBridgeTest PRIVATE Sora.Compute.Backend.Cpu)
add_sora_compute_test(HeftPlannerTest Sora.Compute.Runtime)
add_sora_compute_test(LinearChainPlannerTest Sora.Compute.Runtime)
add_sora_compute_test(PhysicalGraphPlannerTest Sora.Compute.Runtime)
add_sora_compute_test(PerformanceModelTest Sora.Compute.Runtime)
add_sora_compute_test(DurablePerformanceProfileTest Sora.Compute.Runtime)
add_sora_compute_test(DurablePerformanceProfileStoreTest Sora.Compute.Runtime)
add_sora_compute_test(PhysicalResourceCatalogTest Sora.Compute.Runtime)
add_sora_compute_test(LoweringServiceTest Sora.Compute.Runtime)
add_sora_compute_test(PhysicalGraphPlanTest Sora.Compute.Runtime)
add_sora_compute_test(PhysicalGraphMaterializerTest Sora.Compute.Runtime)
target_compile_definitions(Test.Sora.Compute.PhysicalGraphMaterializerTest PRIVATE SORA_PHYSICAL_GRAPH_TESTS_ONLY)
add_sora_compute_test(PreparedGraphTest Sora.Compute.Execution PhysicalGraphMaterializerTest)
target_compile_definitions(Test.Sora.Compute.PreparedGraphTest PRIVATE SORA_PREPARED_GRAPH_TESTS_ONLY)
add_sora_compute_test(PhysicalCommandCanonicalizationTest Sora.Compute.Runtime)
target_link_libraries(Test.Sora.Compute.PhysicalGraphMaterializerTest PRIVATE Sora.Compute.Execution)
add_sora_compute_test(BackendMemoryTransferTest Sora.Compute.Runtime)
add_sora_compute_test(ExecutableGraphTest Sora.Compute.Runtime)
add_sora_compute_test(CompletionTransactionTest Sora.Compute.Runtime)
add_sora_compute_test(GraphTest Sora.Compute.Graph)
add_sora_compute_test(HazardAnalysisTest Sora.Compute.Graph)
add_sora_compute_test(GraphBuilderTest Sora.Compute.Graph)
add_sora_compute_test(FusionOpportunityTest Sora.Compute.Graph)
add_sora_compute_test(AffineByteFootprintTest Sora.Compute.Core)
add_sora_compute_test(ByteCoverageTest Sora.Compute.Core)
add_sora_compute_test(BackendIdentityTest Sora.Compute.Core)
add_sora_compute_test(KernelIRTest Sora.Math.Core)
add_sora_compute_test(KernelProgramTest Sora.Compute.Core)
add_sora_compute_test(KernelProgramImageTest Sora.Compute.Runtime)
add_sora_compute_test(KernelFusionTest Sora.Compute.Compiler)
add_sora_compute_test(GraphOptimizerTest Sora.Compute.Runtime)
add_sora_compute_test(KernelInvocationTest Sora.Compute.Core)
add_sora_compute_test(DispatchPartitionTest Sora.Compute.Core)
add_sora_compute_test(MemoryTest Sora.Compute.Runtime)
add_sora_compute_test(PhysicalBindingTest Sora.Compute.Runtime)
add_sora_compute_test(PortablePackageRegistryTest Sora.Compute.Runtime)
add_sora_compute_test(PortablePackageArchiveTest Sora.Compute.Runtime)
add_sora_compute_test(PortablePackageTest Sora.Compute.Runtime)
add_sora_compute_test(ArtifactStoreTest Sora.Compute.Runtime)
add_sora_compute_test(ResidencyPlannerTest Sora.Compute.Runtime)
add_sora_compute_test(RuntimeTest Sora.Compute.Runtime)
add_sora_compute_test(RuntimeEpochTest Sora.Compute.Runtime)
add_sora_compute_test(RuntimeDiscoveryTest Sora.Compute.Runtime)
target_link_libraries(Test.Sora.Compute.RuntimeDiscoveryTest PRIVATE Sora.Compute.Backend.Cpu)
add_sora_compute_test(BackendPluginTest Sora.Compute.Runtime)
add_library(Sora.Compute.BackendPluginFixture MODULE Compute/BackendPluginFixture.cpp)
target_link_libraries(Sora.Compute.BackendPluginFixture PRIVATE Sora.Compute.Core)
set_target_properties(Sora.Compute.BackendPluginFixture PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    PREFIX ""
    FOLDER "Sora/Tests/Compute"
)
add_dependencies(Test.Sora.Compute.BackendPluginTest Sora.Compute.BackendPluginFixture)
target_compile_definitions(Test.Sora.Compute.BackendPluginTest PRIVATE
    SORA_BACKEND_PLUGIN_FIXTURE_PATH="$<TARGET_FILE:Sora.Compute.BackendPluginFixture>"
)
add_sora_compute_test(BackendPluginCatalogTest Sora.Compute.System)
add_dependencies(Test.Sora.Compute.BackendPluginCatalogTest Sora.Compute.BackendPluginFixture)
target_compile_definitions(Test.Sora.Compute.BackendPluginCatalogTest PRIVATE
    SORA_BACKEND_PLUGIN_FIXTURE_PATH="$<TARGET_FILE:Sora.Compute.BackendPluginFixture>"
)
add_sora_compute_test(SystemComputeContextTest Sora.Compute.System)
if(TARGET Sora.Compute.Backend.Vulkan)
    vk_tp_deploy_slang(Test.Sora.Compute.SystemComputeContextTest)
endif()
add_sora_compute_test(RuntimeInvalidationRegistryTest Sora.Compute.Runtime)
add_sora_compute_test(RuntimeRecoveryCoordinatorTest Sora.Compute.Runtime)
add_sora_compute_test(PipelineServiceTest Sora.Compute.Runtime)
add_sora_compute_test(CpuBackendTest Sora.Compute.Backend.Cpu)
add_sora_compute_test(LlvmCpuCodeProviderTest Sora.Compute.Backend.Cpu.Llvm)
add_sora_compute_test(LlvmCpuBackendIntegrationTest Sora.Compute.Backend.Cpu.Llvm)
add_sora_compute_test(LlvmKernelLoweringTest Sora.Compute.Backend.Cpu.Llvm)
target_include_directories(
    Test.Sora.Compute.LlvmKernelLoweringTest
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src/Compute/Backend"
)
if(TARGET Sora.Compute.Backend.Vulkan)
    add_sora_compute_test(VulkanBackendTest Sora.Compute.Backend.Vulkan)
    target_link_libraries(Test.Sora.Compute.VulkanBackendTest PRIVATE Sora.Compute.Execution)
    vk_tp_deploy_slang(Test.Sora.Compute.VulkanBackendTest)
endif()
if(TARGET Sora.Compute.Backend.UnifiedRuntime)
    add_sora_compute_test(OpenClSpirvLoweringTest Sora.Compute.Backend.UnifiedRuntime.Native)
    add_sora_compute_test(UnifiedRuntimePluginTest Sora.Compute.Runtime)
    target_link_libraries(Test.Sora.Compute.UnifiedRuntimePluginTest PRIVATE
        Sora.Compute.Backend.UnifiedRuntime.Native
    )
    if(WIN32)
        add_custom_command(TARGET Test.Sora.Compute.UnifiedRuntimePluginTest PRE_LINK
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SORA_UNIFIED_RUNTIME_LOADER}"
                "${SORA_UNIFIED_RUNTIME_LEVEL_ZERO_ADAPTER}"
                "$<TARGET_FILE_DIR:Test.Sora.Compute.UnifiedRuntimePluginTest>"
            VERBATIM
        )
    endif()
    add_dependencies(Test.Sora.Compute.UnifiedRuntimePluginTest Sora.Compute.Backend.UnifiedRuntime)
    target_compile_definitions(Test.Sora.Compute.UnifiedRuntimePluginTest PRIVATE
        SORA_UNIFIED_RUNTIME_PLUGIN_PATH="$<TARGET_FILE:Sora.Compute.Backend.UnifiedRuntime>"
    )
endif()
add_sora_math_test(EigenAdaptorTest Sora.Math.Eigen)
add_sora_math_test(ValueTypesTest Sora.Math.Core)
add_sora_math_test(SimdTest Sora.Math.Core)
add_sora_math_test(AffineTest Sora.Math.Core)
add_sora_math_test(GeomTest Sora.Math.Core)
add_sora_math_test(TensorBezierTest Sora.Math.Core)
add_sora_math_test(TensorAlgebraTest Sora.Math.Core)
add_sora_math_test(NurbsTest Sora.Math.Core)
add_sora_math_test(ComputeGraphTest Sora.Math.Compute)
add_sora_standalone_test(Surface ColorSpaceSyntax)
add_sora_test(Surface ColorSpaceTest)
if(TARGET Sora.Compute.Backend.UnifiedRuntime)
    add_sora_math_test(UnifiedRuntimeComputeGraphTest Sora.Math.Compute)
    if(WIN32)
        add_custom_command(TARGET Test.Sora.Math.UnifiedRuntimeComputeGraphTest PRE_LINK
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SORA_UNIFIED_RUNTIME_LOADER}"
                "${SORA_UNIFIED_RUNTIME_LEVEL_ZERO_ADAPTER}"
                "$<TARGET_FILE_DIR:Test.Sora.Math.UnifiedRuntimeComputeGraphTest>"
            VERBATIM
        )
    endif()
    add_dependencies(Test.Sora.Math.UnifiedRuntimeComputeGraphTest Sora.Compute.Backend.UnifiedRuntime)
    target_compile_definitions(Test.Sora.Math.UnifiedRuntimeComputeGraphTest PRIVATE
        SORA_UNIFIED_RUNTIME_PLUGIN_PATH="$<TARGET_FILE:Sora.Compute.Backend.UnifiedRuntime>"
    )
endif()
if(TARGET Sora.Math.Kokkos)
    add_sora_math_test(KokkosInteropTest Sora.Math.Kokkos)
endif()
add_sora_test(Render HandleTest)
add_sora_test(Renderer DeviceFeatureTest)
add_sora_standalone_test(Experimental CATCommandStandaloneTest)

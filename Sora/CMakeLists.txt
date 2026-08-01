cmake_minimum_required(VERSION 4.0)

project(Sora LANGUAGES C CXX)

set(SORA_PAL_CPU_SOURCES
    src/PAL/CpuInfo.cpp
)
set(SORA_PAL_STORAGE_SOURCES
    src/PAL/AtomicFile.cpp
    src/PAL/Environment.cpp
    src/PAL/File.cpp
    src/PAL/InterprocessFileLock.cpp
    src/PAL/Module.cpp
    src/PAL/NativeError.cpp
    src/PAL/Process.cpp
    src/PAL/SystemAPI.cpp
)
set(SORA_PAL_TRACING_SOURCES
    src/PAL/SchedulerTrace.cpp
    src/PAL/SchedulerTraceDecoder.cpp
)

file(GLOB_RECURSE Sora_SOURCES CONFIGURE_DEPENDS
    "src/*.cpp"
    "src/*.h"
)
list(FILTER Sora_SOURCES EXCLUDE REGEX "/src/(Math|Compute)/")
set(SORA_COMPONENT_SOURCES ${SORA_PAL_CPU_SOURCES} ${SORA_PAL_STORAGE_SOURCES} ${SORA_PAL_TRACING_SOURCES})
list(TRANSFORM SORA_COMPONENT_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
list(REMOVE_ITEM Sora_SOURCES ${SORA_COMPONENT_SOURCES})
message(STATUS "${Sora_SOURCES}")

add_library(Sora.PAL.Cpu STATIC ${SORA_PAL_CPU_SOURCES})
add_library(Sora::PAL::Cpu ALIAS Sora.PAL.Cpu)
target_include_directories(Sora.PAL.Cpu PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
set_target_properties(Sora.PAL.Cpu PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    FOLDER "Sora/PAL"
)

add_library(Sora.PAL.Storage STATIC ${SORA_PAL_STORAGE_SOURCES})
add_library(Sora::PAL::Storage ALIAS Sora.PAL.Storage)
target_include_directories(Sora.PAL.Storage PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
set_target_properties(Sora.PAL.Storage PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    FOLDER "Sora/PAL"
)

add_library(Sora.PAL.Tracing STATIC ${SORA_PAL_TRACING_SOURCES})
add_library(Sora::PAL::Tracing ALIAS Sora.PAL.Tracing)
target_include_directories(Sora.PAL.Tracing PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(Sora.PAL.Tracing PUBLIC Sora.PAL.Storage)
set_target_properties(Sora.PAL.Tracing PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    FOLDER "Sora/PAL"
)

add_library(Sora.Compute.Core INTERFACE)
add_library(Sora::Compute::Core ALIAS Sora.Compute.Core)
target_include_directories(Sora.Compute.Core INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_compile_features(Sora.Compute.Core INTERFACE cxx_std_26)
set_target_properties(Sora.Compute.Core PROPERTIES FOLDER "Sora/Compute")

add_library(Sora.Compute.Graph STATIC
    src/Compute/Graph/FusionOpportunity.cpp
    src/Compute/Graph/HazardAnalysis.cpp
    src/Compute/Graph/GraphBuilder.cpp
)
add_library(Sora::Compute::Graph ALIAS Sora.Compute.Graph)
target_link_libraries(Sora.Compute.Graph PUBLIC Sora.Compute.Core)
target_include_directories(Sora.Compute.Graph PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
set_target_properties(Sora.Compute.Graph PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    FOLDER "Sora/Compute"
)

add_library(Sora.Compute.Compiler STATIC
    src/Compute/Compiler/KernelFusion.cpp
    src/Compute/Compiler/KernelProgramImage.cpp
)
add_library(Sora::Compute::Compiler ALIAS Sora.Compute.Compiler)
target_link_libraries(Sora.Compute.Compiler PUBLIC Sora.Compute.Core)
target_include_directories(Sora.Compute.Compiler PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
set_target_properties(Sora.Compute.Compiler PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    FOLDER "Sora/Compute"
)

add_library(Sora.Compute.Runtime STATIC
    src/Compute/AutomaticDispatchExpansion.cpp
    src/Compute/AutomaticDispatchPlanning.cpp
    src/Compute/RouteTransferScheduling.cpp
    src/Compute/ArtifactCache.cpp
    src/Compute/CalibratedTransferTopology.cpp
    src/Compute/CandidateSynthesis.cpp
    src/Compute/DispatchCandidateEnumeration.cpp
    src/Compute/DispatchGraphPlanning.cpp
    src/Compute/GraphResidency.cpp
    src/Compute/GraphOptimizer.cpp
    src/Compute/GraphPortfolio.cpp
    src/Compute/HeftPlanner.cpp
    src/Compute/HeftRanking.cpp
    src/Compute/HostTransferBridge.cpp
    src/Compute/LinearChainPlanner.cpp
    src/Compute/CompilerService.cpp
    src/Compute/PipelineService.cpp
    src/Compute/ArtifactStore.cpp
    src/Compute/PhysicalGraphMaterializer.cpp
    src/Compute/PhysicalResourceCatalog.cpp
    src/Compute/PhysicalGraphPlan.cpp
    src/Compute/PhysicalGraphPlanner.cpp
    src/Compute/PerformanceModel.cpp
    src/Compute/ProgramPackage.cpp
    src/Compute/ProgramCatalog.cpp
    src/Compute/PortablePackage.cpp
    src/Compute/PortablePackageArchive.cpp
    src/Compute/PortablePackageRegistry.cpp
    src/Compute/RuntimeDiscovery.cpp
    src/Compute/BackendPlugin.cpp
    src/Compute/BackendPluginCatalog.cpp
    src/Compute/RuntimeEpoch.cpp
    src/Compute/RuntimeInvalidationRegistry.cpp
    src/Compute/RuntimeRecoveryCoordinator.cpp
)
add_library(Sora::Compute::Runtime ALIAS Sora.Compute.Runtime)
target_link_libraries(Sora.Compute.Runtime PUBLIC Sora.Compute.Graph Sora.Compute.Compiler)
target_link_libraries(Sora.Compute.Runtime PRIVATE Sora.PAL.Storage)
set_target_properties(Sora.Compute.Runtime PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    FOLDER "Sora/Compute"
)

add_library(Sora.Compute.Execution INTERFACE)
add_library(Sora::Compute::Execution ALIAS Sora.Compute.Execution)
target_link_libraries(Sora.Compute.Execution INTERFACE Sora.Compute.Runtime stdexec)
target_compile_features(Sora.Compute.Execution INTERFACE cxx_std_26)
set_target_properties(Sora.Compute.Execution PROPERTIES FOLDER "Sora/Compute")

add_library(Sora.Compute.Backend.Cpu STATIC
    src/Compute/Backend/CpuBackend.cpp
)
add_library(Sora::Compute::Backend::Cpu ALIAS Sora.Compute.Backend.Cpu)
target_link_libraries(Sora.Compute.Backend.Cpu PUBLIC Sora.Compute.Core PRIVATE Sora.PAL.Cpu stdexec)
set_target_properties(Sora.Compute.Backend.Cpu PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    FOLDER "Sora/Compute/Backend"
)

add_library(Sora.Compute.Backend.Cpu.Llvm STATIC
    src/Compute/Backend/LlvmCpuCodeProvider.cpp
    src/Compute/Backend/LlvmKernelLowering.cpp
)
add_library(Sora::Compute::Backend::Cpu::Llvm ALIAS Sora.Compute.Backend.Cpu.Llvm)
target_link_libraries(Sora.Compute.Backend.Cpu.Llvm
    PUBLIC Sora.Compute.Backend.Cpu
    PRIVATE Sora.PAL.Storage Sora.PAL.Cpu
)
set_target_properties(Sora.Compute.Backend.Cpu.Llvm PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    FOLDER "Sora/Compute/Backend"
)

if(TARGET slang AND TARGET volk)
    add_library(Sora.Compute.Backend.Vulkan STATIC
        src/Compute/Backend/SlangKernelLowering.cpp
        src/Compute/Backend/VulkanBackend.cpp
        src/Compute/Backend/VulkanExecution.cpp
    )
    add_library(Sora::Compute::Backend::Vulkan ALIAS Sora.Compute.Backend.Vulkan)
    target_link_libraries(Sora.Compute.Backend.Vulkan
        PUBLIC Sora.Compute.Runtime
        PRIVATE slang volk vma
    )
    set_target_properties(Sora.Compute.Backend.Vulkan PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        POSITION_INDEPENDENT_CODE ON
        FOLDER "Sora/Compute/Backend"
    )
endif()

add_library(Sora.Compute.System STATIC
    src/Compute/SystemArtifactStore.cpp
    src/Compute/SystemComputeContext.cpp
)
add_library(Sora::Compute::System ALIAS Sora.Compute.System)
target_link_libraries(Sora.Compute.System
    PUBLIC Sora.Compute.Execution Sora.Compute.Backend.Cpu.Llvm
    PRIVATE Sora.PAL.Storage
)
if(TARGET Sora.Compute.Backend.Vulkan)
    target_link_libraries(Sora.Compute.System PUBLIC Sora.Compute.Backend.Vulkan)
    target_compile_definitions(Sora.Compute.System PRIVATE SORA_COMPUTE_SYSTEM_HAS_VULKAN=1)
else()
    target_compile_definitions(Sora.Compute.System PRIVATE SORA_COMPUTE_SYSTEM_HAS_VULKAN=0)
endif()
set_target_properties(Sora.Compute.System PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    FOLDER "Sora/Compute"
)

add_library(Sora.Math.Core INTERFACE)
add_library(Sora::Math::Core ALIAS Sora.Math.Core)
target_include_directories(Sora.Math.Core INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_compile_features(Sora.Math.Core INTERFACE cxx_std_26)
target_link_libraries(Sora.Math.Core INTERFACE Sora.Compute.Core)
set_target_properties(Sora.Math.Core PROPERTIES FOLDER "Sora/Math")

add_library(Sora.Math.Eigen INTERFACE)
add_library(Sora::Math::Eigen ALIAS Sora.Math.Eigen)
target_link_libraries(Sora.Math.Eigen INTERFACE Sora.Math.Core eigen)
set_target_properties(Sora.Math.Eigen PROPERTIES FOLDER "Sora/Math/Interop")

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND DEFINED COCA_TOOLCHAIN_ROOT)
    set(SORA_INTEL_RUNTIME_DIR "${COCA_TOOLCHAIN_ROOT}/tools/ifort/lib")
    set(SORA_SVML_LIBRARY "${SORA_INTEL_RUNTIME_DIR}/svml_dispmt.lib")
    set(SORA_IRC_LIBRARY "${SORA_INTEL_RUNTIME_DIR}/libirc.lib")
    set(SORA_SVML_RUNTIME "${COCA_TOOLCHAIN_ROOT}/tools/ifort/redist/svml_dispmd.dll")
    if(EXISTS "${SORA_SVML_LIBRARY}" AND EXISTS "${SORA_IRC_LIBRARY}")
        target_compile_options(
            Sora.Math.Core INTERFACE
            "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CONFIG:Debug>>>:-fveclib=SVML>"
        )
        target_link_directories(Sora.Math.Core INTERFACE "${SORA_INTEL_RUNTIME_DIR}")
        target_link_libraries(Sora.Math.Core INTERFACE "${SORA_SVML_LIBRARY}" "${SORA_IRC_LIBRARY}")
        if(EXISTS "${SORA_SVML_RUNTIME}")
            set_property(TARGET Sora.Math.Core APPEND PROPERTY TRANSITIVE_LINK_PROPERTIES VK_TP_RUNTIME_DLLS)
            set_property(
                TARGET Sora.Math.Core PROPERTY INTERFACE_VK_TP_RUNTIME_DLLS "${SORA_SVML_RUNTIME}"
            )
        endif()
        message(STATUS "Sora SIMD transcendental backend: Intel SVML")
    endif()
endif()

add_library(Sora STATIC)
target_sources(Sora PRIVATE ${Sora_SOURCES})
target_include_directories(Sora PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(Sora PUBLIC Sora.Compute.System Sora.Math.Core tapioca)
target_link_libraries(Sora PUBLIC simdjson::simdjson stdexec)

set_target_properties(Sora PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    FOLDER "Sora"

    COMPILE_PDB_NAME "Sora.Comp"
)

set_property(GLOBAL PROPERTY SORA_DEMO_TARGETS "")
set_property(GLOBAL PROPERTY SORA_TEST_TARGETS "")

add_subdirectory(demos)
add_subdirectory(tests)
add_subdirectory(benchmarks)

get_property(SORA_DEMO_TARGETS GLOBAL PROPERTY SORA_DEMO_TARGETS)
get_property(SORA_TEST_TARGETS GLOBAL PROPERTY SORA_TEST_TARGETS)

add_custom_target(Sora.BuildAll)
add_dependencies(Sora.BuildAll
    Sora
    ${SORA_DEMO_TARGETS}
    ${SORA_TEST_TARGETS}
)
set_target_properties(Sora.BuildAll PROPERTIES FOLDER "Sora")

find_package(Python3 3.12 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_target(Sora.CodeIndex.Check
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/code_index.py" check
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        VERBATIM
    )
    set_target_properties(Sora.CodeIndex.Check PROPERTIES FOLDER "Sora/Tools")
endif()

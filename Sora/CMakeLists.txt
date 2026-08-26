cmake_minimum_required(VERSION 4.0)

project(Sora LANGUAGES C CXX)

set(SORA_PAL_CPU_SOURCES
    src/PAL/CpuInfo.cpp
)
set(SORA_PAL_STORAGE_SOURCES
    src/PAL/AtomicFile.cpp
    src/PAL/D3DAPI.cpp
    src/PAL/Environment.cpp
    src/PAL/File.cpp
    src/PAL/InterprocessFileLock.cpp
    src/PAL/Module.cpp
    src/PAL/NativeError.cpp
    src/PAL/OpenGLAPI.cpp
    src/PAL/VulkanAPI.cpp
    src/PAL/Process.cpp
    src/PAL/SystemAPI.cpp
)
set(SORA_PAL_TRACING_SOURCES
    src/PAL/SchedulerTrace.cpp
    src/PAL/SchedulerTraceDecoder.cpp
)
set(SORA_CORE_MEMORY_SOURCES
    src/Core/Memory/Allocator.cpp
)

file(GLOB_RECURSE Sora_SOURCES CONFIGURE_DEPENDS
    "src/*.cpp"
    "src/*.h"
)
list(FILTER Sora_SOURCES EXCLUDE REGEX "/src/(Math|Compute)/")
set(SORA_COMPONENT_SOURCES
    ${SORA_CORE_MEMORY_SOURCES}
    ${SORA_PAL_CPU_SOURCES}
    ${SORA_PAL_STORAGE_SOURCES}
    ${SORA_PAL_TRACING_SOURCES}
)
list(TRANSFORM SORA_COMPONENT_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
list(REMOVE_ITEM Sora_SOURCES ${SORA_COMPONENT_SOURCES})
message(STATUS "${Sora_SOURCES}")

add_library(Sora.Core.Memory STATIC ${SORA_CORE_MEMORY_SOURCES})
add_library(Sora::Core::Memory ALIAS Sora.Core.Memory)
target_include_directories(Sora.Core.Memory PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(Sora.Core.Memory PRIVATE mimalloc::mimalloc)
set_target_properties(Sora.Core.Memory PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    FOLDER "Sora/Core"
)

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
target_link_libraries(Sora.Compute.Core INTERFACE Sora.Core.Memory)
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
    src/Compute/DurablePerformanceProfile.cpp
    src/Compute/DurablePerformanceProfileStore.cpp
    src/Compute/GraphResidency.cpp
    src/Compute/GraphOptimizer.cpp
    src/Compute/GraphPortfolio.cpp
    src/Compute/HeftPlanner.cpp
    src/Compute/HeftRanking.cpp
    src/Compute/HostTransferBridge.cpp
    src/Compute/LinearChainPlanner.cpp
    src/Compute/CompilerService.cpp
    src/Compute/LoweringService.cpp
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
target_link_libraries(Sora.Compute.Backend.Cpu PUBLIC Sora.Compute.Core PRIVATE Sora.PAL.Cpu TBB::tbb)
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
        src/Compute/Backend/VulkanPortableModule.cpp
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

set(SORA_UNIFIED_RUNTIME_ROOT "" CACHE PATH "Exact Unified Runtime SDK root used to build the optional backend plugin")
if(SORA_UNIFIED_RUNTIME_ROOT)
    set(SORA_UNIFIED_RUNTIME_INCLUDE "${SORA_UNIFIED_RUNTIME_ROOT}/include")
    if(WIN32)
        set(SORA_UNIFIED_RUNTIME_LIBRARY "${SORA_UNIFIED_RUNTIME_ROOT}/lib/ur_loader.lib")
    else()
        find_library(SORA_UNIFIED_RUNTIME_LIBRARY
            NAMES ur_loader
            PATHS "${SORA_UNIFIED_RUNTIME_ROOT}/lib" "${SORA_UNIFIED_RUNTIME_ROOT}/lib64"
            NO_DEFAULT_PATH
        )
    endif()
    if(NOT EXISTS "${SORA_UNIFIED_RUNTIME_INCLUDE}/ur_api.h" OR
       NOT EXISTS "${SORA_UNIFIED_RUNTIME_LIBRARY}")
        message(FATAL_ERROR "SORA_UNIFIED_RUNTIME_ROOT does not contain the Unified Runtime headers and loader")
    endif()

    add_library(Sora.Compute.Backend.UnifiedRuntime.Native OBJECT
        src/Compute/Backend/OpenClPortableModule.cpp
        src/Compute/Backend/OpenClSpirvLowering.cpp
        src/Compute/Backend/UnifiedRuntimeNative.cpp
    )
    target_link_libraries(Sora.Compute.Backend.UnifiedRuntime.Native
        PUBLIC Sora.Compute.Core "${SORA_UNIFIED_RUNTIME_LIBRARY}"
    )
    target_include_directories(Sora.Compute.Backend.UnifiedRuntime.Native
        PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/Compute/Backend>"
        PRIVATE "${SORA_UNIFIED_RUNTIME_INCLUDE}"
    )
    set_target_properties(Sora.Compute.Backend.UnifiedRuntime.Native PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        POSITION_INDEPENDENT_CODE ON
        FOLDER "Sora/Compute/Backend"
    )

    add_library(Sora.Compute.Backend.UnifiedRuntime MODULE src/Compute/Backend/UnifiedRuntimePlugin.cpp)
    target_link_libraries(Sora.Compute.Backend.UnifiedRuntime
        PRIVATE Sora.Compute.Backend.UnifiedRuntime.Native
    )
    set_target_properties(Sora.Compute.Backend.UnifiedRuntime PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        PREFIX ""
        FOLDER "Sora/Compute/Backend"
    )
    if(WIN32)
        set(SORA_UNIFIED_RUNTIME_LOADER "${SORA_UNIFIED_RUNTIME_ROOT}/bin/ur_loader.dll")
        set(SORA_UNIFIED_RUNTIME_LEVEL_ZERO_ADAPTER
            "${SORA_UNIFIED_RUNTIME_ROOT}/bin/ur_adapter_level_zero.dll")
        if(NOT EXISTS "${SORA_UNIFIED_RUNTIME_LOADER}" OR
           NOT EXISTS "${SORA_UNIFIED_RUNTIME_LEVEL_ZERO_ADAPTER}")
            message(FATAL_ERROR "SORA_UNIFIED_RUNTIME_ROOT does not contain the Windows Level Zero runtime")
        endif()
        add_custom_command(TARGET Sora.Compute.Backend.UnifiedRuntime POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SORA_UNIFIED_RUNTIME_LOADER}"
                "${SORA_UNIFIED_RUNTIME_LEVEL_ZERO_ADAPTER}"
                "$<TARGET_FILE_DIR:Sora.Compute.Backend.UnifiedRuntime>"
            VERBATIM
        )
    else()
        set_target_properties(Sora.Compute.Backend.UnifiedRuntime PROPERTIES
            BUILD_RPATH "${SORA_UNIFIED_RUNTIME_ROOT}/lib;${SORA_UNIFIED_RUNTIME_ROOT}/lib64"
        )
    endif()
endif()

add_library(Sora.Compute.System STATIC
    src/Compute/SystemCachePath.cpp
    src/Compute/SystemArtifactStore.cpp
    src/Compute/SystemComputeContext.cpp
    src/Compute/SystemPerformanceProfile.cpp
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

add_library(Sora.Math.Compute INTERFACE)
add_library(Sora::Math::Compute ALIAS Sora.Math.Compute)
target_link_libraries(Sora.Math.Compute INTERFACE Sora.Math.Core Sora.Compute.System)
target_compile_features(Sora.Math.Compute INTERFACE cxx_std_26)
set_target_properties(Sora.Math.Compute PROPERTIES FOLDER "Sora/Math")

add_library(Sora.Math.Eigen INTERFACE)
add_library(Sora::Math::Eigen ALIAS Sora.Math.Eigen)
target_link_libraries(Sora.Math.Eigen INTERFACE Sora.Math.Core eigen)
set_target_properties(Sora.Math.Eigen PROPERTIES FOLDER "Sora/Math/Interop")

option(SORA_MATH_ENABLE_KOKKOS_INTEROP "Enable Kokkos interop when Kokkos 5.2 or newer is available" ON)
if(SORA_MATH_ENABLE_KOKKOS_INTEROP)
    find_package(Kokkos 5.2 CONFIG QUIET)
    if(Kokkos_FOUND)
        get_target_property(_sora_kokkos_configs Kokkos::kokkoscore IMPORTED_CONFIGURATIONS)
        set(_sora_kokkos_binary_identity)
        get_target_property(_sora_kokkos_core Kokkos::kokkoscore IMPORTED_LOCATION)
        if(EXISTS "${_sora_kokkos_core}")
            file(SHA256 "${_sora_kokkos_core}" _sora_kokkos_core_hash)
            string(APPEND _sora_kokkos_binary_identity "GENERIC:${_sora_kokkos_core_hash};")
        endif()
        foreach(_sora_kokkos_config IN LISTS _sora_kokkos_configs)
            string(TOUPPER "${_sora_kokkos_config}" _sora_kokkos_config_upper)
            get_target_property(_sora_kokkos_core Kokkos::kokkoscore
                "IMPORTED_LOCATION_${_sora_kokkos_config_upper}"
            )
            if(EXISTS "${_sora_kokkos_core}")
                file(SHA256 "${_sora_kokkos_core}" _sora_kokkos_core_hash)
                string(APPEND _sora_kokkos_binary_identity
                    "${_sora_kokkos_config_upper}:${_sora_kokkos_core_hash};"
                )
            endif()
        endforeach()
        if(EXISTS "${CMAKE_TOOLCHAIN_FILE}")
            file(SHA256 "${CMAKE_TOOLCHAIN_FILE}" _sora_toolchain_file_hash)
        endif()
        if(EXISTS "${CMAKE_CXX_COMPILER}")
            file(SIZE "${CMAKE_CXX_COMPILER}" _sora_cxx_compiler_size)
            file(TIMESTAMP "${CMAKE_CXX_COMPILER}" _sora_cxx_compiler_timestamp UTC)
        endif()
        set(_sora_kokkos_probe_configuration "${CMAKE_TRY_COMPILE_CONFIGURATION}")
        if(NOT _sora_kokkos_probe_configuration)
            set(_sora_kokkos_probe_configuration "${CMAKE_BUILD_TYPE}")
        endif()
        string(TOUPPER "${_sora_kokkos_probe_configuration}" _sora_kokkos_probe_configuration_upper)
        set(_sora_kokkos_probe_link_flags
            "${CMAKE_EXE_LINKER_FLAGS_${_sora_kokkos_probe_configuration_upper}}"
        )
        set(_sora_kokkos_probe_cmake_arguments)
        if(_sora_kokkos_probe_link_flags)
            string(CONCAT _sora_kokkos_probe_linker_definition
                "-DCMAKE_EXE_LINKER_FLAGS_" "${_sora_kokkos_probe_configuration_upper}:STRING="
                "${_sora_kokkos_probe_link_flags}"
            )
            list(APPEND _sora_kokkos_probe_cmake_arguments CMAKE_FLAGS
                "${_sora_kokkos_probe_linker_definition}"
            )
        endif()
        string(CONCAT _sora_kokkos_abi_identity
            "probe-v6|${Kokkos_DIR}|${Kokkos_VERSION}|${_sora_kokkos_binary_identity}|${CMAKE_CXX_COMPILER}|"
            "${_sora_cxx_compiler_size}|${_sora_cxx_compiler_timestamp}|${CMAKE_CXX_COMPILER_ID}|"
            "${CMAKE_CXX_COMPILER_VERSION}|"
            "${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}|${CMAKE_TOOLCHAIN_FILE}|${_sora_toolchain_file_hash}|"
            "${CMAKE_GENERATOR}|${CMAKE_GENERATOR_PLATFORM}|${CMAKE_GENERATOR_TOOLSET}|${CMAKE_SYSTEM_NAME}|"
            "${CMAKE_SYSTEM_PROCESSOR}|${CMAKE_SIZEOF_VOID_P}|${CMAKE_MSVC_RUNTIME_LIBRARY}|${COCA_TARGET_PROFILE}|"
            "${CMAKE_BUILD_TYPE}|${CMAKE_CXX_FLAGS}|"
            "${CMAKE_CXX_FLAGS_DEBUG}|${CMAKE_CXX_FLAGS_RELEASE}|${CMAKE_CXX_FLAGS_RELWITHDEBINFO}|"
            "${CMAKE_CXX_FLAGS_MINSIZEREL}|"
            "${CMAKE_EXE_LINKER_FLAGS}|${CMAKE_EXE_LINKER_FLAGS_DEBUG}|${CMAKE_EXE_LINKER_FLAGS_RELEASE}|"
            "${CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO}|${CMAKE_EXE_LINKER_FLAGS_MINSIZEREL}"
        )
        string(SHA256 _sora_kokkos_abi_key "${_sora_kokkos_abi_identity}")
        set(_sora_kokkos_abi_result "SORA_MATH_KOKKOS_ABI_${_sora_kokkos_abi_key}")
        if(NOT DEFINED ${_sora_kokkos_abi_result})
            try_compile(${_sora_kokkos_abi_result}
                SOURCES_TYPE NORMAL
                SOURCE_FROM_CONTENT sora_math_kokkos_abi_probe.cpp [[
                    #include <Kokkos_Core.hpp>
                    #include <impl/Kokkos_Error.hpp>
                    int main(int argc, char** argv) {
                        Kokkos::initialize(argc, argv);
                        Kokkos::finalize();
                        Kokkos::Impl::throw_runtime_exception("Sora Kokkos ABI probe");
                    }
                ]]
                ${_sora_kokkos_probe_cmake_arguments}
                LINK_LIBRARIES Kokkos::kokkos
                CXX_STANDARD 26
                CXX_STANDARD_REQUIRED ON
                OUTPUT_VARIABLE SORA_MATH_KOKKOS_ABI_PROBE_OUTPUT
            )
            mark_as_advanced(${_sora_kokkos_abi_result})
        endif()
        set(SORA_MATH_KOKKOS_ABI_COMPATIBLE "${${_sora_kokkos_abi_result}}")
        if(SORA_MATH_KOKKOS_ABI_COMPATIBLE)
            add_library(Sora.Math.Kokkos INTERFACE)
            add_library(Sora::Math::Kokkos ALIAS Sora.Math.Kokkos)
            target_link_libraries(Sora.Math.Kokkos INTERFACE Sora.Math.Core Kokkos::kokkos)
            set_target_properties(Sora.Math.Kokkos PROPERTIES FOLDER "Sora/Math/Interop")
            message(STATUS "[Sora/Math] Kokkos ${Kokkos_VERSION} interop enabled")
        else()
            message(WARNING
                "[Sora/Math] Kokkos ${Kokkos_VERSION} was found but is not link-compatible with the active "
                "compiler, C++ standard library, and sanitizer ABI; interop target disabled"
            )
            message(VERBOSE "${SORA_MATH_KOKKOS_ABI_PROBE_OUTPUT}")
        endif()
    else()
        message(STATUS "[Sora/Math] Kokkos 5.2+ not found; interop target disabled")
    endif()
endif()

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
target_link_libraries(Sora PUBLIC Sora.Core.Memory Sora.Math.Compute tapioca)
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

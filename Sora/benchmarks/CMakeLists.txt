add_executable(Benchmark.Sora.Core.SOA EXCLUDE_FROM_ALL Core/SOABenchmark.cpp)
target_include_directories(Benchmark.Sora.Core.SOA PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Benchmark.Sora.Core.SOA PRIVATE Sora.Math.Core)
set_target_properties(Benchmark.Sora.Core.SOA PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Core"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Sora.Core.SOA PRIVATE -O3 -march=haswell -fno-math-errno -ffp-contract=off)
endif()

vk_tp_deploy_runtime(Benchmark.Sora.Core.SOA)

add_executable(Benchmark.Sora.Math.Value EXCLUDE_FROM_ALL Math/ValueBenchmark.cpp)
target_include_directories(Benchmark.Sora.Math.Value PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Benchmark.Sora.Math.Value PRIVATE Sora.Math.Core Sora.Math.Eigen)
set_target_properties(Benchmark.Sora.Math.Value PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Math"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(
        Benchmark.Sora.Math.Value PRIVATE -O3 -march=haswell -fno-math-errno -ffp-contract=off
    )
endif()

vk_tp_deploy_runtime(Benchmark.Sora.Math.Value)

add_executable(Benchmark.Sora.Compute.Graph EXCLUDE_FROM_ALL Compute/GraphBenchmark.cpp)
target_include_directories(Benchmark.Sora.Compute.Graph PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Benchmark.Sora.Compute.Graph PRIVATE Sora.Compute.Graph)
set_target_properties(Benchmark.Sora.Compute.Graph PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Compute"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Sora.Compute.Graph PRIVATE -O3 -march=haswell)
endif()

vk_tp_deploy_runtime(Benchmark.Sora.Compute.Graph)

add_executable(Benchmark.Sora.Compute.Assembly EXCLUDE_FROM_ALL Compute/AssemblyBenchmark.cpp)
target_include_directories(Benchmark.Sora.Compute.Assembly PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Benchmark.Sora.Compute.Assembly PRIVATE Sora.Compute.Runtime)
set_target_properties(Benchmark.Sora.Compute.Assembly PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Compute"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Sora.Compute.Assembly PRIVATE -O3 -march=haswell)
endif()

vk_tp_deploy_runtime(Benchmark.Sora.Compute.Assembly)

add_executable(Benchmark.Sora.Compute.Recovery EXCLUDE_FROM_ALL Compute/RecoveryBenchmark.cpp)
target_include_directories(Benchmark.Sora.Compute.Recovery PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Benchmark.Sora.Compute.Recovery PRIVATE Sora.Compute.Runtime)
set_target_properties(Benchmark.Sora.Compute.Recovery PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Compute"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Sora.Compute.Recovery PRIVATE -O3 -march=haswell)
endif()

vk_tp_deploy_runtime(Benchmark.Sora.Compute.Recovery)

add_executable(Benchmark.Sora.Compute.RuntimeDiscovery EXCLUDE_FROM_ALL Compute/RuntimeDiscoveryBenchmark.cpp)
target_include_directories(Benchmark.Sora.Compute.RuntimeDiscovery PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Benchmark.Sora.Compute.RuntimeDiscovery PRIVATE Sora.Compute.Backend.Cpu Sora.Compute.Runtime)
add_dependencies(Benchmark.Sora.Compute.RuntimeDiscovery Sora.Compute.BackendPluginFixture)
target_compile_definitions(Benchmark.Sora.Compute.RuntimeDiscovery PRIVATE
    SORA_BACKEND_PLUGIN_FIXTURE_PATH="$<TARGET_FILE:Sora.Compute.BackendPluginFixture>"
)
set_target_properties(Benchmark.Sora.Compute.RuntimeDiscovery PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Compute"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Sora.Compute.RuntimeDiscovery PRIVATE -O3 -march=haswell)
endif()

vk_tp_deploy_runtime(Benchmark.Sora.Compute.RuntimeDiscovery)

add_executable(Benchmark.Sora.Compute.SystemRuntime EXCLUDE_FROM_ALL Compute/SystemRuntimeBenchmark.cpp)
target_include_directories(Benchmark.Sora.Compute.SystemRuntime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Benchmark.Sora.Compute.SystemRuntime PRIVATE Sora.Compute.System)
set_target_properties(Benchmark.Sora.Compute.SystemRuntime PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Compute"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Sora.Compute.SystemRuntime PRIVATE -O3 -march=haswell)
endif()

vk_tp_deploy_runtime(Benchmark.Sora.Compute.SystemRuntime)
if(TARGET Sora.Compute.Backend.Vulkan)
    vk_tp_deploy_slang(Benchmark.Sora.Compute.SystemRuntime)
endif()

add_executable(Benchmark.Sora.Compute.PerformanceModel EXCLUDE_FROM_ALL Compute/PerformanceModelBenchmark.cpp)
target_include_directories(Benchmark.Sora.Compute.PerformanceModel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Benchmark.Sora.Compute.PerformanceModel PRIVATE Sora.Compute.Runtime)
set_target_properties(Benchmark.Sora.Compute.PerformanceModel PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Compute"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Sora.Compute.PerformanceModel PRIVATE -O3 -march=haswell)
endif()

vk_tp_deploy_runtime(Benchmark.Sora.Compute.PerformanceModel)

add_executable(Benchmark.Sora.Compute.DispatchPartition EXCLUDE_FROM_ALL Compute/DispatchPartitionBenchmark.cpp)
target_include_directories(Benchmark.Sora.Compute.DispatchPartition PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Benchmark.Sora.Compute.DispatchPartition PRIVATE Sora.Compute.Runtime)
set_target_properties(Benchmark.Sora.Compute.DispatchPartition PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Compute"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Sora.Compute.DispatchPartition PRIVATE -O3 -march=haswell)
endif()

vk_tp_deploy_runtime(Benchmark.Sora.Compute.DispatchPartition)

add_executable(Benchmark.Sora.Compute.AutomaticPartitionPlanning EXCLUDE_FROM_ALL
    Compute/AutomaticPartitionPlanningBenchmark.cpp
)
target_include_directories(Benchmark.Sora.Compute.AutomaticPartitionPlanning PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Benchmark.Sora.Compute.AutomaticPartitionPlanning PRIVATE Sora.Compute.Runtime)
set_target_properties(Benchmark.Sora.Compute.AutomaticPartitionPlanning PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Compute"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Sora.Compute.AutomaticPartitionPlanning PRIVATE -O3 -march=haswell)
endif()

vk_tp_deploy_runtime(Benchmark.Sora.Compute.AutomaticPartitionPlanning)

add_executable(Benchmark.Sora.Compute.DispatchGraphLowering EXCLUDE_FROM_ALL
    Compute/DispatchGraphLoweringBenchmark.cpp
)
target_include_directories(Benchmark.Sora.Compute.DispatchGraphLowering PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Benchmark.Sora.Compute.DispatchGraphLowering PRIVATE Sora.Compute.Runtime)
set_target_properties(Benchmark.Sora.Compute.DispatchGraphLowering PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Compute"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Sora.Compute.DispatchGraphLowering PRIVATE -O3 -march=haswell)
endif()

vk_tp_deploy_runtime(Benchmark.Sora.Compute.DispatchGraphLowering)

add_executable(Benchmark.Sora.Compute.AffineByteFootprint EXCLUDE_FROM_ALL
    Compute/AffineByteFootprintBenchmark.cpp
)
target_include_directories(Benchmark.Sora.Compute.AffineByteFootprint PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Benchmark.Sora.Compute.AffineByteFootprint PRIVATE Sora.Compute.Core)
set_target_properties(Benchmark.Sora.Compute.AffineByteFootprint PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Compute"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Sora.Compute.AffineByteFootprint PRIVATE -O3 -march=haswell)
endif()

vk_tp_deploy_runtime(Benchmark.Sora.Compute.AffineByteFootprint)

add_executable(Benchmark.Sora.Compute.PortablePackage EXCLUDE_FROM_ALL Compute/PortablePackageBenchmark.cpp)
target_include_directories(Benchmark.Sora.Compute.PortablePackage PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Benchmark.Sora.Compute.PortablePackage PRIVATE Sora.Compute.Runtime)
set_target_properties(Benchmark.Sora.Compute.PortablePackage PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Compute"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Sora.Compute.PortablePackage PRIVATE -O3 -march=haswell)
endif()

vk_tp_deploy_runtime(Benchmark.Sora.Compute.PortablePackage)

add_executable(Benchmark.Sora.Compute.ArtifactStore EXCLUDE_FROM_ALL Compute/ArtifactStoreBenchmark.cpp)
target_include_directories(Benchmark.Sora.Compute.ArtifactStore PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Benchmark.Sora.Compute.ArtifactStore PRIVATE Sora.Compute.Runtime)
set_target_properties(Benchmark.Sora.Compute.ArtifactStore PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Compute"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Sora.Compute.ArtifactStore PRIVATE -O3 -march=haswell)
endif()

vk_tp_deploy_runtime(Benchmark.Sora.Compute.ArtifactStore)

add_executable(Benchmark.Sora.Compute.PlanningInputs EXCLUDE_FROM_ALL Compute/PlanningInputBenchmark.cpp)
target_include_directories(Benchmark.Sora.Compute.PlanningInputs PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Benchmark.Sora.Compute.PlanningInputs PRIVATE Sora.Compute.Runtime)
set_target_properties(Benchmark.Sora.Compute.PlanningInputs PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Compute"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Sora.Compute.PlanningInputs PRIVATE -O3 -march=haswell)
endif()

vk_tp_deploy_runtime(Benchmark.Sora.Compute.PlanningInputs)

add_executable(Benchmark.Sora.Compute.GraphOptimizer EXCLUDE_FROM_ALL Compute/GraphOptimizerBenchmark.cpp)
target_include_directories(Benchmark.Sora.Compute.GraphOptimizer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Benchmark.Sora.Compute.GraphOptimizer PRIVATE Sora.Compute.Runtime)
set_target_properties(Benchmark.Sora.Compute.GraphOptimizer PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Compute"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Sora.Compute.GraphOptimizer PRIVATE -O3 -march=haswell)
endif()

vk_tp_deploy_runtime(Benchmark.Sora.Compute.GraphOptimizer)

add_executable(Benchmark.Sora.Compute.GraphResidency EXCLUDE_FROM_ALL Compute/GraphResidencyBenchmark.cpp)
target_include_directories(Benchmark.Sora.Compute.GraphResidency PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Benchmark.Sora.Compute.GraphResidency PRIVATE Sora.Compute.Runtime)
set_target_properties(Benchmark.Sora.Compute.GraphResidency PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Compute"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Sora.Compute.GraphResidency PRIVATE -O3 -march=haswell)
endif()

vk_tp_deploy_runtime(Benchmark.Sora.Compute.GraphResidency)

add_executable(Benchmark.Sora.Compute.PhysicalGraphMemoryReservation EXCLUDE_FROM_ALL
    Compute/PhysicalGraphMemoryReservationBenchmark.cpp
)
target_include_directories(Benchmark.Sora.Compute.PhysicalGraphMemoryReservation PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Benchmark.Sora.Compute.PhysicalGraphMemoryReservation PRIVATE Sora.Compute.Runtime)
set_target_properties(Benchmark.Sora.Compute.PhysicalGraphMemoryReservation PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Compute"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Sora.Compute.PhysicalGraphMemoryReservation PRIVATE -O3 -march=haswell)
endif()

vk_tp_deploy_runtime(Benchmark.Sora.Compute.PhysicalGraphMemoryReservation)

add_executable(Benchmark.Sora.Compute.LinearChainPlanning EXCLUDE_FROM_ALL
    Compute/LinearChainPlanningBenchmark.cpp
)
target_include_directories(Benchmark.Sora.Compute.LinearChainPlanning PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Benchmark.Sora.Compute.LinearChainPlanning PRIVATE Sora.Compute.Runtime)
set_target_properties(Benchmark.Sora.Compute.LinearChainPlanning PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Compute"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Sora.Compute.LinearChainPlanning PRIVATE -O3 -march=haswell)
endif()

vk_tp_deploy_runtime(Benchmark.Sora.Compute.LinearChainPlanning)

add_executable(Benchmark.Sora.Compute.HeftPlanning EXCLUDE_FROM_ALL Compute/HeftPlanningBenchmark.cpp)
target_include_directories(Benchmark.Sora.Compute.HeftPlanning PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Benchmark.Sora.Compute.HeftPlanning PRIVATE Sora.Compute.Runtime)
set_target_properties(Benchmark.Sora.Compute.HeftPlanning PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Compute"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Sora.Compute.HeftPlanning PRIVATE -O3 -march=haswell)
endif()

vk_tp_deploy_runtime(Benchmark.Sora.Compute.HeftPlanning)

add_executable(Benchmark.Sora.Compute.HostTransferBridge EXCLUDE_FROM_ALL Compute/HostTransferBridgeBenchmark.cpp)
target_include_directories(Benchmark.Sora.Compute.HostTransferBridge PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Benchmark.Sora.Compute.HostTransferBridge
    PRIVATE Sora.Compute.Runtime Sora.Compute.Backend.Cpu
)
set_target_properties(Benchmark.Sora.Compute.HostTransferBridge PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Compute"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Sora.Compute.HostTransferBridge PRIVATE -O3 -march=haswell)
endif()

vk_tp_deploy_runtime(Benchmark.Sora.Compute.HostTransferBridge)

add_executable(Benchmark.Sora.Compute.CpuBackend EXCLUDE_FROM_ALL Compute/CpuBackendBenchmark.cpp)
target_include_directories(Benchmark.Sora.Compute.CpuBackend PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(Benchmark.Sora.Compute.CpuBackend PRIVATE Sora.Compute.Backend.Cpu.Llvm)
set_target_properties(Benchmark.Sora.Compute.CpuBackend PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Compute"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(
        Benchmark.Sora.Compute.CpuBackend PRIVATE -O3 -march=haswell -fno-math-errno
    )
endif()

vk_tp_deploy_runtime(Benchmark.Sora.Compute.CpuBackend)

if(TARGET Sora.Compute.Backend.Vulkan)
    add_executable(Benchmark.Sora.Compute.VulkanBackend EXCLUDE_FROM_ALL Compute/VulkanBackendBenchmark.cpp)
    target_include_directories(Benchmark.Sora.Compute.VulkanBackend PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(Benchmark.Sora.Compute.VulkanBackend PRIVATE Sora.Compute.Backend.Vulkan)
    set_target_properties(Benchmark.Sora.Compute.VulkanBackend PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        FOLDER "Sora/Benchmarks/Compute"
    )

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(Benchmark.Sora.Compute.VulkanBackend PRIVATE -O3)
    endif()

    vk_tp_deploy_runtime(Benchmark.Sora.Compute.VulkanBackend)
    vk_tp_deploy_slang(Benchmark.Sora.Compute.VulkanBackend)
endif()

add_executable(Benchmark.Sora.Core.Reclamation EXCLUDE_FROM_ALL Core/ReclamationBenchmark.cpp)
target_include_directories(Benchmark.Sora.Core.Reclamation PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
)
target_link_libraries(Benchmark.Sora.Core.Reclamation PRIVATE Sora)
set_target_properties(Benchmark.Sora.Core.Reclamation PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Core"
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(Benchmark.Sora.Core.Reclamation PRIVATE -O3 -march=haswell)
endif()

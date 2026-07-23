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
target_link_libraries(Benchmark.Sora.Math.Value PRIVATE Sora.Math.Runtime Sora.Math.Eigen)
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

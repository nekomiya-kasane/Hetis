add_executable(Benchmark.Sora.Core.SOA EXCLUDE_FROM_ALL Core/SOABenchmark.cpp)
target_link_libraries(Benchmark.Sora.Core.SOA PRIVATE Sora)
set_target_properties(Benchmark.Sora.Core.SOA PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Core"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Sora.Core.SOA PRIVATE -O3 -march=haswell -fno-math-errno -ffp-contract=off)
endif()

vk_tp_deploy_runtime(Benchmark.Sora.Core.SOA)

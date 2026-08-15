add_executable(Benchmark.Mashiro.Queues EXCLUDE_FROM_ALL QueueBenchmark.cpp)
target_link_libraries(Benchmark.Mashiro.Queues PRIVATE Mashiro TBB::tbb)
target_include_directories(Benchmark.Mashiro.Queues PRIVATE
    "${CMAKE_SOURCE_DIR}/thirdparty/concurrentqueue"
    "${CMAKE_SOURCE_DIR}/thirdparty/rigtorp-mpmcqueue/include"
    "${CMAKE_SOURCE_DIR}/thirdparty/rigtorp-spscqueue/include"
)
target_compile_definitions(Benchmark.Mashiro.Queues PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(Benchmark.Mashiro.Queues PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Mashiro/Benchmarks"
)
vk_tp_deploy_runtime(Benchmark.Mashiro.Queues)

add_library(Benchmark.Mashiro.BoundedStackMigrationAssembly OBJECT EXCLUDE_FROM_ALL
    BoundedStackMigrationAssembly.cpp
)
target_include_directories(Benchmark.Mashiro.BoundedStackMigrationAssembly PRIVATE
    "${CMAKE_SOURCE_DIR}/Mashiro/include"
)
set_target_properties(Benchmark.Mashiro.BoundedStackMigrationAssembly PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Mashiro/Benchmarks"
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Mashiro.BoundedStackMigrationAssembly PRIVATE -O3)
endif()

add_executable(Benchmark.Mashiro.AsyncQueue EXCLUDE_FROM_ALL AsyncQueueBenchmark.cpp)
target_link_libraries(Benchmark.Mashiro.AsyncQueue PRIVATE Mashiro TBB::tbb)
target_include_directories(Benchmark.Mashiro.AsyncQueue PRIVATE "${CMAKE_SOURCE_DIR}/thirdparty/concurrentqueue")
target_compile_definitions(Benchmark.Mashiro.AsyncQueue PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(Benchmark.Mashiro.AsyncQueue PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Mashiro/Benchmarks"
)
vk_tp_deploy_runtime(Benchmark.Mashiro.AsyncQueue)

add_executable(Benchmark.Mashiro.ConcurrentMaps EXCLUDE_FROM_ALL ConcurrentMapBenchmark.cpp)
target_link_libraries(Benchmark.Mashiro.ConcurrentMaps PRIVATE Mashiro TBB::tbb)
set_target_properties(Benchmark.Mashiro.ConcurrentMaps PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Mashiro/Benchmarks"
)
vk_tp_deploy_runtime(Benchmark.Mashiro.ConcurrentMaps)

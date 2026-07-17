add_executable(Benchmark.Mashiro.Queues EXCLUDE_FROM_ALL QueueBenchmark.cpp)
target_link_libraries(Benchmark.Mashiro.Queues PRIVATE Mashiro TBB::tbb)
target_include_directories(Benchmark.Mashiro.Queues PRIVATE
    "${CMAKE_SOURCE_DIR}/thirdparty/concurrentqueue"
    "${CMAKE_SOURCE_DIR}/thirdparty/rigtorp-spscqueue/include"
)
target_compile_definitions(Benchmark.Mashiro.Queues PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(Benchmark.Mashiro.Queues PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Mashiro/Benchmarks"
)
vk_tp_deploy_runtime(Benchmark.Mashiro.Queues)

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

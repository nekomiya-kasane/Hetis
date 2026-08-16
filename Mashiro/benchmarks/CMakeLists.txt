add_executable(Benchmark.Mashiro.Queues EXCLUDE_FROM_ALL QueueBenchmark.cpp)
target_link_libraries(Benchmark.Mashiro.Queues PRIVATE Mashiro TBB::tbb)
target_include_directories(Benchmark.Mashiro.Queues PRIVATE
    "${CMAKE_SOURCE_DIR}/Sora/include"
    "${CMAKE_SOURCE_DIR}/thirdparty/concurrentqueue"
    "${CMAKE_SOURCE_DIR}/thirdparty/rigtorp-mpmcqueue/include"
    "${CMAKE_SOURCE_DIR}/thirdparty/rigtorp-spscqueue/include"
)
target_compile_definitions(Benchmark.Mashiro.Queues PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    __TBB_NO_IMPLICIT_LINKAGE=1
)
set_target_properties(Benchmark.Mashiro.Queues PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Mashiro/Benchmarks"
)
vk_tp_deploy_runtime(Benchmark.Mashiro.Queues)

add_library(Benchmark.Mashiro.RingMigrationAssembly OBJECT EXCLUDE_FROM_ALL RingBufferMigrationAssembly.cpp)
target_include_directories(Benchmark.Mashiro.RingMigrationAssembly PRIVATE
    "${CMAKE_SOURCE_DIR}/Mashiro/include"
    "${CMAKE_SOURCE_DIR}/Sora/include"
)
set_target_properties(Benchmark.Mashiro.RingMigrationAssembly PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Mashiro/Benchmarks"
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Mashiro.RingMigrationAssembly PRIVATE -O3)
endif()

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

add_library(Benchmark.Sora.Migration.BoundedStackAssembly OBJECT EXCLUDE_FROM_ALL
    BoundedStackMigrationAssembly.cpp
)
target_include_directories(Benchmark.Sora.Migration.BoundedStackAssembly PRIVATE
    "${CMAKE_SOURCE_DIR}/Sora/include"
)
target_compile_definitions(Benchmark.Sora.Migration.BoundedStackAssembly PRIVATE SORA_MIGRATION_TARGET=1)
set_target_properties(Benchmark.Sora.Migration.BoundedStackAssembly PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Migration"
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Sora.Migration.BoundedStackAssembly PRIVATE -O3)
endif()

add_library(Benchmark.Mashiro.ConcurrentMapMigrationAssembly OBJECT EXCLUDE_FROM_ALL
    ConcurrentMapMigrationAssembly.cpp
)
target_include_directories(Benchmark.Mashiro.ConcurrentMapMigrationAssembly PRIVATE
    "${CMAKE_SOURCE_DIR}/Mashiro/include"
)
set_target_properties(Benchmark.Mashiro.ConcurrentMapMigrationAssembly PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Mashiro/Benchmarks"
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Mashiro.ConcurrentMapMigrationAssembly PRIVATE -O3)
endif()

add_library(Benchmark.Sora.Migration.ConcurrentMapAssembly OBJECT EXCLUDE_FROM_ALL
    ConcurrentMapMigrationAssembly.cpp
)
target_include_directories(Benchmark.Sora.Migration.ConcurrentMapAssembly PRIVATE
    "${CMAKE_SOURCE_DIR}/Sora/include"
)
target_compile_definitions(Benchmark.Sora.Migration.ConcurrentMapAssembly PRIVATE SORA_MIGRATION_TARGET=1)
set_target_properties(Benchmark.Sora.Migration.ConcurrentMapAssembly PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Migration"
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Sora.Migration.ConcurrentMapAssembly PRIVATE -O3)
endif()

add_executable(Benchmark.Sora.Migration.ConcurrentMapPaired EXCLUDE_FROM_ALL
    ConcurrentMapMigrationBenchmark.cpp
)
target_link_libraries(Benchmark.Sora.Migration.ConcurrentMapPaired PRIVATE Mashiro Sora)
target_compile_definitions(Benchmark.Sora.Migration.ConcurrentMapPaired PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(Benchmark.Sora.Migration.ConcurrentMapPaired PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Migration"
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Sora.Migration.ConcurrentMapPaired PRIVATE -O3)
endif()
vk_tp_deploy_runtime(Benchmark.Sora.Migration.ConcurrentMapPaired)

add_library(Benchmark.Mashiro.RecordRingMigrationAssembly OBJECT EXCLUDE_FROM_ALL
    RecordRingMigrationAssembly.cpp
)
target_include_directories(Benchmark.Mashiro.RecordRingMigrationAssembly PRIVATE
    "${CMAKE_SOURCE_DIR}/Mashiro/include"
    "${CMAKE_SOURCE_DIR}/Sora/include"
)
set_target_properties(Benchmark.Mashiro.RecordRingMigrationAssembly PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Mashiro/Benchmarks"
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Mashiro.RecordRingMigrationAssembly PRIVATE -O3)
endif()

add_executable(Benchmark.Mashiro.RecordRings EXCLUDE_FROM_ALL RecordRingBufferBenchmark.cpp)
target_link_libraries(Benchmark.Mashiro.RecordRings PRIVATE Mashiro)
target_include_directories(Benchmark.Mashiro.RecordRings PRIVATE
    "${CMAKE_SOURCE_DIR}/Sora/include"
    "${CMAKE_SOURCE_DIR}/thirdparty/rigtorp-mpmcqueue/include"
    "${CMAKE_SOURCE_DIR}/thirdparty/rigtorp-spscqueue/include"
)
target_compile_definitions(Benchmark.Mashiro.RecordRings PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(Benchmark.Mashiro.RecordRings PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Mashiro/Benchmarks"
)
vk_tp_deploy_runtime(Benchmark.Mashiro.RecordRings)

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

add_executable(Benchmark.Sora.Migration.AsyncQueue EXCLUDE_FROM_ALL AsyncQueueBenchmark.cpp)
target_link_libraries(Benchmark.Sora.Migration.AsyncQueue PRIVATE Sora TBB::tbb)
target_include_directories(Benchmark.Sora.Migration.AsyncQueue PRIVATE "${CMAKE_SOURCE_DIR}/thirdparty/concurrentqueue")
target_compile_definitions(Benchmark.Sora.Migration.AsyncQueue PRIVATE
    NOMINMAX
    SORA_MIGRATION_TARGET=1
    WIN32_LEAN_AND_MEAN
)
set_target_properties(Benchmark.Sora.Migration.AsyncQueue PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Migration"
)
vk_tp_deploy_runtime(Benchmark.Sora.Migration.AsyncQueue)

add_executable(Benchmark.Sora.Migration.AsyncQueuePaired EXCLUDE_FROM_ALL AsyncQueueMigrationBenchmark.cpp)
target_link_libraries(Benchmark.Sora.Migration.AsyncQueuePaired PRIVATE Mashiro Sora)
target_compile_definitions(Benchmark.Sora.Migration.AsyncQueuePaired PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(Benchmark.Sora.Migration.AsyncQueuePaired PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Migration"
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Sora.Migration.AsyncQueuePaired PRIVATE -O3)
endif()
vk_tp_deploy_runtime(Benchmark.Sora.Migration.AsyncQueuePaired)

add_library(Benchmark.Mashiro.AsyncQueueMigrationAssembly OBJECT EXCLUDE_FROM_ALL
    AsyncQueueMigrationAssembly.cpp
)
target_link_libraries(Benchmark.Mashiro.AsyncQueueMigrationAssembly PRIVATE Mashiro)
target_include_directories(Benchmark.Mashiro.AsyncQueueMigrationAssembly PRIVATE
    "${CMAKE_SOURCE_DIR}/Mashiro/include"
)
set_target_properties(Benchmark.Mashiro.AsyncQueueMigrationAssembly PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Mashiro/Benchmarks"
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Mashiro.AsyncQueueMigrationAssembly PRIVATE -O3)
endif()

add_library(Benchmark.Sora.Migration.AsyncQueueAssembly OBJECT EXCLUDE_FROM_ALL
    AsyncQueueMigrationAssembly.cpp
)
target_link_libraries(Benchmark.Sora.Migration.AsyncQueueAssembly PRIVATE Sora)
target_include_directories(Benchmark.Sora.Migration.AsyncQueueAssembly PRIVATE
    "${CMAKE_SOURCE_DIR}/Sora/include"
)
target_compile_definitions(Benchmark.Sora.Migration.AsyncQueueAssembly PRIVATE SORA_MIGRATION_TARGET=1)
set_target_properties(Benchmark.Sora.Migration.AsyncQueueAssembly PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Migration"
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(Benchmark.Sora.Migration.AsyncQueueAssembly PRIVATE -O3)
endif()

add_executable(Benchmark.Mashiro.ConcurrentMaps EXCLUDE_FROM_ALL ConcurrentMapBenchmark.cpp)
target_link_libraries(Benchmark.Mashiro.ConcurrentMaps PRIVATE Mashiro TBB::tbb)
set_target_properties(Benchmark.Mashiro.ConcurrentMaps PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Mashiro/Benchmarks"
)
vk_tp_deploy_runtime(Benchmark.Mashiro.ConcurrentMaps)

add_executable(Benchmark.Sora.Migration.ConcurrentMaps EXCLUDE_FROM_ALL ConcurrentMapBenchmark.cpp)
target_link_libraries(Benchmark.Sora.Migration.ConcurrentMaps PRIVATE Sora TBB::tbb)
target_compile_definitions(Benchmark.Sora.Migration.ConcurrentMaps PRIVATE SORA_MIGRATION_TARGET=1)
set_target_properties(Benchmark.Sora.Migration.ConcurrentMaps PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Migration"
)
vk_tp_deploy_runtime(Benchmark.Sora.Migration.ConcurrentMaps)

add_executable(Benchmark.Mashiro.MichaelScott EXCLUDE_FROM_ALL MichaelScottQueueBenchmark.cpp)
target_link_libraries(Benchmark.Mashiro.MichaelScott PRIVATE Mashiro)
target_include_directories(Benchmark.Mashiro.MichaelScott PRIVATE
    "${CMAKE_SOURCE_DIR}/thirdparty/rigtorp-mpmcqueue/include"
)
target_compile_definitions(Benchmark.Mashiro.MichaelScott PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(Benchmark.Mashiro.MichaelScott PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Mashiro/Benchmarks"
)
vk_tp_deploy_runtime(Benchmark.Mashiro.MichaelScott)

add_executable(Benchmark.Sora.Migration.MichaelScott EXCLUDE_FROM_ALL MichaelScottQueueBenchmark.cpp)
target_link_libraries(Benchmark.Sora.Migration.MichaelScott PRIVATE Sora)
target_include_directories(Benchmark.Sora.Migration.MichaelScott PRIVATE
    "${CMAKE_SOURCE_DIR}/thirdparty/rigtorp-mpmcqueue/include"
)
target_compile_definitions(Benchmark.Sora.Migration.MichaelScott PRIVATE
    NOMINMAX
    SORA_MIGRATION_TARGET=1
    WIN32_LEAN_AND_MEAN
)
set_target_properties(Benchmark.Sora.Migration.MichaelScott PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    FOLDER "Sora/Benchmarks/Migration"
)
vk_tp_deploy_runtime(Benchmark.Sora.Migration.MichaelScott)

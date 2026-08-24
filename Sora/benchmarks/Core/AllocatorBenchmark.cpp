/**
 * @file AllocatorBenchmark.cpp
 * @brief Compare explicit mimalloc-backed Sora allocation with the default C++ allocation domain.
 */
#include <Sora/Core/Memory/Allocator.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <print>
#include <string_view>

namespace {

    using Clock = std::chrono::steady_clock;
    using Allocate = void* (*)(std::size_t) noexcept;
    using Deallocate = void (*)(void*) noexcept;

    constexpr std::size_t kSampleCount = 31;
    constexpr std::size_t kWarmupOperations = 10'000;
    constexpr std::array kAllocationSizes{
        std::size_t{16}, std::size_t{64}, std::size_t{256}, std::size_t{1'024},
        std::size_t{4'096}, std::size_t{16'384}, std::size_t{65'536}, std::size_t{1'048'576},
    };
    constexpr std::array kOperationCounts{
        std::size_t{1'000}, std::size_t{10'000}, std::size_t{100'000}, std::size_t{1'000'000},
    };

    struct AllocationDomain {
        std::string_view name;
        Allocate allocate;
        Deallocate deallocate;
    };

    struct Measurement {
        std::uint64_t elapsedNanoseconds;
        std::uint64_t checksum;
    };

    [[gnu::noinline]] void* AllocateDefault(std::size_t bytes) noexcept {
        return ::operator new(bytes, std::nothrow);
    }

    [[gnu::noinline]] void DeallocateDefault(void* allocation) noexcept {
        ::operator delete(allocation);
    }

    [[gnu::noinline]] void* AllocateMimalloc(std::size_t bytes) noexcept {
        return Sora::AllocateMemory(bytes);
    }

    [[gnu::noinline]] void DeallocateMimalloc(void* allocation) noexcept {
        Sora::DeallocateMemory(allocation);
    }

    [[nodiscard]] Measurement Measure(const AllocationDomain& domain, std::size_t bytes,
                                      std::size_t operations) noexcept {
        std::uintptr_t checksum = 0;
        std::atomic_signal_fence(std::memory_order_seq_cst);
        const auto begin = Clock::now();
        for (std::size_t operation = 0; operation < operations; ++operation) {
            void* allocation = domain.allocate(bytes);
            if (!allocation) [[unlikely]] {
                std::abort();
            }
            checksum ^= reinterpret_cast<std::uintptr_t>(allocation) + operation;
            domain.deallocate(allocation);
        }
        const auto end = Clock::now();
        std::atomic_signal_fence(std::memory_order_seq_cst);
        return {
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count()),
            static_cast<std::uint64_t>(checksum),
        };
    }

    void WarmUp(const AllocationDomain& domain, std::size_t bytes, std::size_t operations,
                std::uint64_t& checksum) noexcept {
        const std::size_t warmupOperations = std::min(operations, kWarmupOperations);
        for (std::size_t warmup = 0; warmup < 3; ++warmup) {
            checksum ^= Measure(domain, bytes, warmupOperations).checksum;
        }
    }

    void WriteSample(std::FILE* output, const AllocationDomain& domain, std::size_t bytes,
                     std::size_t operations, std::size_t sample, Measurement measurement) {
        const double nanosecondsPerOperation =
            static_cast<double>(measurement.elapsedNanoseconds) / static_cast<double>(operations);
        const double operationsPerSecond = 1.0e9 / nanosecondsPerOperation;
        std::println(output, "1,pair,{},{},{},{},{},{:.6f},{:.3f},{}", domain.name, bytes, operations, sample,
                     measurement.elapsedNanoseconds, nanosecondsPerOperation, operationsPerSecond,
                     measurement.checksum);
    }

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::println(stderr, "usage: {} <raw-samples.csv>", argv[0]);
        return EXIT_FAILURE;
    }

    std::FILE* output = nullptr;
    if (fopen_s(&output, argv[1], "wb") != 0) {
        output = nullptr;
    }
    if (!output) {
        std::println(stderr, "failed to open benchmark output: {}", argv[1]);
        return EXIT_FAILURE;
    }
    std::println(output,
                 "schema_version,workload,allocator,bytes,operations,sample,elapsed_ns,ns_per_operation,"
                 "operations_per_second,checksum");

    constexpr std::array domains{
        AllocationDomain{"default_new_delete", AllocateDefault, DeallocateDefault},
        AllocationDomain{"mimalloc", AllocateMimalloc, DeallocateMimalloc},
    };
    std::uint64_t checksum = 0;
    for (std::size_t operationCount : kOperationCounts) {
        for (std::size_t allocationSize : kAllocationSizes) {
            for (const AllocationDomain& domain : domains) {
                WarmUp(domain, allocationSize, operationCount, checksum);
            }
            std::array<std::array<Measurement, kSampleCount>, domains.size()> measurements{};
            for (std::size_t sample = 0; sample < kSampleCount; ++sample) {
                const bool reverseOrder = sample % 2 != 0;
                for (std::size_t position = 0; position < domains.size(); ++position) {
                    const std::size_t domainIndex = reverseOrder ? domains.size() - position - 1 : position;
                    const Measurement measurement = Measure(domains[domainIndex], allocationSize, operationCount);
                    checksum ^= measurement.checksum;
                    measurements[domainIndex][sample] = measurement;
                }
            }
            for (std::size_t sample = 0; sample < kSampleCount; ++sample) {
                for (std::size_t domainIndex = 0; domainIndex < domains.size(); ++domainIndex) {
                    WriteSample(output, domains[domainIndex], allocationSize, operationCount, sample,
                                measurements[domainIndex][sample]);
                }
            }
            std::println("bytes {:8}, operations {:8}: {} samples per allocator", allocationSize, operationCount,
                         kSampleCount);
        }
    }

    if (std::fflush(output) != 0 || std::ferror(output)) {
        std::println(stderr, "failed while writing benchmark output: {}", argv[1]);
        std::fclose(output);
        return EXIT_FAILURE;
    }
    std::fclose(output);
    std::println("checksum {}", checksum);
    return EXIT_SUCCESS;
}

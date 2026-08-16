/**
 * @file ConcurrentMapMigrationBenchmark.cpp
 * @brief Same-process paired evidence for the Mashiro-to-Sora ConcurrentMap migration.
 * @ingroup Testing
 */

#include <Mashiro/Core/ConcurrentMap.h>
#include <Sora/Core/Concurrency/ConcurrentMap.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <latch>
#include <memory>
#include <print>
#include <random>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(_WIN32)
#    include <windows.h>
#endif

namespace {

    using Clock = std::chrono::steady_clock;

    constexpr std::size_t kLookupKeys = 1U << 17;
    constexpr std::size_t kFixedCapacity = 1U << 20;

    enum class Workload : std::uint8_t {
        LookupHit,
        LookupMiss,
        Insert,
    };

    struct Config {
        std::size_t samples = 25;
        std::size_t warmups = 2;
        double minimumSampleSeconds = 0.12;
        bool pinWorkers = true;
    };

    struct ThroughputSample {
        double seconds{};
        std::uint64_t operations{};

        [[nodiscard]] double OperationsPerSecond() const noexcept { return static_cast<double>(operations) / seconds; }
    };

    struct PairSample {
        double mashiroOperationsPerSecond{};
        double soraOperationsPerSecond{};
        double ratio{};
    };

    [[nodiscard]] std::vector<std::uint32_t> AvailableCpuSets() {
#if defined(_WIN32)
        ULONG byteCount = 0;
        if (GetSystemCpuSetInformation(nullptr, 0, &byteCount, GetCurrentProcess(), 0) != FALSE ||
            GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            return {};
        }
        std::vector<std::byte> buffer(byteCount);
        if (GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()), byteCount,
                                       &byteCount, GetCurrentProcess(), 0) == FALSE) {
            return {};
        }

        struct CpuSet {
            std::uint32_t id{};
            std::uint16_t group{};
            std::uint8_t logicalProcessor{};
            std::uint8_t core{};
            std::uint8_t efficiency{};
            std::uint8_t sibling{};
        };
        std::vector<CpuSet> cpuSets;
        for (std::size_t offset = 0; offset < byteCount;) {
            const auto* information = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
            if (information->Size == 0 || offset + information->Size > byteCount) {
                return {};
            }
            if (information->Type == CpuSetInformation && information->CpuSet.Parked == 0 &&
                (information->CpuSet.Allocated == 0 || information->CpuSet.AllocatedToTargetProcess != 0)) {
                cpuSets.push_back({
                    .id = information->CpuSet.Id,
                    .group = information->CpuSet.Group,
                    .logicalProcessor = information->CpuSet.LogicalProcessorIndex,
                    .core = information->CpuSet.CoreIndex,
                    .efficiency = information->CpuSet.EfficiencyClass,
                });
            }
            offset += information->Size;
        }
        std::ranges::sort(cpuSets, [](const CpuSet& left, const CpuSet& right) {
            return std::tie(left.group, left.core, left.logicalProcessor) <
                   std::tie(right.group, right.core, right.logicalProcessor);
        });
        for (std::size_t index = 1; index < cpuSets.size(); ++index) {
            if (cpuSets[index].group == cpuSets[index - 1].group && cpuSets[index].core == cpuSets[index - 1].core) {
                cpuSets[index].sibling = static_cast<std::uint8_t>(cpuSets[index - 1].sibling + 1);
            }
        }
        std::ranges::sort(cpuSets, [](const CpuSet& left, const CpuSet& right) {
            if (left.efficiency != right.efficiency) {
                return left.efficiency > right.efficiency;
            }
            return std::tie(left.sibling, left.group, left.core, left.logicalProcessor) <
                   std::tie(right.sibling, right.group, right.core, right.logicalProcessor);
        });
        return cpuSets | std::views::transform(&CpuSet::id) | std::ranges::to<std::vector>();
#else
        return {};
#endif
    }

    [[nodiscard]] std::vector<std::uint32_t> SelectWorkerCpuSets(std::size_t workerCount, bool enabled) {
        if (!enabled) {
            return {};
        }
#if defined(_WIN32)
        static const std::vector<std::uint32_t> available = AvailableCpuSets();
        if (available.size() < workerCount) {
            throw std::runtime_error("not enough CPU sets to pin every ConcurrentMap migration worker");
        }
        return {available.begin(), available.begin() + static_cast<std::ptrdiff_t>(workerCount)};
#else
        static_cast<void>(workerCount);
        return {};
#endif
    }

    [[nodiscard]] bool BindWorker(std::span<const std::uint32_t> cpuSets, std::size_t workerIndex) noexcept {
        if (cpuSets.empty()) {
            return true;
        }
#if defined(_WIN32)
        const ULONG cpuSet = cpuSets[workerIndex];
        return SetThreadSelectedCpuSets(GetCurrentThread(), &cpuSet, 1) != FALSE &&
               SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != FALSE;
#else
        static_cast<void>(workerIndex);
        return true;
#endif
    }

    [[nodiscard]] bool ConfigureBenchmarkProcess() noexcept {
#if defined(_WIN32)
        return SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS) != FALSE;
#else
        return true;
#endif
    }

    template<class Map>
    [[nodiscard]] std::unique_ptr<Map> CreateMap() {
        if constexpr (Map::capacity == std::dynamic_extent) {
            return std::make_unique<Map>(64);
        } else {
            return std::make_unique<Map>();
        }
    }

    template<class Result>
    [[nodiscard]] constexpr bool Inserted(Result result) noexcept {
        return result == Result::Inserted;
    }

    template<class Map>
    [[nodiscard]] ThroughputSample RunLookupSample(std::size_t readers, std::size_t iterationsPerReader, bool hit,
                                                   bool pinWorkers) {
        auto map = CreateMap<Map>();
        for (std::uint64_t key = 0; key < kLookupKeys; ++key) {
            if (!Inserted(map->TryInsert(key, key + 1))) {
                throw std::runtime_error("ConcurrentMap migration lookup setup failed");
            }
        }

        const auto cpuSets = SelectWorkerCpuSets(readers, pinWorkers);
        std::barrier start{static_cast<std::ptrdiff_t>(readers + 1)};
        std::latch done{static_cast<std::ptrdiff_t>(readers)};
        std::atomic<bool> placementFailed{};
        std::vector<std::uint64_t> failures(readers);
        std::vector<std::uint64_t> checksums(readers);
        std::vector<std::thread> workers;
        workers.reserve(readers);

        for (std::size_t reader = 0; reader < readers; ++reader) {
            workers.emplace_back([&, reader] {
                if (!BindWorker(cpuSets, reader)) {
                    placementFailed.store(true, std::memory_order_relaxed);
                }
                start.arrive_and_wait();
                std::uint64_t localFailures = 0;
                std::uint64_t localChecksum = 0;
                for (std::size_t iteration = 0; iteration < iterationsPerReader; ++iteration) {
                    const std::uint64_t offset = (iteration + reader * 977U) & (kLookupKeys - 1);
                    const std::uint64_t key = hit ? offset : kLookupKeys + offset;
                    std::uint64_t value = 0;
                    const bool found = map->TryGet(key, value);
                    if (hit) {
                        localFailures += !found || value != key + 1;
                        localChecksum ^= value;
                    } else {
                        localFailures += found;
                    }
                }
                failures[reader] = localFailures;
                checksums[reader] = localChecksum;
                done.count_down();
            });
        }

        const auto begin = Clock::now();
        start.arrive_and_wait();
        done.wait();
        const auto end = Clock::now();
        for (auto& worker : workers) {
            worker.join();
        }
        std::uint64_t failureCount = 0;
        std::uint64_t checksum = 0;
        for (std::size_t reader = 0; reader < readers; ++reader) {
            failureCount += failures[reader];
            checksum ^= checksums[reader];
        }
        if (placementFailed.load(std::memory_order_relaxed) || failureCount != 0 || (hit && checksum == 0)) {
            throw std::runtime_error("ConcurrentMap migration lookup integrity check failed");
        }
        return {
            .seconds = std::chrono::duration<double>(end - begin).count(),
            .operations = readers * iterationsPerReader,
        };
    }

    template<class Map>
    [[nodiscard]] ThroughputSample RunInsertSample(std::size_t writers, std::size_t keysPerWriter, bool pinWorkers) {
        auto map = CreateMap<Map>();
        const auto cpuSets = SelectWorkerCpuSets(writers, pinWorkers);
        std::barrier start{static_cast<std::ptrdiff_t>(writers + 1)};
        std::latch done{static_cast<std::ptrdiff_t>(writers)};
        std::atomic<bool> placementFailed{};
        std::vector<std::uint64_t> failures(writers);
        std::vector<std::thread> workers;
        workers.reserve(writers);

        for (std::size_t writer = 0; writer < writers; ++writer) {
            workers.emplace_back([&, writer] {
                if (!BindWorker(cpuSets, writer)) {
                    placementFailed.store(true, std::memory_order_relaxed);
                }
                start.arrive_and_wait();
                std::uint64_t localFailures = 0;
                for (std::size_t offset = 0; offset < keysPerWriter; ++offset) {
                    const std::uint64_t key = writer * keysPerWriter + offset;
                    localFailures += !Inserted(map->TryInsert(key, key + 1));
                }
                failures[writer] = localFailures;
                done.count_down();
            });
        }

        const auto begin = Clock::now();
        start.arrive_and_wait();
        done.wait();
        const auto end = Clock::now();
        for (auto& worker : workers) {
            worker.join();
        }
        const std::uint64_t operations = writers * keysPerWriter;
        std::uint64_t failureCount = 0;
        for (const std::uint64_t failuresForWriter : failures) {
            failureCount += failuresForWriter;
        }
        if (placementFailed.load(std::memory_order_relaxed) || failureCount != 0 || map->SizeApprox() != operations) {
            throw std::runtime_error("ConcurrentMap migration insert integrity check failed");
        }
        return {
            .seconds = std::chrono::duration<double>(end - begin).count(),
            .operations = operations,
        };
    }

    template<Workload Work, class Map>
    [[nodiscard]] ThroughputSample RunSample(std::size_t threads, std::size_t workPerThread, bool pinWorkers) {
        if constexpr (Work == Workload::Insert) {
            return RunInsertSample<Map>(threads, workPerThread, pinWorkers);
        } else {
            return RunLookupSample<Map>(threads, workPerThread, Work == Workload::LookupHit, pinWorkers);
        }
    }

    template<Workload Work, class MashiroMap, class SoraMap>
    [[nodiscard]] std::size_t CalibrateWork(const Config& config, std::size_t threads) {
        std::size_t work = Work == Workload::Insert ? 10'000 : 50'000;
        const std::size_t maximumWork =
            Work == Workload::Insert ? kFixedCapacity / (threads * 2) : std::size_t{1} << 28;
        for (std::size_t attempt = 0; attempt < 8; ++attempt) {
            const auto mashiro = RunSample<Work, MashiroMap>(threads, work, config.pinWorkers);
            const auto sora = RunSample<Work, SoraMap>(threads, work, config.pinWorkers);
            const double minimumSeconds = std::min(mashiro.seconds, sora.seconds);
            if (minimumSeconds >= config.minimumSampleSeconds || work == maximumWork) {
                return work;
            }
            const double scale = std::max(2.0, config.minimumSampleSeconds * 1.2 / minimumSeconds);
            work = std::min(maximumWork, static_cast<std::size_t>(std::ceil(static_cast<double>(work) * scale)));
        }
        return work;
    }

    [[nodiscard]] double Median(std::span<const double> values) {
        std::vector<double> sorted{values.begin(), values.end()};
        std::ranges::sort(sorted);
        const std::size_t middle = sorted.size() / 2;
        return sorted.size() % 2 == 0 ? (sorted[middle - 1] + sorted[middle]) * 0.5 : sorted[middle];
    }

    [[nodiscard]] std::pair<double, double> BootstrapMedianInterval(std::span<const double> values,
                                                                    std::uint32_t seed) {
        constexpr std::size_t kResamples = 20'000;
        std::minstd_rand random{seed};
        std::uniform_int_distribution<std::size_t> pick{0, values.size() - 1};
        std::vector<double> sample(values.size());
        std::vector<double> medians;
        medians.reserve(kResamples);
        for (std::size_t resample = 0; resample < kResamples; ++resample) {
            for (double& value : sample) {
                value = values[pick(random)];
            }
            medians.push_back(Median(sample));
        }
        std::ranges::sort(medians);
        return {medians[kResamples * 25 / 1000], medians[kResamples * 975 / 1000]};
    }

    template<Workload Work, class MashiroMap, class SoraMap>
    [[nodiscard]] double MeasureCase(const Config& config, std::string_view name, std::string_view storage,
                                     std::string_view topology, std::string_view operation, std::size_t threads,
                                     std::uint32_t seed) {
        static_assert(sizeof(MashiroMap) == sizeof(SoraMap));
        static_assert(alignof(MashiroMap) == alignof(SoraMap));
        const std::size_t work = CalibrateWork<Work, MashiroMap, SoraMap>(config, threads);
        for (std::size_t warmup = 0; warmup < config.warmups; ++warmup) {
            const std::size_t warmupWork = std::max(1uz, work / 2);
            if (warmup % 2 == 0) {
                static_cast<void>(RunSample<Work, MashiroMap>(threads, warmupWork, config.pinWorkers));
                static_cast<void>(RunSample<Work, SoraMap>(threads, warmupWork, config.pinWorkers));
            } else {
                static_cast<void>(RunSample<Work, SoraMap>(threads, warmupWork, config.pinWorkers));
                static_cast<void>(RunSample<Work, MashiroMap>(threads, warmupWork, config.pinWorkers));
            }
        }

        std::vector<double> ratios;
        ratios.reserve(config.samples);
        for (std::size_t sampleIndex = 0; sampleIndex < config.samples; ++sampleIndex) {
            ThroughputSample mashiro;
            ThroughputSample sora;
            const std::string_view order = sampleIndex % 2 == 0 ? "MS" : "SM";
            if (sampleIndex % 2 == 0) {
                mashiro = RunSample<Work, MashiroMap>(threads, work, config.pinWorkers);
                sora = RunSample<Work, SoraMap>(threads, work, config.pinWorkers);
            } else {
                sora = RunSample<Work, SoraMap>(threads, work, config.pinWorkers);
                mashiro = RunSample<Work, MashiroMap>(threads, work, config.pinWorkers);
            }
            const PairSample pair{
                .mashiroOperationsPerSecond = mashiro.OperationsPerSecond(),
                .soraOperationsPerSecond = sora.OperationsPerSecond(),
                .ratio = sora.OperationsPerSecond() / mashiro.OperationsPerSecond(),
            };
            ratios.push_back(pair.ratio);
            std::println("sample,{},{},{},{},{},{},{},{},{:.9g},{:.9g},{:.9f},,,,,", name, storage, topology, operation,
                         threads, mashiro.operations, sampleIndex, order, pair.mashiroOperationsPerSecond,
                         pair.soraOperationsPerSecond, pair.ratio);
        }
        const double median = Median(ratios);
        const auto [lower, upper] = BootstrapMedianInterval(ratios, seed);
        std::println("summary,{},{},{},{},{},{},,,,,,{},median,{:.9f},{:.9f},{:.9f}", name, storage, topology,
                     operation, threads, work * threads, config.samples, median, lower, upper);
        return median;
    }

    [[nodiscard]] Config ParseConfig(int argc, char** argv) {
        Config config;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument.starts_with("--samples=")) {
                config.samples = std::stoull(std::string{argument.substr(10)});
            } else if (argument.starts_with("--warmups=")) {
                config.warmups = std::stoull(std::string{argument.substr(10)});
            } else if (argument.starts_with("--minimum-ms=")) {
                config.minimumSampleSeconds = std::stod(std::string{argument.substr(13)}) / 1000.0;
            } else if (argument == "--no-pin") {
                config.pinWorkers = false;
            } else {
                throw std::invalid_argument("unknown ConcurrentMap migration benchmark argument");
            }
        }
        if (config.samples == 0 || config.minimumSampleSeconds <= 0.0) {
            throw std::invalid_argument("samples and minimum duration must be positive");
        }
        return config;
    }

    void MeasureAll(const Config& config, std::vector<double>& medians) {
        using MashiroFixedLocal = Mashiro::ConcurrentMap<std::uint64_t, std::uint64_t, kFixedCapacity,
                                                         Mashiro::Concurrency::MapTopologies::Local>;
        using SoraFixedLocal =
            Sora::ConcurrentMap<std::uint64_t, std::uint64_t, kFixedCapacity, Sora::Concurrency::MapTopologies::Local>;
        using MashiroFixedMpmc = Mashiro::ConcurrentMap<std::uint64_t, std::uint64_t, kFixedCapacity,
                                                        Mashiro::Concurrency::MapTopologies::Mpmc>;
        using SoraFixedMpmc =
            Sora::ConcurrentMap<std::uint64_t, std::uint64_t, kFixedCapacity, Sora::Concurrency::MapTopologies::Mpmc>;
        using MashiroDynamicLocal = Mashiro::ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent,
                                                           Mashiro::Concurrency::MapTopologies::Local>;
        using SoraDynamicLocal = Sora::ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent,
                                                     Sora::Concurrency::MapTopologies::Local>;
        using MashiroDynamicMpmc = Mashiro::ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent,
                                                          Mashiro::Concurrency::MapTopologies::Mpmc>;
        using SoraDynamicMpmc = Sora::ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent,
                                                    Sora::Concurrency::MapTopologies::Mpmc>;

        medians.push_back(MeasureCase<Workload::LookupHit, MashiroFixedLocal, SoraFixedLocal>(
            config, "fixed_local_find_hit", "fixed", "Local", "find_hit", 1, 0x101u));
        medians.push_back(MeasureCase<Workload::LookupHit, MashiroFixedMpmc, SoraFixedMpmc>(
            config, "fixed_mpmc_find_hit", "fixed", "MPMC", "find_hit", 4, 0x102u));
        medians.push_back(MeasureCase<Workload::LookupMiss, MashiroFixedMpmc, SoraFixedMpmc>(
            config, "fixed_mpmc_find_miss", "fixed", "MPMC", "find_miss", 4, 0x103u));
        medians.push_back(MeasureCase<Workload::Insert, MashiroFixedMpmc, SoraFixedMpmc>(
            config, "fixed_mpmc_insert", "fixed", "MPMC", "insert", 4, 0x104u));
        medians.push_back(MeasureCase<Workload::LookupHit, MashiroDynamicLocal, SoraDynamicLocal>(
            config, "dynamic_local_find_hit", "dynamic", "Local", "find_hit", 1, 0x105u));
        medians.push_back(MeasureCase<Workload::LookupHit, MashiroDynamicMpmc, SoraDynamicMpmc>(
            config, "dynamic_mpmc_find_hit", "dynamic", "MPMC", "find_hit", 4, 0x106u));
        medians.push_back(MeasureCase<Workload::Insert, MashiroDynamicLocal, SoraDynamicLocal>(
            config, "dynamic_local_insert", "dynamic", "Local", "insert", 1, 0x107u));
        medians.push_back(MeasureCase<Workload::Insert, MashiroDynamicMpmc, SoraDynamicMpmc>(
            config, "dynamic_mpmc_insert", "dynamic", "MPMC", "insert", 4, 0x108u));
    }

} // namespace

int main(int argc, char** argv) {
    try {
        const Config config = ParseConfig(argc, argv);
        if (!ConfigureBenchmarkProcess()) {
            throw std::runtime_error("failed to configure ConcurrentMap migration benchmark process priority");
        }
        std::println("kind,case,storage,topology,operation,threads,operations,sample_index,order,"
                     "mashiro_ops_per_second,sora_ops_per_second,sora_over_mashiro,sample_count,aggregate,"
                     "aggregate_ratio,ci_low,ci_high");
        std::vector<double> medians;
        medians.reserve(8);
        MeasureAll(config, medians);
        double logarithmSum = 0.0;
        for (const double median : medians) {
            logarithmSum += std::log(median);
        }
        std::println("overall,all,all,all,all,0,0,,,,,,{},geometric_mean,{:.9f},,", medians.size(),
                     std::exp(logarithmSum / static_cast<double>(medians.size())));
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::println(stderr, "ConcurrentMap migration benchmark failed: {}", error.what());
        return EXIT_FAILURE;
    }
}

/**
 * @file ConcurrentMapMigrationBenchmark.cpp
 * @brief Same-process paired evidence for the Mashiro-to-Sora ConcurrentMap migration.
 * @ingroup Testing
 */

#include <Mashiro/Core/ConcurrentMap.h>
#include <Sora/Core/Concurrency/ConcurrentMap.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
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
    constexpr std::size_t kFixedLookupCapacity = 1U << 20;
    constexpr std::size_t kFixedInsertCapacity = 1U << 23;

    enum class Workload : std::uint8_t {
        LookupHit,
        LookupMiss,
        Insert,
    };

    enum class Comparison : std::uint8_t { Migration, MashiroControl, SoraControl };

    enum class RunRole : std::uint8_t {
        ForwardFirst,
        ForwardSecond,
        ReverseFirst,
        ReverseSecond,
    };

    constexpr std::array<std::array<RunRole, 4>, 4> kCrossoverBlocks{
        std::array{RunRole::ForwardFirst, RunRole::ForwardSecond, RunRole::ForwardSecond, RunRole::ForwardFirst},
        std::array{RunRole::ForwardSecond, RunRole::ForwardFirst, RunRole::ForwardFirst, RunRole::ForwardSecond},
        std::array{RunRole::ReverseFirst, RunRole::ReverseSecond, RunRole::ReverseSecond, RunRole::ReverseFirst},
        std::array{RunRole::ReverseSecond, RunRole::ReverseFirst, RunRole::ReverseFirst, RunRole::ReverseSecond},
    };

    template<Workload Work>
    constexpr std::size_t kCrossoverRepetitions = Work == Workload::Insert ? 4 : 1;

    [[nodiscard]] constexpr std::string_view ComparisonName(Comparison comparison) noexcept {
        switch (comparison) {
            case Comparison::Migration:
                return "Mashiro/Sora";
            case Comparison::MashiroControl:
                return "Mashiro/Mashiro";
            case Comparison::SoraControl:
                return "Sora/Sora";
        }
        std::unreachable();
    }

    struct Config {
        std::size_t samples = 25;
        std::size_t warmups = 2;
        double minimumSampleSeconds = 0.12;
        bool pinWorkers = true;
        std::string_view caseName;
        Comparison comparison = Comparison::Migration;
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

    void Accumulate(ThroughputSample& aggregate, const ThroughputSample& sample) noexcept {
        aggregate.seconds += sample.seconds;
        aggregate.operations += sample.operations;
    }

    struct CpuSet {
        std::uint32_t id{};
        std::uint16_t group{};
        std::uint8_t logicalProcessor{};
        std::uint8_t core{};
        std::uint8_t efficiency{};
        std::uint8_t sibling{};
    };

    [[nodiscard]] std::vector<CpuSet> AvailableCpuSets() {
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
            if (left.sibling != right.sibling) {
                return left.sibling < right.sibling;
            }
            if (left.efficiency != right.efficiency) {
                return left.efficiency > right.efficiency;
            }
            return std::tie(left.group, left.core, left.logicalProcessor) <
                   std::tie(right.group, right.core, right.logicalProcessor);
        });
        return cpuSets;
#else
        return {};
#endif
    }

    [[nodiscard]] std::vector<std::uint32_t> SelectWorkerCpuSets(std::size_t workerCount, bool enabled) {
        if (!enabled) {
            return {};
        }
#if defined(_WIN32)
        static const std::vector<CpuSet> available = AvailableCpuSets();
        if (available.size() < workerCount) {
            throw std::runtime_error("not enough CPU sets to pin every ConcurrentMap migration worker");
        }
        return available | std::views::take(workerCount) | std::views::transform(&CpuSet::id) |
               std::ranges::to<std::vector>();
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

    template<class Result>
    [[nodiscard]] constexpr bool Inserted(Result result) noexcept {
        return result == Result::Inserted;
    }

    struct AlignedStorageDelete {
        std::size_t alignment{};

        void operator()(void* storage) const noexcept { ::operator delete(storage, std::align_val_t{alignment}); }
    };

    template<Workload Work, class FirstMap, class SecondMap>
    class PairedRunner {
        enum class Implementation : std::uint8_t { First, Second, Stop };
        static_assert(sizeof(FirstMap) == sizeof(SecondMap));
        static_assert(alignof(FirstMap) == alignof(SecondMap));

        struct alignas(64) WorkerResult {
            std::uint64_t failures{};
            std::uint64_t checksum{};
        };

    public:
        PairedRunner(std::size_t threads, bool pinWorkers)
            : threads_(threads),
              cpuSets_(SelectWorkerCpuSets(threads, pinWorkers)),
              results_(threads),
              storage_(::operator new(sizeof(FirstMap), std::align_val_t{alignof(FirstMap)}),
                       AlignedStorageDelete{alignof(FirstMap)}),
              commandBarrier_(static_cast<std::ptrdiff_t>(threads + 1)),
              readyBarrier_(static_cast<std::ptrdiff_t>(threads + 1)),
              startBarrier_(static_cast<std::ptrdiff_t>(threads + 1)),
              finishBarrier_(static_cast<std::ptrdiff_t>(threads + 1)) {
            workers_.reserve(threads_);
            try {
                for (std::size_t workerIndex = 0; workerIndex < threads_; ++workerIndex) {
                    workers_.emplace_back([this, workerIndex](std::stop_token stopToken) {
                        while (!launched_.load(std::memory_order_acquire)) {
                            if (stopToken.stop_requested()) {
                                return;
                            }
                            std::this_thread::yield();
                        }
                        Worker(workerIndex);
                    });
                }
            } catch (...) {
                for (std::jthread& worker : workers_) {
                    worker.request_stop();
                }
                for (std::jthread& worker : workers_) {
                    worker.join();
                }
                throw;
            }
            launched_.store(true, std::memory_order_release);
        }

        PairedRunner(const PairedRunner&) = delete;
        PairedRunner& operator=(const PairedRunner&) = delete;

        ~PairedRunner() {
            command_ = Implementation::Stop;
            commandBarrier_.arrive_and_wait();
            for (std::jthread& worker : workers_) {
                worker.join();
            }
        }

        [[nodiscard]] ThroughputSample RunFirst(std::size_t workPerThread) {
            return RunInSharedStorage<FirstMap>(Implementation::First, firstMap_, workPerThread);
        }

        [[nodiscard]] ThroughputSample RunSecond(std::size_t workPerThread) {
            return RunInSharedStorage<SecondMap>(Implementation::Second, secondMap_, workPerThread);
        }

    private:
        template<class Map>
        [[nodiscard]] static Map* ConstructMap(void* storage) {
            if constexpr (Map::capacity == std::dynamic_extent) {
                return std::construct_at(static_cast<Map*>(storage), 64);
            } else {
                return std::construct_at(static_cast<Map*>(storage));
            }
        }

        template<class Map>
        void Prepare(Map& map) {
            if constexpr (Work != Workload::Insert) {
                for (std::uint64_t key = 0; key < kLookupKeys; ++key) {
                    if (!Inserted(map.TryInsert(key, key + 1))) {
                        throw std::runtime_error("ConcurrentMap migration lookup setup failed");
                    }
                }
            }
        }

        template<class Map>
        [[nodiscard]] ThroughputSample RunInSharedStorage(Implementation implementation, Map*& activeMap,
                                                          std::size_t workPerThread) {
            Map* map = ConstructMap<Map>(storage_.get());
            try {
                Prepare(*map);
                activeMap = map;
                ThroughputSample sample = Run(implementation, workPerThread);
                activeMap = nullptr;
                std::destroy_at(map);
                return sample;
            } catch (...) {
                activeMap = nullptr;
                std::destroy_at(map);
                throw;
            }
        }

        template<class Map>
        [[gnu::noinline, gnu::aligned(64)]]
        void Transfer(Map& map, std::size_t workerIndex) noexcept {
            std::uint64_t failures = 0;
            std::uint64_t checksum = 0;
            if constexpr (Work == Workload::Insert) {
                for (std::size_t offset = 0; offset < workPerThread_; ++offset) {
                    const std::uint64_t key = workerIndex * workPerThread_ + offset;
                    failures += !Inserted(map.TryInsert(key, key + 1));
                }
            } else {
                constexpr bool hit = Work == Workload::LookupHit;
                for (std::size_t iteration = 0; iteration < workPerThread_; ++iteration) {
                    const std::uint64_t offset = (iteration + workerIndex * 977U) & (kLookupKeys - 1);
                    const std::uint64_t key = hit ? offset : kLookupKeys + offset;
                    std::uint64_t value = 0;
                    const bool found = map.TryGet(key, value);
                    if constexpr (hit) {
                        failures += !found || value != key + 1;
                        checksum += value;
                    } else {
                        failures += found;
                    }
                }
            }
            results_[workerIndex] = {.failures = failures, .checksum = checksum};
        }

        void Worker(std::size_t workerIndex) {
            if (!BindWorker(cpuSets_, workerIndex)) {
                placementFailed_.store(true, std::memory_order_relaxed);
            }
            for (;;) {
                commandBarrier_.arrive_and_wait();
                if (command_ == Implementation::Stop) {
                    return;
                }
                readyBarrier_.arrive_and_wait();
                startBarrier_.arrive_and_wait();
                if (command_ == Implementation::First) {
                    Transfer(*firstMap_, workerIndex);
                } else {
                    Transfer(*secondMap_, workerIndex);
                }
                finishBarrier_.arrive_and_wait();
            }
        }

        [[nodiscard]] ThroughputSample Run(Implementation implementation, std::size_t workPerThread) {
            command_ = implementation;
            workPerThread_ = workPerThread;
            commandBarrier_.arrive_and_wait();
            readyBarrier_.arrive_and_wait();
            const auto begin = Clock::now();
            startBarrier_.arrive_and_wait();
            finishBarrier_.arrive_and_wait();
            const auto end = Clock::now();

            std::uint64_t failureCount = 0;
            std::uint64_t checksum = 0;
            for (const WorkerResult& result : results_) {
                failureCount += result.failures;
                checksum += result.checksum;
            }
            const std::uint64_t operations = threads_ * workPerThread;
            const bool invalidSize = Work == Workload::Insert && ActiveMapSize() != operations;
            const bool invalidChecksum = Work == Workload::LookupHit && checksum == 0;
            if (placementFailed_.load(std::memory_order_relaxed) || failureCount != 0 || invalidSize ||
                invalidChecksum) {
                throw std::runtime_error("ConcurrentMap migration throughput integrity check failed");
            }
            return {.seconds = std::chrono::duration<double>(end - begin).count(), .operations = operations};
        }

        [[nodiscard]] std::size_t ActiveMapSize() const noexcept {
            return command_ == Implementation::First ? firstMap_->SizeApprox() : secondMap_->SizeApprox();
        }

        std::size_t threads_{};
        std::vector<std::uint32_t> cpuSets_;
        std::vector<WorkerResult> results_;
        std::unique_ptr<void, AlignedStorageDelete> storage_;
        std::vector<std::jthread> workers_;
        std::barrier<> commandBarrier_;
        std::barrier<> readyBarrier_;
        std::barrier<> startBarrier_;
        std::barrier<> finishBarrier_;
        Implementation command_ = Implementation::Stop;
        std::size_t workPerThread_{};
        FirstMap* firstMap_ = nullptr;
        SecondMap* secondMap_ = nullptr;
        std::atomic<bool> placementFailed_{};
        std::atomic<bool> launched_{};
    };

    template<Workload Work, class MashiroMap, class SoraMap>
    [[nodiscard]] std::size_t CalibrateWork(const Config& config, PairedRunner<Work, MashiroMap, SoraMap>& forward,
                                            PairedRunner<Work, SoraMap, MashiroMap>& reverse, std::size_t threads) {
        std::size_t work = Work == Workload::Insert ? 10'000 : 50'000;
        const std::size_t maximumWork =
            Work == Workload::Insert ? kFixedInsertCapacity / (threads * 2) : std::size_t{1} << 28;
        for (std::size_t attempt = 0; attempt < 8; ++attempt) {
            const auto forwardMashiro = forward.RunFirst(work);
            const auto forwardSora = forward.RunSecond(work);
            const auto reverseSora = reverse.RunFirst(work);
            const auto reverseMashiro = reverse.RunSecond(work);
            const double minimumSeconds =
                std::min({forwardMashiro.seconds, forwardSora.seconds, reverseSora.seconds, reverseMashiro.seconds});
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
        PairedRunner<Work, MashiroMap, SoraMap> forward(threads, config.pinWorkers);
        PairedRunner<Work, SoraMap, MashiroMap> reverse(threads, config.pinWorkers);
        const std::size_t work = CalibrateWork(config, forward, reverse, threads);
        for (std::size_t warmup = 0; warmup < config.warmups; ++warmup) {
            const std::size_t warmupWork = std::max(1uz, work / 2);
            if (warmup % 2 == 0) {
                static_cast<void>(forward.RunFirst(warmupWork));
                static_cast<void>(forward.RunSecond(warmupWork));
                static_cast<void>(forward.RunSecond(warmupWork));
                static_cast<void>(forward.RunFirst(warmupWork));
                static_cast<void>(reverse.RunFirst(warmupWork));
                static_cast<void>(reverse.RunSecond(warmupWork));
                static_cast<void>(reverse.RunSecond(warmupWork));
                static_cast<void>(reverse.RunFirst(warmupWork));
            } else {
                static_cast<void>(reverse.RunFirst(warmupWork));
                static_cast<void>(reverse.RunSecond(warmupWork));
                static_cast<void>(reverse.RunSecond(warmupWork));
                static_cast<void>(reverse.RunFirst(warmupWork));
                static_cast<void>(forward.RunFirst(warmupWork));
                static_cast<void>(forward.RunSecond(warmupWork));
                static_cast<void>(forward.RunSecond(warmupWork));
                static_cast<void>(forward.RunFirst(warmupWork));
            }
        }

        std::vector<double> ratios;
        ratios.reserve(config.samples);
        for (std::size_t sampleIndex = 0; sampleIndex < config.samples; ++sampleIndex) {
            ThroughputSample mashiro;
            ThroughputSample sora;
            const std::string_view order = "complete-crossover";
            for (std::size_t repetition = 0; repetition < kCrossoverRepetitions<Work>; ++repetition) {
                const std::size_t blockRotation = (sampleIndex + repetition + seed) % kCrossoverBlocks.size();
                for (std::size_t blockOffset = 0; blockOffset < kCrossoverBlocks.size(); ++blockOffset) {
                    const auto& block = kCrossoverBlocks[(blockRotation + blockOffset) % kCrossoverBlocks.size()];
                    for (const RunRole role : block) {
                        switch (role) {
                            case RunRole::ForwardFirst:
                                Accumulate(mashiro, forward.RunFirst(work));
                                break;
                            case RunRole::ForwardSecond:
                                Accumulate(sora, forward.RunSecond(work));
                                break;
                            case RunRole::ReverseFirst:
                                Accumulate(sora, reverse.RunFirst(work));
                                break;
                            case RunRole::ReverseSecond:
                                Accumulate(mashiro, reverse.RunSecond(work));
                                break;
                        }
                    }
                }
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

    template<Workload Work, class MashiroMap, class SoraMap>
    [[nodiscard]] double MeasureComparison(const Config& config, std::string_view name, std::string_view storage,
                                           std::string_view topology, std::string_view operation, std::size_t threads,
                                           std::uint32_t seed) {
        switch (config.comparison) {
            case Comparison::MashiroControl:
                return MeasureCase<Work, MashiroMap, MashiroMap>(config, name, storage, topology, operation, threads,
                                                                 seed);
            case Comparison::SoraControl:
                return MeasureCase<Work, SoraMap, SoraMap>(config, name, storage, topology, operation, threads, seed);
            case Comparison::Migration:
                return MeasureCase<Work, MashiroMap, SoraMap>(config, name, storage, topology, operation, threads,
                                                              seed);
        }
        std::unreachable();
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
            } else if (argument.starts_with("--case=")) {
                config.caseName = argument.substr(7);
            } else if (argument == "--control=mashiro") {
                config.comparison = Comparison::MashiroControl;
            } else if (argument == "--control=sora") {
                config.comparison = Comparison::SoraControl;
            } else {
                throw std::invalid_argument("unknown ConcurrentMap migration benchmark argument");
            }
        }
        constexpr std::array caseNames{
            std::string_view{"fixed_local_find_hit"},   std::string_view{"fixed_mpmc_find_hit"},
            std::string_view{"fixed_mpmc_find_miss"},   std::string_view{"fixed_mpmc_insert"},
            std::string_view{"dynamic_local_find_hit"}, std::string_view{"dynamic_mpmc_find_hit"},
            std::string_view{"dynamic_local_insert"},   std::string_view{"dynamic_mpmc_insert"},
        };
        if (config.samples == 0 || config.minimumSampleSeconds <= 0.0 ||
            (!config.caseName.empty() && !std::ranges::contains(caseNames, config.caseName))) {
            throw std::invalid_argument("invalid samples, duration, or case filter");
        }
        return config;
    }

    void MeasureAll(const Config& config, std::vector<double>& medians) {
        using MashiroFixedLocal = Mashiro::ConcurrentMap<std::uint64_t, std::uint64_t, kFixedLookupCapacity,
                                                         Mashiro::Concurrency::MapTopologies::Local>;
        using SoraFixedLocal = Sora::ConcurrentMap<std::uint64_t, std::uint64_t, kFixedLookupCapacity,
                                                   Sora::Concurrency::MapTopologies::Local>;
        using MashiroFixedMpmc = Mashiro::ConcurrentMap<std::uint64_t, std::uint64_t, kFixedLookupCapacity,
                                                        Mashiro::Concurrency::MapTopologies::Mpmc>;
        using SoraFixedMpmc = Sora::ConcurrentMap<std::uint64_t, std::uint64_t, kFixedLookupCapacity,
                                                  Sora::Concurrency::MapTopologies::Mpmc>;
        using MashiroFixedMpmcInsert = Mashiro::ConcurrentMap<std::uint64_t, std::uint64_t, kFixedInsertCapacity,
                                                              Mashiro::Concurrency::MapTopologies::Mpmc>;
        using SoraFixedMpmcInsert = Sora::ConcurrentMap<std::uint64_t, std::uint64_t, kFixedInsertCapacity,
                                                        Sora::Concurrency::MapTopologies::Mpmc>;
        using MashiroDynamicLocal = Mashiro::ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent,
                                                           Mashiro::Concurrency::MapTopologies::Local>;
        using SoraDynamicLocal = Sora::ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent,
                                                     Sora::Concurrency::MapTopologies::Local>;
        using MashiroDynamicMpmc = Mashiro::ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent,
                                                          Mashiro::Concurrency::MapTopologies::Mpmc>;
        using SoraDynamicMpmc = Sora::ConcurrentMap<std::uint64_t, std::uint64_t, std::dynamic_extent,
                                                    Sora::Concurrency::MapTopologies::Mpmc>;

        if (config.caseName.empty() || config.caseName == "fixed_local_find_hit") {
            medians.push_back(MeasureComparison<Workload::LookupHit, MashiroFixedLocal, SoraFixedLocal>(
                config, "fixed_local_find_hit", "fixed", "Local", "find_hit", 1, 0x101u));
        }
        if (config.caseName.empty() || config.caseName == "fixed_mpmc_find_hit") {
            medians.push_back(MeasureComparison<Workload::LookupHit, MashiroFixedMpmc, SoraFixedMpmc>(
                config, "fixed_mpmc_find_hit", "fixed", "MPMC", "find_hit", 4, 0x102u));
        }
        if (config.caseName.empty() || config.caseName == "fixed_mpmc_find_miss") {
            medians.push_back(MeasureComparison<Workload::LookupMiss, MashiroFixedMpmc, SoraFixedMpmc>(
                config, "fixed_mpmc_find_miss", "fixed", "MPMC", "find_miss", 4, 0x103u));
        }
        if (config.caseName.empty() || config.caseName == "fixed_mpmc_insert") {
            medians.push_back(MeasureComparison<Workload::Insert, MashiroFixedMpmcInsert, SoraFixedMpmcInsert>(
                config, "fixed_mpmc_insert", "fixed", "MPMC", "insert", 4, 0x104u));
        }
        if (config.caseName.empty() || config.caseName == "dynamic_local_find_hit") {
            medians.push_back(MeasureComparison<Workload::LookupHit, MashiroDynamicLocal, SoraDynamicLocal>(
                config, "dynamic_local_find_hit", "dynamic", "Local", "find_hit", 1, 0x105u));
        }
        if (config.caseName.empty() || config.caseName == "dynamic_mpmc_find_hit") {
            medians.push_back(MeasureComparison<Workload::LookupHit, MashiroDynamicMpmc, SoraDynamicMpmc>(
                config, "dynamic_mpmc_find_hit", "dynamic", "MPMC", "find_hit", 4, 0x106u));
        }
        if (config.caseName.empty() || config.caseName == "dynamic_local_insert") {
            medians.push_back(MeasureComparison<Workload::Insert, MashiroDynamicLocal, SoraDynamicLocal>(
                config, "dynamic_local_insert", "dynamic", "Local", "insert", 1, 0x107u));
        }
        if (config.caseName.empty() || config.caseName == "dynamic_mpmc_insert") {
            medians.push_back(MeasureComparison<Workload::Insert, MashiroDynamicMpmc, SoraDynamicMpmc>(
                config, "dynamic_mpmc_insert", "dynamic", "MPMC", "insert", 4, 0x108u));
        }
    }

} // namespace

int main(int argc, char** argv) {
    try {
        const Config config = ParseConfig(argc, argv);
        if (!ConfigureBenchmarkProcess()) {
            throw std::runtime_error("failed to configure ConcurrentMap migration benchmark process priority");
        }
        std::println(stderr, "comparison={},crossover_blocks={}", ComparisonName(config.comparison),
                     kCrossoverBlocks.size());
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

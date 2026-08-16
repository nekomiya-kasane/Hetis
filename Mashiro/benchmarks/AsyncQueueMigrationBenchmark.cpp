/**
 * @file AsyncQueueMigrationBenchmark.cpp
 * @brief Same-process paired throughput evidence for the Mashiro-to-Sora AsyncQueue migration.
 * @ingroup Core
 */

#include <Mashiro/Concurrency/AsyncQueue.h>
#include <Mashiro/Core/RingBuffer.h>
#include <Sora/Core/Concurrency/AsyncQueue.h>
#include <Sora/Core/Concurrency/RingBuffer.h>

#include <stdexec/execution.hpp>

#include <algorithm>
#include <array>
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
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#if defined(_WIN32)
#    include <windows.h>
#endif

namespace {

    using Clock = std::chrono::steady_clock;

    struct Config {
        std::size_t samples = 15;
        std::size_t warmups = 2;
        double minimumSampleSeconds = 0.15;
        bool pinWorkers = true;
    };

    template<std::size_t Size>
    struct Payload {
        static_assert(Size > sizeof(std::uint64_t));
        std::uint64_t id{};
        std::array<std::byte, Size - sizeof(std::uint64_t)> data{};
    };

    template<>
    struct Payload<sizeof(std::uint64_t)> {
        std::uint64_t id{};
    };

    static_assert(sizeof(Payload<8>) == 8);
    static_assert(sizeof(Payload<64>) == 64);

    struct ThroughputSample {
        double seconds{};
        std::uint64_t transfers{};

        [[nodiscard]] double OperationsPerSecond() const noexcept { return static_cast<double>(transfers) / seconds; }
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
            throw std::runtime_error("not enough CPU sets to pin every AsyncQueue migration worker");
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
        return SetThreadSelectedCpuSets(GetCurrentThread(), &cpuSet, 1) != FALSE;
#else
        static_cast<void>(workerIndex);
        return true;
#endif
    }

    template<typename Port, typename T>
    void PushAndWait(Port& port, T value) {
        auto result = stdexec::sync_wait(port.Push(std::move(value)));
        if (!result.has_value() || !std::get<0>(*result).Enqueued()) {
            std::terminate();
        }
    }

    template<typename Port, typename T>
    [[nodiscard]] bool PopAndWait(Port& port, T& value) {
        auto result = stdexec::sync_wait(port.Pop());
        if (!result.has_value()) {
            return false;
        }
        value = std::move(std::get<0>(*result));
        return true;
    }

    template<typename Queue, typename T>
    [[nodiscard]] ThroughputSample RunSample(std::size_t producers, std::size_t consumers, std::size_t itemsPerProducer,
                                             bool pinWorkers) {
        Queue queue;
        const std::size_t workerCount = producers + consumers;
        const auto cpuSets = SelectWorkerCpuSets(workerCount, pinWorkers);
        std::barrier start{static_cast<std::ptrdiff_t>(workerCount + 1)};
        std::latch done{static_cast<std::ptrdiff_t>(workerCount)};
        std::atomic<std::size_t> remainingProducers{producers};
        std::atomic<std::uint64_t> consumed{};
        std::atomic<std::uint64_t> checksum{};
        std::atomic<bool> placementFailed{};
        std::vector<std::thread> workers;
        workers.reserve(workerCount);

        for (std::size_t producerIndex = 0; producerIndex < producers; ++producerIndex) {
            workers.emplace_back([&, producerIndex] {
                auto port = queue.Producer();
                if (!BindWorker(cpuSets, consumers + producerIndex)) {
                    placementFailed.store(true, std::memory_order_relaxed);
                }
                start.arrive_and_wait();
                const std::uint64_t first = producerIndex * itemsPerProducer + 1;
                for (std::size_t index = 0; index < itemsPerProducer; ++index) {
                    PushAndWait(port, T{.id = first + index});
                }
                if (remainingProducers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    queue.Close();
                }
                done.count_down();
            });
        }
        for (std::size_t consumerIndex = 0; consumerIndex < consumers; ++consumerIndex) {
            workers.emplace_back([&, consumerIndex] {
                auto port = queue.Consumer();
                if (!BindWorker(cpuSets, consumerIndex)) {
                    placementFailed.store(true, std::memory_order_relaxed);
                }
                start.arrive_and_wait();
                std::uint64_t localCount = 0;
                std::uint64_t localChecksum = 0;
                T value{};
                while (PopAndWait(port, value)) {
                    ++localCount;
                    localChecksum += value.id;
                }
                consumed.fetch_add(localCount, std::memory_order_relaxed);
                checksum.fetch_add(localChecksum, std::memory_order_relaxed);
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
        const std::uint64_t expected = producers * itemsPerProducer;
        const std::uint64_t expectedChecksum = expected * (expected + 1) / 2;
        if (placementFailed.load(std::memory_order_relaxed) || consumed.load(std::memory_order_relaxed) != expected ||
            checksum.load(std::memory_order_relaxed) != expectedChecksum) {
            throw std::runtime_error("AsyncQueue migration throughput integrity check failed");
        }
        return {.seconds = std::chrono::duration<double>(end - begin).count(), .transfers = expected};
    }

    template<typename MashiroQueue, typename SoraQueue, typename T>
    [[nodiscard]] std::size_t CalibrateItems(const Config& config, std::size_t producers, std::size_t consumers) {
        std::size_t items = 20'000;
        for (std::size_t attempt = 0; attempt < 8; ++attempt) {
            const auto mashiro = RunSample<MashiroQueue, T>(producers, consumers, items, config.pinWorkers);
            const auto sora = RunSample<SoraQueue, T>(producers, consumers, items, config.pinWorkers);
            const double minimumSeconds = std::min(mashiro.seconds, sora.seconds);
            if (minimumSeconds >= config.minimumSampleSeconds) {
                return items;
            }
            const double scale = std::max(2.0, config.minimumSampleSeconds * 1.2 / minimumSeconds);
            items = static_cast<std::size_t>(std::ceil(static_cast<double>(items) * scale));
        }
        return items;
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

    template<typename MashiroQueue, typename SoraQueue, typename T>
    [[nodiscard]] double MeasureCase(const Config& config, std::string_view topology, std::size_t producers,
                                     std::size_t consumers, std::uint32_t seed) {
        const std::size_t items = CalibrateItems<MashiroQueue, SoraQueue, T>(config, producers, consumers);
        for (std::size_t warmup = 0; warmup < config.warmups; ++warmup) {
            const std::size_t warmupItems = std::max(1uz, items / 2);
            if (warmup % 2 == 0) {
                static_cast<void>(RunSample<MashiroQueue, T>(producers, consumers, warmupItems, config.pinWorkers));
                static_cast<void>(RunSample<SoraQueue, T>(producers, consumers, warmupItems, config.pinWorkers));
            } else {
                static_cast<void>(RunSample<SoraQueue, T>(producers, consumers, warmupItems, config.pinWorkers));
                static_cast<void>(RunSample<MashiroQueue, T>(producers, consumers, warmupItems, config.pinWorkers));
            }
        }

        std::vector<double> ratios;
        ratios.reserve(config.samples);
        for (std::size_t sampleIndex = 0; sampleIndex < config.samples; ++sampleIndex) {
            ThroughputSample mashiro;
            ThroughputSample sora;
            const std::string_view order = sampleIndex % 2 == 0 ? "MS" : "SM";
            if (sampleIndex % 2 == 0) {
                mashiro = RunSample<MashiroQueue, T>(producers, consumers, items, config.pinWorkers);
                sora = RunSample<SoraQueue, T>(producers, consumers, items, config.pinWorkers);
            } else {
                sora = RunSample<SoraQueue, T>(producers, consumers, items, config.pinWorkers);
                mashiro = RunSample<MashiroQueue, T>(producers, consumers, items, config.pinWorkers);
            }
            const PairSample pair{
                .mashiroOperationsPerSecond = mashiro.OperationsPerSecond(),
                .soraOperationsPerSecond = sora.OperationsPerSecond(),
                .ratio = sora.OperationsPerSecond() / mashiro.OperationsPerSecond(),
            };
            ratios.push_back(pair.ratio);
            std::println("sample,{},{},{},{},{},{},{},{:.9g},{:.9g},{:.9f},,,,,", topology, sizeof(T), producers,
                         consumers, items, sampleIndex, order, pair.mashiroOperationsPerSecond,
                         pair.soraOperationsPerSecond, pair.ratio);
        }
        const double median = Median(ratios);
        const auto [lower, upper] = BootstrapMedianInterval(ratios, seed);
        std::println("summary,{},{},{},{},{},,,,,,{},median,{:.9f},{:.9f},{:.9f}", topology, sizeof(T), producers,
                     consumers, items, config.samples, median, lower, upper);
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
                throw std::invalid_argument("unknown AsyncQueue migration benchmark argument");
            }
        }
        if (config.samples == 0 || config.minimumSampleSeconds <= 0.0) {
            throw std::invalid_argument("samples and minimum duration must be positive");
        }
        return config;
    }

    template<typename T, Mashiro::Concurrency::QueueTopology MashiroTopology,
             Sora::Concurrency::QueueTopology SoraTopology>
    [[nodiscard]] double MeasureTopology(const Config& config, std::string_view topology, std::size_t producers,
                                         std::size_t consumers, std::uint32_t seed) {
        constexpr std::size_t kCapacity = 1024;
        using MashiroStorage = Mashiro::RingBuffer<T, kCapacity, MashiroTopology>;
        using MashiroQueue = Mashiro::Concurrency::AsyncQueue<MashiroStorage>;
        using SoraStorage = Sora::Concurrency::RingBuffer<T, kCapacity, SoraTopology>;
        using SoraQueue = Sora::Concurrency::AsyncQueue<SoraStorage>;
        static_assert(sizeof(MashiroQueue) == sizeof(SoraQueue));
        static_assert(alignof(MashiroQueue) == alignof(SoraQueue));
        return MeasureCase<MashiroQueue, SoraQueue, T>(config, topology, producers, consumers, seed);
    }

    template<typename T>
    void MeasurePayload(const Config& config, std::vector<double>& medians, std::uint32_t seed) {
        medians.push_back(
            MeasureTopology<T, Mashiro::Concurrency::QueueTopologies::Spsc, Sora::Concurrency::QueueTopologies::Spsc>(
                config, "SPSC", 1, 1, seed));
        medians.push_back(
            MeasureTopology<T, Mashiro::Concurrency::QueueTopologies::Mpsc, Sora::Concurrency::QueueTopologies::Mpsc>(
                config, "MPSC", 4, 1, seed + 1));
        medians.push_back(
            MeasureTopology<T, Mashiro::Concurrency::QueueTopologies::Mpmc, Sora::Concurrency::QueueTopologies::Mpmc>(
                config, "MPMC", 4, 4, seed + 2));
    }

} // namespace

int main(int argc, char** argv) {
    try {
        const Config config = ParseConfig(argc, argv);
        std::println("kind,topology,payload_bytes,producers,consumers,items_per_producer,sample_index,order,"
                     "mashiro_ops_per_second,sora_ops_per_second,sora_over_mashiro,sample_count,aggregate,"
                     "aggregate_ratio,ci_low,ci_high");
        std::vector<double> medians;
        medians.reserve(6);
        MeasurePayload<Payload<8>>(config, medians, 0x8a51u);
        MeasurePayload<Payload<64>>(config, medians, 0x64a51u);
        double logarithmSum = 0.0;
        for (const double median : medians) {
            logarithmSum += std::log(median);
        }
        std::println("overall,all,0,0,0,0,,,,,,{},geometric_mean,{:.9f},,", medians.size(),
                     std::exp(logarithmSum / static_cast<double>(medians.size())));
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::println(stderr, "AsyncQueue migration benchmark failed: {}", error.what());
        return EXIT_FAILURE;
    }
}

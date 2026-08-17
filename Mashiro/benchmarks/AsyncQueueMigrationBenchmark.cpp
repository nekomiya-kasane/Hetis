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
    constexpr std::size_t kBalancedSubBlocks = 2;
    static_assert(kBalancedSubBlocks % 2 == 0);

    enum class Comparison : std::uint8_t { Migration, MashiroControl, SoraControl };

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
        std::size_t samples = 15;
        std::size_t warmups = 2;
        double minimumSampleSeconds = 0.15;
        bool pinWorkers = true;
        std::string_view topology;
        std::size_t payloadBytes = 0;
        Comparison comparison = Comparison::Migration;
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

    void Accumulate(ThroughputSample& aggregate, const ThroughputSample& sample) noexcept {
        aggregate.seconds += sample.seconds;
        aggregate.transfers += sample.transfers;
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
            throw std::runtime_error("not enough CPU sets to pin every AsyncQueue migration worker");
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

    template<typename MashiroQueue, typename SoraQueue, typename T>
    class PairedRunner {
        enum class Implementation : std::uint8_t { Mashiro, Sora, Stop };
        static_assert(sizeof(MashiroQueue) == sizeof(SoraQueue));
        static_assert(alignof(MashiroQueue) == alignof(SoraQueue));

    public:
        PairedRunner(std::size_t producers, std::size_t consumers, bool pinWorkers)
            : producers_(producers),
              consumers_(consumers),
              workerCount_(producers + consumers),
              cpuSets_(SelectWorkerCpuSets(workerCount_, pinWorkers)),
              commandBarrier_(static_cast<std::ptrdiff_t>(workerCount_ + 1)),
              readyBarrier_(static_cast<std::ptrdiff_t>(workerCount_ + 1)),
              startBarrier_(static_cast<std::ptrdiff_t>(workerCount_ + 1)),
              finishBarrier_(static_cast<std::ptrdiff_t>(workerCount_ + 1)) {
            workers_.reserve(workerCount_);
            try {
                for (std::size_t workerIndex = 0; workerIndex < workerCount_; ++workerIndex) {
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

        [[nodiscard]] ThroughputSample RunMashiro(std::size_t itemsPerProducer) {
            return RunInSharedStorage<MashiroQueue>(Implementation::Mashiro, mashiroQueue_, itemsPerProducer);
        }

        [[nodiscard]] ThroughputSample RunSora(std::size_t itemsPerProducer) {
            return RunInSharedStorage<SoraQueue>(Implementation::Sora, soraQueue_, itemsPerProducer);
        }

    private:
        template<typename Queue>
        [[nodiscard]] ThroughputSample RunInSharedStorage(Implementation implementation, Queue*& activeQueue,
                                                          std::size_t itemsPerProducer) {
            Queue* queue = std::construct_at(reinterpret_cast<Queue*>(queueStorage_));
            activeQueue = queue;
            try {
                ThroughputSample sample = Run(implementation, itemsPerProducer);
                std::destroy_at(queue);
                activeQueue = nullptr;
                return sample;
            } catch (...) {
                std::destroy_at(queue);
                activeQueue = nullptr;
                throw;
            }
        }

        template<typename Queue>
        void Produce(Queue& queue, std::size_t producerIndex) {
            auto port = queue.Producer();
            readyBarrier_.arrive_and_wait();
            startBarrier_.arrive_and_wait();
            const std::uint64_t first = producerIndex * itemsPerProducer_ + 1;
            for (std::size_t index = 0; index < itemsPerProducer_; ++index) {
                PushAndWait(port, T{.id = first + index});
            }
            if (remainingProducers_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                queue.Close();
            }
        }

        template<typename Queue>
        void Consume(Queue& queue) {
            auto port = queue.Consumer();
            readyBarrier_.arrive_and_wait();
            startBarrier_.arrive_and_wait();
            std::uint64_t localCount = 0;
            std::uint64_t localChecksum = 0;
            T value{};
            while (PopAndWait(port, value)) {
                ++localCount;
                localChecksum += value.id;
            }
            consumed_.fetch_add(localCount, std::memory_order_relaxed);
            checksum_.fetch_add(localChecksum, std::memory_order_relaxed);
        }

        template<typename Queue>
        void Transfer(Queue& queue, std::size_t workerIndex) {
            if (workerIndex < consumers_) {
                Consume(queue);
            } else {
                Produce(queue, workerIndex - consumers_);
            }
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
                if (command_ == Implementation::Mashiro) {
                    Transfer(*mashiroQueue_, workerIndex);
                } else {
                    Transfer(*soraQueue_, workerIndex);
                }
                finishBarrier_.arrive_and_wait();
            }
        }

        [[nodiscard]] ThroughputSample Run(Implementation implementation, std::size_t itemsPerProducer) {
            command_ = implementation;
            itemsPerProducer_ = itemsPerProducer;
            remainingProducers_.store(producers_, std::memory_order_relaxed);
            consumed_.store(0, std::memory_order_relaxed);
            checksum_.store(0, std::memory_order_relaxed);
            commandBarrier_.arrive_and_wait();
            readyBarrier_.arrive_and_wait();
            const auto begin = Clock::now();
            startBarrier_.arrive_and_wait();
            finishBarrier_.arrive_and_wait();
            const auto end = Clock::now();

            const std::uint64_t expected = producers_ * itemsPerProducer;
            const std::uint64_t expectedChecksum = expected * (expected + 1) / 2;
            if (placementFailed_.load(std::memory_order_relaxed) ||
                consumed_.load(std::memory_order_relaxed) != expected ||
                checksum_.load(std::memory_order_relaxed) != expectedChecksum) {
                throw std::runtime_error("AsyncQueue migration throughput integrity check failed");
            }
            return {.seconds = std::chrono::duration<double>(end - begin).count(), .transfers = expected};
        }

        std::size_t producers_{};
        std::size_t consumers_{};
        std::size_t workerCount_{};
        std::vector<std::uint32_t> cpuSets_;
        std::vector<std::jthread> workers_;
        std::barrier<> commandBarrier_;
        std::barrier<> readyBarrier_;
        std::barrier<> startBarrier_;
        std::barrier<> finishBarrier_;
        Implementation command_ = Implementation::Stop;
        std::size_t itemsPerProducer_{};
        alignas(MashiroQueue) std::byte queueStorage_[sizeof(MashiroQueue)];
        MashiroQueue* mashiroQueue_ = nullptr;
        SoraQueue* soraQueue_ = nullptr;
        std::atomic<std::size_t> remainingProducers_{};
        std::atomic<std::uint64_t> consumed_{};
        std::atomic<std::uint64_t> checksum_{};
        std::atomic<bool> placementFailed_{};
        std::atomic<bool> launched_{};
    };

    template<typename MashiroQueue, typename SoraQueue, typename T>
    [[nodiscard]] std::size_t CalibrateItems(const Config& config, PairedRunner<MashiroQueue, SoraQueue, T>& forward,
                                             PairedRunner<SoraQueue, MashiroQueue, T>& reverse) {
        std::size_t items = 20'000;
        for (std::size_t attempt = 0; attempt < 8; ++attempt) {
            const auto forwardMashiro = forward.RunMashiro(items);
            const auto forwardSora = forward.RunSora(items);
            const auto reverseSora = reverse.RunMashiro(items);
            const auto reverseMashiro = reverse.RunSora(items);
            const double minimumSeconds =
                std::min({forwardMashiro.seconds, forwardSora.seconds, reverseSora.seconds, reverseMashiro.seconds});
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
        PairedRunner<MashiroQueue, SoraQueue, T> forward(producers, consumers, config.pinWorkers);
        PairedRunner<SoraQueue, MashiroQueue, T> reverse(producers, consumers, config.pinWorkers);
        const std::size_t items = CalibrateItems(config, forward, reverse);
        for (std::size_t warmup = 0; warmup < config.warmups; ++warmup) {
            const std::size_t warmupItems = std::max(1uz, items / 2);
            if (warmup % 2 == 0) {
                static_cast<void>(forward.RunMashiro(warmupItems));
                static_cast<void>(forward.RunSora(warmupItems));
                static_cast<void>(forward.RunSora(warmupItems));
                static_cast<void>(forward.RunMashiro(warmupItems));
                static_cast<void>(reverse.RunMashiro(warmupItems));
                static_cast<void>(reverse.RunSora(warmupItems));
                static_cast<void>(reverse.RunSora(warmupItems));
                static_cast<void>(reverse.RunMashiro(warmupItems));
            } else {
                static_cast<void>(reverse.RunMashiro(warmupItems));
                static_cast<void>(reverse.RunSora(warmupItems));
                static_cast<void>(reverse.RunSora(warmupItems));
                static_cast<void>(reverse.RunMashiro(warmupItems));
                static_cast<void>(forward.RunMashiro(warmupItems));
                static_cast<void>(forward.RunSora(warmupItems));
                static_cast<void>(forward.RunSora(warmupItems));
                static_cast<void>(forward.RunMashiro(warmupItems));
            }
        }

        std::vector<double> ratios;
        ratios.reserve(config.samples);
        for (std::size_t sampleIndex = 0; sampleIndex < config.samples; ++sampleIndex) {
            ThroughputSample mashiro;
            ThroughputSample sora;
            const std::string_view order = sampleIndex % 2 == 0 ? "MSSM+SMMS" : "SMMS+MSSM";
            for (std::size_t subBlock = 0; subBlock < kBalancedSubBlocks; ++subBlock) {
                if ((sampleIndex + subBlock) % 2 == 0) {
                    Accumulate(mashiro, forward.RunMashiro(items));
                    Accumulate(sora, forward.RunSora(items));
                    Accumulate(sora, forward.RunSora(items));
                    Accumulate(mashiro, forward.RunMashiro(items));
                    Accumulate(sora, reverse.RunMashiro(items));
                    Accumulate(mashiro, reverse.RunSora(items));
                    Accumulate(mashiro, reverse.RunSora(items));
                    Accumulate(sora, reverse.RunMashiro(items));
                } else {
                    Accumulate(sora, reverse.RunMashiro(items));
                    Accumulate(mashiro, reverse.RunSora(items));
                    Accumulate(mashiro, reverse.RunSora(items));
                    Accumulate(sora, reverse.RunMashiro(items));
                    Accumulate(mashiro, forward.RunMashiro(items));
                    Accumulate(sora, forward.RunSora(items));
                    Accumulate(sora, forward.RunSora(items));
                    Accumulate(mashiro, forward.RunMashiro(items));
                }
            }
            const double mashiroOperationsPerSecond = mashiro.OperationsPerSecond();
            const double soraOperationsPerSecond = sora.OperationsPerSecond();
            const PairSample pair{
                .mashiroOperationsPerSecond = mashiroOperationsPerSecond,
                .soraOperationsPerSecond = soraOperationsPerSecond,
                .ratio = soraOperationsPerSecond / mashiroOperationsPerSecond,
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
            } else if (argument.starts_with("--topology=")) {
                config.topology = argument.substr(11);
            } else if (argument.starts_with("--payload-bytes=")) {
                config.payloadBytes = std::stoull(std::string{argument.substr(16)});
            } else if (argument == "--control=mashiro") {
                config.comparison = Comparison::MashiroControl;
            } else if (argument == "--control=sora") {
                config.comparison = Comparison::SoraControl;
            } else {
                throw std::invalid_argument("unknown AsyncQueue migration benchmark argument");
            }
        }
        if (config.samples == 0 || config.minimumSampleSeconds <= 0.0 ||
            (!config.topology.empty() && config.topology != "SPSC" && config.topology != "MPSC" &&
             config.topology != "MPMC") ||
            (config.payloadBytes != 0 && config.payloadBytes != 8 && config.payloadBytes != 64)) {
            throw std::invalid_argument("invalid samples, duration, topology, or payload filter");
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
        switch (config.comparison) {
            case Comparison::MashiroControl:
                return MeasureCase<MashiroQueue, MashiroQueue, T>(config, topology, producers, consumers, seed);
            case Comparison::SoraControl:
                return MeasureCase<SoraQueue, SoraQueue, T>(config, topology, producers, consumers, seed);
            case Comparison::Migration:
                return MeasureCase<MashiroQueue, SoraQueue, T>(config, topology, producers, consumers, seed);
        }
        std::unreachable();
    }

    template<typename T>
    void MeasurePayload(const Config& config, std::vector<double>& medians, std::uint32_t seed) {
        if (config.topology.empty() || config.topology == "SPSC") {
            medians.push_back(MeasureTopology<T, Mashiro::Concurrency::QueueTopologies::Spsc,
                                              Sora::Concurrency::QueueTopologies::Spsc>(config, "SPSC", 1, 1, seed));
        }
        if (config.topology.empty() || config.topology == "MPSC") {
            medians.push_back(
                MeasureTopology<T, Mashiro::Concurrency::QueueTopologies::Mpsc,
                                Sora::Concurrency::QueueTopologies::Mpsc>(config, "MPSC", 4, 1, seed + 1));
        }
        if (config.topology.empty() || config.topology == "MPMC") {
            medians.push_back(
                MeasureTopology<T, Mashiro::Concurrency::QueueTopologies::Mpmc,
                                Sora::Concurrency::QueueTopologies::Mpmc>(config, "MPMC", 4, 4, seed + 2));
        }
    }

} // namespace

int main(int argc, char** argv) {
    try {
        const Config config = ParseConfig(argc, argv);
        if (!ConfigureBenchmarkProcess()) {
            throw std::runtime_error("failed to configure high-priority AsyncQueue migration benchmark process");
        }
        std::println(stderr, "comparison={},balanced_sub_blocks={}", ComparisonName(config.comparison),
                     kBalancedSubBlocks);
        std::println("kind,topology,payload_bytes,producers,consumers,items_per_producer,sample_index,order,"
                     "mashiro_ops_per_second,sora_ops_per_second,sora_over_mashiro,sample_count,aggregate,"
                     "aggregate_ratio,ci_low,ci_high");
        std::vector<double> medians;
        medians.reserve(6);
        if (config.payloadBytes == 0 || config.payloadBytes == 8) {
            MeasurePayload<Payload<8>>(config, medians, 0x8a51u);
        }
        if (config.payloadBytes == 0 || config.payloadBytes == 64) {
            MeasurePayload<Payload<64>>(config, medians, 0x64a51u);
        }
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

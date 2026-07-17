/**
 * @file AsyncQueueBenchmark.cpp
 * @brief Semantic-layered benchmark for asynchronous bounded queues and comparable mainstream implementations.
 * @ingroup Core
 */

#include <Mashiro/Concurrency/AsyncQueue.h>
#include <Mashiro/Core/MpmcQueue.h>
#include <Mashiro/Core/MpscQueue.h>
#include <Mashiro/Core/SpscRingBuffer.h>

#include <blockingconcurrentqueue.h>
#include <oneapi/tbb/concurrent_queue.h>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <latch>
#include <memory>
#include <mutex>
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

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

namespace Mashiro::Benchmark {

    using Clock = std::chrono::steady_clock;
    using Nanoseconds = std::chrono::nanoseconds;

    /** @brief Benchmark configuration parsed from the command line. */
    struct Config {
        std::filesystem::path output = "async-queue-results.csv";
        std::string label = "formal";
        std::string section = "all";
        std::size_t samples = 25;
        std::size_t warmups = 3;
        std::size_t itemsPerProducer = 20'000;
        std::size_t latencyIterations = 10'000;
        double minimumSampleSeconds = 0.3;
        std::chrono::microseconds parkDelay{20};
        bool pinWorkers = true;
    };

    /** @brief Fixed-size trivially copyable payload carrying a correctness identity. */
    template<std::size_t Size>
    struct Payload {
        static_assert(Size > sizeof(std::uint64_t));
        std::uint64_t id{};
        std::array<std::byte, Size - sizeof(std::uint64_t)> data{};
    };

    /** @brief Minimal payload specialization without a non-zero empty-array representation. */
    template<>
    struct Payload<sizeof(std::uint64_t)> {
        std::uint64_t id{};
    };

    static_assert(sizeof(Payload<8>) == 8);
    static_assert(sizeof(Payload<64>) == 64);

    /** @brief One end-to-end throughput sample. */
    struct ThroughputSample {
        double seconds{};
        std::uint64_t transfers{};
    };

    /** @brief One latency distribution measured over repeated park/resume operations. */
    struct LatencySample {
        std::vector<std::uint64_t> nanoseconds{};
    };

    /** @brief Return CPU-set IDs ordered by performance class, physical core, and SMT sibling. */
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

    /** @brief Select one deterministic CPU set for every benchmark worker. */
    [[nodiscard]] std::vector<std::uint32_t> SelectWorkerCpuSets(std::size_t workerCount, bool enabled) {
        if (!enabled) {
            return {};
        }
#if defined(_WIN32)
        static const std::vector<std::uint32_t> available = AvailableCpuSets();
        if (available.size() < workerCount) {
            throw std::runtime_error("not enough CPU sets to pin every async-queue benchmark worker");
        }
        return {available.begin(), available.begin() + static_cast<std::ptrdiff_t>(workerCount)};
#else
        static_cast<void>(workerCount);
        return {};
#endif
    }

    /** @brief Bind the current worker to its selected CPU set. */
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

    /** @brief Delay briefly without inheriting the operating system's coarse sleep quantum. */
    void SpinDelay(std::chrono::microseconds delay) noexcept {
        const auto deadline = Clock::now() + delay;
        while (Clock::now() < deadline) {
#if defined(_M_X64) || defined(__x86_64__)
            _mm_pause();
#else
            std::this_thread::yield();
#endif
        }
    }

    /** @brief Force one object state to remain observable to the optimizer without emitting runtime instructions. */
    template<typename T>
    inline void DoNotOptimize(const T& value) noexcept {
#if defined(__clang__) || defined(__GNUC__)
        asm volatile("" : : "g"(std::addressof(value)) : "memory");
#else
        static_cast<void>(value);
        std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
    }

    /** @brief Strictly bounded blocking queue used as a transparent monitor-based reference. */
    template<typename T, std::size_t Capacity>
    class ConditionQueue {
    public:
        bool Push(T value) {
            std::unique_lock lock{mutex_};
            writable_.wait(lock, [&] { return aborted_ || queue_.size() != Capacity; });
            if (aborted_) {
                return false;
            }
            queue_.push_back(std::move(value));
            readable_.notify_one();
            return true;
        }

        bool Pop(T& value) {
            std::unique_lock lock{mutex_};
            readable_.wait(lock, [&] { return aborted_ || closed_ || !queue_.empty(); });
            if (queue_.empty()) {
                return false;
            }
            value = std::move(queue_.front());
            queue_.pop_front();
            writable_.notify_one();
            return true;
        }

        void Close() noexcept {
            {
                std::scoped_lock lock{mutex_};
                closed_ = true;
            }
            readable_.notify_all();
        }

        void Abort() noexcept {
            {
                std::scoped_lock lock{mutex_};
                aborted_ = true;
            }
            readable_.notify_all();
            writable_.notify_all();
        }

    private:
        std::mutex mutex_{};
        std::condition_variable readable_{};
        std::condition_variable writable_{};
        std::deque<T> queue_{};
        bool closed_{};
        bool aborted_{};
    };

    /** @brief P2300 receiver recording one pop completion without introducing a scheduler or allocation. */
    template<typename T>
    struct InlineReceiver {
        using receiver_concept = stdexec::receiver_t;

        T* value{};
        std::atomic<bool>* completed{};
        stdexec::inplace_stop_token token{};

        [[nodiscard]] auto get_env() const noexcept {
            return stdexec::env{stdexec::prop{stdexec::get_stop_token, token}};
        }

        void set_value(T result) && noexcept {
            if (value != nullptr) {
                *value = std::move(result);
            }
            completed->store(true, std::memory_order_release);
            completed->notify_one();
        }

        void set_stopped() && noexcept {
            completed->store(true, std::memory_order_release);
            completed->notify_one();
        }

        void set_error(Concurrency::QueueError) && noexcept {
            completed->store(true, std::memory_order_release);
            completed->notify_one();
        }
    };

    /** @brief P2300 receiver recording one push completion. */
    struct PushReceiver {
        using receiver_concept = stdexec::receiver_t;

        std::atomic<bool>* completed{};

        [[nodiscard]] stdexec::env<> get_env() const noexcept { return {}; }

        void set_value(Concurrency::PushOutcome) && noexcept {
            completed->store(true, std::memory_order_release);
            completed->notify_one();
        }

        void set_stopped() && noexcept {
            completed->store(true, std::memory_order_release);
            completed->notify_one();
        }

        void set_error(Concurrency::QueueError) && noexcept {
            completed->store(true, std::memory_order_release);
            completed->notify_one();
        }
    };

    /** @brief Receiver counting callback fan-out completions without blocking a thread. */
    template<typename T>
    struct FanoutReceiver {
        using receiver_concept = stdexec::receiver_t;

        std::atomic<std::size_t>* completions{};

        [[nodiscard]] stdexec::env<> get_env() const noexcept { return {}; }

        void set_value(T) && noexcept { Complete(); }
        void set_stopped() && noexcept { Complete(); }
        void set_error(Concurrency::QueueError) && noexcept { Complete(); }

    private:
        void Complete() noexcept {
            completions->fetch_add(1, std::memory_order_release);
            completions->notify_one();
        }
    };

    /** @brief Write all benchmark categories into one normalized CSV stream. */
    class CsvWriter {
    public:
        explicit CsvWriter(const Config& config) : stream_(config.output) {
            if (!stream_) {
                throw std::runtime_error("cannot open async-queue benchmark output");
            }
            auto rawLatencyPath = config.output;
            rawLatencyPath.replace_filename(config.output.stem().string() + "-latency.csv");
            rawLatencyStream_.open(rawLatencyPath);
            if (!rawLatencyStream_) {
                throw std::runtime_error("cannot open raw async-queue latency output");
            }
            stream_ << "label,category,scenario,implementation,payload_bytes,capacity,producers,consumers,sample,"
                       "observations,seconds,operations_per_second,p50_ns,p90_ns,p99_ns,p999_ns,max_ns,bytes,"
                       "thread_placement,park_delay_us\n";
            rawLatencyStream_ << "label,category,scenario,implementation,sample,observation,nanoseconds\n";
        }

        void WriteThroughput(const Config& config, std::string_view scenario, std::string_view implementation,
                             std::size_t payloadBytes, std::size_t capacity, std::size_t producers,
                             std::size_t consumers, std::size_t sampleIndex, const ThroughputSample& sample) {
            WritePrefix(config, "throughput", scenario, implementation, payloadBytes, capacity, producers, consumers,
                        sampleIndex, sample.transfers, sample.seconds,
                        static_cast<double>(sample.transfers) / sample.seconds);
            stream_ << ",,,,,,0," << Placement(config) << ',' << config.parkDelay.count() << '\n';
            stream_.flush();
        }

        void WriteRate(const Config& config, std::string_view category, std::string_view scenario,
                       std::string_view implementation, std::size_t sampleIndex, std::uint64_t operations,
                       double seconds) {
            WritePrefix(config, category, scenario, implementation, 0, 0, 1, 1, sampleIndex, operations, seconds,
                        static_cast<double>(operations) / seconds);
            stream_ << ",,,,,,0," << Placement(config) << ',' << config.parkDelay.count() << '\n';
            stream_.flush();
        }

        void WriteLatency(const Config& config, std::string_view category, std::string_view scenario,
                          std::string_view implementation, std::size_t sampleIndex, LatencySample sample) {
            if (sample.nanoseconds.empty()) {
                throw std::runtime_error("latency sample is empty");
            }
            for (std::size_t index = 0; index < sample.nanoseconds.size(); ++index) {
                rawLatencyStream_ << config.label << ',' << category << ',' << scenario << ',' << implementation
                                  << ',' << sampleIndex << ',' << index << ',' << sample.nanoseconds[index] << '\n';
            }
            rawLatencyStream_.flush();
            std::ranges::sort(sample.nanoseconds);
            const auto percentile = [&](double quantile) {
                const double position = quantile * static_cast<double>(sample.nanoseconds.size() - 1);
                return sample.nanoseconds[static_cast<std::size_t>(std::llround(position))];
            };
            WritePrefix(config, category, scenario, implementation, 0, 0, 1, 1, sampleIndex,
                        sample.nanoseconds.size(), 0.0, 0.0);
            stream_ << ',' << percentile(0.50) << ',' << percentile(0.90) << ',' << percentile(0.99) << ','
                    << percentile(0.999) << ',' << sample.nanoseconds.back() << ",0," << Placement(config) << ','
                    << config.parkDelay.count() << '\n';
            stream_.flush();
        }

        void WriteSize(const Config& config, std::string_view scenario, std::string_view implementation,
                       std::size_t bytes) {
            WritePrefix(config, "size", scenario, implementation, 0, 0, 0, 0, 0, 1, 0.0, 0.0);
            stream_ << ",,,,,," << bytes << ',' << Placement(config) << ',' << config.parkDelay.count() << '\n';
            stream_.flush();
        }

    private:
        static std::string_view Placement(const Config& config) noexcept {
            return config.pinWorkers ? "cpu-set" : "scheduler";
        }

        void WritePrefix(const Config& config, std::string_view category, std::string_view scenario,
                         std::string_view implementation, std::size_t payloadBytes, std::size_t capacity,
                         std::size_t producers, std::size_t consumers, std::size_t sampleIndex,
                         std::uint64_t observations, double seconds, double rate) {
            stream_ << config.label << ',' << category << ',' << scenario << ',' << implementation << ','
                    << payloadBytes << ',' << capacity << ',' << producers << ',' << consumers << ',' << sampleIndex
                    << ',' << observations << ',' << seconds << ',' << rate;
        }

        std::ofstream stream_;
        std::ofstream rawLatencyStream_;
    };

    /** @brief oneTBB bounded blocking queue normalized to the benchmark lifecycle protocol. */
    template<typename T, std::size_t Capacity>
    class TbbBlockingQueue {
    public:
        TbbBlockingQueue() { queue_.set_capacity(Capacity); }

        void Push(T value) { queue_.push(std::move(value)); }

        bool Pop(T& value, const std::atomic<bool>&) {
            try {
                queue_.pop(value);
                return true;
            } catch (const oneapi::tbb::user_abort&) {
                return false;
            }
        }

        void ProducersDone() noexcept {}
        void ConsumersDone() noexcept { queue_.abort(); }

    private:
        oneapi::tbb::concurrent_bounded_queue<T> queue_{};
    };

    /** @brief moodycamel blocking queue normalized to the benchmark lifecycle protocol. */
    template<typename T, std::size_t Capacity>
    class MoodycamelBlockingQueue {
    public:
        MoodycamelBlockingQueue() : queue_(Capacity) {}

        void Push(T value) { queue_.enqueue(std::move(value)); }

        bool Pop(T& value, const std::atomic<bool>& allConsumed) {
            while (!allConsumed.load(std::memory_order_acquire)) {
                if (queue_.wait_dequeue_timed(value, std::chrono::microseconds{100})) {
                    return true;
                }
            }
            return false;
        }

        void ProducersDone() noexcept {}
        void ConsumersDone() noexcept {}

    private:
        moodycamel::BlockingConcurrentQueue<T> queue_;
    };

    /** @brief condition-variable queue normalized to the benchmark lifecycle protocol. */
    template<typename T, std::size_t Capacity>
    class ConditionBlockingQueue {
    public:
        void Push(T value) {
            if (!queue_.Push(std::move(value))) {
                std::terminate();
            }
        }

        bool Pop(T& value, const std::atomic<bool>&) { return queue_.Pop(value); }
        void ProducersDone() noexcept { queue_.Close(); }
        void ConsumersDone() noexcept { queue_.Abort(); }

    private:
        ConditionQueue<T, Capacity> queue_{};
    };

    /** @brief Run one Mashiro AsyncQueue throughput sample through the public sender API. */
    template<typename Storage, typename T>
    [[nodiscard]] ThroughputSample RunMashiroThroughput(std::size_t producers, std::size_t consumers,
                                                        std::size_t itemsPerProducer, bool pinWorkers) {
        using Queue = Concurrency::AsyncQueue<Storage>;
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
                    auto result = stdexec::sync_wait(port.Push(T{.id = first + index}));
                    if (!result.has_value() || !std::get<0>(*result).Enqueued()) {
                        std::terminate();
                    }
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
                while (auto result = stdexec::sync_wait(port.Pop())) {
                    ++localCount;
                    localChecksum += std::get<0>(*result).id;
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
            throw std::runtime_error("Mashiro AsyncQueue throughput integrity check failed");
        }
        return {.seconds = std::chrono::duration<double>(end - begin).count(), .transfers = expected};
    }

    /** @brief Run one external blocking queue through the same producer/consumer topology and integrity checks. */
    template<typename Queue, typename T>
    [[nodiscard]] ThroughputSample RunExternalThroughput(std::size_t producers, std::size_t consumers,
                                                         std::size_t itemsPerProducer, bool pinWorkers) {
        Queue queue;
        const std::size_t workerCount = producers + consumers;
        const auto cpuSets = SelectWorkerCpuSets(workerCount, pinWorkers);
        std::barrier start{static_cast<std::ptrdiff_t>(workerCount + 1)};
        std::latch done{static_cast<std::ptrdiff_t>(workerCount)};
        std::atomic<std::size_t> remainingProducers{producers};
        std::atomic<std::uint64_t> consumed{};
        std::atomic<std::uint64_t> checksum{};
        std::atomic<bool> allConsumed{};
        std::atomic<bool> placementFailed{};
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        const std::uint64_t expected = producers * itemsPerProducer;

        for (std::size_t producerIndex = 0; producerIndex < producers; ++producerIndex) {
            workers.emplace_back([&, producerIndex] {
                if (!BindWorker(cpuSets, consumers + producerIndex)) {
                    placementFailed.store(true, std::memory_order_relaxed);
                }
                start.arrive_and_wait();
                const std::uint64_t first = producerIndex * itemsPerProducer + 1;
                for (std::size_t index = 0; index < itemsPerProducer; ++index) {
                    queue.Push(T{.id = first + index});
                }
                if (remainingProducers.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    queue.ProducersDone();
                }
                done.count_down();
            });
        }
        for (std::size_t consumerIndex = 0; consumerIndex < consumers; ++consumerIndex) {
            workers.emplace_back([&, consumerIndex] {
                if (!BindWorker(cpuSets, consumerIndex)) {
                    placementFailed.store(true, std::memory_order_relaxed);
                }
                start.arrive_and_wait();
                std::uint64_t localChecksum = 0;
                T value{};
                while (!allConsumed.load(std::memory_order_acquire) && queue.Pop(value, allConsumed)) {
                    localChecksum += value.id;
                    if (consumed.fetch_add(1, std::memory_order_acq_rel) + 1 == expected) {
                        allConsumed.store(true, std::memory_order_release);
                        queue.ConsumersDone();
                        break;
                    }
                }
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
        const std::uint64_t expectedChecksum = expected * (expected + 1) / 2;
        if (placementFailed.load(std::memory_order_relaxed) || consumed.load(std::memory_order_relaxed) != expected ||
            checksum.load(std::memory_order_relaxed) != expectedChecksum) {
            throw std::runtime_error("external blocking-queue throughput integrity check failed");
        }
        return {.seconds = std::chrono::duration<double>(end - begin).count(), .transfers = expected};
    }

    /** @brief Calibrate a throughput runner until its pilot reaches the configured minimum duration. */
    template<typename Runner>
    [[nodiscard]] std::size_t CalibrateItems(const Config& config, Runner&& runner) {
        std::size_t items = config.itemsPerProducer;
        for (std::size_t attempt = 0; attempt < 8 && config.minimumSampleSeconds != 0.0; ++attempt) {
            const auto pilot = runner(items);
            if (pilot.seconds >= config.minimumSampleSeconds) {
                return items;
            }
            const double scale = std::max(2.0, config.minimumSampleSeconds * 1.2 / pilot.seconds);
            const double requested = static_cast<double>(items) * scale;
            if (!std::isfinite(requested) || requested > 1'000'000'000.0) {
                throw std::overflow_error("async-queue throughput calibration exceeded the practical item limit");
            }
            items = static_cast<std::size_t>(std::ceil(requested));
        }
        return items;
    }

    /** @brief Run warmups and measured throughput samples for one implementation. */
    template<typename Runner>
    void RunThroughputCase(const Config& config, CsvWriter& writer, std::string_view scenario,
                           std::string_view implementation, std::size_t payloadBytes, std::size_t capacity,
                           std::size_t producers, std::size_t consumers, Runner&& runner) {
        std::size_t items = CalibrateItems(config, runner);
        std::cout << "throughput | " << scenario << " | " << implementation << " | payload=" << payloadBytes
                  << " | items/producer=" << items << '\n';
        for (std::size_t warmup = 0; warmup < config.warmups; ++warmup) {
            static_cast<void>(runner(std::max(1uz, items / 2)));
        }
        for (std::size_t sample = 0; sample < config.samples; ++sample) {
            for (std::size_t attempt = 0;; ++attempt) {
                const ThroughputSample result = runner(items);
                if (config.minimumSampleSeconds == 0.0 || result.seconds >= config.minimumSampleSeconds) {
                    writer.WriteThroughput(config, scenario, implementation, payloadBytes, capacity, producers,
                                           consumers, sample, result);
                    if (config.minimumSampleSeconds != 0.0) {
                        constexpr double kTargetMargin = 1.25;
                        const double scale =
                            std::clamp(config.minimumSampleSeconds * kTargetMargin / result.seconds, 0.1, 10.0);
                        items = std::max(1uz, static_cast<std::size_t>(std::ceil(items * scale)));
                    }
                    break;
                }
                if (attempt == 7) {
                    throw std::runtime_error("throughput sample did not reach the minimum duration after adaptation");
                }
                const double scale =
                    std::max(2.0, config.minimumSampleSeconds * 1.25 / std::max(result.seconds, 1e-9));
                const double requested = static_cast<double>(items) * scale;
                if (!std::isfinite(requested) || requested > 1'000'000'000.0) {
                    throw std::overflow_error("adaptive throughput sample exceeded the practical item limit");
                }
                items = static_cast<std::size_t>(std::ceil(requested));
            }
        }
    }

    /** @brief Measure same-thread ready-path cost for storage, facade, and connected sender layers. */
    template<typename T, std::size_t Capacity>
    void RunReadyPath(const Config& config, CsvWriter& writer) {
        using Storage = SpscRingBuffer<T, Capacity>;
        using Queue = Concurrency::AsyncQueue<Storage>;
        const auto runRate = [&](std::string_view implementation, auto&& body) {
            std::uint64_t iterations = 100'000;
            while (config.minimumSampleSeconds != 0.0) {
                const auto begin = Clock::now();
                body(iterations);
                const double seconds = std::chrono::duration<double>(Clock::now() - begin).count();
                if (seconds >= config.minimumSampleSeconds) {
                    break;
                }
                const double scale = std::max(2.0, config.minimumSampleSeconds * 1.2 / seconds);
                iterations = static_cast<std::uint64_t>(std::ceil(static_cast<double>(iterations) * scale));
            }
            for (std::size_t warmup = 0; warmup < config.warmups; ++warmup) {
                body(std::max<std::uint64_t>(1, iterations / 2));
            }
            for (std::size_t sample = 0; sample < config.samples; ++sample) {
                for (std::size_t attempt = 0;; ++attempt) {
                    const auto begin = Clock::now();
                    body(iterations);
                    const double seconds = std::chrono::duration<double>(Clock::now() - begin).count();
                    if (config.minimumSampleSeconds == 0.0 || seconds >= config.minimumSampleSeconds) {
                        writer.WriteRate(config, "ready", "push-pop", implementation, sample, iterations * 2,
                                         seconds);
                        if (config.minimumSampleSeconds != 0.0) {
                            const double scale =
                                std::clamp(config.minimumSampleSeconds * 1.25 / seconds, 0.1, 10.0);
                            iterations = std::max<std::uint64_t>(
                                1, static_cast<std::uint64_t>(std::ceil(iterations * scale)));
                        }
                        break;
                    }
                    if (attempt == 7) {
                        throw std::runtime_error("ready-path sample did not reach the minimum duration");
                    }
                    const double scale =
                        std::max(2.0, config.minimumSampleSeconds * 1.25 / std::max(seconds, 1e-9));
                    iterations =
                        static_cast<std::uint64_t>(std::ceil(static_cast<double>(iterations) * scale));
                }
            }
        };

        runRate("Mashiro storage try", [](std::uint64_t iterations) {
            Storage storage;
            std::uint64_t checksum = 0;
            for (std::uint64_t index = 1; index <= iterations; ++index) {
                T value{.id = index};
                if (!storage.TryPush(value)) {
                    std::terminate();
                }
                DoNotOptimize(storage);
                auto result = storage.TryPop();
                if (!result) {
                    std::terminate();
                }
                DoNotOptimize(storage);
                checksum += result->id;
            }
            if (checksum != iterations * (iterations + 1) / 2) {
                std::terminate();
            }
        });
        runRate("Mashiro AsyncQueue try", [](std::uint64_t iterations) {
            Queue queue;
            std::uint64_t checksum = 0;
            for (std::uint64_t index = 1; index <= iterations; ++index) {
                T value{.id = index};
                if (!queue.TryPush(value)) {
                    std::terminate();
                }
                DoNotOptimize(queue);
                auto result = queue.TryPop();
                if (!result) {
                    std::terminate();
                }
                DoNotOptimize(queue);
                checksum += result->id;
            }
            if (checksum != iterations * (iterations + 1) / 2) {
                std::terminate();
            }
        });
        runRate("Mashiro sender start", [](std::uint64_t iterations) {
            Queue queue;
            auto producer = queue.Producer();
            auto consumer = queue.Consumer();
            std::uint64_t checksum = 0;
            for (std::uint64_t index = 1; index <= iterations; ++index) {
                std::atomic<bool> pushed{};
                auto pushSender = producer.Push(T{.id = index});
                auto pushOperation = stdexec::connect(std::move(pushSender), PushReceiver{.completed = &pushed});
                stdexec::start(pushOperation);
                if (!pushed.load(std::memory_order_acquire)) {
                    std::terminate();
                }

                std::atomic<bool> popped{};
                T value{};
                auto popSender = consumer.Pop();
                auto popOperation =
                    stdexec::connect(std::move(popSender), InlineReceiver<T>{.value = &value, .completed = &popped});
                stdexec::start(popOperation);
                if (!popped.load(std::memory_order_acquire)) {
                    std::terminate();
                }
                checksum += value.id;
            }
            if (checksum != iterations * (iterations + 1) / 2) {
                std::terminate();
            }
        });
    }

    /** @brief oneTBB adapter for latency and bounded backpressure measurements. */
    template<typename T, std::size_t Capacity>
    class TbbLatencyQueue {
    public:
        static constexpr std::size_t capacity = Capacity;

        TbbLatencyQueue() { queue_.set_capacity(Capacity); }

        void Push(T value) { queue_.push(std::move(value)); }

        T Pop() {
            T value{};
            queue_.pop(value);
            return value;
        }

    private:
        oneapi::tbb::concurrent_bounded_queue<T> queue_{};
    };

    /** @brief moodycamel adapter for consumer-wakeup latency; it has no strict producer backpressure. */
    template<typename T, std::size_t Capacity>
    class MoodycamelLatencyQueue {
    public:
        static constexpr std::size_t capacity = Capacity;

        MoodycamelLatencyQueue() : queue_(Capacity) {}

        void Push(T value) { queue_.enqueue(std::move(value)); }

        T Pop() {
            T value{};
            queue_.wait_dequeue(value);
            return value;
        }

    private:
        moodycamel::BlockingConcurrentQueue<T> queue_;
    };

    /** @brief condition-variable adapter for latency and bounded backpressure measurements. */
    template<typename T, std::size_t Capacity>
    class ConditionLatencyQueue {
    public:
        static constexpr std::size_t capacity = Capacity;

        void Push(T value) {
            if (!queue_.Push(std::move(value))) {
                std::terminate();
            }
        }

        T Pop() {
            T value{};
            if (!queue_.Pop(value)) {
                std::terminate();
            }
            return value;
        }

    private:
        ConditionQueue<T, Capacity> queue_{};
    };

    /** @brief Measure Mashiro consumer resume after the pop operation is observably pending. */
    template<typename T>
    [[nodiscard]] LatencySample RunMashiroConsumerResume(const Config& config) {
        using Queue = Concurrency::AsyncQueue<SpscRingBuffer<T, 8>>;
        Queue queue;
        auto producer = queue.Producer();
        auto consumer = queue.Consumer();
        const auto cpuSets = SelectWorkerCpuSets(2, config.pinWorkers);
        std::atomic<std::size_t> ready{};
        std::atomic<std::int64_t> started{};
        LatencySample sample{.nanoseconds = std::vector<std::uint64_t>(config.latencyIterations)};
        std::thread consumerThread{[&] {
            if (!BindWorker(cpuSets, 0)) {
                std::terminate();
            }
            for (std::size_t index = 0; index < config.latencyIterations; ++index) {
                std::atomic<bool> completed{};
                T value{};
                auto sender = consumer.Pop();
                auto operation =
                    stdexec::connect(std::move(sender), InlineReceiver<T>{.value = &value, .completed = &completed});
                stdexec::start(operation);
                ready.store(index + 1, std::memory_order_release);
                ready.notify_one();
                while (!completed.load(std::memory_order_acquire)) {
                    completed.wait(false, std::memory_order_relaxed);
                }
                const auto end = std::chrono::duration_cast<Nanoseconds>(Clock::now().time_since_epoch()).count();
                const auto begin = started.load(std::memory_order_acquire);
                if (value.id != index + 1 || end < begin) {
                    std::terminate();
                }
                sample.nanoseconds[index] = static_cast<std::uint64_t>(end - begin);
            }
        }};
        if (!BindWorker(cpuSets, 1)) {
            std::terminate();
        }
        for (std::size_t index = 0; index < config.latencyIterations; ++index) {
            while (ready.load(std::memory_order_acquire) != index + 1) {
                ready.wait(index, std::memory_order_relaxed);
            }
            SpinDelay(config.parkDelay);
            started.store(std::chrono::duration_cast<Nanoseconds>(Clock::now().time_since_epoch()).count(),
                          std::memory_order_release);
            auto result = stdexec::sync_wait(producer.Push(T{.id = index + 1}));
            if (!result || !std::get<0>(*result).Enqueued()) {
                std::terminate();
            }
        }
        consumerThread.join();
        return sample;
    }

    /** @brief Measure Mashiro producer resume after the push operation is observably pending. */
    template<typename T>
    [[nodiscard]] LatencySample RunMashiroProducerResume(const Config& config) {
        using Queue = Concurrency::AsyncQueue<SpscRingBuffer<T, 8>>;
        Queue queue;
        auto producer = queue.Producer();
        auto consumer = queue.Consumer();
        for (std::size_t index = 0; index < Queue::capacity; ++index) {
            auto result = stdexec::sync_wait(producer.Push(T{.id = index + 1}));
            if (!result || !std::get<0>(*result).Enqueued()) {
                std::terminate();
            }
        }
        const auto cpuSets = SelectWorkerCpuSets(2, config.pinWorkers);
        std::atomic<std::size_t> ready{};
        std::atomic<std::int64_t> started{};
        LatencySample sample{.nanoseconds = std::vector<std::uint64_t>(config.latencyIterations)};
        std::thread producerThread{[&] {
            if (!BindWorker(cpuSets, 1)) {
                std::terminate();
            }
            for (std::size_t index = 0; index < config.latencyIterations; ++index) {
                std::atomic<bool> completed{};
                auto sender = producer.Push(T{.id = Queue::capacity + index + 1});
                auto operation = stdexec::connect(std::move(sender), PushReceiver{.completed = &completed});
                stdexec::start(operation);
                ready.store(index + 1, std::memory_order_release);
                ready.notify_one();
                while (!completed.load(std::memory_order_acquire)) {
                    completed.wait(false, std::memory_order_relaxed);
                }
                const auto end = std::chrono::duration_cast<Nanoseconds>(Clock::now().time_since_epoch()).count();
                const auto begin = started.load(std::memory_order_acquire);
                if (end < begin) {
                    std::terminate();
                }
                sample.nanoseconds[index] = static_cast<std::uint64_t>(end - begin);
            }
        }};
        if (!BindWorker(cpuSets, 0)) {
            std::terminate();
        }
        for (std::size_t index = 0; index < config.latencyIterations; ++index) {
            while (ready.load(std::memory_order_acquire) != index + 1) {
                ready.wait(index, std::memory_order_relaxed);
            }
            SpinDelay(config.parkDelay);
            started.store(std::chrono::duration_cast<Nanoseconds>(Clock::now().time_since_epoch()).count(),
                          std::memory_order_release);
            if (!consumer.TryPop()) {
                std::terminate();
            }
        }
        producerThread.join();
        return sample;
    }

    /** @brief Measure consumer park-to-thread-resume latency for one blocking queue adapter. */
    template<typename Queue, typename T>
    [[nodiscard]] LatencySample RunConsumerResume(const Config& config) {
        Queue queue;
        const auto cpuSets = SelectWorkerCpuSets(2, config.pinWorkers);
        std::atomic<std::size_t> ready{};
        std::atomic<std::int64_t> started{};
        LatencySample sample{.nanoseconds = std::vector<std::uint64_t>(config.latencyIterations)};
        std::thread consumerThread{[&] {
            if (!BindWorker(cpuSets, 0)) {
                std::terminate();
            }
            for (std::size_t index = 0; index < config.latencyIterations; ++index) {
                ready.store(index + 1, std::memory_order_release);
                ready.notify_one();
                const T value = queue.Pop();
                const auto end = std::chrono::duration_cast<Nanoseconds>(Clock::now().time_since_epoch()).count();
                const auto begin = started.load(std::memory_order_acquire);
                if (value.id != index + 1 || end < begin) {
                    std::terminate();
                }
                sample.nanoseconds[index] = static_cast<std::uint64_t>(end - begin);
            }
        }};
        if (!BindWorker(cpuSets, 1)) {
            std::terminate();
        }
        for (std::size_t index = 0; index < config.latencyIterations; ++index) {
            while (ready.load(std::memory_order_acquire) != index + 1) {
                ready.wait(index, std::memory_order_relaxed);
            }
            SpinDelay(config.parkDelay);
            started.store(std::chrono::duration_cast<Nanoseconds>(Clock::now().time_since_epoch()).count(),
                          std::memory_order_release);
            queue.Push(T{.id = index + 1});
        }
        consumerThread.join();
        return sample;
    }

    /** @brief Measure producer backpressure park-to-thread-resume latency for one strictly bounded adapter. */
    template<typename Queue, typename T>
    [[nodiscard]] LatencySample RunProducerResume(const Config& config) {
        Queue queue;
        for (std::size_t index = 0; index < Queue::capacity; ++index) {
            queue.Push(T{.id = index + 1});
        }
        const auto cpuSets = SelectWorkerCpuSets(2, config.pinWorkers);
        std::atomic<std::size_t> ready{};
        std::atomic<std::int64_t> started{};
        LatencySample sample{.nanoseconds = std::vector<std::uint64_t>(config.latencyIterations)};
        std::thread producerThread{[&] {
            if (!BindWorker(cpuSets, 1)) {
                std::terminate();
            }
            for (std::size_t index = 0; index < config.latencyIterations; ++index) {
                ready.store(index + 1, std::memory_order_release);
                ready.notify_one();
                queue.Push(T{.id = Queue::capacity + index + 1});
                const auto end = std::chrono::duration_cast<Nanoseconds>(Clock::now().time_since_epoch()).count();
                const auto begin = started.load(std::memory_order_acquire);
                if (end < begin) {
                    std::terminate();
                }
                sample.nanoseconds[index] = static_cast<std::uint64_t>(end - begin);
            }
        }};
        if (!BindWorker(cpuSets, 0)) {
            std::terminate();
        }
        for (std::size_t index = 0; index < config.latencyIterations; ++index) {
            while (ready.load(std::memory_order_acquire) != index + 1) {
                ready.wait(index, std::memory_order_relaxed);
            }
            SpinDelay(config.parkDelay);
            started.store(std::chrono::duration_cast<Nanoseconds>(Clock::now().time_since_epoch()).count(),
                          std::memory_order_release);
            static_cast<void>(queue.Pop());
        }
        producerThread.join();
        return sample;
    }

    /** @brief Measure synchronous stop-callback delivery for a parked AsyncQueue pop. */
    template<typename T>
    [[nodiscard]] LatencySample RunMashiroCancellation(const Config& config) {
        using Queue = Concurrency::AsyncQueue<SpscRingBuffer<T, 8>>;
        Queue queue;
        auto consumer = queue.Consumer();
        LatencySample sample{.nanoseconds = std::vector<std::uint64_t>(config.latencyIterations)};
        for (std::size_t index = 0; index < config.latencyIterations; ++index) {
            stdexec::inplace_stop_source stopSource;
            std::atomic<bool> completed{};
            T value{};
            auto sender = consumer.Pop();
            auto operation = stdexec::connect(
                std::move(sender), InlineReceiver<T>{.value = &value, .completed = &completed,
                                                     .token = stopSource.get_token()});
            stdexec::start(operation);
            const auto begin = Clock::now();
            stopSource.request_stop();
            const auto end = Clock::now();
            if (!completed.load(std::memory_order_acquire)) {
                std::terminate();
            }
            sample.nanoseconds[index] =
                static_cast<std::uint64_t>(std::chrono::duration_cast<Nanoseconds>(end - begin).count());
        }
        return sample;
    }

    /** @brief Run repeated latency distributions and serialize their per-sample percentiles. */
    template<typename Runner>
    void RunLatencyCase(const Config& config, CsvWriter& writer, std::string_view category, std::string_view scenario,
                        std::string_view implementation, Runner&& runner) {
        std::cout << category << " | " << scenario << " | " << implementation << '\n';
        for (std::size_t warmup = 0; warmup < config.warmups; ++warmup) {
            static_cast<void>(runner(config));
        }
        for (std::size_t sample = 0; sample < config.samples; ++sample) {
            writer.WriteLatency(config, category, scenario, implementation, sample, runner(config));
        }
    }

    /** @brief Mashiro MPMC AsyncQueue adapter for abort fan-out measurements. */
    template<typename T>
    class MashiroAbortQueue {
    public:
        void Wait() {
            auto consumer = queue_.Consumer();
            std::atomic<bool> completed{};
            T value{};
            auto sender = consumer.Pop();
            auto operation =
                stdexec::connect(std::move(sender), InlineReceiver<T>{.value = &value, .completed = &completed});
            stdexec::start(operation);
            while (!completed.load(std::memory_order_acquire)) {
                completed.wait(false, std::memory_order_relaxed);
            }
        }

        void Abort() noexcept { queue_.Abort(); }

    private:
        Concurrency::AsyncQueue<MpmcQueue<T, 1024>> queue_{};
    };

    /** @brief oneTBB adapter for abort fan-out measurements. */
    template<typename T>
    class TbbAbortQueue {
    public:
        TbbAbortQueue() { queue_.set_capacity(1024); }

        void Wait() {
            T value{};
            try {
                queue_.pop(value);
            } catch (const oneapi::tbb::user_abort&) {
            }
        }

        void Abort() noexcept { queue_.abort(); }

    private:
        oneapi::tbb::concurrent_bounded_queue<T> queue_{};
    };

    /** @brief condition-variable adapter for abort fan-out measurements. */
    template<typename T>
    class ConditionAbortQueue {
    public:
        void Wait() {
            T value{};
            static_cast<void>(queue_.Pop(value));
        }

        void Abort() noexcept { queue_.Abort(); }

    private:
        ConditionQueue<T, 1024> queue_{};
    };

    /** @brief Measure the time from abort initiation until every parked waiter returns. */
    template<typename Queue>
    [[nodiscard]] LatencySample RunAbortFanout(const Config& config, std::size_t waiterCount) {
        Queue queue;
        const auto cpuSets = SelectWorkerCpuSets(waiterCount, config.pinWorkers);
        std::atomic<std::size_t> ready{};
        std::latch done{static_cast<std::ptrdiff_t>(waiterCount)};
        std::vector<std::thread> waiters;
        waiters.reserve(waiterCount);
        for (std::size_t index = 0; index < waiterCount; ++index) {
            waiters.emplace_back([&, index] {
                if (!BindWorker(cpuSets, index)) {
                    std::terminate();
                }
                ready.fetch_add(1, std::memory_order_release);
                ready.notify_one();
                queue.Wait();
                done.count_down();
            });
        }
        while (ready.load(std::memory_order_acquire) != waiterCount) {
            ready.wait(ready.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        SpinDelay(config.parkDelay);
        const auto begin = Clock::now();
        queue.Abort();
        done.wait();
        const auto end = Clock::now();
        for (auto& waiter : waiters) {
            waiter.join();
        }
        return LatencySample{.nanoseconds = {
                                 static_cast<std::uint64_t>(
                                     std::chrono::duration_cast<Nanoseconds>(end - begin).count()),
                             }};
    }

    /** @brief Measure AsyncQueue abort-to-receiver delivery without introducing a thread terminal. */
    template<typename T>
    [[nodiscard]] LatencySample RunAbortCallbackFanout(const Config&, std::size_t waiterCount) {
        using Queue = Concurrency::AsyncQueue<MpmcQueue<T, 1024>>;
        Queue queue;
        auto consumer = queue.Consumer();
        std::atomic<std::size_t> completions{};
        using Sender = decltype(consumer.Pop());
        using Operation =
            decltype(stdexec::connect(std::declval<Sender>(), std::declval<FanoutReceiver<T>>()));
        std::vector<std::unique_ptr<Operation>> operations;
        operations.reserve(waiterCount);
        for (std::size_t index = 0; index < waiterCount; ++index) {
            auto sender = consumer.Pop();
            operations.emplace_back(
                new Operation(stdexec::connect(std::move(sender), FanoutReceiver<T>{.completions = &completions})));
            stdexec::start(*operations.back());
        }
        const auto begin = Clock::now();
        queue.Abort();
        const auto end = Clock::now();
        if (completions.load(std::memory_order_acquire) != waiterCount) {
            std::terminate();
        }
        return LatencySample{.nanoseconds = {
                                 static_cast<std::uint64_t>(
                                     std::chrono::duration_cast<Nanoseconds>(end - begin).count()),
                             }};
    }

    /** @brief Measure pending-pop to inline receiver callback delivery without an OS thread wake. */
    template<typename T>
    [[nodiscard]] LatencySample RunMashiroCallbackDelivery(const Config& config) {
        using Queue = Concurrency::AsyncQueue<SpscRingBuffer<T, 8>>;
        Queue queue;
        auto producer = queue.Producer();
        auto consumer = queue.Consumer();
        LatencySample sample{.nanoseconds = std::vector<std::uint64_t>(config.latencyIterations)};
        for (std::size_t index = 0; index < config.latencyIterations; ++index) {
            std::atomic<bool> completed{};
            T value{};
            auto popSender = consumer.Pop();
            auto popOperation =
                stdexec::connect(std::move(popSender), InlineReceiver<T>{.value = &value, .completed = &completed});
            stdexec::start(popOperation);
            const auto begin = Clock::now();
            auto result = stdexec::sync_wait(producer.Push(T{.id = index + 1}));
            const auto end = Clock::now();
            if (!result || !completed.load(std::memory_order_acquire) || value.id != index + 1) {
                std::terminate();
            }
            sample.nanoseconds[index] =
                static_cast<std::uint64_t>(std::chrono::duration_cast<Nanoseconds>(end - begin).count());
        }
        return sample;
    }

    /** @brief Record queue, sender, and connected-operation storage footprints. */
    void WriteSizes(const Config& config, CsvWriter& writer) {
        using Storage = SpscRingBuffer<Payload<8>, 1024>;
        using Queue = Concurrency::AsyncQueue<Storage>;
        using Producer = Concurrency::ProducerPort<Queue>;
        using Consumer = Concurrency::ConsumerPort<Queue>;
        using PushSender = decltype(std::declval<Producer&>().Push(Payload<8>{}));
        using PopSender = decltype(std::declval<Consumer&>().Pop());
        using PushOperation = decltype(stdexec::connect(std::declval<PushSender>(), std::declval<PushReceiver>()));
        using PopOperation =
            decltype(stdexec::connect(std::declval<PopSender>(), std::declval<InlineReceiver<Payload<8>>>()));
        writer.WriteSize(config, "queue", "Mashiro AsyncQueue", sizeof(Queue));
        writer.WriteSize(config, "producer-port", "Mashiro AsyncQueue", sizeof(Producer));
        writer.WriteSize(config, "consumer-port", "Mashiro AsyncQueue", sizeof(Consumer));
        writer.WriteSize(config, "push-sender", "Mashiro AsyncQueue", sizeof(PushSender));
        writer.WriteSize(config, "pop-sender", "Mashiro AsyncQueue", sizeof(PopSender));
        writer.WriteSize(config, "push-operation", "Mashiro AsyncQueue", sizeof(PushOperation));
        writer.WriteSize(config, "pop-operation", "Mashiro AsyncQueue", sizeof(PopOperation));
        writer.WriteSize(config, "queue", "oneTBB bounded",
                         sizeof(oneapi::tbb::concurrent_bounded_queue<Payload<8>>));
        writer.WriteSize(config, "queue", "moodycamel blocking",
                         sizeof(moodycamel::BlockingConcurrentQueue<Payload<8>>));
        writer.WriteSize(config, "queue", "condition variable", sizeof(ConditionQueue<Payload<8>, 1024>));
    }

    /** @brief Parse one unsigned command-line value. */
    [[nodiscard]] std::size_t ParseSize(std::string_view text, std::string_view option) {
        try {
            std::size_t consumed = 0;
            const auto value = std::stoull(std::string{text}, &consumed);
            if (consumed != text.size()) {
                throw std::invalid_argument("trailing characters");
            }
            return value;
        } catch (const std::exception&) {
            throw std::invalid_argument(std::string{option} + " requires an unsigned integer");
        }
    }

    /** @brief Parse benchmark command-line options. */
    [[nodiscard]] Config ParseConfig(int argc, char** argv) {
        Config config;
        for (int index = 1; index < argc; ++index) {
            const std::string_view option = argv[index];
            const auto requireValue = [&]() -> std::string_view {
                if (++index == argc) {
                    throw std::invalid_argument(std::string{option} + " requires a value");
                }
                return argv[index];
            };
            if (option == "--output") {
                config.output = requireValue();
            } else if (option == "--label") {
                config.label = requireValue();
            } else if (option == "--section") {
                config.section = requireValue();
            } else if (option == "--samples") {
                config.samples = ParseSize(requireValue(), option);
            } else if (option == "--warmups") {
                config.warmups = ParseSize(requireValue(), option);
            } else if (option == "--items") {
                config.itemsPerProducer = ParseSize(requireValue(), option);
            } else if (option == "--latency-iterations") {
                config.latencyIterations = ParseSize(requireValue(), option);
            } else if (option == "--minimum-ms") {
                config.minimumSampleSeconds = static_cast<double>(ParseSize(requireValue(), option)) / 1'000.0;
            } else if (option == "--park-delay-us") {
                config.parkDelay = std::chrono::microseconds{ParseSize(requireValue(), option)};
            } else if (option == "--no-pin") {
                config.pinWorkers = false;
            } else if (option == "--quick") {
                config.samples = 2;
                config.warmups = 1;
                config.itemsPerProducer = 2'000;
                config.latencyIterations = 200;
                config.minimumSampleSeconds = 0.02;
            } else {
                throw std::invalid_argument("unknown option: " + std::string{option});
            }
        }
        if (config.samples == 0 || config.latencyIterations == 0 || config.itemsPerProducer == 0) {
            throw std::invalid_argument("sample and iteration counts must be non-zero");
        }
        constexpr std::array sections{"all", "ready", "throughput", "latency", "fanout"};
        if (std::ranges::find(sections, config.section) == sections.end()) {
            throw std::invalid_argument("--section requires all, ready, throughput, latency, or fanout");
        }
        return config;
    }

    /** @brief Run the complete semantic-layered benchmark matrix. */
    void Run(const Config& config) {
        CsvWriter writer{config};
        const auto selected = [&](std::string_view section) {
            return config.section == "all" || config.section == section;
        };
        if (config.section == "all") {
            WriteSizes(config, writer);
        }
        if (selected("ready")) {
            RunReadyPath<Payload<8>, 1024>(config, writer);
        }

        const auto throughput = [&]<typename T, std::size_t Capacity>(std::size_t producers, std::size_t consumers,
                                                                      std::string_view scenario) {
            if (producers == 1 && consumers == 1) {
                using Storage = SpscRingBuffer<T, Capacity>;
                RunThroughputCase(config, writer, scenario, "Mashiro AsyncQueue", sizeof(T), Capacity, producers,
                                  consumers, [&](std::size_t items) {
                                      return RunMashiroThroughput<Storage, T>(producers, consumers, items,
                                                                             config.pinWorkers);
                                  });
            } else if (consumers == 1) {
                using Storage = MpscQueue<T, Capacity>;
                RunThroughputCase(config, writer, scenario, "Mashiro AsyncQueue", sizeof(T), Capacity, producers,
                                  consumers, [&](std::size_t items) {
                                      return RunMashiroThroughput<Storage, T>(producers, consumers, items,
                                                                             config.pinWorkers);
                                  });
            } else {
                using Storage = MpmcQueue<T, Capacity>;
                RunThroughputCase(config, writer, scenario, "Mashiro AsyncQueue", sizeof(T), Capacity, producers,
                                  consumers, [&](std::size_t items) {
                                      return RunMashiroThroughput<Storage, T>(producers, consumers, items,
                                                                             config.pinWorkers);
                                  });
            }
            RunThroughputCase(config, writer, scenario, "oneTBB bounded", sizeof(T), Capacity, producers, consumers,
                              [&](std::size_t items) {
                                  return RunExternalThroughput<TbbBlockingQueue<T, Capacity>, T>(
                                      producers, consumers, items, config.pinWorkers);
                              });
            RunThroughputCase(config, writer, scenario, "moodycamel blocking", sizeof(T), Capacity, producers,
                              consumers, [&](std::size_t items) {
                                  return RunExternalThroughput<MoodycamelBlockingQueue<T, Capacity>, T>(
                                      producers, consumers, items, config.pinWorkers);
                              });
            RunThroughputCase(config, writer, scenario, "condition variable", sizeof(T), Capacity, producers,
                              consumers, [&](std::size_t items) {
                                  return RunExternalThroughput<ConditionBlockingQueue<T, Capacity>, T>(
                                      producers, consumers, items, config.pinWorkers);
                              });
        };

        if (selected("throughput")) {
            throughput.template operator()<Payload<8>, 1024>(1, 1, "SPSC");
            throughput.template operator()<Payload<8>, 1024>(4, 1, "MPSC");
            throughput.template operator()<Payload<8>, 1024>(4, 4, "MPMC");
            throughput.template operator()<Payload<64>, 1024>(1, 1, "SPSC");
            throughput.template operator()<Payload<64>, 1024>(4, 1, "MPSC");
            throughput.template operator()<Payload<64>, 1024>(4, 4, "MPMC");
        }

        using T = Payload<8>;
        if (selected("latency")) {
            RunLatencyCase(config, writer, "consumer-resume", "parked-pop", "Mashiro AsyncQueue",
                           RunMashiroConsumerResume<T>);
            RunLatencyCase(config, writer, "consumer-resume", "parked-pop", "oneTBB bounded",
                           RunConsumerResume<TbbLatencyQueue<T, 8>, T>);
            RunLatencyCase(config, writer, "consumer-resume", "parked-pop", "moodycamel blocking",
                           RunConsumerResume<MoodycamelLatencyQueue<T, 8>, T>);
            RunLatencyCase(config, writer, "consumer-resume", "parked-pop", "condition variable",
                           RunConsumerResume<ConditionLatencyQueue<T, 8>, T>);
            RunLatencyCase(config, writer, "producer-resume", "full-queue", "Mashiro AsyncQueue",
                           RunMashiroProducerResume<T>);
            RunLatencyCase(config, writer, "producer-resume", "full-queue", "oneTBB bounded",
                           RunProducerResume<TbbLatencyQueue<T, 8>, T>);
            RunLatencyCase(config, writer, "producer-resume", "full-queue", "condition variable",
                           RunProducerResume<ConditionLatencyQueue<T, 8>, T>);
            RunLatencyCase(config, writer, "callback-delivery", "parked-pop", "Mashiro AsyncQueue",
                           RunMashiroCallbackDelivery<T>);
            RunLatencyCase(config, writer, "cancellation", "parked-pop", "Mashiro AsyncQueue",
                           RunMashiroCancellation<T>);
        }

        if (selected("fanout")) {
            const std::array<std::size_t, 5> waiterCounts{1, 2, 4, 8, 16};
            for (const std::size_t waiters : waiterCounts) {
                if (config.pinWorkers && AvailableCpuSets().size() < waiters) {
                    std::cout << "abort-fanout | waiters-" << waiters << " | skipped: insufficient CPU sets\n";
                    continue;
                }
                const std::string scenario = "waiters-" + std::to_string(waiters);
                RunLatencyCase(config, writer, "abort-callback-fanout", scenario, "Mashiro AsyncQueue",
                               [&](const Config& current) {
                                   return RunAbortCallbackFanout<T>(current, waiters);
                               });
                RunLatencyCase(config, writer, "abort-thread-fanout", scenario, "Mashiro + atomic wait",
                               [&](const Config& current) {
                                   return RunAbortFanout<MashiroAbortQueue<T>>(current, waiters);
                               });
                RunLatencyCase(config, writer, "abort-thread-fanout", scenario, "oneTBB bounded",
                               [&](const Config& current) {
                                   return RunAbortFanout<TbbAbortQueue<T>>(current, waiters);
                               });
                RunLatencyCase(config, writer, "abort-thread-fanout", scenario, "condition variable",
                               [&](const Config& current) {
                                   return RunAbortFanout<ConditionAbortQueue<T>>(current, waiters);
                               });
            }
        }
    }

} // namespace Mashiro::Benchmark

int main(int argc, char** argv) {
    try {
        std::cout << std::unitbuf;
        const auto config = Mashiro::Benchmark::ParseConfig(argc, argv);
        Mashiro::Benchmark::Run(config);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AsyncQueue benchmark failed: " << error.what() << '\n';
        return 1;
    }
}

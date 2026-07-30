/**
 * @file QueueBenchmark.cpp
 * @brief Reproducible multi-sample throughput benchmark for Mashiro bounded queues and established baselines.
 * @ingroup Core
 */

#include <Mashiro/Core/RingBuffer.h>
#include <Mashiro/Core/SpscChannel.h>

#include <concurrentqueue.h>
#include <oneapi/tbb/concurrent_queue.h>
#include <rigtorp/MPMCQueue.h>
#include <rigtorp/SPSCQueue.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
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

    /** @brief Benchmark configuration parsed from the command line. */
    struct Config {
        std::filesystem::path output = "queue-results.csv";
        std::string label = "baseline";
        std::size_t samples = 15;
        std::size_t warmups = 2;
        std::size_t itemsPerProducer = 200'000;
        double minimumSampleSeconds = 0.2;
        std::uint64_t seed = 0x534F52414D415348ULL;
        bool quick = false;
        bool pinWorkers = true;
        bool paired = false;
        bool layouts = false;
        bool references = false;
        bool spscAudit = false;
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

    /** @brief Per-thread counters kept on separate cache lines to avoid benchmark-induced false sharing. */
    struct alignas(64) WorkerResult {
        std::uint64_t transferred{};
        std::uint64_t checksum{};
        std::uint64_t failures{};
    };

    /** @brief One independently measured benchmark sample. */
    struct Sample {
        double seconds{};
        std::uint64_t transferred{};
        std::uint64_t pushFailures{};
        std::uint64_t popFailures{};
    };

    /** @brief Prevent an unsuccessful non-blocking queue operation from monopolizing the execution pipeline. */
    inline void Relax() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
        _mm_pause();
#else
        std::this_thread::yield();
#endif
    }

    /** @brief Return CPU-set IDs ordered by performance class, physical core, and then SMT sibling. */
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
        for (std::size_t index = 0; index < cpuSets.size(); ++index) {
            if (index != 0 && cpuSets[index].group == cpuSets[index - 1].group &&
                cpuSets[index].core == cpuSets[index - 1].core) {
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
            throw std::runtime_error("not enough available CPU sets to pin every queue benchmark worker");
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
        return SetThreadSelectedCpuSets(GetCurrentThread(), &cpuSet, 1) != FALSE &&
               SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != FALSE;
#else
        static_cast<void>(workerIndex);
        return true;
#endif
    }

    /** @brief Reduce scheduler preemption noise for pinned strict-FIFO queue measurements. */
    [[nodiscard]] bool ConfigureBenchmarkProcess(bool enabled) noexcept {
        if (!enabled) {
            return true;
        }
#if defined(_WIN32)
        return SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS) != FALSE;
#else
        return true;
#endif
    }

    /** @brief Mashiro SPSC storage adapter. */
    template<typename T, std::size_t Capacity>
    class MashiroSpsc {
    public:
        static constexpr std::string_view name = "Mashiro SPSC";
        using Storage = RingBuffer<T, Capacity, Concurrency::QueueTopologies::Spsc>;
        static constexpr std::size_t padding = (Platform::kCacheLineSize - 1) / sizeof(T) + 1;
        static constexpr std::size_t storageBytes = sizeof(Storage) + (Capacity + 1 + 2 * padding) * sizeof(T);

        [[nodiscard]] bool Push(const T& value) noexcept { return queue_.TryPush(value); }
        [[nodiscard]] bool Pop(T& value) noexcept {
            T* front = queue_.Front();
            if (front == nullptr) {
                return false;
            }
            value = std::move(*front);
            queue_.PopFront();
            return true;
        }

    private:
        Storage queue_{};
    };

    /** @brief Mashiro SPSC channel adapter, including its atomic notification-plane cost. */
    template<typename T, std::size_t Capacity>
    class MashiroSpscChannel {
    public:
        static constexpr std::string_view name = "Mashiro SPSC channel";
        static constexpr std::size_t storageBytes = sizeof(SpscChannel<T, Capacity>);

        [[nodiscard]] bool Push(const T& value) noexcept { return queue_.TryPush(value); }
        [[nodiscard]] bool Pop(T& value) noexcept { return queue_.TryPop(value); }

    private:
        SpscChannel<T, Capacity> queue_{};
    };

    /** @brief Pre-optimization SPSC channel with one bidirectionally contended wake sequence. */
    template<typename T, std::size_t Capacity>
    class SharedEpochSpscChannel {
    public:
        static constexpr std::string_view name = "shared-epoch SPSC channel";
        using Storage = RingBuffer<T, Capacity, Concurrency::QueueTopologies::Spsc>;
        static constexpr std::size_t storageBytes = sizeof(Storage) + Platform::kCacheLineSize;

        [[nodiscard]] bool Push(const T& value) noexcept {
            if (!queue_.TryPush(value)) {
                return false;
            }
            Signal();
            return true;
        }

        [[nodiscard]] bool Pop(T& value) noexcept {
            if (!queue_.TryPop(value)) {
                return false;
            }
            Signal();
            return true;
        }

    private:
        void Signal() noexcept {
            epoch_.fetch_add(1, std::memory_order_release);
            epoch_.notify_one();
        }

        Storage queue_{};
        alignas(Platform::kCacheLineSize) std::atomic<std::uint64_t> epoch_{0};
    };

    /** @brief Mashiro MPSC storage adapter. */
    template<typename T, std::size_t Capacity>
    class MashiroMpsc {
    public:
        static constexpr std::string_view name = "Mashiro MPSC";
        using Storage = RingBuffer<T, Capacity, Concurrency::QueueTopologies::Mpsc>;
        static constexpr std::size_t storageBytes = sizeof(Storage);

        [[nodiscard]] bool Push(const T& value) noexcept { return queue_.TryPush(value); }
        [[nodiscard]] bool Pop(T& value) noexcept { return queue_.TryPop(value); }

    private:
        Storage queue_{};
    };

    /** @brief Mashiro SPMC storage adapter. */
    template<typename T, std::size_t Capacity>
    class MashiroSpmc {
    public:
        static constexpr std::string_view name = "Mashiro SPMC";
        using Storage = RingBuffer<T, Capacity, Concurrency::QueueTopologies::Spmc>;
        static constexpr std::size_t storageBytes = sizeof(Storage);

        [[nodiscard]] bool Push(const T& value) noexcept { return queue_.TryPush(value); }
        [[nodiscard]] bool Pop(T& value) noexcept { return queue_.TryPop(value); }

    private:
        Storage queue_{};
    };

    /** @brief Adapter for one compile-time sequence-cell layout candidate. */
    template<typename T, std::size_t Capacity, Concurrency::QueueTopology Topology, bool IsolateCells>
    class MashiroSequenceLayout {
    public:
        static constexpr std::string_view name = IsolateCells ? "isolated cells" : "compact cells";
        using Storage = Detail::SequenceCellRingBuffer<T, Capacity, Topology, IsolateCells>;
        static constexpr std::size_t storageBytes = sizeof(Storage);

        [[nodiscard]] bool Push(const T& value) noexcept { return queue_.TryPush(value); }
        [[nodiscard]] bool Pop(T& value) noexcept { return queue_.TryPop(value); }

    private:
        Storage queue_{};
    };

    /** @brief MPSC adapter retaining the value-returning optional pop path for paired measurements. */
    template<typename T, std::size_t Capacity>
    class MashiroMpscOptional {
    public:
        static constexpr std::string_view name = "Mashiro MPSC optional";
        using Storage = RingBuffer<T, Capacity, Concurrency::QueueTopologies::Mpsc>;
        static constexpr std::size_t storageBytes = sizeof(Storage);

        [[nodiscard]] bool Push(const T& value) noexcept { return queue_.TryPush(value); }
        [[nodiscard]] bool Pop(T& value) noexcept {
            auto result = queue_.TryPop();
            if (!result) {
                return false;
            }
            value = std::move(*result);
            return true;
        }

    private:
        Storage queue_{};
    };

    /** @brief Mashiro MPMC storage adapter. */
    template<typename T, std::size_t Capacity>
    class MashiroMpmc {
    public:
        static constexpr std::string_view name = "Mashiro MPMC";
        using Storage = RingBuffer<T, Capacity, Concurrency::QueueTopologies::Mpmc>;
        static constexpr std::size_t storageBytes = sizeof(Storage);

        [[nodiscard]] bool Push(const T& value) noexcept { return queue_.TryPush(value); }
        [[nodiscard]] bool Pop(T& value) noexcept { return queue_.TryPop(value); }

    private:
        Storage queue_{};
    };

    /** @brief MPMC adapter retaining the value-returning optional pop path for paired measurements. */
    template<typename T, std::size_t Capacity>
    class MashiroMpmcOptional {
    public:
        static constexpr std::string_view name = "Mashiro MPMC optional";
        using Storage = RingBuffer<T, Capacity, Concurrency::QueueTopologies::Mpmc>;
        static constexpr std::size_t storageBytes = sizeof(Storage);

        [[nodiscard]] bool Push(const T& value) noexcept { return queue_.TryPush(value); }
        [[nodiscard]] bool Pop(T& value) noexcept {
            auto result = queue_.TryPop();
            if (!result) {
                return false;
            }
            value = std::move(*result);
            return true;
        }

    private:
        Storage queue_{};
    };

    /** @brief Rigtorp's specialized bounded SPSC queue adapter. */
    template<typename T, std::size_t Capacity>
    class RigtorpSpsc {
    public:
        static constexpr std::string_view name = "Rigtorp SPSC";
        static constexpr std::size_t padding = (Platform::kCacheLineSize - 1) / sizeof(T) + 1;
        static constexpr std::size_t storageBytes =
            sizeof(rigtorp::SPSCQueue<T>) + (Capacity + 1 + 2 * padding) * sizeof(T);

        RigtorpSpsc() : queue_(Capacity) {
            if (queue_.capacity() != Capacity) {
                throw std::runtime_error("Rigtorp SPSC allocator changed the requested capacity");
            }
        }

        [[nodiscard]] bool Push(const T& value) noexcept { return queue_.try_push(value); }
        [[nodiscard]] bool Pop(T& value) noexcept {
            T* front = queue_.front();
            if (front == nullptr) {
                return false;
            }
            value = std::move(*front);
            queue_.pop();
            return true;
        }

    private:
        rigtorp::SPSCQueue<T> queue_;
    };

    /** @brief Give one queue implementation an independent benchmark identity without changing its operations. */
    template<typename Queue, typename Tag>
    class NamedQueue : public Queue {
    public:
        using Queue::Queue;
        static constexpr std::string_view name = Tag::name;
        static constexpr std::size_t storageBytes = Queue::storageBytes;
    };

    struct MashiroSpscA {
        static constexpr std::string_view name = "Mashiro SPSC A";
    };
    struct MashiroSpscB {
        static constexpr std::string_view name = "Mashiro SPSC B";
    };
    struct RigtorpSpscA {
        static constexpr std::string_view name = "Rigtorp SPSC A";
    };
    struct RigtorpSpscB {
        static constexpr std::string_view name = "Rigtorp SPSC B";
    };

    /** @brief Rigtorp's bounded strict-FIFO MPMC queue adapter. */
    template<typename T, std::size_t Capacity>
    class RigtorpMpmc {
    public:
        static constexpr std::string_view name = "Rigtorp MPMC";
        static constexpr std::size_t storageBytes =
            sizeof(rigtorp::MPMCQueue<T>) + (Capacity + 1) * sizeof(rigtorp::mpmc::Slot<T>);

        RigtorpMpmc() : queue_(Capacity) {}

        [[nodiscard]] bool Push(const T& value) noexcept { return queue_.try_push(value); }
        [[nodiscard]] bool Pop(T& value) noexcept { return queue_.try_pop(value); }

    private:
        rigtorp::MPMCQueue<T> queue_;
    };

    /** @brief Official moodycamel ConcurrentQueue adapter with preallocated initial capacity. */
    template<typename T, std::size_t Capacity>
    class MoodycamelQueue {
    public:
        static constexpr std::string_view name = "moodycamel";
        static constexpr std::size_t storageBytes = 0;

        MoodycamelQueue() : queue_(Capacity) {}

        [[nodiscard]] bool Push(const T& value) noexcept { return queue_.try_enqueue(value); }
        [[nodiscard]] bool Pop(T& value) noexcept { return queue_.try_dequeue(value); }

    private:
        moodycamel::ConcurrentQueue<T> queue_;
    };

    /** @brief oneTBB bounded concurrent queue adapter. */
    template<typename T, std::size_t Capacity>
    class TbbQueue {
    public:
        static constexpr std::string_view name = "oneTBB bounded";
        static constexpr std::size_t storageBytes = 0;

        TbbQueue() { queue_.set_capacity(Capacity); }

        [[nodiscard]] bool Push(const T& value) { return queue_.try_push(value); }
        [[nodiscard]] bool Pop(T& value) { return queue_.try_pop(value); }

    private:
        oneapi::tbb::concurrent_bounded_queue<T> queue_{};
    };

    /** @brief Bounded mutex/deque reference adapter. */
    template<typename T, std::size_t Capacity>
    class MutexQueue {
    public:
        static constexpr std::string_view name = "mutex deque";
        static constexpr std::size_t storageBytes = 0;

        [[nodiscard]] bool Push(const T& value) {
            std::scoped_lock lock{mutex_};
            if (queue_.size() == Capacity) {
                return false;
            }
            queue_.push_back(value);
            return true;
        }

        [[nodiscard]] bool Pop(T& value) {
            std::scoped_lock lock{mutex_};
            if (queue_.empty()) {
                return false;
            }
            value = std::move(queue_.front());
            queue_.pop_front();
            return true;
        }

    private:
        std::mutex mutex_{};
        std::deque<T> queue_{};
    };

    /** @brief Run one producer/consumer topology and validate every transferred identity. */
    template<typename Queue, typename T>
    [[nodiscard]] Sample RunSample(std::size_t producers, std::size_t consumers, std::size_t itemsPerProducer,
                                   bool pinWorkers) {
        auto queue = std::make_unique<Queue>();
        const std::size_t workerCount = producers + consumers;
        const std::vector<std::uint32_t> cpuSets = SelectWorkerCpuSets(workerCount, pinWorkers);
        std::barrier start{static_cast<std::ptrdiff_t>(workerCount + 1)};
        std::latch done{static_cast<std::ptrdiff_t>(workerCount)};
        std::atomic<std::size_t> activeProducers{producers};
        std::atomic<bool> placementFailed{false};
        std::vector<WorkerResult> producerResults(producers);
        std::vector<WorkerResult> consumerResults(consumers);
        std::vector<std::thread> workers;
        workers.reserve(workerCount);

        for (std::size_t producer = 0; producer < producers; ++producer) {
            workers.emplace_back([&, producer] [[gnu::noinline, gnu::aligned(Platform::kCacheLineSize)]] {
                if (!BindWorker(cpuSets, consumers + producer)) {
                    placementFailed.store(true, std::memory_order_relaxed);
                }
                start.arrive_and_wait();
                WorkerResult result{};
                const std::uint64_t first = producer * itemsPerProducer + 1;
                for (std::size_t index = 0; index < itemsPerProducer; ++index) {
                    const T value{.id = first + index};
                    while (!queue->Push(value)) {
                        ++result.failures;
                        Relax();
                    }
                    ++result.transferred;
                }
                producerResults[producer] = result;
                activeProducers.fetch_sub(1, std::memory_order_release);
                done.count_down();
            });
        }

        for (std::size_t consumer = 0; consumer < consumers; ++consumer) {
            workers.emplace_back([&, consumer] [[gnu::noinline, gnu::aligned(Platform::kCacheLineSize)]] {
                if (!BindWorker(cpuSets, consumer)) {
                    placementFailed.store(true, std::memory_order_relaxed);
                }
                start.arrive_and_wait();
                WorkerResult result{};
                T value{};
                for (;;) {
                    if (queue->Pop(value)) {
                        ++result.transferred;
                        result.checksum += value.id;
                        continue;
                    }
                    ++result.failures;
                    if (activeProducers.load(std::memory_order_acquire) == 0) {
                        if (!queue->Pop(value)) {
                            break;
                        }
                        ++result.transferred;
                        result.checksum += value.id;
                        continue;
                    }
                    Relax();
                }
                consumerResults[consumer] = result;
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
        if (placementFailed.load(std::memory_order_relaxed)) {
            throw std::runtime_error("failed to bind a queue benchmark worker to its selected CPU set");
        }

        const std::uint64_t expectedCount = producers * itemsPerProducer;
        const std::uint64_t expectedChecksum = expectedCount * (expectedCount + 1) / 2;
        const std::uint64_t consumed = std::ranges::fold_left(
            consumerResults, std::uint64_t{}, [](std::uint64_t sum, const WorkerResult& item) {
                return sum + item.transferred;
            });
        const std::uint64_t checksum = std::ranges::fold_left(
            consumerResults, std::uint64_t{}, [](std::uint64_t sum, const WorkerResult& item) {
                return sum + item.checksum;
            });
        if (consumed != expectedCount || checksum != expectedChecksum) {
            throw std::runtime_error("queue benchmark integrity check failed");
        }

        return {
            .seconds = std::chrono::duration<double>(end - begin).count(),
            .transferred = consumed,
            .pushFailures = std::ranges::fold_left(
                producerResults, std::uint64_t{},
                [](std::uint64_t sum, const WorkerResult& item) { return sum + item.failures; }),
            .popFailures = std::ranges::fold_left(
                consumerResults, std::uint64_t{},
                [](std::uint64_t sum, const WorkerResult& item) { return sum + item.failures; }),
        };
    }

    /** @brief Own the CSV stream and serialize one row per independent sample. */
    class CsvWriter {
    public:
        explicit CsvWriter(const Config& config) : stream_(config.output) {
            if (!stream_) {
                throw std::runtime_error("cannot open benchmark output");
            }
            stream_ << "label,scenario,queue,payload_bytes,capacity,producers,consumers,sample,transfers,seconds,"
                       "million_transfers_per_second,push_failures,pop_failures,queue_bytes,thread_placement,"
                       "minimum_sample_ms\n";
        }

        template<typename Queue>
        void Write(const Config& config, std::string_view scenario, std::size_t payloadBytes, std::size_t capacity,
                   std::size_t producers, std::size_t consumers, std::size_t sampleIndex, const Sample& sample) {
            const double throughput = static_cast<double>(sample.transferred) / sample.seconds / 1'000'000.0;
            stream_ << config.label << ',' << scenario << ',' << Queue::name << ',' << payloadBytes << ',' << capacity
                    << ',' << producers << ',' << consumers << ',' << sampleIndex << ',' << sample.transferred << ','
                    << sample.seconds << ',' << throughput << ',' << sample.pushFailures << ',' << sample.popFailures
                    << ',' << Queue::storageBytes << ','
                    << (config.pinWorkers ? "cpu-set-high-priority" : "scheduler") << ','
                    << config.minimumSampleSeconds * 1'000.0 << '\n';
        }

    private:
        std::ofstream stream_;
    };

    /** @brief Calibrate one specialization until a pilot reaches the configured minimum duration. */
    template<typename Queue, typename T>
    [[nodiscard]] std::size_t CalibratedItems(const Config& config, std::size_t producers, std::size_t consumers) {
        constexpr double kCalibrationMargin = 1.2;
        constexpr double kMaximumItemsPerProducer = 1'000'000'000.0;
        constexpr std::size_t kMaximumCalibrationAttempts = 8;
        std::size_t calibratedItems = config.itemsPerProducer;
        for (std::size_t attempt = 0; config.minimumSampleSeconds != 0.0; ++attempt) {
            const Sample calibration = RunSample<Queue, T>(producers, consumers, calibratedItems, config.pinWorkers);
            if (calibration.seconds >= config.minimumSampleSeconds) {
                break;
            }
            if (attempt + 1 == kMaximumCalibrationAttempts) {
                throw std::runtime_error("queue benchmark calibration did not reach the requested sample duration");
            }
            const double calibrationScale = config.minimumSampleSeconds * kCalibrationMargin / calibration.seconds;
            const double requestedItems = calibratedItems * std::max(2.0, calibrationScale);
            if (!std::isfinite(requestedItems) || requestedItems > kMaximumItemsPerProducer) {
                throw std::overflow_error("calibrated queue benchmark item count exceeds the practical limit");
            }
            calibratedItems = static_cast<std::size_t>(std::ceil(requestedItems));
        }
        return calibratedItems;
    }

    /** @brief Execute warmups and measured samples for one queue specialization. */
    template<typename Queue, typename T>
    void RunCase(const Config& config, CsvWriter& writer, std::string_view scenario, std::size_t capacity,
                 std::size_t producers, std::size_t consumers) {
        const std::size_t calibratedItems = CalibratedItems<Queue, T>(config, producers, consumers);
        std::cout << scenario << " | " << Queue::name << " | payload=" << sizeof(T) << " | capacity=" << capacity
                  << " | " << producers << 'P' << consumers << "C | items/producer=" << calibratedItems << '\n';
        for (std::size_t warmup = 0; warmup < config.warmups; ++warmup) {
            static_cast<void>(
                RunSample<Queue, T>(producers, consumers, std::max(1uz, calibratedItems / 2), config.pinWorkers));
        }
        for (std::size_t sampleIndex = 0; sampleIndex < config.samples; ++sampleIndex) {
            const auto sample = RunSample<Queue, T>(producers, consumers, calibratedItems, config.pinWorkers);
            writer.Write<Queue>(config, scenario, sizeof(T), capacity, producers, consumers, sampleIndex, sample);
        }
    }

    /** @brief Alternate two implementations sample by sample to suppress time-window drift. */
    template<typename First, typename Second, typename T>
    void RunPairedCase(const Config& config, CsvWriter& writer, std::string_view scenario, std::size_t capacity,
                       std::size_t producers, std::size_t consumers) {
        const std::size_t firstItems = CalibratedItems<First, T>(config, producers, consumers);
        const std::size_t secondItems = CalibratedItems<Second, T>(config, producers, consumers);
        const std::size_t pairedItems = std::max(firstItems, secondItems);
        std::cout << scenario << " | " << First::name << " versus " << Second::name << " | payload=" << sizeof(T)
                  << " | capacity=" << capacity << " | " << producers << 'P' << consumers
                  << "C | items/producer=" << pairedItems << '\n';
        for (std::size_t warmup = 0; warmup < config.warmups; ++warmup) {
            static_cast<void>(
                RunSample<First, T>(producers, consumers, std::max(1uz, pairedItems / 2), config.pinWorkers));
            static_cast<void>(
                RunSample<Second, T>(producers, consumers, std::max(1uz, pairedItems / 2), config.pinWorkers));
        }
        for (std::size_t sampleIndex = 0; sampleIndex < config.samples; ++sampleIndex) {
            auto runFirst = [&] {
                const Sample sample = RunSample<First, T>(producers, consumers, pairedItems, config.pinWorkers);
                writer.Write<First>(config, scenario, sizeof(T), capacity, producers, consumers, sampleIndex, sample);
            };
            auto runSecond = [&] {
                const Sample sample = RunSample<Second, T>(producers, consumers, pairedItems, config.pinWorkers);
                writer.Write<Second>(config, scenario, sizeof(T), capacity, producers, consumers, sampleIndex, sample);
            };
            if ((sampleIndex & 1uz) == 0) {
                runFirst();
                runSecond();
            } else {
                runSecond();
                runFirst();
            }
        }
    }

    /** @brief Type-erased benchmark case used only outside measured regions. */
    struct Case {
        std::function<void()> run;
    };

    /** @brief Append the topology matrix for one capacity and payload type. */
    template<std::size_t Capacity, typename T>
    void AppendCases(const Config& config, CsvWriter& writer, std::vector<Case>& cases) {
        auto add = [&cases]<typename Queue>(const Config& cfg, CsvWriter& out, std::string_view scenario,
                                           std::size_t producers, std::size_t consumers) {
            cases.push_back({[&cfg, &out, scenario, producers, consumers] {
                RunCase<Queue, T>(cfg, out, scenario, Capacity, producers, consumers);
            }});
        };

        add.template operator()<MashiroSpsc<T, Capacity>>(config, writer, "SPSC", 1, 1);
        add.template operator()<MashiroSpscChannel<T, Capacity>>(config, writer, "SPSC", 1, 1);
        add.template operator()<RigtorpSpsc<T, Capacity>>(config, writer, "SPSC", 1, 1);
        add.template operator()<MoodycamelQueue<T, Capacity>>(config, writer, "SPSC", 1, 1);
        add.template operator()<TbbQueue<T, Capacity>>(config, writer, "SPSC", 1, 1);
        add.template operator()<MutexQueue<T, Capacity>>(config, writer, "SPSC", 1, 1);

        const std::array<std::size_t, 3> producerCounts = config.quick ? std::array{2uz, 2uz, 2uz}
                                                                      : std::array{2uz, 4uz, 8uz};
        for (const std::size_t producers : producerCounts) {
            add.template operator()<MashiroMpsc<T, Capacity>>(config, writer, "MPSC", producers, 1);
            add.template operator()<MashiroMpmc<T, Capacity>>(config, writer, "MPSC", producers, 1);
            add.template operator()<RigtorpMpmc<T, Capacity>>(config, writer, "MPSC", producers, 1);
            add.template operator()<MoodycamelQueue<T, Capacity>>(config, writer, "MPSC", producers, 1);
            add.template operator()<TbbQueue<T, Capacity>>(config, writer, "MPSC", producers, 1);
            add.template operator()<MutexQueue<T, Capacity>>(config, writer, "MPSC", producers, 1);
            if (config.quick) {
                break;
            }
        }

        const std::array<std::size_t, 3> consumerCounts =
            config.quick ? std::array{2uz, 2uz, 2uz} : std::array{2uz, 4uz, 8uz};
        for (const std::size_t consumers : consumerCounts) {
            add.template operator()<MashiroSpmc<T, Capacity>>(config, writer, "SPMC", 1, consumers);
            add.template operator()<MashiroMpmc<T, Capacity>>(config, writer, "SPMC", 1, consumers);
            add.template operator()<RigtorpMpmc<T, Capacity>>(config, writer, "SPMC", 1, consumers);
            add.template operator()<MoodycamelQueue<T, Capacity>>(config, writer, "SPMC", 1, consumers);
            add.template operator()<TbbQueue<T, Capacity>>(config, writer, "SPMC", 1, consumers);
            add.template operator()<MutexQueue<T, Capacity>>(config, writer, "SPMC", 1, consumers);
            if (config.quick) {
                break;
            }
        }

        const std::array<std::size_t, 2> symmetricCounts = config.quick ? std::array{2uz, 2uz}
                                                                       : std::array{2uz, 4uz};
        for (const std::size_t count : symmetricCounts) {
            add.template operator()<MashiroMpmc<T, Capacity>>(config, writer, "MPMC", count, count);
            add.template operator()<RigtorpMpmc<T, Capacity>>(config, writer, "MPMC", count, count);
            add.template operator()<MoodycamelQueue<T, Capacity>>(config, writer, "MPMC", count, count);
            add.template operator()<TbbQueue<T, Capacity>>(config, writer, "MPMC", count, count);
            add.template operator()<MutexQueue<T, Capacity>>(config, writer, "MPMC", count, count);
            if (config.quick) {
                break;
            }
        }
    }

    /** @brief Append same-process A/B cases for each optimized Mashiro data path. */
    template<std::size_t Capacity, typename T>
    void AppendPairedCases(const Config& config, CsvWriter& writer, std::vector<Case>& cases) {
        auto add = [&cases]<typename First, typename Second>(const Config& cfg, CsvWriter& out,
                                                             std::string_view scenario, std::size_t producers,
                                                             std::size_t consumers) {
            cases.push_back({[&cfg, &out, scenario, producers, consumers] {
                RunPairedCase<First, Second, T>(cfg, out, scenario, Capacity, producers, consumers);
            }});
        };

        add.template operator()<MashiroSpscChannel<T, Capacity>, SharedEpochSpscChannel<T, Capacity>>(
            config, writer, "SPSC", 1, 1);
        const std::array<std::size_t, 3> producerCounts = config.quick ? std::array{2uz, 2uz, 2uz}
                                                                      : std::array{2uz, 4uz, 8uz};
        for (const std::size_t producers : producerCounts) {
            add.template operator()<MashiroMpsc<T, Capacity>, MashiroMpscOptional<T, Capacity>>(
                config, writer, "MPSC", producers, 1);
            add.template operator()<MashiroMpmc<T, Capacity>, MashiroMpmcOptional<T, Capacity>>(
                config, writer, "MPSC", producers, 1);
            if (config.quick) {
                break;
            }
        }
        const std::array<std::size_t, 2> symmetricCounts = config.quick ? std::array{2uz, 2uz}
                                                                       : std::array{2uz, 4uz};
        for (const std::size_t count : symmetricCounts) {
            add.template operator()<MashiroMpmc<T, Capacity>, MashiroMpmcOptional<T, Capacity>>(
                config, writer, "MPMC", count, count);
            if (config.quick) {
                break;
            }
        }
    }

    /** @brief Append alternating layout comparisons for every concurrent sequence-cell topology. */
    template<std::size_t Capacity, typename T>
    void AppendLayoutCases(const Config& config, CsvWriter& writer, std::vector<Case>& cases) {
        auto add = [&cases]<Concurrency::QueueTopology Topology>(const Config& cfg, CsvWriter& out,
                                                                 std::string_view scenario, std::size_t producers,
                                                                 std::size_t consumers) {
            using First = MashiroSequenceLayout<T, Capacity, Topology, false>;
            using Second = MashiroSequenceLayout<T, Capacity, Topology, true>;
            cases.push_back({[&cfg, &out, scenario, producers, consumers] {
                RunPairedCase<First, Second, T>(cfg, out, scenario, Capacity, producers, consumers);
            }});
        };

        const std::array<std::size_t, 3> endpointCounts =
            config.quick ? std::array{2uz, 2uz, 2uz} : std::array{2uz, 4uz, 8uz};
        for (const std::size_t producers : endpointCounts) {
            add.template operator()<Concurrency::QueueTopologies::Mpsc>(config, writer, "MPSC", producers, 1);
            if (config.quick) {
                break;
            }
        }
        for (const std::size_t consumers : endpointCounts) {
            add.template operator()<Concurrency::QueueTopologies::Spmc>(config, writer, "SPMC", 1, consumers);
            if (config.quick) {
                break;
            }
        }

        const std::array<std::size_t, 2> symmetricCounts = config.quick ? std::array{2uz, 2uz} : std::array{2uz, 4uz};
        for (const std::size_t count : symmetricCounts) {
            add.template operator()<Concurrency::QueueTopologies::Mpmc>(config, writer, "MPMC", count, count);
            if (config.quick) {
                break;
            }
        }
    }

    /** @brief Append strict-FIFO Mashiro versus Rigtorp reference comparisons for every topology. */
    template<std::size_t Capacity, typename T>
    void AppendReferenceCases(const Config& config, CsvWriter& writer, std::vector<Case>& cases) {
        auto add = [&cases]<typename First, typename Second>(const Config& cfg, CsvWriter& out,
                                                             std::string_view scenario, std::size_t producers,
                                                             std::size_t consumers) {
            cases.push_back({[&cfg, &out, scenario, producers, consumers] {
                RunPairedCase<First, Second, T>(cfg, out, scenario, Capacity, producers, consumers);
            }});
        };

        add.template operator()<MashiroSpsc<T, Capacity>, RigtorpSpsc<T, Capacity>>(config, writer, "SPSC", 1, 1);

        const std::array<std::size_t, 3> endpointCounts =
            config.quick ? std::array{2uz, 2uz, 2uz} : std::array{2uz, 4uz, 8uz};
        for (const std::size_t producers : endpointCounts) {
            add.template operator()<MashiroMpsc<T, Capacity>, RigtorpMpmc<T, Capacity>>(config, writer, "MPSC",
                                                                                        producers, 1);
            if (config.quick) {
                break;
            }
        }
        for (const std::size_t consumers : endpointCounts) {
            add.template operator()<MashiroSpmc<T, Capacity>, RigtorpMpmc<T, Capacity>>(config, writer, "SPMC", 1,
                                                                                        consumers);
            if (config.quick) {
                break;
            }
        }

        const std::array<std::size_t, 2> symmetricCounts = config.quick ? std::array{2uz, 2uz} : std::array{2uz, 4uz};
        for (const std::size_t count : symmetricCounts) {
            add.template operator()<MashiroMpmc<T, Capacity>, RigtorpMpmc<T, Capacity>>(config, writer, "MPMC", count,
                                                                                        count);
            if (config.quick) {
                break;
            }
        }
    }

    /** @brief Append controls that expose benchmark-template and implementation-order bias in SPSC comparisons. */
    template<std::size_t Capacity, typename T>
    void AppendSpscAuditCases(const Config& config, CsvWriter& writer, std::vector<Case>& cases) {
        using MashiroA = NamedQueue<MashiroSpsc<T, Capacity>, MashiroSpscA>;
        using MashiroB = NamedQueue<MashiroSpsc<T, Capacity>, MashiroSpscB>;
        using RigtorpA = NamedQueue<RigtorpSpsc<T, Capacity>, RigtorpSpscA>;
        using RigtorpB = NamedQueue<RigtorpSpsc<T, Capacity>, RigtorpSpscB>;

        auto add = [&cases]<typename First, typename Second>(const Config& cfg, CsvWriter& out) {
            cases.push_back({[&cfg, &out] { RunPairedCase<First, Second, T>(cfg, out, "SPSC", Capacity, 1, 1); }});
        };

        add.template operator()<MashiroA, MashiroB>(config, writer);
        add.template operator()<RigtorpA, RigtorpB>(config, writer);
        add.template operator()<MashiroA, RigtorpA>(config, writer);
        add.template operator()<RigtorpB, MashiroB>(config, writer);
    }

    /** @brief Parse benchmark command-line options. */
    [[nodiscard]] Config ParseArguments(int argc, char** argv) {
        Config config;
        auto requireValue = [argc, argv](int& index) -> std::string_view {
            if (++index >= argc) {
                throw std::invalid_argument("missing command-line option value");
            }
            return argv[index];
        };
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--output") {
                config.output = requireValue(index);
            } else if (argument == "--label") {
                config.label = requireValue(index);
            } else if (argument == "--samples") {
                config.samples = std::stoull(std::string{requireValue(index)});
            } else if (argument == "--warmups") {
                config.warmups = std::stoull(std::string{requireValue(index)});
            } else if (argument == "--items") {
                config.itemsPerProducer = std::stoull(std::string{requireValue(index)});
            } else if (argument == "--minimum-ms") {
                config.minimumSampleSeconds = std::stod(std::string{requireValue(index)}) / 1'000.0;
            } else if (argument == "--seed") {
                config.seed = std::stoull(std::string{requireValue(index)});
            } else if (argument == "--quick") {
                config.quick = true;
            } else if (argument == "--no-pin") {
                config.pinWorkers = false;
            } else if (argument == "--paired") {
                config.paired = true;
            } else if (argument == "--layouts") {
                config.layouts = true;
            } else if (argument == "--references") {
                config.references = true;
            } else if (argument == "--spsc-audit") {
                config.spscAudit = true;
            } else {
                throw std::invalid_argument("unknown command-line option: " + std::string{argument});
            }
        }
        if (config.samples == 0 || config.itemsPerProducer == 0 || !std::isfinite(config.minimumSampleSeconds) ||
            config.minimumSampleSeconds < 0.0) {
            throw std::invalid_argument(
                "samples and items must be non-zero and minimum-ms must be finite and non-negative");
        }
        if (static_cast<unsigned>(config.paired) + static_cast<unsigned>(config.layouts) +
                static_cast<unsigned>(config.references) + static_cast<unsigned>(config.spscAudit) >
            1) {
            throw std::invalid_argument(
                "paired algorithm, layout, reference, and SPSC audit modes are mutually exclusive");
        }
        return config;
    }

} // namespace Mashiro::Benchmark

int main(int argc, char** argv) {
    try {
        const auto config = Mashiro::Benchmark::ParseArguments(argc, argv);
        if (!Mashiro::Benchmark::ConfigureBenchmarkProcess(config.pinWorkers)) {
            throw std::runtime_error("failed to raise queue benchmark process priority");
        }
        std::filesystem::create_directories(config.output.parent_path().empty() ? "." : config.output.parent_path());
        Mashiro::Benchmark::CsvWriter writer{config};
        std::vector<Mashiro::Benchmark::Case> cases;

        if (config.spscAudit) {
            Mashiro::Benchmark::AppendSpscAuditCases<4096, Mashiro::Benchmark::Payload<8>>(config, writer, cases);
        } else if (config.quick && config.references) {
            Mashiro::Benchmark::AppendReferenceCases<4096, Mashiro::Benchmark::Payload<8>>(config, writer, cases);
        } else if (config.references) {
            Mashiro::Benchmark::AppendReferenceCases<1024, Mashiro::Benchmark::Payload<8>>(config, writer, cases);
            Mashiro::Benchmark::AppendReferenceCases<16'384, Mashiro::Benchmark::Payload<8>>(config, writer, cases);
            Mashiro::Benchmark::AppendReferenceCases<1024, Mashiro::Benchmark::Payload<64>>(config, writer, cases);
            Mashiro::Benchmark::AppendReferenceCases<16'384, Mashiro::Benchmark::Payload<64>>(config, writer, cases);
        } else if (config.quick && config.layouts) {
            Mashiro::Benchmark::AppendLayoutCases<4096, Mashiro::Benchmark::Payload<8>>(config, writer, cases);
        } else if (config.layouts) {
            Mashiro::Benchmark::AppendLayoutCases<1024, Mashiro::Benchmark::Payload<8>>(config, writer, cases);
            Mashiro::Benchmark::AppendLayoutCases<16'384, Mashiro::Benchmark::Payload<8>>(config, writer, cases);
            Mashiro::Benchmark::AppendLayoutCases<1024, Mashiro::Benchmark::Payload<64>>(config, writer, cases);
            Mashiro::Benchmark::AppendLayoutCases<16'384, Mashiro::Benchmark::Payload<64>>(config, writer, cases);
        } else if (config.quick) {
            if (config.paired) {
                Mashiro::Benchmark::AppendPairedCases<4096, Mashiro::Benchmark::Payload<8>>(config, writer, cases);
            } else {
                Mashiro::Benchmark::AppendCases<4096, Mashiro::Benchmark::Payload<8>>(config, writer, cases);
            }
        } else if (config.paired) {
            Mashiro::Benchmark::AppendPairedCases<1024, Mashiro::Benchmark::Payload<8>>(config, writer, cases);
            Mashiro::Benchmark::AppendPairedCases<16'384, Mashiro::Benchmark::Payload<8>>(config, writer, cases);
            Mashiro::Benchmark::AppendPairedCases<1024, Mashiro::Benchmark::Payload<64>>(config, writer, cases);
            Mashiro::Benchmark::AppendPairedCases<16'384, Mashiro::Benchmark::Payload<64>>(config, writer, cases);
        } else {
            Mashiro::Benchmark::AppendCases<1024, Mashiro::Benchmark::Payload<8>>(config, writer, cases);
            Mashiro::Benchmark::AppendCases<16'384, Mashiro::Benchmark::Payload<8>>(config, writer, cases);
            Mashiro::Benchmark::AppendCases<1024, Mashiro::Benchmark::Payload<64>>(config, writer, cases);
            Mashiro::Benchmark::AppendCases<16'384, Mashiro::Benchmark::Payload<64>>(config, writer, cases);
        }

        std::mt19937_64 random{config.seed};
        std::ranges::shuffle(cases, random);
        for (auto& benchmark : cases) {
            benchmark.run();
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "queue benchmark failed: " << error.what() << '\n';
        return 1;
    }
}

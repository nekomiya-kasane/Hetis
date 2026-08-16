/**
 * @file MichaelScottQueueBenchmark.cpp
 * @brief Reproducible throughput baseline for bounded Michael-Scott storage and MPMC references.
 * @ingroup Benchmark
 */
#if defined(SORA_MIGRATION_TARGET)
#    include <Sora/Core/Concurrency/MichaelScottQueue.h>
#    include <Sora/Core/Concurrency/RingBuffer.h>
#else
#    include <Mashiro/Core/MichaelScottQueue.h>
#    include <Mashiro/Core/RingBuffer.h>
#endif

#include <rigtorp/MPMCQueue.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(SORA_MIGRATION_TARGET)
#    define Mashiro Sora

namespace Sora {

    template<typename T, std::size_t Capacity, Concurrency::QueueTopology Topology>
    using RingBuffer = Concurrency::RingBuffer<T, Capacity, Topology>;

} // namespace Sora
#endif

namespace Mashiro::Benchmark {

    using Clock = std::chrono::steady_clock;

    struct Config {
        std::filesystem::path output = "Mashiro/benchmarks/results/michael-scott-baseline.csv";
        std::size_t samples = 9;
        std::size_t warmups = 2;
        std::size_t itemsPerProducer = 200'000;
    };

    template<typename T>
    class MutexQueue {
    public:
        explicit MutexQueue(std::size_t capacity) : capacity_(capacity) {}

        [[nodiscard]] bool TryPush(T value) {
            std::scoped_lock lock{mutex_};
            if (queue_.size() == capacity_) {
                return false;
            }
            queue_.push_back(std::move(value));
            return true;
        }

        [[nodiscard]] bool TryPop(T& value) {
            std::scoped_lock lock{mutex_};
            if (queue_.empty()) {
                return false;
            }
            value = std::move(queue_.front());
            queue_.pop_front();
            return true;
        }

    private:
        std::size_t capacity_;
        std::mutex mutex_;
        std::deque<T> queue_;
    };

    template<typename Queue>
    struct NativeWrapper {
        Queue queue;

        template<typename T>
        [[nodiscard]] bool Push(T value) { return queue.TryPush(std::move(value)); }
        [[nodiscard]] bool Pop(std::size_t& value) { return queue.TryPop(value); }
    };

    template<std::size_t Capacity>
    struct RingWrapper {
        RingBuffer<std::size_t, Capacity, Concurrency::QueueTopologies::Mpmc> queue;

        [[nodiscard]] bool Push(std::size_t value) { return queue.TryPush(std::move(value)); }
        [[nodiscard]] bool Pop(std::size_t& value) { return queue.TryPop(value); }
    };

    template<std::size_t Capacity>
    struct RigtorpWrapper {
        rigtorp::MPMCQueue<std::size_t> queue{Capacity};

        [[nodiscard]] bool Push(std::size_t value) { return queue.try_push(std::move(value)); }
        [[nodiscard]] bool Pop(std::size_t& value) { return queue.try_pop(value); }
    };

    struct Result {
        double seconds = 0.0;
        std::size_t count = 0;
        std::uint64_t checksum = 0;
    };

    template<typename Factory>
    [[nodiscard]] Result Run(Factory&& factory, std::size_t producers, std::size_t consumers,
                             std::size_t itemsPerProducer) {
        auto queue = factory();
        const std::size_t total = producers * itemsPerProducer;
        std::atomic<std::size_t> consumed{0};
        std::atomic<std::size_t> activeProducers{producers};
        std::atomic<std::size_t> ready{0};
        std::atomic<bool> start{false};
        std::atomic<std::uint64_t> checksum{0};
        std::vector<std::jthread> workers;
        workers.reserve(producers + consumers);

        for (std::size_t producer = 0; producer != producers; ++producer) {
            workers.emplace_back([&, producer] {
                ready.fetch_add(1, std::memory_order_release);
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (std::size_t index = 0; index != itemsPerProducer; ++index) {
                    const std::size_t value = producer * itemsPerProducer + index;
                    while (!queue.Push(value)) {
                        std::this_thread::yield();
                    }
                }
                activeProducers.fetch_sub(1, std::memory_order_release);
            });
        }
        for (std::size_t consumer = 0; consumer != consumers; ++consumer) {
            workers.emplace_back([&] {
                ready.fetch_add(1, std::memory_order_release);
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                std::size_t value = 0;
                for (;;) {
                    if (queue.Pop(value)) {
                        checksum.fetch_add(value, std::memory_order_relaxed);
                        consumed.fetch_add(1, std::memory_order_relaxed);
                    } else if (activeProducers.load(std::memory_order_acquire) == 0 &&
                               consumed.load(std::memory_order_acquire) == total) {
                        return;
                    } else {
                        std::this_thread::yield();
                    }
                }
            });
        }

        while (ready.load(std::memory_order_acquire) != producers + consumers) {
            std::this_thread::yield();
        }
        const auto begin = Clock::now();
        start.store(true, std::memory_order_release);
        for (auto& worker : workers) {
            worker.join();
        }
        const auto end = Clock::now();
        return Result{.seconds = std::chrono::duration<double>(end - begin).count(),
                      .count = consumed.load(std::memory_order_relaxed),
                      .checksum = checksum.load(std::memory_order_relaxed)};
    }

    void WriteHeader(std::ofstream& output) {
        output << "implementation,producers,consumers,capacity,sample,items,count,checksum,seconds,mops\n";
    }

    template<typename Factory>
    void Measure(std::ofstream& output, std::string_view name, std::size_t producers, std::size_t consumers,
                 std::size_t capacity, const Config& config, Factory&& factory) {
        for (std::size_t warmup = 0; warmup != config.warmups; ++warmup) {
            static_cast<void>(Run(factory, producers, consumers, config.itemsPerProducer));
        }
        for (std::size_t sample = 0; sample != config.samples; ++sample) {
            const Result result = Run(factory, producers, consumers, config.itemsPerProducer);
            const std::size_t expectedCount = producers * config.itemsPerProducer;
            const std::uint64_t expectedChecksum = (expectedCount - 1) * expectedCount / 2;
            if (result.count != expectedCount || result.checksum != expectedChecksum) {
                throw std::runtime_error(std::string{name} + " integrity failure: count=" +
                                         std::to_string(result.count) + " expected=" + std::to_string(expectedCount) +
                                         " checksum=" + std::to_string(result.checksum) +
                                         " expectedChecksum=" + std::to_string(expectedChecksum));
            }
            const double mops = static_cast<double>(result.count) / result.seconds / 1.0e6;
            output << name << ',' << producers << ',' << consumers << ',' << capacity << ',' << sample << ','
                   << config.itemsPerProducer << ',' << result.count << ',' << result.checksum << ',' << result.seconds
                   << ',' << mops << '\n';
            std::cout << name << " " << producers << 'P' << consumers << "C sample " << sample << ": " << mops
                      << " Mops/s\n";
        }
    }

    Config Parse(int argc, char** argv) {
        Config config;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--output" && index + 1 < argc) {
                config.output = argv[++index];
            } else if (argument == "--samples" && index + 1 < argc) {
                config.samples = std::stoull(argv[++index]);
            } else if (argument == "--warmups" && index + 1 < argc) {
                config.warmups = std::stoull(argv[++index]);
            } else if (argument == "--items" && index + 1 < argc) {
                config.itemsPerProducer = std::stoull(argv[++index]);
            } else {
                throw std::invalid_argument("unknown or incomplete benchmark option");
            }
        }
        return config;
    }

} // namespace Mashiro::Benchmark

int main(int argc, char** argv) {
    using namespace Mashiro::Benchmark;
    try {
        const Config config = Parse(argc, argv);
        std::filesystem::create_directories(config.output.parent_path());
        std::ofstream output(config.output);
        if (!output) {
            throw std::runtime_error("cannot open benchmark output");
        }
        WriteHeader(output);

        using MichaelScott = Mashiro::MichaelScottQueue<std::size_t, Mashiro::MichaelScottQueueConfig{
            .capacity = 64,
            .maxConcurrentOperations = 16,
            .reclamationThreshold = 16,
            .backing = Mashiro::MichaelScottBacking::Inline}>;
        for (const auto [producers, consumers] : {std::pair{1uz, 1uz}, std::pair{4uz, 4uz}}) {
            Measure(output, "michael-scott", producers, consumers, 64, config,
                    [] { return NativeWrapper<MichaelScott>{}; });
            Measure(output, "mashiro-ring", producers, consumers, 64, config,
                    [] { return RingWrapper<64>{}; });
            Measure(output, "rigtorp-mpmc", producers, consumers, 64, config,
                    [] { return RigtorpWrapper<64>{}; });
            Measure(output, "mutex-deque", producers, consumers, 64, config,
                    [] { return NativeWrapper<MutexQueue<std::size_t>>{MutexQueue<std::size_t>{64}}; });
        }
    } catch (const std::exception& error) {
        std::cerr << "Michael-Scott benchmark failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}

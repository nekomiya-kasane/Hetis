#include "Sora/Experimental/EventPortBaseUnknown.h"
#include "Sora/Kernel/Core/EventPort.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Bench {

    namespace Kernel = Sora::Kernel;
    namespace Exp = Sora::Experimental;
    using Clock = std::chrono::steady_clock;

    volatile std::uintptr_t gPointerSink = 0;
    volatile std::uint64_t gCounterSink = 0;
    constexpr std::uint64_t kLookupObjectCount = 1024;

    struct KernelPing {
        std::uint64_t value{};
    };

    struct ExperimentalPing {
        std::uint64_t value{};
    };

    class [[= Kernel::$::Role{Kernel::TypeOfClass::Implementation}]] KernelObject : public Kernel::BaseUnknown {
        S_OBJECT

    public:
        using Emits = Kernel::EventList<KernelPing>;
        using Accepts = Kernel::EventList<KernelPing>;
        using Callbacks = Kernel::EventList<Kernel::DefaultCallbackTag>;
    };

    struct PlainInline {
        using Emits = Exp::EventList<ExperimentalPing>;
        using Accepts = Exp::EventList<ExperimentalPing>;

        Exp::EventPort events;
    };

    struct PlainExternal {
        using Emits = Exp::EventList<ExperimentalPing>;
        using Accepts = Exp::EventList<ExperimentalPing>;
    };

    std::unordered_map<PlainExternal*, Exp::EventPort> gExternalPorts;

    Exp::EventPort& EventPortOf(PlainExternal& object) {
        return gExternalPorts.at(std::addressof(object));
    }

    struct Sample {
        std::string scenario;
        std::string path;
        std::uint64_t listeners{};
        std::uint64_t iterations{};
        double bestNsPerOp{};
        double medianNsPerOp{};
        double meanNsPerOp{};
        std::uint64_t callbackCount{};
    };

    template<typename F>
    [[nodiscard]] double TimeNsPerOp(std::uint64_t iterations, F&& body) {
        const auto begin = Clock::now();
        body(iterations);
        const auto end = Clock::now();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
        return static_cast<double>(ns) / static_cast<double>(iterations);
    }

    template<typename F>
    [[nodiscard]] Sample Measure(std::string scenario, std::string path, std::uint64_t listeners,
                                 std::uint64_t iterations, std::uint64_t expectedCallbacks, F&& body) {
        constexpr int kWarmups = 2;
        constexpr int kRepeats = 5;
        for (int i = 0; i < kWarmups; ++i) {
            body(iterations / 16 + 1);
        }

        std::vector<double> runs;
        runs.reserve(kRepeats);
        for (int i = 0; i < kRepeats; ++i) {
            runs.push_back(TimeNsPerOp(iterations, body));
        }

        std::ranges::sort(runs);
        const double sum = std::accumulate(runs.begin(), runs.end(), 0.0);
        return Sample{.scenario = std::move(scenario),
                      .path = std::move(path),
                      .listeners = listeners,
                      .iterations = iterations,
                      .bestNsPerOp = runs.front(),
                      .medianNsPerOp = runs[runs.size() / 2],
                      .meanNsPerOp = sum / static_cast<double>(runs.size()),
                      .callbackCount = expectedCallbacks};
    }

    [[nodiscard]] Sample MeasureKernelLookup(std::uint64_t iterations) {
        std::vector<Kernel::ComPtr<KernelObject>> owners;
        std::vector<KernelObject*> objects;
        owners.reserve(kLookupObjectCount);
        objects.reserve(kLookupObjectCount);
        for (std::uint64_t i = 0; i < kLookupObjectCount; ++i) {
            auto object = Kernel::MakeComPtr<KernelObject>();
            (void)Kernel::Events(*object);
            objects.push_back(object.Get());
            owners.push_back(std::move(object));
        }
        auto* table = objects.data();
        std::uintptr_t acc = 0;
        return Measure("port_lookup", "kernel_extension", 0, iterations, 0, [&](std::uint64_t n) {
            for (std::uint64_t i = 0; i < n; ++i) {
                acc += reinterpret_cast<std::uintptr_t>(std::addressof(Kernel::Events(*table[i & 1023u]))) + (i & 1u);
            }
            gPointerSink ^= acc;
        });
    }

    [[nodiscard]] Sample MeasureAdaptorLookup(std::uint64_t iterations) {
        std::vector<Kernel::ComPtr<Kernel::ComAdaptor<int>>> owners;
        std::vector<Kernel::ComAdaptor<int>*> objects;
        owners.reserve(kLookupObjectCount);
        objects.reserve(kLookupObjectCount);
        for (std::uint64_t i = 0; i < kLookupObjectCount; ++i) {
            auto object = Kernel::MakeComAdaptor<int>(7);
            (void)Exp::Events(*object);
            objects.push_back(object.Get());
            owners.push_back(std::move(object));
        }
        auto* table = objects.data();
        std::uintptr_t acc = 0;
        return Measure("port_lookup", "com_adaptor_extension", 0, iterations, 0, [&](std::uint64_t n) {
            for (std::uint64_t i = 0; i < n; ++i) {
                acc += reinterpret_cast<std::uintptr_t>(std::addressof(Exp::Events(*table[i & 1023u]))) + (i & 1u);
            }
            gPointerSink ^= acc;
        });
    }

    [[nodiscard]] Sample MeasureInlineLookup(std::uint64_t iterations) {
        std::vector<std::unique_ptr<PlainInline>> owners;
        std::vector<PlainInline*> objects;
        owners.reserve(kLookupObjectCount);
        objects.reserve(kLookupObjectCount);
        for (std::uint64_t i = 0; i < kLookupObjectCount; ++i) {
            auto object = std::make_unique<PlainInline>();
            objects.push_back(object.get());
            owners.push_back(std::move(object));
        }
        auto* table = objects.data();
        std::uintptr_t acc = 0;
        return Measure("port_lookup", "plain_inline_member", 0, iterations, 0, [&](std::uint64_t n) {
            for (std::uint64_t i = 0; i < n; ++i) {
                acc += reinterpret_cast<std::uintptr_t>(std::addressof(Exp::Events(*table[i & 1023u]))) + (i & 1u);
            }
            gPointerSink ^= acc;
        });
    }

    [[nodiscard]] Sample MeasureExternalLookup(std::uint64_t iterations) {
        std::vector<std::unique_ptr<PlainExternal>> owners;
        std::vector<PlainExternal*> objects;
        owners.reserve(kLookupObjectCount);
        objects.reserve(kLookupObjectCount);
        for (std::uint64_t i = 0; i < kLookupObjectCount; ++i) {
            auto object = std::make_unique<PlainExternal>();
            gExternalPorts.try_emplace(object.get());
            objects.push_back(object.get());
            owners.push_back(std::move(object));
        }
        auto* table = objects.data();
        std::uintptr_t acc = 0;
        auto result = Measure("port_lookup", "plain_external_unordered_map", 0, iterations, 0, [&](std::uint64_t n) {
            for (std::uint64_t i = 0; i < n; ++i) {
                acc += reinterpret_cast<std::uintptr_t>(std::addressof(Exp::Events(*table[i & 1023u]))) + (i & 1u);
            }
            gPointerSink ^= acc;
        });
        for (auto* object : objects) {
            gExternalPorts.erase(object);
        }
        return result;
    }

    [[nodiscard]] std::uint64_t ScaleIterations(std::uint64_t iterations, std::uint64_t divisor) {
        return std::max<std::uint64_t>(1, iterations / std::max<std::uint64_t>(1, divisor));
    }

    [[nodiscard]] std::uint64_t EmitIterationsFor(std::uint64_t listeners, std::uint64_t divisor) {
        if (listeners == 0) {
            return ScaleIterations(100'000, divisor);
        }
        if (listeners == 1) {
            return ScaleIterations(50'000, divisor);
        }
        if (listeners == 8) {
            return ScaleIterations(15'000, divisor);
        }
        return ScaleIterations(4'000, divisor);
    }

    [[nodiscard]] Sample MeasureKernelEmit(std::uint64_t listeners, std::uint64_t divisor) {
        auto object = Kernel::MakeComPtr<KernelObject>();
        auto& port = Kernel::Events(*object);
        std::uint64_t callbackCount = 0;
        std::vector<Kernel::EventLink> links;
        links.reserve(listeners);
        for (std::uint64_t i = 0; i < listeners; ++i) {
            links.push_back(port.ListenTyped<KernelPing>(object.Get(), [&](Kernel::EventContext<KernelPing>& context) {
                callbackCount += context.event.value;
            }));
        }
        const auto iterations = EmitIterationsFor(listeners, divisor);
        KernelObject* volatile ptr = object.Get();
        const KernelPing event{1};
        auto result = Measure("lookup_plus_emit", "kernel_extension", listeners, iterations, listeners * iterations,
                              [&](std::uint64_t n) {
            for (std::uint64_t i = 0; i < n; ++i) {
                Kernel::Events(*ptr).Emit(event, ptr);
            }
        });
        result.callbackCount = callbackCount;
        gCounterSink += callbackCount;
        return result;
    }

    [[nodiscard]] Sample MeasureAdaptorEmit(std::uint64_t listeners, std::uint64_t divisor) {
        auto object = Kernel::MakeComAdaptor<int>(7);
        auto& port = Exp::Events(*object);
        std::uint64_t callbackCount = 0;
        std::vector<Exp::EventLink> links;
        links.reserve(listeners);
        for (std::uint64_t i = 0; i < listeners; ++i) {
            links.push_back(
                port.ListenTyped<ExperimentalPing>(*object, *object, [&](Exp::EventContext<ExperimentalPing>& context) {
                    callbackCount += context.event.value;
                }));
        }
        const auto iterations = EmitIterationsFor(listeners, divisor);
        Kernel::ComAdaptor<int>* volatile ptr = object.Get();
        const ExperimentalPing event{1};
        auto result = Measure("lookup_plus_emit", "com_adaptor_extension", listeners, iterations,
                              listeners * iterations, [&](std::uint64_t n) {
            for (std::uint64_t i = 0; i < n; ++i) {
                Exp::Events(*ptr).Emit(*ptr, event);
            }
        });
        result.callbackCount = callbackCount;
        gCounterSink += callbackCount;
        return result;
    }

    [[nodiscard]] Sample MeasureInlineEmit(std::uint64_t listeners, std::uint64_t divisor) {
        PlainInline object;
        auto& port = Exp::Events(object);
        std::uint64_t callbackCount = 0;
        std::vector<Exp::EventLink> links;
        links.reserve(listeners);
        for (std::uint64_t i = 0; i < listeners; ++i) {
            links.push_back(
                port.ListenTyped<ExperimentalPing>(object, object, [&](Exp::EventContext<ExperimentalPing>& context) {
                    callbackCount += context.event.value;
                }));
        }
        const auto iterations = EmitIterationsFor(listeners, divisor);
        PlainInline* volatile ptr = std::addressof(object);
        const ExperimentalPing event{1};
        auto result = Measure("lookup_plus_emit", "plain_inline_member", listeners, iterations, listeners * iterations,
                              [&](std::uint64_t n) {
            for (std::uint64_t i = 0; i < n; ++i) {
                Exp::Events(*ptr).Emit(*ptr, event);
            }
        });
        result.callbackCount = callbackCount;
        gCounterSink += callbackCount;
        return result;
    }

    [[nodiscard]] Sample MeasureExternalEmit(std::uint64_t listeners, std::uint64_t divisor) {
        PlainExternal object;
        gExternalPorts.try_emplace(std::addressof(object));
        auto& port = Exp::Events(object);
        std::uint64_t callbackCount = 0;
        std::vector<Exp::EventLink> links;
        links.reserve(listeners);
        for (std::uint64_t i = 0; i < listeners; ++i) {
            links.push_back(
                port.ListenTyped<ExperimentalPing>(object, object, [&](Exp::EventContext<ExperimentalPing>& context) {
                    callbackCount += context.event.value;
                }));
        }
        const auto iterations = EmitIterationsFor(listeners, divisor);
        PlainExternal* volatile ptr = std::addressof(object);
        const ExperimentalPing event{1};
        auto result = Measure("lookup_plus_emit", "plain_external_unordered_map", listeners, iterations,
                              listeners * iterations, [&](std::uint64_t n) {
            for (std::uint64_t i = 0; i < n; ++i) {
                Exp::Events(*ptr).Emit(*ptr, event);
            }
        });
        result.callbackCount = callbackCount;
        gCounterSink += callbackCount;
        gExternalPorts.erase(std::addressof(object));
        return result;
    }

    [[nodiscard]] std::vector<Sample> RunAll(std::uint64_t divisor) {
        std::vector<Sample> samples;
        samples.reserve(32);

        const std::uint64_t lookupIterations = ScaleIterations(500'000, divisor);
        samples.push_back(MeasureKernelLookup(lookupIterations));
        samples.push_back(MeasureAdaptorLookup(lookupIterations));
        samples.push_back(MeasureInlineLookup(lookupIterations));
        samples.push_back(MeasureExternalLookup(lookupIterations));

        for (const std::uint64_t listeners : {0ull, 1ull, 8ull, 64ull}) {
            samples.push_back(MeasureKernelEmit(listeners, divisor));
            samples.push_back(MeasureAdaptorEmit(listeners, divisor));
            samples.push_back(MeasureInlineEmit(listeners, divisor));
            samples.push_back(MeasureExternalEmit(listeners, divisor));
        }

        return samples;
    }

    void WriteCsv(const std::filesystem::path& path, const std::vector<Sample>& samples) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path);
        out << "scenario,path,listeners,iterations,best_ns_per_op,median_ns_per_op,mean_ns_per_op,callback_count\n";
        out << std::fixed << std::setprecision(3);
        for (const auto& sample : samples) {
            out << sample.scenario << ',' << sample.path << ',' << sample.listeners << ',' << sample.iterations << ','
                << sample.bestNsPerOp << ',' << sample.medianNsPerOp << ',' << sample.meanNsPerOp << ','
                << sample.callbackCount << '\n';
        }
    }

    void PrintTable(const std::vector<Sample>& samples) {
        std::println("{:<18} {:<30} {:>9} {:>12} {:>12} {:>12} {:>12}", "scenario", "path", "listeners", "iters",
                     "best ns", "median ns", "callbacks");
        for (const auto& sample : samples) {
            std::println("{:<18} {:<30} {:>9} {:>12} {:>12.3f} {:>12.3f} {:>12}", sample.scenario, sample.path,
                         sample.listeners, sample.iterations, sample.bestNsPerOp, sample.medianNsPerOp,
                         sample.callbackCount);
        }
    }

} // namespace Bench

int main(int argc, char** argv) {
    std::filesystem::path output =
        argc > 1 ? std::filesystem::path{argv[1]} : std::filesystem::path{"eventport-bench-results.csv"};
    const auto scaleDivisor = argc > 2 ? std::max<std::uint64_t>(1, std::stoull(argv[2])) : 1;
    auto samples = Bench::RunAll(scaleDivisor);
    Bench::PrintTable(samples);
    Bench::WriteCsv(output, samples);
    std::println("csv={}", output.string());
    std::println("scale_divisor={}", scaleDivisor);
    const auto pointerSink = static_cast<std::uintptr_t>(Bench::gPointerSink);
    const auto counterSink = static_cast<std::uint64_t>(Bench::gCounterSink);
    std::println("sinks: pointer={}, counter={}", pointerSink, counterSink);
    return 0;
}

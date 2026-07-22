/**
 * @file CpuProfilerTest.cpp
 * @brief Verify CPU profiler scope semantics, bounded rings, concurrency, and trace export.
 * @ingroup Testing
 */

#include <Sora/Core/CpuProfiler.h>

#include <catch2/catch_test_macros.hpp>
#include <simdjson.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

    void RecordCurrentFunctionScope() {
        const Sora::CpuProfileScope _{Sora::ProfileCurrentFunction};
    }

    class ProfilerReset {
    public:
        ProfilerReset() {
            profiler_.SetEnabled(false);
            profiler_.Clear();
        }

        ~ProfilerReset() {
            profiler_.SetEnabled(false);
            profiler_.Clear();
        }

        [[nodiscard]] Sora::CpuProfiler& Get() noexcept { return profiler_; }

    private:
        Sora::CpuProfiler& profiler_ = Sora::CpuProfiler::Default();
    };

    class TemporaryTraceFile {
    public:
        TemporaryTraceFile() : path_(std::filesystem::temp_directory_path() / L"sora-cpu-profiler-test.json") {
            std::error_code error;
            std::filesystem::remove(path_, error);
        }

        ~TemporaryTraceFile() {
            std::error_code error;
            std::filesystem::remove(path_, error);
        }

        [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

    private:
        std::filesystem::path path_;
    };

} // namespace

TEST_CASE("CpuProfiler records nested scopes only while enabled", "[Sora.Core.CpuProfiler]") {
    ProfilerReset reset;
    Sora::CpuProfiler& profiler = reset.Get();

    {
        Sora::CpuProfileScope disabled{"disabled"};
    }
    REQUIRE(profiler.EventCount() == 0);

    profiler.SetEnabled(true);
    {
        Sora::CpuProfileScope outer{"outer", Sora::LogCategory::Core};
        {
            const Sora::CpuProfileScope _{"inner", Sora::LogCategory::Render};
        }
    }
    profiler.SetEnabled(false);

    const std::vector<Sora::CpuProfileEvent> events = profiler.SnapshotEvents();
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].name == "outer");
    REQUIRE(events[0].category == Sora::LogCategory::Core);
    REQUIRE(events[0].depth == 0);
    REQUIRE(events[1].name == "inner");
    REQUIRE(events[1].category == Sora::LogCategory::Render);
    REQUIRE(events[1].depth == 1);
    REQUIRE(events[0].startNanoseconds <= events[0].endNanoseconds);
    REQUIRE(events[1].startNanoseconds <= events[1].endNanoseconds);
    REQUIRE(Sora::CpuProfiler::CurrentDepth() == 0);
    REQUIRE(profiler.RecentEvents(1).front().name == "inner");
}

TEST_CASE("CpuProfiler supports C++26 placeholder scopes and call-site function names", "[Sora.Core.CpuProfiler]") {
    ProfilerReset reset;
    Sora::CpuProfiler& profiler = reset.Get();
    profiler.SetEnabled(true);
    {
        const Sora::CpuProfileScope _{"first"};
        const Sora::CpuProfileScope _{"second"};
        RecordCurrentFunctionScope();
    }
    profiler.SetEnabled(false);

    const std::vector<Sora::CpuProfileEvent> events = profiler.SnapshotEvents();
    REQUIRE(events.size() == 3);
    REQUIRE(std::ranges::any_of(
        events, [](const Sora::CpuProfileEvent& event) { return event.name.contains("RecordCurrentFunctionScope"); }));
}

TEST_CASE("CpuProfiler thread rings retain only their newest bounded history", "[Sora.Core.CpuProfiler]") {
    ProfilerReset reset;
    Sora::CpuProfiler& profiler = reset.Get();
    profiler.SetEnabled(true);

    constexpr size_t kExtraEvents = 17;
    constexpr size_t kTotalEvents = Sora::CpuProfiler::kThreadBufferCapacity + kExtraEvents;
    for (size_t index = 0; index < kTotalEvents; ++index) {
        profiler.RecordEvent("ring", Sora::LogCategory::Core, index, index + 1, 0);
    }
    profiler.SetEnabled(false);

    REQUIRE(profiler.EventCount() == Sora::CpuProfiler::kThreadBufferCapacity);
    REQUIRE(profiler.DroppedEventCount() == kExtraEvents);
    const std::vector<Sora::CpuProfileEvent> events = profiler.SnapshotEvents();
    REQUIRE(events.front().startNanoseconds == kExtraEvents);
    REQUIRE(events.back().startNanoseconds == kTotalEvents - 1);
}

TEST_CASE("CpuProfiler merges independent producer rings into chronological order", "[Sora.Core.CpuProfiler]") {
    ProfilerReset reset;
    Sora::CpuProfiler& profiler = reset.Get();
    profiler.SetEnabled(true);

    constexpr size_t kThreadCount = 4;
    constexpr size_t kEventsPerThread = 256;
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    for (size_t threadIndex = 0; threadIndex < kThreadCount; ++threadIndex) {
        threads.emplace_back([threadIndex, &profiler] {
            for (size_t index = 0; index < kEventsPerThread; ++index) {
                const uint64_t timestamp = static_cast<uint64_t>(index * kThreadCount + threadIndex);
                profiler.RecordEvent("parallel", Sora::LogCategory::Kernel, timestamp, timestamp + 1, 0);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    profiler.SetEnabled(false);

    const std::vector<Sora::CpuProfileEvent> events = profiler.SnapshotEvents();
    REQUIRE(events.size() == kThreadCount * kEventsPerThread);
    for (size_t index = 0; index < events.size(); ++index) {
        REQUIRE(events[index].startNanoseconds == index);
    }
}

TEST_CASE("CpuProfiler snapshots remain coherent while producers are active", "[Sora.Core.CpuProfiler]") {
    ProfilerReset reset;
    Sora::CpuProfiler& profiler = reset.Get();
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> sequence{0};
    profiler.SetEnabled(true);

    std::vector<std::thread> threads;
    constexpr size_t kThreadCount = 4;
    threads.reserve(kThreadCount);
    for (size_t threadIndex = 0; threadIndex < kThreadCount; ++threadIndex) {
        threads.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                const uint64_t timestamp = sequence.fetch_add(1, std::memory_order_relaxed);
                profiler.RecordEvent("concurrent", Sora::LogCategory::Core, timestamp, timestamp + 1, 0);
            }
        });
    }
    while (sequence.load(std::memory_order_acquire) < 1'000) {
        std::this_thread::yield();
    }

    for (size_t iteration = 0; iteration < 16; ++iteration) {
        const std::vector<Sora::CpuProfileEvent> snapshot = profiler.SnapshotEvents();
        REQUIRE_FALSE(snapshot.empty());
        for (size_t index = 1; index < snapshot.size(); ++index) {
            REQUIRE(snapshot[index - 1].startNanoseconds <= snapshot[index].startNanoseconds);
        }
    }

    stop.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }
    profiler.SetEnabled(false);
    REQUIRE(profiler.EventCount() != 0);
}

TEST_CASE("CpuProfiler exports escaped Trace Event JSON and restores enabled state", "[Sora.Core.CpuProfiler]") {
    ProfilerReset reset;
    Sora::CpuProfiler& profiler = reset.Get();
    TemporaryTraceFile trace;
    profiler.SetEnabled(true);
    profiler.RecordEvent("quoted\"\nname", Sora::LogCategory::Resource, 1'250, 4'500, 3);

    REQUIRE(profiler.ExportTrace(trace.Path()).has_value());
    REQUIRE(profiler.IsEnabled());
    profiler.SetEnabled(false);

    std::ifstream input{trace.Path(), std::ios::binary};
    REQUIRE(input.is_open());
    std::ostringstream buffer;
    buffer << input.rdbuf();

    const std::string json = buffer.str();
    simdjson::dom::parser parser;
    simdjson::dom::element document{};
    REQUIRE(parser.parse(json).get(document) == simdjson::SUCCESS);

    std::string_view displayUnit{};
    REQUIRE(document["displayTimeUnit"].get(displayUnit) == simdjson::SUCCESS);
    REQUIRE(displayUnit == "ns");

    simdjson::dom::array events{};
    REQUIRE(document["traceEvents"].get(events) == simdjson::SUCCESS);
    REQUIRE(events.size() == 1);

    simdjson::dom::element event{};
    REQUIRE(events.at(0).get(event) == simdjson::SUCCESS);

    std::string_view name{};
    std::string_view category{};
    std::string_view phase{};
    REQUIRE(event["name"].get(name) == simdjson::SUCCESS);
    REQUIRE(event["cat"].get(category) == simdjson::SUCCESS);
    REQUIRE(event["ph"].get(phase) == simdjson::SUCCESS);
    REQUIRE(name == "quoted\"\nname");
    REQUIRE(category == "Resource");
    REQUIRE(phase == "X");

    double timestamp = 0.0;
    double duration = 0.0;
    int64_t depth = 0;
    REQUIRE(event["ts"].get(timestamp) == simdjson::SUCCESS);
    REQUIRE(event["dur"].get(duration) == simdjson::SUCCESS);
    REQUIRE(event["args"]["depth"].get(depth) == simdjson::SUCCESS);
    REQUIRE(timestamp == 1.250);
    REQUIRE(duration == 3.250);
    REQUIRE(depth == 3);
}

TEST_CASE("CpuProfiler rejects inverted intervals", "[Sora.Core.CpuProfiler]") {
    ProfilerReset reset;
    Sora::CpuProfiler& profiler = reset.Get();
    profiler.SetEnabled(true);
    profiler.RecordEvent("invalid", Sora::LogCategory::Core, 9, 4, 0);
    profiler.SetEnabled(false);
    REQUIRE(profiler.EventCount() == 0);
}

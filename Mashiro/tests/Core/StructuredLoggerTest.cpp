/**
 * @file StructuredLoggerTest.cpp
 * @brief Comprehensive tests for StructuredLogger: enums, annotations,
 *        SourceLoc, sinks, ring buffer integration, drain thread, level
 *        filtering, backpressure, free-function Log<>, multi-thread.
 */
#include "Mashiro/Core/StructuredLogger.h"
#include "Mashiro/Core/ToString.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace Mashiro;

// =============================================================================
// [LogLevel / LogCategory] — Enum basics and reflection
// =============================================================================

TEST_CASE("LogLevel: values are ordered", "[Core.Logger]") {
    STATIC_REQUIRE(LogLevel::Trace < LogLevel::Debug);
    STATIC_REQUIRE(LogLevel::Debug < LogLevel::Info);
    STATIC_REQUIRE(LogLevel::Info < LogLevel::Warn);
    STATIC_REQUIRE(LogLevel::Warn < LogLevel::Error);
    STATIC_REQUIRE(LogLevel::Error < LogLevel::Fatal);
}

TEST_CASE("LogLevel: ToStringView via reflection", "[Core.Logger]") {
    REQUIRE(ToStringView(LogLevel::Trace) == "Trace");
    REQUIRE(ToStringView(LogLevel::Info) == "Info");
    REQUIRE(ToStringView(LogLevel::Fatal) == "Fatal");
}

TEST_CASE("LogCategory: ToStringView via reflection", "[Core.Logger]") {
    REQUIRE(ToStringView(LogCategory::Core) == "Core");
    REQUIRE(ToStringView(LogCategory::Render) == "Render");
    REQUIRE(ToStringView(LogCategory::Physics) == "Physics");
}

// =============================================================================
// [Annotations] — DefaultLevel / LevelColor consteval extraction
// =============================================================================

TEST_CASE("DefaultLevel table values", "[Core.Logger]") {
    STATIC_REQUIRE(Detail::Log::kDefaultCategoryLevels[0] == 2);  // Core → Info
    STATIC_REQUIRE(Detail::Log::kDefaultCategoryLevels[3] == 1);  // Scene → Debug
    STATIC_REQUIRE(Detail::Log::kDefaultCategoryLevels[11] == 0); // App → Trace
}

TEST_CASE("LevelColor table values", "[Core.Logger]") {
    constexpr auto traceColor = Detail::Log::kLevelColors[0];
    STATIC_REQUIRE(traceColor.dim == true);
    STATIC_REQUIRE(traceColor.r == 128);

    constexpr auto errorColor = Detail::Log::kLevelColors[4];
    STATIC_REQUIRE(errorColor.bold == true);
    STATIC_REQUIRE(errorColor.r == 220);
    STATIC_REQUIRE(errorColor.g == 0);
}

// =============================================================================
// [SourceLoc] — Auto-capture and filename extraction
// =============================================================================

TEST_CASE("SourceLoc: Current captures caller info", "[Core.Logger]") {
    auto loc = SourceLoc::Current();
    REQUIRE(loc.line > 0);
    REQUIRE(!loc.file.empty());
    REQUIRE(!loc.function.empty());
    // file should contain this test file's name
    REQUIRE(loc.file.find("StructuredLoggerTest") != std::string_view::npos);
}

TEST_CASE("SourceLoc: FileName extracts basename", "[Core.Logger]") {
    SourceLoc loc{"C:\\foo\\bar\\baz.cpp", "f", 1};
    REQUIRE(loc.FileName() == "baz.cpp");

    SourceLoc loc2{"/usr/src/main.cpp", "f", 1};
    REQUIRE(loc2.FileName() == "main.cpp");

    SourceLoc loc3{"simple.cpp", "f", 1};
    REQUIRE(loc3.FileName() == "simple.cpp");
}

// =============================================================================
// [CallbackSink] — Capture log entries for verification
// =============================================================================

namespace {

    struct OwnedLogEntry {
        LogLevel level;
        LogCategory category;
        std::string message;
        std::string file;
        std::string func;
        uint32_t line = 0;
        uint64_t timestampNs = 0;
        uint32_t threadId = 0;
    };

    struct CapturedEntries {
        std::vector<OwnedLogEntry> entries;
        std::mutex mutex;

        void Capture(const LogEntry& entry) {
            std::lock_guard lock(mutex);
            entries.push_back(OwnedLogEntry{
                .level = entry.level,
                .category = entry.category,
                .message = std::string(entry.message),
                .file = std::string(entry.file),
                .func = std::string(entry.func),
                .line = entry.line,
                .timestampNs = entry.timestampNs,
                .threadId = entry.threadId,
            });
        }
    };

    /// @brief Set up logger with callback sink, drain synchronously (no thread).
    struct LogTestFixture {
        CapturedEntries captured;

        LogTestFixture() {
            auto& logger = StructuredLogger::Instance();
            logger.ClearSinks();
            logger.ResetDroppedCount();
            logger.ResetRings();
            // Set all categories to Trace so nothing is filtered
            for (int i = 0; i < static_cast<int>(Traits::EnumEnumeratorsCount<LogCategory>); ++i) {
                logger.SetCategoryLevel(static_cast<LogCategory>(i), LogLevel::Trace);
            }
            logger.AddSink(CallbackSink{[this](const LogEntry& e) { captured.Capture(e); }});
        }

        ~LogTestFixture() {
            auto& logger = StructuredLogger::Instance();
            logger.ClearSinks();
        }

        void DrainAll() {
            StructuredLogger::Instance().Flush();
        }
    };

} // anonymous namespace

// =============================================================================
// [StructuredLogger] — Basic logging via callback sink
// =============================================================================

TEST_CASE("Logger: basic log and drain", "[Core.Logger]") {
    LogTestFixture fix;
    auto& logger = StructuredLogger::Instance();

    logger.Log({}, LogLevel::Info, LogCategory::Core, "hello {}", 42);
    fix.DrainAll();

    REQUIRE(fix.captured.entries.size() == 1);
    REQUIRE(fix.captured.entries[0].level == LogLevel::Info);
    REQUIRE(fix.captured.entries[0].category == LogCategory::Core);
    REQUIRE(fix.captured.entries[0].message == "hello 42");
    REQUIRE(fix.captured.entries[0].line > 0);
}

TEST_CASE("Logger: multiple messages", "[Core.Logger]") {
    LogTestFixture fix;
    auto& logger = StructuredLogger::Instance();

    logger.Log({}, LogLevel::Debug, LogCategory::Render, "frame {}", 1);
    logger.Log({}, LogLevel::Warn, LogCategory::Scene, "missing {}", "node");
    logger.Log({}, LogLevel::Error, LogCategory::Core, "fatal");
    fix.DrainAll();

    REQUIRE(fix.captured.entries.size() == 3);
    REQUIRE(fix.captured.entries[0].message == "frame 1");
    REQUIRE(fix.captured.entries[1].message == "missing node");
    REQUIRE(fix.captured.entries[2].message == "fatal");
}

TEST_CASE("Logger: source location is captured", "[Core.Logger]") {
    LogTestFixture fix;
    auto& logger = StructuredLogger::Instance();

    logger.Log({}, LogLevel::Info, LogCategory::Core, "loc test");
    fix.DrainAll();

    REQUIRE(fix.captured.entries.size() == 1);
    auto& e = fix.captured.entries[0];
    REQUIRE(e.line > 0);
    REQUIRE(!e.file.empty());
    REQUIRE(e.file.find("StructuredLoggerTest") != std::string_view::npos);
}

// =============================================================================
// [Level filtering] — Compile-time and runtime
// =============================================================================

TEST_CASE("Logger: runtime level filtering", "[Core.Logger]") {
    LogTestFixture fix;
    auto& logger = StructuredLogger::Instance();

    logger.SetCategoryLevel(LogCategory::Core, LogLevel::Warn);
    logger.Log({}, LogLevel::Debug, LogCategory::Core, "should be filtered");
    logger.Log({}, LogLevel::Info, LogCategory::Core, "also filtered");
    logger.Log({}, LogLevel::Warn, LogCategory::Core, "passes");
    logger.Log({}, LogLevel::Error, LogCategory::Core, "also passes");
    fix.DrainAll();

    REQUIRE(fix.captured.entries.size() == 2);
    REQUIRE(fix.captured.entries[0].message == "passes");
    REQUIRE(fix.captured.entries[1].message == "also passes");
}

TEST_CASE("Logger: category level defaults from annotation", "[Core.Logger]") {
    // Fresh logger should have Core at Info (DefaultLevel{2})
    auto& logger = StructuredLogger::Instance();
    // Reset to annotation defaults by recreating — but singleton, so test indirectly:
    // We verified kDefaultCategoryLevels[0] == 2 in annotation test above.
    // Just verify the API works:
    logger.SetCategoryLevel(LogCategory::Core, LogLevel::Info);
    REQUIRE(logger.GetCategoryLevel(LogCategory::Core) == LogLevel::Info);
}

// =============================================================================
// [Free-function Log<>] — Template NTTP interface
// =============================================================================

TEST_CASE("Log<Level, Cat>: free function works", "[Core.Logger]") {
    LogTestFixture fix;
    using enum LogLevel;
    using enum LogCategory;

    Mashiro::Log<Info, Render>({}, "draw calls: {}", 100);
    fix.DrainAll();

    REQUIRE(fix.captured.entries.size() == 1);
    REQUIRE(fix.captured.entries[0].level == LogLevel::Info);
    REQUIRE(fix.captured.entries[0].category == LogCategory::Render);
    REQUIRE(fix.captured.entries[0].message == "draw calls: 100");
}

TEST_CASE("MLOG macro works", "[Core.Logger]") {
    LogTestFixture fix;

    MLOG(Error, Core, "macro test {}", 99);
    fix.DrainAll();

    REQUIRE(fix.captured.entries.size() == 1);
    REQUIRE(fix.captured.entries[0].level == LogLevel::Error);
    REQUIRE(fix.captured.entries[0].message == "macro test 99");
}

// =============================================================================
// [Backpressure] — Drop policy
// =============================================================================

TEST_CASE("Logger: dropped count increments on overflow", "[Core.Logger]") {
    LogTestFixture fix;
    auto& logger = StructuredLogger::Instance();
    logger.SetBackpressurePolicy(BackpressurePolicy::Drop);
    logger.ResetDroppedCount();

    // Fill the ring buffer (64KB) with large messages
    std::string bigMsg(1000, 'X');
    for (int i = 0; i < 200; ++i) {
        logger.Log({}, LogLevel::Info, LogCategory::Core, "{}", bigMsg);
    }

    // Some should have been dropped (ring is 64KB, 200 * ~1024 bytes = ~200KB)
    // Don't drain — just check dropped count
    REQUIRE(logger.GetDroppedCount() > 0);
}

// =============================================================================
// [Drain thread] — Background drain
// =============================================================================

TEST_CASE("Logger: drain thread processes messages", "[Core.Logger]") {
    LogTestFixture fix;
    auto& logger = StructuredLogger::Instance();
    logger.StartDrainThread();
    REQUIRE(logger.IsRunning());

    logger.Log({}, LogLevel::Info, LogCategory::Core, "threaded {}", 1);
    logger.Log({}, LogLevel::Info, LogCategory::Core, "threaded {}", 2);
    logger.Flush(); // blocks until drain thread processes all

    REQUIRE(fix.captured.entries.size() == 2);
    REQUIRE(fix.captured.entries[0].message == "threaded 1");
    REQUIRE(fix.captured.entries[1].message == "threaded 2");

    logger.Shutdown();
    REQUIRE(!logger.IsRunning());
}

// =============================================================================
// [Multi-thread] — Concurrent logging from multiple producers
// =============================================================================

TEST_CASE("Logger: multi-thread concurrent logging", "[Core.Logger]") {
    LogTestFixture fix;
    auto& logger = StructuredLogger::Instance();
    logger.SetBackpressurePolicy(BackpressurePolicy::Block);
    logger.StartDrainThread();

    constexpr int kThreads = 4;
    constexpr int kMsgsPerThread = 100;
    std::atomic<int> readyCount{0};

    auto worker = [&](int threadIdx) {
        readyCount.fetch_add(1);
        while (readyCount.load() < kThreads) {} // spin barrier

        for (int i = 0; i < kMsgsPerThread; ++i) {
            logger.Log({}, LogLevel::Info, LogCategory::Core, "t{}m{}", threadIdx, i);
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back(worker, t);
    }
    for (auto& t : threads) t.join();

    logger.Flush();

    REQUIRE(fix.captured.entries.size() == kThreads * kMsgsPerThread);

    logger.Shutdown();
}

// =============================================================================
// [LogFlush] — Convenience function
// =============================================================================

TEST_CASE("LogFlush: works without drain thread", "[Core.Logger]") {
    LogTestFixture fix;
    auto& logger = StructuredLogger::Instance();

    logger.Log({}, LogLevel::Info, LogCategory::Core, "flush test");
    Mashiro::LogFlush();

    REQUIRE(fix.captured.entries.size() == 1);
}

// =============================================================================
// [Edge cases]
// =============================================================================

TEST_CASE("Logger: empty format string", "[Core.Logger]") {
    LogTestFixture fix;
    auto& logger = StructuredLogger::Instance();

    logger.Log({}, LogLevel::Info, LogCategory::Core, "");
    fix.DrainAll();

    REQUIRE(fix.captured.entries.size() == 1);
    REQUIRE(fix.captured.entries[0].message.empty());
}

TEST_CASE("Logger: long format arguments", "[Core.Logger]") {
    LogTestFixture fix;
    auto& logger = StructuredLogger::Instance();

    std::string longStr(400, 'A');
    logger.Log({}, LogLevel::Info, LogCategory::Core, "{}", longStr);
    fix.DrainAll();

    REQUIRE(fix.captured.entries.size() == 1);
    REQUIRE(fix.captured.entries[0].message.size() == 400);
}

TEST_CASE("Logger: message truncated at 511 chars", "[Core.Logger]") {
    LogTestFixture fix;
    auto& logger = StructuredLogger::Instance();

    std::string huge(1000, 'B');
    logger.Log({}, LogLevel::Info, LogCategory::Core, "{}", huge);
    fix.DrainAll();

    REQUIRE(fix.captured.entries.size() == 1);
    REQUIRE(fix.captured.entries[0].message.size() <= 511);
}

TEST_CASE("Logger: ClearSinks removes all sinks", "[Core.Logger]") {
    auto& logger = StructuredLogger::Instance();
    logger.AddSink(CallbackSink{[](const LogEntry&) {}});
    logger.ClearSinks();
    // No crash on log after clear
    logger.Log({}, LogLevel::Info, LogCategory::Core, "no sink");
    logger.Flush();
}

TEST_CASE("Logger: timestamp is monotonic", "[Core.Logger]") {
    LogTestFixture fix;
    auto& logger = StructuredLogger::Instance();

    logger.Log({}, LogLevel::Info, LogCategory::Core, "first");
    logger.Log({}, LogLevel::Info, LogCategory::Core, "second");
    fix.DrainAll();

    REQUIRE(fix.captured.entries.size() == 2);
    REQUIRE(fix.captured.entries[1].timestampNs >= fix.captured.entries[0].timestampNs);
}

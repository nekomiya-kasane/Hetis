#include "Sora/Core/StructuredLogger.h"
#include "Sora/Core/ToJson.h"

#include <catch2/catch_test_macros.hpp>

#include <simdjson.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

TEST_CASE("StructuredLogger initializes category thresholds from enum annotations", "[Sora.Core.StructuredLogger]") {
    Sora::StructuredLogger logger;
    CHECK(logger.GetCategoryLevel(Sora::LogCategory::Core) == Sora::LogLevel::Info);
    CHECK(logger.GetCategoryLevel(Sora::LogCategory::Scene) == Sora::LogLevel::Debug);
    CHECK(logger.GetCategoryLevel(Sora::LogCategory::Audio) == Sora::LogLevel::Info);
    CHECK(logger.GetCategoryLevel(Sora::LogCategory::Editor) == Sora::LogLevel::Debug);
    CHECK(logger.GetCategoryLevel(Sora::LogCategory::App) == Sora::LogLevel::Trace);
    CHECK(logger.GetCategoryLevel(static_cast<Sora::LogCategory>(63)) == Sora::LogLevel::Trace);
    logger.Shutdown();
}

TEST_CASE("JsonLogSink emits parseable NDJSON through ToJson", "[Sora.Core.StructuredLogger]") {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / std::format("sora-json-log-{}.ndjson", stamp);

    {
        Sora::JsonLogSink sink(path);
        REQUIRE(sink.IsOpen());

        const Sora::LogRecord record{.level = Sora::LogLevel::Warn,
                                     .category = Sora::LogCategory::Resource,
                                     .source = {.file = "G:/Teaching/Vulkan/Sora/tests/Core/StructuredLoggerTest.cpp",
                                                .function = "JsonLogSinkTest",
                                                .line = 42,
                                                .column = 7},
                                     .timestamp = std::chrono::system_clock::time_point{std::chrono::milliseconds{123}},
                                     .threadId = 17,
                                     .message = "quote \" slash \\ newline\n"};
        sink.Write(record);
        sink.Flush();
    }

    std::ifstream input(path);
    REQUIRE(input.is_open());

    std::string line;
    REQUIRE(static_cast<bool>(std::getline(input, line)));

    simdjson::dom::parser parser;
    const simdjson::dom::element parsed = parser.parse(line);

    CHECK(int64_t(parsed["ts"]) == 123);
    CHECK(std::string_view(parsed["level"]) == "Warn");
    CHECK(std::string_view(parsed["category"]) == "Resource");
    CHECK(int64_t(parsed["thread"]) == 17);
    CHECK(std::string_view(parsed["file"]) == "G:/Teaching/Vulkan/Sora/tests/Core/StructuredLoggerTest.cpp");
    CHECK(int64_t(parsed["line"]) == 42);
    CHECK(std::string_view(parsed["function"]) == "JsonLogSinkTest");
    CHECK(std::string_view(parsed["message"]) == "quote \" slash \\ newline\n");

    input.close();
    std::error_code cleanupError;
    std::filesystem::remove(path, cleanupError);
}

TEST_CASE("StructuredLogger dispatches through the instance that owns the producer ring",
          "[Sora.Core.StructuredLogger]") {
    Sora::StructuredLogger logger;
    std::mutex recordsMutex;
    std::vector<std::string> messages;
    std::vector<std::string> files;
    std::vector<std::string> functions;
    logger.AddSink(Sora::CallbackLogSink{[&](const Sora::LogRecord& record) {
        std::lock_guard lock(recordsMutex);
        messages.push_back(record.message);
        files.emplace_back(record.source.file);
        functions.emplace_back(record.source.function);
    }});

    logger.Submit(Sora::LogLevel::Info, Sora::LogCategory::Core,
                  {.file = "instance.cpp", .function = "Submit", .line = 19, .column = 7}, "owned record");
    logger.Flush();

    {
        std::lock_guard lock(recordsMutex);
        REQUIRE(messages.size() == 1);
        CHECK(messages.front() == "owned record");
        CHECK(files.front() == "instance.cpp");
        CHECK(functions.front() == "Submit");
    }
    logger.Shutdown();
}

TEST_CASE("StructuredLogger permits a sink to submit a deferred record", "[Sora.Core.StructuredLogger]") {
    Sora::StructuredLogger logger;
    std::mutex recordsMutex;
    std::vector<std::string> messages;
    logger.AddSink(Sora::CallbackLogSink{[&](const Sora::LogRecord& record) {
        {
            std::lock_guard lock(recordsMutex);
            messages.push_back(record.message);
        }
        if (record.message == "outer") {
            logger.Submit(Sora::LogLevel::Info, Sora::LogCategory::Core,
                          {.file = "recursive.cpp", .function = "Sink", .line = 31}, "inner");
        }
    }});

    logger.Submit(Sora::LogLevel::Info, Sora::LogCategory::Core,
                  {.file = "recursive.cpp", .function = "Producer", .line = 27}, "outer");
    logger.Flush();

    {
        std::lock_guard lock(recordsMutex);
        CHECK(messages == std::vector<std::string>{"outer", "inner"});
    }
    logger.Shutdown();
}

TEST_CASE("StructuredLogger drains and unregisters rings owned by exiting producer threads",
          "[Sora.Core.StructuredLogger]") {
    Sora::StructuredLogger logger;
    std::atomic<uint32_t> delivered{0};
    logger.AddSink(
        Sora::CallbackLogSink{[&](const Sora::LogRecord&) { delivered.fetch_add(1, std::memory_order_relaxed); }});

    std::vector<std::thread> producers;
    for (uint32_t index = 0; index < 8; ++index) {
        producers.emplace_back([&] {
            logger.Submit(Sora::LogLevel::Info, Sora::LogCategory::Core,
                          {.file = "thread.cpp", .function = "Producer", .line = 11}, "thread record");
        });
    }
    for (std::thread& producer : producers) {
        producer.join();
    }
    logger.Flush();

    CHECK(delivered.load(std::memory_order_relaxed) == producers.size());
    logger.Shutdown();
}

TEST_CASE("StructuredLogger truncates an oversized blocking record without losing its message prefix",
          "[Sora.Core.StructuredLogger]") {
    Sora::StructuredLogger logger;
    logger.SetBackpressurePolicy(Sora::LogBackpressurePolicy::Block);
    std::mutex recordMutex;
    std::string deliveredMessage;
    std::string deliveredFile;
    std::string deliveredFunction;
    logger.AddSink(Sora::CallbackLogSink{[&](const Sora::LogRecord& record) {
        std::lock_guard lock(recordMutex);
        deliveredMessage = record.message;
        deliveredFile = record.source.file;
        deliveredFunction = record.source.function;
    }});

    const std::string message(128 * 1024, 'm');
    const std::string file(64 * 1024, 'f');
    const std::string function(64 * 1024, 'g');
    logger.Submit(Sora::LogLevel::Info, Sora::LogCategory::Core,
                  {.file = file, .function = function, .line = 23, .column = 5}, message);
    logger.Flush();

    {
        std::lock_guard lock(recordMutex);
        REQUIRE_FALSE(deliveredMessage.empty());
        CHECK(deliveredMessage.size() < message.size());
        CHECK(std::ranges::all_of(deliveredMessage, [](char value) { return value == 'm'; }));
        CHECK(deliveredFile.empty());
        CHECK(deliveredFunction.empty());
    }
    CHECK(logger.DroppedCount() == 0);
    logger.Shutdown();
}

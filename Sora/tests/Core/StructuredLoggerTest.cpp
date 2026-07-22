#include "Sora/Core/StructuredLogger.h"
#include "Sora/Core/ToJson.h"

#include <catch2/catch_test_macros.hpp>

#include <simdjson.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

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

/**
 * @file ProcessTest.cpp
 * @brief Verify current-process identity, startup metadata, and resource-usage observations.
 * @ingroup Testing
 */

#include <Sora/Core/PAL/Process.h>
#include <Sora/Core/PAL/Thread.h>
#include <Sora/Core/Unicode.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <ranges>
#include <string>
#include <type_traits>
#include <vector>

static_assert(std::is_trivially_copyable_v<Sora::PAL::ProcessUsage>);
static_assert(std::is_nothrow_invocable_r_v<size_t, std::hash<Sora::PAL::NativeProcessId>,
                                            Sora::PAL::NativeProcessId>);

TEST_CASE("Current process identity is stable and has a parent", "[Sora.PAL.Process]") {
    const auto first = Sora::PAL::CurrentNativeProcessId();
    const auto second = Sora::PAL::CurrentNativeProcessId();
    const auto parent = Sora::PAL::ParentNativeProcessId();

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(*second == *first);
    REQUIRE(first->Value() != 0);
    REQUIRE(parent.has_value());
    REQUIRE(*parent != *first);
}

TEST_CASE("Current process thread snapshot contains the calling thread", "[Sora.PAL.Process]") {
    const auto process = Sora::PAL::CurrentNativeProcessId();
    const auto thread = Sora::PAL::GetCurrentNativeThreadId();
    REQUIRE(process.has_value());
    REQUIRE(thread.has_value());

    const auto threads = Sora::PAL::EnumerateProcessThreads(*process);
    REQUIRE(threads.has_value());
    REQUIRE(std::ranges::is_sorted(*threads));
    REQUIRE(std::ranges::find(*threads, *thread) != threads->end());
}

TEST_CASE("Current process image path identifies the running test", "[Sora.PAL.Process]") {
    const auto image = Sora::PAL::CurrentProcessImagePath();
    REQUIRE(image.has_value());
    REQUIRE_FALSE(image->empty());
    REQUIRE_FALSE(image->filename().empty());

    std::error_code error;
    REQUIRE(std::filesystem::is_regular_file(*image, error));
    REQUIRE_FALSE(error);
}

TEST_CASE("Current process arguments preserve ordered UTF-8 startup values", "[Sora.PAL.Process]") {
    const auto arguments = Sora::PAL::CurrentProcessArguments();
    REQUIRE(arguments.has_value());
    REQUIRE_FALSE(arguments->empty());
    for (const std::string& argument : *arguments) {
        REQUIRE(Sora::Unicode::ValidateUtf8(argument));
    }
}

TEST_CASE("Current process usage reports monotonic CPU time and resident memory", "[Sora.PAL.Process]") {
    const auto before = Sora::PAL::CaptureCurrentProcessUsage();
    REQUIRE(before.has_value());

    const auto after = Sora::PAL::CaptureCurrentProcessUsage();
    REQUIRE(after.has_value());
    REQUIRE(after->userCpuTime >= before->userCpuTime);
    REQUIRE(after->kernelCpuTime >= before->kernelCpuTime);
    REQUIRE(after->residentMemoryBytes != 0);
    REQUIRE(after->peakResidentMemoryBytes >= after->residentMemoryBytes);
}

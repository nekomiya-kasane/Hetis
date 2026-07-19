/**
 * @file ProcessTest.cpp
 * @brief Verify current-process identity, startup metadata, and resource-usage observations.
 * @ingroup Testing
 */

#include <Sora/Core/PAL/Process.h>
#include <Sora/Core/Unicode.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <type_traits>
#include <vector>

static_assert(std::is_trivially_copyable_v<Sora::PAL::ProcessUsage>);

TEST_CASE("Current process identity is stable and has a parent", "[Sora.PAL.Process]") {
    const auto first = Sora::PAL::CurrentProcessId();
    const auto second = Sora::PAL::CurrentProcessId();
    const auto parent = Sora::PAL::ParentProcessId();

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(*second == *first);
    REQUIRE(*first != 0);
    REQUIRE(parent.has_value());
    REQUIRE(*parent != *first);
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

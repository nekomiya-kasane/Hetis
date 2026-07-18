/**
 * @file ThreadTest.cpp
 * @brief Verify PAL thread identity, names, processor observations, and stack bounds.
 * @ingroup Testing
 */

#include <Sora/Core/PAL/Thread.h>
#include <Sora/Platform.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <latch>
#include <string>
#include <thread>

static_assert(Sora::PAL::ThreadStackBounds{.lower = 2, .upper = 1}.Size() == 0);

TEST_CASE("Native thread ids are stable and distinguish concurrent threads", "[Sora.PAL.Thread]") {
    const uint64_t callingThread = Sora::PAL::CurrentNativeThreadId();
    REQUIRE(callingThread != 0);
    REQUIRE(Sora::PAL::CurrentNativeThreadId() == callingThread);

    std::array<uint64_t, 2> workerIds{};
    std::latch captured{2};
    std::array workers{
        std::thread{[&] {
            workerIds[0] = Sora::PAL::CurrentNativeThreadId();
            captured.arrive_and_wait();
        }},
        std::thread{[&] {
            workerIds[1] = Sora::PAL::CurrentNativeThreadId();
            captured.arrive_and_wait();
        }},
    };
    for (std::thread& worker : workers) {
        worker.join();
    }

    REQUIRE(workerIds[0] != 0);
    REQUIRE(workerIds[1] != 0);
    REQUIRE(workerIds[0] != callingThread);
    REQUIRE(workerIds[1] != callingThread);
    REQUIRE(workerIds[0] != workerIds[1]);
}

TEST_CASE("Current thread names round-trip as UTF-8", "[Sora.PAL.Thread]") {
    const auto original = Sora::PAL::CurrentThreadName();
    REQUIRE(original.has_value());

    constexpr std::string_view kName{"Sora.\xCE\xBC"};
    REQUIRE(Sora::PAL::SetCurrentThreadName(kName).has_value());
    const auto current = Sora::PAL::CurrentThreadName();
    REQUIRE(current.has_value());
    REQUIRE(*current == kName);
    REQUIRE(Sora::PAL::SetCurrentThreadName(*original).has_value());
}

TEST_CASE("Thread names reject malformed UTF-8 and embedded nulls", "[Sora.PAL.Thread]") {
    const std::array malformed{static_cast<char>(0xC0), static_cast<char>(0x80)};
    const auto invalidUtf8 = Sora::PAL::SetCurrentThreadName(std::string_view{malformed.data(), malformed.size()});
    REQUIRE_FALSE(invalidUtf8.has_value());
    REQUIRE(invalidUtf8.error() == Sora::ErrorCode::InvalidThreadName);

    constexpr std::array embeddedNull{'a', '\0', 'b'};
    const auto invalidNull =
        Sora::PAL::SetCurrentThreadName(std::string_view{embeddedNull.data(), embeddedNull.size()});
    REQUIRE_FALSE(invalidNull.has_value());
    REQUIRE(invalidNull.error() == Sora::ErrorCode::InvalidThreadName);

    if constexpr (Sora::kIsLinux || Sora::kIsMacOS) {
        const size_t nativeLimit = 15;
        const std::string tooLong(nativeLimit + 1, 'x');
        const auto invalidLength = Sora::PAL::SetCurrentThreadName(tooLong);
        REQUIRE_FALSE(invalidLength.has_value());
        REQUIRE(invalidLength.error() == Sora::ErrorCode::ThreadNameTooLong);
    }
}

TEST_CASE("Current logical processor reports a native scheduler observation", "[Sora.PAL.Thread]") {
    const auto processor = Sora::PAL::CurrentLogicalProcessor();
    if constexpr (Sora::kIsWindows || Sora::kIsLinux) {
        REQUIRE(processor.has_value());
    } else {
        REQUIRE(processor.error() == Sora::ErrorCode::NotSupported);
    }
}

TEST_CASE("Current stack bounds contain calling-frame storage", "[Sora.PAL.Thread]") {
    const auto bounds = Sora::PAL::CurrentThreadStackBounds();
    REQUIRE(bounds.has_value());
    REQUIRE(bounds->lower < bounds->upper);
    REQUIRE(bounds->Size() != 0);

    const int stackValue = 0;
    REQUIRE(bounds->Contains(&stackValue));
    REQUIRE_FALSE(bounds->Contains(nullptr));
}

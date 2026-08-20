/**
 * @file ThreadTest.cpp
 * @brief Verify PAL thread identity, names, processor observations, and stack bounds.
 * @ingroup Testing
 */

#include <Sora/Core/PAL/Thread.h>
#include <Sora/Platform.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <latch>
#include <string>
#include <thread>
#include <type_traits>

static_assert(Sora::PAL::ThreadStackBounds{.lower = 2, .upper = 1}.Size() == 0);
static_assert(std::is_nothrow_invocable_r_v<size_t, std::hash<Sora::PAL::NativeThreadId>, Sora::PAL::NativeThreadId>);

TEST_CASE("Native thread ids are stable and distinguish concurrent threads", "[Sora.PAL.Thread]") {
    const auto callingThread = Sora::PAL::GetCurrentNativeThreadId();
    REQUIRE(callingThread.has_value());
    const auto repeated = Sora::PAL::GetCurrentNativeThreadId();
    REQUIRE(repeated.has_value());
    REQUIRE(*repeated == *callingThread);

    std::array<Sora::PAL::NativeThreadId, 2> workerIds{};
    std::latch captured{2};
    std::array workers{
        std::thread{[&] {
            workerIds[0] = Sora::PAL::GetCurrentNativeThreadId().value_or(Sora::PAL::NativeThreadId{});
            captured.arrive_and_wait();
        }},
        std::thread{[&] {
            workerIds[1] = Sora::PAL::GetCurrentNativeThreadId().value_or(Sora::PAL::NativeThreadId{});
            captured.arrive_and_wait();
        }},
    };
    for (std::thread& worker : workers) {
        worker.join();
    }

    REQUIRE(workerIds[0].Value() != 0);
    REQUIRE(workerIds[1].Value() != 0);
    REQUIRE(workerIds[0] != *callingThread);
    REQUIRE(workerIds[1] != *callingThread);
    REQUIRE(workerIds[0] != workerIds[1]);
}

TEST_CASE("Current thread names round-trip as UTF-8", "[Sora.PAL.Thread]") {
    const auto original = Sora::PAL::GetCurrentThreadName();
    REQUIRE(original.has_value());

    constexpr std::string_view kName{"Sora.\xCE\xBC"};
    REQUIRE(Sora::PAL::SetCurrentThreadName(kName).has_value());
    const auto current = Sora::PAL::GetCurrentThreadName();
    REQUIRE(current.has_value());
    REQUIRE(*current == kName);
    REQUIRE(Sora::PAL::SetCurrentThreadName(*original).has_value());
    const auto restored = Sora::PAL::GetCurrentThreadName();
    REQUIRE(restored.has_value());
    REQUIRE(*restored == *original);
}

TEST_CASE("Non-current thread metadata is resolved by native id at call time", "[Sora.PAL.Thread]") {
    std::atomic<bool> release = false;
    Sora::PAL::NativeThreadId workerId{};
    std::latch captured{1};
    std::jthread worker{[&] {
        workerId = Sora::PAL::GetCurrentNativeThreadId().value_or(Sora::PAL::NativeThreadId{});
        captured.count_down();
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }};
    captured.wait();
    REQUIRE(workerId.Value() != 0);

    constexpr std::string_view kWorkerName{"Sora.Worker"};
    const auto named = Sora::PAL::SetThreadName(workerId, kWorkerName);
    const auto observed = Sora::PAL::GetThreadName(workerId);
    const auto alive = Sora::PAL::IsThreadAlive(workerId);
    if constexpr (Sora::kIsMacOS) {
        REQUIRE(named.error() == Sora::ErrorCode::NotSupported);
        REQUIRE(observed.error() == Sora::ErrorCode::NotSupported);
        REQUIRE(alive.error() == Sora::ErrorCode::NotSupported);
    } else {
        REQUIRE(named.has_value());
        REQUIRE(observed.has_value());
        REQUIRE(*observed == kWorkerName);
        REQUIRE(alive.has_value());
        REQUIRE(*alive);
    }

    release.store(true, std::memory_order_release);
    worker.join();
    if constexpr (!Sora::kIsMacOS) {
        const auto stopped = Sora::PAL::IsThreadAlive(workerId);
        REQUIRE(stopped.has_value());
        REQUIRE_FALSE(*stopped);
    }
}

TEST_CASE("Thread-id operations reject the zero sentinel", "[Sora.PAL.Thread]") {
    constexpr Sora::PAL::NativeThreadId invalid{};
    REQUIRE(Sora::PAL::SetThreadName(invalid, "name").error() == Sora::ErrorCode::InvalidArgument);
    REQUIRE(Sora::PAL::GetThreadName(invalid).error() == Sora::ErrorCode::InvalidArgument);
    REQUIRE(Sora::PAL::IsThreadAlive(invalid).error() == Sora::ErrorCode::InvalidArgument);
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
    const auto processor = Sora::PAL::GetCurrentLogicalProcessor();
    if constexpr (Sora::kIsWindows || Sora::kIsLinux) {
        REQUIRE(processor.has_value());
    } else {
        REQUIRE(processor.error() == Sora::ErrorCode::NotSupported);
    }
}

TEST_CASE("Current stack bounds contain calling-frame storage", "[Sora.PAL.Thread]") {
    const auto bounds = Sora::PAL::GetCurrentThreadStackBounds();
    REQUIRE(bounds.has_value());
    REQUIRE(bounds->lower < bounds->upper);
    REQUIRE(bounds->Size() != 0);

    const int stackValue = 0;
    REQUIRE(bounds->Contains(&stackValue));
    REQUIRE_FALSE(bounds->Contains(nullptr));
}

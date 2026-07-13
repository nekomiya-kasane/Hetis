#include <Sora/Core/Time/Stopwatch.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <type_traits>

namespace {

    struct ManualClock {
        using rep = std::int64_t;
        using period = std::milli;
        using duration = std::chrono::duration<rep, period>;
        using time_point = std::chrono::time_point<ManualClock>;

        static constexpr bool is_steady = true;

        [[nodiscard]] static constexpr time_point now() noexcept { return current; }

        static inline time_point current{};
    };

    using ManualStopwatch = Sora::Time::BasicStopwatch<ManualClock>;

    static_assert(sizeof(ManualStopwatch) == 2 * sizeof(ManualClock::time_point));
    static_assert(std::is_trivially_copyable_v<ManualStopwatch>);

} // namespace

TEST_CASE("BasicStopwatch measures total and lap durations from one monotonic clock", "[Sora.Core.Time.Stopwatch]") {
    ManualClock::current = ManualClock::time_point{ManualClock::duration{10}};
    ManualStopwatch stopwatch;

    ManualClock::current += ManualClock::duration{25};
    REQUIRE(stopwatch.Elapsed() == ManualClock::duration{25});
    REQUIRE(stopwatch.PeekLap() == ManualClock::duration{25});
    REQUIRE(stopwatch.Lap() == ManualClock::duration{25});

    ManualClock::current += ManualClock::duration{7};
    REQUIRE(stopwatch.ElapsedAs<std::chrono::milliseconds>() == std::chrono::milliseconds{32});
    REQUIRE(stopwatch.PeekLapAs<std::chrono::milliseconds>() == std::chrono::milliseconds{7});
    REQUIRE(stopwatch.LapAs<std::chrono::milliseconds>() == std::chrono::milliseconds{7});
}

TEST_CASE("BasicStopwatch reset and restart update total and lap origins together", "[Sora.Core.Time.Stopwatch]") {
    ManualClock::current = ManualClock::time_point{ManualClock::duration{100}};
    ManualStopwatch stopwatch;

    ManualClock::current += ManualClock::duration{30};
    REQUIRE(stopwatch.Restart() == ManualClock::duration{30});
    REQUIRE(stopwatch.Elapsed() == ManualClock::duration::zero());
    REQUIRE(stopwatch.PeekLap() == ManualClock::duration::zero());

    ManualClock::current += ManualClock::duration{11};
    stopwatch.Reset();
    ManualClock::current += ManualClock::duration{5};
    REQUIRE(stopwatch.Elapsed() == ManualClock::duration{5});
    REQUIRE(stopwatch.PeekLap() == ManualClock::duration{5});
}

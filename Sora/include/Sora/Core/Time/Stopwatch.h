/**
 * @file Stopwatch.h
 * @brief Provide a zero-allocation monotonic stopwatch with independently resettable lap timing.
 * @details @ref Sora::Time::BasicStopwatch samples its clock once per operation and stores only the start and lap time
 * points. The clock is a template parameter so deterministic tests and specialized monotonic clocks do not require
 * runtime type erasure. The default @ref Sora::Time::Stopwatch uses @c std::chrono::steady_clock.
 * @ingroup Core
 */
#pragma once

#include <chrono>
#include <concepts>

namespace Sora {

    namespace Time {

        namespace Concept {

            /** @brief Clock suitable for measuring elapsed time without wall-clock discontinuities. */
            template<typename Clock>
            concept StopwatchClock = requires {
                typename Clock::duration;
                typename Clock::time_point;
                { Clock::now() } -> std::same_as<typename Clock::time_point>;
                requires Clock::is_steady;
            };

        } // namespace Concept

        /**
         * @brief Measure total and lap elapsed time using a monotonic clock.
         * @tparam Clock Monotonic chrono clock sampled by stopwatch operations.
         * @note Concurrent calls that mutate the same stopwatch require external synchronization.
         */
        template<Concept::StopwatchClock Clock>
        class BasicStopwatch {
        public:
            using ClockType = Clock;                      /**< Monotonic clock used for measurements. */
            using Duration = typename Clock::duration;    /**< Native clock duration returned without conversion. */
            using TimePoint = typename Clock::time_point; /**< Native clock time point retained by the stopwatch. */

            /** @brief Start total and lap timing from one clock sample. */
            constexpr BasicStopwatch() noexcept(noexcept(Clock::now())) : BasicStopwatch(Clock::now()) {}

            /**
             * @brief Start total and lap timing from @p start.
             * @param[in] start Initial start and lap time point.
             */
            constexpr explicit BasicStopwatch(TimePoint start) noexcept : start_{start}, lap_{start} {}

            /** @brief Return the total elapsed duration without changing stopwatch state. */
            [[nodiscard]] constexpr Duration Elapsed() const noexcept(noexcept(Clock::now())) {
                return Clock::now() - start_;
            }

            /**
             * @brief Return the total elapsed time converted to @p ToDuration.
             * @tparam ToDuration Target chrono duration type.
             */
            template<typename ToDuration>
            [[nodiscard]] constexpr ToDuration ElapsedAs() const noexcept(noexcept(Clock::now())) {
                return std::chrono::duration_cast<ToDuration>(Elapsed());
            }

            /** @brief Return the current lap duration, then begin the next lap from the same clock sample. */
            [[nodiscard]] constexpr Duration Lap() noexcept(noexcept(Clock::now())) {
                const TimePoint now = Clock::now();
                const Duration elapsed = now - lap_;
                lap_ = now;
                return elapsed;
            }

            /**
             * @brief Return the current lap duration converted to @p ToDuration and begin the next lap.
             * @tparam ToDuration Target chrono duration type.
             */
            template<typename ToDuration>
            [[nodiscard]] constexpr ToDuration LapAs() noexcept(noexcept(Clock::now())) {
                return std::chrono::duration_cast<ToDuration>(Lap());
            }

            /** @brief Return the duration since the previous lap or reset without changing the lap time point. */
            [[nodiscard]] constexpr Duration PeekLap() const noexcept(noexcept(Clock::now())) {
                return Clock::now() - lap_;
            }

            /**
             * @brief Return the current lap duration converted to @p ToDuration without changing the lap time point.
             * @tparam ToDuration Target chrono duration type.
             */
            template<typename ToDuration>
            [[nodiscard]] constexpr ToDuration PeekLapAs() const noexcept(noexcept(Clock::now())) {
                return std::chrono::duration_cast<ToDuration>(PeekLap());
            }

            /** @brief Reset total and lap timing from one clock sample. */
            constexpr void Reset() noexcept(noexcept(Clock::now())) {
                const TimePoint now = Clock::now();
                start_ = now;
                lap_ = now;
            }

            /** @brief Return the total elapsed duration and reset total and lap timing from the same clock sample. */
            [[nodiscard]] constexpr Duration Restart() noexcept(noexcept(Clock::now())) {
                const TimePoint now = Clock::now();
                const Duration elapsed = now - start_;
                start_ = now;
                lap_ = now;
                return elapsed;
            }

        private:
            TimePoint start_;
            TimePoint lap_;
        };

        /** @brief Default monotonic stopwatch using @c std::chrono::steady_clock. */
        using Stopwatch = BasicStopwatch<std::chrono::steady_clock>;

    } // namespace Time

    namespace Concept {

        inline namespace Time {

            template<typename Clock>
            concept StopwatchClock = Sora::Time::Concept::StopwatchClock<Clock>;

        }

    } // namespace Concept

} // namespace Sora

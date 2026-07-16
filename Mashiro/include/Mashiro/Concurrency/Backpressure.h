/**
 * @file Backpressure.h
 * @brief Compile-time producer policies for bounded asynchronous queue backpressure.
 * @ingroup Concurrency
 */
#pragma once

#include <atomic>
#include <concepts>
#include <cstdint>
#include <exception>
#include <thread>
#include <type_traits>
#include <utility>

namespace Mashiro::Concurrency {

    /** @brief Action selected after a bounded queue rejects one producer-side transfer attempt. */
    enum class BackpressureAction : std::uint8_t {
        Suspend,
        Retry,
        Reject,
        Drop,
    };

    /** @brief Successful value-channel outcome of a producer sender. */
    enum class PushDisposition : std::uint8_t {
        Enqueued,
        Rejected,
        Dropped,
    };

    /** @brief Value returned by producer senders when queue lifecycle errors did not occur. */
    struct PushOutcome {
        PushDisposition disposition{PushDisposition::Enqueued};

        [[nodiscard]] constexpr bool Enqueued() const noexcept { return disposition == PushDisposition::Enqueued; }

        [[nodiscard]] friend constexpr bool operator==(PushOutcome, PushOutcome) noexcept = default;
    };

    /** @brief Stateless or per-submission policy contract used by producer operation states. */
    template<class Policy>
    concept BackpressurePolicy =
        std::copyable<Policy> && std::is_nothrow_copy_constructible_v<Policy> &&
        std::is_nothrow_move_constructible_v<Policy> && std::is_nothrow_copy_assignable_v<Policy> &&
        std::is_nothrow_move_assignable_v<Policy> && requires(Policy& policy) {
            { Policy::maySuspend } -> std::same_as<const bool&>;
            { policy.OnBackpressure() } noexcept -> std::same_as<BackpressureAction>;
            { policy.BeforeRetry() } noexcept -> std::same_as<void>;
            { policy.OnCommitted() } noexcept -> std::same_as<void>;
        };

    namespace Backpressure {

        /** @brief Park the sender operation until capacity or cancellation becomes observable. */
        struct Suspend {
            static constexpr bool maySuspend = true;

            [[nodiscard]] constexpr BackpressureAction OnBackpressure() noexcept { return BackpressureAction::Suspend; }

            constexpr void BeforeRetry() noexcept {}
            constexpr void OnCommitted() noexcept {}
        };

        /** @brief Complete normally with @ref PushDisposition::Rejected without consuming the submitted value. */
        struct Reject {
            static constexpr bool maySuspend = false;

            [[nodiscard]] constexpr BackpressureAction OnBackpressure() noexcept { return BackpressureAction::Reject; }

            constexpr void BeforeRetry() noexcept {}
            constexpr void OnCommitted() noexcept {}
        };

        /** @brief Complete normally with @ref PushDisposition::Dropped and destroy the submitted value with the sender.
         */
        struct DropNewest {
            static constexpr bool maySuspend = false;

            [[nodiscard]] constexpr BackpressureAction OnBackpressure() noexcept { return BackpressureAction::Drop; }

            constexpr void BeforeRetry() noexcept {}
            constexpr void OnCommitted() noexcept {}
        };

        /** @brief CPU behavior between bounded immediate retry attempts. */
        enum class RetryMode : std::uint8_t {
            Spin,
            Yield,
        };

        /**
         * @brief Retry a fixed number of times before delegating to @p Fallback.
         * @tparam Retries Number of additional immediate storage attempts after the first backpressure observation.
         * @tparam Mode Tight retry or scheduler-friendly thread yield between attempts.
         * @tparam Fallback Policy consulted after the retry budget is exhausted.
         */
        template<std::uint32_t Retries, RetryMode Mode = RetryMode::Spin, BackpressurePolicy Fallback = Reject>
        class Retry {
        public:
            static constexpr bool maySuspend = Fallback::maySuspend;

            constexpr Retry()
                requires std::default_initializable<Fallback>
            = default;

            explicit constexpr Retry(Fallback fallback) noexcept : fallback_(std::move(fallback)) {}

            [[nodiscard]] constexpr BackpressureAction OnBackpressure() noexcept {
                if (remaining_ != 0) {
                    --remaining_;
                    return BackpressureAction::Retry;
                }
                return fallback_.OnBackpressure();
            }

            void BeforeRetry() noexcept {
                if constexpr (Mode == RetryMode::Yield) {
                    std::this_thread::yield();
                }
            }

            constexpr void OnCommitted() noexcept { fallback_.OnCommitted(); }

        private:
            [[msvc::no_unique_address]] Fallback fallback_;
            std::uint32_t remaining_{Retries};
        };

        /** @brief Explicitly shared counters for observing a backpressure policy without hidden allocation. */
        struct Counters {
            std::atomic_uint64_t committed{0};
            std::atomic_uint64_t full{0};
            std::atomic_uint64_t suspended{0};
            std::atomic_uint64_t retried{0};
            std::atomic_uint64_t rejected{0};
            std::atomic_uint64_t dropped{0};
        };

        /**
         * @brief Decorate a policy with opt-in relaxed atomic telemetry.
         * @note The referenced @ref Counters object must outlive every producer port, sender, and operation state
         * copied from this policy.
         */
        template<BackpressurePolicy Policy>
        class Instrumented {
        public:
            static constexpr bool maySuspend = Policy::maySuspend;

            Instrumented(Policy policy, Counters& counters) noexcept
                : policy_(std::move(policy)), counters_(&counters) {}

            [[nodiscard]] BackpressureAction OnBackpressure() noexcept {
                counters_->full.fetch_add(1, std::memory_order_relaxed);
                const BackpressureAction action = policy_.OnBackpressure();
                CounterFor(action).fetch_add(1, std::memory_order_relaxed);
                return action;
            }

            void BeforeRetry() noexcept { policy_.BeforeRetry(); }

            void OnCommitted() noexcept {
                policy_.OnCommitted();
                counters_->committed.fetch_add(1, std::memory_order_relaxed);
            }

        private:
            [[nodiscard]] std::atomic_uint64_t& CounterFor(BackpressureAction action) noexcept {
                switch (action) {
                    case BackpressureAction::Suspend:
                        return counters_->suspended;
                    case BackpressureAction::Retry:
                        return counters_->retried;
                    case BackpressureAction::Reject:
                        return counters_->rejected;
                    case BackpressureAction::Drop:
                        return counters_->dropped;
                }
                std::terminate();
            }

            [[msvc::no_unique_address]] Policy policy_;
            Counters* counters_;
        };

        template<class Policy>
        Instrumented(Policy, Counters&) -> Instrumented<Policy>;

    } // namespace Backpressure

} // namespace Mashiro::Concurrency

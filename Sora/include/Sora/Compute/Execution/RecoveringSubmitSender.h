/**
 * @file RecoveringSubmitSender.h
 * @brief Bounded fresh-attempt graph submission with runtime-epoch recovery.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Execution/SubmitSender.h>
#include <Sora/Compute/Runtime/GraphAttempt.h>
#include <Sora/Compute/Runtime/RuntimeRecoveryCoordinator.h>

#include <exec/repeat_until.hpp>
#include <exec/variant_sender.hpp>
#include <stdexec/execution.hpp>

#include <concepts>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace Sora::Compute {

    inline constexpr uint32_t kMaximumGraphSubmissionAttempts = 16;

    /** @brief Bounded policy for transient queue fallback and device-generation
     * recovery. */
    struct GraphRecoveryPolicy {
        uint32_t maximumAttempts = 2;
        bool retryQueueUnavailable = true;
        bool recoverDeviceLoss = true;

        /** @brief Return whether the attempt bound is finite, nonzero, and supported.
         */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return maximumAttempts != 0 && maximumAttempts <= kMaximumGraphSubmissionAttempts;
        }
    };

    /** @brief Invalid retry policy rejected before invoking the attempt factory. */
    struct InvalidGraphRecoveryPolicy {
        GraphRecoveryPolicy policy{};
    };

    /** @brief Placement, planning, or materialization failure from attempt @p
     * attempt. */
    template<typename Error>
    struct GraphAttemptBuildFailure {
        uint32_t attempt = 0;
        Error error;
    };

    /** @brief Non-retryable physical graph submission failure. */
    struct TerminalGraphSubmissionFailure {
        uint32_t attempt = 0;
        GraphSubmissionError error{};
    };

    /** @brief Runtime epoch reconstruction failure after one device-generation
     * error. */
    struct GraphRuntimeRecoveryFailure {
        uint32_t attempt = 0;
        RuntimeRecoveryError error{};
    };

    /** @brief Runtime observation that cannot or must not reconstruct an epoch
     * under the active policy. */
    struct GraphRuntimeObservationFailure {
        uint32_t attempt = 0;
        RuntimeEpochError error{};
    };

    /** @brief Retryable submission failure that exhausted the configured attempt
     * bound. */
    struct GraphRetryLimitExceeded {
        uint32_t attempts = 0;
        GraphSubmissionError lastSubmission{};
    };

    /** @brief Failure while connecting or scheduling the structured retry
     * operation. */
    struct GraphOrchestrationFailure {
        std::exception_ptr exception{};
    };

    /** @brief Exact terminal error alternatives of bounded recovering graph
     * submission. */
    template<typename AttemptError>
    using RecoveringGraphError =
        std::variant<InvalidGraphRecoveryPolicy, GraphAttemptBuildFailure<AttemptError>, TerminalGraphSubmissionFailure,
                     GraphRuntimeRecoveryFailure, GraphRuntimeObservationFailure, GraphRetryLimitExceeded,
                     GraphOrchestrationFailure>;

    template<typename Result>
    concept GraphAttemptResult = requires(Result& result) {
        typename Result::value_type;
        typename Result::error_type;
        requires std::same_as<typename Result::value_type, ExecutableGraph>;
        { result.has_value() } noexcept -> std::same_as<bool>;
        { *result } -> std::same_as<ExecutableGraph&>;
        { result.error() } -> std::same_as<typename Result::error_type&>;
    };

    template<typename Factory>
    concept GraphAttemptFactory = requires(Factory& factory, const GraphAttemptContext& context) {
        requires GraphAttemptResult<std::invoke_result_t<Factory&, const GraphAttemptContext&>>;
        requires std::is_nothrow_invocable_v<Factory&, const GraphAttemptContext&>;
        requires std::is_nothrow_move_constructible_v<
            typename std::invoke_result_t<Factory&, const GraphAttemptContext&>::error_type>;
    };

    namespace Detail {

        /** @brief Zero-storage semantic token making a stateful submission sender single-shot. */
        struct SingleShotSubmission final {
            SingleShotSubmission() noexcept = default;
            SingleShotSubmission(SingleShotSubmission&&) noexcept = default;
            SingleShotSubmission& operator=(SingleShotSubmission&&) noexcept = default;
            SingleShotSubmission(const SingleShotSubmission&) = delete;
            SingleShotSubmission& operator=(const SingleShotSubmission&) = delete;
        };

        [[nodiscard]] constexpr bool Retryable(GraphRecoveryPolicy policy, SubmissionErrorCode code) noexcept {
            return (policy.retryQueueUnavailable && code == SubmissionErrorCode::QueueUnavailable) ||
                   (policy.recoverDeviceLoss && code == SubmissionErrorCode::DeviceLost);
        }

        template<GraphAttemptFactory Factory, RuntimeEpochRecoveryProvider Provider>
        class RecoveringGraphState final {
        public:
            using AttemptResult = std::invoke_result_t<Factory&, const GraphAttemptContext&>;
            using AttemptError = typename AttemptResult::error_type;
            using Error = RecoveringGraphError<AttemptError>;

            RecoveringGraphState(RuntimeRecoveryCoordinator& recovery, GraphRecoveryPolicy value, Factory makeAttempt,
                                 Provider rebuildEpoch) noexcept
                : coordinator_(&recovery),
                  policy_(value),
                  factory_(std::move(makeAttempt)),
                  provider_(std::move(rebuildEpoch)) {}

            /** @brief Initialize the epoch at sender start rather than sender
             * construction. */
            void Initialize() noexcept {
                if (!epoch_) {
                    epoch_ = coordinator_->Current();
                }
            }

            [[nodiscard]] bool PrepareAttempt() noexcept {
                Initialize();
                auto refreshed = epoch_->RefreshDeviceSnapshots();
                if (refreshed) {
                    refreshedSnapshots_ = std::move(*refreshed);
                    if (previousSubmission_ && previousSubmission_->error.code == SubmissionErrorCode::DeviceLost) {
                        error_.emplace(std::in_place_type<TerminalGraphSubmissionFailure>, attempts_ - 1,
                                       *previousSubmission_);
                        return false;
                    }
                    return true;
                }

                const uint32_t failedAttempt = previousSubmission_ ? attempts_ - 1 : attempts_;
                const bool recoverable = refreshed.error().code == RuntimeEpochErrorCode::DeviceLost ||
                                         refreshed.error().code == RuntimeEpochErrorCode::DeviceGenerationChanged;
                if (!policy_.recoverDeviceLoss || !recoverable) {
                    error_.emplace(std::in_place_type<GraphRuntimeObservationFailure>, failedAttempt,
                                   refreshed.error());
                    return false;
                }
                auto recovered = coordinator_->Recover(epoch_, refreshed.error(), provider_);
                if (!recovered) {
                    error_.emplace(std::in_place_type<GraphRuntimeRecoveryFailure>, failedAttempt, recovered.error());
                    return false;
                }
                epoch_ = std::move(*recovered);
                refreshedSnapshots_.clear();
                return true;
            }

            [[nodiscard]] GraphAttemptContext NextContext() const noexcept {
                return {.epoch = epoch_,
                        .devices = epoch_->Devices(),
                        .snapshots = refreshedSnapshots_.empty() ? epoch_->Snapshots()
                                                                 : std::span<const DeviceSnapshot>{refreshedSnapshots_},
                        .previousSubmission = previousSubmission_ ? &*previousSubmission_ : nullptr,
                        .attempt = attempts_};
            }

            [[nodiscard]] AttemptResult BuildAttempt(const GraphAttemptContext& context) noexcept {
                ++attempts_;
                return std::invoke(factory_, context);
            }

            void Finish(EventObservation observation) noexcept { observation_ = observation; }

            void FailBuild(uint32_t attempt, AttemptError error) noexcept {
                error_.emplace(std::in_place_type<GraphAttemptBuildFailure<AttemptError>>, attempt, std::move(error));
            }

            [[nodiscard]] bool HandleSubmissionFailure(GraphSubmissionError failure) noexcept {
                const uint32_t failedAttempt = attempts_ - 1;
                if (!Retryable(policy_, failure.error.code)) {
                    error_.emplace(std::in_place_type<TerminalGraphSubmissionFailure>, failedAttempt, failure);
                    return true;
                }
                if (attempts_ >= policy_.maximumAttempts) {
                    error_.emplace(std::in_place_type<GraphRetryLimitExceeded>, attempts_, failure);
                    return true;
                }
                previousSubmission_ = failure;
                return false;
            }

            [[nodiscard]] bool PolicyValid() const noexcept { return policy_.Valid(); }
            [[nodiscard]] const GraphRecoveryPolicy& Policy() const noexcept { return policy_; }
            [[nodiscard]] const std::optional<EventObservation>& Observation() const noexcept { return observation_; }
            [[nodiscard]] std::optional<Error>& ErrorResult() noexcept { return error_; }

        private:
            RuntimeRecoveryCoordinator* coordinator_ = nullptr;
            GraphRecoveryPolicy policy_{};
            Factory factory_;
            Provider provider_;
            std::shared_ptr<const RuntimeEpoch> epoch_{};
            std::vector<DeviceSnapshot> refreshedSnapshots_{};
            std::optional<GraphSubmissionError> previousSubmission_{};
            std::optional<EventObservation> observation_{};
            std::optional<Error> error_{};
            uint32_t attempts_ = 0;
        };

        template<typename Error>
        using RecoveringValueSender = decltype(stdexec::just(EventObservation{}));

        template<typename Error>
        using RecoveringErrorSender = decltype(stdexec::just_error(std::declval<Error>()));

        template<typename Error>
        using RecoveringCompletionSender =
            exec::variant_sender<RecoveringValueSender<Error>, RecoveringErrorSender<Error>>;

        template<typename State>
        [[nodiscard]] auto CompleteRecoveringGraph(const std::shared_ptr<State>& state) noexcept
            -> RecoveringCompletionSender<typename State::Error> {
            if (state->Observation()) {
                return stdexec::just(*state->Observation());
            }
            return stdexec::just_error(std::move(*state->ErrorResult()));
        }

        template<typename Error, typename Failure>
        [[nodiscard]] auto NormalizeRecoveringError(Failure&& failure) noexcept -> RecoveringErrorSender<Error> {
            using FailureType = std::remove_cvref_t<Failure>;
            if constexpr (std::same_as<FailureType, Error>) {
                return stdexec::just_error(std::forward<Failure>(failure));
            } else {
                static_assert(std::same_as<FailureType, std::exception_ptr>);
                return stdexec::just_error(
                    Error{std::in_place_type<GraphOrchestrationFailure>, std::forward<Failure>(failure)});
            }
        }

        template<stdexec::scheduler Scheduler, typename State>
        [[nodiscard]] auto SubmitGraphAttemptOn(Scheduler scheduler, ExecutableGraph graph,
                                                const std::shared_ptr<State>& state) {
            return SubmitOn(std::move(scheduler), std::move(graph)) |
                   stdexec::then([state](EventObservation observation) noexcept {
                       state->Finish(observation);
                       return true;
                   }) |
                   stdexec::upon_error([state](GraphSubmissionError failure) noexcept {
                       return state->HandleSubmissionFailure(failure);
                   });
        }

    } // namespace Detail

    /**
     * @brief Build a lazy move-only sender that creates a fresh physical graph for every bounded submission attempt.
     * @details Device loss reconstructs and atomically republishes the runtime
     * epoch before the next factory call. Queue unavailability retries against the
     * same epoch while exposing the previous failure to placement policy.
     */
    template<stdexec::scheduler Scheduler, GraphAttemptFactory Factory, RuntimeEpochRecoveryProvider Provider>
        requires std::is_nothrow_move_constructible_v<Factory> && std::is_nothrow_move_constructible_v<Provider>
    [[nodiscard]] auto RecoveringSubmitOn(Scheduler scheduler, RuntimeRecoveryCoordinator& recovery,
                                          GraphRecoveryPolicy policy, Factory factory, Provider provider) {
        using State = Detail::RecoveringGraphState<Factory, Provider>;
        using Error = typename State::Error;
        auto state = std::make_shared<State>(recovery, policy, std::move(factory), std::move(provider));

        auto effect =
            stdexec::read_env(stdexec::get_stop_token) | stdexec::let_value([scheduler, state](auto stopToken) mutable {
                using DoneSender = decltype(stdexec::just(true));
                using StoppedSender = decltype(stdexec::just_stopped());
                using SubmittedSender =
                    decltype(Detail::SubmitGraphAttemptOn(scheduler, std::declval<ExecutableGraph>(), state));
                using EffectSender = exec::variant_sender<DoneSender, SubmittedSender, StoppedSender>;

                if (stopToken.stop_requested()) {
                    return EffectSender{stdexec::just_stopped()};
                }
                if (!state->PolicyValid()) {
                    state->ErrorResult().emplace(std::in_place_type<InvalidGraphRecoveryPolicy>, state->Policy());
                    return EffectSender{stdexec::just(true)};
                }
                if (!state->PrepareAttempt()) {
                    return EffectSender{stdexec::just(true)};
                }
                const GraphAttemptContext context = state->NextContext();
                auto attempt = state->BuildAttempt(context);
                if (!attempt) {
                    state->FailBuild(context.attempt, std::move(attempt.error()));
                    return EffectSender{stdexec::just(true)};
                }
                auto submitted = Detail::SubmitGraphAttemptOn(scheduler, std::move(*attempt), state);
                return EffectSender{std::move(submitted)};
            });
        return exec::repeat_until(std::move(effect)) |
               stdexec::let_value([state, singleShot = Detail::SingleShotSubmission{}]() noexcept {
                   static_cast<void>(singleShot);
                   return Detail::CompleteRecoveringGraph(state);
               }) |
               stdexec::let_error([]<typename Failure>(Failure& failure) noexcept {
                   return Detail::NormalizeRecoveringError<Error>(std::move(failure));
               });
    }

} // namespace Sora::Compute

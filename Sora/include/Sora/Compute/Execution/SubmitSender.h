/**
 * @file SubmitSender.h
 * @brief Structured asynchronous submission sender for physical compute batches.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Runtime/ExecutableBatch.h>
#include <Sora/Compute/Runtime/ExecutableGraph.h>

#include <exec/variant_sender.hpp>
#include <stdexec/execution.hpp>

#include <cstdint>
#include <utility>

namespace Sora::Compute {

    namespace Detail {

        enum class SubmitChannel : uint8_t {
            Value,
            Error,
            Stopped,
        };

        template<typename Error>
        struct SubmitOutcome {
            SubmitChannel channel = SubmitChannel::Error;
            EventObservation observation{};
            Error error{};
        };

        using SubmitValueSender = decltype(stdexec::just(EventObservation{}));
        using SubmitStoppedSender = decltype(stdexec::just_stopped());

        template<typename Error>
        using SubmitErrorSender = decltype(stdexec::just_error(Error{}));

        template<typename Error>
        using SubmitCompletionSender =
            exec::variant_sender<SubmitValueSender, SubmitErrorSender<Error>, SubmitStoppedSender>;

        template<typename Error>
        [[nodiscard]] SubmitCompletionSender<Error> CompleteSubmit(SubmitOutcome<Error> outcome) noexcept {
            switch (outcome.channel) {
                case SubmitChannel::Value:
                    return stdexec::just(outcome.observation);
                case SubmitChannel::Error:
                    return stdexec::just_error(std::move(outcome.error));
                case SubmitChannel::Stopped:
                    return stdexec::just_stopped();
            }
            std::unreachable();
        }

        template<typename Error, stdexec::scheduler Scheduler, typename Work, typename SubmissionErrorProjection,
                 typename CompletionErrorProjection>
        [[nodiscard]] auto SubmitWorkOn(Scheduler scheduler, Work work,
                                        SubmissionErrorProjection submissionErrorProjection,
                                        CompletionErrorProjection completionErrorProjection) {
            auto operation =
                stdexec::read_env(stdexec::get_stop_token) |
                stdexec::then([work = std::move(work), submissionErrorProjection,
                               completionErrorProjection](auto stopToken) mutable noexcept -> SubmitOutcome<Error> {
                    if (stopToken.stop_requested()) {
                        return {.channel = SubmitChannel::Stopped};
                    }
                    auto submitted = std::move(work).Submit();
                    if (!submitted) {
                        return {.channel = SubmitChannel::Error, .error = submissionErrorProjection(submitted.error())};
                    }
                    const EventObservation observation = submitted->WaitUntilTerminal();
                    if (stopToken.stop_requested() || observation.status == EventStatus::Stopped) {
                        return {.channel = SubmitChannel::Stopped};
                    }
                    if (observation.status == EventStatus::Completed) {
                        return {.channel = SubmitChannel::Value, .observation = observation};
                    }
                    return {.channel = SubmitChannel::Error, .error = completionErrorProjection(observation)};
                }) |
                stdexec::let_value(
                    [](SubmitOutcome<Error> outcome) noexcept { return CompleteSubmit(std::move(outcome)); });
            return stdexec::starts_on(std::move(scheduler), std::move(operation));
        }

    } // namespace Detail

    /**
     * @brief Build a lazy sender that submits and drains @p batch on @p scheduler.
     * @details A stop request before submission prevents backend work. A stop request after submission is reported only
     * after the completion event is drained, preserving all resource leases until the backend is safe.
     */
    template<stdexec::scheduler Scheduler>
    [[nodiscard]] auto SubmitOn(Scheduler scheduler, ExecutableBatch batch) {
        return Detail::SubmitWorkOn<SubmissionError>(
            std::move(scheduler), std::move(batch), [](SubmissionError error) noexcept { return error; },
            [](EventObservation observation) noexcept { return Detail::ObservationError(observation); });
    }

    /**
     * @brief Build a lazy sender that submits, drains, and finalizes @p graph on @p scheduler.
     * @details Submission failures retain their physical node. Completion and publication failures use an invalid node
     * identity because they apply to the accepted graph as a whole.
     */
    template<stdexec::scheduler Scheduler>
    [[nodiscard]] auto SubmitOn(Scheduler scheduler, ExecutableGraph graph) {
        return Detail::SubmitWorkOn<GraphSubmissionError>(
            std::move(scheduler), std::move(graph), [](GraphSubmissionError error) noexcept { return error; },
            [](EventObservation observation) noexcept {
                return GraphSubmissionError{UINT32_MAX, Detail::ObservationError(observation)};
            });
    }

} // namespace Sora::Compute

/**
 * @file PhysicalSegment.h
 * @brief Static sum types unifying queue batches and asynchronous transfers in physical execution graphs.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Runtime/ExecutableBatch.h>
#include <Sora/Compute/Runtime/ExecutableTransfer.h>
#include <Sora/Compute/Runtime/PerformanceModel.h>

#include <atomic>
#include <concepts>
#include <cstdint>
#include <expected>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace Sora::Compute {

    /** @brief Backend event domain in which device-side predecessor waits may be passed directly. */
    struct PhysicalSynchronizationDomain {
        DeviceId device{};
        BackendKind backend = BackendKind::Cpu;

        /** @brief Return whether this domain identifies one backend device event namespace. */
        [[nodiscard]] constexpr bool Valid() const noexcept { return device.Valid() && ValidBackendKind(backend); }

        friend constexpr bool operator==(PhysicalSynchronizationDomain,
                                         PhysicalSynchronizationDomain) noexcept = default;
    };

    namespace Detail {

        /** @brief Map transfer failures into the graph-wide asynchronous submission error domain. */
        [[nodiscard]] constexpr SubmissionError MapTransferSubmissionError(TransferError error) noexcept {
            SubmissionErrorCode code = SubmissionErrorCode::SubmissionFailed;
            switch (error.code) {
                case TransferErrorCode::InvalidApi:
                case TransferErrorCode::InvalidRequest:
                    code = SubmissionErrorCode::InvalidRequest;
                    break;
                case TransferErrorCode::EngineUnavailable:
                    code = SubmissionErrorCode::QueueUnavailable;
                    break;
                case TransferErrorCode::UnsupportedTransfer:
                    code = SubmissionErrorCode::UnsupportedCommand;
                    break;
                case TransferErrorCode::InvalidResult:
                    code = SubmissionErrorCode::InvalidEvent;
                    break;
                case TransferErrorCode::OutOfMemory:
                    code = SubmissionErrorCode::OutOfMemory;
                    break;
                case TransferErrorCode::Stopped:
                    code = SubmissionErrorCode::Stopped;
                    break;
                case TransferErrorCode::DeviceLost:
                    code = SubmissionErrorCode::DeviceLost;
                    break;
                case TransferErrorCode::None:
                case TransferErrorCode::TransferFailed:
                    break;
            }
            return {code, error.detail};
        }

    } // namespace Detail

    /** @brief Accepted queue or transfer work retaining the concrete operation state without type erasure. */
    class PhysicalSubmission final {
    public:
        PhysicalSubmission(const PhysicalSubmission&) = delete;
        PhysicalSubmission& operator=(const PhysicalSubmission&) = delete;
        PhysicalSubmission(PhysicalSubmission&& other) noexcept
            : state_(std::move(other.state_)),
              telemetry_(other.telemetry_),
              observer_(other.observer_),
              telemetryObserved_(other.telemetryObserved_.load(std::memory_order_relaxed)) {
            other.telemetryObserved_.store(true, std::memory_order_relaxed);
        }
        PhysicalSubmission& operator=(PhysicalSubmission&& other) noexcept {
            if (this != &other) {
                state_ = std::move(other.state_);
                telemetry_ = other.telemetry_;
                observer_ = other.observer_;
                telemetryObserved_.store(other.telemetryObserved_.load(std::memory_order_relaxed),
                                         std::memory_order_relaxed);
                other.telemetryObserved_.store(true, std::memory_order_relaxed);
            }
            return *this;
        }

        /** @brief Observe the concrete segment operation state without blocking. */
        [[nodiscard]] EventObservation Query() const noexcept {
            return Observe(std::visit([](const auto& submission) { return submission.Query(); }, state_));
        }

        /** @brief Wait without a deadline until the concrete segment reaches a terminal state. */
        [[nodiscard]] EventObservation WaitUntilTerminal() const noexcept {
            return Observe(std::visit([](const auto& submission) { return submission.WaitUntilTerminal(); }, state_));
        }

        /** @brief Wait for the concrete segment operation state. */
        [[nodiscard]] EventObservation Wait(uint64_t timeoutNanoseconds) const noexcept {
            return Observe(std::visit(
                [timeoutNanoseconds](const auto& submission) { return submission.Wait(timeoutNanoseconds); }, state_));
        }

        /** @brief Return a shared handle to the concrete segment completion event. */
        [[nodiscard]] Event Completion() const noexcept {
            return std::visit([](const auto& submission) { return submission.Completion(); }, state_);
        }

    private:
        friend class PhysicalSegment;

        explicit PhysicalSubmission(Submission submission, KernelPerformanceObservationDescriptor telemetry,
                                    KernelPerformanceObserver observer) noexcept
            : state_(std::move(submission)), telemetry_(telemetry), observer_(observer) {}
        explicit PhysicalSubmission(TransferSubmission submission) noexcept : state_(std::move(submission)) {}

        [[nodiscard]] EventObservation Observe(EventObservation observation) const noexcept {
            if (observation.status == EventStatus::Completed && observation.executionNanoseconds != 0 &&
                telemetry_.Valid() && observer_) {
                bool expected = false;
                if (!telemetryObserved_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                                std::memory_order_relaxed)) {
                    return observation;
                }
                observer_(KernelPerformanceObservation{.key = telemetry_.key,
                                                       .workItemCount = telemetry_.workItemCount,
                                                       .computeNanoseconds = observation.executionNanoseconds,
                                                       .deterministic = telemetry_.deterministic});
            }
            return observation;
        }

        std::variant<Submission, TransferSubmission> state_;
        KernelPerformanceObservationDescriptor telemetry_{};
        KernelPerformanceObserver observer_{};
        mutable std::atomic_bool telemetryObserved_ = false;
    };

    /** @brief Not-yet-submitted queue batch or asynchronous transfer with one static synchronization interface. */
    class PhysicalSegment final {
    public:
        PhysicalSegment(ExecutableBatch batch, KernelPerformanceObservationDescriptor telemetry = {},
                        KernelPerformanceObserver observer = {}) noexcept
            : state_(std::move(batch)), telemetry_(telemetry), observer_(observer) {}
        PhysicalSegment(ExecutableTransfer transfer) noexcept : state_(std::move(transfer)) {}

        PhysicalSegment(const PhysicalSegment&) = delete;
        PhysicalSegment& operator=(const PhysicalSegment&) = delete;
        PhysicalSegment(PhysicalSegment&&) noexcept = default;
        PhysicalSegment& operator=(PhysicalSegment&&) noexcept = default;

        /** @brief Return the event namespace used by direct predecessor waits. */
        [[nodiscard]] PhysicalSynchronizationDomain SynchronizationDomain() const noexcept {
            return std::visit(
                []<typename Segment>(const Segment& segment) {
                    if constexpr (std::same_as<Segment, ExecutableBatch>) {
                        const QueueView queue = segment.QueueIdentity();
                        return PhysicalSynchronizationDomain{queue.device, queue.backend};
                    } else {
                        return PhysicalSynchronizationDomain{segment.CompletionDevice(), segment.CompletionBackend()};
                    }
                },
                state_);
        }

        /** @brief Submit the concrete segment exactly once with graph-derived predecessor waits. */
        [[nodiscard]] std::expected<PhysicalSubmission, SubmissionError>
        Submit(std::vector<Event> dynamicWaits = {}) && noexcept {
            return std::visit(
                [&dynamicWaits,
                 this]<typename Segment>(Segment&& segment) -> std::expected<PhysicalSubmission, SubmissionError> {
                    if constexpr (std::same_as<std::remove_cvref_t<Segment>, ExecutableBatch>) {
                        auto submitted = std::move(segment).Submit(std::move(dynamicWaits));
                        if (!submitted) {
                            return std::unexpected(submitted.error());
                        }
                        return PhysicalSubmission{std::move(*submitted), telemetry_, observer_};
                    } else {
                        auto submitted = std::move(segment).Submit(std::move(dynamicWaits));
                        if (!submitted) {
                            return std::unexpected(Detail::MapTransferSubmissionError(submitted.error()));
                        }
                        return PhysicalSubmission{std::move(*submitted)};
                    }
                },
                std::move(state_));
        }

    private:
        std::variant<ExecutableBatch, ExecutableTransfer> state_;
        KernelPerformanceObservationDescriptor telemetry_{};
        KernelPerformanceObserver observer_{};
    };

    static_assert(std::is_trivially_copyable_v<PhysicalSynchronizationDomain>);
    static_assert(std::is_nothrow_move_constructible_v<PhysicalSegment>);
    static_assert(std::is_nothrow_move_constructible_v<PhysicalSubmission>);

} // namespace Sora::Compute

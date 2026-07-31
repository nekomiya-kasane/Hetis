/**
 * @file Event.h
 * @brief Versioned backend event ABI and shared runtime ownership.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Backend/Abi.h>
#include <Sora/Compute/Backend/Service.h>
#include <Sora/Compute/Device.h>

#include <cassert>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace Sora::Compute {

    /** @brief Public lifecycle state of one asynchronous backend event. */
    enum class EventStatus : uint8_t {
        Pending,
        Completed,
        Failed,
        Stopped,
        DeviceLost,
    };

    /** @brief Stable submission and completion error independent of a driver SDK. */
    enum class SubmissionErrorCode : uint8_t {
        None,
        InvalidRequest,
        QueueUnavailable,
        UnsupportedCommand,
        SubmissionFailed,
        Stopped,
        DeviceLost,
        InvalidEvent,
        PublicationFailed,
        OutOfMemory,
    };

    /** @brief Submission error with an opaque backend-specific diagnostic identity. */
    struct SubmissionError {
        SubmissionErrorCode code = SubmissionErrorCode::None;
        uint64_t detail = 0;

        friend constexpr bool operator==(SubmissionError, SubmissionError) noexcept = default;
    };

    /** @brief Normalized event observation returned by query and wait callbacks. */
    struct EventObservation {
        EventStatus status = EventStatus::Pending;
        SubmissionError error{};
        uint64_t executionNanoseconds = 0;

        /** @brief Return whether the event reached any terminal state. */
        [[nodiscard]] constexpr bool Terminal() const noexcept { return status != EventStatus::Pending; }

        friend constexpr bool operator==(EventObservation, EventObservation) noexcept = default;
    };

    /** @brief Non-owning event view accepted by a compatible backend queue. */
    struct EventView {
        DeviceId device{};
        BackendKind backend = BackendKind::Cpu;
        void* state = nullptr;
        BackendServiceId service{};

        /** @brief Return whether this view denotes a backend event. */
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return state != nullptr && service.Valid(); }

        friend constexpr bool operator==(EventView, EventView) noexcept = default;
    };

    /** @brief Runtime-allocated ABI observation filled by an event callback. */
    struct BackendEventObservation {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(BackendEventObservation);
        EventStatus status = EventStatus::Pending;
        SubmissionError error{};
        uint64_t executionNanoseconds = 0;
    };

    using BackendEventDestroy = void (*)(void* state) noexcept;
    using BackendEventQuery = void (*)(void* state, BackendEventObservation* observation) noexcept;
    using BackendEventWait = void (*)(void* state, uint64_t timeoutNanoseconds,
                                      BackendEventObservation* observation) noexcept;

    /** @brief ABI payload transferring ownership of one backend event to the runtime. */
    struct BackendEventPayload {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(BackendEventPayload);
        DeviceId device{};
        BackendKind backend = BackendKind::Cpu;
        void* state = nullptr;
        BackendEventDestroy destroy = nullptr;
        BackendEventQuery query = nullptr;
        BackendEventWait wait = nullptr;
    };

    /** @brief Failure while adopting an event payload. */
    enum class EventError : uint8_t {
        InvalidPayload,
        IdentityMismatch,
        OutOfMemory,
    };

    namespace Detail {

        [[nodiscard]] constexpr bool ValidSubmissionErrorCode(SubmissionErrorCode code) noexcept {
            return code == SubmissionErrorCode::None || code == SubmissionErrorCode::InvalidRequest ||
                   code == SubmissionErrorCode::QueueUnavailable || code == SubmissionErrorCode::UnsupportedCommand ||
                   code == SubmissionErrorCode::SubmissionFailed || code == SubmissionErrorCode::Stopped ||
                   code == SubmissionErrorCode::DeviceLost || code == SubmissionErrorCode::InvalidEvent ||
                   code == SubmissionErrorCode::PublicationFailed || code == SubmissionErrorCode::OutOfMemory;
        }

        /** @brief Return whether @p code may originate across the backend ABI boundary. */
        [[nodiscard]] constexpr bool ValidBackendSubmissionErrorCode(SubmissionErrorCode code) noexcept {
            return ValidSubmissionErrorCode(code) && code != SubmissionErrorCode::PublicationFailed;
        }

        [[nodiscard]] constexpr EventObservation InvalidEventObservation() noexcept {
            return {.status = EventStatus::Failed, .error = {SubmissionErrorCode::InvalidEvent, 0}};
        }

        [[nodiscard]] constexpr EventObservation NormalizeEventObservation(EventObservation observation) noexcept {
            if (!ValidSubmissionErrorCode(observation.error.code)) {
                return InvalidEventObservation();
            }
            const bool valid =
                ((observation.status == EventStatus::Pending || observation.status == EventStatus::Completed) &&
                 observation.error.code == SubmissionErrorCode::None) ||
                (observation.status == EventStatus::Failed && observation.error.code != SubmissionErrorCode::None) ||
                (observation.status == EventStatus::Stopped &&
                 observation.error.code == SubmissionErrorCode::Stopped) ||
                (observation.status == EventStatus::DeviceLost &&
                 observation.error.code == SubmissionErrorCode::DeviceLost);
            const bool validTiming =
                observation.status == EventStatus::Completed || observation.executionNanoseconds == 0;
            return valid && validTiming ? observation : InvalidEventObservation();
        }

        [[nodiscard]] constexpr EventObservation
        NormalizeEventObservation(const BackendEventObservation& observation) noexcept {
            if (!ValidBackendAbi(observation.abiVersion, observation.structSize, sizeof(BackendEventObservation)) ||
                !ValidBackendSubmissionErrorCode(observation.error.code)) {
                return InvalidEventObservation();
            }
            return NormalizeEventObservation(
                EventObservation{observation.status, observation.error, observation.executionNanoseconds});
        }

    } // namespace Detail

    /** @brief Shared RAII ownership and normalized observation of one backend event. */
    class Event {
        struct Control {
            Control(BackendEventPayload value, BackendService serviceOwner) noexcept
                : payload(value), service(std::move(serviceOwner)) {}
            ~Control() noexcept { payload.destroy(payload.state); }

            BackendEventPayload payload;
            BackendService service;
        };

    public:
        Event() noexcept = default;

        /** @brief Adopt @p payload and arrange exactly-once backend destruction. */
        [[nodiscard]] static std::expected<Event, EventError> Adopt(BackendService service,
                                                                    BackendEventPayload payload) noexcept {
            const bool valid =
                payload.abiVersion == kComputeBackendAbiVersion && payload.structSize >= sizeof(BackendEventPayload) &&
                payload.device.Valid() && ValidBackendKind(payload.backend) && payload.state != nullptr &&
                payload.destroy != nullptr && payload.query != nullptr && payload.wait != nullptr && service;
            if (!valid) {
                if (payload.state != nullptr && payload.destroy != nullptr) {
                    payload.destroy(payload.state);
                }
                return std::unexpected(EventError::InvalidPayload);
            }
            try {
                return Event{std::make_shared<Control>(payload, std::move(service))};
            } catch (const std::bad_alloc&) {
                payload.destroy(payload.state);
                return std::unexpected(EventError::OutOfMemory);
            }
        }

        /** @brief Adopt only when @p payload belongs to the expected backend and device. */
        [[nodiscard]] static std::expected<Event, EventError> Adopt(BackendService service, BackendEventPayload payload,
                                                                    BackendKind expectedBackend,
                                                                    DeviceId expectedDevice) noexcept {
            if (payload.backend != expectedBackend || payload.device != expectedDevice) {
                if (payload.state != nullptr && payload.destroy != nullptr) {
                    payload.destroy(payload.state);
                }
                return std::unexpected(EventError::IdentityMismatch);
            }
            return Adopt(std::move(service), payload);
        }

        /** @brief Return whether this handle owns a backend event. */
        [[nodiscard]] explicit operator bool() const noexcept { return control_ != nullptr; }

        /** @brief Return a non-owning backend wait view. */
        [[nodiscard]] EventView View() const noexcept {
            return control_ != nullptr ? EventView{control_->payload.device, control_->payload.backend,
                                                   control_->payload.state, control_->service.Id()}
                                       : EventView{};
        }

        /** @brief Observe without blocking. @pre This handle is non-empty. */
        [[nodiscard]] EventObservation Query() const noexcept {
            assert(control_ != nullptr);
            BackendEventObservation observation{};
            control_->payload.query(control_->payload.state, &observation);
            return Detail::NormalizeEventObservation(observation);
        }

        /** @brief Wait for completion or until @p timeoutNanoseconds expires. @pre This handle is non-empty. */
        [[nodiscard]] EventObservation Wait(uint64_t timeoutNanoseconds) const noexcept {
            assert(control_ != nullptr);
            BackendEventObservation observation{};
            control_->payload.wait(control_->payload.state, timeoutNanoseconds, &observation);
            return Detail::NormalizeEventObservation(observation);
        }

        /** @brief Wait without a deadline until the backend reports a terminal state. */
        [[nodiscard]] EventObservation WaitUntilTerminal() const noexcept {
            EventObservation observation;
            do {
                observation = Wait(UINT64_MAX);
            } while (observation.status == EventStatus::Pending);
            return observation;
        }

        /** @brief Return the number of shared owners, primarily for lifecycle diagnostics. */
        [[nodiscard]] long UseCount() const noexcept { return control_.use_count(); }

    private:
        explicit Event(std::shared_ptr<Control> control) noexcept : control_(std::move(control)) {}

        std::shared_ptr<Control> control_{};
    };

    static_assert(std::is_trivially_copyable_v<EventView>);
    static_assert(std::is_trivially_copyable_v<BackendEventObservation>);
    static_assert(std::is_trivially_copyable_v<BackendEventPayload>);
    static_assert(std::is_standard_layout_v<BackendEventPayload>);

} // namespace Sora::Compute

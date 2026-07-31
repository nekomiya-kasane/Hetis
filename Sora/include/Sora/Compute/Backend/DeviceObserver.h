/**
 * @file DeviceObserver.h
 * @brief Service-scoped device health observation and generation leases.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Backend/Service.h>
#include <Sora/Compute/Device.h>

#include <cstdint>
#include <expected>
#include <type_traits>
#include <utility>

namespace Sora::Compute {

    using BackendObserveDevice = void (*)(void* state, DeviceSnapshot* snapshot) noexcept;

    /** @brief Allocation-free ABI table for one service-scoped physical device observation source. */
    struct BackendDeviceObserverApiV4 {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(BackendDeviceObserverApiV4);
        DeviceId device{};
        BackendKind backend = BackendKind::Cpu;
        void* state = nullptr;
        BackendObserveDevice observe = nullptr;
    };

    /** @brief Failure to construct or sample a device observer. */
    enum class DeviceObservationError : uint8_t {
        InvalidApi,
        InvalidSnapshot,
        Unavailable,
        Lost,
        GenerationChanged,
    };

    /** @brief Validated observer retaining the backend service and callback code it invokes. */
    class DeviceObserver final {
    public:
        DeviceObserver() noexcept = default;

        /** @brief Validate and retain one immutable observation callback. */
        [[nodiscard]] static std::expected<DeviceObserver, DeviceObservationError>
        Create(BackendService service, BackendDeviceObserverApiV4 api) noexcept {
            if (!service || !ValidBackendAbi(api.abiVersion, api.structSize, sizeof(BackendDeviceObserverApiV4)) ||
                !api.device.Valid() || !ValidBackendKind(api.backend) || api.state == nullptr ||
                api.observe == nullptr) {
                return std::unexpected(DeviceObservationError::InvalidApi);
            }
            return DeviceObserver{std::move(service), api};
        }

        /** @brief Return whether this observer owns a complete service-scoped callback. */
        [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(service_); }

        /** @brief Return the backend service generation owning all observed native resources. */
        [[nodiscard]] BackendServiceId Service() const noexcept { return service_.Id(); }

        /** @brief Return the physical device identity sampled by this observer. */
        [[nodiscard]] constexpr DeviceId Device() const noexcept { return api_.device; }

        /** @brief Return the backend responsible for this device generation. */
        [[nodiscard]] constexpr BackendKind Backend() const noexcept { return api_.backend; }

        /** @brief Sample and validate current device health without allocating or synchronizing command queues. */
        [[nodiscard]] std::expected<DeviceSnapshot, DeviceObservationError> Observe() const noexcept {
            if (!service_) {
                return std::unexpected(DeviceObservationError::InvalidApi);
            }
            DeviceSnapshot snapshot{};
            api_.observe(api_.state, &snapshot);
            if (!ValidDeviceSnapshot(snapshot) || snapshot.id != api_.device) {
                return std::unexpected(DeviceObservationError::InvalidSnapshot);
            }
            return snapshot;
        }

    private:
        DeviceObserver(BackendService service, BackendDeviceObserverApiV4 api) noexcept
            : service_(std::move(service)), api_(api) {}

        BackendService service_{};
        BackendDeviceObserverApiV4 api_{};
    };

    /** @brief Exact service and native-resource generation captured before planning or materialization. */
    class DeviceLease final {
    public:
        DeviceLease() noexcept = default;

        /** @brief Capture an available device generation from @p observer. */
        [[nodiscard]] static std::expected<DeviceLease, DeviceObservationError>
        Capture(DeviceObserver observer) noexcept {
            auto snapshot = observer.Observe();
            if (!snapshot) {
                return std::unexpected(snapshot.error());
            }
            if (snapshot->state == DeviceState::Lost) {
                return std::unexpected(DeviceObservationError::Lost);
            }
            if (snapshot->state != DeviceState::Available) {
                return std::unexpected(DeviceObservationError::Unavailable);
            }
            return DeviceLease{std::move(observer), snapshot->generation};
        }

        /** @brief Return whether this lease has a complete service and nonzero generation. */
        [[nodiscard]] explicit operator bool() const noexcept { return observer_ && generation_ != 0; }

        /** @brief Return the exact backend service owning this generation. */
        [[nodiscard]] BackendServiceId Service() const noexcept { return observer_.Service(); }

        /** @brief Return the observed physical device identity. */
        [[nodiscard]] DeviceId Device() const noexcept { return observer_.Device(); }

        /** @brief Return the observed backend identity. */
        [[nodiscard]] BackendKind Backend() const noexcept { return observer_.Backend(); }

        /** @brief Return the expected native-resource generation. */
        [[nodiscard]] uint64_t Generation() const noexcept { return generation_; }

        /** @brief Verify availability and generation immediately before a side-effecting boundary. */
        [[nodiscard]] std::expected<DeviceSnapshot, DeviceObservationError> Validate() const noexcept {
            if (!*this) {
                return std::unexpected(DeviceObservationError::InvalidApi);
            }
            auto snapshot = observer_.Observe();
            if (!snapshot) {
                return std::unexpected(snapshot.error());
            }
            if (snapshot->generation != generation_) {
                return std::unexpected(DeviceObservationError::GenerationChanged);
            }
            if (snapshot->state == DeviceState::Lost) {
                return std::unexpected(DeviceObservationError::Lost);
            }
            if (snapshot->state != DeviceState::Available) {
                return std::unexpected(DeviceObservationError::Unavailable);
            }
            return snapshot;
        }

    private:
        DeviceLease(DeviceObserver observer, uint64_t generation) noexcept
            : observer_(std::move(observer)), generation_(generation) {}

        DeviceObserver observer_{};
        uint64_t generation_ = 0;
    };

    static_assert(std::is_trivially_copyable_v<BackendDeviceObserverApiV4>);
    static_assert(std::is_standard_layout_v<BackendDeviceObserverApiV4>);

} // namespace Sora::Compute

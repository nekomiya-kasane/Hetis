/**
 * @file RuntimeEpoch.h
 * @brief Immutable physical-resource epoch published by heterogeneous runtime reconstruction.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Runtime/PhysicalResourceCatalog.h>

#include <cstdint>
#include <expected>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Compute {

    /** @brief Static association between one backend service and compiler-independent physical target facts. */
    struct RuntimeDeviceBinding {
        BackendServiceId service{};
        DeviceDescriptor descriptor{};
        RuntimeTargetFacts target{};

        friend constexpr bool operator==(RuntimeDeviceBinding, RuntimeDeviceBinding) noexcept = default;
    };

    /** @brief Stable failure while validating or observing a reconstructed runtime epoch. */
    enum class RuntimeEpochErrorCode : uint8_t {
        InvalidSequence,
        EmptyDeviceSet,
        CapacityExceeded,
        InvalidDeviceBinding,
        DuplicateService,
        DuplicateBackendDevice,
        MissingDeviceObserver,
        IncompatibleDeviceObserver,
        DeviceSetMismatch,
        DeviceObservationFailed,
        DeviceGenerationChanged,
        DeviceLost,
        OutOfMemory,
    };

    /** @brief Runtime-epoch failure with the canonical device index and observation cause. */
    struct RuntimeEpochError {
        RuntimeEpochErrorCode code = RuntimeEpochErrorCode::InvalidSequence;
        uint32_t device = UINT32_MAX;
        DeviceObservationError cause = DeviceObservationError::InvalidApi;

        friend constexpr bool operator==(RuntimeEpochError, RuntimeEpochError) noexcept = default;
    };

    /** @brief One physical coordinate whose backend service was reconstructed between epochs. */
    struct RuntimeDeviceReplacement {
        BackendKind backend = BackendKind::Cpu;
        DeviceId device{};
        BackendServiceId previousService{};
        BackendServiceId replacementService{};
        bool targetChanged = false;

        friend constexpr bool operator==(RuntimeDeviceReplacement, RuntimeDeviceReplacement) noexcept = default;
    };

    /** @brief Canonical service and physical-device difference between two immutable runtime epochs. */
    struct RuntimeEpochDiff {
        std::vector<BackendServiceId> retainedServices{};
        std::vector<BackendServiceId> removedServices{};
        std::vector<BackendServiceId> addedServices{};
        std::vector<RuntimeDeviceReplacement> replacements{};
    };

    /** @brief Stable failure while comparing service-instance identity across runtime epochs. */
    enum class RuntimeEpochDiffErrorCode : uint8_t {
        ServiceIdentityConflict,
        OutOfMemory,
    };

    /** @brief Epoch-difference failure preserving the conflicting service identity. */
    struct RuntimeEpochDiffError {
        RuntimeEpochDiffErrorCode code = RuntimeEpochDiffErrorCode::ServiceIdentityConflict;
        BackendServiceId service{};

        friend constexpr bool operator==(RuntimeEpochDiffError, RuntimeEpochDiffError) noexcept = default;
    };

    /** @brief Immutable catalog and device set published as one all-or-nothing runtime generation. */
    class RuntimeEpoch final {
    public:
        RuntimeEpoch(const RuntimeEpoch&) = delete;
        RuntimeEpoch& operator=(const RuntimeEpoch&) = delete;
        RuntimeEpoch(RuntimeEpoch&&) noexcept = default;
        RuntimeEpoch& operator=(RuntimeEpoch&&) noexcept = default;

        /** @brief Validate, observe, canonicalize, and atomically construct one unpublished epoch value. */
        [[nodiscard]] static std::expected<RuntimeEpoch, RuntimeEpochError>
        Create(uint64_t sequence, PhysicalResourceCatalog resources,
               std::vector<RuntimeDeviceBinding> devices) noexcept;

        /** @brief Return the monotonic process-local publication sequence. */
        [[nodiscard]] uint64_t Sequence() const noexcept { return sequence_; }

        /** @brief Return the immutable resource catalog owned by this epoch. */
        [[nodiscard]] const PhysicalResourceCatalog& Resources() const noexcept { return resources_; }

        /** @brief Return the canonical immutable service, descriptor, and runtime-target bindings. */
        [[nodiscard]] std::span<const RuntimeDeviceBinding> Devices() const noexcept { return devices_; }

        /** @brief Return initial observations indexed identically to @ref Devices. */
        [[nodiscard]] std::span<const DeviceSnapshot> Snapshots() const noexcept { return snapshots_; }

        /** @brief Resample every device without mutating this epoch or conflating telemetry with generation. */
        [[nodiscard]] std::expected<std::vector<DeviceSnapshot>, RuntimeEpochError>
        RefreshDeviceSnapshots() const noexcept;

    private:
        RuntimeEpoch(uint64_t sequence, PhysicalResourceCatalog resources, std::vector<RuntimeDeviceBinding> devices,
                     std::vector<DeviceSnapshot> snapshots) noexcept
            : sequence_(sequence),
              resources_(std::move(resources)),
              devices_(std::move(devices)),
              snapshots_(std::move(snapshots)) {}

        uint64_t sequence_ = 0;
        PhysicalResourceCatalog resources_;
        std::vector<RuntimeDeviceBinding> devices_;
        std::vector<DeviceSnapshot> snapshots_;
    };

    /**
     * @brief Compare two immutable epochs without consulting backend state.
     * @details Retaining one BackendServiceId requires an identical binding. Replacements match the same physical
     * backend/device coordinate under a different service and report whether target compatibility also changed.
     */
    [[nodiscard]] std::expected<RuntimeEpochDiff, RuntimeEpochDiffError>
    DiffRuntimeEpochs(const RuntimeEpoch& previous, const RuntimeEpoch& replacement) noexcept;

    static_assert(std::is_trivially_copyable_v<RuntimeDeviceBinding>);
    static_assert(std::is_trivially_copyable_v<RuntimeDeviceReplacement>);

} // namespace Sora::Compute

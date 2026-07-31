/**
 * @file HostTransferBridge.h
 * @brief Portable inter-service host bridges and bounded transfer calibration.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Backend/HostMemoryApi.h>
#include <Sora/Compute/Backend/TransferApi.h>
#include <Sora/Compute/Runtime/PerformanceModel.h>
#include <Sora/Compute/Runtime/PhysicalResourceCatalog.h>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <type_traits>
#include <vector>

namespace Sora::Compute {

    inline constexpr size_t kHostTransferCalibrationPayloadCount = 4;
    inline constexpr uint32_t kMaximumHostTransferCalibrationWarmups = 8;
    inline constexpr uint32_t kMaximumHostTransferCalibrationSamples = 31;
    inline constexpr uint64_t kMaximumHostTransferCalibrationPayloadBytes = uint64_t{1} << 30U;

    /** @brief Bounded policy for measuring each discovered inter-service host bridge. */
    struct HostTransferCalibrationConfig {
        std::array<uint64_t, kHostTransferCalibrationPayloadCount> payloadBytes{1, 4U << 10U, 1U << 20U, 16U << 20U};
        uint32_t warmupSamples = 1;
        uint32_t measuredSamples = 5;

        /** @brief Return whether sample counts and payload buckets are finite and strictly increasing. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            if (warmupSamples > kMaximumHostTransferCalibrationWarmups || measuredSamples == 0 ||
                measuredSamples > kMaximumHostTransferCalibrationSamples) {
                return false;
            }
            for (size_t index = 0; index < payloadBytes.size(); ++index) {
                if (payloadBytes[index] == 0 || payloadBytes[index] > kMaximumHostTransferCalibrationPayloadBytes ||
                    (index != 0 && payloadBytes[index - 1] >= payloadBytes[index])) {
                    return false;
                }
            }
            return true;
        }
    };

    /** @brief Stable stage at which host-bridge synthesis or calibration failed. */
    enum class HostTransferBridgeErrorCode : uint8_t {
        InvalidConfiguration,
        InvalidAccessorSet,
        CapacityExceeded,
        InsufficientModelCapacity,
        EngineConstructionFailed,
        MissingResource,
        AllocationFailed,
        MappingFailed,
        TransferFailed,
        VerificationFailed,
        CalibrationRejected,
        OutOfMemory,
    };

    /** @brief Host-bridge failure preserving the exact edge, sample, and nested facade cause. */
    struct HostTransferBridgeError {
        HostTransferBridgeErrorCode code = HostTransferBridgeErrorCode::InvalidConfiguration;
        uint32_t edge = UINT32_MAX;
        uint32_t payload = UINT32_MAX;
        MemoryAllocationError allocation{};
        HostMemoryError mapping{};
        TransferError transfer{};
        PerformanceModelError calibration = PerformanceModelError::InvalidObservation;

        friend constexpr bool operator==(HostTransferBridgeError, HostTransferBridgeError) noexcept = default;
    };

    /** @brief Synthesize every directed inter-service bridge from explicit coherent host accessors. */
    [[nodiscard]] std::expected<std::vector<TransferEngine>, HostTransferBridgeError>
    CreateHostTransferBridges(std::span<const HostMemoryAccessor> accessors) noexcept;

    /** @brief Measure every host bridge and publish payload-specific observations into @p performance. */
    [[nodiscard]] std::expected<void, HostTransferBridgeError>
    CalibrateHostTransferBridges(const PhysicalResourceCatalog& resources, PerformanceModel& performance,
                                 HostTransferCalibrationConfig config = {}) noexcept;

    static_assert(std::is_trivially_copyable_v<HostTransferCalibrationConfig>);

} // namespace Sora::Compute

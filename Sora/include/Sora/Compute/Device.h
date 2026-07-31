/**
 * @file Device.h
 * @brief Backend-independent physical compute-device identity and capability model.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/KernelIR.h>

#include <bit>
#include <compare>
#include <concepts>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace Sora::Compute {

    /** @brief Stable physical or logical device identity supplied by a backend. */
    struct DeviceId {
        uint64_t low = 0;
        uint64_t high = 0;

        /** @brief Return whether this identity denotes a physical or logical device. */
        [[nodiscard]] constexpr bool Valid() const noexcept { return low != 0 || high != 0; }

        friend constexpr bool operator==(DeviceId, DeviceId) noexcept = default;
        friend constexpr auto operator<=>(DeviceId, DeviceId) noexcept = default;
    };

    /** @brief Compiler and execution API responsible for one device. */
    enum class BackendKind : uint8_t {
        Cpu,
        Vulkan,
        Cuda,
        Hip,
        Metal,
        WebGpu,
        Remote,
    };

    /** @brief Return whether @p backend is a defined backend kind. */
    [[nodiscard]] constexpr bool ValidBackendKind(BackendKind backend) noexcept {
        return backend == BackendKind::Cpu || backend == BackendKind::Vulkan || backend == BackendKind::Cuda ||
               backend == BackendKind::Hip || backend == BackendKind::Metal || backend == BackendKind::WebGpu ||
               backend == BackendKind::Remote;
    }

    /** @brief Physical execution architecture. */
    enum class DeviceKind : uint8_t {
        Cpu,
        IntegratedGpu,
        DiscreteGpu,
        Accelerator,
        Remote,
    };

    /** @brief Return whether @p kind is a defined physical execution architecture. */
    [[nodiscard]] constexpr bool ValidDeviceKind(DeviceKind kind) noexcept {
        return kind == DeviceKind::Cpu || kind == DeviceKind::IntegratedGpu || kind == DeviceKind::DiscreteGpu ||
               kind == DeviceKind::Accelerator || kind == DeviceKind::Remote;
    }

    /** @brief Stable feature bits used as hard lowering and placement constraints. */
    enum class DeviceFeature : uint64_t {
        Float16 = uint64_t{1} << 0,
        Float64 = uint64_t{1} << 1,
        Int64 = uint64_t{1} << 2,
        FusedMultiplyAdd = uint64_t{1} << 3,
        Subgroup = uint64_t{1} << 4,
        SubgroupShuffle = uint64_t{1} << 5,
        AtomicFloat32 = uint64_t{1} << 6,
        AtomicFloat64 = uint64_t{1} << 7,
        UnifiedMemory = uint64_t{1} << 8,
        TimelineEvents = uint64_t{1} << 9,
        AsynchronousCopy = uint64_t{1} << 10,
        DeviceAddress = uint64_t{1} << 11,
        Int8 = uint64_t{1} << 12,
        Int16 = uint64_t{1} << 13,
    };

    /** @brief Compact set of device features with constexpr subset algebra. */
    class DeviceFeatureMask {
    public:
        constexpr DeviceFeatureMask() noexcept = default;
        constexpr DeviceFeatureMask(DeviceFeature feature) noexcept : bits_(std::to_underlying(feature)) {}
        constexpr explicit DeviceFeatureMask(uint64_t bits) noexcept : bits_(bits) {}

        /** @brief Return the stable raw bit representation. */
        [[nodiscard]] constexpr uint64_t Bits() const noexcept { return bits_; }

        /** @brief Return whether every feature in @p required is present. */
        [[nodiscard]] constexpr bool Contains(DeviceFeatureMask required) const noexcept {
            return (bits_ & required.bits_) == required.bits_;
        }

        /** @brief Return the number of represented features. */
        [[nodiscard]] constexpr int Count() const noexcept { return std::popcount(bits_); }

        friend constexpr bool operator==(DeviceFeatureMask, DeviceFeatureMask) noexcept = default;

        friend constexpr DeviceFeatureMask operator|(DeviceFeatureMask left, DeviceFeatureMask right) noexcept {
            return DeviceFeatureMask(left.bits_ | right.bits_);
        }

        constexpr DeviceFeatureMask& operator|=(DeviceFeatureMask right) noexcept {
            bits_ |= right.bits_;
            return *this;
        }

    private:
        uint64_t bits_ = 0;
    };

    /** @brief Build one device-feature set from individual enumerators. */
    template<typename... Features>
        requires(sizeof...(Features) > 0 && (std::same_as<Features, DeviceFeature> && ...))
    [[nodiscard]] constexpr DeviceFeatureMask DeviceFeatures(Features... features) noexcept {
        return (DeviceFeatureMask(features) | ...);
    }

    /** @brief Immutable capabilities and topology-independent limits of one device. */
    struct DeviceDescriptor {
        DeviceId id{};
        BackendKind backend = BackendKind::Cpu;
        DeviceKind kind = DeviceKind::Cpu;
        DeviceFeatureMask features{};
        uint64_t architectureId = 0;
        uint64_t totalMemoryBytes = 0;
        uint64_t maximumAllocationBytes = 0;
        uint32_t computeUnitCount = 0;
        uint32_t subgroupWidthMinimum = 1;
        uint32_t subgroupWidthMaximum = 1;
        uint32_t maximumWorkgroupInvocations = 1;

        friend constexpr bool operator==(DeviceDescriptor, DeviceDescriptor) noexcept = default;
    };

    /** @brief Return whether one immutable device descriptor is structurally consistent. */
    [[nodiscard]] constexpr bool ValidDeviceDescriptor(const DeviceDescriptor& descriptor) noexcept {
        return descriptor.id.Valid() && ValidBackendKind(descriptor.backend) && ValidDeviceKind(descriptor.kind) &&
               descriptor.subgroupWidthMinimum != 0 &&
               descriptor.subgroupWidthMinimum <= descriptor.subgroupWidthMaximum &&
               descriptor.subgroupWidthMinimum <= descriptor.maximumWorkgroupInvocations;
    }

    /** @brief Runtime health of a device at one observation. */
    enum class DeviceState : uint8_t {
        Available,
        Unavailable,
        Lost,
    };

    /**
     * @brief Dynamic state sampled by a planner immediately before placement.
     * @details Generation changes only when native resources from an earlier generation become unusable.
     * Observation sequence may advance for ordinary submissions and telemetry updates without invalidating plans.
     */
    struct DeviceSnapshot {
        DeviceId id{};
        DeviceState state = DeviceState::Unavailable;
        uint64_t generation = 0;
        uint64_t observationSequence = 0;
        uint64_t availableMemoryBytes = 0;
        uint64_t estimatedQueueDelayNanoseconds = 0;
        uint16_t loadPermille = 0;

        friend constexpr bool operator==(DeviceSnapshot, DeviceSnapshot) noexcept = default;
    };

    /** @brief Return whether @p state is a defined runtime device state. */
    [[nodiscard]] constexpr bool ValidDeviceState(DeviceState state) noexcept {
        return state == DeviceState::Available || state == DeviceState::Unavailable || state == DeviceState::Lost;
    }

    /** @brief Return whether one device observation is internally consistent. */
    [[nodiscard]] constexpr bool ValidDeviceSnapshot(const DeviceSnapshot& snapshot) noexcept {
        return snapshot.id.Valid() && ValidDeviceState(snapshot.state) && snapshot.generation != 0 &&
               snapshot.loadPermille <= 1000;
    }

} // namespace Sora::Compute

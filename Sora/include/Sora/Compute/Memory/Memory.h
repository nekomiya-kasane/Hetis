/**
 * @file Memory.h
 * @brief Backend-independent identities, regions, properties, and errors for compute memory.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Device.h>

#include <bit>
#include <compare>
#include <cstdint>
#include <expected>
#include <type_traits>
#include <utility>

namespace Sora::Compute {

    inline constexpr uint8_t kMaximumBufferRank = 8;

    /** @brief Stable identity of one logical buffer. */
    struct BufferId {
        uint64_t low = 0;
        uint64_t high = 0;

        /** @brief Return whether this identity denotes a logical buffer. */
        [[nodiscard]] constexpr bool Valid() const noexcept { return low != 0 || high != 0; }

        friend constexpr bool operator==(BufferId, BufferId) noexcept = default;
        friend constexpr auto operator<=>(BufferId, BufferId) noexcept = default;
    };

    /** @brief Identity of one mutually coherent memory and addressability domain. */
    struct MemoryDomainId {
        uint64_t low = 0;
        uint64_t high = 0;

        /** @brief Return whether this identity denotes a coherence domain. */
        [[nodiscard]] constexpr bool Valid() const noexcept { return low != 0 || high != 0; }

        friend constexpr bool operator==(MemoryDomainId, MemoryDomainId) noexcept = default;
        friend constexpr auto operator<=>(MemoryDomainId, MemoryDomainId) noexcept = default;
    };

    /** @brief Stable identity of one physical byte-addressable storage object across imported backend handles. */
    struct PhysicalMemoryId {
        uint64_t low = 0;
        uint64_t high = 0;

        /** @brief Return whether this identity denotes physical storage. */
        [[nodiscard]] constexpr bool Valid() const noexcept { return low != 0 || high != 0; }

        friend constexpr bool operator==(PhysicalMemoryId, PhysicalMemoryId) noexcept = default;
        friend constexpr auto operator<=>(PhysicalMemoryId, PhysicalMemoryId) noexcept = default;
    };

    /** @brief Addressability of one backend allocation handle within a coherence domain. */
    struct MemoryLocation {
        MemoryDomainId domain{};
        DeviceId device{};
        BackendKind backend = BackendKind::Cpu;

        /** @brief Return whether every identity and enum value is valid. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return domain.Valid() && device.Valid() && ValidBackendKind(backend);
        }

        friend constexpr bool operator==(MemoryLocation, MemoryLocation) noexcept = default;
        friend constexpr auto operator<=>(MemoryLocation, MemoryLocation) noexcept = default;
    };

    /** @brief Stable properties of a memory domain or allocation. */
    enum class MemoryProperty : uint32_t {
        HostVisible = uint32_t{1} << 0,
        HostCoherent = uint32_t{1} << 1,
        DeviceLocal = uint32_t{1} << 2,
        Unified = uint32_t{1} << 3,
        Remote = uint32_t{1} << 4,
    };

    /** @brief Compact memory-property set with constexpr subset algebra. */
    class MemoryPropertyMask {
    public:
        constexpr MemoryPropertyMask() noexcept = default;
        constexpr MemoryPropertyMask(MemoryProperty property) noexcept : bits_(std::to_underlying(property)) {}
        constexpr explicit MemoryPropertyMask(uint32_t bits) noexcept : bits_(bits) {}

        /** @brief Return the stable raw bit representation. */
        [[nodiscard]] constexpr uint32_t Bits() const noexcept { return bits_; }

        /** @brief Return whether no unknown property bit is set. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            constexpr uint32_t known =
                std::to_underlying(MemoryProperty::HostVisible) | std::to_underlying(MemoryProperty::HostCoherent) |
                std::to_underlying(MemoryProperty::DeviceLocal) | std::to_underlying(MemoryProperty::Unified) |
                std::to_underlying(MemoryProperty::Remote);
            return (bits_ & ~known) == 0;
        }

        /** @brief Return whether every property in @p required is present. */
        [[nodiscard]] constexpr bool Contains(MemoryPropertyMask required) const noexcept {
            return (bits_ & required.bits_) == required.bits_;
        }

        friend constexpr bool operator==(MemoryPropertyMask, MemoryPropertyMask) noexcept = default;

        friend constexpr MemoryPropertyMask operator|(MemoryPropertyMask left, MemoryPropertyMask right) noexcept {
            return MemoryPropertyMask(left.bits_ | right.bits_);
        }

    private:
        uint32_t bits_ = 0;
    };

    /** @brief Form the property set containing @p left and @p right. */
    [[nodiscard]] constexpr MemoryPropertyMask operator|(MemoryProperty left, MemoryProperty right) noexcept {
        return MemoryPropertyMask(left) | MemoryPropertyMask(right);
    }

    /** @brief Recoverable validation or residency failure in the compute-memory model. */
    enum class MemoryError : uint8_t {
        InvalidDescriptor,
        InvalidRegion,
        InvalidAllocation,
        InvalidService,
        RetiredService,
        StaleVersion,
        NotResident,
        VersionExhausted,
        OutOfMemory,
    };

    /** @brief Overflow-safe half-open byte interval. */
    class ByteRegion {
    public:
        constexpr ByteRegion() noexcept = default;

        /** @brief Construct the half-open interval [@p offset, @p offset + @p size). */
        [[nodiscard]] static constexpr std::expected<ByteRegion, MemoryError> Make(uint64_t offset,
                                                                                   uint64_t size) noexcept {
            if (size > UINT64_MAX - offset) {
                return std::unexpected(MemoryError::InvalidRegion);
            }
            return ByteRegion{offset, size};
        }

        /** @brief Return the interval offset in bytes. */
        [[nodiscard]] constexpr uint64_t Offset() const noexcept { return offset_; }

        /** @brief Return the interval size in bytes. */
        [[nodiscard]] constexpr uint64_t Size() const noexcept { return size_; }

        /** @brief Return the exclusive interval end. */
        [[nodiscard]] constexpr uint64_t End() const noexcept { return offset_ + size_; }

        /** @brief Return whether this interval contains every byte of @p other. */
        [[nodiscard]] constexpr bool Contains(ByteRegion other) const noexcept {
            return other.Offset() >= Offset() && other.End() <= End();
        }

        /** @brief Return whether this interval and @p other share at least one byte. */
        [[nodiscard]] constexpr bool Intersects(ByteRegion other) const noexcept {
            return Size() != 0 && other.Size() != 0 && Offset() < other.End() && other.Offset() < End();
        }

        friend constexpr bool operator==(ByteRegion, ByteRegion) noexcept = default;
        friend constexpr auto operator<=>(ByteRegion, ByteRegion) noexcept = default;

    private:
        constexpr ByteRegion(uint64_t offset, uint64_t size) noexcept : offset_(offset), size_(size) {}

        uint64_t offset_ = 0;
        uint64_t size_ = 0;
    };

    /** @brief One overflow-safe byte interval in a physical storage object's canonical address space. */
    struct PhysicalMemoryRange {
        PhysicalMemoryId memory{};
        ByteRegion bytes{};

        /** @brief Return whether this range denotes a non-empty physical interval. */
        [[nodiscard]] constexpr bool Valid() const noexcept { return memory.Valid() && bytes.Size() != 0; }

        /** @brief Return whether two handles resolve to intersecting bytes of the same physical storage. */
        [[nodiscard]] constexpr bool Intersects(PhysicalMemoryRange other) const noexcept {
            return memory == other.memory && bytes.Intersects(other.bytes);
        }

        friend constexpr bool operator==(PhysicalMemoryRange, PhysicalMemoryRange) noexcept = default;
    };

    static_assert(std::is_trivially_copyable_v<BufferId>);
    static_assert(std::is_trivially_copyable_v<MemoryDomainId>);
    static_assert(std::is_trivially_copyable_v<PhysicalMemoryId>);
    static_assert(std::is_trivially_copyable_v<MemoryLocation>);
    static_assert(std::is_trivially_copyable_v<ByteRegion>);
    static_assert(std::is_trivially_copyable_v<PhysicalMemoryRange>);

} // namespace Sora::Compute

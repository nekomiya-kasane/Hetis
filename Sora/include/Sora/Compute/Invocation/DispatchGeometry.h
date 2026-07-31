/**
 * @file DispatchGeometry.h
 * @brief Canonical workgroup geometry shared by pure planning and backend lowering.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Invocation/DispatchPartition.h>

#include <array>
#include <cstdint>
#include <expected>
#include <limits>
#include <type_traits>

namespace Sora::Compute {

    /** @brief Canonical local workgroup size selected solely from the portable dispatch rank. */
    [[nodiscard]] constexpr std::array<uint32_t, 3> CanonicalDispatchLocalSize(uint8_t rank) noexcept {
        switch (rank) {
            case 1:
                return {64, 1, 1};
            case 2:
                return {8, 8, 1};
            case 3:
                return {4, 4, 4};
            default:
                return {};
        }
    }

    /** @brief Backend command geometry derived from one validated portable index domain. */
    struct DispatchGeometry {
        std::array<uint32_t, 3> workgroupCount{};

        /** @brief Return whether every backend launch dimension is nonzero. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return workgroupCount[0] != 0 && workgroupCount[1] != 0 && workgroupCount[2] != 0;
        }

        friend constexpr bool operator==(DispatchGeometry, DispatchGeometry) noexcept = default;
    };

    /** @brief Stable reason a portable dispatch domain cannot be represented by the backend command ABI. */
    enum class DispatchGeometryError : uint8_t {
        InvalidDomain,
        CapacityExceeded,
    };

    /** @brief Derive the exact command geometry consumed by every current lowering provider. */
    [[nodiscard]] constexpr std::expected<DispatchGeometry, DispatchGeometryError>
    PlanDispatchSliceGeometry(const DispatchSlice& slice) noexcept {
        const auto localSize = CanonicalDispatchLocalSize(slice.rank);
        if (slice.rank == 0 || slice.rank > localSize.size() || slice.workItemCount == 0 || localSize[0] == 0) {
            return std::unexpected(DispatchGeometryError::InvalidDomain);
        }
        DispatchGeometry result{.workgroupCount = {1, 1, 1}};
        uint64_t workItems = 1;
        for (uint8_t axis = 0; axis < kMaximumIndexRank; ++axis) {
            if (axis >= slice.rank) {
                if (slice.origin[axis] != 0 || slice.extents[axis] != 0) {
                    return std::unexpected(DispatchGeometryError::InvalidDomain);
                }
                continue;
            }
            const uint64_t extent = slice.extents[axis];
            if (extent == 0 || workItems > std::numeric_limits<uint64_t>::max() / extent) {
                return std::unexpected(DispatchGeometryError::InvalidDomain);
            }
            workItems *= extent;
            const uint64_t groups = 1 + (extent - 1) / localSize[axis];
            if (groups > std::numeric_limits<uint32_t>::max()) {
                return std::unexpected(DispatchGeometryError::CapacityExceeded);
            }
            result.workgroupCount[axis] = static_cast<uint32_t>(groups);
        }
        if (workItems != slice.workItemCount) {
            return std::unexpected(DispatchGeometryError::InvalidDomain);
        }
        return result;
    }

    /** @brief Derive geometry for the unique whole-domain slice. */
    [[nodiscard]] constexpr std::expected<DispatchGeometry, DispatchGeometryError>
    PlanDispatchGeometry(const DispatchDomain& domain) noexcept {
        return PlanDispatchSliceGeometry(DispatchSlice::Whole(domain));
    }

    static_assert(std::is_trivially_copyable_v<DispatchGeometry>);

} // namespace Sora::Compute

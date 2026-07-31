/**
 * @file AffineByteFootprint.h
 * @brief Compressed exact byte occupancy for finite non-overlapping affine layouts.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Memory/Memory.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <type_traits>

namespace Sora::Compute {

    class BufferView;
    class ByteCoverageView;
    class TranslatedByteCoverage;

    /** @brief Stable failure while forming or projecting an affine byte footprint. */
    enum class AffineByteFootprintError : uint8_t {
        InvalidDescriptor,
        ArithmeticOverflow,
        OverlappingElements,
        CapacityExceeded,
    };

    /** @brief Fixed-capacity canonical projection of an affine footprint into sorted, disjoint byte intervals. */
    template<size_t Capacity>
        requires(Capacity != 0)
    class CanonicalByteRegions final {
        friend class AffineByteFootprint;
        friend class ByteCoverageView;
        friend class TranslatedByteCoverage;

    public:
        /** @brief Return the active canonical byte intervals. */
        [[nodiscard]] constexpr std::span<const ByteRegion> Regions() const noexcept {
            return {regions_.data(), size_};
        }

        /** @brief Return the number of active canonical intervals. */
        [[nodiscard]] constexpr size_t Size() const noexcept { return size_; }

    private:
        std::array<ByteRegion, Capacity> regions_{};
        size_t size_ = 0;
    };

    /**
     * @brief Exact compressed occupancy of a finite affine byte layout.
     * @details Scalar lane @c lane at logical index @c i occupies
     * `[offset + sum(i[axis] * stride[axis]) + lane * componentStride, scalarBytes)`. Construction proves that
     * all scalar intervals are disjoint. Canonical projection is explicitly capacity bounded, so a sparse footprint
     * never makes planning memory or work proportional to an unbounded element count.
     */
    class AffineByteFootprint final {
        struct Dimension {
            uint64_t stride = 0;
            uint64_t extent = 0;
        };

    public:
        constexpr AffineByteFootprint() noexcept = default;

        /** @brief Validate and compress one ranked affine scalar-lane address map. */
        [[nodiscard]] static constexpr std::expected<AffineByteFootprint, AffineByteFootprintError>
        Create(std::span<const uint64_t> extents, std::span<const uint64_t> byteStrides, uint64_t componentStrideBytes,
               uint64_t offsetBytes, uint64_t scalarBytes, uint8_t lanes) noexcept {
            if (extents.empty() || extents.size() > kMaximumBufferRank || extents.size() != byteStrides.size() ||
                componentStrideBytes == 0 || scalarBytes == 0 || lanes == 0) {
                return std::unexpected(AffineByteFootprintError::InvalidDescriptor);
            }

            AffineByteFootprint result;
            result.rank_ = static_cast<uint8_t>(extents.size());
            result.componentStrideBytes_ = componentStrideBytes;
            result.offsetBytes_ = offsetBytes;
            result.scalarBytes_ = scalarBytes;
            result.lanes_ = lanes;

            std::array<Dimension, kMaximumBufferRank + 1> dimensions{};
            size_t dimensionCount = 0;
            uint64_t occupiedBytes = scalarBytes;
            for (size_t axis = 0; axis < extents.size(); ++axis) {
                if (extents[axis] == 0 || byteStrides[axis] == 0) {
                    return std::unexpected(AffineByteFootprintError::InvalidDescriptor);
                }
                result.extents_[axis] = extents[axis];
                result.byteStrides_[axis] = byteStrides[axis];
                if (!CheckedMultiply(occupiedBytes, extents[axis], occupiedBytes)) {
                    return std::unexpected(AffineByteFootprintError::ArithmeticOverflow);
                }
                if (extents[axis] > 1) {
                    dimensions[dimensionCount++] = {.stride = byteStrides[axis], .extent = extents[axis]};
                }
            }
            if (!CheckedMultiply(occupiedBytes, lanes, occupiedBytes)) {
                return std::unexpected(AffineByteFootprintError::ArithmeticOverflow);
            }
            if (lanes > 1) {
                dimensions[dimensionCount++] = {.stride = componentStrideBytes, .extent = lanes};
            }
            SortDimensions(dimensions, dimensionCount);

            uint64_t boundingSize = scalarBytes;
            for (size_t index = 0; index < dimensionCount; ++index) {
                const Dimension dimension = dimensions[index];
                if (dimension.stride < boundingSize) {
                    return std::unexpected(AffineByteFootprintError::OverlappingElements);
                }
                uint64_t contribution = 0;
                if (!CheckedMultiply(dimension.extent - 1, dimension.stride, contribution) ||
                    !CheckedAdd(boundingSize, contribution, boundingSize)) {
                    return std::unexpected(AffineByteFootprintError::ArithmeticOverflow);
                }
            }
            const auto bounding = ByteRegion::Make(offsetBytes, boundingSize);
            if (!bounding) {
                return std::unexpected(AffineByteFootprintError::ArithmeticOverflow);
            }
            result.boundingRegion_ = *bounding;
            result.occupiedBytes_ = occupiedBytes;
            return result;
        }

        /** @brief Return whether this object denotes a validated non-empty footprint. */
        [[nodiscard]] constexpr bool Valid() const noexcept { return rank_ != 0; }

        /** @brief Return the logical rank. */
        [[nodiscard]] constexpr uint8_t Rank() const noexcept { return rank_; }

        /** @brief Return the active logical extents. */
        [[nodiscard]] constexpr std::span<const uint64_t> Extents() const noexcept { return {extents_.data(), rank_}; }

        /** @brief Return the active logical byte strides. */
        [[nodiscard]] constexpr std::span<const uint64_t> ByteStrides() const noexcept {
            return {byteStrides_.data(), rank_};
        }

        /** @brief Return the byte distance between adjacent scalar lanes. */
        [[nodiscard]] constexpr uint64_t ComponentStrideBytes() const noexcept { return componentStrideBytes_; }

        /** @brief Return the first occupied byte offset. */
        [[nodiscard]] constexpr uint64_t OffsetBytes() const noexcept { return offsetBytes_; }

        /** @brief Return the size of one occupied scalar interval. */
        [[nodiscard]] constexpr uint64_t ScalarBytes() const noexcept { return scalarBytes_; }

        /** @brief Return the number of scalar lanes per logical element. */
        [[nodiscard]] constexpr uint8_t Lanes() const noexcept { return lanes_; }

        /** @brief Return the exact number of occupied bytes, excluding every affine gap. */
        [[nodiscard]] constexpr uint64_t OccupiedByteCount() const noexcept { return occupiedBytes_; }

        /** @brief Return the smallest interval containing every occupied byte. */
        [[nodiscard]] constexpr ByteRegion BoundingRegion() const noexcept { return boundingRegion_; }

        /** @brief Return whether every byte of the bounding interval is occupied. */
        [[nodiscard]] constexpr bool IsContiguous() const noexcept {
            return Valid() && occupiedBytes_ == boundingRegion_.Size();
        }

        /** @brief Return whether @p byteOffset denotes one occupied byte. */
        [[nodiscard]] constexpr bool OccupiesByte(uint64_t byteOffset) const noexcept {
            const auto requested = ByteRegion::Make(byteOffset, 1);
            if (!Valid() || !requested || !boundingRegion_.Contains(*requested)) {
                return false;
            }
            if (IsContiguous()) {
                return true;
            }

            std::array<Dimension, kMaximumBufferRank + 1> dimensions{};
            const size_t dimensionCount = ActiveDimensions(dimensions);
            SortDimensions(dimensions, dimensionCount);

            uint64_t remainder = byteOffset - offsetBytes_;
            for (size_t index = dimensionCount; index-- > 0;) {
                const Dimension dimension = dimensions[index];
                const uint64_t coordinate = remainder / dimension.stride;
                if (coordinate >= dimension.extent) {
                    return false;
                }
                remainder -= coordinate * dimension.stride;
            }
            return remainder < scalarBytes_;
        }

        /** @brief Return whether @p requested lies within one occupied scalar interval. */
        [[nodiscard]] constexpr bool ContainsWithinScalarInterval(ByteRegion requested) const noexcept {
            if (!Valid() || requested.Size() == 0 || !boundingRegion_.Contains(requested)) {
                return false;
            }
            if (IsContiguous()) {
                return true;
            }

            std::array<Dimension, kMaximumBufferRank + 1> dimensions{};
            const size_t dimensionCount = ActiveDimensions(dimensions);
            SortDimensions(dimensions, dimensionCount);
            uint64_t remainder = requested.Offset() - offsetBytes_;
            for (size_t index = dimensionCount; index-- > 0;) {
                const Dimension dimension = dimensions[index];
                const uint64_t coordinate = remainder / dimension.stride;
                if (coordinate >= dimension.extent) {
                    return false;
                }
                remainder -= coordinate * dimension.stride;
            }
            return remainder < scalarBytes_ && requested.Size() <= scalarBytes_ - remainder;
        }

        /** @brief Recover the logical origin of one layout-preserving affine subview. */
        [[nodiscard]] constexpr std::optional<std::array<uint64_t, kMaximumBufferRank>>
        AffineSubviewOrigin(const AffineByteFootprint& requested) const noexcept {
            if (!Valid() || !requested.Valid() || rank_ != requested.rank_ || scalarBytes_ != requested.scalarBytes_ ||
                lanes_ != requested.lanes_ || componentStrideBytes_ != requested.componentStrideBytes_ ||
                offsetBytes_ > requested.offsetBytes_ || !boundingRegion_.Contains(requested.boundingRegion_)) {
                return std::nullopt;
            }

            std::array<uint8_t, kMaximumBufferRank> axisOrder{};
            for (uint8_t axis = 0; axis < rank_; ++axis) {
                if (byteStrides_[axis] != requested.byteStrides_[axis] || extents_[axis] < requested.extents_[axis]) {
                    return std::nullopt;
                }
                axisOrder[axis] = axis;
            }
            for (uint8_t index = 1; index < rank_; ++index) {
                const uint8_t axis = axisOrder[index];
                uint8_t insertion = index;
                while (insertion != 0 && byteStrides_[axisOrder[insertion - 1]] > byteStrides_[axis]) {
                    axisOrder[insertion] = axisOrder[insertion - 1];
                    --insertion;
                }
                axisOrder[insertion] = axis;
            }

            std::array<uint64_t, kMaximumBufferRank> origins{};
            uint64_t remainder = requested.offsetBytes_ - offsetBytes_;
            for (uint8_t index = rank_; index-- > 0;) {
                const uint8_t axis = axisOrder[index];
                const uint64_t coordinate = remainder / byteStrides_[axis];
                if (coordinate >= extents_[axis]) {
                    return std::nullopt;
                }
                origins[axis] = coordinate;
                remainder -= coordinate * byteStrides_[axis];
            }
            if (remainder != 0) {
                return std::nullopt;
            }
            for (uint8_t axis = 0; axis < rank_; ++axis) {
                if (requested.extents_[axis] > extents_[axis] - origins[axis]) {
                    return std::nullopt;
                }
            }
            return origins;
        }

        /** @brief Prove that @p requested is a layout-preserving logical index subview of this footprint. */
        [[nodiscard]] constexpr bool ContainsAffineSubview(const AffineByteFootprint& requested) const noexcept {
            return AffineSubviewOrigin(requested).has_value();
        }

        /**
         * @brief Project exact occupancy into at most @p Capacity sorted, maximally merged intervals.
         * @return A canonical projection, or CapacityExceeded after bounded work when more runs are required.
         */
        template<size_t Capacity>
            requires(Capacity != 0)
        [[nodiscard]] constexpr std::expected<CanonicalByteRegions<Capacity>, AffineByteFootprintError>
        ProjectCanonicalRegions() const noexcept {
            if (!Valid()) {
                return std::unexpected(AffineByteFootprintError::InvalidDescriptor);
            }

            CanonicalByteRegions<Capacity> result;
            if (IsContiguous()) {
                result.regions_[0] = boundingRegion_;
                result.size_ = 1;
                return result;
            }

            std::array<Dimension, kMaximumBufferRank + 1> dimensions{};
            size_t dimensionCount = ActiveDimensions(dimensions);
            SortDimensions(dimensions, dimensionCount);

            uint64_t blockBytes = scalarBytes_;
            size_t firstSparse = 0;
            while (firstSparse < dimensionCount && dimensions[firstSparse].stride == blockBytes) {
                if (!CheckedMultiply(blockBytes, dimensions[firstSparse].extent, blockBytes)) {
                    return std::unexpected(AffineByteFootprintError::ArithmeticOverflow);
                }
                ++firstSparse;
            }

            const size_t sparseCount = dimensionCount - firstSparse;
            std::array<uint64_t, kMaximumBufferRank + 1> indices{};
            uint64_t currentOffset = offsetBytes_;
            while (true) {
                const auto current = ByteRegion::Make(currentOffset, blockBytes);
                if (!current) {
                    return std::unexpected(AffineByteFootprintError::ArithmeticOverflow);
                }
                if (result.size_ != 0 && result.regions_[result.size_ - 1].End() == current->Offset()) {
                    const ByteRegion previous = result.regions_[result.size_ - 1];
                    result.regions_[result.size_ - 1] =
                        *ByteRegion::Make(previous.Offset(), current->End() - previous.Offset());
                } else {
                    if (result.size_ == Capacity) {
                        return std::unexpected(AffineByteFootprintError::CapacityExceeded);
                    }
                    result.regions_[result.size_++] = *current;
                }

                bool advanced = false;
                for (size_t index = 0; index < sparseCount; ++index) {
                    const Dimension dimension = dimensions[firstSparse + index];
                    if (++indices[index] < dimension.extent) {
                        currentOffset += dimension.stride;
                        advanced = true;
                        break;
                    }
                    currentOffset -= (dimension.extent - 1) * dimension.stride;
                    indices[index] = 0;
                }
                if (!advanced) {
                    break;
                }
            }
            return result;
        }

        friend constexpr bool operator==(const AffineByteFootprint&, const AffineByteFootprint&) noexcept = default;
        friend constexpr auto operator<=>(const AffineByteFootprint&, const AffineByteFootprint&) noexcept = default;

    private:
        [[nodiscard]] static constexpr bool CheckedMultiply(uint64_t left, uint64_t right, uint64_t& result) noexcept {
            if (left != 0 && right > UINT64_MAX / left) {
                return false;
            }
            result = left * right;
            return true;
        }

        [[nodiscard]] static constexpr bool CheckedAdd(uint64_t left, uint64_t right, uint64_t& result) noexcept {
            if (right > UINT64_MAX - left) {
                return false;
            }
            result = left + right;
            return true;
        }

        static constexpr void SortDimensions(std::array<Dimension, kMaximumBufferRank + 1>& dimensions,
                                             size_t count) noexcept {
            std::ranges::sort(dimensions.begin(), dimensions.begin() + count, {}, &Dimension::stride);
        }

        [[nodiscard]] constexpr size_t
        ActiveDimensions(std::array<Dimension, kMaximumBufferRank + 1>& dimensions) const noexcept {
            size_t count = 0;
            for (uint8_t axis = 0; axis < rank_; ++axis) {
                if (extents_[axis] > 1) {
                    dimensions[count++] = {.stride = byteStrides_[axis], .extent = extents_[axis]};
                }
            }
            if (lanes_ > 1) {
                dimensions[count++] = {.stride = componentStrideBytes_, .extent = lanes_};
            }
            return count;
        }

        friend class BufferView;

        uint8_t rank_ = 0;
        std::array<uint64_t, kMaximumBufferRank> extents_{};
        std::array<uint64_t, kMaximumBufferRank> byteStrides_{};
        uint64_t componentStrideBytes_ = 0;
        uint64_t offsetBytes_ = 0;
        uint64_t scalarBytes_ = 0;
        uint64_t occupiedBytes_ = 0;
        ByteRegion boundingRegion_{};
        uint8_t lanes_ = 0;
    };

    static_assert(std::is_trivially_copyable_v<AffineByteFootprint>);

} // namespace Sora::Compute

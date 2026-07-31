/**
 * @file BufferView.h
 * @brief Validated affine views over logical compute buffers.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Memory/AffineByteFootprint.h>
#include <Sora/Compute/Memory/Buffer.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <numeric>
#include <span>
#include <type_traits>
#include <utility>

namespace Sora::Compute {

    /** @brief Runtime affine layout of one ranked logical buffer view. */
    struct BufferViewDesc {
        LayoutKind layout = LayoutKind::Contiguous;
        uint8_t rank = 0;
        std::array<uint64_t, kMaximumBufferRank> extents{};
        std::array<uint64_t, kMaximumBufferRank> byteStrides{};
        uint64_t componentStrideBytes = 0;
        uint64_t offsetBytes = 0;

        friend constexpr bool operator==(BufferViewDesc, BufferViewDesc) noexcept = default;
    };

    /** @brief Stable reason a logical buffer view could not be formed. */
    enum class BufferViewError : uint8_t {
        InvalidBuffer,
        InvalidDescriptor,
        IncompatibleLayout,
        ArithmeticOverflow,
        OutOfBounds,
        OutsideLogicalCoverage,
        IndeterminateLogicalCoverage,
        OverlappingElements,
    };

    namespace Detail {

        [[nodiscard]] constexpr bool CheckedMultiplyU64(uint64_t left, uint64_t right, uint64_t& result) noexcept {
            if (left != 0 && right > UINT64_MAX / left) {
                return false;
            }
            result = left * right;
            return true;
        }

        [[nodiscard]] constexpr bool CheckedAddU64(uint64_t left, uint64_t right, uint64_t& result) noexcept {
            if (right > UINT64_MAX - left) {
                return false;
            }
            result = left + right;
            return true;
        }

        [[nodiscard]] constexpr bool ViewLayoutCompatible(LayoutKind storage, LayoutKind view) noexcept {
            if (view == LayoutKind::Strided) {
                return storage != LayoutKind::Scalar;
            }
            return storage == view && (view == LayoutKind::Contiguous || view == LayoutKind::StructureOfArrays);
        }

    } // namespace Detail

    /**
     * @brief Owning logical buffer view with a validated affine footprint.
     * @details Element component @c lane at logical index @c i is addressed as
     * `offset + sum(i[axis] * byteStrides[axis]) + lane * componentStrideBytes`. The stored byte region is the
     * conservative bounding interval used by residency and graph-hazard analysis.
     */
    class BufferView {
    public:
        BufferView() noexcept = default;

        /** @brief Validate @p desc against @p buffer and retain its logical lifetime. */
        [[nodiscard]] static std::expected<BufferView, BufferViewError> Create(Buffer buffer,
                                                                               BufferViewDesc desc) noexcept {
            if (!buffer) {
                return std::unexpected(BufferViewError::InvalidBuffer);
            }
            const BufferDesc storage = buffer.Describe();
            if (!ValidLayoutKind(desc.layout) || desc.layout == LayoutKind::Scalar || desc.rank == 0 ||
                desc.rank > kMaximumBufferRank || desc.rank != storage.rank ||
                !Detail::ViewLayoutCompatible(storage.layout, desc.layout)) {
                return std::unexpected(BufferViewError::IncompatibleLayout);
            }

            const uint64_t scalarBytes = ScalarByteSize(storage.elementType.scalar);
            const uint64_t valueBytes = storage.elementType.ByteSize();
            if (scalarBytes == 0 || valueBytes == 0 || desc.offsetBytes % scalarBytes != 0 ||
                desc.componentStrideBytes == 0 || desc.componentStrideBytes % scalarBytes != 0) {
                return std::unexpected(BufferViewError::InvalidDescriptor);
            }
            for (uint8_t axis = 0; axis < kMaximumBufferRank; ++axis) {
                const bool active = axis < desc.rank;
                if ((active && (desc.extents[axis] == 0 || desc.byteStrides[axis] == 0 ||
                                desc.byteStrides[axis] % scalarBytes != 0)) ||
                    (!active && (desc.extents[axis] != 0 || desc.byteStrides[axis] != 0))) {
                    return std::unexpected(BufferViewError::InvalidDescriptor);
                }
            }

            uint64_t canonicalStride = desc.layout == LayoutKind::StructureOfArrays ? scalarBytes : valueBytes;
            for (uint8_t axis = desc.rank; axis-- > 0;) {
                if ((desc.layout == LayoutKind::Contiguous || desc.layout == LayoutKind::StructureOfArrays) &&
                    desc.byteStrides[axis] != canonicalStride) {
                    return std::unexpected(BufferViewError::InvalidDescriptor);
                }
                if (!Detail::CheckedMultiplyU64(canonicalStride, desc.extents[axis], canonicalStride)) {
                    return std::unexpected(BufferViewError::ArithmeticOverflow);
                }
            }
            if (desc.layout == LayoutKind::Contiguous && desc.componentStrideBytes != scalarBytes) {
                return std::unexpected(BufferViewError::InvalidDescriptor);
            }
            if (desc.layout == LayoutKind::StructureOfArrays && desc.componentStrideBytes < canonicalStride) {
                return std::unexpected(BufferViewError::InvalidDescriptor);
            }

            const auto footprint = AffineByteFootprint::Create(
                std::span<const uint64_t>{desc.extents.data(), desc.rank},
                std::span<const uint64_t>{desc.byteStrides.data(), desc.rank}, desc.componentStrideBytes,
                desc.offsetBytes, scalarBytes, storage.elementType.lanes);
            if (!footprint) {
                switch (footprint.error()) {
                    case AffineByteFootprintError::ArithmeticOverflow:
                        return std::unexpected(BufferViewError::ArithmeticOverflow);
                    case AffineByteFootprintError::OverlappingElements:
                        return std::unexpected(BufferViewError::OverlappingElements);
                    default:
                        return std::unexpected(BufferViewError::InvalidDescriptor);
                }
            }
            if (footprint->BoundingRegion().End() > storage.sizeBytes) {
                return std::unexpected(BufferViewError::OutOfBounds);
            }
            const CoverageContainment containment =
                buffer.LogicalCoverage().Contains(ByteCoverageView::Affine(*footprint));
            if (containment == CoverageContainment::No) {
                return std::unexpected(BufferViewError::OutsideLogicalCoverage);
            }
            if (containment == CoverageContainment::Unknown) {
                return std::unexpected(BufferViewError::IndeterminateLogicalCoverage);
            }

            uint64_t baseAlignment = storage.alignment;
            if (desc.offsetBytes != 0) {
                baseAlignment = std::gcd(baseAlignment, desc.offsetBytes);
            }
            if (desc.layout == LayoutKind::StructureOfArrays && storage.elementType.lanes > 1) {
                baseAlignment = std::gcd(baseAlignment, desc.componentStrideBytes);
            }
            return BufferView{std::move(buffer), desc, footprint->BoundingRegion(), footprint->OccupiedByteCount(),
                              baseAlignment};
        }

        /** @brief Form a canonical row-major AoS view. */
        [[nodiscard]] static std::expected<BufferView, BufferViewError>
        Contiguous(Buffer buffer, std::span<const uint64_t> extents, uint64_t offsetBytes = 0) noexcept {
            if (!buffer || extents.empty() || extents.size() > kMaximumBufferRank) {
                return std::unexpected(BufferViewError::InvalidDescriptor);
            }
            const BufferDesc storage = buffer.Describe();
            BufferViewDesc desc{.layout = LayoutKind::Contiguous,
                                .rank = static_cast<uint8_t>(extents.size()),
                                .componentStrideBytes = ScalarByteSize(storage.elementType.scalar),
                                .offsetBytes = offsetBytes};
            uint64_t stride = storage.elementType.ByteSize();
            for (size_t axis = extents.size(); axis-- > 0;) {
                desc.extents[axis] = extents[axis];
                desc.byteStrides[axis] = stride;
                if (!Detail::CheckedMultiplyU64(stride, extents[axis], stride)) {
                    return std::unexpected(BufferViewError::ArithmeticOverflow);
                }
            }
            return Create(std::move(buffer), desc);
        }

        /** @brief Form a canonical row-major SoA view with optional padded component planes. */
        [[nodiscard]] static std::expected<BufferView, BufferViewError>
        StructureOfArrays(Buffer buffer, std::span<const uint64_t> extents, uint64_t offsetBytes = 0,
                          uint64_t componentStrideBytes = 0) noexcept {
            if (!buffer || extents.empty() || extents.size() > kMaximumBufferRank) {
                return std::unexpected(BufferViewError::InvalidDescriptor);
            }
            const BufferDesc storage = buffer.Describe();
            const uint64_t scalarBytes = ScalarByteSize(storage.elementType.scalar);
            BufferViewDesc desc{.layout = LayoutKind::StructureOfArrays,
                                .rank = static_cast<uint8_t>(extents.size()),
                                .componentStrideBytes = componentStrideBytes,
                                .offsetBytes = offsetBytes};
            uint64_t stride = scalarBytes;
            for (size_t axis = extents.size(); axis-- > 0;) {
                desc.extents[axis] = extents[axis];
                desc.byteStrides[axis] = stride;
                if (!Detail::CheckedMultiplyU64(stride, extents[axis], stride)) {
                    return std::unexpected(BufferViewError::ArithmeticOverflow);
                }
            }
            if (desc.componentStrideBytes == 0) {
                desc.componentStrideBytes = stride;
            }
            return Create(std::move(buffer), desc);
        }

        /** @brief Return whether this handle denotes a validated view. */
        [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(buffer_); }

        /** @brief Return the retained logical buffer. @pre This view is non-empty. */
        [[nodiscard]] const Buffer& LogicalBuffer() const noexcept { return buffer_; }

        /** @brief Return the underlying logical buffer descriptor. @pre This view is non-empty. */
        [[nodiscard]] BufferDesc BufferDescriptor() const noexcept { return buffer_.Describe(); }

        /** @brief Return the validated affine layout. */
        [[nodiscard]] const BufferViewDesc& Describe() const noexcept { return desc_; }

        /** @brief Return the conservative bounding byte interval touched by this view. */
        [[nodiscard]] ByteRegion Region() const noexcept { return region_; }

        /** @brief Return the exact compressed byte occupancy of this validated view. @pre This view is non-empty. */
        [[nodiscard]] AffineByteFootprint Footprint() const noexcept {
            const BufferDesc storage = buffer_.Describe();
            AffineByteFootprint result;
            result.rank_ = desc_.rank;
            result.extents_ = desc_.extents;
            result.byteStrides_ = desc_.byteStrides;
            result.componentStrideBytes_ = desc_.componentStrideBytes;
            result.offsetBytes_ = desc_.offsetBytes;
            result.scalarBytes_ = ScalarByteSize(storage.elementType.scalar);
            result.occupiedBytes_ = occupiedBytes_;
            result.boundingRegion_ = region_;
            result.lanes_ = storage.elementType.lanes;
            return result;
        }

        /** @brief Return the guaranteed alignment of the first element and every SoA component plane. */
        [[nodiscard]] uint64_t BaseAlignment() const noexcept { return baseAlignment_; }

        /** @brief Form an affine subview while retaining the strongest valid layout guarantee. */
        [[nodiscard]] std::expected<BufferView, BufferViewError>
        SubView(std::span<const uint64_t> origin, std::span<const uint64_t> extents) const noexcept {
            if (!*this || origin.size() != desc_.rank || extents.size() != desc_.rank) {
                return std::unexpected(BufferViewError::InvalidDescriptor);
            }
            BufferViewDesc desc = desc_;
            uint64_t offset = desc.offsetBytes;
            for (uint8_t axis = 0; axis < desc.rank; ++axis) {
                if (extents[axis] == 0 || origin[axis] > desc.extents[axis] ||
                    extents[axis] > desc.extents[axis] - origin[axis]) {
                    return std::unexpected(BufferViewError::InvalidDescriptor);
                }
                uint64_t contribution = 0;
                if (!Detail::CheckedMultiplyU64(origin[axis], desc.byteStrides[axis], contribution) ||
                    !Detail::CheckedAddU64(offset, contribution, offset)) {
                    return std::unexpected(BufferViewError::ArithmeticOverflow);
                }
                desc.extents[axis] = extents[axis];
            }
            desc.offsetBytes = offset;
            if (desc.layout == LayoutKind::Contiguous || desc.layout == LayoutKind::StructureOfArrays) {
                const BufferDesc storage = buffer_.Describe();
                uint64_t canonicalStride = desc.layout == LayoutKind::StructureOfArrays
                                               ? ScalarByteSize(storage.elementType.scalar)
                                               : storage.elementType.ByteSize();
                for (uint8_t axis = desc.rank; axis-- > 0;) {
                    if (desc.extents[axis] == 1) {
                        desc.byteStrides[axis] = canonicalStride;
                    } else if (desc.byteStrides[axis] != canonicalStride) {
                        desc.layout = LayoutKind::Strided;
                        break;
                    }
                    if (!Detail::CheckedMultiplyU64(canonicalStride, desc.extents[axis], canonicalStride)) {
                        return std::unexpected(BufferViewError::ArithmeticOverflow);
                    }
                }
            }
            return Create(buffer_, desc);
        }

    private:
        BufferView(Buffer buffer, BufferViewDesc desc, ByteRegion region, uint64_t occupiedBytes,
                   uint64_t baseAlignment) noexcept
            : buffer_(std::move(buffer)),
              desc_(desc),
              region_(region),
              occupiedBytes_(occupiedBytes),
              baseAlignment_(baseAlignment) {}

        Buffer buffer_{};
        BufferViewDesc desc_{};
        ByteRegion region_{};
        uint64_t occupiedBytes_ = 0;
        uint64_t baseAlignment_ = 0;
    };

    static_assert(std::is_trivially_copyable_v<BufferViewDesc>);

} // namespace Sora::Compute

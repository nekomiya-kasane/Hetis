/**
 * @file ByteCoverage.h
 * @brief Exact borrowed byte-set semantics and affine logical-to-physical translation.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Memory/AffineByteFootprint.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <type_traits>

namespace Sora::Compute {

    /** @brief Three-valued result of testing whether one byte coverage contains another. */
    enum class CoverageContainment : uint8_t {
        No,
        Yes,
        Unknown,
    };

    /** @brief Three-valued relation used when exact overlap proof is unavailable within bounded work. */
    enum class CoverageOverlap : uint8_t {
        Disjoint,
        Overlap,
        Unknown,
    };

    /** @brief Three-valued equivalence result for an exact byte-set union. */
    enum class CoverageEquivalence : uint8_t {
        Different,
        Equal,
        Unknown,
    };

    /**
     * @brief Borrowed exact byte coverage with a compact contiguous fast path.
     * @details An affine instance borrows its footprint. The referenced footprint must outlive this view. Queries never
     * enumerate storage proportional to the logical element count; undecidable bounded queries return Unknown.
     */
    class ByteCoverageView final {
    public:
        constexpr ByteCoverageView() noexcept = default;

        /** @brief Form exact coverage of one non-empty contiguous interval. */
        [[nodiscard]] static constexpr ByteCoverageView Interval(ByteRegion region) noexcept {
            ByteCoverageView result;
            if (region.Size() != 0) {
                result.bounding_ = region;
            }
            return result;
        }

        /** @brief Borrow exact coverage from one validated affine footprint. */
        [[nodiscard]] static constexpr ByteCoverageView Affine(const AffineByteFootprint& footprint) noexcept {
            ByteCoverageView result;
            if (footprint.Valid()) {
                result.bounding_ = footprint.BoundingRegion();
                result.affine_ = &footprint;
            }
            return result;
        }

        /** @brief Return whether this view denotes a non-empty exact byte set. */
        [[nodiscard]] constexpr bool Valid() const noexcept { return bounding_.Size() != 0; }

        /** @brief Return whether every byte in the bounding interval is occupied. */
        [[nodiscard]] constexpr bool IsContiguous() const noexcept {
            return Valid() && (affine_ == nullptr || affine_->IsContiguous());
        }

        /** @brief Return the smallest interval containing every occupied byte. */
        [[nodiscard]] constexpr ByteRegion BoundingRegion() const noexcept { return bounding_; }

        /** @brief Return the exact number of occupied bytes. */
        [[nodiscard]] constexpr uint64_t OccupiedByteCount() const noexcept {
            return affine_ != nullptr ? affine_->OccupiedByteCount() : bounding_.Size();
        }

        /** @brief Return the borrowed affine descriptor, or null for the interval fast path. */
        [[nodiscard]] constexpr const AffineByteFootprint* BorrowedAffineFootprint() const noexcept { return affine_; }

        /** @brief Decide whether this coverage contains one contiguous byte interval in O(rank) work. */
        [[nodiscard]] constexpr CoverageContainment Contains(ByteRegion requested) const noexcept {
            if (!Valid() || requested.Size() == 0) {
                return CoverageContainment::No;
            }
            if (affine_ == nullptr) {
                return bounding_.Contains(requested) ? CoverageContainment::Yes : CoverageContainment::No;
            }
            if (!bounding_.Contains(requested) || !affine_->OccupiesByte(requested.Offset()) ||
                !affine_->OccupiesByte(requested.End() - 1)) {
                return CoverageContainment::No;
            }
            if (affine_->ContainsWithinScalarInterval(requested)) {
                return CoverageContainment::Yes;
            }
            return CoverageContainment::Unknown;
        }

        /** @brief Decide coverage containment without unbounded affine expansion. */
        [[nodiscard]] constexpr CoverageContainment Contains(ByteCoverageView requested) const noexcept {
            if (!Valid() || !requested.Valid()) {
                return CoverageContainment::No;
            }
            if (requested.affine_ == nullptr) {
                return Contains(requested.bounding_);
            }
            if (affine_ == nullptr) {
                return bounding_.Contains(requested.bounding_) ? CoverageContainment::Yes : CoverageContainment::No;
            }
            if (*affine_ == *requested.affine_) {
                return CoverageContainment::Yes;
            }
            if (!bounding_.Contains(requested.bounding_) || OccupiedByteCount() < requested.OccupiedByteCount()) {
                return CoverageContainment::No;
            }
            if (affine_->ContainsAffineSubview(*requested.affine_)) {
                return CoverageContainment::Yes;
            }
            return CoverageContainment::Unknown;
        }

        /** @brief Decide whether two exact byte sets overlap without unbounded affine expansion. */
        [[nodiscard]] constexpr CoverageOverlap Relation(ByteCoverageView other) const noexcept {
            if (!Valid() || !other.Valid()) {
                return CoverageOverlap::Unknown;
            }
            if (!bounding_.Intersects(other.bounding_)) {
                return CoverageOverlap::Disjoint;
            }
            if (affine_ == nullptr && other.affine_ == nullptr) {
                return CoverageOverlap::Overlap;
            }
            if (affine_ != nullptr && other.affine_ != nullptr && *affine_ == *other.affine_) {
                return CoverageOverlap::Overlap;
            }
            if (Contains(other.bounding_) == CoverageContainment::Yes ||
                other.Contains(bounding_) == CoverageContainment::Yes) {
                return CoverageOverlap::Overlap;
            }
            return CoverageOverlap::Unknown;
        }

        /** @brief Project exact occupancy into at most @p Capacity canonical intervals. */
        template<size_t Capacity>
            requires(Capacity != 0)
        [[nodiscard]] constexpr std::expected<CanonicalByteRegions<Capacity>, AffineByteFootprintError>
        ProjectCanonicalRegions() const noexcept {
            if (!Valid()) {
                return std::unexpected(AffineByteFootprintError::InvalidDescriptor);
            }
            if (affine_ != nullptr) {
                return affine_->ProjectCanonicalRegions<Capacity>();
            }
            CanonicalByteRegions<Capacity> result;
            result.regions_[0] = bounding_;
            result.size_ = 1;
            return result;
        }

        friend constexpr bool operator==(ByteCoverageView left, ByteCoverageView right) noexcept {
            if ((left.affine_ == nullptr) != (right.affine_ == nullptr)) {
                return false;
            }
            if (left.affine_ == nullptr) {
                return left.bounding_ == right.bounding_;
            }
            return *left.affine_ == *right.affine_;
        }

    private:
        ByteRegion bounding_{};
        const AffineByteFootprint* affine_ = nullptr;
    };

    /** @brief Intersect sibling affine subviews expressed in one shared logical domain. */
    [[nodiscard]] constexpr std::expected<std::optional<AffineByteFootprint>, AffineByteFootprintError>
    IntersectAffineSubviews(ByteCoverageView domain, ByteCoverageView left, ByteCoverageView right) noexcept {
        const AffineByteFootprint* domainAffine = domain.BorrowedAffineFootprint();
        const AffineByteFootprint* leftAffine = left.BorrowedAffineFootprint();
        const AffineByteFootprint* rightAffine = right.BorrowedAffineFootprint();
        const auto leftOrigin = domainAffine != nullptr && leftAffine != nullptr
                                    ? domainAffine->AffineSubviewOrigin(*leftAffine)
                                    : std::nullopt;
        const auto rightOrigin = domainAffine != nullptr && rightAffine != nullptr
                                     ? domainAffine->AffineSubviewOrigin(*rightAffine)
                                     : std::nullopt;
        if (!leftOrigin || !rightOrigin) {
            return std::unexpected(AffineByteFootprintError::InvalidDescriptor);
        }

        std::array<uint64_t, kMaximumBufferRank> extents{};
        std::array<uint64_t, kMaximumBufferRank> origin{};
        for (uint8_t axis = 0; axis < domainAffine->Rank(); ++axis) {
            origin[axis] = std::max((*leftOrigin)[axis], (*rightOrigin)[axis]);
            const uint64_t end = std::min((*leftOrigin)[axis] + leftAffine->Extents()[axis],
                                          (*rightOrigin)[axis] + rightAffine->Extents()[axis]);
            if (origin[axis] >= end) {
                return std::optional<AffineByteFootprint>{};
            }
            extents[axis] = end - origin[axis];
        }

        uint64_t offset = domainAffine->OffsetBytes();
        for (uint8_t axis = 0; axis < domainAffine->Rank(); ++axis) {
            const uint64_t stride = domainAffine->ByteStrides()[axis];
            if (origin[axis] != 0 && stride > (UINT64_MAX - offset) / origin[axis]) {
                return std::unexpected(AffineByteFootprintError::ArithmeticOverflow);
            }
            offset += origin[axis] * stride;
        }
        auto intersection = AffineByteFootprint::Create(
            {extents.data(), domainAffine->Rank()}, domainAffine->ByteStrides(), domainAffine->ComponentStrideBytes(),
            offset, domainAffine->ScalarBytes(), domainAffine->Lanes());
        if (!intersection) {
            return std::unexpected(intersection.error());
        }
        return std::optional<AffineByteFootprint>{*intersection};
    }

    /** @brief Fixed-capacity affine index-box complement represented as layout-preserving subviews. */
    template<size_t Capacity>
        requires(Capacity != 0)
    class AffineCoverageComplement final {
        struct IndexBox {
            std::array<uint64_t, kMaximumBufferRank> origin{};
            std::array<uint64_t, kMaximumBufferRank> extents{};
        };

    public:
        /** @brief Subtract @p removed affine subviews from @p target without enumerating logical elements. */
        [[nodiscard]] static constexpr std::expected<AffineCoverageComplement, AffineByteFootprintError>
        Create(ByteCoverageView target, std::span<const ByteCoverageView> removed) noexcept {
            const AffineByteFootprint* targetAffine = target.BorrowedAffineFootprint();
            if (targetAffine == nullptr) {
                return std::unexpected(AffineByteFootprintError::InvalidDescriptor);
            }
            constexpr std::array<uint64_t, kMaximumBufferRank> origin{};
            return CreateWithOrigin(*targetAffine, targetAffine->Extents(), origin, removed);
        }

        /** @brief Subtract sibling affine subviews from @p target in one shared logical @p domain. */
        [[nodiscard]] static constexpr std::expected<AffineCoverageComplement, AffineByteFootprintError>
        Create(ByteCoverageView domain, ByteCoverageView target, std::span<const ByteCoverageView> removed) noexcept {
            const AffineByteFootprint* domainAffine = domain.BorrowedAffineFootprint();
            const AffineByteFootprint* targetAffine = target.BorrowedAffineFootprint();
            const auto targetOrigin = domainAffine != nullptr && targetAffine != nullptr
                                          ? domainAffine->AffineSubviewOrigin(*targetAffine)
                                          : std::nullopt;
            if (!targetOrigin) {
                return std::unexpected(AffineByteFootprintError::InvalidDescriptor);
            }
            return CreateWithOrigin(*domainAffine, targetAffine->Extents(), *targetOrigin, removed);
        }

        /** @brief Borrow the active complement subviews in ascending byte-offset order. */
        [[nodiscard]] constexpr std::span<const AffineByteFootprint> Footprints() const noexcept {
            return {footprints_.data(), size_};
        }

        /** @brief Return the number of active complement subviews. */
        [[nodiscard]] constexpr size_t Size() const noexcept { return size_; }

    private:
        [[nodiscard]] static constexpr std::expected<AffineCoverageComplement, AffineByteFootprintError>
        CreateWithOrigin(const AffineByteFootprint& domain, std::span<const uint64_t> targetExtents,
                         const std::array<uint64_t, kMaximumBufferRank>& targetOrigin,
                         std::span<const ByteCoverageView> removed) noexcept {
            std::array<IndexBox, Capacity> current{};
            std::array<IndexBox, Capacity> next{};
            size_t currentCount = 1;
            current[0].origin = targetOrigin;
            std::ranges::copy(targetExtents, current[0].extents.begin());
            for (const ByteCoverageView coverage : removed) {
                const AffineByteFootprint* removedAffine = coverage.BorrowedAffineFootprint();
                const auto removedOrigin =
                    removedAffine != nullptr ? domain.AffineSubviewOrigin(*removedAffine) : std::nullopt;
                if (!removedOrigin) {
                    return std::unexpected(AffineByteFootprintError::InvalidDescriptor);
                }

                size_t nextCount = 0;
                for (size_t boxIndex = 0; boxIndex < currentCount; ++boxIndex) {
                    const IndexBox& box = current[boxIndex];
                    std::array<uint64_t, kMaximumBufferRank> intersectionBegin{};
                    std::array<uint64_t, kMaximumBufferRank> intersectionEnd{};
                    bool intersects = true;
                    for (uint8_t axis = 0; axis < domain.Rank(); ++axis) {
                        intersectionBegin[axis] = std::max(box.origin[axis], (*removedOrigin)[axis]);
                        intersectionEnd[axis] = std::min(box.origin[axis] + box.extents[axis],
                                                         (*removedOrigin)[axis] + removedAffine->Extents()[axis]);
                        intersects &= intersectionBegin[axis] < intersectionEnd[axis];
                    }
                    if (!intersects) {
                        if (!Append(next, nextCount, box)) {
                            return std::unexpected(AffineByteFootprintError::CapacityExceeded);
                        }
                        continue;
                    }

                    IndexBox core = box;
                    for (uint8_t axis = 0; axis < domain.Rank(); ++axis) {
                        if (core.origin[axis] < intersectionBegin[axis]) {
                            IndexBox left = core;
                            left.extents[axis] = intersectionBegin[axis] - core.origin[axis];
                            if (!Append(next, nextCount, left)) {
                                return std::unexpected(AffineByteFootprintError::CapacityExceeded);
                            }
                            core.extents[axis] -= left.extents[axis];
                            core.origin[axis] = intersectionBegin[axis];
                        }
                        const uint64_t coreEnd = core.origin[axis] + core.extents[axis];
                        if (intersectionEnd[axis] < coreEnd) {
                            IndexBox right = core;
                            right.origin[axis] = intersectionEnd[axis];
                            right.extents[axis] = coreEnd - intersectionEnd[axis];
                            if (!Append(next, nextCount, right)) {
                                return std::unexpected(AffineByteFootprintError::CapacityExceeded);
                            }
                            core.extents[axis] = intersectionEnd[axis] - core.origin[axis];
                        }
                    }
                }
                current = next;
                currentCount = nextCount;
                if (currentCount == 0) {
                    break;
                }
            }

            AffineCoverageComplement result;
            for (size_t boxIndex = 0; boxIndex < currentCount; ++boxIndex) {
                const IndexBox& box = current[boxIndex];
                uint64_t offset = domain.OffsetBytes();
                for (uint8_t axis = 0; axis < domain.Rank(); ++axis) {
                    const uint64_t stride = domain.ByteStrides()[axis];
                    if (box.origin[axis] != 0 && stride > (UINT64_MAX - offset) / box.origin[axis]) {
                        return std::unexpected(AffineByteFootprintError::ArithmeticOverflow);
                    }
                    offset += box.origin[axis] * stride;
                }
                auto footprint = AffineByteFootprint::Create({box.extents.data(), domain.Rank()}, domain.ByteStrides(),
                                                             domain.ComponentStrideBytes(), offset,
                                                             domain.ScalarBytes(), domain.Lanes());
                if (!footprint) {
                    return std::unexpected(footprint.error());
                }
                result.footprints_[result.size_++] = *footprint;
            }
            std::ranges::sort(result.footprints_.begin(), result.footprints_.begin() + result.size_, {},
                              &AffineByteFootprint::OffsetBytes);
            return result;
        }

        [[nodiscard]] static constexpr bool Append(std::array<IndexBox, Capacity>& boxes, size_t& size,
                                                   const IndexBox& box) noexcept {
            if (size == Capacity) {
                return false;
            }
            boxes[size++] = box;
            return true;
        }

        std::array<AffineByteFootprint, Capacity> footprints_{};
        size_t size_ = 0;
    };

    namespace Detail {

        /** @brief Decide sibling affine-subview overlap in one shared logical coordinate system. */
        [[nodiscard]] constexpr std::optional<CoverageOverlap>
        AffineSubviewRelation(ByteCoverageView domain, ByteCoverageView left, ByteCoverageView right) noexcept {
            const AffineByteFootprint* domainAffine = domain.BorrowedAffineFootprint();
            const AffineByteFootprint* leftAffine = left.BorrowedAffineFootprint();
            const AffineByteFootprint* rightAffine = right.BorrowedAffineFootprint();
            if (domainAffine == nullptr || leftAffine == nullptr || rightAffine == nullptr) {
                return std::nullopt;
            }
            const auto leftOrigin = domainAffine->AffineSubviewOrigin(*leftAffine);
            const auto rightOrigin = domainAffine->AffineSubviewOrigin(*rightAffine);
            if (!leftOrigin || !rightOrigin) {
                return std::nullopt;
            }
            for (uint8_t axis = 0; axis < domainAffine->Rank(); ++axis) {
                if ((*leftOrigin)[axis] + leftAffine->Extents()[axis] <= (*rightOrigin)[axis] ||
                    (*rightOrigin)[axis] + rightAffine->Extents()[axis] <= (*leftOrigin)[axis]) {
                    return CoverageOverlap::Disjoint;
                }
            }
            return CoverageOverlap::Overlap;
        }

        /** @brief Prove an affine subview union without work proportional to its logical element count. */
        [[nodiscard]] constexpr std::optional<CoverageEquivalence>
        ExactAffineSubviewUnion(ByteCoverageView target, std::span<const ByteCoverageView> parts) noexcept {
            const AffineByteFootprint* targetAffine = target.BorrowedAffineFootprint();
            if (targetAffine == nullptr) {
                return std::nullopt;
            }

            uint64_t occupiedBytes = 0;
            for (size_t index = 0; index < parts.size(); ++index) {
                const AffineByteFootprint* part = parts[index].BorrowedAffineFootprint();
                if (part == nullptr) {
                    return std::nullopt;
                }
                const auto origin = targetAffine->AffineSubviewOrigin(*part);
                if (!origin) {
                    return std::nullopt;
                }
                if (part->OccupiedByteCount() > targetAffine->OccupiedByteCount() - occupiedBytes) {
                    return CoverageEquivalence::Different;
                }
                occupiedBytes += part->OccupiedByteCount();

                for (size_t previousIndex = 0; previousIndex < index; ++previousIndex) {
                    const AffineByteFootprint* previous = parts[previousIndex].BorrowedAffineFootprint();
                    if (previous == nullptr) {
                        return std::nullopt;
                    }
                    const auto previousOrigin = targetAffine->AffineSubviewOrigin(*previous);
                    if (!previousOrigin) {
                        return std::nullopt;
                    }
                    bool boxesOverlap = true;
                    for (uint8_t axis = 0; axis < targetAffine->Rank(); ++axis) {
                        if ((*origin)[axis] + part->Extents()[axis] <= (*previousOrigin)[axis] ||
                            (*previousOrigin)[axis] + previous->Extents()[axis] <= (*origin)[axis]) {
                            boxesOverlap = false;
                            break;
                        }
                    }
                    if (boxesOverlap) {
                        return CoverageEquivalence::Different;
                    }
                }
            }
            return occupiedBytes == targetAffine->OccupiedByteCount() ? CoverageEquivalence::Equal
                                                                      : CoverageEquivalence::Different;
        }

    } // namespace Detail

    /** @brief Decide overlap of two coverages expressed within one exact shared domain. */
    [[nodiscard]] constexpr CoverageOverlap CoverageRelationWithin(ByteCoverageView domain, ByteCoverageView left,
                                                                   ByteCoverageView right) noexcept {
        if (!domain.Valid() || domain.Contains(left) != CoverageContainment::Yes ||
            domain.Contains(right) != CoverageContainment::Yes) {
            return CoverageOverlap::Unknown;
        }
        if (const auto affine = Detail::AffineSubviewRelation(domain, left, right)) {
            return *affine;
        }
        return left.Relation(right);
    }

    /** @brief Prove that non-overlapping @p parts have a union exactly equal to @p target within bounded work. */
    template<size_t Capacity>
        requires(Capacity != 0)
    [[nodiscard]] constexpr CoverageEquivalence ExactCoverageUnion(ByteCoverageView target,
                                                                   std::span<const ByteCoverageView> parts) noexcept {
        if (!target.Valid() || parts.empty()) {
            return CoverageEquivalence::Different;
        }
        if (parts.size() == 1 && parts.front() == target) {
            return CoverageEquivalence::Equal;
        }
        if (const auto affine = Detail::ExactAffineSubviewUnion(target, parts)) {
            return *affine;
        }
        const auto targetRegions = target.ProjectCanonicalRegions<Capacity>();
        if (!targetRegions) {
            return targetRegions.error() == AffineByteFootprintError::CapacityExceeded ? CoverageEquivalence::Unknown
                                                                                       : CoverageEquivalence::Different;
        }

        std::array<ByteRegion, Capacity> regions{};
        size_t regionCount = 0;
        for (const ByteCoverageView part : parts) {
            const auto projected = part.ProjectCanonicalRegions<Capacity>();
            if (!projected) {
                return projected.error() == AffineByteFootprintError::CapacityExceeded ? CoverageEquivalence::Unknown
                                                                                       : CoverageEquivalence::Different;
            }
            if (projected->Size() > Capacity - regionCount) {
                return CoverageEquivalence::Unknown;
            }
            for (const ByteRegion region : projected->Regions()) {
                regions[regionCount++] = region;
            }
        }
        std::sort(regions.begin(), regions.begin() + static_cast<std::ptrdiff_t>(regionCount),
                  [](ByteRegion left, ByteRegion right) {
                      return left.Offset() != right.Offset() ? left.Offset() < right.Offset()
                                                             : left.Size() < right.Size();
                  });

        size_t mergedCount = 0;
        for (size_t index = 0; index < regionCount; ++index) {
            const ByteRegion current = regions[index];
            if (mergedCount == 0 || regions[mergedCount - 1].End() < current.Offset()) {
                regions[mergedCount++] = current;
                continue;
            }
            if (regions[mergedCount - 1].End() > current.Offset()) {
                return CoverageEquivalence::Different;
            }
            const ByteRegion previous = regions[mergedCount - 1];
            const auto merged = ByteRegion::Make(previous.Offset(), current.End() - previous.Offset());
            if (!merged) {
                return CoverageEquivalence::Different;
            }
            regions[mergedCount - 1] = *merged;
        }
        if (mergedCount != targetRegions->Size()) {
            return CoverageEquivalence::Different;
        }
        for (size_t index = 0; index < mergedCount; ++index) {
            if (regions[index] != targetRegions->Regions()[index]) {
                return CoverageEquivalence::Different;
            }
        }
        return CoverageEquivalence::Equal;
    }

    /** @brief Stable failure while translating exact logical byte coverage into one physical allocation. */
    enum class ByteMappingError : uint8_t {
        InvalidCoverage,
        InvalidAllocation,
        OutsideCoverage,
        IndeterminateCoverage,
        ArithmeticOverflow,
        CapacityExceeded,
    };

    /**
     * @brief Borrowed exact coverage translated into one retained allocation's byte coordinates.
     * @details The affine translation is `physical = allocationOffset + logical - logicalBoundingOffset`. Physical
     * storage therefore retains the logical bounding gaps while publication may project only occupied bytes.
     */
    class TranslatedByteCoverage final {
    public:
        constexpr TranslatedByteCoverage() noexcept = default;

        /** @brief Validate one logical coverage mapping within an allocation of @p allocationSize bytes. */
        [[nodiscard]] static constexpr std::expected<TranslatedByteCoverage, ByteMappingError>
        Create(ByteCoverageView coverage, uint64_t allocationOffset, uint64_t allocationSize) noexcept {
            if (!coverage.Valid()) {
                return std::unexpected(ByteMappingError::InvalidCoverage);
            }
            if (allocationSize == 0 || allocationOffset > allocationSize ||
                coverage.BoundingRegion().Size() > allocationSize - allocationOffset) {
                return std::unexpected(ByteMappingError::InvalidAllocation);
            }
            return TranslatedByteCoverage{coverage, allocationOffset, allocationSize};
        }

        /** @brief Return the borrowed exact logical coverage. */
        [[nodiscard]] constexpr ByteCoverageView LogicalCoverage() const noexcept { return coverage_; }

        /** @brief Return the physical offset corresponding to the logical bounding offset. */
        [[nodiscard]] constexpr uint64_t AllocationOffset() const noexcept { return allocationOffset_; }

        /** @brief Resolve one exactly covered logical interval to physical allocation coordinates. */
        [[nodiscard]] constexpr std::expected<ByteRegion, ByteMappingError> Resolve(ByteRegion logical) const noexcept {
            const CoverageContainment containment = coverage_.Contains(logical);
            if (containment == CoverageContainment::No) {
                return std::unexpected(ByteMappingError::OutsideCoverage);
            }
            if (containment == CoverageContainment::Unknown) {
                return std::unexpected(ByteMappingError::IndeterminateCoverage);
            }
            const uint64_t delta = logical.Offset() - coverage_.BoundingRegion().Offset();
            if (delta > UINT64_MAX - allocationOffset_) {
                return std::unexpected(ByteMappingError::ArithmeticOverflow);
            }
            const auto physical = ByteRegion::Make(allocationOffset_ + delta, logical.Size());
            if (!physical || physical->End() > allocationSize_) {
                return std::unexpected(ByteMappingError::ArithmeticOverflow);
            }
            return *physical;
        }

        /** @brief Project and translate exact occupied bytes into at most @p Capacity physical intervals. */
        template<size_t Capacity>
            requires(Capacity != 0)
        [[nodiscard]] constexpr std::expected<CanonicalByteRegions<Capacity>, ByteMappingError>
        ProjectPhysicalRegions() const noexcept {
            const auto logical = coverage_.ProjectCanonicalRegions<Capacity>();
            if (!logical) {
                return std::unexpected(logical.error() == AffineByteFootprintError::CapacityExceeded
                                           ? ByteMappingError::CapacityExceeded
                                           : ByteMappingError::InvalidCoverage);
            }
            CanonicalByteRegions<Capacity> result;
            for (const ByteRegion region : logical->Regions()) {
                const uint64_t delta = region.Offset() - coverage_.BoundingRegion().Offset();
                if (delta > UINT64_MAX - allocationOffset_) {
                    return std::unexpected(ByteMappingError::ArithmeticOverflow);
                }
                const auto physical = ByteRegion::Make(allocationOffset_ + delta, region.Size());
                if (!physical || physical->End() > allocationSize_) {
                    return std::unexpected(ByteMappingError::ArithmeticOverflow);
                }
                result.regions_[result.size_++] = *physical;
            }
            return result;
        }

    private:
        constexpr TranslatedByteCoverage(ByteCoverageView coverage, uint64_t allocationOffset,
                                         uint64_t allocationSize) noexcept
            : coverage_(coverage), allocationOffset_(allocationOffset), allocationSize_(allocationSize) {}

        ByteCoverageView coverage_{};
        uint64_t allocationOffset_ = 0;
        uint64_t allocationSize_ = 0;
    };

    static_assert(std::is_trivially_copyable_v<ByteCoverageView>);
    static_assert(std::is_trivially_copyable_v<TranslatedByteCoverage>);
    static_assert(sizeof(ByteCoverageView) <= 24);

} // namespace Sora::Compute

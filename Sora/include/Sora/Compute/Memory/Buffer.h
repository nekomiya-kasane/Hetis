/**
 * @file Buffer.h
 * @brief Logical compute-buffer identity, schema, version, and ready residency.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Memory/ByteCoverage.h>
#include <Sora/Compute/Memory/ResidencyTable.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Compute {

    class CompletionTransaction;
    class WeakBuffer;

    /** @brief Immutable logical schema and storage requirements of one buffer. */
    struct BufferDesc {
        BufferId id{};
        ValueType elementType{};
        LayoutKind layout = LayoutKind::Contiguous;
        uint8_t rank = 1;
        uint64_t sizeBytes = 0;
        uint64_t alignment = 1;

        /** @brief Return whether every field forms a supported logical buffer descriptor. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return id.Valid() && elementType.Valid() && layout != LayoutKind::Scalar && rank != 0 &&
                   rank <= kMaximumBufferRank && sizeBytes != 0 && std::has_single_bit(alignment);
        }

        friend constexpr bool operator==(BufferDesc, BufferDesc) noexcept = default;
    };

    namespace Detail {

        /** @brief Validate that @p footprint is the canonical logical value coverage described by @p desc. */
        [[nodiscard]] constexpr bool ValidBufferLogicalFootprint(BufferDesc desc,
                                                                 const AffineByteFootprint& footprint) noexcept {
            const uint64_t scalarBytes = ScalarByteSize(desc.elementType.scalar);
            if (!desc.Valid() || !footprint.Valid() || footprint.Rank() != desc.rank ||
                footprint.ScalarBytes() != scalarBytes || footprint.Lanes() != desc.elementType.lanes ||
                footprint.BoundingRegion().End() > desc.sizeBytes || footprint.OffsetBytes() % scalarBytes != 0 ||
                footprint.ComponentStrideBytes() % scalarBytes != 0) {
                return false;
            }
            for (const uint64_t stride : footprint.ByteStrides()) {
                if (stride % scalarBytes != 0) {
                    return false;
                }
            }

            if (desc.layout == LayoutKind::Strided) {
                return footprint.ComponentStrideBytes() == scalarBytes;
            }

            uint64_t canonicalStride =
                desc.layout == LayoutKind::StructureOfArrays ? scalarBytes : desc.elementType.ByteSize();
            for (uint8_t axis = desc.rank; axis-- > 0;) {
                if (footprint.ByteStrides()[axis] != canonicalStride ||
                    canonicalStride > UINT64_MAX / footprint.Extents()[axis]) {
                    return false;
                }
                canonicalStride *= footprint.Extents()[axis];
            }
            if (desc.layout == LayoutKind::Contiguous) {
                return footprint.ComponentStrideBytes() == scalarBytes && footprint.IsContiguous();
            }
            return desc.layout == LayoutKind::StructureOfArrays && footprint.ComponentStrideBytes() >= canonicalStride;
        }

        /** @brief Borrow the exact canonical logical coverage owned by one buffer state or snapshot. */
        [[nodiscard]] constexpr ByteCoverageView
        BufferLogicalCoverage(BufferDesc desc, const std::optional<AffineByteFootprint>& footprint) noexcept {
            if (footprint) {
                return ByteCoverageView::Affine(*footprint);
            }
            const auto whole = ByteRegion::Make(0, desc.sizeBytes);
            return whole ? ByteCoverageView::Interval(*whole) : ByteCoverageView{};
        }

    } // namespace Detail

    /** @brief Atomic planning snapshot retaining every ready allocation lease for one logical version. */
    struct BufferResidencySnapshot {
        BufferDesc desc{};
        std::optional<AffineByteFootprint> logicalFootprint{};
        uint64_t version = 0;
        std::vector<ResidentReplica> replicas{};

        /** @brief Borrow the immutable logical value coverage owned by this snapshot. */
        [[nodiscard]] constexpr ByteCoverageView LogicalCoverage() const noexcept {
            return Detail::BufferLogicalCoverage(desc, logicalFootprint);
        }
    };

    /** @brief Shared logical buffer with a monotonically increasing whole-buffer version. */
    class Buffer {
        struct State {
            explicit State(BufferDesc value, std::optional<AffineByteFootprint> footprint = {}) noexcept
                : desc(value), logicalFootprint(std::move(footprint)) {}

            BufferDesc desc{};
            std::optional<AffineByteFootprint> logicalFootprint{};
            mutable std::mutex mutex{};
            ResidencyTable residency{};
            std::vector<BackendServiceId> retiredServices{};
            uint64_t version = 0;
        };

    public:
        Buffer() noexcept = default;

        /** @brief Create a contiguous logical buffer whose complete physical address domain stores values. */
        [[nodiscard]] static std::expected<Buffer, MemoryError> Create(BufferDesc desc) noexcept {
            if (!desc.Valid() || desc.layout != LayoutKind::Contiguous ||
                desc.sizeBytes % desc.elementType.ByteSize() != 0) {
                return std::unexpected(MemoryError::InvalidDescriptor);
            }
            try {
                return Buffer{std::make_shared<State>(desc)};
            } catch (const std::bad_alloc&) {
                return std::unexpected(MemoryError::OutOfMemory);
            }
        }

        /** @brief Create a logical buffer with explicit exact affine value coverage inside its physical domain. */
        [[nodiscard]] static std::expected<Buffer, MemoryError> Create(BufferDesc desc,
                                                                       AffineByteFootprint logicalFootprint) noexcept {
            if (!Detail::ValidBufferLogicalFootprint(desc, logicalFootprint)) {
                return std::unexpected(MemoryError::InvalidDescriptor);
            }
            try {
                return Buffer{std::make_shared<State>(desc, std::move(logicalFootprint))};
            } catch (const std::bad_alloc&) {
                return std::unexpected(MemoryError::OutOfMemory);
            }
        }

        /** @brief Return whether this handle denotes a logical buffer. */
        [[nodiscard]] explicit operator bool() const noexcept { return state_ != nullptr; }

        /** @brief Return a non-owning handle for runtime invalidation registries. */
        [[nodiscard]] WeakBuffer Weak() const noexcept;

        /** @brief Return the immutable descriptor. @pre This handle is non-empty. */
        [[nodiscard]] BufferDesc Describe() const noexcept {
            assert(state_ != nullptr);
            return state_->desc;
        }

        /** @brief Borrow the immutable exact logical value coverage. @pre This handle is non-empty and outlives it. */
        [[nodiscard]] ByteCoverageView LogicalCoverage() const noexcept {
            assert(state_ != nullptr);
            return Detail::BufferLogicalCoverage(state_->desc, state_->logicalFootprint);
        }

        /** @brief Return the current logical version, or zero before the first write. */
        [[nodiscard]] uint64_t CurrentVersion() const {
            assert(state_ != nullptr);
            std::scoped_lock lock{state_->mutex};
            return state_->version;
        }

        /**
         * @brief Commit a completed write of the entire logical buffer into @p allocation.
         * @return The newly committed logical version.
         */
        [[nodiscard]] std::expected<uint64_t, MemoryError> CommitWholeWrite(Allocation allocation) {
            assert(state_ != nullptr);
            if (!allocation || allocation.Alignment() < state_->desc.alignment ||
                !allocation.View().Covers(0, state_->desc.sizeBytes)) {
                return std::unexpected(MemoryError::InvalidAllocation);
            }

            std::vector<ResidentReplica> retired;
            uint64_t nextVersion = 0;
            {
                std::scoped_lock lock{state_->mutex};
                if (std::ranges::binary_search(state_->retiredServices, allocation.View().service)) {
                    return std::unexpected(MemoryError::RetiredService);
                }
                if (state_->version == std::numeric_limits<uint64_t>::max()) {
                    return std::unexpected(MemoryError::VersionExhausted);
                }
                nextVersion = state_->version + 1;
                auto reset = state_->residency.ResetToWholeWrite(std::move(allocation), nextVersion,
                                                                 state_->desc.sizeBytes, LogicalCoverage());
                if (!reset) {
                    return std::unexpected(reset.error());
                }
                retired = std::move(*reset);
                state_->version = nextVersion;
            }
            return nextVersion;
        }

        /** @brief Publish a completed replica of @p version and @p region. */
        [[nodiscard]] std::expected<void, MemoryError>
        PublishReplica(uint64_t version, ByteRegion region, Allocation allocation, uint64_t allocationOffset = 0) {
            return PublishCoverage(version, ByteCoverageView::Interval(region), std::move(allocation),
                                   allocationOffset);
        }

        /** @brief Publish a completed exact affine replica of @p version. */
        [[nodiscard]] std::expected<void, MemoryError> PublishReplica(uint64_t version,
                                                                      const AffineByteFootprint& footprint,
                                                                      Allocation allocation,
                                                                      uint64_t allocationOffset = 0) {
            return PublishCoverage(version, ByteCoverageView::Affine(footprint), std::move(allocation),
                                   allocationOffset);
        }

        /** @brief Acquire an owning lease for the current version of exact @p coverage at @p location. */
        [[nodiscard]] std::expected<ResidentReplica, MemoryError> AcquireReplica(MemoryLocation location,
                                                                                 ByteCoverageView coverage) const {
            assert(state_ != nullptr);
            std::scoped_lock lock{state_->mutex};
            auto replica = state_->residency.Find(location, state_->version, coverage);
            if (!replica) {
                return std::unexpected(MemoryError::NotResident);
            }
            return std::move(*replica);
        }

        /** @brief Acquire an owning lease for the current version of @p region at @p location. */
        [[nodiscard]] std::expected<ResidentReplica, MemoryError> AcquireReplica(MemoryLocation location,
                                                                                 ByteRegion region) const {
            return AcquireReplica(location, ByteCoverageView::Interval(region));
        }

    private:
        [[nodiscard]] std::expected<void, MemoryError>
        PublishCoverage(uint64_t version, ByteCoverageView coverage, Allocation allocation, uint64_t allocationOffset) {
            assert(state_ != nullptr);
            auto replica = ResidentReplica::Create(std::move(allocation), coverage, allocationOffset, version);
            if (!replica) {
                return std::unexpected(replica.error());
            }
            std::optional<ResidentReplica> retired;
            {
                std::scoped_lock lock{state_->mutex};
                if (std::ranges::binary_search(state_->retiredServices, replica->allocation.View().service)) {
                    return std::unexpected(MemoryError::RetiredService);
                }
                auto published = state_->residency.Publish(std::move(*replica), state_->version, LogicalCoverage());
                if (!published) {
                    return std::unexpected(published.error());
                }
                retired = std::move(*published);
            }
            return {};
        }

    public:
        /** @brief Atomically snapshot the current version and every ready replica into owning planning leases. */
        [[nodiscard]] std::expected<BufferResidencySnapshot, MemoryError> SnapshotResidency() const noexcept {
            assert(state_ != nullptr);
            std::scoped_lock lock{state_->mutex};
            auto replicas = state_->residency.Snapshot(state_->version);
            if (!replicas) {
                return std::unexpected(replicas.error());
            }
            return BufferResidencySnapshot{
                .desc = state_->desc,
                .logicalFootprint = state_->logicalFootprint,
                .version = state_->version,
                .replicas = std::move(*replicas),
            };
        }

        /** @brief Return the number of ready replicas of the current version. */
        [[nodiscard]] size_t ReplicaCount() const {
            assert(state_ != nullptr);
            std::scoped_lock lock{state_->mutex};
            return state_->residency.Size();
        }

        /**
         * @brief Permanently reject @p service and atomically extract all of its ready replicas.
         * @details Allocation destruction occurs after releasing the buffer mutex so backend callbacks may re-enter.
         */
        [[nodiscard]] std::expected<size_t, MemoryError> InvalidateService(BackendServiceId service) const noexcept {
            assert(state_ != nullptr);
            if (!service.Valid()) {
                return std::unexpected(MemoryError::InvalidService);
            }
            std::vector<ResidentReplica> retired;
            {
                std::scoped_lock lock{state_->mutex};
                const bool newlyRetired = !std::ranges::binary_search(state_->retiredServices, service);
                if (newlyRetired) {
                    try {
                        state_->retiredServices.reserve(state_->retiredServices.size() + 1);
                    } catch (const std::bad_alloc&) {
                        return std::unexpected(MemoryError::OutOfMemory);
                    }
                }
                auto extracted = state_->residency.RetireServiceReplicas(service);
                if (!extracted) {
                    return std::unexpected(extracted.error());
                }
                retired = std::move(*extracted);
                if (newlyRetired) {
                    const auto position = std::ranges::lower_bound(state_->retiredServices, service);
                    state_->retiredServices.insert(position, service);
                }
            }
            return retired.size();
        }

    private:
        friend class CompletionTransaction;
        friend class WeakBuffer;

        explicit Buffer(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

        std::shared_ptr<State> state_{};
    };

    /** @brief Non-owning logical-buffer identity that never extends allocation or residency lifetime. */
    class WeakBuffer final {
    public:
        WeakBuffer() noexcept = default;

        /** @brief Return the stable logical identity retained independently of buffer lifetime. */
        [[nodiscard]] constexpr BufferId Identity() const noexcept { return id_; }

        /** @brief Return whether the observed logical buffer no longer exists. */
        [[nodiscard]] bool Expired() const noexcept { return state_.expired(); }

        /** @brief Acquire a temporary owning handle, or an empty handle after expiration. */
        [[nodiscard]] Buffer Lock() const noexcept { return Buffer{state_.lock()}; }

        /** @brief Return whether two weak handles observe the same shared logical state. */
        [[nodiscard]] bool SharesOwnership(const WeakBuffer& other) const noexcept {
            return !state_.owner_before(other.state_) && !other.state_.owner_before(state_);
        }

    private:
        friend class Buffer;

        WeakBuffer(BufferId id, std::weak_ptr<Buffer::State> state) noexcept : id_(id), state_(std::move(state)) {}

        BufferId id_{};
        std::weak_ptr<Buffer::State> state_{};
    };

    inline WeakBuffer Buffer::Weak() const noexcept {
        return state_ != nullptr ? WeakBuffer{state_->desc.id, state_} : WeakBuffer{};
    }

    static_assert(std::is_trivially_copyable_v<BufferDesc>);

} // namespace Sora::Compute

/**
 * @file DispatchPartition.h
 * @brief Backend-independent dispatch slices and strict elementwise partition covers.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Invocation/KernelInvocation.h>

#include <array>
#include <cstdint>
#include <expected>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Compute {

    /** @brief Stable reason a dispatch slice or elementwise partition cover is invalid. */
    enum class DispatchPartitionError : uint8_t {
        InvalidDomain,
        InvalidContract,
        EmptyPartition,
        TooManyShards,
        ArithmeticOverflow,
        IncompleteCover,
        OutOfMemory,
    };

    /** @brief One non-empty axis-aligned subdomain retaining coordinates in the global logical index space. */
    struct DispatchSlice {
        uint8_t rank = 0;
        std::array<uint64_t, kMaximumIndexRank> origin{};
        std::array<uint64_t, kMaximumIndexRank> extents{};
        uint64_t workItemCount = 0;

        /** @brief Return whether this slice is a non-empty subdomain of @p domain without integer overflow. */
        [[nodiscard]] constexpr bool ValidWithin(const DispatchDomain& domain) const noexcept {
            if (!domain.Valid() || rank != domain.rank || workItemCount == 0) {
                return false;
            }
            uint64_t count = 1;
            for (uint8_t axis = 0; axis < kMaximumIndexRank; ++axis) {
                if (axis >= rank) {
                    if (origin[axis] != 0 || extents[axis] != 0) {
                        return false;
                    }
                    continue;
                }
                if (extents[axis] == 0 || origin[axis] > domain.extents[axis] ||
                    extents[axis] > domain.extents[axis] - origin[axis] ||
                    count > std::numeric_limits<uint64_t>::max() / extents[axis]) {
                    return false;
                }
                count *= extents[axis];
            }
            return count == workItemCount;
        }

        /** @brief Return whether this slice covers the complete dispatch domain. */
        [[nodiscard]] constexpr bool IsWhole(const DispatchDomain& domain) const noexcept {
            if (!ValidWithin(domain)) {
                return false;
            }
            for (uint8_t axis = 0; axis < rank; ++axis) {
                if (origin[axis] != 0 || extents[axis] != domain.extents[axis]) {
                    return false;
                }
            }
            return true;
        }

        /** @brief Form the unique slice covering all work items in @p domain. */
        [[nodiscard]] static constexpr DispatchSlice Whole(const DispatchDomain& domain) noexcept {
            return {
                .rank = domain.rank, .origin = {}, .extents = domain.extents, .workItemCount = domain.workItemCount};
        }

        friend constexpr bool operator==(DispatchSlice, DispatchSlice) noexcept = default;
    };

    /** @brief Immutable strict cover used as the semantic input to multi-device placement. */
    class ElementwisePartitionPlan final {
    public:
        /** @brief Validate that @p shards are the canonical ordered strict cover admitted by @p contract. */
        [[nodiscard]] static constexpr std::expected<void, DispatchPartitionError>
        ValidateStrictCover(const DispatchDomain& domain, PartitionContract contract, uint8_t axis,
                            std::span<const DispatchSlice> shards) noexcept {
            if (const auto error = ValidateRequest(domain, contract, axis)) {
                return std::unexpected(*error);
            }
            if (shards.empty()) {
                return std::unexpected(DispatchPartitionError::EmptyPartition);
            }
            if (shards.size() > UINT32_MAX) {
                return std::unexpected(DispatchPartitionError::TooManyShards);
            }

            uint64_t covered = 0;
            for (const DispatchSlice& shard : shards) {
                if (!shard.ValidWithin(domain) || covered > domain.extents[axis] ||
                    shard.extents[axis] > domain.extents[axis] - covered) {
                    return std::unexpected(DispatchPartitionError::IncompleteCover);
                }
                DispatchSlice expected = DispatchSlice::Whole(domain);
                expected.origin[axis] = covered;
                expected.extents[axis] = shard.extents[axis];
                expected.workItemCount = domain.workItemCount / domain.extents[axis] * shard.extents[axis];
                if (shard != expected) {
                    return std::unexpected(DispatchPartitionError::IncompleteCover);
                }
                covered += shard.extents[axis];
            }
            if (covered != domain.extents[axis]) {
                return std::unexpected(DispatchPartitionError::IncompleteCover);
            }
            return {};
        }

        /**
         * @brief Build a strict cover along @p axis from explicit positive shard extents.
         */
        [[nodiscard]] static std::expected<ElementwisePartitionPlan, DispatchPartitionError>
        Create(const DispatchDomain& domain, PartitionContract contract, uint8_t axis,
               std::span<const uint64_t> shardExtents) noexcept {
            if (const auto error = ValidateRequest(domain, contract, axis)) {
                return std::unexpected(*error);
            }
            if (shardExtents.empty()) {
                return std::unexpected(DispatchPartitionError::EmptyPartition);
            }
            if (shardExtents.size() > UINT32_MAX) {
                return std::unexpected(DispatchPartitionError::TooManyShards);
            }

            try {
                std::vector<DispatchSlice> shards;
                shards.reserve(shardExtents.size());
                uint64_t covered = 0;
                for (const uint64_t shardExtent : shardExtents) {
                    if (shardExtent == 0) {
                        return std::unexpected(DispatchPartitionError::EmptyPartition);
                    }
                    if (covered > domain.extents[axis] || shardExtent > domain.extents[axis] - covered) {
                        return std::unexpected(DispatchPartitionError::IncompleteCover);
                    }
                    DispatchSlice shard = DispatchSlice::Whole(domain);
                    shard.origin[axis] = covered;
                    shard.extents[axis] = shardExtent;
                    shard.workItemCount = 1;
                    for (uint8_t currentAxis = 0; currentAxis < shard.rank; ++currentAxis) {
                        if (shard.workItemCount > std::numeric_limits<uint64_t>::max() / shard.extents[currentAxis]) {
                            return std::unexpected(DispatchPartitionError::ArithmeticOverflow);
                        }
                        shard.workItemCount *= shard.extents[currentAxis];
                    }
                    shards.push_back(shard);
                    covered += shardExtent;
                }
                if (covered != domain.extents[axis]) {
                    return std::unexpected(DispatchPartitionError::IncompleteCover);
                }
                return ElementwisePartitionPlan{domain, contract, axis, std::move(shards)};
            } catch (const std::bad_alloc&) {
                return std::unexpected(DispatchPartitionError::OutOfMemory);
            }
        }

        /** @brief Build the most even strict cover possible for @p shardCount runtime devices. */
        [[nodiscard]] static std::expected<ElementwisePartitionPlan, DispatchPartitionError>
        Balanced(const DispatchDomain& domain, PartitionContract contract, uint8_t axis, uint32_t shardCount) noexcept {
            if (const auto error = ValidateRequest(domain, contract, axis)) {
                return std::unexpected(*error);
            }
            if (shardCount == 0) {
                return std::unexpected(DispatchPartitionError::EmptyPartition);
            }
            if (domain.extents[axis] < shardCount) {
                return std::unexpected(DispatchPartitionError::TooManyShards);
            }
            try {
                std::vector<uint64_t> extents(shardCount, domain.extents[axis] / shardCount);
                const uint64_t remainder = domain.extents[axis] % shardCount;
                for (uint32_t shard = 0; shard < remainder; ++shard) {
                    ++extents[shard];
                }
                return Create(domain, contract, axis, extents);
            } catch (const std::bad_alloc&) {
                return std::unexpected(DispatchPartitionError::OutOfMemory);
            }
        }

        /** @brief Return the global logical domain covered exactly once. */
        [[nodiscard]] const DispatchDomain& Domain() const noexcept { return domain_; }

        /** @brief Return the split axis. */
        [[nodiscard]] uint8_t Axis() const noexcept { return axis_; }

        /** @brief Return the validated semantic partition contract. */
        [[nodiscard]] PartitionContract Contract() const noexcept { return contract_; }

        /** @brief Return non-empty, ordered shards whose union is the complete domain. */
        [[nodiscard]] std::span<const DispatchSlice> Shards() const noexcept { return shards_; }

    private:
        [[nodiscard]] static constexpr std::optional<DispatchPartitionError>
        ValidateRequest(const DispatchDomain& domain, PartitionContract contract, uint8_t axis) noexcept {
            if (!domain.Valid()) {
                return DispatchPartitionError::InvalidDomain;
            }
            const IndexDomain indexDomain{.rank = domain.rank};
            if (axis >= domain.rank || !contract.Valid(indexDomain) || contract.kind != PartitionKind::Elementwise ||
                (contract.splittableAxisMask & static_cast<uint8_t>(uint8_t{1} << axis)) == 0) {
                return DispatchPartitionError::InvalidContract;
            }
            return std::nullopt;
        }

        ElementwisePartitionPlan(DispatchDomain domain, PartitionContract contract, uint8_t axis,
                                 std::vector<DispatchSlice> shards) noexcept
            : domain_(domain), contract_(contract), axis_(axis), shards_(std::move(shards)) {}

        DispatchDomain domain_{};
        PartitionContract contract_{};
        uint8_t axis_ = 0;
        std::vector<DispatchSlice> shards_{};
    };

    static_assert(std::is_trivially_copyable_v<DispatchSlice>);

} // namespace Sora::Compute

/**
 * @file ResidencyTable.h
 * @brief Versioned ready-replica table for one logical compute buffer.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Memory/Allocation.h>
#include <Sora/Compute/Memory/ByteCoverage.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace Sora::Compute {

    /** @brief One ready physical replica of a logical buffer region and version. */
    struct ResidentReplica {
        Allocation allocation{};
        ByteRegion bufferRegion{};
        uint64_t allocationOffset = 0;
        uint64_t version = 0;
        std::shared_ptr<const AffineByteFootprint> byteFootprint{};

        /** @brief Create one owning exact logical-to-physical replica mapping. */
        [[nodiscard]] static std::expected<ResidentReplica, MemoryError>
        Create(Allocation allocation, ByteCoverageView coverage, uint64_t allocationOffset, uint64_t version) noexcept {
            if (version == 0) {
                return std::unexpected(MemoryError::StaleVersion);
            }
            if (!allocation || !coverage.Valid() ||
                !allocation.View().Covers(allocationOffset, coverage.BoundingRegion().Size())) {
                return std::unexpected(MemoryError::InvalidAllocation);
            }
            try {
                std::shared_ptr<const AffineByteFootprint> footprint;
                if (const AffineByteFootprint* affine = coverage.BorrowedAffineFootprint(); affine != nullptr) {
                    footprint = std::make_shared<const AffineByteFootprint>(*affine);
                }
                return ResidentReplica{.allocation = std::move(allocation),
                                       .bufferRegion = coverage.BoundingRegion(),
                                       .allocationOffset = allocationOffset,
                                       .version = version,
                                       .byteFootprint = std::move(footprint)};
            } catch (const std::bad_alloc&) {
                return std::unexpected(MemoryError::OutOfMemory);
            }
        }

        /** @brief Borrow the exact logical byte coverage owned by this replica. */
        [[nodiscard]] ByteCoverageView Coverage() const noexcept {
            return byteFootprint ? ByteCoverageView::Affine(*byteFootprint) : ByteCoverageView::Interval(bufferRegion);
        }

        /** @brief Return whether this replica contains @p requested at @p requiredVersion. */
        [[nodiscard]] bool Covers(uint64_t requiredVersion, ByteCoverageView requested) const noexcept {
            return allocation && version == requiredVersion &&
                   Coverage().Contains(requested) == CoverageContainment::Yes;
        }

        /** @brief Return whether this replica contains @p requested at @p requiredVersion. */
        [[nodiscard]] bool Covers(uint64_t requiredVersion, ByteRegion requested) const noexcept {
            return Covers(requiredVersion, ByteCoverageView::Interval(requested));
        }

        /** @brief Resolve exact @p requested coverage to its physical bounding offset in this allocation. */
        [[nodiscard]] std::optional<uint64_t> ResolveAllocationOffset(ByteCoverageView requested) const noexcept {
            if (Coverage().Contains(requested) != CoverageContainment::Yes) {
                return std::nullopt;
            }
            return allocationOffset + requested.BoundingRegion().Offset() - bufferRegion.Offset();
        }

        /** @brief Resolve one contained logical interval to its physical byte offset. */
        [[nodiscard]] std::optional<uint64_t> ResolveAllocationOffset(ByteRegion requested) const noexcept {
            return ResolveAllocationOffset(ByteCoverageView::Interval(requested));
        }
    };

    /**
     * @brief Non-thread-safe ready-replica set used under its owning buffer's lock.
     * @details Pending replicas belong to executable-graph event state and are published here only after completion.
     */
    class ResidencyTable {
    public:
        /** @brief Replace every retained replica with one new whole-buffer write. */
        [[nodiscard]] std::expected<std::vector<ResidentReplica>, MemoryError>
        ResetToWholeWrite(Allocation allocation, uint64_t version, uint64_t bufferSize,
                          ByteCoverageView logicalCoverage) noexcept {
            if (!allocation || version == 0 || !allocation.View().Covers(0, bufferSize)) {
                return std::unexpected(MemoryError::InvalidAllocation);
            }
            auto replica = ResidentReplica::Create(std::move(allocation), logicalCoverage,
                                                   logicalCoverage.BoundingRegion().Offset(), version);
            if (!replica) {
                return std::unexpected(replica.error());
            }

            try {
                std::vector<ResidentReplica> replacement;
                replacement.reserve(1);
                replacement.push_back(std::move(*replica));
                replicas_.swap(replacement);
                return std::move(replacement);
            } catch (const std::bad_alloc&) {
                return std::unexpected(MemoryError::OutOfMemory);
            }
        }

        /** @brief Replace every retained replica with an ordered, exact byte cover produced by one logical write. */
        [[nodiscard]] std::expected<std::vector<ResidentReplica>, MemoryError>
        ResetToPartitionedWrite(std::vector<ResidentReplica> replicas, uint64_t version,
                                ByteCoverageView logicalCoverage) noexcept {
            constexpr size_t coverageCapacity = 256;
            if (version == 0 || replicas.empty() || !logicalCoverage.Valid() || replicas.size() > coverageCapacity) {
                return std::unexpected(MemoryError::InvalidRegion);
            }
            std::array<ByteCoverageView, coverageCapacity> coverages{};
            size_t coverageCount = 0;
            for (const ResidentReplica& replica : replicas) {
                if (!replica.allocation || replica.version != version || !replica.Coverage().Valid() ||
                    !replica.allocation.View().Covers(replica.allocationOffset, replica.bufferRegion.Size())) {
                    return std::unexpected(MemoryError::InvalidAllocation);
                }
                coverages[coverageCount++] = replica.Coverage();
            }
            if (ExactCoverageUnion<coverageCapacity>(logicalCoverage, {coverages.data(), coverageCount}) !=
                CoverageEquivalence::Equal) {
                return std::unexpected(MemoryError::InvalidRegion);
            }
            replicas_.swap(replicas);
            return std::move(replicas);
        }

        /** @brief Publish one ready replica of the current logical version. */
        [[nodiscard]] std::expected<std::optional<ResidentReplica>, MemoryError>
        Publish(ResidentReplica replica, uint64_t currentVersion, ByteCoverageView logicalCoverage) noexcept {
            if (!logicalCoverage.Valid() || !replica.Coverage().Valid() ||
                logicalCoverage.Contains(replica.Coverage()) != CoverageContainment::Yes) {
                return std::unexpected(MemoryError::InvalidRegion);
            }
            if (currentVersion == 0 || replica.version != currentVersion) {
                return std::unexpected(MemoryError::StaleVersion);
            }
            if (!replica.allocation ||
                !replica.allocation.View().Covers(replica.allocationOffset, replica.bufferRegion.Size())) {
                return std::unexpected(MemoryError::InvalidAllocation);
            }

            const auto sameLocation = [&replica](const ResidentReplica& existing) {
                return existing.version == replica.version &&
                       existing.allocation.View().service == replica.allocation.View().service &&
                       existing.allocation.Location() == replica.allocation.Location() &&
                       existing.Coverage() == replica.Coverage();
            };
            if (auto found = std::ranges::find_if(replicas_, sameLocation); found != replicas_.end()) {
                std::optional<ResidentReplica> retired{std::move(*found)};
                *found = std::move(replica);
                return std::move(retired);
            }
            try {
                replicas_.push_back(std::move(replica));
                return std::nullopt;
            } catch (const std::bad_alloc&) {
                return std::unexpected(MemoryError::OutOfMemory);
            }
        }

        /** @brief Find the smallest ready replica at @p location that contains @p requested. */
        [[nodiscard]] std::optional<ResidentReplica> Find(MemoryLocation location, uint64_t version,
                                                          ByteCoverageView requested) const noexcept {
            const ResidentReplica* best = nullptr;
            for (const ResidentReplica& replica : replicas_) {
                if (replica.allocation.Location() != location || !replica.Covers(version, requested)) {
                    continue;
                }
                if (best == nullptr || replica.bufferRegion.Size() < best->bufferRegion.Size()) {
                    best = &replica;
                }
            }
            return best != nullptr ? std::optional<ResidentReplica>{*best} : std::nullopt;
        }

        /** @brief Find the smallest ready replica containing one contiguous requested interval. */
        [[nodiscard]] std::optional<ResidentReplica> Find(MemoryLocation location, uint64_t version,
                                                          ByteRegion requested) const noexcept {
            return Find(location, version, ByteCoverageView::Interval(requested));
        }

        /** @brief Copy every ready replica of @p version into owning planning leases. */
        [[nodiscard]] std::expected<std::vector<ResidentReplica>, MemoryError>
        Snapshot(uint64_t version) const noexcept {
            try {
                std::vector<ResidentReplica> snapshot;
                snapshot.reserve(replicas_.size());
                for (const ResidentReplica& replica : replicas_) {
                    if (replica.version == version) {
                        snapshot.push_back(replica);
                    }
                }
                return snapshot;
            } catch (const std::bad_alloc&) {
                return std::unexpected(MemoryError::OutOfMemory);
            }
        }

        /** @brief Return the number of retained ready replicas. */
        [[nodiscard]] size_t Size() const noexcept { return replicas_.size(); }

        /** @brief Atomically extract every replica owned by @p service for destruction by the caller. */
        [[nodiscard]] std::expected<std::vector<ResidentReplica>, MemoryError>
        RetireServiceReplicas(BackendServiceId service) noexcept {
            try {
                std::vector<ResidentReplica> retired;
                const size_t count = static_cast<size_t>(std::ranges::count_if(
                    replicas_, [&](const auto& replica) { return replica.allocation.View().service == service; }));
                retired.reserve(count);
                const auto retiredRange = std::ranges::partition(
                    replicas_, [service](const auto& replica) { return replica.allocation.View().service != service; });
                for (ResidentReplica& replica : retiredRange) {
                    retired.push_back(std::move(replica));
                }
                replicas_.erase(retiredRange.begin(), retiredRange.end());
                return retired;
            } catch (const std::bad_alloc&) {
                return std::unexpected(MemoryError::OutOfMemory);
            }
        }

    private:
        std::vector<ResidentReplica> replicas_{};
    };

} // namespace Sora::Compute

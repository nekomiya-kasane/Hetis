/**
 * @file CompletionTransaction.h
 * @brief Atomic publication of completed replicas and whole-buffer writes across logical buffers.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Memory/Buffer.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <utility>
#include <vector>

namespace Sora::Compute {

    /** @brief Stable completion-publication failure with no partial logical-buffer mutation. */
    enum class PublicationErrorCode : uint8_t {
        EmptyTransaction,
        InvalidAction,
        RetiredService,
        ConflictingActions,
        CapacityExceeded,
        StaleVersion,
        VersionExhausted,
        OutOfMemory,
    };

    /** @brief Publication failure identifying the affected logical buffer and action. */
    struct PublicationError {
        PublicationErrorCode code = PublicationErrorCode::InvalidAction;
        BufferId buffer{};
        uint32_t action = UINT32_MAX;

        friend constexpr bool operator==(PublicationError, PublicationError) noexcept = default;
    };

    /** @brief One ordered physical byte interval contributing to a partitioned logical write. */
    struct PartitionedWritePart {
        ByteRegion region{};
        Allocation allocation{};
        uint64_t allocationOffset = 0;
        std::optional<AffineByteFootprint> byteFootprint{};

        /** @brief Borrow the exact logical bytes contributed by this part. */
        [[nodiscard]] ByteCoverageView Coverage() const noexcept {
            return byteFootprint ? ByteCoverageView::Affine(*byteFootprint) : ByteCoverageView::Interval(region);
        }
    };

    /**
     * @brief Move-only completion transaction publishing graph results only after every event succeeds.
     * @details One buffer may receive multiple replicas of its current version, one terminal whole-buffer write, or
     * one ordered partition cover that jointly advances the logical version. Mixed publication kinds are rejected.
     */
    class CompletionTransaction final {
        enum class ActionKind : uint8_t {
            Replica,
            WholeWrite,
            PartitionedWrite,
        };

        struct Action {
            ActionKind kind = ActionKind::Replica;
            Buffer buffer{};
            uint64_t expectedVersion = 0;
            ByteRegion region{};
            Allocation allocation{};
            uint64_t allocationOffset = 0;
            std::optional<AffineByteFootprint> byteFootprint{};

            [[nodiscard]] ByteCoverageView Coverage() const noexcept {
                return byteFootprint ? ByteCoverageView::Affine(*byteFootprint) : ByteCoverageView::Interval(region);
            }
        };

        struct StagedBuffer {
            std::shared_ptr<Buffer::State> state{};
            ResidencyTable residency{};
            uint64_t version = 0;
        };

    public:
        CompletionTransaction() = default;
        CompletionTransaction(const CompletionTransaction&) = delete;
        CompletionTransaction& operator=(const CompletionTransaction&) = delete;
        CompletionTransaction(CompletionTransaction&&) noexcept = default;
        CompletionTransaction& operator=(CompletionTransaction&&) noexcept = default;

        /** @brief Add a ready replica of the buffer version observed during physical planning. */
        [[nodiscard]] std::expected<void, PublicationError> AddReplica(Buffer buffer, uint64_t expectedVersion,
                                                                       ByteRegion region, Allocation allocation,
                                                                       uint64_t allocationOffset = 0) noexcept {
            return AddReplicaCoverage(std::move(buffer), expectedVersion, ByteCoverageView::Interval(region),
                                      std::move(allocation), allocationOffset);
        }

        /** @brief Add a ready exact affine replica of the version observed during physical planning. */
        [[nodiscard]] std::expected<void, PublicationError> AddReplica(Buffer buffer, uint64_t expectedVersion,
                                                                       const AffineByteFootprint& footprint,
                                                                       Allocation allocation,
                                                                       uint64_t allocationOffset = 0) noexcept {
            return AddReplicaCoverage(std::move(buffer), expectedVersion, ByteCoverageView::Affine(footprint),
                                      std::move(allocation), allocationOffset);
        }

        /** @brief Add one exact, non-overlapping cover that jointly advances a buffer by exactly one version. */
        [[nodiscard]] std::expected<void, PublicationError>
        AddPartitionedWrite(Buffer buffer, uint64_t expectedVersion,
                            std::span<const PartitionedWritePart> parts) noexcept {
            constexpr size_t coverageCapacity = 256;
            const BufferDesc desc = buffer ? buffer.Describe() : BufferDesc{};
            if (!buffer || parts.empty()) {
                return std::unexpected(PublicationError{PublicationErrorCode::InvalidAction,
                                                        buffer ? desc.id : BufferId{},
                                                        static_cast<uint32_t>(actions_.size())});
            }
            if (parts.size() > coverageCapacity) {
                return std::unexpected(PublicationError{PublicationErrorCode::CapacityExceeded, desc.id,
                                                        static_cast<uint32_t>(actions_.size())});
            }
            if (HasAnyAction(buffer.state_)) {
                return std::unexpected(PublicationError{PublicationErrorCode::ConflictingActions, desc.id,
                                                        static_cast<uint32_t>(actions_.size())});
            }
            std::array<ByteCoverageView, coverageCapacity> coverages{};
            size_t coverageCount = 0;
            for (const PartitionedWritePart& part : parts) {
                const ByteCoverageView coverage = part.Coverage();
                if (!coverage.Valid() || coverage.BoundingRegion() != part.region || !part.allocation ||
                    part.allocation.Alignment() < desc.alignment ||
                    !part.allocation.View().Covers(part.allocationOffset, part.region.Size())) {
                    return std::unexpected(PublicationError{PublicationErrorCode::InvalidAction, desc.id,
                                                            static_cast<uint32_t>(actions_.size())});
                }
                coverages[coverageCount++] = coverage;
            }
            const CoverageEquivalence equivalence =
                ExactCoverageUnion<coverageCapacity>(buffer.LogicalCoverage(), {coverages.data(), coverageCount});
            if (equivalence != CoverageEquivalence::Equal || parts.size() > UINT32_MAX - actions_.size()) {
                return std::unexpected(PublicationError{parts.size() > UINT32_MAX - actions_.size() ||
                                                                equivalence == CoverageEquivalence::Unknown
                                                            ? PublicationErrorCode::CapacityExceeded
                                                            : PublicationErrorCode::InvalidAction,
                                                        desc.id, static_cast<uint32_t>(actions_.size())});
            }
            try {
                actions_.reserve(actions_.size() + parts.size());
                for (const PartitionedWritePart& part : parts) {
                    actions_.push_back({.kind = ActionKind::PartitionedWrite,
                                        .buffer = buffer,
                                        .expectedVersion = expectedVersion,
                                        .region = part.region,
                                        .allocation = part.allocation,
                                        .allocationOffset = part.allocationOffset,
                                        .byteFootprint = part.byteFootprint});
                }
                return {};
            } catch (const std::bad_alloc&) {
                return std::unexpected(PublicationError{PublicationErrorCode::OutOfMemory, desc.id,
                                                        static_cast<uint32_t>(actions_.size())});
            }
        }

        /** @brief Add one terminal whole-buffer write based on the version observed during physical planning. */
        [[nodiscard]] std::expected<void, PublicationError> AddWholeWrite(Buffer buffer, uint64_t expectedVersion,
                                                                          Allocation allocation) noexcept {
            const BufferDesc desc = buffer ? buffer.Describe() : BufferDesc{};
            if (!buffer || !allocation || allocation.Alignment() < desc.alignment ||
                !allocation.View().Covers(0, desc.sizeBytes)) {
                return std::unexpected(PublicationError{PublicationErrorCode::InvalidAction, desc.id,
                                                        static_cast<uint32_t>(actions_.size())});
            }
            if (HasAnyAction(buffer.state_)) {
                return std::unexpected(PublicationError{PublicationErrorCode::ConflictingActions, desc.id,
                                                        static_cast<uint32_t>(actions_.size())});
            }
            return AddAction({.kind = ActionKind::WholeWrite,
                              .buffer = std::move(buffer),
                              .expectedVersion = expectedVersion,
                              .allocation = std::move(allocation)});
        }

        /** @brief Return the number of logical publication actions. */
        [[nodiscard]] size_t ActionCount() const noexcept { return actions_.size(); }

        /** @brief Validate every expected version and atomically publish all staged residency states. */
        [[nodiscard]] std::expected<void, PublicationError> Commit() && noexcept {
            if (actions_.empty()) {
                return std::unexpected(PublicationError{PublicationErrorCode::EmptyTransaction});
            }
            try {
                std::vector<std::shared_ptr<Buffer::State>> states;
                states.reserve(actions_.size());
                for (const Action& action : actions_) {
                    states.push_back(action.buffer.state_);
                }
                const auto stateLess = [](const auto& left, const auto& right) {
                    return std::less<const void*>{}(left.get(), right.get());
                };
                std::ranges::sort(states, stateLess);
                states.erase(std::ranges::unique(states).begin(), states.end());

                std::vector<StagedBuffer> staged;
                std::vector<std::unique_lock<std::mutex>> locks;
                staged.reserve(states.size());
                locks.reserve(states.size());
                for (const auto& state : states) {
                    locks.emplace_back(state->mutex);
                }
                for (const auto& state : states) {
                    staged.push_back({state, state->residency, state->version});
                }

                for (uint32_t actionIndex = 0; actionIndex < actions_.size(); ++actionIndex) {
                    const Action& action = actions_[actionIndex];
                    if (action.kind == ActionKind::PartitionedWrite && actionIndex != 0 &&
                        actions_[actionIndex - 1].kind == ActionKind::PartitionedWrite &&
                        actions_[actionIndex - 1].buffer.state_ == action.buffer.state_) {
                        continue;
                    }
                    const auto state = std::ranges::lower_bound(states, action.buffer.state_, stateLess);
                    const size_t stateIndex = static_cast<size_t>(state - states.begin());
                    StagedBuffer& target = staged[stateIndex];
                    if (std::ranges::binary_search(target.state->retiredServices, action.allocation.View().service)) {
                        return std::unexpected(
                            PublicationError{PublicationErrorCode::RetiredService, target.state->desc.id, actionIndex});
                    }
                    if (target.version != action.expectedVersion) {
                        return std::unexpected(
                            PublicationError{PublicationErrorCode::StaleVersion, target.state->desc.id, actionIndex});
                    }
                    if (action.kind == ActionKind::Replica) {
                        auto replica = ResidentReplica::Create(action.allocation, action.Coverage(),
                                                               action.allocationOffset, action.expectedVersion);
                        if (!replica) {
                            return std::unexpected(MapMemoryError(replica.error(), target.state->desc.id, actionIndex));
                        }
                        const ByteCoverageView logicalCoverage =
                            Detail::BufferLogicalCoverage(target.state->desc, target.state->logicalFootprint);
                        auto published = target.residency.Publish(std::move(*replica), target.version, logicalCoverage);
                        if (!published) {
                            return std::unexpected(
                                MapMemoryError(published.error(), target.state->desc.id, actionIndex));
                        }
                        continue;
                    }
                    if (target.version == std::numeric_limits<uint64_t>::max()) {
                        return std::unexpected(PublicationError{PublicationErrorCode::VersionExhausted,
                                                                target.state->desc.id, actionIndex});
                    }
                    const uint64_t nextVersion = target.version + 1;
                    if (action.kind == ActionKind::PartitionedWrite) {
                        std::vector<ResidentReplica> replicas;
                        uint32_t end = actionIndex;
                        while (end < actions_.size() && actions_[end].kind == ActionKind::PartitionedWrite &&
                               actions_[end].buffer.state_ == action.buffer.state_) {
                            const Action& part = actions_[end];
                            if (std::ranges::binary_search(target.state->retiredServices,
                                                           part.allocation.View().service)) {
                                return std::unexpected(
                                    PublicationError{PublicationErrorCode::RetiredService, target.state->desc.id, end});
                            }
                            if (part.expectedVersion != target.version) {
                                return std::unexpected(
                                    PublicationError{PublicationErrorCode::StaleVersion, target.state->desc.id, end});
                            }
                            auto replica = ResidentReplica::Create(part.allocation, part.Coverage(),
                                                                   part.allocationOffset, nextVersion);
                            if (!replica) {
                                return std::unexpected(MapMemoryError(replica.error(), target.state->desc.id, end));
                            }
                            replicas.push_back(std::move(*replica));
                            ++end;
                        }
                        const ByteCoverageView logicalCoverage =
                            Detail::BufferLogicalCoverage(target.state->desc, target.state->logicalFootprint);
                        auto reset =
                            target.residency.ResetToPartitionedWrite(std::move(replicas), nextVersion, logicalCoverage);
                        if (!reset) {
                            return std::unexpected(MapMemoryError(reset.error(), target.state->desc.id, actionIndex));
                        }
                        target.version = nextVersion;
                        continue;
                    }
                    const ByteCoverageView logicalCoverage =
                        Detail::BufferLogicalCoverage(target.state->desc, target.state->logicalFootprint);
                    auto reset = target.residency.ResetToWholeWrite(action.allocation, nextVersion,
                                                                    target.state->desc.sizeBytes, logicalCoverage);
                    if (!reset) {
                        return std::unexpected(MapMemoryError(reset.error(), target.state->desc.id, actionIndex));
                    }
                    target.version = nextVersion;
                }

                for (StagedBuffer& source : staged) {
                    std::swap(source.state->residency, source.residency);
                    source.state->version = source.version;
                }
                return {};
            } catch (const std::bad_alloc&) {
                return std::unexpected(PublicationError{PublicationErrorCode::OutOfMemory});
            }
        }

    private:
        [[nodiscard]] std::expected<void, PublicationError> AddReplicaCoverage(Buffer buffer, uint64_t expectedVersion,
                                                                               ByteCoverageView coverage,
                                                                               Allocation allocation,
                                                                               uint64_t allocationOffset) noexcept {
            if (!buffer || expectedVersion == 0 || !coverage.Valid() || !allocation ||
                !allocation.View().Covers(allocationOffset, coverage.BoundingRegion().Size())) {
                return std::unexpected(PublicationError{PublicationErrorCode::InvalidAction,
                                                        buffer ? buffer.Describe().id : BufferId{},
                                                        static_cast<uint32_t>(actions_.size())});
            }
            const BufferDesc desc = buffer.Describe();
            if (buffer.LogicalCoverage().Contains(coverage) != CoverageContainment::Yes || HasWrite(buffer.state_)) {
                return std::unexpected(PublicationError{PublicationErrorCode::ConflictingActions, desc.id,
                                                        static_cast<uint32_t>(actions_.size())});
            }
            std::optional<AffineByteFootprint> footprint;
            if (const AffineByteFootprint* affine = coverage.BorrowedAffineFootprint(); affine != nullptr) {
                footprint = *affine;
            }
            return AddAction({.kind = ActionKind::Replica,
                              .buffer = std::move(buffer),
                              .expectedVersion = expectedVersion,
                              .region = coverage.BoundingRegion(),
                              .allocation = std::move(allocation),
                              .allocationOffset = allocationOffset,
                              .byteFootprint = std::move(footprint)});
        }

        [[nodiscard]] bool HasAnyAction(const std::shared_ptr<Buffer::State>& state) const noexcept {
            return std::ranges::any_of(actions_,
                                       [&state](const Action& action) { return action.buffer.state_ == state; });
        }

        [[nodiscard]] bool HasWrite(const std::shared_ptr<Buffer::State>& state) const noexcept {
            return std::ranges::any_of(actions_, [&state](const Action& action) {
                return action.buffer.state_ == state && action.kind != ActionKind::Replica;
            });
        }

        [[nodiscard]] std::expected<void, PublicationError> AddAction(Action action) noexcept {
            const BufferId buffer = action.buffer.Describe().id;
            if (actions_.size() == UINT32_MAX) {
                return std::unexpected(PublicationError{PublicationErrorCode::CapacityExceeded, buffer, UINT32_MAX});
            }
            try {
                actions_.push_back(std::move(action));
                return {};
            } catch (const std::bad_alloc&) {
                return std::unexpected(PublicationError{PublicationErrorCode::OutOfMemory, buffer,
                                                        static_cast<uint32_t>(actions_.size())});
            }
        }

        [[nodiscard]] static constexpr PublicationError MapMemoryError(MemoryError error, BufferId buffer,
                                                                       uint32_t action) noexcept {
            switch (error) {
                case MemoryError::StaleVersion:
                    return {PublicationErrorCode::StaleVersion, buffer, action};
                case MemoryError::RetiredService:
                    return {PublicationErrorCode::RetiredService, buffer, action};
                case MemoryError::VersionExhausted:
                    return {PublicationErrorCode::VersionExhausted, buffer, action};
                case MemoryError::OutOfMemory:
                    return {PublicationErrorCode::OutOfMemory, buffer, action};
                default:
                    return {PublicationErrorCode::InvalidAction, buffer, action};
            }
        }

        std::vector<Action> actions_{};
    };

} // namespace Sora::Compute

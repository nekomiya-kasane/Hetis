/**
 * @file GraphResidency.h
 * @brief Exact symbolic residency for frozen and graph-produced values during pure graph planning.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Graph/GraphModel.h>
#include <Sora/Compute/Memory/Buffer.h>
#include <Sora/Compute/Memory/ResidencyPlanner.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace Sora::Compute {

    class ExecutionGraph;

    /** @brief One service-scoped graph-local replica and the graph node that materializes it. */
    struct GraphLocalResidencyReplica {
        BackendServiceId service{};
        MemoryLocation location{};
        GraphNodeId materializer{};

        /** @brief Return whether every physical and logical identity is complete. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return service.Valid() && location.Valid() && materializer.Valid();
        }

        friend constexpr bool operator==(GraphLocalResidencyReplica, GraphLocalResidencyReplica) noexcept = default;
        friend constexpr auto operator<=>(GraphLocalResidencyReplica, GraphLocalResidencyReplica) noexcept = default;
    };

    /** @brief One maximal exact coverage sharing a latest logical origin and graph-local replica set. */
    struct GraphResidencySegment {
        BufferId buffer{};
        ByteRegion region{};
        std::optional<AffineByteFootprint> byteFootprint{};
        uint64_t version = 0;
        std::optional<GraphNodeId> producer{};
        std::vector<GraphLocalResidencyReplica> replicas{};

        /** @brief Borrow the exact bytes owned by this latest logical value. */
        [[nodiscard]] ByteCoverageView Coverage() const noexcept {
            return byteFootprint ? ByteCoverageView::Affine(*byteFootprint) : ByteCoverageView::Interval(region);
        }

        friend bool operator==(const GraphResidencySegment&, const GraphResidencySegment&) noexcept = default;
    };

    /** @brief Selected graph-local source for one exact route. */
    struct GraphLocalResidencySource {
        BufferId buffer{};
        ByteRegion coverage{};
        std::optional<AffineByteFootprint> byteFootprint{};
        uint64_t version = 0;
        std::optional<GraphNodeId> producer{};
        GraphLocalResidencyReplica replica{};

        /** @brief Borrow the exact bytes available from this symbolic source. */
        [[nodiscard]] ByteCoverageView Coverage() const noexcept {
            return byteFootprint ? ByteCoverageView::Affine(*byteFootprint) : ByteCoverageView::Interval(coverage);
        }

        /** @brief Return whether this source contains @p requested with one complete symbolic identity. */
        [[nodiscard]] bool Covers(ByteCoverageView requested) const noexcept {
            return buffer.Valid() && coverage.Size() != 0 && version != 0 && (!producer || producer->Valid()) &&
                   replica.Valid() && Coverage().Contains(requested) == CoverageContainment::Yes;
        }
    };

    /** @brief Ready physical or graph-local source selected for one exact route. */
    using GraphResidencySource = std::variant<ResidentReplica, GraphLocalResidencySource>;

    /** @brief Exact route from a closed graph-level source variant to one service-scoped destination. */
    struct GraphResidencyRoute {
        BufferId buffer{};
        GraphResidencySource source{};
        BackendServiceId destinationService{};
        MemoryLocation destination{};
        ByteRegion region{};
        std::optional<AffineByteFootprint> byteFootprint{};
        uint64_t version = 0;
        uint64_t estimatedNanoseconds = 0;
        std::vector<TransferLink> hops{};

        /** @brief Return whether the selected source is already available at the destination. */
        [[nodiscard]] bool AlreadyResident() const noexcept { return hops.empty(); }

        /** @brief Borrow the exact logical bytes carried by this route. */
        [[nodiscard]] ByteCoverageView Coverage() const noexcept {
            return byteFootprint ? ByteCoverageView::Affine(*byteFootprint) : ByteCoverageView::Interval(region);
        }

        /** @brief Return whether source coverage and every service-scoped topology hop are continuous. */
        [[nodiscard]] bool Valid() const noexcept {
            const ByteCoverageView coverage = Coverage();
            if (!buffer.Valid() || !destinationService.Valid() || !destination.Valid() || !coverage.Valid() ||
                coverage.BoundingRegion() != region || version == 0) {
                return false;
            }
            BackendServiceId service{};
            MemoryLocation location{};
            if (const auto* ready = std::get_if<ResidentReplica>(&source)) {
                if (!ready->allocation || !ready->Covers(version, coverage)) {
                    return false;
                }
                service = ready->allocation.View().service;
                location = ready->allocation.Location();
            } else {
                const auto& graphLocal = std::get<GraphLocalResidencySource>(source);
                if (graphLocal.buffer != buffer || graphLocal.version != version || !graphLocal.Covers(coverage)) {
                    return false;
                }
                service = graphLocal.replica.service;
                location = graphLocal.replica.location;
            }
            for (const TransferLink& hop : hops) {
                if (!hop.Valid() || hop.sourceService != service || hop.source != location ||
                    region.Size() > hop.maximumTransferBytes) {
                    return false;
                }
                service = hop.service;
                location = hop.destination;
            }
            return service == destinationService && location == destination &&
                   (hops.empty() || estimatedNanoseconds != 0);
        }
    };

    /** @brief Latest-value overwrite installed by one selected logical writer. */
    struct ProducedResidencyWrite {
        BufferId buffer{};
        ByteRegion region{};
        std::optional<AffineByteFootprint> byteFootprint{};
        uint64_t version = 0;
        GraphNodeId producer{};
        GraphLocalResidencyReplica replica{};

        /** @brief Borrow the exact bytes overwritten by this transition. */
        [[nodiscard]] ByteCoverageView Coverage() const noexcept {
            return byteFootprint ? ByteCoverageView::Affine(*byteFootprint) : ByteCoverageView::Interval(region);
        }
    };

    /** @brief Additional graph-local location for one frozen or graph-produced latest value. */
    struct GraphResidencyReplicaAddition {
        BufferId buffer{};
        ByteRegion region{};
        std::optional<AffineByteFootprint> byteFootprint{};
        uint64_t version = 0;
        std::optional<GraphNodeId> producer{};
        GraphLocalResidencyReplica replica{};

        /** @brief Borrow the exact bytes materialized at the additional location. */
        [[nodiscard]] ByteCoverageView Coverage() const noexcept {
            return byteFootprint ? ByteCoverageView::Affine(*byteFootprint) : ByteCoverageView::Interval(region);
        }
    };

    /** @brief Atomic symbolic transition emitted after one candidate projection is selected. */
    struct GraphResidencyUpdate {
        std::vector<GraphResidencyReplicaAddition> replicas{};
        std::vector<ProducedResidencyWrite> writes{};
    };

    /** @brief Final logical reader of one buffer in an ordered planning domain. */
    struct GraphBufferReadLifetime {
        BufferId buffer{};
        GraphNodeId lastReader{};

        /** @brief Return whether both the buffer and final reader identities are complete. */
        [[nodiscard]] constexpr bool Valid() const noexcept { return buffer.Valid() && lastReader.Valid(); }

        friend constexpr bool operator==(GraphBufferReadLifetime, GraphBufferReadLifetime) noexcept = default;
        friend constexpr auto operator<=>(GraphBufferReadLifetime, GraphBufferReadLifetime) noexcept = default;
    };

    /** @brief Stable rejection while constructing or advancing symbolic graph residency. */
    enum class GraphResidencyError : uint8_t {
        InvalidRequest,
        MissingBuffer,
        BaseVersionMismatch,
        VersionExhausted,
        MissingLogicalValue,
        OverlappingWrites,
        IndeterminateCoverage,
        CapacityExceeded,
        OutOfMemory,
    };

    /** @brief Stable stage at which one graph-wide ready-residency snapshot failed. */
    enum class GraphResidencySnapshotErrorCode : uint8_t {
        InvalidGraph,
        ConflictingBufferIdentity,
        SnapshotFailed,
        OutOfMemory,
    };

    /** @brief Graph-wide snapshot failure with the first logical node and buffer involved. */
    struct GraphResidencySnapshotError {
        GraphResidencySnapshotErrorCode code = GraphResidencySnapshotErrorCode::InvalidGraph;
        GraphNodeId node{};
        BufferId buffer{};
        uint64_t cause = 0;

        friend constexpr bool operator==(GraphResidencySnapshotError, GraphResidencySnapshotError) noexcept = default;
    };

    /**
     * @brief Sorted exact-coverage overlay for latest frozen and graph-produced values.
     * @details Buffers and coverages use contiguous sorted vectors. Continuous values retain interval splitting;
     * affine values remain compressed descriptors. Apply() has the strong guarantee: every failure leaves the state
     * byte-for-byte equivalent to its pre-call value.
     */
    class GraphResidencyState final {
        struct BaselineBuffer {
            Buffer buffer{};
            BufferResidencySnapshot snapshot{};
        };

        struct BaselineStorage {
            std::vector<BaselineBuffer> buffers{};
        };

        struct GraphLocalBuffer {
            BufferId id{};
            std::vector<GraphResidencySegment> segments{};

            friend bool operator==(const GraphLocalBuffer&, const GraphLocalBuffer&) noexcept = default;
        };

    public:
        /** @brief Register one immutable buffer descriptor and its frozen pre-graph logical version. */
        [[nodiscard]] std::expected<void, GraphResidencyError> Track(BufferDesc desc, uint64_t baseVersion) noexcept;

        /** @brief Snapshot and register one exact ready-residency baseline with its logical buffer lease. */
        [[nodiscard]] std::expected<void, GraphResidencyError> Track(Buffer buffer) noexcept;

        /** @brief Return the frozen pre-graph version of @p buffer, or no value when it is not tracked. */
        [[nodiscard]] std::optional<uint64_t> BaseVersion(BufferId buffer) const noexcept;

        /** @brief Return the attempt-frozen ready-residency snapshot of @p buffer, or null when untracked. */
        [[nodiscard]] const BufferResidencySnapshot* ReadySnapshot(BufferId buffer) const noexcept;

        /** @brief Return whether @p buffer is the exact logical owner frozen for its identity. */
        [[nodiscard]] bool SharesBufferOwnership(const Buffer& buffer) const noexcept;

        /** @brief Return every graph-local segment of @p buffer in ascending bounding-interval order. */
        [[nodiscard]] std::span<const GraphResidencySegment> Segments(BufferId buffer) const noexcept;

        /**
         * @brief Return whether both states descend from one frozen baseline and have identical canonical overlays.
         * @details This is the exact Bellman-state equivalence relation for one planning attempt. Independently frozen
         * baselines are conservatively distinct even when their current values happen to compare equal.
         */
        [[nodiscard]] bool SamePlanningState(const GraphResidencyState& other) const noexcept;

        /** @brief Validate and atomically apply replica additions followed by latest-value overwrites. */
        [[nodiscard]] std::expected<void, GraphResidencyError> Apply(const GraphResidencyUpdate& update) noexcept;

        /**
         * @brief Apply @p update in place to a consumed state and return the advanced state.
         * @details Failure may mutate the consumed object, which must not be observed after this call. This removes the
         * rollback copy when a planner already owns an isolated transition state.
         */
        [[nodiscard]] std::expected<GraphResidencyState, GraphResidencyError>
        Transition(const GraphResidencyUpdate& update) && noexcept;

        /** @brief Discard produced intervals whose logical producer precedes @p firstLiveProducer. */
        void DiscardProducedBefore(GraphNodeId firstLiveProducer) noexcept;

        /**
         * @brief Discard overlays for buffers with no reader after @p completedReader.
         * @pre @p lifetimes is sorted by unique buffer identity and contains every graph buffer that is ever read.
         */
        void DiscardReadDeadBuffers(GraphNodeId completedReader,
                                    std::span<const GraphBufferReadLifetime> lifetimes) noexcept;

    private:
        [[nodiscard]] std::expected<void, GraphResidencyError> ApplyTo(std::vector<GraphLocalBuffer>& candidate,
                                                                       const GraphResidencyUpdate& update);

        [[nodiscard]] std::expected<void, GraphResidencyError> Track(Buffer buffer,
                                                                     BufferResidencySnapshot snapshot) noexcept;

        std::shared_ptr<BaselineStorage> baseline_{};
        std::vector<GraphLocalBuffer> graphLocal_{};
    };

    /** @brief Freeze every distinct logical buffer referenced by @p graph exactly once for one planning attempt. */
    [[nodiscard]] std::expected<GraphResidencyState, GraphResidencySnapshotError>
    FreezeGraphResidency(const ExecutionGraph& graph) noexcept;

} // namespace Sora::Compute

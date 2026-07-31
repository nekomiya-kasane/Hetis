/**
 * @file ResidencyPlanner.h
 * @brief Pure selection of ready replicas and minimum-cost transfer routes.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Cost.h>
#include <Sora/Compute/Memory/Buffer.h>
#include <Sora/Compute/Memory/Transfer.h>

#include <algorithm>
#include <cstddef>
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

    /** @brief Requested ready location and logical byte region. */
    struct ResidencyRequest {
        BackendServiceId service{};
        MemoryLocation destination{};
        ByteRegion region{};
        ByteCoverageView coverage{};

        /** @brief Borrow the exact requested bytes, defaulting to the interval fast path. */
        [[nodiscard]] constexpr ByteCoverageView Coverage() const noexcept {
            return coverage.Valid() ? coverage : ByteCoverageView::Interval(region);
        }
    };

    /** @brief Chosen source lease and owned route to one destination location. */
    struct ResidencyRoute {
        BufferId buffer{};
        ResidentReplica source{};
        BackendServiceId destinationService{};
        MemoryLocation destination{};
        ByteRegion region{};
        std::optional<AffineByteFootprint> byteFootprint{};
        uint64_t version = 0;
        uint64_t estimatedNanoseconds = 0;
        std::vector<TransferLink> hops{};

        /** @brief Return whether the requested region is already ready at the destination. */
        [[nodiscard]] bool AlreadyResident() const noexcept { return hops.empty(); }

        /** @brief Borrow the exact logical bytes carried by this route. */
        [[nodiscard]] ByteCoverageView Coverage() const noexcept {
            return byteFootprint ? ByteCoverageView::Affine(*byteFootprint) : ByteCoverageView::Interval(region);
        }
    };

    /** @brief Service-scoped location of one abstract ready or graph-produced residency source. */
    struct ResidencyOrigin {
        BackendServiceId service{};
        MemoryLocation location{};

        /** @brief Return whether this origin can participate in physical transfer routing. */
        [[nodiscard]] constexpr bool Valid() const noexcept { return service.Valid() && location.Valid(); }

        friend constexpr bool operator==(ResidencyOrigin, ResidencyOrigin) noexcept = default;
        friend constexpr auto operator<=>(ResidencyOrigin, ResidencyOrigin) noexcept = default;
    };

    /** @brief Minimum-cost route from one selected abstract origin to an exact destination. */
    struct ResidencyPath {
        size_t origin = std::numeric_limits<size_t>::max();
        BackendServiceId destinationService{};
        MemoryLocation destination{};
        uint64_t estimatedNanoseconds = 0;
        std::vector<TransferLink> hops{};

        /** @brief Return whether source and destination denote the same service-scoped location. */
        [[nodiscard]] bool AlreadyResident() const noexcept { return hops.empty(); }
    };

    /** @brief Stable failure while planning a residency route without mutating backend state. */
    enum class ResidencyPlanningError : uint8_t {
        InvalidRequest,
        InvalidTopology,
        UninitializedBuffer,
        NoSourceReplica,
        NoRoute,
        CapacityExceeded,
        OutOfMemory,
    };

    namespace Detail {

        struct ResidencySearchNode {
            BackendServiceId service{};
            MemoryLocation location{};
            uint64_t distance = std::numeric_limits<uint64_t>::max();
            size_t hopCount = std::numeric_limits<size_t>::max();
            size_t sourceReplica = std::numeric_limits<size_t>::max();
            size_t predecessorNode = std::numeric_limits<size_t>::max();
            size_t predecessorLink = std::numeric_limits<size_t>::max();
            bool visited = false;
        };

        [[nodiscard]] constexpr uint64_t TransferCost(const TransferLink& link, uint64_t bytes) noexcept {
            const uint64_t payload =
                link.kind == TransferKind::CoherentAlias ? 0 : RateCost(bytes, link.bytesPerSecond);
            return SaturatingAdd(link.latencyNanoseconds, payload);
        }

        [[nodiscard]] inline size_t FindLocation(std::span<const ResidencySearchNode> nodes, BackendServiceId service,
                                                 MemoryLocation location) noexcept {
            for (size_t index = 0; index < nodes.size(); ++index) {
                if (nodes[index].service == service && nodes[index].location == location) {
                    return index;
                }
            }
            return std::numeric_limits<size_t>::max();
        }

        inline void AddLocation(std::vector<ResidencySearchNode>& nodes, BackendServiceId service,
                                MemoryLocation location) {
            if (FindLocation(nodes, service, location) == std::numeric_limits<size_t>::max()) {
                nodes.push_back({.service = service, .location = location});
            }
        }

        [[nodiscard]] inline bool BetterPath(uint64_t distance, size_t hopCount, size_t sourceReplica,
                                             BackendServiceId predecessorService, MemoryLocation predecessor,
                                             TransferEngineId engine, const ResidencySearchNode& current,
                                             std::span<const ResidencySearchNode> nodes,
                                             std::span<const TransferLink> topology) noexcept {
            if (distance != current.distance) {
                return distance < current.distance;
            }
            if (hopCount != current.hopCount) {
                return hopCount < current.hopCount;
            }
            if (sourceReplica != current.sourceReplica) {
                return sourceReplica < current.sourceReplica;
            }
            if (current.predecessorNode == std::numeric_limits<size_t>::max()) {
                return true;
            }
            const ResidencySearchNode& currentPredecessor = nodes[current.predecessorNode];
            if (predecessorService != currentPredecessor.service) {
                return predecessorService < currentPredecessor.service;
            }
            if (predecessor != currentPredecessor.location) {
                return predecessor < currentPredecessor.location;
            }
            return engine < topology[current.predecessorLink].engine;
        }

        [[nodiscard]] inline size_t SelectNextNode(std::span<const ResidencySearchNode> nodes) noexcept {
            size_t best = std::numeric_limits<size_t>::max();
            for (size_t index = 0; index < nodes.size(); ++index) {
                if (nodes[index].visited || nodes[index].distance == std::numeric_limits<uint64_t>::max()) {
                    continue;
                }
                if (best == std::numeric_limits<size_t>::max() || nodes[index].distance < nodes[best].distance ||
                    (nodes[index].distance == nodes[best].distance &&
                     (nodes[index].hopCount < nodes[best].hopCount ||
                      (nodes[index].hopCount == nodes[best].hopCount &&
                       (nodes[index].service < nodes[best].service ||
                        (nodes[index].service == nodes[best].service &&
                         nodes[index].location < nodes[best].location)))))) {
                    best = index;
                }
            }
            return best;
        }

        template<typename OriginAt>
        [[nodiscard]] std::expected<ResidencyPath, ResidencyPlanningError>
        PlanResidencyPathUnchecked(size_t originCount, OriginAt&& originAt, const ResidencyRequest& request,
                                   std::span<const TransferLink> topology) noexcept {
            try {
                constexpr size_t maximum = std::numeric_limits<size_t>::max();
                if (originCount == maximum || topology.size() > (maximum - originCount - 1) / 2) {
                    return std::unexpected(ResidencyPlanningError::CapacityExceeded);
                }

                std::vector<ResidencySearchNode> nodes;
                nodes.reserve(topology.size() * 2 + originCount + 1);
                AddLocation(nodes, request.service, request.destination);
                for (const TransferLink& link : topology) {
                    AddLocation(nodes, link.sourceService, link.source);
                    AddLocation(nodes, link.service, link.destination);
                }

                bool hasSource = false;
                for (size_t originIndex = 0; originIndex < originCount; ++originIndex) {
                    const std::optional<ResidencyOrigin> origin = originAt(originIndex);
                    if (!origin) {
                        continue;
                    }
                    hasSource = true;
                    AddLocation(nodes, origin->service, origin->location);
                    auto& node = nodes[FindLocation(nodes, origin->service, origin->location)];
                    if (node.distance != 0 || originIndex < node.sourceReplica) {
                        node.distance = 0;
                        node.hopCount = 0;
                        node.sourceReplica = originIndex;
                    }
                }
                if (!hasSource) {
                    return std::unexpected(ResidencyPlanningError::NoSourceReplica);
                }

                const size_t targetNode = FindLocation(nodes, request.service, request.destination);
                while (true) {
                    const size_t sourceNode = SelectNextNode(nodes);
                    if (sourceNode == maximum || sourceNode == targetNode) {
                        break;
                    }
                    nodes[sourceNode].visited = true;
                    for (size_t linkIndex = 0; linkIndex < topology.size(); ++linkIndex) {
                        const TransferLink& link = topology[linkIndex];
                        if (link.sourceService != nodes[sourceNode].service ||
                            link.source != nodes[sourceNode].location ||
                            request.region.Size() > link.maximumTransferBytes) {
                            continue;
                        }
                        const uint64_t stepCost = TransferCost(link, request.region.Size());
                        const uint64_t distance = SaturatingAdd(nodes[sourceNode].distance, stepCost);
                        if (distance == std::numeric_limits<uint64_t>::max()) {
                            continue;
                        }
                        const size_t destinationNode = FindLocation(nodes, link.service, link.destination);
                        const size_t hopCount = nodes[sourceNode].hopCount + 1;
                        if (BetterPath(distance, hopCount, nodes[sourceNode].sourceReplica, nodes[sourceNode].service,
                                       nodes[sourceNode].location, link.engine, nodes[destinationNode], nodes,
                                       topology)) {
                            nodes[destinationNode].distance = distance;
                            nodes[destinationNode].hopCount = hopCount;
                            nodes[destinationNode].sourceReplica = nodes[sourceNode].sourceReplica;
                            nodes[destinationNode].predecessorNode = sourceNode;
                            nodes[destinationNode].predecessorLink = linkIndex;
                        }
                    }
                }

                if (nodes[targetNode].distance == std::numeric_limits<uint64_t>::max()) {
                    return std::unexpected(ResidencyPlanningError::NoRoute);
                }
                std::vector<TransferLink> hops;
                hops.reserve(nodes[targetNode].hopCount);
                for (size_t node = targetNode; nodes[node].predecessorNode != maximum;
                     node = nodes[node].predecessorNode) {
                    hops.push_back(topology[nodes[node].predecessorLink]);
                }
                std::ranges::reverse(hops);
                return ResidencyPath{.origin = nodes[targetNode].sourceReplica,
                                     .destinationService = request.service,
                                     .destination = request.destination,
                                     .estimatedNanoseconds = nodes[targetNode].distance,
                                     .hops = std::move(hops)};
            } catch (const std::bad_alloc&) {
                return std::unexpected(ResidencyPlanningError::OutOfMemory);
            }
        }

    } // namespace Detail

    /** @brief Select the minimum-cost service-scoped path from abstract @p origins. */
    [[nodiscard]] inline std::expected<ResidencyPath, ResidencyPlanningError>
    PlanResidencyPath(std::span<const ResidencyOrigin> origins, const ResidencyRequest& request,
                      std::span<const TransferLink> topology) noexcept {
        if (!request.service.Valid() || !request.destination.Valid() || request.region.Size() == 0) {
            return std::unexpected(ResidencyPlanningError::InvalidRequest);
        }
        for (const ResidencyOrigin origin : origins) {
            if (!origin.Valid()) {
                return std::unexpected(ResidencyPlanningError::InvalidRequest);
            }
        }
        for (const TransferLink& link : topology) {
            if (!link.Valid()) {
                return std::unexpected(ResidencyPlanningError::InvalidTopology);
            }
        }
        return Detail::PlanResidencyPathUnchecked(
            origins.size(), [&origins](size_t index) { return std::optional{origins[index]}; }, request, topology);
    }

    /**
     * @brief Select an existing destination replica or the minimum-cost transfer route from @p snapshot.
     * @details This operation is pure with respect to buffers and backends. The returned source allocation is an
     * owning lease; a later materializer allocates hop targets, emits physical batches, and publishes on completion.
     */
    [[nodiscard]] inline std::expected<ResidencyRoute, ResidencyPlanningError>
    PlanResidency(const BufferResidencySnapshot& snapshot, const ResidencyRequest& request,
                  std::span<const TransferLink> topology) noexcept {
        const ByteCoverageView requested = request.Coverage();
        if (!snapshot.desc.Valid() || !request.service.Valid() || !request.destination.Valid() ||
            request.region.Size() == 0 || !requested.Valid() || requested.BoundingRegion() != request.region ||
            snapshot.LogicalCoverage().Contains(requested) != CoverageContainment::Yes) {
            return std::unexpected(ResidencyPlanningError::InvalidRequest);
        }
        if (snapshot.version == 0) {
            return std::unexpected(ResidencyPlanningError::UninitializedBuffer);
        }
        for (const TransferLink& link : topology) {
            if (!link.Valid()) {
                return std::unexpected(ResidencyPlanningError::InvalidTopology);
            }
        }

        size_t readyDestination = std::numeric_limits<size_t>::max();
        for (size_t index = 0; index < snapshot.replicas.size(); ++index) {
            const ResidentReplica& replica = snapshot.replicas[index];
            if (replica.allocation.View().service == request.service &&
                replica.allocation.Location() == request.destination && replica.Covers(snapshot.version, requested) &&
                (readyDestination == std::numeric_limits<size_t>::max() ||
                 replica.bufferRegion.Size() < snapshot.replicas[readyDestination].bufferRegion.Size())) {
                readyDestination = index;
            }
        }
        if (readyDestination != std::numeric_limits<size_t>::max()) {
            std::optional<AffineByteFootprint> footprint;
            if (const AffineByteFootprint* affine = requested.BorrowedAffineFootprint(); affine != nullptr) {
                footprint = *affine;
            }
            return ResidencyRoute{.buffer = snapshot.desc.id,
                                  .source = snapshot.replicas[readyDestination],
                                  .destinationService = request.service,
                                  .destination = request.destination,
                                  .region = request.region,
                                  .byteFootprint = std::move(footprint),
                                  .version = snapshot.version};
        }

        auto path = Detail::PlanResidencyPathUnchecked(
            snapshot.replicas.size(),
            [&](size_t index) -> std::optional<ResidencyOrigin> {
                const ResidentReplica& replica = snapshot.replicas[index];
                if (!replica.Covers(snapshot.version, requested)) {
                    return std::nullopt;
                }
                return ResidencyOrigin{replica.allocation.View().service, replica.allocation.Location()};
            },
            request, topology);
        if (!path) {
            return std::unexpected(path.error());
        }
        std::optional<AffineByteFootprint> footprint;
        if (const AffineByteFootprint* affine = requested.BorrowedAffineFootprint(); affine != nullptr) {
            footprint = *affine;
        }
        return ResidencyRoute{.buffer = snapshot.desc.id,
                              .source = snapshot.replicas[path->origin],
                              .destinationService = path->destinationService,
                              .destination = path->destination,
                              .region = request.region,
                              .byteFootprint = std::move(footprint),
                              .version = snapshot.version,
                              .estimatedNanoseconds = path->estimatedNanoseconds,
                              .hops = std::move(path->hops)};
    }

} // namespace Sora::Compute

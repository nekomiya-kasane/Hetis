/**
 * @file PhysicalGraphPlanner.cpp
 * @brief Pure residency-route expansion for physical graph planning.
 * @ingroup Compute
 */

#include <Sora/Compute/Runtime/PhysicalGraphPlanner.h>

#include <algorithm>
#include <bit>
#include <memory>
#include <new>
#include <ranges>
#include <tuple>
#include <variant>

namespace Sora::Compute {

    namespace {

        [[nodiscard]] PhysicalPlanSegmentId StorageReadyAfter(const PlannedStorageRef& storage) noexcept {
            const auto* planned = std::get_if<PlannedAllocationRef>(&storage);
            return planned != nullptr ? planned->readyAfter : PhysicalPlanSegmentId{};
        }

        [[nodiscard]] constexpr auto GraphLocalStorageKey(const GraphLocalResidencySource& source) noexcept {
            return std::tie(source.buffer, source.coverage, source.byteFootprint, source.version, source.producer,
                            source.replica.service, source.replica.location, source.replica.materializer);
        }

        [[nodiscard]] constexpr auto GraphLocalStorageKey(const GraphLocalStorageBinding& binding) noexcept {
            return GraphLocalStorageKey(binding.symbolic);
        }

        inline constexpr auto kGraphLocalStorageProjection = [](const GraphLocalStorageBinding& binding) noexcept {
            return GraphLocalStorageKey(binding);
        };

        [[nodiscard]] std::expected<ResidencyPlanFragment, PhysicalGraphPlanningError>
        ExpandPhysicalResidency(Buffer buffer, GraphNodeId logicalConsumer, PlannedStorageRef source,
                                BackendServiceId sourceService, MemoryLocation sourceLocation,
                                BackendServiceId destinationService, MemoryLocation destination,
                                ByteCoverageView coverage, uint64_t version, uint64_t estimatedNanoseconds,
                                std::span<const TransferLink> hops, uint32_t allocationBase, uint32_t segmentBase,
                                ResidencyPlanOptions options) noexcept {
            const ByteRegion region = coverage.BoundingRegion();
            if (!buffer || !logicalConsumer.Valid() || !sourceService.Valid() || !sourceLocation.Valid() ||
                !destinationService.Valid() || !destination.Valid() || !coverage.Valid() || version == 0 ||
                !options.requiredProperties.Valid() || !options.preferredProperties.Valid() ||
                !std::has_single_bit(options.alignment)) {
                return std::unexpected(PhysicalGraphPlanningError::InvalidRoute);
            }
            size_t asynchronousHopCount = 0;
            BackendServiceId currentService = sourceService;
            MemoryLocation currentLocation = sourceLocation;
            for (const TransferLink& hop : hops) {
                if (!hop.Valid() || hop.sourceService != currentService || hop.source != currentLocation ||
                    region.Size() > hop.maximumTransferBytes) {
                    return std::unexpected(PhysicalGraphPlanningError::InvalidRoute);
                }
                asynchronousHopCount += hop.kind != TransferKind::CoherentAlias;
                currentService = hop.service;
                currentLocation = hop.destination;
            }
            if (currentService != destinationService || currentLocation != destination ||
                hops.size() > UINT32_MAX - allocationBase || asynchronousHopCount > UINT32_MAX - segmentBase) {
                return std::unexpected(currentService != destinationService || currentLocation != destination
                                           ? PhysicalGraphPlanningError::InvalidRoute
                                           : PhysicalGraphPlanningError::CapacityExceeded);
            }

            try {
                ResidencyPlanFragment fragment{.destination = std::move(source),
                                               .estimatedNanoseconds = estimatedNanoseconds};
                fragment.storage.allocations.reserve(hops.size());
                fragment.storage.segments.reserve(asynchronousHopCount);
                fragment.storage.edges.reserve(asynchronousHopCount);
                fragment.transfers.reserve(asynchronousHopCount);

                for (uint32_t hopIndex = 0; hopIndex < hops.size(); ++hopIndex) {
                    const TransferLink& hop = hops[hopIndex];
                    const PlannedAllocationId allocationId{allocationBase +
                                                           static_cast<uint32_t>(fragment.storage.allocations.size())};
                    const PhysicalPlanSegmentId inheritedReadiness = StorageReadyAfter(fragment.destination);
                    PlannedAllocation allocation{.id = allocationId,
                                                 .buffer = buffer,
                                                 .service = hop.service,
                                                 .location = hop.destination,
                                                 .bufferRegion = region,
                                                 .version = version};
                    if (const AffineByteFootprint* affine = coverage.BorrowedAffineFootprint(); affine != nullptr) {
                        allocation.byteFootprint = *affine;
                    }
                    PlannedAllocationRef target{allocationId, inheritedReadiness};
                    if (hop.kind == TransferKind::CoherentAlias) {
                        allocation.materialization =
                            PlannedCoherentAlias{.engine = hop.engine, .source = fragment.destination};
                    } else {
                        const PhysicalPlanSegmentId segmentId{segmentBase +
                                                              static_cast<uint32_t>(fragment.storage.segments.size())};
                        allocation.materialization = PlannedAllocate{.requiredProperties = options.requiredProperties,
                                                                     .preferredProperties = options.preferredProperties,
                                                                     .alignment = options.alignment};
                        target.readyAfter = segmentId;
                        fragment.storage.segments.emplace_back(
                            PlannedTransfer{.logicalNode = logicalConsumer,
                                            .service = hop.service,
                                            .engine = hop.engine,
                                            .buffer = buffer.Describe().id,
                                            .region = region,
                                            .byteFootprint = allocation.byteFootprint,
                                            .version = version,
                                            .source = fragment.destination,
                                            .destination = allocationId});
                        fragment.transfers.push_back({hopIndex, segmentId});
                        if (inheritedReadiness.Valid()) {
                            fragment.storage.edges.push_back({inheritedReadiness, segmentId});
                        }
                    }
                    fragment.storage.allocations.push_back(std::move(allocation));
                    fragment.destination = target;
                }
                if (!fragment.storage.allocations.empty() && options.publishDestination) {
                    fragment.storage.allocations.back().publication = PlannedPublication{
                        .kind = PlannedPublicationKind::Replica,
                        .expectedVersion = version,
                        .producer = StorageReadyAfter(fragment.destination),
                    };
                }
                return fragment;
            } catch (const std::bad_alloc&) {
                return std::unexpected(PhysicalGraphPlanningError::OutOfMemory);
            }
        }

        struct ResolvedRouteSource {
            PlannedStorageRef storage{};
            BackendServiceId service{};
            MemoryLocation location{};
        };

        [[nodiscard]] bool ValidAssemblyPartBinding(const GraphLocalResidencySource& symbolic,
                                                    const PlannedAllocationRef& physical,
                                                    const PlannedAllocation& allocation,
                                                    const PhysicalGraphPlanStorage& prefix) noexcept {
            if (allocation.purpose != PlannedAllocationPurpose::DispatchInputAssembly || allocation.version != 0 ||
                !physical.readyAfter.Valid() || physical.readyAfter.index >= prefix.segments.size()) {
                return false;
            }
            const auto* assembly = std::get_if<PlannedAssembly>(&prefix.segments[physical.readyAfter.index]);
            if (assembly == nullptr || assembly->logicalNode != symbolic.replica.materializer ||
                assembly->destination != physical.allocation) {
                return false;
            }

            const ByteCoverageView coverage = symbolic.Coverage();
            const PlannedAssemblyPart* matching = nullptr;
            for (const PlannedAssemblyPart& part : assembly->parts) {
                if (part.Coverage() != coverage) {
                    continue;
                }
                if (matching != nullptr || part.source || !part.transferProducer.Valid() ||
                    part.transferProducer.index >= prefix.segments.size()) {
                    return false;
                }
                matching = std::addressof(part);
            }
            if (matching == nullptr ||
                std::ranges::find(prefix.edges, PhysicalPlanEdge{matching->transferProducer, physical.readyAfter}) ==
                    prefix.edges.end()) {
                return false;
            }
            const auto* transfer = std::get_if<PlannedTransfer>(&prefix.segments[matching->transferProducer.index]);
            return transfer != nullptr && transfer->logicalNode == symbolic.replica.materializer &&
                   transfer->buffer == symbolic.buffer && transfer->version == symbolic.version &&
                   transfer->Coverage() == coverage && transfer->destination == physical.allocation &&
                   transfer->target == PlannedTransferTarget::AssemblyPart;
        }

        [[nodiscard]] std::expected<ResolvedRouteSource, PhysicalGraphPlanningError>
        ResolveRouteSource(Buffer buffer, GraphResidencyRoute& route, const PhysicalGraphPlanStorage& prefix,
                           const GraphLocalStorageMap& graphLocal) noexcept {
            if (!buffer || route.buffer != buffer.Describe().id || !route.Valid() ||
                prefix.allocations.size() > UINT32_MAX || prefix.segments.size() > UINT32_MAX) {
                return std::unexpected(prefix.allocations.size() > UINT32_MAX || prefix.segments.size() > UINT32_MAX
                                           ? PhysicalGraphPlanningError::CapacityExceeded
                                           : PhysicalGraphPlanningError::InvalidRoute);
            }
            if (auto* ready = std::get_if<ResidentReplica>(&route.source)) {
                const BackendServiceId service = ready->allocation.View().service;
                const MemoryLocation location = ready->allocation.Location();
                return ResolvedRouteSource{.storage = ReadyStorageRef{route.buffer, std::move(*ready)},
                                           .service = service,
                                           .location = location};
            }

            const auto& symbolic = std::get<GraphLocalResidencySource>(route.source);
            const PlannedAllocationRef* physical = graphLocal.Find(symbolic);
            if (physical == nullptr) {
                return std::unexpected(PhysicalGraphPlanningError::MissingGraphLocalBinding);
            }
            if (!physical->allocation.Valid() || physical->allocation.index >= prefix.allocations.size() ||
                (physical->readyAfter.Valid() && physical->readyAfter.index >= prefix.segments.size())) {
                return std::unexpected(PhysicalGraphPlanningError::InvalidGraphLocalBinding);
            }
            const PlannedAllocation& allocation = prefix.allocations[physical->allocation.index];
            const bool logicalValue =
                allocation.purpose == PlannedAllocationPurpose::LogicalValue && allocation.version == symbolic.version;
            const bool assemblyPart = ValidAssemblyPartBinding(symbolic, *physical, allocation, prefix);
            if (allocation.id != physical->allocation || allocation.BufferIdentity() != symbolic.buffer ||
                (!logicalValue && !assemblyPart) ||
                allocation.Coverage().Contains(symbolic.Coverage()) != CoverageContainment::Yes ||
                allocation.service != symbolic.replica.service || allocation.location != symbolic.replica.location) {
                return std::unexpected(PhysicalGraphPlanningError::InvalidGraphLocalBinding);
            }
            return ResolvedRouteSource{
                .storage = *physical, .service = symbolic.replica.service, .location = symbolic.replica.location};
        }

    } // namespace

    std::expected<void, PhysicalGraphPlanningError>
    GraphLocalStorageMap::Bind(GraphLocalStorageBinding binding) noexcept {
        if (!binding.symbolic.Covers(binding.symbolic.Coverage()) || !binding.physical.allocation.Valid()) {
            return std::unexpected(PhysicalGraphPlanningError::InvalidGraphLocalBinding);
        }
        const auto key = GraphLocalStorageKey(binding);
        const auto found = std::ranges::lower_bound(bindings_, key, {}, kGraphLocalStorageProjection);
        if (found != bindings_.end() && GraphLocalStorageKey(*found) == key) {
            return std::unexpected(PhysicalGraphPlanningError::DuplicateGraphLocalBinding);
        }
        if (bindings_.size() == bindings_.max_size()) {
            return std::unexpected(PhysicalGraphPlanningError::CapacityExceeded);
        }
        try {
            bindings_.insert(found, std::move(binding));
            return {};
        } catch (const std::bad_alloc&) {
            return std::unexpected(PhysicalGraphPlanningError::OutOfMemory);
        }
    }

    const PlannedAllocationRef* GraphLocalStorageMap::Find(const GraphLocalResidencySource& source) const noexcept {
        const auto key = GraphLocalStorageKey(source);
        const auto found = std::ranges::lower_bound(bindings_, key, {}, kGraphLocalStorageProjection);
        return found != bindings_.end() && GraphLocalStorageKey(*found) == key ? std::addressof(found->physical)
                                                                               : nullptr;
    }

    std::expected<ResidencyPlanFragment, PhysicalGraphPlanningError>
    PlanPhysicalResidency(Buffer buffer, GraphNodeId logicalConsumer, ResidencyRoute route,
                          ResidencyPlanOptions options) noexcept {
        if (!buffer || !logicalConsumer.Valid() || route.buffer != buffer.Describe().id || !route.source.allocation ||
            !route.destinationService.Valid() || !route.destination.Valid() || route.region.Size() == 0 ||
            route.version == 0 || !route.source.Covers(route.version, route.Coverage()) ||
            !options.requiredProperties.Valid() || !options.preferredProperties.Valid() ||
            !std::has_single_bit(options.alignment)) {
            return std::unexpected(PhysicalGraphPlanningError::InvalidRoute);
        }
        const BackendServiceId sourceService = route.source.allocation.View().service;
        const MemoryLocation sourceLocation = route.source.allocation.Location();
        return ExpandPhysicalResidency(buffer, logicalConsumer,
                                       ReadyStorageRef{buffer.Describe().id, std::move(route.source)}, sourceService,
                                       sourceLocation, route.destinationService, route.destination, route.Coverage(),
                                       route.version, route.estimatedNanoseconds, route.hops, 0, 0, options);
    }

    std::expected<ResidencyPlanFragment, PhysicalGraphPlanningError>
    PlanPhysicalResidency(Buffer buffer, GraphNodeId logicalConsumer, GraphResidencyRoute route,
                          const PhysicalGraphPlanStorage& prefix, const GraphLocalStorageMap& graphLocal,
                          ResidencyPlanOptions options) noexcept {
        auto source = ResolveRouteSource(buffer, route, prefix, graphLocal);
        if (!source) {
            return std::unexpected(source.error());
        }
        return ExpandPhysicalResidency(buffer, logicalConsumer, std::move(source->storage), source->service,
                                       source->location, route.destinationService, route.destination, route.Coverage(),
                                       route.version, route.estimatedNanoseconds, route.hops,
                                       static_cast<uint32_t>(prefix.allocations.size()),
                                       static_cast<uint32_t>(prefix.segments.size()), options);
    }

    std::expected<ResidencyPrefillFragment, PhysicalGraphPlanningError>
    PlanPhysicalResidencyPrefill(Buffer buffer, GraphNodeId logicalConsumer, GraphResidencyRoute route,
                                 const PhysicalGraphPlanStorage& prefix, const GraphLocalStorageMap& graphLocal,
                                 PlannedAllocationId destination, ResidencyPlanOptions options) noexcept {
        if (route.hops.empty() || route.hops.back().kind == TransferKind::CoherentAlias || !destination.Valid() ||
            destination.index >= prefix.allocations.size() || options.publishDestination) {
            return std::unexpected(PhysicalGraphPlanningError::InvalidRoute);
        }
        const PlannedAllocation& target = prefix.allocations[destination.index];
        const ByteCoverageView coverage = route.Coverage();
        const TransferLink& finalHop = route.hops.back();
        if (target.id != destination || target.purpose != PlannedAllocationPurpose::DispatchInputAssembly ||
            target.version != 0 || target.publication ||
            !std::holds_alternative<PlannedAllocate>(target.materialization) ||
            target.BufferIdentity() != route.buffer ||
            target.Coverage().Contains(coverage) != CoverageContainment::Yes ||
            target.service != route.destinationService || target.location != route.destination ||
            finalHop.service != target.service || finalHop.destination != target.location) {
            return std::unexpected(PhysicalGraphPlanningError::InvalidRoute);
        }
        auto source = ResolveRouteSource(buffer, route, prefix, graphLocal);
        if (!source) {
            return std::unexpected(source.error());
        }
        const std::span<const TransferLink> intermediate{route.hops.data(), route.hops.size() - 1};
        auto fragment = ExpandPhysicalResidency(
            buffer, logicalConsumer, std::move(source->storage), source->service, source->location,
            finalHop.sourceService, finalHop.source, coverage, route.version, route.estimatedNanoseconds, intermediate,
            static_cast<uint32_t>(prefix.allocations.size()), static_cast<uint32_t>(prefix.segments.size()), options);
        if (!fragment) {
            return std::unexpected(fragment.error());
        }
        if (fragment->storage.segments.size() >= UINT32_MAX - prefix.segments.size()) {
            return std::unexpected(PhysicalGraphPlanningError::CapacityExceeded);
        }
        try {
            const PhysicalPlanSegmentId producer{
                static_cast<uint32_t>(prefix.segments.size() + fragment->storage.segments.size())};
            const PhysicalPlanSegmentId inheritedReadiness = StorageReadyAfter(fragment->destination);
            fragment->storage.segments.emplace_back(PlannedTransfer{.logicalNode = logicalConsumer,
                                                                    .service = finalHop.service,
                                                                    .engine = finalHop.engine,
                                                                    .buffer = route.buffer,
                                                                    .region = route.region,
                                                                    .byteFootprint = route.byteFootprint,
                                                                    .version = route.version,
                                                                    .source = std::move(fragment->destination),
                                                                    .destination = destination,
                                                                    .target = PlannedTransferTarget::AssemblyPart});
            fragment->transfers.push_back({static_cast<uint32_t>(route.hops.size() - 1), producer});
            if (inheritedReadiness.Valid()) {
                fragment->storage.edges.push_back({inheritedReadiness, producer});
            }
            return ResidencyPrefillFragment{.storage = std::move(fragment->storage),
                                            .producer = producer,
                                            .transfers = std::move(fragment->transfers),
                                            .estimatedNanoseconds = route.estimatedNanoseconds};
        } catch (const std::bad_alloc&) {
            return std::unexpected(PhysicalGraphPlanningError::OutOfMemory);
        }
    }

} // namespace Sora::Compute

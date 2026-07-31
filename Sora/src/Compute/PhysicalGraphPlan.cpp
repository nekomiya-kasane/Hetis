/**
 * @file PhysicalGraphPlan.cpp
 * @brief Pure validation for target-specific physical graph plans.
 * @ingroup Compute
 */

#include <Sora/Compute/Runtime/PhysicalGraphPlan.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <new>
#include <optional>
#include <variant>

namespace Sora::Compute {

    namespace {

        [[nodiscard]] bool ValidQueue(const PlannedQueue& queue, BackendCommandKind command,
                                      const PhysicalResourceCatalog& resources) noexcept {
            const Queue* physical = resources.FindQueue(queue.service, queue.backend, queue.device, queue.queue);
            return physical != nullptr && physical->View().capabilities.Supports(command);
        }

        [[nodiscard]] bool SegmentResourcesPresent(const PhysicalPlanSegment& segment,
                                                   const PhysicalResourceCatalog& resources) noexcept {
            return std::visit(
                [&](const auto& value) {
                    using Segment = std::remove_cvref_t<decltype(value)>;
                    if constexpr (std::same_as<Segment, PlannedDispatch>) {
                        const Queue* queue = resources.FindQueue(value.queue.service, value.queue.backend,
                                                                 value.queue.device, value.queue.queue);
                        if (queue == nullptr) {
                            return false;
                        }
                        const QueueView view = queue->View();
                        if (resources.FindCompiler(view.service, value.target.backend, value.target.compilerIdentity) ==
                            nullptr) {
                            return false;
                        }
                        if (view.dispatchArtifactKind == ArtifactKind::DeviceExecutable) {
                            return true;
                        }
                        if (!value.pipeline || !value.pipeline->Valid()) {
                            return true;
                        }
                        return resources.FindPipelineFactory(view.service, value.target.backend, value.target.device,
                                                             value.pipeline->factory) != nullptr;
                    } else if constexpr (std::same_as<Segment, PlannedCopy> || std::same_as<Segment, PlannedFill> ||
                                         std::same_as<Segment, PlannedAssembly>) {
                        return resources.FindQueue(value.queue.service, value.queue.backend, value.queue.device,
                                                   value.queue.queue) != nullptr;
                    } else {
                        return resources.FindTransferEngine(value.service, value.engine) != nullptr;
                    }
                },
                segment);
        }

        [[nodiscard]] BackendServiceId SegmentService(const PhysicalPlanSegment& segment,
                                                      const PhysicalResourceCatalog& resources) noexcept {
            return std::visit(
                [&](const auto& value) {
                    using Segment = std::remove_cvref_t<decltype(value)>;
                    if constexpr (std::same_as<Segment, PlannedTransfer>) {
                        return value.service;
                    } else {
                        return value.queue.service;
                    }
                },
                segment);
        }

        [[nodiscard]] constexpr BackendServiceId AllocationService(const PlannedAllocation& allocation) noexcept {
            return allocation.service;
        }

        [[nodiscard]] BackendServiceId ReadyStorageService(const PlannedStorageRef& storage) noexcept {
            const auto* ready = std::get_if<ReadyStorageRef>(&storage);
            return ready != nullptr ? ready->replica.allocation.View().service : BackendServiceId{};
        }

        [[nodiscard]] constexpr bool PhysicalPlanEdgeLess(const PhysicalPlanEdge& left,
                                                          const PhysicalPlanEdge& right) noexcept {
            if (left.successor != right.successor) {
                return left.successor < right.successor;
            }
            return left.predecessor < right.predecessor;
        }

        [[nodiscard]] bool HasDirectDependency(std::span<const PhysicalPlanEdge> edges,
                                               PhysicalPlanSegmentId predecessor,
                                               PhysicalPlanSegmentId successor) noexcept {
            const PhysicalPlanEdge key{predecessor, successor};
            const auto found = std::ranges::lower_bound(edges, key, PhysicalPlanEdgeLess);
            return found != edges.end() && *found == key;
        }

        [[nodiscard]] const PlannedAllocation* FindAllocation(std::span<const PlannedAllocation> allocations,
                                                              PlannedAllocationId id) noexcept {
            return id.Valid() && id.index < allocations.size() ? &allocations[id.index] : nullptr;
        }

        [[nodiscard]] bool ValidReadyStorage(const ReadyStorageRef& storage, BufferId buffer, ByteCoverageView coverage,
                                             uint64_t version, BackendServiceId service,
                                             MemoryLocation location) noexcept {
            return storage.buffer == buffer && storage.replica.Covers(version, coverage) &&
                   storage.replica.allocation.View().service == service &&
                   storage.replica.allocation.Location() == location;
        }

        [[nodiscard]] bool ValidReadyStorageOnDevice(const ReadyStorageRef& storage, BufferId buffer,
                                                     ByteCoverageView coverage, BackendServiceId service,
                                                     BackendKind backend, DeviceId device) noexcept {
            const MemoryLocation location = storage.replica.allocation.Location();
            return storage.buffer == buffer && storage.replica.version != 0 &&
                   storage.replica.Covers(storage.replica.version, coverage) &&
                   storage.replica.allocation.View().service == service && location.backend == backend &&
                   location.device == device;
        }

        [[nodiscard]] bool ReadyBefore(const PlannedAllocationRef& storage, PhysicalPlanSegmentId consumer,
                                       std::span<const PhysicalPlanEdge> edges) noexcept {
            return !storage.readyAfter.Valid() ||
                   (storage.readyAfter < consumer && HasDirectDependency(edges, storage.readyAfter, consumer));
        }

        [[nodiscard]] bool ValidInputStorage(const PlannedStorageRef& storage, BufferId buffer,
                                             ByteCoverageView coverage, uint64_t version, BackendServiceId service,
                                             MemoryLocation location, PhysicalPlanSegmentId consumer,
                                             std::span<const PlannedAllocation> allocations,
                                             std::span<const PhysicalPlanEdge> edges) noexcept {
            if (const auto* ready = std::get_if<ReadyStorageRef>(&storage)) {
                return ValidReadyStorage(*ready, buffer, coverage, version, service, location);
            }
            const PlannedAllocationRef produced = std::get<PlannedAllocationRef>(storage);
            const PlannedAllocation* allocation = FindAllocation(allocations, produced.allocation);
            return allocation != nullptr && allocation->BufferIdentity() == buffer && allocation->version == version &&
                   allocation->Coverage().Contains(coverage) == CoverageContainment::Yes &&
                   allocation->service == service && allocation->location == location &&
                   ReadyBefore(produced, consumer, edges);
        }

        [[nodiscard]] bool ValidInputStorageOnDevice(const PlannedStorageRef& storage, BufferId buffer,
                                                     ByteCoverageView coverage, BackendServiceId service,
                                                     BackendKind backend, DeviceId device,
                                                     PhysicalPlanSegmentId consumer,
                                                     std::span<const PlannedAllocation> allocations,
                                                     std::span<const PhysicalPlanEdge> edges) noexcept {
            if (const auto* ready = std::get_if<ReadyStorageRef>(&storage)) {
                return ValidReadyStorageOnDevice(*ready, buffer, coverage, service, backend, device);
            }
            const PlannedAllocationRef produced = std::get<PlannedAllocationRef>(storage);
            const PlannedAllocation* allocation = FindAllocation(allocations, produced.allocation);
            const bool validValue =
                allocation != nullptr &&
                ((allocation->purpose == PlannedAllocationPurpose::LogicalValue && allocation->version != 0) ||
                 (allocation->purpose == PlannedAllocationPurpose::DispatchInputAssembly && allocation->version == 0));
            return validValue && allocation->BufferIdentity() == buffer &&
                   allocation->Coverage().Contains(coverage) == CoverageContainment::Yes &&
                   allocation->service == service && allocation->location.backend == backend &&
                   allocation->location.device == device && ReadyBefore(produced, consumer, edges);
        }

        [[nodiscard]] bool ValidOutputStorageOnDevice(PlannedAllocationId id, BufferId buffer,
                                                      ByteCoverageView coverage, BackendServiceId service,
                                                      BackendKind backend, DeviceId device,
                                                      std::span<const PlannedAllocation> allocations) noexcept {
            const PlannedAllocation* allocation = FindAllocation(allocations, id);
            return allocation != nullptr && std::holds_alternative<PlannedAllocate>(allocation->materialization) &&
                   allocation->purpose == PlannedAllocationPurpose::LogicalValue &&
                   allocation->BufferIdentity() == buffer && allocation->version != 0 &&
                   allocation->Coverage().Contains(coverage) == CoverageContainment::Yes &&
                   allocation->service == service && allocation->location.backend == backend &&
                   allocation->location.device == device;
        }

        [[nodiscard]] size_t BufferArgumentCount(const KernelInvocation& invocation) noexcept {
            size_t count = 0;
            for (const KernelArgumentDesc& argument : invocation.Signature()) {
                count += argument.kind == ParameterKind::Buffer;
            }
            return count;
        }

        [[nodiscard]] bool ValidDispatch(const PlannedDispatch& dispatch, PhysicalPlanSegmentId segment,
                                         std::span<const PlannedAllocation> allocations,
                                         std::span<const PhysicalPlanEdge> edges,
                                         const PhysicalResourceCatalog& resources) noexcept {
            const Queue* queue = resources.FindQueue(dispatch.queue.service, dispatch.queue.backend,
                                                     dispatch.queue.device, dispatch.queue.queue);
            if (queue == nullptr) {
                return false;
            }
            const QueueView queueView = queue->View();
            const BackendCompiler* compiler =
                resources.FindCompiler(queueView.service, dispatch.target.backend, dispatch.target.compilerIdentity);
            const bool validArtifactContract =
                queueView.dispatchArtifactKind == ArtifactKind::DeviceExecutable
                    ? !dispatch.pipeline
                    : dispatch.pipeline && dispatch.pipeline->Valid() &&
                          resources.FindPipelineFactory(queueView.service, dispatch.target.backend,
                                                        dispatch.target.device, dispatch.pipeline->factory) != nullptr;
            const auto expectedGeometry = PlanDispatchSliceGeometry(dispatch.slice);
            if (!dispatch.logicalNode.Valid() || !dispatch.program ||
                dispatch.program.Hash() != dispatch.invocation.ProgramHash() ||
                dispatch.invocation.Domain().workItemCount == 0 || dispatch.target.device != dispatch.queue.device ||
                dispatch.target.backend != dispatch.queue.backend || compiler == nullptr ||
                compiler->ExecutableDomain() != dispatch.target.executableDomain ||
                !ValidQueue(dispatch.queue, BackendCommandKind::Dispatch, resources) || !validArtifactContract ||
                !dispatch.slice.ValidWithin(dispatch.invocation.Domain()) || !expectedGeometry ||
                dispatch.geometry != *expectedGeometry ||
                dispatch.bufferStorage.size() != BufferArgumentCount(dispatch.invocation)) {
                return false;
            }

            size_t bufferIndex = 0;
            const auto signature = dispatch.invocation.Signature();
            const auto bindings = dispatch.invocation.Bindings();
            if (signature.size() != bindings.size()) {
                return false;
            }
            for (size_t argumentIndex = 0; argumentIndex < signature.size(); ++argumentIndex) {
                const KernelArgumentDesc& argument = signature[argumentIndex];
                if (argument.kind != ParameterKind::Buffer) {
                    continue;
                }
                const auto* view = std::get_if<BufferView>(&bindings[argumentIndex]);
                if (view == nullptr || !*view) {
                    return false;
                }
                const std::span<const uint64_t> origin{dispatch.slice.origin.data(), dispatch.slice.rank};
                const std::span<const uint64_t> extents{dispatch.slice.extents.data(), dispatch.slice.rank};
                const auto shardView = view->SubView(origin, extents);
                if (!shardView) {
                    return false;
                }
                const AffineByteFootprint footprint = shardView->Footprint();
                const ByteCoverageView coverage = ByteCoverageView::Affine(footprint);
                const PlannedDispatchBuffer& storage = dispatch.bufferStorage[bufferIndex++];
                const BufferId buffer = view->BufferDescriptor().id;
                const bool validInput =
                    storage.input && ValidInputStorageOnDevice(*storage.input, buffer, coverage, dispatch.queue.service,
                                                               dispatch.target.backend, dispatch.target.device, segment,
                                                               allocations, edges);
                const bool validOutput =
                    storage.output.Valid() &&
                    ValidOutputStorageOnDevice(storage.output, buffer, coverage, dispatch.queue.service,
                                               dispatch.target.backend, dispatch.target.device, allocations);
                bool validStorage = false;
                switch (argument.access) {
                    case AccessMode::Read:
                        validStorage = validInput && !storage.output.Valid();
                        break;
                    case AccessMode::Write:
                        validStorage = !storage.input && validOutput;
                        break;
                    case AccessMode::ReadWrite: {
                        const auto* input =
                            storage.input ? std::get_if<PlannedAllocationRef>(&*storage.input) : nullptr;
                        validStorage =
                            validInput && validOutput && input != nullptr && input->allocation == storage.output;
                        break;
                    }
                }
                if (!validStorage) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool ValidCopy(const PlannedCopy& copy, PhysicalPlanSegmentId segment,
                                     std::span<const PlannedAllocation> allocations,
                                     std::span<const PhysicalPlanEdge> edges,
                                     const PhysicalResourceCatalog& resources) noexcept {
            const BufferSlice& source = copy.operation.Source();
            const BufferSlice& destination = copy.operation.Destination();
            return copy.logicalNode.Valid() && ValidQueue(copy.queue, BackendCommandKind::Copy, resources) &&
                   ValidInputStorageOnDevice(copy.source, source.BufferDescriptor().id,
                                             ByteCoverageView::Interval(source.Region()), copy.queue.service,
                                             copy.queue.backend, copy.queue.device, segment, allocations, edges) &&
                   ValidOutputStorageOnDevice(copy.destination, destination.BufferDescriptor().id,
                                              ByteCoverageView::Interval(destination.Region()), copy.queue.service,
                                              copy.queue.backend, copy.queue.device, allocations);
        }

        [[nodiscard]] bool ValidFill(const PlannedFill& fill, PhysicalPlanSegmentId segment,
                                     std::span<const PlannedAllocation> allocations,
                                     const PhysicalResourceCatalog& resources) noexcept {
            const BufferSlice& destination = fill.operation.Destination();
            return fill.logicalNode.Valid() && ValidQueue(fill.queue, BackendCommandKind::Fill, resources) &&
                   ValidOutputStorageOnDevice(fill.destination, destination.BufferDescriptor().id,
                                              ByteCoverageView::Interval(destination.Region()), fill.queue.service,
                                              fill.queue.backend, fill.queue.device, allocations);
        }

        [[nodiscard]] std::expected<void, PhysicalGraphPlanErrorCode>
        ValidateAssembly(const PlannedAssembly& assembly, PhysicalPlanSegmentId segment,
                         std::span<const PlannedAllocation> allocations, std::span<const PhysicalPlanEdge> edges,
                         std::span<const PhysicalPlanSegment> segments,
                         const PhysicalResourceCatalog& resources) noexcept {
            constexpr size_t coverageCapacity = 256;
            const PlannedAllocation* destination = FindAllocation(allocations, assembly.destination);
            const Queue* queue = resources.FindQueue(assembly.queue.service, assembly.queue.backend,
                                                     assembly.queue.device, assembly.queue.queue);
            const bool requiresCopy = std::ranges::any_of(
                assembly.parts, [](const PlannedAssemblyPart& part) { return part.source.has_value(); });
            if (!assembly.logicalNode.Valid() || queue == nullptr ||
                (requiresCopy && !queue->View().capabilities.Supports(BackendCommandKind::Copy)) ||
                destination == nullptr || destination->purpose != PlannedAllocationPurpose::DispatchInputAssembly ||
                destination->version != 0 || destination->publication ||
                !std::holds_alternative<PlannedAllocate>(destination->materialization) || assembly.parts.empty() ||
                destination->service != assembly.queue.service ||
                destination->location.backend != assembly.queue.backend ||
                destination->location.device != assembly.queue.device) {
                return std::unexpected(PhysicalGraphPlanErrorCode::InvalidSegment);
            }
            if (assembly.parts.size() > coverageCapacity) {
                return std::unexpected(PhysicalGraphPlanErrorCode::CapacityExceeded);
            }

            std::array<ByteCoverageView, coverageCapacity> coverages{};
            size_t count = 0;
            bool boundingRegionsDisjoint = true;
            for (const PlannedAssemblyPart& part : assembly.parts) {
                const ByteCoverageView coverage = part.Coverage();
                const bool localCopy = part.source.has_value() && !part.transferProducer.Valid();
                const bool prefilled = !part.source && part.transferProducer.Valid();
                if (!coverage.Valid() || coverage.BoundingRegion() != part.region ||
                    destination->Coverage().Contains(coverage) != CoverageContainment::Yes ||
                    (!localCopy && !prefilled)) {
                    return std::unexpected(PhysicalGraphPlanErrorCode::InvalidSegment);
                }
                if (localCopy && !ValidInputStorageOnDevice(*part.source, destination->BufferIdentity(), coverage,
                                                            assembly.queue.service, assembly.queue.backend,
                                                            assembly.queue.device, segment, allocations, edges)) {
                    return std::unexpected(PhysicalGraphPlanErrorCode::InvalidSegment);
                }
                if (prefilled) {
                    if (part.transferProducer >= segment || part.transferProducer.index >= segments.size() ||
                        !HasDirectDependency(edges, part.transferProducer, segment)) {
                        return std::unexpected(PhysicalGraphPlanErrorCode::InvalidSegment);
                    }
                    const auto* transfer = std::get_if<PlannedTransfer>(&segments[part.transferProducer.index]);
                    if (transfer == nullptr || transfer->target != PlannedTransferTarget::AssemblyPart ||
                        transfer->destination != assembly.destination || transfer->Coverage() != coverage) {
                        return std::unexpected(PhysicalGraphPlanErrorCode::InvalidSegment);
                    }
                }
                for (size_t prior = 0; prior < count; ++prior) {
                    boundingRegionsDisjoint = boundingRegionsDisjoint &&
                                              !coverages[prior].BoundingRegion().Intersects(coverage.BoundingRegion());
                }
                coverages[count++] = coverage;
            }
            const CoverageEquivalence exact =
                ExactCoverageUnion<coverageCapacity>(destination->Coverage(), {coverages.data(), count});
            if (exact != CoverageEquivalence::Equal) {
                return std::unexpected(exact == CoverageEquivalence::Unknown
                                           ? PhysicalGraphPlanErrorCode::CapacityExceeded
                                           : PhysicalGraphPlanErrorCode::InvalidSegment);
            }
            if (boundingRegionsDisjoint) {
                return {};
            }
            size_t commandCount = 0;
            for (const ByteCoverageView coverage : std::span{coverages.data(), count}) {
                const auto regions = coverage.ProjectCanonicalRegions<coverageCapacity>();
                if (!regions) {
                    return std::unexpected(regions.error() == AffineByteFootprintError::CapacityExceeded
                                               ? PhysicalGraphPlanErrorCode::CapacityExceeded
                                               : PhysicalGraphPlanErrorCode::InvalidSegment);
                }
                if (regions->Size() > coverageCapacity - commandCount) {
                    return std::unexpected(PhysicalGraphPlanErrorCode::CapacityExceeded);
                }
                commandCount += regions->Size();
            }
            return {};
        }

        [[nodiscard]] std::expected<void, PhysicalGraphPlanErrorCode>
        ValidateTransfer(const PlannedTransfer& transfer, PhysicalPlanSegmentId segment,
                         std::span<const PlannedAllocation> allocations, std::span<const PhysicalPlanEdge> edges,
                         const PhysicalResourceCatalog& resources) noexcept {
            constexpr size_t regionCapacity = 256;
            const TransferEngine* engine = resources.FindTransferEngine(transfer.service, transfer.engine);
            const PlannedAllocation* destination = FindAllocation(allocations, transfer.destination);
            const auto* materialization =
                destination != nullptr ? std::get_if<PlannedAllocate>(&destination->materialization) : nullptr;
            const ByteCoverageView coverage = transfer.Coverage();
            if (!transfer.logicalNode.Valid() || engine == nullptr || engine->Kind() == TransferKind::CoherentAlias ||
                destination == nullptr || materialization == nullptr || !coverage.Valid() ||
                coverage.BoundingRegion() != transfer.region || transfer.buffer != destination->BufferIdentity() ||
                transfer.version == 0 || destination->service != transfer.service ||
                destination->location != engine->Destination() ||
                !ValidInputStorage(transfer.source, transfer.buffer, coverage, transfer.version,
                                   engine->SourceService(), engine->Source(), segment, allocations, edges)) {
                return std::unexpected(PhysicalGraphPlanErrorCode::InvalidSegment);
            }
            const bool complete = transfer.target == PlannedTransferTarget::CompleteAllocation &&
                                  destination->purpose == PlannedAllocationPurpose::LogicalValue &&
                                  transfer.version == destination->version && coverage == destination->Coverage();
            const bool assemblyPart = transfer.target == PlannedTransferTarget::AssemblyPart &&
                                      destination->purpose == PlannedAllocationPurpose::DispatchInputAssembly &&
                                      destination->version == 0 && !destination->publication &&
                                      destination->Coverage().Contains(coverage) == CoverageContainment::Yes;
            if (!complete && !assemblyPart) {
                return std::unexpected(PhysicalGraphPlanErrorCode::InvalidSegment);
            }
            const auto regions = coverage.ProjectCanonicalRegions<regionCapacity>();
            if (!regions) {
                return std::unexpected(regions.error() == AffineByteFootprintError::CapacityExceeded
                                           ? PhysicalGraphPlanErrorCode::CapacityExceeded
                                           : PhysicalGraphPlanErrorCode::InvalidSegment);
            }
            return {};
        }

        [[nodiscard]] std::optional<PhysicalPlanSegmentId>
        MissingDependency(const PlannedStorageRef& storage, PhysicalPlanSegmentId consumer,
                          std::span<const PhysicalPlanEdge> edges) noexcept {
            const auto* produced = std::get_if<PlannedAllocationRef>(&storage);
            if (produced != nullptr && produced->readyAfter.Valid() && produced->readyAfter < consumer &&
                !HasDirectDependency(edges, produced->readyAfter, consumer)) {
                return produced->readyAfter;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<PhysicalPlanSegmentId>
        MissingDataProducer(const PhysicalPlanSegment& segment, PhysicalPlanSegmentId consumer,
                            std::span<const PhysicalPlanEdge> edges) noexcept {
            return std::visit(
                [&](const auto& value) -> std::optional<PhysicalPlanSegmentId> {
                    using Segment = std::remove_cvref_t<decltype(value)>;
                    if constexpr (std::same_as<Segment, PlannedDispatch>) {
                        size_t bufferIndex = 0;
                        for (const KernelArgumentDesc& argument : value.invocation.Signature()) {
                            if (argument.kind != ParameterKind::Buffer) {
                                continue;
                            }
                            const PlannedDispatchBuffer& storage = value.bufferStorage[bufferIndex++];
                            if (storage.input) {
                                if (auto missing = MissingDependency(*storage.input, consumer, edges)) {
                                    return missing;
                                }
                            }
                        }
                    } else if constexpr (std::same_as<Segment, PlannedAssembly>) {
                        for (const PlannedAssemblyPart& part : value.parts) {
                            if (part.source) {
                                if (auto missing = MissingDependency(*part.source, consumer, edges)) {
                                    return missing;
                                }
                            } else if (part.transferProducer.Valid() &&
                                       !HasDirectDependency(edges, part.transferProducer, consumer)) {
                                return part.transferProducer;
                            }
                        }
                    } else if constexpr (std::same_as<Segment, PlannedCopy> || std::same_as<Segment, PlannedTransfer>) {
                        return MissingDependency(value.source, consumer, edges);
                    }
                    return std::nullopt;
                },
                segment);
        }

        [[nodiscard]] bool SegmentProduces(const PhysicalPlanSegment& segment,
                                           PlannedAllocationId allocation) noexcept {
            return std::visit(
                [allocation](const auto& value) {
                    using Segment = std::remove_cvref_t<decltype(value)>;
                    if constexpr (std::same_as<Segment, PlannedDispatch>) {
                        size_t bufferIndex = 0;
                        for (const KernelArgumentDesc& argument : value.invocation.Signature()) {
                            if (argument.kind != ParameterKind::Buffer) {
                                continue;
                            }
                            const PlannedDispatchBuffer& storage = value.bufferStorage[bufferIndex++];
                            if ((argument.access == AccessMode::Write || argument.access == AccessMode::ReadWrite) &&
                                storage.output == allocation) {
                                return true;
                            }
                        }
                        return false;
                    } else if constexpr (std::same_as<Segment, PlannedTransfer>) {
                        return value.target == PlannedTransferTarget::CompleteAllocation &&
                               value.destination == allocation;
                    } else {
                        return value.destination == allocation;
                    }
                },
                segment);
        }

        [[nodiscard]] bool ValidAliasMaterialization(const PlannedAllocation& allocation, size_t allocationIndex,
                                                     std::span<const PlannedAllocation> allocations,
                                                     const PhysicalResourceCatalog& resources) noexcept {
            const auto* alias = std::get_if<PlannedCoherentAlias>(&allocation.materialization);
            if (alias == nullptr) {
                return false;
            }
            const TransferEngine* engine = resources.FindTransferEngine(allocation.service, alias->engine);
            if (engine == nullptr || engine->Kind() != TransferKind::CoherentAlias ||
                engine->Destination() != allocation.location) {
                return false;
            }
            if (const auto* ready = std::get_if<ReadyStorageRef>(&alias->source)) {
                return ValidReadyStorage(*ready, allocation.BufferIdentity(), allocation.Coverage(), allocation.version,
                                         engine->SourceService(), engine->Source());
            }
            const PlannedAllocationRef sourceState = std::get<PlannedAllocationRef>(alias->source);
            const PlannedAllocation* source = FindAllocation(allocations, sourceState.allocation);
            return source != nullptr && sourceState.allocation.index < allocationIndex &&
                   source->BufferIdentity() == allocation.BufferIdentity() && source->version == allocation.version &&
                   source->Coverage().Contains(allocation.Coverage()) == CoverageContainment::Yes &&
                   source->service == engine->SourceService() && source->location == engine->Source();
        }

        [[nodiscard]] PhysicalPlanSegmentId StorageReadyAfter(const PlannedStorageRef& storage) noexcept {
            const auto* produced = std::get_if<PlannedAllocationRef>(&storage);
            return produced != nullptr ? produced->readyAfter : PhysicalPlanSegmentId{};
        }

        [[nodiscard]] bool ValidStorageState(const PlannedStorageRef& storage,
                                             std::span<const PlannedAllocation> allocations,
                                             std::span<const PhysicalPlanSegment> segments) noexcept {
            if (const auto* ready = std::get_if<ReadyStorageRef>(&storage)) {
                return ready->buffer.Valid() && static_cast<bool>(ready->replica.allocation);
            }
            const PlannedAllocationRef produced = std::get<PlannedAllocationRef>(storage);
            const PlannedAllocation* allocation = FindAllocation(allocations, produced.allocation);
            if (allocation == nullptr) {
                return false;
            }
            if (const auto* alias = std::get_if<PlannedCoherentAlias>(&allocation->materialization)) {
                return produced.readyAfter == StorageReadyAfter(alias->source) &&
                       ValidStorageState(alias->source, allocations, segments);
            }
            return produced.readyAfter.Valid() && produced.readyAfter.index < segments.size() &&
                   SegmentProduces(segments[produced.readyAfter.index], produced.allocation);
        }

        [[nodiscard]] bool ValidSegmentStorageStates(const PhysicalPlanSegment& segment,
                                                     std::span<const PlannedAllocation> allocations,
                                                     std::span<const PhysicalPlanSegment> segments) noexcept {
            return std::visit(
                [&](const auto& value) {
                    using Segment = std::remove_cvref_t<decltype(value)>;
                    if constexpr (std::same_as<Segment, PlannedDispatch>) {
                        for (const PlannedDispatchBuffer& storage : value.bufferStorage) {
                            if (storage.input && !ValidStorageState(*storage.input, allocations, segments)) {
                                return false;
                            }
                        }
                        return true;
                    } else if constexpr (std::same_as<Segment, PlannedAssembly>) {
                        return std::ranges::all_of(value.parts, [&](const PlannedAssemblyPart& part) {
                            return part.source
                                       ? ValidStorageState(*part.source, allocations, segments)
                                       : part.transferProducer.Valid() && part.transferProducer.index < segments.size();
                        });
                    } else if constexpr (std::same_as<Segment, PlannedCopy> || std::same_as<Segment, PlannedTransfer>) {
                        return ValidStorageState(value.source, allocations, segments);
                    } else {
                        return true;
                    }
                },
                segment);
        }

        void MarkSegmentOutputs(const PhysicalPlanSegment& segment, std::span<uint8_t> produced) noexcept {
            std::visit(
                [&](const auto& value) {
                    using Segment = std::remove_cvref_t<decltype(value)>;
                    if constexpr (std::same_as<Segment, PlannedDispatch>) {
                        size_t bufferIndex = 0;
                        for (const KernelArgumentDesc& argument : value.invocation.Signature()) {
                            if (argument.kind != ParameterKind::Buffer) {
                                continue;
                            }
                            const PlannedDispatchBuffer& storage = value.bufferStorage[bufferIndex++];
                            if ((argument.access == AccessMode::Write || argument.access == AccessMode::ReadWrite) &&
                                storage.output.Valid() && storage.output.index < produced.size()) {
                                produced[storage.output.index] = 1;
                            }
                        }
                    } else if constexpr (std::same_as<Segment, PlannedTransfer>) {
                        if (value.target == PlannedTransferTarget::CompleteAllocation && value.destination.Valid() &&
                            value.destination.index < produced.size()) {
                            produced[value.destination.index] = 1;
                        }
                    } else if (value.destination.Valid() && value.destination.index < produced.size()) {
                        produced[value.destination.index] = 1;
                    }
                },
                segment);
        }

        [[nodiscard]] bool ValidPublication(const PlannedAllocation& allocation,
                                            std::span<const PlannedAllocation> allocations,
                                            std::span<const PhysicalPlanSegment> segments) noexcept {
            if (!allocation.publication) {
                return true;
            }
            const PlannedPublication publication = *allocation.publication;
            if (!ValidStorageState(PlannedAllocationRef{allocation.id, publication.producer}, allocations, segments)) {
                return false;
            }
            const BufferDesc desc = allocation.buffer.Describe();
            const auto* allocate = std::get_if<PlannedAllocate>(&allocation.materialization);
            switch (publication.kind) {
                case PlannedPublicationKind::Replica:
                    return publication.expectedVersion == allocation.version &&
                           publication.partitionIndex == UINT32_MAX && publication.partitionCount == 0;
                case PlannedPublicationKind::WholeWrite: {
                    const std::array part{allocation.Coverage()};
                    return publication.expectedVersion != UINT64_MAX &&
                           allocation.version == publication.expectedVersion + 1 &&
                           ExactCoverageUnion<256>(allocation.buffer.LogicalCoverage(), part) ==
                               CoverageEquivalence::Equal &&
                           allocate != nullptr && allocate->alignment >= desc.alignment &&
                           publication.partitionIndex == UINT32_MAX && publication.partitionCount == 0;
                }
                case PlannedPublicationKind::PartitionedWrite:
                    return publication.expectedVersion != UINT64_MAX &&
                           allocation.version == publication.expectedVersion + 1 &&
                           allocation.buffer.LogicalCoverage().Contains(allocation.Coverage()) ==
                               CoverageContainment::Yes &&
                           allocate != nullptr && allocate->alignment >= desc.alignment &&
                           publication.partitionCount > 1 && publication.partitionIndex < publication.partitionCount;
            }
            return false;
        }

        struct PublicationRecord {
            BufferId buffer{};
            PlannedPublicationKind kind = PlannedPublicationKind::Replica;
            uint32_t allocation = kInvalidPhysicalPlanIndex;
        };

        [[nodiscard]] constexpr bool PublicationRecordLess(const PublicationRecord& left,
                                                           const PublicationRecord& right) noexcept {
            if (left.buffer != right.buffer) {
                return left.buffer < right.buffer;
            }
            return left.allocation < right.allocation;
        }

        [[nodiscard]] std::expected<std::vector<uint32_t>, PhysicalGraphPlanError>
        PublicationConflicts(std::span<const PlannedAllocation> allocations) noexcept {
            std::vector<PublicationRecord> records;
            std::vector<uint32_t> related;
            try {
                records.reserve(allocations.size());
                related.resize(allocations.size(), kInvalidPhysicalPlanIndex);
                for (size_t index = 0; index < allocations.size(); ++index) {
                    const PlannedAllocation& allocation = allocations[index];
                    if (allocation.publication) {
                        records.push_back(
                            {allocation.BufferIdentity(), allocation.publication->kind, static_cast<uint32_t>(index)});
                    }
                }
            } catch (const std::bad_alloc&) {
                return std::unexpected(PhysicalGraphPlanError{PhysicalGraphPlanErrorCode::OutOfMemory});
            }

            std::ranges::sort(records, PublicationRecordLess);
            size_t begin = 0;
            while (begin < records.size()) {
                size_t end = begin + 1;
                while (end < records.size() && records[end].buffer == records[begin].buffer) {
                    ++end;
                }
                const uint32_t firstAllocation = records[begin].allocation;
                const bool allReplicas = std::ranges::all_of(
                    std::span{records}.subspan(begin, end - begin),
                    [](const PublicationRecord& record) { return record.kind == PlannedPublicationKind::Replica; });
                if (allReplicas) {
                    begin = end;
                    continue;
                }
                if (end - begin == 1 && records[begin].kind == PlannedPublicationKind::WholeWrite) {
                    begin = end;
                    continue;
                }
                const bool allPartitioned = std::ranges::all_of(
                    std::span{records}.subspan(begin, end - begin), [](const PublicationRecord& record) {
                        return record.kind == PlannedPublicationKind::PartitionedWrite;
                    });
                if (!allPartitioned) {
                    uint32_t firstWholeWrite = kInvalidPhysicalPlanIndex;
                    for (size_t index = begin; index < end; ++index) {
                        const PublicationRecord record = records[index];
                        if (record.kind == PlannedPublicationKind::WholeWrite) {
                            if (index != begin) {
                                related[record.allocation] = firstAllocation;
                            }
                            if (firstWholeWrite == kInvalidPhysicalPlanIndex) {
                                firstWholeWrite = record.allocation;
                            }
                        } else if (firstWholeWrite != kInvalidPhysicalPlanIndex) {
                            related[record.allocation] = firstWholeWrite;
                        }
                    }
                    begin = end;
                    continue;
                }

                const PlannedPublication firstPublication = *allocations[firstAllocation].publication;
                const uint32_t partitionCount = firstPublication.partitionCount;
                if (partitionCount == 0 || partitionCount != end - begin) {
                    related[firstAllocation] = firstAllocation;
                    begin = end;
                    continue;
                }
                std::vector<uint32_t> ordered;
                try {
                    ordered.assign(partitionCount, kInvalidPhysicalPlanIndex);
                } catch (const std::bad_alloc&) {
                    return std::unexpected(PhysicalGraphPlanError{PhysicalGraphPlanErrorCode::OutOfMemory});
                }
                bool valid = true;
                for (size_t index = begin; index < end; ++index) {
                    const uint32_t allocationIndex = records[index].allocation;
                    const PlannedPublication publication = *allocations[allocationIndex].publication;
                    if (publication.partitionCount != partitionCount ||
                        publication.expectedVersion != firstPublication.expectedVersion ||
                        publication.partitionIndex >= partitionCount ||
                        ordered[publication.partitionIndex] != kInvalidPhysicalPlanIndex) {
                        valid = false;
                        break;
                    }
                    ordered[publication.partitionIndex] = allocationIndex;
                }
                constexpr size_t coverageCapacity = 256;
                std::array<ByteCoverageView, coverageCapacity> coverages{};
                size_t coverageCount = 0;
                if (partitionCount > coverageCapacity) {
                    return std::unexpected(PhysicalGraphPlanError{PhysicalGraphPlanErrorCode::CapacityExceeded,
                                                                  firstPublication.producer.index, firstAllocation});
                }
                for (const uint32_t allocationIndex : ordered) {
                    if (allocationIndex == kInvalidPhysicalPlanIndex || coverageCount == coverageCapacity) {
                        valid = false;
                        break;
                    }
                    coverages[coverageCount++] = allocations[allocationIndex].Coverage();
                }
                if (valid) {
                    const CoverageEquivalence equivalence = ExactCoverageUnion<coverageCapacity>(
                        allocations[firstAllocation].buffer.LogicalCoverage(), {coverages.data(), coverageCount});
                    if (equivalence == CoverageEquivalence::Unknown) {
                        return std::unexpected(PhysicalGraphPlanError{PhysicalGraphPlanErrorCode::CapacityExceeded,
                                                                      firstPublication.producer.index,
                                                                      firstAllocation});
                    }
                    valid = equivalence == CoverageEquivalence::Equal;
                }
                if (!valid) {
                    related[firstAllocation] = firstAllocation;
                }
                begin = end;
            }
            return related;
        }

    } // namespace

    std::expected<PhysicalGraphPlan, PhysicalGraphPlanError>
    PhysicalGraphPlan::Create(PhysicalGraphPlanStorage storage, const PhysicalResourceCatalog& resources) noexcept {
        if (storage.segments.empty()) {
            return std::unexpected(PhysicalGraphPlanError{PhysicalGraphPlanErrorCode::EmptyPlan});
        }
        if (storage.segments.size() > UINT32_MAX || storage.allocations.size() > UINT32_MAX ||
            storage.edges.size() > UINT32_MAX) {
            return std::unexpected(PhysicalGraphPlanError{PhysicalGraphPlanErrorCode::CapacityExceeded});
        }
        for (size_t index = 0; index < storage.allocations.size(); ++index) {
            const PlannedAllocation& allocation = storage.allocations[index];
            const bool resourcePresent = std::visit(
                [&](const auto& materialization) {
                    using Materialization = std::remove_cvref_t<decltype(materialization)>;
                    if constexpr (std::same_as<Materialization, PlannedAllocate>) {
                        return resources.FindAllocator(allocation.service, allocation.location) != nullptr;
                    } else {
                        return resources.FindTransferEngine(allocation.service, materialization.engine) != nullptr;
                    }
                },
                allocation.materialization);
            if (!resourcePresent) {
                return std::unexpected(PhysicalGraphPlanError{PhysicalGraphPlanErrorCode::MissingPhysicalResource,
                                                              kInvalidPhysicalPlanIndex, static_cast<uint32_t>(index)});
            }
            const auto* allocate = std::get_if<PlannedAllocate>(&allocation.materialization);
            const bool validAllocate = allocate != nullptr && allocate->requiredProperties.Valid() &&
                                       allocate->preferredProperties.Valid() &&
                                       std::has_single_bit(allocate->alignment) &&
                                       resources.FindAllocator(allocation.service, allocation.location) != nullptr;
            const bool validAlias = ValidAliasMaterialization(allocation, index, storage.allocations, resources);
            const BufferDesc desc = allocation.buffer ? allocation.buffer.Describe() : BufferDesc{};
            const bool validMapping =
                allocation.buffer && allocation.Coverage().Valid() &&
                allocation.Coverage().BoundingRegion() == allocation.bufferRegion &&
                allocation.buffer.LogicalCoverage().Contains(allocation.Coverage()) == CoverageContainment::Yes;
            const bool validPurpose =
                (allocation.purpose == PlannedAllocationPurpose::LogicalValue && allocation.version != 0) ||
                (allocation.purpose == PlannedAllocationPurpose::DispatchInputAssembly && allocation.version == 0 &&
                 !allocation.publication && validAllocate);
            if (allocation.id != PlannedAllocationId{static_cast<uint32_t>(index)} || !validMapping ||
                !allocation.service.Valid() || !allocation.location.Valid() || allocation.bufferRegion.Size() == 0 ||
                !validPurpose || (!validAllocate && !validAlias)) {
                return std::unexpected(PhysicalGraphPlanError{PhysicalGraphPlanErrorCode::InvalidAllocation,
                                                              kInvalidPhysicalPlanIndex, static_cast<uint32_t>(index)});
            }
        }
        for (const PhysicalPlanEdge edge : storage.edges) {
            if (!edge.predecessor.Valid() || !edge.successor.Valid() ||
                edge.predecessor.index >= storage.segments.size() || edge.successor.index >= storage.segments.size() ||
                edge.predecessor >= edge.successor) {
                return std::unexpected(PhysicalGraphPlanError{PhysicalGraphPlanErrorCode::InvalidDependency,
                                                              edge.successor.index, edge.predecessor.index});
            }
        }
        std::ranges::sort(storage.edges, PhysicalPlanEdgeLess);
        for (size_t index = 1; index < storage.edges.size(); ++index) {
            const PhysicalPlanEdge edge = storage.edges[index];
            if (storage.edges[index - 1] == edge) {
                return std::unexpected(PhysicalGraphPlanError{PhysicalGraphPlanErrorCode::DuplicateDependency,
                                                              edge.successor.index, edge.predecessor.index});
            }
        }

        for (size_t index = 0; index < storage.segments.size(); ++index) {
            const uint32_t segmentIndex = static_cast<uint32_t>(index);
            if (!SegmentResourcesPresent(storage.segments[index], resources)) {
                return std::unexpected(
                    PhysicalGraphPlanError{PhysicalGraphPlanErrorCode::MissingPhysicalResource, segmentIndex});
            }
        }

        for (size_t index = 0; index < storage.segments.size(); ++index) {
            const uint32_t segmentIndex = static_cast<uint32_t>(index);
            if (auto missing =
                    MissingDataProducer(storage.segments[index], PhysicalPlanSegmentId{segmentIndex}, storage.edges)) {
                return std::unexpected(PhysicalGraphPlanError{PhysicalGraphPlanErrorCode::MissingDataDependency,
                                                              segmentIndex, missing->index});
            }
        }

        for (size_t index = 0; index < storage.segments.size(); ++index) {
            const uint32_t segmentIndex = static_cast<uint32_t>(index);
            const PhysicalPlanSegmentId id{segmentIndex};
            std::optional<PhysicalGraphPlanErrorCode> validationError;
            const bool valid = std::visit(
                [&]<typename Segment>(const Segment& segment) {
                    if constexpr (std::same_as<Segment, PlannedDispatch>) {
                        return ValidDispatch(segment, id, storage.allocations, storage.edges, resources);
                    } else if constexpr (std::same_as<Segment, PlannedCopy>) {
                        return ValidCopy(segment, id, storage.allocations, storage.edges, resources);
                    } else if constexpr (std::same_as<Segment, PlannedFill>) {
                        return ValidFill(segment, id, storage.allocations, resources);
                    } else if constexpr (std::same_as<Segment, PlannedAssembly>) {
                        auto validation = ValidateAssembly(segment, id, storage.allocations, storage.edges,
                                                           storage.segments, resources);
                        if (!validation) {
                            validationError = validation.error();
                        }
                        return validation.has_value();
                    } else {
                        auto validation = ValidateTransfer(segment, id, storage.allocations, storage.edges, resources);
                        if (!validation) {
                            validationError = validation.error();
                        }
                        return validation.has_value();
                    }
                },
                storage.segments[index]);
            if (!valid) {
                return std::unexpected(PhysicalGraphPlanError{
                    validationError.value_or(PhysicalGraphPlanErrorCode::InvalidSegment), segmentIndex});
            }
            if (!ValidSegmentStorageStates(storage.segments[index], storage.allocations, storage.segments)) {
                return std::unexpected(
                    PhysicalGraphPlanError{PhysicalGraphPlanErrorCode::InvalidDependency, segmentIndex});
            }
        }

        auto publicationConflicts = PublicationConflicts(storage.allocations);
        if (!publicationConflicts) {
            return std::unexpected(publicationConflicts.error());
        }

        std::vector<uint8_t> produced;
        try {
            produced.resize(storage.allocations.size());
        } catch (const std::bad_alloc&) {
            return std::unexpected(PhysicalGraphPlanError{PhysicalGraphPlanErrorCode::OutOfMemory});
        }
        for (const PhysicalPlanSegment& segment : storage.segments) {
            MarkSegmentOutputs(segment, produced);
        }

        for (size_t index = 0; index < storage.allocations.size(); ++index) {
            const PlannedAllocation& allocation = storage.allocations[index];
            if (std::holds_alternative<PlannedAllocate>(allocation.materialization) && produced[index] == 0) {
                return std::unexpected(PhysicalGraphPlanError{PhysicalGraphPlanErrorCode::InvalidAllocation,
                                                              kInvalidPhysicalPlanIndex, allocation.id.index});
            }
            if (const auto* alias = std::get_if<PlannedCoherentAlias>(&allocation.materialization);
                alias != nullptr && !ValidStorageState(alias->source, storage.allocations, storage.segments)) {
                return std::unexpected(PhysicalGraphPlanError{PhysicalGraphPlanErrorCode::InvalidAllocation,
                                                              StorageReadyAfter(alias->source).index,
                                                              allocation.id.index});
            }
            if (!ValidPublication(allocation, storage.allocations, storage.segments)) {
                return std::unexpected(PhysicalGraphPlanError{
                    PhysicalGraphPlanErrorCode::InvalidPublication,
                    allocation.publication ? allocation.publication->producer.index : kInvalidPhysicalPlanIndex,
                    allocation.id.index});
            }
            if ((*publicationConflicts)[index] != kInvalidPhysicalPlanIndex) {
                return std::unexpected(PhysicalGraphPlanError{PhysicalGraphPlanErrorCode::InvalidPublication,
                                                              allocation.publication->producer.index,
                                                              (*publicationConflicts)[index]});
            }
        }
        try {
            std::vector<DeviceLease> deviceLeases;
            deviceLeases.reserve(storage.allocations.size() + storage.segments.size());
            const auto capture = [&](BackendServiceId service) -> std::expected<void, PhysicalGraphPlanError> {
                if (!service.Valid()) {
                    return std::unexpected(PhysicalGraphPlanError{PhysicalGraphPlanErrorCode::MissingPhysicalResource});
                }
                if (std::ranges::any_of(deviceLeases,
                                        [service](const DeviceLease& lease) { return lease.Service() == service; })) {
                    return {};
                }
                const DeviceObserver* observer = resources.FindDeviceObserver(service);
                if (observer == nullptr) {
                    return std::unexpected(PhysicalGraphPlanError{PhysicalGraphPlanErrorCode::MissingPhysicalResource});
                }
                auto lease = DeviceLease::Capture(*observer);
                if (!lease) {
                    return std::unexpected(PhysicalGraphPlanError{PhysicalGraphPlanErrorCode::DeviceUnavailable});
                }
                deviceLeases.push_back(std::move(*lease));
                return {};
            };
            for (const PlannedAllocation& allocation : storage.allocations) {
                auto captured = capture(AllocationService(allocation));
                if (!captured) {
                    return std::unexpected(captured.error());
                }
                if (const auto* alias = std::get_if<PlannedCoherentAlias>(&allocation.materialization)) {
                    const BackendServiceId readyService = ReadyStorageService(alias->source);
                    if (readyService.Valid()) {
                        captured = capture(readyService);
                        if (!captured) {
                            return std::unexpected(captured.error());
                        }
                    }
                }
            }
            for (const PhysicalPlanSegment& segment : storage.segments) {
                auto captured = capture(SegmentService(segment, resources));
                if (!captured) {
                    return std::unexpected(captured.error());
                }
                const auto captureStorage = [&](const PlannedStorageRef& source) {
                    const BackendServiceId readyService = ReadyStorageService(source);
                    return readyService.Valid() ? capture(readyService) : std::expected<void, PhysicalGraphPlanError>{};
                };
                captured = std::visit(
                    [&](const auto& value) -> std::expected<void, PhysicalGraphPlanError> {
                        using Segment = std::remove_cvref_t<decltype(value)>;
                        if constexpr (std::same_as<Segment, PlannedDispatch>) {
                            for (const PlannedDispatchBuffer& buffer : value.bufferStorage) {
                                if (buffer.input) {
                                    auto inputCaptured = captureStorage(*buffer.input);
                                    if (!inputCaptured) {
                                        return inputCaptured;
                                    }
                                }
                            }
                            return {};
                        } else if constexpr (std::same_as<Segment, PlannedAssembly>) {
                            for (const PlannedAssemblyPart& part : value.parts) {
                                if (part.source) {
                                    auto sourceCaptured = captureStorage(*part.source);
                                    if (!sourceCaptured) {
                                        return sourceCaptured;
                                    }
                                }
                            }
                            return {};
                        } else if constexpr (std::same_as<Segment, PlannedCopy> ||
                                             std::same_as<Segment, PlannedTransfer>) {
                            return captureStorage(value.source);
                        } else {
                            return {};
                        }
                    },
                    segment);
                if (!captured) {
                    return std::unexpected(captured.error());
                }
            }
            std::ranges::sort(deviceLeases, {}, &DeviceLease::Service);
            return PhysicalGraphPlan{std::move(storage), std::move(deviceLeases)};
        } catch (const std::bad_alloc&) {
            return std::unexpected(PhysicalGraphPlanError{PhysicalGraphPlanErrorCode::OutOfMemory});
        }
    }

} // namespace Sora::Compute

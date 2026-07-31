/**
 * @file PhysicalGraphMaterializer.cpp
 * @brief Transactional physical allocation, compilation, binding, and
 * executable-graph construction.
 * @ingroup Compute
 */

#include <Sora/Compute/Runtime/PhysicalGraphMaterializer.h>

#include <Sora/Compute/Runtime/PhysicalBinding.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace Sora::Compute {

    namespace {

        using Error = PhysicalGraphMaterializationError;
        using ErrorCode = PhysicalGraphMaterializationErrorCode;

        [[nodiscard]] constexpr Error Failure(ErrorCode code, uint32_t segment = kInvalidPhysicalPlanIndex,
                                              uint32_t subject = kInvalidPhysicalPlanIndex, uint32_t cause = 0,
                                              uint64_t detail = 0) noexcept {
            return {code, segment, subject, cause, detail};
        }

        template<typename Enum>
        [[nodiscard]] constexpr uint32_t Cause(Enum value) noexcept {
            return static_cast<uint32_t>(std::to_underlying(value));
        }

        class AllocationJournal final {
        public:
            AllocationJournal() = default;
            AllocationJournal(const AllocationJournal&) = delete;
            AllocationJournal& operator=(const AllocationJournal&) = delete;

            ~AllocationJournal() noexcept {
                for (auto allocation = allocations_.rbegin(); allocation != allocations_.rend(); ++allocation) {
                    *allocation = {};
                }
            }

            void Reserve(size_t count) { allocations_.reserve(count); }
            void Append(Allocation allocation) { allocations_.push_back(std::move(allocation)); }

            [[nodiscard]] size_t Size() const noexcept { return allocations_.size(); }
            [[nodiscard]] const Allocation& operator[](size_t index) const noexcept { return allocations_[index]; }

        private:
            std::vector<Allocation> allocations_{};
        };

        struct ResolvedStorage {
            Allocation allocation{};
            uint64_t offset = 0;
        };

        [[nodiscard]] std::expected<ResolvedStorage, Error>
        ResolveStorage(const PlannedStorageRef& storage, ByteRegion requested,
                       std::span<const PlannedAllocation> planned, const AllocationJournal& materialized,
                       uint32_t segment, uint32_t subject) noexcept {
            if (const auto* ready = std::get_if<ReadyStorageRef>(&storage)) {
                const auto offset = ready->replica.ResolveAllocationOffset(requested);
                if (!offset) {
                    return std::unexpected(Failure(ErrorCode::InvalidPlan, segment, subject));
                }
                return ResolvedStorage{ready->replica.allocation, *offset};
            }
            const PlannedAllocationRef produced = std::get<PlannedAllocationRef>(storage);
            if (!produced.allocation.Valid() || produced.allocation.index >= planned.size() ||
                produced.allocation.index >= materialized.Size()) {
                return std::unexpected(Failure(ErrorCode::InvalidPlan, segment, subject));
            }
            const PlannedAllocation& mapping = planned[produced.allocation.index];
            if (!mapping.bufferRegion.Contains(requested)) {
                return std::unexpected(Failure(ErrorCode::InvalidPlan, segment, subject));
            }
            const uint64_t offset = requested.Offset() - mapping.bufferRegion.Offset();
            const Allocation& allocation = materialized[produced.allocation.index];
            if (!allocation.View().Covers(offset, requested.Size())) {
                return std::unexpected(Failure(ErrorCode::InvalidPlan, segment, subject));
            }
            return ResolvedStorage{allocation, offset};
        }

        [[nodiscard]] std::expected<ResolvedStorage, Error>
        ResolveAllocation(PlannedAllocationId id, ByteRegion requested, std::span<const PlannedAllocation> planned,
                          const AllocationJournal& materialized, uint32_t segment, uint32_t subject) noexcept {
            if (!id.Valid() || id.index >= planned.size() || id.index >= materialized.Size()) {
                return std::unexpected(Failure(ErrorCode::InvalidPlan, segment, subject));
            }
            const PlannedAllocation& mapping = planned[id.index];
            if (!mapping.bufferRegion.Contains(requested)) {
                return std::unexpected(Failure(ErrorCode::InvalidPlan, segment, subject));
            }
            const uint64_t offset = requested.Offset() - mapping.bufferRegion.Offset();
            const Allocation& allocation = materialized[id.index];
            if (!allocation.View().Covers(offset, requested.Size())) {
                return std::unexpected(Failure(ErrorCode::InvalidPlan, segment, subject));
            }
            return ResolvedStorage{allocation, offset};
        }

        [[nodiscard]] std::expected<PendingReplica, Error> PendingFor(PlannedAllocationId id,
                                                                      std::span<const PlannedAllocation> planned,
                                                                      const AllocationJournal& materialized,
                                                                      uint32_t segment, uint32_t subject) noexcept {
            if (!id.Valid() || id.index >= planned.size() || id.index >= materialized.Size()) {
                return std::unexpected(Failure(ErrorCode::InvalidPlan, segment, subject));
            }
            const PlannedAllocation& mapping = planned[id.index];
            auto pending = PendingReplica::Create(materialized[id.index], mapping.bufferRegion);
            if (!pending) {
                return std::unexpected(Failure(ErrorCode::InvalidPlan, segment, subject, Cause(pending.error())));
            }
            return std::move(*pending);
        }

        [[nodiscard]] std::expected<PhysicalBufferSource, Error>
        ReadSource(const PlannedStorageRef& storage, std::span<const PlannedAllocation> planned,
                   const AllocationJournal& materialized, uint32_t segment, uint32_t subject) noexcept {
            if (const auto* ready = std::get_if<ReadyStorageRef>(&storage)) {
                return PhysicalBufferSource{ready->replica};
            }
            return PendingFor(std::get<PlannedAllocationRef>(storage).allocation, planned, materialized, segment,
                              subject)
                .transform([](PendingReplica pending) { return PhysicalBufferSource{std::move(pending)}; });
        }

        [[nodiscard]] std::expected<ResolvedStorage, Error>
        ResolveCoverageStorage(const PlannedStorageRef& storage, ByteCoverageView coverage,
                               std::span<const PlannedAllocation> planned, const AllocationJournal& materialized,
                               uint32_t segment, uint32_t subject) noexcept {
            if (const auto* ready = std::get_if<ReadyStorageRef>(&storage)) {
                const auto offset = ready->replica.ResolveAllocationOffset(coverage);
                if (!offset) {
                    return std::unexpected(Failure(ErrorCode::InvalidPlan, segment, subject));
                }
                return ResolvedStorage{ready->replica.allocation, *offset};
            }
            const PlannedAllocationRef produced = std::get<PlannedAllocationRef>(storage);
            if (!produced.allocation.Valid() || produced.allocation.index >= planned.size() ||
                produced.allocation.index >= materialized.Size()) {
                return std::unexpected(Failure(ErrorCode::InvalidPlan, segment, subject));
            }
            const PlannedAllocation& mapping = planned[produced.allocation.index];
            if (mapping.Coverage().Contains(coverage) != CoverageContainment::Yes) {
                return std::unexpected(Failure(ErrorCode::InvalidPlan, segment, subject));
            }
            const uint64_t offset = coverage.BoundingRegion().Offset() - mapping.bufferRegion.Offset();
            const Allocation& allocation = materialized[produced.allocation.index];
            if (!allocation.View().Covers(offset, coverage.BoundingRegion().Size())) {
                return std::unexpected(Failure(ErrorCode::InvalidPlan, segment, subject));
            }
            return ResolvedStorage{allocation, offset};
        }

        [[nodiscard]] std::expected<std::optional<CompletionTransaction>, Error>
        BuildCompletion(std::span<const PlannedAllocation> planned, const AllocationJournal& materialized) noexcept {
            CompletionTransaction transaction;
            for (size_t index = 0; index < planned.size(); ++index) {
                const PlannedAllocation& allocation = planned[index];
                const uint32_t allocationIndex = static_cast<uint32_t>(index);
                if (!allocation.publication) {
                    continue;
                }
                const PlannedPublication publication = *allocation.publication;
                if (publication.kind == PlannedPublicationKind::PartitionedWrite && publication.partitionIndex != 0) {
                    continue;
                }
                std::expected<void, PublicationError> added;
                if (publication.kind == PlannedPublicationKind::Replica) {
                    if (allocation.byteFootprint) {
                        added = transaction.AddReplica(allocation.buffer, publication.expectedVersion,
                                                       *allocation.byteFootprint, materialized[index]);
                    } else {
                        added = transaction.AddReplica(allocation.buffer, publication.expectedVersion,
                                                       allocation.bufferRegion, materialized[index]);
                    }
                } else if (publication.kind == PlannedPublicationKind::WholeWrite) {
                    added =
                        transaction.AddWholeWrite(allocation.buffer, publication.expectedVersion, materialized[index]);
                } else {
                    try {
                        std::vector<PartitionedWritePart> parts;
                        parts.reserve(publication.partitionCount);
                        for (uint32_t partitionIndex = 0; partitionIndex < publication.partitionCount;
                             ++partitionIndex) {
                            const auto found = std::ranges::find_if(planned, [&](const PlannedAllocation& candidate) {
                                return candidate.BufferIdentity() == allocation.BufferIdentity() &&
                                       candidate.publication &&
                                       candidate.publication->kind == PlannedPublicationKind::PartitionedWrite &&
                                       candidate.publication->expectedVersion == publication.expectedVersion &&
                                       candidate.publication->partitionCount == publication.partitionCount &&
                                       candidate.publication->partitionIndex == partitionIndex;
                            });
                            if (found == planned.end()) {
                                return std::unexpected(Failure(ErrorCode::CompletionConstructionFailed,
                                                               publication.producer.index, allocationIndex));
                            }
                            const size_t foundIndex = static_cast<size_t>(found - planned.begin());
                            parts.push_back({.region = found->bufferRegion,
                                             .allocation = materialized[foundIndex],
                                             .allocationOffset = 0,
                                             .byteFootprint = found->byteFootprint});
                        }
                        added = transaction.AddPartitionedWrite(allocation.buffer, publication.expectedVersion, parts);
                    } catch (const std::bad_alloc&) {
                        return std::unexpected(Failure(ErrorCode::OutOfMemory));
                    }
                }
                if (!added) {
                    return std::unexpected(Failure(ErrorCode::CompletionConstructionFailed, publication.producer.index,
                                                   allocationIndex, Cause(added.error().code), added.error().action));
                }
            }
            if (transaction.ActionCount() == 0) {
                return std::optional<CompletionTransaction>{};
            }
            return std::optional<CompletionTransaction>{std::move(transaction)};
        }

        [[nodiscard]] std::expected<PhysicalSegment, Error>
        MaterializeDispatch(const PlannedDispatch& dispatch, uint32_t segment,
                            std::span<const PlannedAllocation> planned, const AllocationJournal& materialized,
                            const PhysicalResourceCatalog& resources, ArtifactResolutionServices services) {
            const Queue* queue = resources.FindQueue(dispatch.queue.service, dispatch.queue.backend,
                                                     dispatch.queue.device, dispatch.queue.queue);
            if (queue == nullptr) {
                return std::unexpected(Failure(ErrorCode::MissingPhysicalResource, segment));
            }
            const QueueView queueView = queue->View();
            const BackendCompiler* compiler =
                resources.FindCompiler(queueView.service, dispatch.target.backend, dispatch.target.compilerIdentity);
            if (compiler == nullptr) {
                return std::unexpected(Failure(ErrorCode::MissingPhysicalResource, segment));
            }

            std::vector<PhysicalBufferSource> sources;
            sources.reserve(dispatch.bufferStorage.size());
            size_t bufferIndex = 0;
            for (size_t argumentIndex = 0; argumentIndex < dispatch.invocation.Signature().size(); ++argumentIndex) {
                const KernelArgumentDesc& argument = dispatch.invocation.Signature()[argumentIndex];
                if (argument.kind != ParameterKind::Buffer) {
                    continue;
                }
                const uint32_t argumentId = static_cast<uint32_t>(argumentIndex);
                if (bufferIndex >= dispatch.bufferStorage.size()) {
                    return std::unexpected(Failure(ErrorCode::InvalidPlan, segment, argumentId));
                }
                const PlannedDispatchBuffer& storage = dispatch.bufferStorage[bufferIndex++];
                if (argument.access == AccessMode::Read) {
                    if (!storage.input) {
                        return std::unexpected(Failure(ErrorCode::InvalidPlan, segment, argumentId));
                    }
                    auto source = ReadSource(*storage.input, planned, materialized, segment, argumentId);
                    if (!source) {
                        return std::unexpected(source.error());
                    }
                    sources.push_back(std::move(*source));
                } else {
                    auto pending = PendingFor(storage.output, planned, materialized, segment, argumentId);
                    if (!pending) {
                        return std::unexpected(pending.error());
                    }
                    sources.emplace_back(std::move(*pending));
                }
            }

            auto bindings = MaterializeKernelBindings(dispatch.invocation, sources, dispatch.slice);
            if (!bindings) {
                return std::unexpected(Failure(ErrorCode::BindingFailed, segment, bindings.error().argument,
                                               Cause(bindings.error().code), bindings.error().related));
            }
            if (bindings->buffers.size() > UINT32_MAX || bindings->scalars.size() > UINT32_MAX) {
                return std::unexpected(Failure(ErrorCode::BindingFailed, segment));
            }

            const std::span<const std::byte> module = dispatch.program.Bytes();
            const BackendCompileRequest request{.moduleData = module.data(),
                                                .moduleSize = module.size(),
                                                .moduleHash = dispatch.program.Hash(),
                                                .target = dispatch.target,
                                                .specialization = dispatch.specialization,
                                                .artifactKind = ArtifactKind::DeviceExecutable};
            auto executable = services.compiler.Resolve(*compiler, request);
            if (!executable) {
                return std::unexpected(Failure(ErrorCode::CompilationFailed, segment, kInvalidPhysicalPlanIndex,
                                               Cause(executable.error().code), executable.error().detail));
            }

            Artifact dispatchArtifact = *executable;
            if (queueView.dispatchArtifactKind == ArtifactKind::DevicePipeline) {
                if (!dispatch.pipeline) {
                    return std::unexpected(Failure(ErrorCode::InvalidPlan, segment));
                }
                const BackendPipelineFactory* factory = resources.FindPipelineFactory(
                    queueView.service, dispatch.target.backend, dispatch.target.device, dispatch.pipeline->factory);
                if (factory == nullptr) {
                    return std::unexpected(Failure(ErrorCode::MissingPhysicalResource, segment));
                }
                const PipelineStateKey pipelineState{
                    .state = dispatch.pipeline->state,
                    .bindingLayout = PipelineBindingLayoutIdentity(dispatch.invocation.Signature()),
                };
                auto pipeline = services.pipeline.Resolve(*factory, *executable, pipelineState);
                if (!pipeline) {
                    return std::unexpected(Failure(ErrorCode::PipelineResolutionFailed, segment,
                                                   kInvalidPhysicalPlanIndex, Cause(pipeline.error().code),
                                                   pipeline.error().detail));
                }
                dispatchArtifact = std::move(*pipeline);
            } else if (queueView.dispatchArtifactKind != ArtifactKind::DeviceExecutable || dispatch.pipeline) {
                return std::unexpected(Failure(ErrorCode::InvalidPlan, segment));
            }

            BackendCommand command{
                .kind = BackendCommandKind::Dispatch,
                .bindingCount = static_cast<uint32_t>(bindings->buffers.size()),
                .scalarBindingCount = static_cast<uint32_t>(bindings->scalars.size()),
                .artifact = dispatchArtifact.View(),
                .dispatchOrigin = {dispatch.slice.origin[0], dispatch.slice.origin[1], dispatch.slice.origin[2]},
                .dispatchExtents = {dispatch.slice.extents[0], dispatch.slice.extents[1], dispatch.slice.extents[2]},
                .globalExtents = {dispatch.invocation.Domain().extents[0], dispatch.invocation.Domain().extents[1],
                                  dispatch.invocation.Domain().extents[2]},
                .workgroupCount = {dispatch.geometry.workgroupCount[0], dispatch.geometry.workgroupCount[1],
                                   dispatch.geometry.workgroupCount[2]}};
            ExecutableBatchStorage storage{.queue = *queue,
                                           .commands = {command},
                                           .bindings = std::move(bindings->buffers),
                                           .scalarData = std::move(bindings->scalarData),
                                           .scalarBindings = std::move(bindings->scalars),
                                           .artifacts = {std::move(dispatchArtifact)}};
            storage.allocations.reserve(sources.size());
            for (const PhysicalBufferSource& source : sources) {
                std::visit(
                    [&](const auto& value) {
                        using Source = std::remove_cvref_t<decltype(value)>;
                        if constexpr (std::same_as<Source, ResidentReplica>) {
                            storage.allocations.push_back(value.allocation);
                        } else {
                            storage.allocations.push_back(value.Storage());
                        }
                    },
                    source);
            }
            auto batch = ExecutableBatch::Create(std::move(storage));
            if (!batch) {
                return std::unexpected(Failure(ErrorCode::BatchConstructionFailed, segment, kInvalidPhysicalPlanIndex,
                                               Cause(batch.error().code), batch.error().detail));
            }
            return PhysicalSegment{
                std::move(*batch),
                KernelPerformanceObservationDescriptor{.key = {.kernel = dispatch.invocation.ProgramHash(),
                                                               .target = dispatch.target,
                                                               .specialization = dispatch.specialization,
                                                               .queue = dispatch.queue.queue},
                                                       .workItemCount = dispatch.slice.workItemCount,
                                                       .deterministic = dispatch.deterministic},
                services.kernelPerformance};
        }

        [[nodiscard]] std::expected<PhysicalSegment, Error> MaterializeCopy(const PlannedCopy& copy, uint32_t segment,
                                                                            std::span<const PlannedAllocation> planned,
                                                                            const AllocationJournal& materialized,
                                                                            const PhysicalResourceCatalog& resources) {
            const Queue* queue =
                resources.FindQueue(copy.queue.service, copy.queue.backend, copy.queue.device, copy.queue.queue);
            if (queue == nullptr) {
                return std::unexpected(Failure(ErrorCode::MissingPhysicalResource, segment));
            }
            auto source =
                ResolveStorage(copy.source, copy.operation.Source().Region(), planned, materialized, segment, 0);
            if (!source) {
                return std::unexpected(source.error());
            }
            auto destination = ResolveAllocation(copy.destination, copy.operation.Destination().Region(), planned,
                                                 materialized, segment, 1);
            if (!destination) {
                return std::unexpected(destination.error());
            }
            const BackendCommand command{.kind = BackendCommandKind::Copy,
                                         .source = source->allocation.View(),
                                         .target = destination->allocation.View(),
                                         .sourceOffset = source->offset,
                                         .targetOffset = destination->offset,
                                         .sizeBytes = copy.operation.SizeBytes()};
            ExecutableBatchStorage storage{
                .queue = *queue, .commands = {command}, .allocations = {source->allocation, destination->allocation}};
            auto batch = ExecutableBatch::Create(std::move(storage));
            if (!batch) {
                return std::unexpected(Failure(ErrorCode::BatchConstructionFailed, segment, kInvalidPhysicalPlanIndex,
                                               Cause(batch.error().code), batch.error().detail));
            }
            return PhysicalSegment{std::move(*batch)};
        }

        [[nodiscard]] std::expected<PhysicalSegment, Error> MaterializeFill(const PlannedFill& fill, uint32_t segment,
                                                                            std::span<const PlannedAllocation> planned,
                                                                            const AllocationJournal& materialized,
                                                                            const PhysicalResourceCatalog& resources) {
            const Queue* queue =
                resources.FindQueue(fill.queue.service, fill.queue.backend, fill.queue.device, fill.queue.queue);
            if (queue == nullptr) {
                return std::unexpected(Failure(ErrorCode::MissingPhysicalResource, segment));
            }
            auto destination = ResolveAllocation(fill.destination, fill.operation.Destination().Region(), planned,
                                                 materialized, segment, 0);
            if (!destination) {
                return std::unexpected(destination.error());
            }
            const FillPattern pattern = fill.operation.Pattern();
            BackendCommand command{.kind = BackendCommandKind::Fill,
                                   .target = destination->allocation.View(),
                                   .targetOffset = destination->offset,
                                   .sizeBytes = fill.operation.SizeBytes(),
                                   .fillPatternSize = pattern.size};
            for (size_t index = 0; index < pattern.Bytes().size(); ++index) {
                command.fillPattern[index] = std::to_integer<uint8_t>(pattern.Bytes()[index]);
            }
            ExecutableBatchStorage storage{
                .queue = *queue, .commands = {command}, .allocations = {destination->allocation}};
            auto batch = ExecutableBatch::Create(std::move(storage));
            if (!batch) {
                return std::unexpected(Failure(ErrorCode::BatchConstructionFailed, segment, kInvalidPhysicalPlanIndex,
                                               Cause(batch.error().code), batch.error().detail));
            }
            return PhysicalSegment{std::move(*batch)};
        }

        [[nodiscard]] std::expected<PhysicalSegment, Error>
        MaterializeAssembly(const PlannedAssembly& assembly, uint32_t segment,
                            std::span<const PlannedAllocation> planned, const AllocationJournal& materialized,
                            const PhysicalResourceCatalog& resources) {
            constexpr size_t commandCapacity = 256;
            const Queue* queue = resources.FindQueue(assembly.queue.service, assembly.queue.backend,
                                                     assembly.queue.device, assembly.queue.queue);
            if (queue == nullptr || !assembly.destination.Valid() || assembly.destination.index >= planned.size() ||
                assembly.destination.index >= materialized.Size()) {
                return std::unexpected(Failure(ErrorCode::InvalidPlan, segment));
            }
            const PlannedAllocation& destinationMapping = planned[assembly.destination.index];
            const Allocation& destination = materialized[assembly.destination.index];

            bool boundingRegionsDisjoint = true;
            for (size_t index = 0; index < assembly.parts.size(); ++index) {
                for (size_t prior = 0; prior < index; ++prior) {
                    boundingRegionsDisjoint = boundingRegionsDisjoint &&
                                              !assembly.parts[prior].region.Intersects(assembly.parts[index].region);
                }
            }

            ExecutableBatchStorage batchStorage{.queue = *queue};
            const size_t localPartCount = std::ranges::count_if(
                assembly.parts, [](const PlannedAssemblyPart& part) { return part.source.has_value(); });
            if (localPartCount != 0) {
                batchStorage.commands.reserve(commandCapacity);
            }
            batchStorage.allocations.reserve(localPartCount + 1);
            for (uint32_t index = 0; index < assembly.parts.size(); ++index) {
                const PlannedAssemblyPart& part = assembly.parts[index];
                if (!part.source) {
                    continue;
                }
                const ByteCoverageView coverage = part.Coverage();
                auto source = ResolveCoverageStorage(*part.source, coverage, planned, materialized, segment, index);
                if (!source) {
                    return std::unexpected(source.error());
                }
                const auto append = [&](ByteRegion logical) -> bool {
                    if (batchStorage.commands.size() == commandCapacity) {
                        return false;
                    }
                    const uint64_t sourceOffset = source->offset + logical.Offset() - part.region.Offset();
                    const uint64_t destinationOffset = logical.Offset() - destinationMapping.bufferRegion.Offset();
                    if (!source->allocation.View().Covers(sourceOffset, logical.Size()) ||
                        !destination.View().Covers(destinationOffset, logical.Size())) {
                        return false;
                    }
                    batchStorage.commands.push_back({.kind = BackendCommandKind::Copy,
                                                     .source = source->allocation.View(),
                                                     .target = destination.View(),
                                                     .sourceOffset = sourceOffset,
                                                     .targetOffset = destinationOffset,
                                                     .sizeBytes = logical.Size()});
                    return true;
                };
                if (boundingRegionsDisjoint) {
                    if (!append(part.region)) {
                        return std::unexpected(Failure(ErrorCode::InvalidPlan, segment, index));
                    }
                } else {
                    const auto regions = coverage.ProjectCanonicalRegions<commandCapacity>();
                    if (!regions) {
                        return std::unexpected(Failure(ErrorCode::InvalidPlan, segment, index, Cause(regions.error())));
                    }
                    for (const ByteRegion region : regions->Regions()) {
                        if (!append(region)) {
                            return std::unexpected(Failure(ErrorCode::InvalidPlan, segment, index));
                        }
                    }
                }
                batchStorage.allocations.push_back(std::move(source->allocation));
            }
            batchStorage.allocations.push_back(destination);
            batchStorage.signalOnly = batchStorage.commands.empty();
            auto batch = ExecutableBatch::Create(std::move(batchStorage));
            if (!batch) {
                return std::unexpected(Failure(ErrorCode::BatchConstructionFailed, segment, kInvalidPhysicalPlanIndex,
                                               Cause(batch.error().code), batch.error().detail));
            }
            return PhysicalSegment{std::move(*batch)};
        }

        [[nodiscard]] std::expected<PhysicalSegment, Error>
        MaterializeTransfer(const PlannedTransfer& transfer, uint32_t segment,
                            std::span<const PlannedAllocation> planned, const AllocationJournal& materialized,
                            const PhysicalResourceCatalog& resources) {
            constexpr size_t regionCapacity = 256;
            const TransferEngine* engine = resources.FindTransferEngine(transfer.service, transfer.engine);
            if (engine == nullptr) {
                return std::unexpected(Failure(ErrorCode::MissingPhysicalResource, segment));
            }
            const ByteCoverageView coverage = transfer.Coverage();
            auto source = ResolveCoverageStorage(transfer.source, coverage, planned, materialized, segment, 0);
            if (!source) {
                return std::unexpected(source.error());
            }
            auto destination =
                ResolveAllocation(transfer.destination, transfer.region, planned, materialized, segment, 1);
            if (!destination) {
                return std::unexpected(destination.error());
            }
            const auto projected = coverage.ProjectCanonicalRegions<regionCapacity>();
            if (!projected) {
                return std::unexpected(
                    Failure(ErrorCode::InvalidPlan, segment, kInvalidPhysicalPlanIndex, Cause(projected.error())));
            }
            std::vector<TransferRegion> regions;
            regions.reserve(projected->Size());
            for (const ByteRegion logical : projected->Regions()) {
                const uint64_t relativeOffset = logical.Offset() - transfer.region.Offset();
                regions.push_back({.sourceOffset = source->offset + relativeOffset,
                                   .targetOffset = destination->offset + relativeOffset,
                                   .sizeBytes = logical.Size()});
            }
            auto executable = ExecutableTransfer::Create({.engine = *engine,
                                                          .source = source->allocation,
                                                          .destination = destination->allocation,
                                                          .regions = std::move(regions)});
            if (!executable) {
                return std::unexpected(Failure(ErrorCode::TransferConstructionFailed, segment,
                                               kInvalidPhysicalPlanIndex, Cause(executable.error().code),
                                               executable.error().detail));
            }
            return PhysicalSegment{std::move(*executable)};
        }

        [[nodiscard]] std::expected<PhysicalSegment, Error>
        MaterializeSegment(const PhysicalPlanSegment& segment, uint32_t index,
                           std::span<const PlannedAllocation> planned, const AllocationJournal& materialized,
                           const PhysicalResourceCatalog& resources, ArtifactResolutionServices services) {
            return std::visit(
                [&]<typename Segment>(const Segment& value) -> std::expected<PhysicalSegment, Error> {
                    if constexpr (std::same_as<Segment, PlannedDispatch>) {
                        return MaterializeDispatch(value, index, planned, materialized, resources, services);
                    } else if constexpr (std::same_as<Segment, PlannedCopy>) {
                        return MaterializeCopy(value, index, planned, materialized, resources);
                    } else if constexpr (std::same_as<Segment, PlannedFill>) {
                        return MaterializeFill(value, index, planned, materialized, resources);
                    } else if constexpr (std::same_as<Segment, PlannedAssembly>) {
                        return MaterializeAssembly(value, index, planned, materialized, resources);
                    } else {
                        return MaterializeTransfer(value, index, planned, materialized, resources);
                    }
                },
                segment);
        }

        [[nodiscard]] bool HasService(std::span<const DeviceLease> leases, BackendServiceId service) noexcept {
            return std::ranges::any_of(leases,
                                       [service](const DeviceLease& lease) { return lease.Service() == service; });
        }

        [[nodiscard]] bool ResourcesMatchPlan(const PhysicalGraphPlan& plan,
                                              const PhysicalResourceCatalog& resources) noexcept {
            const std::span<const DeviceLease> leases = plan.DeviceLeases();
            for (const PlannedAllocation& allocation : plan.Allocations()) {
                const BackendServiceId service = allocation.service;
                if (!service.Valid() || !HasService(leases, service)) {
                    return false;
                }
            }
            for (const PhysicalPlanSegment& segment : plan.Segments()) {
                const BackendServiceId service = std::visit(
                    [&](const auto& value) {
                        using Segment = std::remove_cvref_t<decltype(value)>;
                        if constexpr (std::same_as<Segment, PlannedTransfer>) {
                            return value.service;
                        } else {
                            return value.queue.service;
                        }
                    },
                    segment);
                if (!service.Valid() || !HasService(leases, service)) {
                    return false;
                }
            }
            return true;
        }

    } // namespace

    std::expected<ExecutableGraph, PhysicalGraphMaterializationError>
    MaterializePhysicalGraph(const PhysicalGraphPlan& plan, const PhysicalResourceCatalog& resources,
                             ArtifactResolutionServices services) noexcept {
        try {
            if (!ResourcesMatchPlan(plan, resources)) {
                return std::unexpected(Failure(ErrorCode::DeviceInvalidated));
            }
            for (uint32_t index = 0; index < plan.DeviceLeases().size(); ++index) {
                auto current = plan.DeviceLeases()[index].Validate();
                if (!current) {
                    return std::unexpected(Failure(ErrorCode::DeviceInvalidated, kInvalidPhysicalPlanIndex, index,
                                                   Cause(current.error())));
                }
            }
            const std::span<const PlannedAllocation> planned = plan.Allocations();
            AllocationJournal materialized;
            materialized.Reserve(planned.size());
            for (size_t index = 0; index < planned.size(); ++index) {
                const PlannedAllocation& allocation = planned[index];
                const uint32_t allocationIndex = static_cast<uint32_t>(index);
                if (const auto* request = std::get_if<PlannedAllocate>(&allocation.materialization)) {
                    const MemoryAllocator* allocator = resources.FindAllocator(allocation.service, allocation.location);
                    if (allocator == nullptr) {
                        return std::unexpected(
                            Failure(ErrorCode::MissingPhysicalResource, kInvalidPhysicalPlanIndex, allocationIndex));
                    }
                    auto storage = allocator->Allocate({.location = allocation.location,
                                                        .requiredProperties = request->requiredProperties,
                                                        .preferredProperties = request->preferredProperties,
                                                        .sizeBytes = allocation.bufferRegion.Size(),
                                                        .alignment = request->alignment});
                    if (!storage) {
                        return std::unexpected(Failure(ErrorCode::AllocationFailed, kInvalidPhysicalPlanIndex,
                                                       allocationIndex, Cause(storage.error().code),
                                                       storage.error().detail));
                    }
                    materialized.Append(std::move(*storage));
                    continue;
                }

                const auto& alias = std::get<PlannedCoherentAlias>(allocation.materialization);
                const TransferEngine* engine = resources.FindTransferEngine(allocation.service, alias.engine);
                if (engine == nullptr) {
                    return std::unexpected(
                        Failure(ErrorCode::MissingPhysicalResource, kInvalidPhysicalPlanIndex, allocationIndex));
                }
                auto source = ResolveStorage(alias.source, allocation.bufferRegion, planned, materialized,
                                             kInvalidPhysicalPlanIndex, allocationIndex);
                if (!source) {
                    return std::unexpected(source.error());
                }
                auto imported =
                    engine->ImportAlias(std::move(source->allocation), source->offset, allocation.bufferRegion.Size());
                if (!imported) {
                    return std::unexpected(Failure(ErrorCode::AliasImportFailed, kInvalidPhysicalPlanIndex,
                                                   allocationIndex, Cause(imported.error().code),
                                                   imported.error().detail));
                }
                materialized.Append(std::move(*imported));
            }

            auto completion = BuildCompletion(planned, materialized);
            if (!completion) {
                return std::unexpected(completion.error());
            }

            std::vector<PhysicalSegment> segments;
            segments.reserve(plan.Segments().size());
            for (size_t index = 0; index < plan.Segments().size(); ++index) {
                const uint32_t segmentIndex = static_cast<uint32_t>(index);
                auto segment = MaterializeSegment(plan.Segments()[index], segmentIndex, planned, materialized,
                                                  resources, services);
                if (!segment) {
                    return std::unexpected(segment.error());
                }
                segments.push_back(std::move(*segment));
            }

            std::vector<ExecutableEdge> edges;
            edges.reserve(plan.Edges().size());
            for (const PhysicalPlanEdge& edge : plan.Edges()) {
                const PhysicalSynchronizationDomain predecessor =
                    segments[edge.predecessor.index].SynchronizationDomain();
                const PhysicalSynchronizationDomain successor = segments[edge.successor.index].SynchronizationDomain();
                edges.push_back({.predecessor = {edge.predecessor.index},
                                 .successor = {edge.successor.index},
                                 .kind = predecessor == successor ? ExecutableDependencyKind::DeviceWait
                                                                  : ExecutableDependencyKind::HostCompletion});
            }

            std::vector<DeviceLease> deviceLeases{plan.DeviceLeases().begin(), plan.DeviceLeases().end()};
            auto graph = ExecutableGraph::Create({.deviceLeases = std::move(deviceLeases),
                                                  .segments = std::move(segments),
                                                  .edges = std::move(edges),
                                                  .completion = std::move(*completion)});
            if (!graph) {
                return std::unexpected(Failure(ErrorCode::GraphConstructionFailed, graph.error().successor,
                                               graph.error().predecessor, Cause(graph.error().code)));
            }
            return std::move(*graph);
        } catch (const std::bad_alloc&) {
            return std::unexpected(Failure(ErrorCode::OutOfMemory));
        }
    }

} // namespace Sora::Compute

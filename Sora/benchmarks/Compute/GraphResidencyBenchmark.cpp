/**
 * @file GraphResidencyBenchmark.cpp
 * @brief Independent baselines for symbolic residency lookup and physical route expansion.
 */

#include <BenchmarkHarness.h>
#include <Sora/Compute/Runtime/DispatchCandidateEnumeration.h>
#include <Sora/Compute/Runtime/GraphResidency.h>
#include <Sora/Compute/Runtime/PhysicalGraphPlanner.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <print>
#include <utility>
#include <vector>

namespace {

    constexpr size_t kSamples = 9;
    constexpr uint32_t kBindingCount = 1024;
    constexpr uint32_t kSelectedBinding = kBindingCount / 2;
    constexpr auto kMinimumSampleTime = std::chrono::milliseconds(50);
    constexpr Sora::Compute::BackendServiceId kSourceService{100, 101};
    constexpr Sora::Compute::BackendServiceId kDestinationService{102, 103};
    constexpr Sora::Compute::DeviceId kSourceDevice{10, 11};
    constexpr Sora::Compute::DeviceId kDestinationDevice{12, 13};
    constexpr Sora::Compute::MemoryLocation kSourceLocation{
        .domain = {20, 21}, .device = kSourceDevice, .backend = Sora::Compute::BackendKind::Cpu};
    constexpr Sora::Compute::MemoryLocation kDestinationLocation{
        .domain = {22, 23}, .device = kDestinationDevice, .backend = Sora::Compute::BackendKind::Vulkan};
    constexpr Sora::Compute::ValueType kFloat{Sora::Compute::ScalarKind::Float32, 1};
    constexpr std::array kAffineExtents{uint64_t{1'000'000}};
    constexpr std::array kAffineStrides{uint64_t{8}};
    constexpr std::array kAffineHalfExtents{uint64_t{500'000}};
    constexpr auto kAffineDomain =
        Sora::Compute::AffineByteFootprint::Create(kAffineExtents, kAffineStrides, sizeof(float), 0, sizeof(float), 1);
    constexpr auto kAffineFirst = Sora::Compute::AffineByteFootprint::Create(kAffineHalfExtents, kAffineStrides,
                                                                             sizeof(float), 0, sizeof(float), 1);
    constexpr auto kAffineSecond = Sora::Compute::AffineByteFootprint::Create(
        kAffineHalfExtents, kAffineStrides, sizeof(float), 4'000'000, sizeof(float), 1);
    static_assert(kAffineDomain && kAffineFirst && kAffineSecond);

    class GraphResidencyFixture final {
    public:
        GraphResidencyFixture() {
            buffers_.reserve(kBindingCount);
            prefix_.allocations.reserve(kBindingCount);
            prefix_.segments.reserve(kBindingCount);
            for (uint32_t index = 0; index < kBindingCount; ++index) {
                auto buffer = Sora::Compute::Buffer::Create({.id = BufferIdentity(index),
                                                             .elementType = kFloat,
                                                             .layout = Sora::Compute::LayoutKind::Contiguous,
                                                             .rank = 1,
                                                             .sizeBytes = 4096,
                                                             .alignment = 64});
                if (!buffer || !residency_.Track(buffer->Describe(), 0)) {
                    failed_ = true;
                    return;
                }
                const auto symbolic = SymbolicSource(index);
                const auto write = Sora::Compute::GraphResidencyUpdate{.writes = {{.buffer = symbolic.buffer,
                                                                                   .region = symbolic.coverage,
                                                                                   .version = symbolic.version,
                                                                                   .producer = *symbolic.producer,
                                                                                   .replica = symbolic.replica}}};
                if (!residency_.Apply(write) ||
                    !graphLocal_.Bind({.symbolic = symbolic, .physical = {.allocation = {index}}})) {
                    failed_ = true;
                    return;
                }
                prefix_.allocations.push_back({.id = {index},
                                               .buffer = *buffer,
                                               .service = kSourceService,
                                               .location = kSourceLocation,
                                               .bufferRegion = symbolic.coverage,
                                               .version = symbolic.version,
                                               .materialization = Sora::Compute::PlannedAllocate{.alignment = 64}});
                prefix_.segments.emplace_back(Sora::Compute::PlannedTransfer{});
                buffers_.push_back(std::move(*buffer));
            }
            selected_ = SymbolicSource(kSelectedBinding);
            route_ = {.buffer = selected_.buffer,
                      .source = selected_,
                      .destinationService = kDestinationService,
                      .destination = kDestinationLocation,
                      .region = *Sora::Compute::ByteRegion::Make(128, 256),
                      .version = selected_.version,
                      .estimatedNanoseconds = 400,
                      .hops = {{.sourceService = kSourceService,
                                .service = kDestinationService,
                                .engine = {30, 31},
                                .source = kSourceLocation,
                                .destination = kDestinationLocation,
                                .kind = Sora::Compute::TransferKind::Remote,
                                .bytesPerSecond = 8'000'000'000,
                                .latencyNanoseconds = 80}}};

            auto affineBuffer = Sora::Compute::Buffer::Create({.id = {0xB033, 1},
                                                               .elementType = kFloat,
                                                               .layout = Sora::Compute::LayoutKind::Strided,
                                                               .rank = 1,
                                                               .sizeBytes = kAffineDomain->BoundingRegion().End(),
                                                               .alignment = 64},
                                                              *kAffineDomain);
            if (!affineBuffer || !affineResidency_.Track(*affineBuffer)) {
                failed_ = true;
                return;
            }
            affineBuffer_ = std::move(*affineBuffer);
            const auto write = [](const Sora::Compute::AffineByteFootprint& footprint) {
                return Sora::Compute::ProducedResidencyWrite{
                    .buffer = {0xB033, 1},
                    .region = footprint.BoundingRegion(),
                    .byteFootprint = footprint,
                    .version = 1,
                    .producer = Sora::Compute::GraphNodeId{0},
                    .replica = {.service = kSourceService, .location = kSourceLocation, .materializer = {0}}};
            };
            affineUpdate_.writes = {write(*kAffineFirst), write(*kAffineSecond)};
            if (!affineResidency_.Apply(affineUpdate_)) {
                failed_ = true;
                return;
            }

            if (!affineWholeResidency_.Track(affineBuffer_)) {
                failed_ = true;
                return;
            }
            affineWholeUpdate_.writes = {write(*kAffineDomain)};
            if (!affineWholeResidency_.Apply(affineWholeUpdate_)) {
                failed_ = true;
                return;
            }
            const auto replica = [](const Sora::Compute::AffineByteFootprint& footprint) {
                return Sora::Compute::GraphResidencyReplicaAddition{
                    .buffer = {0xB033, 1},
                    .region = footprint.BoundingRegion(),
                    .byteFootprint = footprint,
                    .version = 1,
                    .producer = Sora::Compute::GraphNodeId{0},
                    .replica = {.service = kDestinationService, .location = kDestinationLocation, .materializer = {1}}};
            };
            exactAffineReplica_.replicas = {replica(*kAffineDomain)};
            partialAffineReplica_.replicas = {replica(*kAffineFirst)};

            if (!partitionCommitResidency_.Track(affineBuffer_)) {
                failed_ = true;
                return;
            }
            const auto binding = [&](Sora::Compute::KernelArgumentId argument,
                                     const Sora::Compute::AffineByteFootprint& footprint) {
                return Sora::Compute::DispatchMemoryBinding{.argument = argument,
                                                            .access = Sora::Compute::AccessMode::Write,
                                                            .buffer = affineBuffer_,
                                                            .region = footprint.BoundingRegion(),
                                                            .byteFootprint = footprint,
                                                            .alignment = 64};
            };
            partitionLogicalBindings_[0] = binding({0}, *kAffineDomain);
            partitionProjections_[0] = {
                .service = kSourceService,
                .device = kSourceDevice,
                .destination = Sora::Compute::PhysicalMemoryDescriptor{.service = kSourceService,
                                                                       .location = kSourceLocation,
                                                                       .supportedProperties =
                                                                           Sora::Compute::MemoryProperty::DeviceLocal},
                .bindings = {binding({0}, *kAffineFirst)},
            };
            partitionProjections_[1] = {
                .service = kDestinationService,
                .device = kDestinationDevice,
                .destination = Sora::Compute::PhysicalMemoryDescriptor{.service = kDestinationService,
                                                                       .location = kDestinationLocation,
                                                                       .supportedProperties =
                                                                           Sora::Compute::MemoryProperty::DeviceLocal},
                .bindings = {binding({0}, *kAffineSecond)},
            };

            constexpr Sora::Compute::BufferDesc baselineDesc{.id = {0xB034, 1},
                                                             .elementType = kFloat,
                                                             .layout = Sora::Compute::LayoutKind::Contiguous,
                                                             .rank = 1,
                                                             .sizeBytes = 4096,
                                                             .alignment = 64};
            if (!baselineResidency_.Track(baselineDesc, 1)) {
                failed_ = true;
                return;
            }
            baselineReplica_.replicas = {
                {.buffer = baselineDesc.id,
                 .region = *Sora::Compute::ByteRegion::Make(0, baselineDesc.sizeBytes),
                 .version = 1,
                 .producer = std::nullopt,
                 .replica = {.service = kDestinationService, .location = kDestinationLocation, .materializer = {1}}}};

            const Sora::Compute::PlannedAllocationId assemblyAllocation{
                static_cast<uint32_t>(prefix_.allocations.size())};
            const Sora::Compute::PhysicalPlanSegmentId firstTransfer{static_cast<uint32_t>(prefix_.segments.size())};
            const Sora::Compute::PhysicalPlanSegmentId secondTransfer{firstTransfer.index + 1};
            const Sora::Compute::PhysicalPlanSegmentId assemblySegment{firstTransfer.index + 2};
            prefix_.allocations.push_back({.id = assemblyAllocation,
                                           .purpose = Sora::Compute::PlannedAllocationPurpose::DispatchInputAssembly,
                                           .buffer = affineBuffer_,
                                           .service = kDestinationService,
                                           .location = kDestinationLocation,
                                           .bufferRegion = kAffineDomain->BoundingRegion(),
                                           .byteFootprint = *kAffineDomain,
                                           .version = 0,
                                           .materialization = Sora::Compute::PlannedAllocate{.alignment = 64}});
            const auto transfer = [&](const Sora::Compute::AffineByteFootprint& footprint) {
                return Sora::Compute::PlannedTransfer{.logicalNode = {1},
                                                      .service = kDestinationService,
                                                      .engine = {30, 31},
                                                      .buffer = affineBuffer_.Describe().id,
                                                      .region = footprint.BoundingRegion(),
                                                      .byteFootprint = footprint,
                                                      .version = 1,
                                                      .destination = assemblyAllocation,
                                                      .target = Sora::Compute::PlannedTransferTarget::AssemblyPart};
            };
            prefix_.segments.emplace_back(transfer(*kAffineFirst));
            prefix_.segments.emplace_back(transfer(*kAffineSecond));
            prefix_.segments.emplace_back(
                Sora::Compute::PlannedAssembly{.logicalNode = {1},
                                               .destination = assemblyAllocation,
                                               .parts = {{.transferProducer = firstTransfer,
                                                          .region = kAffineFirst->BoundingRegion(),
                                                          .byteFootprint = *kAffineFirst},
                                                         {.transferProducer = secondTransfer,
                                                          .region = kAffineSecond->BoundingRegion(),
                                                          .byteFootprint = *kAffineSecond}}});
            prefix_.edges.push_back({firstTransfer, assemblySegment});
            prefix_.edges.push_back({secondTransfer, assemblySegment});
            assemblySource_ = {
                .buffer = affineBuffer_.Describe().id,
                .coverage = kAffineFirst->BoundingRegion(),
                .byteFootprint = *kAffineFirst,
                .version = 1,
                .producer = Sora::Compute::GraphNodeId{0},
                .replica = {.service = kDestinationService, .location = kDestinationLocation, .materializer = {1}}};
            if (!graphLocal_.Bind({.symbolic = assemblySource_,
                                   .physical = {.allocation = assemblyAllocation, .readyAfter = assemblySegment}})) {
                failed_ = true;
                return;
            }
            logicalResolveRoute_ = {.buffer = selected_.buffer,
                                    .source = selected_,
                                    .destinationService = kSourceService,
                                    .destination = kSourceLocation,
                                    .region = selected_.coverage,
                                    .version = selected_.version};
            assemblyResolveRoute_ = {.buffer = assemblySource_.buffer,
                                     .source = assemblySource_,
                                     .destinationService = kDestinationService,
                                     .destination = kDestinationLocation,
                                     .region = assemblySource_.coverage,
                                     .byteFootprint = assemblySource_.byteFootprint,
                                     .version = assemblySource_.version};
        }

        void LookupSegments() noexcept {
            const auto segments = residency_.Segments(selected_.buffer);
            checksum_ += segments.size() + (segments.empty() ? 0 : segments.front().replicas.size());
        }

        void LookupGraphLocalStorage() noexcept {
            const auto* storage = graphLocal_.Find(selected_);
            checksum_ += storage == nullptr ? 0 : storage->allocation.index + 1;
        }

        void LookupAffineSegments() noexcept {
            const auto segments = affineResidency_.Segments(affineBuffer_.Describe().id);
            checksum_ += segments.size() + (segments.empty() ? 0 : segments.front().Coverage().OccupiedByteCount());
        }

        void ApplyAffinePartition() noexcept {
            const auto applied = affineResidency_.Apply(affineUpdate_);
            checksum_ += applied ? affineResidency_.Segments(affineBuffer_.Describe().id).size() : 0;
        }

        void CommitAffinePartitionSelection() noexcept {
            const auto committed = Sora::Compute::CommitPartitionedDispatchResidencySelection(
                {0}, partitionLogicalBindings_, partitionProjections_, partitionCommitResidency_);
            checksum_ += committed ? partitionCommitResidency_.Segments(affineBuffer_.Describe().id).size() : 0;
        }

        void BranchExactAffineReplica() noexcept { BranchAffineReplica(exactAffineReplica_); }

        void BranchPartialAffineReplica() noexcept { BranchAffineReplica(partialAffineReplica_); }

        void BranchBaselineReplica() noexcept {
            try {
                Sora::Compute::GraphResidencyState branch = baselineResidency_;
                const auto applied = branch.Apply(baselineReplica_);
                checksum_ += applied ? branch.Segments({0xB034, 1}).size() : 0;
            } catch (const std::bad_alloc&) {
                failed_ = true;
            }
        }

        void ExpandPhysicalRoute() noexcept {
            const auto fragment = Sora::Compute::PlanPhysicalResidency(
                buffers_[kSelectedBinding], {kSelectedBinding + 1}, route_, prefix_, graphLocal_, {.alignment = 64});
            checksum_ += fragment ? fragment->storage.allocations.size() + fragment->storage.segments.size() +
                                        fragment->storage.edges.size()
                                  : 0;
        }

        void ResolveLogicalGraphLocal() noexcept { ResolveGraphLocal(logicalResolveRoute_); }

        void ResolveAssemblyGraphLocalPart() noexcept { ResolveGraphLocal(assemblyResolveRoute_); }

        [[nodiscard]] bool Failed() const noexcept {
            return failed_ || buffers_.size() != kBindingCount || graphLocal_.Size() != kBindingCount + 1;
        }

        [[nodiscard]] uint64_t Checksum() const noexcept { return checksum_; }

    private:
        [[nodiscard]] static constexpr Sora::Compute::BufferId BufferIdentity(uint32_t index) noexcept {
            return {0xB032, static_cast<uint64_t>(index) + 1};
        }

        [[nodiscard]] static Sora::Compute::GraphLocalResidencySource SymbolicSource(uint32_t index) noexcept {
            return {.buffer = BufferIdentity(index),
                    .coverage = *Sora::Compute::ByteRegion::Make(0, 4096),
                    .version = 1,
                    .producer = Sora::Compute::GraphNodeId{index},
                    .replica = {.service = kSourceService, .location = kSourceLocation, .materializer = {index}}};
        }

        void BranchAffineReplica(const Sora::Compute::GraphResidencyUpdate& update) noexcept {
            try {
                Sora::Compute::GraphResidencyState branch = affineWholeResidency_;
                const auto applied = branch.Apply(update);
                checksum_ += applied ? branch.Segments(affineBuffer_.Describe().id).size() : 0;
            } catch (const std::bad_alloc&) {
                failed_ = true;
            }
        }

        void ResolveGraphLocal(const Sora::Compute::GraphResidencyRoute& route) noexcept {
            const Sora::Compute::Buffer& buffer =
                route.buffer == affineBuffer_.Describe().id ? affineBuffer_ : buffers_[kSelectedBinding];
            const auto fragment =
                Sora::Compute::PlanPhysicalResidency(buffer, {kSelectedBinding + 1}, route, prefix_, graphLocal_);
            checksum_ +=
                fragment ? std::get<Sora::Compute::PlannedAllocationRef>(fragment->destination).allocation.index : 0;
        }

        std::vector<Sora::Compute::Buffer> buffers_{};
        Sora::Compute::GraphResidencyState residency_{};
        Sora::Compute::Buffer affineBuffer_{};
        Sora::Compute::GraphResidencyState affineResidency_{};
        Sora::Compute::GraphResidencyState affineWholeResidency_{};
        Sora::Compute::GraphResidencyUpdate affineUpdate_{};
        Sora::Compute::GraphResidencyUpdate affineWholeUpdate_{};
        Sora::Compute::GraphResidencyUpdate exactAffineReplica_{};
        Sora::Compute::GraphResidencyUpdate partialAffineReplica_{};
        Sora::Compute::GraphResidencyState partitionCommitResidency_{};
        std::array<Sora::Compute::DispatchMemoryBinding, 1> partitionLogicalBindings_{};
        std::array<Sora::Compute::DispatchResidencyProjection, 2> partitionProjections_{};
        Sora::Compute::GraphResidencyState baselineResidency_{};
        Sora::Compute::GraphResidencyUpdate baselineReplica_{};
        Sora::Compute::GraphLocalStorageMap graphLocal_{};
        Sora::Compute::PhysicalGraphPlanStorage prefix_{};
        Sora::Compute::GraphLocalResidencySource selected_{};
        Sora::Compute::GraphLocalResidencySource assemblySource_{};
        Sora::Compute::GraphResidencyRoute route_{};
        Sora::Compute::GraphResidencyRoute logicalResolveRoute_{};
        Sora::Compute::GraphResidencyRoute assemblyResolveRoute_{};
        uint64_t checksum_ = 0;
        bool failed_ = false;
    };

} // namespace

int main() {
    GraphResidencyFixture fixture;
    if (fixture.Failed()) {
        return 2;
    }
    auto segmentLookup = [&fixture]() noexcept { fixture.LookupSegments(); };
    auto graphLocalLookup = [&fixture]() noexcept { fixture.LookupGraphLocalStorage(); };
    auto affineLookup = [&fixture]() noexcept { fixture.LookupAffineSegments(); };
    auto affineApply = [&fixture]() noexcept { fixture.ApplyAffinePartition(); };
    auto partitionCommit = [&fixture]() noexcept { fixture.CommitAffinePartitionSelection(); };
    auto exactAffineReplica = [&fixture]() noexcept { fixture.BranchExactAffineReplica(); };
    auto partialAffineReplica = [&fixture]() noexcept { fixture.BranchPartialAffineReplica(); };
    auto baselineReplica = [&fixture]() noexcept { fixture.BranchBaselineReplica(); };
    auto routeExpansion = [&fixture]() noexcept { fixture.ExpandPhysicalRoute(); };
    auto logicalGraphLocalResolve = [&fixture]() noexcept { fixture.ResolveLogicalGraphLocal(); };
    auto assemblyPartResolve = [&fixture]() noexcept { fixture.ResolveAssemblyGraphLocalPart(); };
    const std::array cases{
        Sora::Benchmark::MakeCase("symbolic_segment_lookup_1024", segmentLookup),
        Sora::Benchmark::MakeCase("graph_local_storage_lookup_1024", graphLocalLookup),
        Sora::Benchmark::MakeCase("symbolic_affine_partition_lookup", affineLookup),
        Sora::Benchmark::MakeCase("symbolic_affine_partition_apply", affineApply),
        Sora::Benchmark::MakeCase("symbolic_affine_partition_commit", partitionCommit),
        Sora::Benchmark::MakeCase("symbolic_exact_affine_replica_branch", exactAffineReplica),
        Sora::Benchmark::MakeCase("symbolic_partial_affine_replica_branch", partialAffineReplica),
        Sora::Benchmark::MakeCase("symbolic_baseline_replica_branch", baselineReplica),
        Sora::Benchmark::MakeCase("symbolic_route_physical_expansion", routeExpansion),
        Sora::Benchmark::MakeCase("logical_graph_local_storage_resolve", logicalGraphLocalResolve),
        Sora::Benchmark::MakeCase("assembly_part_graph_local_storage_resolve", assemblyPartResolve),
    };
    const auto timing = Sora::Benchmark::MeasureInterleavedIndependent<kSamples>(cases, 1, kMinimumSampleTime);
    if (fixture.Checksum() == 0 || fixture.Checksum() == std::numeric_limits<uint64_t>::max()) {
        return 3;
    }

    std::println("metric,value,unit");
    std::println("symbolic_segment_lookup_1024,{:.6f},ns/operation", timing.MedianTime(0));
    std::println("graph_local_storage_lookup_1024,{:.6f},ns/operation", timing.MedianTime(1));
    std::println("symbolic_affine_partition_lookup,{:.6f},ns/operation", timing.MedianTime(2));
    std::println("symbolic_affine_partition_apply,{:.6f},ns/operation", timing.MedianTime(3));
    std::println("symbolic_affine_partition_commit,{:.6f},ns/operation", timing.MedianTime(4));
    std::println("partition_commit_over_apply,{:.6f},ratio", timing.PairedRatio(4, 3));
    std::println("symbolic_exact_affine_replica_branch,{:.6f},ns/operation", timing.MedianTime(5));
    std::println("symbolic_partial_affine_replica_branch,{:.6f},ns/operation", timing.MedianTime(6));
    std::println("partial_over_exact_affine_replica,{:.6f},ratio", timing.PairedRatio(6, 5));
    std::println("symbolic_baseline_replica_branch,{:.6f},ns/operation", timing.MedianTime(7));
    std::println("symbolic_route_physical_expansion,{:.6f},ns/operation", timing.MedianTime(8));
    std::println("logical_graph_local_storage_resolve,{:.6f},ns/operation", timing.MedianTime(9));
    std::println("assembly_part_graph_local_storage_resolve,{:.6f},ns/operation", timing.MedianTime(10));
    std::println("assembly_part_over_logical_resolve,{:.6f},ratio", timing.PairedRatio(10, 9));
    std::println("segment_lookup_repetitions,{},count", timing.repetitions[0]);
    std::println("graph_local_lookup_repetitions,{},count", timing.repetitions[1]);
    std::println("affine_lookup_repetitions,{},count", timing.repetitions[2]);
    std::println("affine_apply_repetitions,{},count", timing.repetitions[3]);
    std::println("partition_commit_repetitions,{},count", timing.repetitions[4]);
    std::println("exact_affine_replica_repetitions,{},count", timing.repetitions[5]);
    std::println("partial_affine_replica_repetitions,{},count", timing.repetitions[6]);
    std::println("baseline_replica_repetitions,{},count", timing.repetitions[7]);
    std::println("route_expansion_repetitions,{},count", timing.repetitions[8]);
    std::println("logical_graph_local_resolve_repetitions,{},count", timing.repetitions[9]);
    std::println("assembly_part_resolve_repetitions,{},count", timing.repetitions[10]);
    std::println("graph_local_residency_segment_size,{},bytes", sizeof(Sora::Compute::GraphResidencySegment));
    std::println("checksum,{},value", fixture.Checksum());
}

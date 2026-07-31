/**
 * @file GraphResidency.cpp
 * @brief Attempt-frozen ready residency and transactional symbolic interval operations.
 * @ingroup Compute
 */

#include <Sora/Compute/Runtime/GraphResidency.h>

#include <Sora/Compute/Graph/ExecutionGraph.h>

#include <algorithm>
#include <array>
#include <concepts>
#include <iterator>
#include <limits>
#include <new>
#include <ranges>
#include <tuple>

namespace Sora::Compute {
    namespace {

        constexpr size_t kGraphResidencyCoverageCapacity = 256;
        // One axis-aligned box subtraction creates at most two residual boxes per axis.
        constexpr size_t kAffineReplicaFragmentCapacity = 2 * kMaximumBufferRank;

        template<typename BufferRange>
        [[nodiscard]] auto FindBaseline(BufferRange& buffers, BufferId id) noexcept {
            return std::ranges::lower_bound(buffers, id, {},
                                            [](const auto& buffer) { return buffer.snapshot.desc.id; });
        }

        template<typename BufferRange>
        [[nodiscard]] auto FindGraphLocal(BufferRange& buffers, BufferId id) noexcept {
            return std::ranges::lower_bound(buffers, id, {}, [](const auto& buffer) { return buffer.id; });
        }

        [[nodiscard]] bool ValidCoverage(const BufferResidencySnapshot& snapshot, ByteRegion region,
                                         ByteCoverageView coverage) noexcept {
            return coverage.Valid() && coverage.BoundingRegion() == region &&
                   snapshot.LogicalCoverage().Contains(coverage) == CoverageContainment::Yes;
        }

        [[nodiscard]] bool SameState(const GraphResidencySegment& left, const GraphResidencySegment& right) noexcept {
            return left.buffer == right.buffer && left.version == right.version && left.producer == right.producer &&
                   left.byteFootprint == right.byteFootprint && left.replicas == right.replicas;
        }

        void Canonicalize(std::vector<GraphResidencySegment>& segments) {
            size_t output = 0;
            for (GraphResidencySegment& segment : segments) {
                std::ranges::sort(segment.replicas);
                const auto unique = std::ranges::unique(segment.replicas);
                segment.replicas.erase(unique.begin(), unique.end());
                if (output != 0 && !segments[output - 1].byteFootprint && !segment.byteFootprint &&
                    segments[output - 1].region.End() == segment.region.Offset() &&
                    SameState(segments[output - 1], segment)) {
                    const uint64_t size = segment.region.End() - segments[output - 1].region.Offset();
                    segments[output - 1].region = *ByteRegion::Make(segments[output - 1].region.Offset(), size);
                    continue;
                }
                if (output != static_cast<size_t>(&segment - segments.data())) {
                    segments[output] = std::move(segment);
                }
                ++output;
            }
            segments.resize(output);
        }

        void SplitAt(std::vector<GraphResidencySegment>& segments, uint64_t offset) {
            const auto found = std::ranges::find_if(segments, [offset](const GraphResidencySegment& segment) {
                return !segment.byteFootprint && segment.region.Offset() < offset && offset < segment.region.End();
            });
            if (found == segments.end()) {
                return;
            }
            GraphResidencySegment right = *found;
            right.region = *ByteRegion::Make(offset, found->region.End() - offset);
            found->region = *ByteRegion::Make(found->region.Offset(), offset - found->region.Offset());
            segments.insert(std::next(found), std::move(right));
        }

        [[nodiscard]] bool SameLogicalValue(const GraphResidencySegment& segment,
                                            const GraphResidencyReplicaAddition& addition) noexcept {
            return segment.buffer == addition.buffer && segment.version == addition.version &&
                   segment.producer == addition.producer;
        }

        [[nodiscard]] bool FullyCoveredIntervalByValue(std::span<const GraphResidencySegment> segments,
                                                       const GraphResidencyReplicaAddition& addition) noexcept {
            uint64_t cursor = addition.region.Offset();
            for (const GraphResidencySegment& segment : segments) {
                if (segment.byteFootprint) {
                    continue;
                }
                if (segment.region.End() <= cursor) {
                    continue;
                }
                if (segment.region.Offset() > cursor || !SameLogicalValue(segment, addition)) {
                    return false;
                }
                cursor = std::min(segment.region.End(), addition.region.End());
                if (cursor == addition.region.End()) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] constexpr GraphResidencyError MapAffineCoverageError(AffineByteFootprintError error) noexcept {
            return error == AffineByteFootprintError::CapacityExceeded ? GraphResidencyError::CapacityExceeded
                                                                       : GraphResidencyError::IndeterminateCoverage;
        }

        [[nodiscard]] std::expected<void, GraphResidencyError>
        ValidateAffinePartition(ByteCoverageView domain, std::span<const ByteCoverageView> parts) noexcept {
            for (size_t index = 0; index < parts.size(); ++index) {
                if (domain.Contains(parts[index]) != CoverageContainment::Yes) {
                    return std::unexpected(GraphResidencyError::IndeterminateCoverage);
                }
                for (size_t prior = 0; prior < index; ++prior) {
                    if (CoverageRelationWithin(domain, parts[prior], parts[index]) != CoverageOverlap::Disjoint) {
                        return std::unexpected(GraphResidencyError::IndeterminateCoverage);
                    }
                }
            }
            if (ExactCoverageUnion<kAffineReplicaFragmentCapacity + 1>(domain, parts) != CoverageEquivalence::Equal) {
                return std::unexpected(GraphResidencyError::IndeterminateCoverage);
            }
            return {};
        }

        template<typename GraphLocalBuffer>
        [[nodiscard]] std::expected<void, GraphResidencyError>
        AddAffineReplica(GraphLocalBuffer& buffer, const GraphResidencyReplicaAddition& addition) {
            const ByteCoverageView additionCoverage = addition.Coverage();
            size_t containingIndex = buffer.segments.size();
            bool indeterminate = false;
            for (size_t index = 0; index < buffer.segments.size(); ++index) {
                const GraphResidencySegment& segment = buffer.segments[index];
                if (!SameLogicalValue(segment, addition)) {
                    continue;
                }
                const CoverageContainment containment = segment.Coverage().Contains(additionCoverage);
                if (containment == CoverageContainment::Yes) {
                    if (containingIndex != buffer.segments.size()) {
                        return std::unexpected(GraphResidencyError::IndeterminateCoverage);
                    }
                    containingIndex = index;
                } else {
                    indeterminate |= containment == CoverageContainment::Unknown;
                }
            }
            if (containingIndex == buffer.segments.size()) {
                return std::unexpected(indeterminate ? GraphResidencyError::IndeterminateCoverage
                                                     : GraphResidencyError::MissingLogicalValue);
            }

            const GraphResidencySegment& containing = buffer.segments[containingIndex];
            if (!containing.byteFootprint) {
                return std::unexpected(GraphResidencyError::IndeterminateCoverage);
            }
            const std::array removed{additionCoverage};
            auto complement =
                AffineCoverageComplement<kAffineReplicaFragmentCapacity>::Create(containing.Coverage(), removed);
            if (!complement) {
                return std::unexpected(MapAffineCoverageError(complement.error()));
            }

            const size_t replacementCount = complement->Size() + 1;
            if (replacementCount > kGraphResidencyCoverageCapacity ||
                buffer.segments.size() - 1 > kGraphResidencyCoverageCapacity - replacementCount) {
                return std::unexpected(GraphResidencyError::CapacityExceeded);
            }
            std::array<ByteCoverageView, kAffineReplicaFragmentCapacity + 1> partition{};
            partition[0] = additionCoverage;
            size_t partitionCount = 1;
            for (const AffineByteFootprint& residual : complement->Footprints()) {
                partition[partitionCount++] = ByteCoverageView::Affine(residual);
            }
            if (auto validation = ValidateAffinePartition(containing.Coverage(), {partition.data(), partitionCount});
                !validation) {
                return std::unexpected(validation.error());
            }

            std::vector<GraphResidencySegment> replacement;
            replacement.reserve(buffer.segments.size() - 1 + replacementCount);
            for (size_t index = 0; index < buffer.segments.size(); ++index) {
                if (index != containingIndex) {
                    replacement.push_back(buffer.segments[index]);
                    continue;
                }
                for (const AffineByteFootprint& residual : complement->Footprints()) {
                    auto fragment = containing;
                    fragment.region = residual.BoundingRegion();
                    fragment.byteFootprint = residual;
                    replacement.push_back(std::move(fragment));
                }
                auto requested = containing;
                requested.region = addition.region;
                requested.byteFootprint = addition.byteFootprint;
                requested.replicas.push_back(addition.replica);
                replacement.push_back(std::move(requested));
            }
            std::ranges::sort(replacement, [](const GraphResidencySegment& left, const GraphResidencySegment& right) {
                return std::tuple{left.region.Offset(), left.region.Size()} <
                       std::tuple{right.region.Offset(), right.region.Size()};
            });
            Canonicalize(replacement);
            buffer.segments = std::move(replacement);
            return {};
        }

        template<typename GraphLocalBuffer>
        [[nodiscard]] std::expected<void, GraphResidencyError>
        InsertBaselineReplica(GraphLocalBuffer& buffer, const BufferResidencySnapshot& snapshot,
                              const GraphResidencyReplicaAddition& addition) {
            if (buffer.segments.size() == kGraphResidencyCoverageCapacity) {
                return std::unexpected(GraphResidencyError::CapacityExceeded);
            }
            const ByteCoverageView logical = snapshot.LogicalCoverage();
            const ByteCoverageView added = addition.Coverage();
            for (const GraphResidencySegment& segment : buffer.segments) {
                const CoverageOverlap relation = CoverageRelationWithin(logical, segment.Coverage(), added);
                if (relation == CoverageOverlap::Unknown) {
                    return std::unexpected(GraphResidencyError::IndeterminateCoverage);
                }
                if (relation == CoverageOverlap::Overlap) {
                    return std::unexpected(GraphResidencyError::MissingLogicalValue);
                }
            }
            buffer.segments.push_back({.buffer = addition.buffer,
                                       .region = addition.region,
                                       .byteFootprint = addition.byteFootprint,
                                       .version = addition.version,
                                       .producer = std::nullopt,
                                       .replicas = {addition.replica}});
            std::ranges::sort(buffer.segments,
                              [](const GraphResidencySegment& left, const GraphResidencySegment& right) {
                                  return std::tuple{left.region.Offset(), left.region.Size()} <
                                         std::tuple{right.region.Offset(), right.region.Size()};
                              });
            Canonicalize(buffer.segments);
            return {};
        }

        template<typename GraphLocalBuffer>
        [[nodiscard]] std::expected<void, GraphResidencyError>
        AddReplica(GraphLocalBuffer& buffer, const BufferResidencySnapshot& snapshot,
                   const GraphResidencyReplicaAddition& addition) {
            const ByteCoverageView additionCoverage = addition.Coverage();
            if (!ValidCoverage(snapshot, addition.region, additionCoverage) || addition.version == 0 ||
                (addition.producer &&
                 (!addition.producer->Valid() || addition.replica.materializer.index < addition.producer->index)) ||
                (!addition.producer && addition.version != snapshot.version) || !addition.replica.Valid()) {
                return std::unexpected(GraphResidencyError::MissingLogicalValue);
            }
            if (addition.byteFootprint) {
                const auto exact = std::ranges::find_if(buffer.segments, [&](const GraphResidencySegment& segment) {
                    return SameLogicalValue(segment, addition) && segment.Coverage() == additionCoverage;
                });
                if (exact != buffer.segments.end()) {
                    exact->replicas.push_back(addition.replica);
                    Canonicalize(buffer.segments);
                    return {};
                }
                auto added = AddAffineReplica(buffer, addition);
                if (added || addition.producer || added.error() != GraphResidencyError::MissingLogicalValue) {
                    return added;
                }
                return InsertBaselineReplica(buffer, snapshot, addition);
            }
            if (!FullyCoveredIntervalByValue(std::span<const GraphResidencySegment>{buffer.segments}, addition)) {
                return addition.producer ? std::unexpected(GraphResidencyError::MissingLogicalValue)
                                         : InsertBaselineReplica(buffer, snapshot, addition);
            }
            SplitAt(buffer.segments, addition.region.Offset());
            SplitAt(buffer.segments, addition.region.End());
            for (GraphResidencySegment& segment : buffer.segments) {
                if (addition.region.Contains(segment.region)) {
                    segment.replicas.push_back(addition.replica);
                }
            }
            Canonicalize(buffer.segments);
            return {};
        }

        template<typename GraphLocalBuffer>
        void OverwriteInterval(GraphLocalBuffer& buffer, const ProducedResidencyWrite& write) {
            std::vector<GraphResidencySegment> replacement;
            replacement.reserve(buffer.segments.size() + 1);
            for (const GraphResidencySegment& segment : buffer.segments) {
                if (segment.byteFootprint) {
                    replacement.push_back(segment);
                    continue;
                }
                if (!segment.region.Intersects(write.region)) {
                    replacement.push_back(segment);
                    continue;
                }
                if (segment.region.Offset() < write.region.Offset()) {
                    auto left = segment;
                    left.region =
                        *ByteRegion::Make(segment.region.Offset(), write.region.Offset() - segment.region.Offset());
                    replacement.push_back(std::move(left));
                }
                if (write.region.End() < segment.region.End()) {
                    auto right = segment;
                    right.region = *ByteRegion::Make(write.region.End(), segment.region.End() - write.region.End());
                    replacement.push_back(std::move(right));
                }
            }
            replacement.push_back({.buffer = write.buffer,
                                   .region = write.region,
                                   .byteFootprint = write.byteFootprint,
                                   .version = write.version,
                                   .producer = write.producer,
                                   .replicas = {write.replica}});
            std::ranges::sort(replacement, {},
                              [](const GraphResidencySegment& segment) { return segment.region.Offset(); });
            Canonicalize(replacement);
            buffer.segments = std::move(replacement);
        }

        template<typename GraphLocalBuffer>
        [[nodiscard]] std::expected<void, GraphResidencyError>
        OverwriteExact(GraphLocalBuffer& buffer, ByteCoverageView domain, const ProducedResidencyWrite& write) {
            std::vector<GraphResidencySegment> replacement;
            replacement.reserve(buffer.segments.size() + 1);
            const ByteCoverageView written = write.Coverage();
            for (const GraphResidencySegment& segment : buffer.segments) {
                const ByteCoverageView current = segment.Coverage();
                const CoverageOverlap relation = CoverageRelationWithin(domain, current, written);
                if (relation == CoverageOverlap::Disjoint) {
                    replacement.push_back(segment);
                    continue;
                }
                if (relation == CoverageOverlap::Unknown || written.Contains(current) != CoverageContainment::Yes) {
                    return std::unexpected(GraphResidencyError::IndeterminateCoverage);
                }
            }
            replacement.push_back({.buffer = write.buffer,
                                   .region = write.region,
                                   .byteFootprint = write.byteFootprint,
                                   .version = write.version,
                                   .producer = write.producer,
                                   .replicas = {write.replica}});
            std::ranges::sort(replacement, [](const GraphResidencySegment& left, const GraphResidencySegment& right) {
                return std::tuple{left.region.Offset(), left.region.Size()} <
                       std::tuple{right.region.Offset(), right.region.Size()};
            });
            Canonicalize(replacement);
            buffer.segments = std::move(replacement);
            return {};
        }

        struct GraphBuffer {
            GraphNodeId node{};
            Buffer buffer{};
        };

        void CollectBuffer(std::vector<GraphBuffer>& buffers, GraphNodeId node, const Buffer& buffer) {
            if (buffer) {
                buffers.push_back({node, buffer});
            }
        }

        void CollectOperationBuffers(std::vector<GraphBuffer>& buffers, GraphNodeId node,
                                     const GraphOperation& operation) {
            std::visit(
                [&]<typename Operation>(const Operation& value) {
                    if constexpr (std::same_as<Operation, KernelInvocation>) {
                        for (const KernelArgumentBinding& binding : value.Bindings()) {
                            if (const auto* view = std::get_if<BufferView>(&binding)) {
                                CollectBuffer(buffers, node, view->LogicalBuffer());
                            }
                        }
                    } else if constexpr (std::same_as<Operation, CopyOperation>) {
                        CollectBuffer(buffers, node, value.Source().LogicalBuffer());
                        CollectBuffer(buffers, node, value.Destination().LogicalBuffer());
                    } else if constexpr (std::same_as<Operation, FillOperation>) {
                        CollectBuffer(buffers, node, value.Destination().LogicalBuffer());
                    } else {
                        for (const ExternalResourceBinding& resource : value.Resources()) {
                            CollectBuffer(buffers, node, resource.view.LogicalBuffer());
                        }
                    }
                },
                operation);
        }

    } // namespace

    std::expected<void, GraphResidencyError> GraphResidencyState::Track(BufferDesc desc,
                                                                        uint64_t baseVersion) noexcept {
        if (!desc.Valid()) {
            return std::unexpected(GraphResidencyError::InvalidRequest);
        }
        return Track({}, {.desc = desc, .version = baseVersion});
    }

    std::expected<void, GraphResidencyError> GraphResidencyState::Track(Buffer buffer) noexcept {
        if (!buffer) {
            return std::unexpected(GraphResidencyError::InvalidRequest);
        }
        auto snapshot = buffer.SnapshotResidency();
        if (!snapshot) {
            return std::unexpected(snapshot.error() == MemoryError::OutOfMemory ? GraphResidencyError::OutOfMemory
                                                                                : GraphResidencyError::InvalidRequest);
        }
        return Track(std::move(buffer), std::move(*snapshot));
    }

    std::expected<void, GraphResidencyError> GraphResidencyState::Track(Buffer buffer,
                                                                        BufferResidencySnapshot snapshot) noexcept {
        if (!snapshot.desc.Valid() || (buffer && buffer.Describe() != snapshot.desc)) {
            return std::unexpected(GraphResidencyError::InvalidRequest);
        }
        try {
            if (!baseline_) {
                baseline_ = std::make_shared<BaselineStorage>();
            } else if (baseline_.use_count() != 1) {
                baseline_ = std::make_shared<BaselineStorage>(*baseline_);
            }
            auto found = FindBaseline(baseline_->buffers, snapshot.desc.id);
            if (found != baseline_->buffers.end() && found->snapshot.desc.id == snapshot.desc.id) {
                const bool sameOwner = (!found->buffer && !buffer) ||
                                       (found->buffer && buffer && found->buffer.Weak().SharesOwnership(buffer.Weak()));
                return found->snapshot.desc == snapshot.desc && found->snapshot.version == snapshot.version && sameOwner
                           ? std::expected<void, GraphResidencyError>{}
                           : std::unexpected(GraphResidencyError::BaseVersionMismatch);
            }
            baseline_->buffers.insert(found, {.buffer = std::move(buffer), .snapshot = std::move(snapshot)});
            return {};
        } catch (const std::bad_alloc&) {
            return std::unexpected(GraphResidencyError::OutOfMemory);
        }
    }

    std::optional<uint64_t> GraphResidencyState::BaseVersion(BufferId buffer) const noexcept {
        const auto* snapshot = ReadySnapshot(buffer);
        return snapshot != nullptr ? std::optional{snapshot->version} : std::nullopt;
    }

    const BufferResidencySnapshot* GraphResidencyState::ReadySnapshot(BufferId buffer) const noexcept {
        if (!baseline_) {
            return nullptr;
        }
        const auto found = FindBaseline(baseline_->buffers, buffer);
        return found != baseline_->buffers.end() && found->snapshot.desc.id == buffer ? std::addressof(found->snapshot)
                                                                                      : nullptr;
    }

    bool GraphResidencyState::SharesBufferOwnership(const Buffer& buffer) const noexcept {
        if (!buffer || !baseline_) {
            return false;
        }
        const auto found = FindBaseline(baseline_->buffers, buffer.Describe().id);
        return found != baseline_->buffers.end() && found->snapshot.desc.id == buffer.Describe().id && found->buffer &&
               found->buffer.Weak().SharesOwnership(buffer.Weak());
    }

    std::span<const GraphResidencySegment> GraphResidencyState::Segments(BufferId buffer) const noexcept {
        const auto found = FindGraphLocal(graphLocal_, buffer);
        return found != graphLocal_.end() && found->id == buffer
                   ? std::span<const GraphResidencySegment>{found->segments}
                   : std::span<const GraphResidencySegment>{};
    }

    bool GraphResidencyState::SamePlanningState(const GraphResidencyState& other) const noexcept {
        return baseline_ && baseline_ == other.baseline_ && graphLocal_ == other.graphLocal_;
    }

    std::expected<void, GraphResidencyError> GraphResidencyState::ApplyTo(std::vector<GraphLocalBuffer>& candidate,
                                                                          const GraphResidencyUpdate& update) {
        for (const GraphResidencyReplicaAddition& addition : update.replicas) {
            if (!baseline_) {
                return std::unexpected(GraphResidencyError::MissingBuffer);
            }
            const auto baseline = FindBaseline(baseline_->buffers, addition.buffer);
            if (baseline == baseline_->buffers.end() || baseline->snapshot.desc.id != addition.buffer) {
                return std::unexpected(GraphResidencyError::MissingBuffer);
            }
            auto buffer = FindGraphLocal(candidate, addition.buffer);
            if (buffer == candidate.end() || buffer->id != addition.buffer) {
                buffer = candidate.insert(buffer, GraphLocalBuffer{.id = addition.buffer});
            }
            if (auto added = AddReplica(*buffer, baseline->snapshot, addition); !added) {
                return std::unexpected(added.error());
            }
        }
        for (size_t index = 0; index < update.writes.size(); ++index) {
            const ProducedResidencyWrite& write = update.writes[index];
            if (!baseline_) {
                return std::unexpected(GraphResidencyError::MissingBuffer);
            }
            const auto baseline = FindBaseline(baseline_->buffers, write.buffer);
            if (baseline == baseline_->buffers.end() || baseline->snapshot.desc.id != write.buffer) {
                return std::unexpected(GraphResidencyError::MissingBuffer);
            }
            if (baseline->snapshot.version == std::numeric_limits<uint64_t>::max()) {
                return std::unexpected(GraphResidencyError::VersionExhausted);
            }
            if (!ValidCoverage(baseline->snapshot, write.region, write.Coverage()) ||
                write.version != baseline->snapshot.version + 1 || !write.producer.Valid() || !write.replica.Valid() ||
                write.replica.materializer != write.producer) {
                return std::unexpected(GraphResidencyError::InvalidRequest);
            }
        }

        for (size_t first = 0; first < update.writes.size(); ++first) {
            const BufferId id = update.writes[first].buffer;
            const bool processed =
                std::ranges::any_of(update.writes.begin(), update.writes.begin() + first,
                                    [id](const ProducedResidencyWrite& write) { return write.buffer == id; });
            if (processed) {
                continue;
            }

            const auto baseline = FindBaseline(baseline_->buffers, id);
            const ByteCoverageView logical = baseline->snapshot.LogicalCoverage();
            std::array<ByteCoverageView, kGraphResidencyCoverageCapacity> coverages{};
            size_t coverageCount = 0;
            bool indeterminate = false;
            const GraphNodeId producer = update.writes[first].producer;
            for (size_t index = first; index < update.writes.size(); ++index) {
                if (update.writes[index].buffer != id) {
                    continue;
                }
                if (update.writes[index].producer != producer) {
                    return std::unexpected(GraphResidencyError::InvalidRequest);
                }
                if (coverageCount == kGraphResidencyCoverageCapacity) {
                    return std::unexpected(GraphResidencyError::CapacityExceeded);
                }
                const ByteCoverageView coverage = update.writes[index].Coverage();
                for (size_t prior = 0; prior < coverageCount; ++prior) {
                    const CoverageOverlap relation = CoverageRelationWithin(logical, coverages[prior], coverage);
                    if (relation == CoverageOverlap::Overlap) {
                        return std::unexpected(GraphResidencyError::OverlappingWrites);
                    }
                    indeterminate |= relation == CoverageOverlap::Unknown;
                }
                coverages[coverageCount++] = coverage;
            }

            const CoverageEquivalence complete =
                ExactCoverageUnion<kGraphResidencyCoverageCapacity>(logical, {coverages.data(), coverageCount});
            if (complete == CoverageEquivalence::Unknown || (complete != CoverageEquivalence::Equal && indeterminate)) {
                return std::unexpected(GraphResidencyError::IndeterminateCoverage);
            }

            auto buffer = FindGraphLocal(candidate, id);
            if (buffer == candidate.end() || buffer->id != id) {
                buffer = candidate.insert(buffer, GraphLocalBuffer{.id = id});
            }
            if (complete == CoverageEquivalence::Equal) {
                buffer->segments.clear();
                buffer->segments.reserve(coverageCount);
                for (const ProducedResidencyWrite& write : update.writes) {
                    if (write.buffer == id) {
                        buffer->segments.push_back({.buffer = write.buffer,
                                                    .region = write.region,
                                                    .byteFootprint = write.byteFootprint,
                                                    .version = write.version,
                                                    .producer = write.producer,
                                                    .replicas = {write.replica}});
                    }
                }
                std::ranges::sort(buffer->segments,
                                  [](const GraphResidencySegment& left, const GraphResidencySegment& right) {
                                      return std::tuple{left.region.Offset(), left.region.Size()} <
                                             std::tuple{right.region.Offset(), right.region.Size()};
                                  });
                Canonicalize(buffer->segments);
                continue;
            }

            for (const ProducedResidencyWrite& write : update.writes) {
                if (write.buffer != id) {
                    continue;
                }
                const bool intervalState =
                    !write.byteFootprint &&
                    std::ranges::none_of(buffer->segments, [](const GraphResidencySegment& segment) {
                        return segment.byteFootprint.has_value();
                    });
                if (intervalState) {
                    OverwriteInterval(*buffer, write);
                } else if (auto overwritten = OverwriteExact(*buffer, logical, write); !overwritten) {
                    return std::unexpected(overwritten.error());
                }
            }
        }
        return {};
    }

    std::expected<void, GraphResidencyError> GraphResidencyState::Apply(const GraphResidencyUpdate& update) noexcept {
        if (update.replicas.empty() && update.writes.empty()) {
            return {};
        }
        try {
            auto candidate = graphLocal_;
            if (auto applied = ApplyTo(candidate, update); !applied) {
                return std::unexpected(applied.error());
            }
            graphLocal_.swap(candidate);
            return {};
        } catch (const std::bad_alloc&) {
            return std::unexpected(GraphResidencyError::OutOfMemory);
        }
    }

    std::expected<GraphResidencyState, GraphResidencyError>
    GraphResidencyState::Transition(const GraphResidencyUpdate& update) && noexcept {
        if (update.replicas.empty() && update.writes.empty()) {
            return std::move(*this);
        }
        try {
            if (auto applied = ApplyTo(graphLocal_, update); !applied) {
                return std::unexpected(applied.error());
            }
            return std::move(*this);
        } catch (const std::bad_alloc&) {
            return std::unexpected(GraphResidencyError::OutOfMemory);
        }
    }

    void GraphResidencyState::DiscardProducedBefore(GraphNodeId firstLiveProducer) noexcept {
        if (!firstLiveProducer.Valid()) {
            return;
        }
        for (GraphLocalBuffer& buffer : graphLocal_) {
            std::erase_if(buffer.segments, [&](const GraphResidencySegment& segment) {
                return segment.producer && *segment.producer < firstLiveProducer;
            });
        }
        std::erase_if(graphLocal_, [](const GraphLocalBuffer& buffer) { return buffer.segments.empty(); });
    }

    void GraphResidencyState::DiscardReadDeadBuffers(GraphNodeId completedReader,
                                                     std::span<const GraphBufferReadLifetime> lifetimes) noexcept {
        if (!completedReader.Valid()) {
            return;
        }
        std::erase_if(graphLocal_, [&](const GraphLocalBuffer& buffer) {
            const auto lifetime = std::ranges::lower_bound(
                lifetimes, buffer.id, {}, [](const GraphBufferReadLifetime& value) { return value.buffer; });
            return lifetime == lifetimes.end() || lifetime->buffer != buffer.id ||
                   lifetime->lastReader <= completedReader;
        });
    }

    std::expected<GraphResidencyState, GraphResidencySnapshotError>
    FreezeGraphResidency(const ExecutionGraph& graph) noexcept {
        if (!ValidateGraphStructure(graph.View())) {
            return std::unexpected(GraphResidencySnapshotError{GraphResidencySnapshotErrorCode::InvalidGraph});
        }
        try {
            std::vector<GraphBuffer> buffers;
            buffers.reserve(graph.AccessCount());
            for (uint32_t index = 0; index < graph.NodeCount(); ++index) {
                CollectOperationBuffers(buffers, {index}, graph.Operation({index}));
            }
            std::ranges::sort(buffers, {}, [](const GraphBuffer& value) { return value.buffer.Describe().id; });

            GraphResidencyState result;
            BufferId previous{};
            Buffer previousBuffer;
            for (const GraphBuffer& entry : buffers) {
                const BufferId id = entry.buffer.Describe().id;
                if (id == previous) {
                    if (!previousBuffer.Weak().SharesOwnership(entry.buffer.Weak())) {
                        return std::unexpected(GraphResidencySnapshotError{
                            GraphResidencySnapshotErrorCode::ConflictingBufferIdentity, entry.node, id});
                    }
                    continue;
                }
                if (auto tracked = result.Track(entry.buffer); !tracked) {
                    return std::unexpected(
                        GraphResidencySnapshotError{tracked.error() == GraphResidencyError::OutOfMemory
                                                        ? GraphResidencySnapshotErrorCode::OutOfMemory
                                                        : GraphResidencySnapshotErrorCode::SnapshotFailed,
                                                    entry.node, id, static_cast<uint64_t>(tracked.error())});
                }
                previous = id;
                previousBuffer = entry.buffer;
            }
            return result;
        } catch (const std::bad_alloc&) {
            return std::unexpected(GraphResidencySnapshotError{GraphResidencySnapshotErrorCode::OutOfMemory});
        }
    }

} // namespace Sora::Compute

/**
 * @file DispatchCandidateEnumeration.cpp
 * @brief Exact residency-aware candidate enumeration for logical dispatch
 * nodes.
 * @ingroup Compute
 */

#include <Sora/Compute/Runtime/DispatchCandidateEnumeration.h>

#include "DispatchInputAssembly.h"

#include <Sora/Compute/Runtime/CalibratedTransferTopology.h>
#include <Sora/Compute/Runtime/PipelineBindingLayout.h>

#include <algorithm>
#include <array>
#include <bit>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <ranges>
#include <tuple>
#include <variant>

namespace Sora::Compute {
    namespace {

        struct ExactRegion {
            ByteRegion region{};
            std::optional<AffineByteFootprint> byteFootprint{};

            [[nodiscard]] ByteCoverageView Coverage() const noexcept {
                return byteFootprint ? ByteCoverageView::Affine(*byteFootprint) : ByteCoverageView::Interval(region);
            }
        };

        struct BufferRegions {
            Buffer buffer{};
            std::vector<ByteRegion> touched{};
            std::vector<ExactRegion> readable{};
            const BufferResidencySnapshot* snapshot = nullptr;
        };

        struct DeviceDestination {
            BackendServiceId service{};
            DeviceId device{};
            std::optional<PhysicalMemoryDescriptor> destination{};
        };

        [[nodiscard]] constexpr bool Readable(AccessMode access) noexcept {
            return access == AccessMode::Read || access == AccessMode::ReadWrite;
        }

        [[nodiscard]] constexpr bool Writable(AccessMode access) noexcept {
            return access == AccessMode::Write || access == AccessMode::ReadWrite;
        }

        [[nodiscard]] ByteCoverageView BindingCoverage(const DispatchMemoryBinding& binding) noexcept {
            return binding.byteFootprint ? ByteCoverageView::Affine(*binding.byteFootprint)
                                         : ByteCoverageView::Interval(binding.region);
        }

        [[nodiscard]] constexpr bool TargetMatches(const RuntimeDeviceBinding& binding) noexcept {
            return binding.service.Valid() && ValidDeviceDescriptor(binding.descriptor) && binding.target.Valid() &&
                   binding.target.device == binding.descriptor.id &&
                   binding.target.backend == binding.descriptor.backend &&
                   binding.target.features == binding.descriptor.features &&
                   binding.target.architectureId == binding.descriptor.architectureId;
        }

        [[nodiscard]] constexpr bool ValidPublication(DestinationPublication publication) noexcept {
            return publication == DestinationPublication::GraphLocal || publication == DestinationPublication::Publish;
        }

        [[nodiscard]] bool ValidDispatchMemoryBinding(const DispatchMemoryBinding& binding) noexcept {
            if (!binding.argument.Valid() || !ValidAccessMode(binding.access) || !binding.buffer ||
                !std::has_single_bit(binding.alignment) || !ValidPublication(binding.publication)) {
                return false;
            }
            const ByteCoverageView coverage = BindingCoverage(binding);
            return coverage.Valid() && coverage.BoundingRegion() == binding.region &&
                   binding.buffer.LogicalCoverage().Contains(coverage) == CoverageContainment::Yes;
        }

        [[nodiscard]] bool SameBindingSchema(const DispatchMemoryBinding& left,
                                             const DispatchMemoryBinding& right) noexcept {
            return ValidDispatchMemoryBinding(left) && ValidDispatchMemoryBinding(right) &&
                   left.argument == right.argument && left.access == right.access &&
                   left.buffer.Describe() == right.buffer.Describe() &&
                   left.buffer.Weak().SharesOwnership(right.buffer.Weak()) && left.alignment == right.alignment &&
                   left.publication == right.publication;
        }

        [[nodiscard]] bool ValidPolicy(const DestinationMemoryPolicy& policy) noexcept {
            const bool mentionsRequiredLocation =
                policy.requiredLocation.domain.Valid() || policy.requiredLocation.device.Valid();
            if (!policy.requiredProperties.Valid() || !policy.preferredProperties.Valid() ||
                (policy.backend && !ValidBackendKind(*policy.backend)) ||
                (mentionsRequiredLocation && !policy.requiredLocation.Valid()) ||
                !ValidPublication(policy.publication)) {
                return false;
            }
            for (size_t index = 0; index < policy.preferredLocations.size(); ++index) {
                if (!policy.preferredLocations[index].Valid()) {
                    return false;
                }
                for (size_t prior = 0; prior < index; ++prior) {
                    if (policy.preferredLocations[prior] == policy.preferredLocations[index]) {
                        return false;
                    }
                }
            }
            return true;
        }

        [[nodiscard]] bool DeviceAdmissible(const RuntimeDeviceBinding& binding,
                                            const DestinationMemoryPolicy& policy) noexcept {
            return (!policy.service.Valid() || policy.service == binding.service) &&
                   (!policy.device.Valid() || policy.device == binding.descriptor.id) &&
                   (!policy.backend || *policy.backend == binding.descriptor.backend);
        }

        [[nodiscard]] size_t PreferredLocationRank(std::span<const MemoryLocation> preferred,
                                                   MemoryLocation location) noexcept {
            const auto found = std::ranges::find(preferred, location);
            return found == preferred.end() ? preferred.size() : static_cast<size_t>(found - preferred.begin());
        }

        [[nodiscard]] const PhysicalMemoryDescriptor*
        SelectDestination(const RuntimeDeviceBinding& binding, std::span<const PhysicalMemoryDescriptor> memories,
                          const DestinationMemoryPolicy& policy) noexcept {
            const PhysicalMemoryDescriptor* selected = nullptr;
            for (const PhysicalMemoryDescriptor& memory : memories) {
                if (memory.service != binding.service || memory.location.device != binding.descriptor.id ||
                    memory.location.backend != binding.descriptor.backend ||
                    !memory.supportedProperties.Contains(policy.requiredProperties) ||
                    (policy.requiredLocation.Valid() && memory.location != policy.requiredLocation)) {
                    continue;
                }
                if (selected == nullptr) {
                    selected = std::addressof(memory);
                    continue;
                }
                const auto rank = [&](const PhysicalMemoryDescriptor& candidate) {
                    const size_t preferredLocation =
                        PreferredLocationRank(policy.preferredLocations, candidate.location);
                    const uint32_t missingProperties =
                        policy.preferredProperties.Bits() & ~candidate.supportedProperties.Bits();
                    return std::tuple{preferredLocation, std::popcount(missingProperties), candidate.location};
                };
                if (rank(memory) < rank(*selected)) {
                    selected = std::addressof(memory);
                }
            }
            return selected;
        }

        void CanonicalizeRegions(std::vector<ByteRegion>& regions) {
            std::ranges::sort(regions, {}, &ByteRegion::Offset);
            size_t output = 0;
            for (const ByteRegion region : regions) {
                if (output == 0 || region.Offset() > regions[output - 1].End()) {
                    regions[output++] = region;
                    continue;
                }
                const uint64_t end = std::max(regions[output - 1].End(), region.End());
                regions[output - 1] =
                    *ByteRegion::Make(regions[output - 1].Offset(), end - regions[output - 1].Offset());
            }
            regions.resize(output);
        }

        void CanonicalizeExactRegions(std::vector<ExactRegion>& regions) {
            std::ranges::sort(regions, [](const ExactRegion& left, const ExactRegion& right) {
                return std::tuple{left.region.Offset(), left.region.Size(), left.byteFootprint.has_value()} <
                       std::tuple{right.region.Offset(), right.region.Size(), right.byteFootprint.has_value()};
            });
            size_t output = 0;
            for (ExactRegion& region : regions) {
                if (output != 0 && regions[output - 1].Coverage() == region.Coverage()) {
                    continue;
                }
                if (output != 0 && !regions[output - 1].byteFootprint && !region.byteFootprint &&
                    region.region.Offset() <= regions[output - 1].region.End()) {
                    const uint64_t end = std::max(regions[output - 1].region.End(), region.region.End());
                    regions[output - 1].region = *ByteRegion::Make(regions[output - 1].region.Offset(),
                                                                   end - regions[output - 1].region.Offset());
                    continue;
                }
                if (output != static_cast<size_t>(std::addressof(region) - regions.data())) {
                    regions[output] = std::move(region);
                }
                ++output;
            }
            regions.resize(output);
        }

        [[nodiscard]] std::expected<void, DispatchCandidateEnumerationError> AddBytes(uint64_t bytes, uint64_t& total,
                                                                                      GraphNodeId node) noexcept {
            if (bytes > UINT64_MAX - total) {
                return std::unexpected(
                    DispatchCandidateEnumerationError{DispatchCandidateEnumerationErrorCode::CapacityExceeded, node});
            }
            total += bytes;
            return {};
        }

        [[nodiscard]] ArtifactResolutionMode ArtifactResolutionFor(const ArtifactAvailabilitySnapshot& artifacts,
                                                                   const ProgramCatalogSnapshot& packages,
                                                                   const ArtifactKey& artifact,
                                                                   BackendServiceId service,
                                                                   bool supportsPersistence) noexcept {
            if (artifacts.Contains({artifact, service})) {
                return ArtifactResolutionMode::Live;
            }
            if (supportsPersistence && packages.FindArtifact(artifact)) {
                return ArtifactResolutionMode::PackageImport;
            }
            return supportsPersistence && artifact.PersistentCacheable() ? ArtifactResolutionMode::Persistent
                                                                         : ArtifactResolutionMode::TransientProduction;
        }

        [[nodiscard]] bool SupportsPersistentArtifacts(const PhysicalResourceInventory& resources,
                                                       const PipelineConfiguration& configuration) noexcept {
            const auto found = std::ranges::find_if(resources.PipelineFactories(), [&](const auto& factory) {
                return factory.service == configuration.service && factory.factory == configuration.factory &&
                       factory.device == configuration.device && factory.backend == configuration.backend;
            });
            return found != resources.PipelineFactories().end() && found->supportsPersistentArtifacts;
        }

        template<typename Planner>
        [[nodiscard]] auto PlanWithCalibratedTopology(uint64_t bytes, const GraphPlanningServices& services,
                                                      bool allowCoherentAlias, Planner&& planner) noexcept
            -> decltype(planner(std::span<const TransferLink>{})) {
            auto direct = planner(std::span<const TransferLink>{});
            if (direct || direct.error() != ResidencyPlanningError::NoRoute) {
                return direct;
            }
            auto topology = ProjectCalibratedTransferTopology(services.resources, services.performance, bytes);
            if (!topology) {
                const auto cause = topology.error() == CalibratedTransferTopologyError::OutOfMemory
                                       ? ResidencyPlanningError::OutOfMemory
                                       : ResidencyPlanningError::InvalidTopology;
                return std::unexpected(cause);
            }
            if (allowCoherentAlias) {
                return planner(topology->Links());
            }
            try {
                std::vector<TransferLink> filtered;
                filtered.reserve(topology->Links().size());
                std::ranges::copy_if(topology->Links(), std::back_inserter(filtered),
                                     [](const TransferLink& link) { return link.kind != TransferKind::CoherentAlias; });
                return planner(filtered);
            } catch (const std::bad_alloc&) {
                return std::unexpected(ResidencyPlanningError::OutOfMemory);
            }
        }

        [[nodiscard]] GraphResidencyRoute ToGraphRoute(ResidencyRoute route) {
            return {.buffer = route.buffer,
                    .source = std::move(route.source),
                    .destinationService = route.destinationService,
                    .destination = route.destination,
                    .region = route.region,
                    .byteFootprint = std::move(route.byteFootprint),
                    .version = route.version,
                    .estimatedNanoseconds = route.estimatedNanoseconds,
                    .hops = std::move(route.hops)};
        }

        [[nodiscard]] std::expected<GraphResidencyRoute, ResidencyPlanningError>
        PlanReadyResidency(const BufferResidencySnapshot& snapshot, BackendServiceId service,
                           MemoryLocation destination, ByteCoverageView coverage, const GraphPlanningServices& services,
                           bool allowCoherentAlias) noexcept {
            const ByteRegion region = coverage.BoundingRegion();
            const ResidencyRequest request{
                .service = service, .destination = destination, .region = region, .coverage = coverage};
            auto route = PlanWithCalibratedTopology(
                region.Size(), services, allowCoherentAlias,
                [&](std::span<const TransferLink> topology) { return PlanResidency(snapshot, request, topology); });
            if (!route) {
                return std::unexpected(route.error());
            }
            return ToGraphRoute(std::move(*route));
        }

        [[nodiscard]] std::expected<GraphResidencyRoute, ResidencyPlanningError>
        PlanGraphLocalResidency(const GraphResidencySegment& segment, BackendServiceId service,
                                MemoryLocation destination, ByteCoverageView requested,
                                const GraphPlanningServices& services, bool allowCoherentAlias) noexcept {
            try {
                std::vector<ResidencyOrigin> origins;
                origins.reserve(segment.replicas.size());
                for (const GraphLocalResidencyReplica& replica : segment.replicas) {
                    origins.push_back({replica.service, replica.location});
                }
                const ByteRegion region = requested.BoundingRegion();
                const ResidencyRequest request{
                    .service = service, .destination = destination, .region = region, .coverage = requested};
                auto path = PlanWithCalibratedTopology(region.Size(), services, allowCoherentAlias,
                                                       [&](std::span<const TransferLink> topology) {
                                                           return PlanResidencyPath(origins, request, topology);
                                                       });
                if (!path) {
                    return std::unexpected(path.error());
                }
                std::optional<AffineByteFootprint> requestedFootprint;
                if (const AffineByteFootprint* affine = requested.BorrowedAffineFootprint(); affine != nullptr) {
                    requestedFootprint = *affine;
                }
                return GraphResidencyRoute{.buffer = segment.buffer,
                                           .source =
                                               GraphLocalResidencySource{.buffer = segment.buffer,
                                                                         .coverage = segment.region,
                                                                         .byteFootprint = segment.byteFootprint,
                                                                         .version = segment.version,
                                                                         .producer = segment.producer,
                                                                         .replica = segment.replicas[path->origin]},
                                           .destinationService = path->destinationService,
                                           .destination = path->destination,
                                           .region = region,
                                           .byteFootprint = std::move(requestedFootprint),
                                           .version = segment.version,
                                           .estimatedNanoseconds = path->estimatedNanoseconds,
                                           .hops = std::move(path->hops)};
            } catch (const std::bad_alloc&) {
                return std::unexpected(ResidencyPlanningError::OutOfMemory);
            }
        }

        [[nodiscard]] std::expected<void, ResidencyPlanningError>
        PlanReadableInterval(const BufferRegions& buffer, ByteRegion requested, BackendServiceId service,
                             MemoryLocation destination, const GraphPlanningServices& services,
                             const GraphResidencyState& residency, bool allowCoherentAlias,
                             std::vector<GraphResidencyRoute>& routes) {
            uint64_t cursor = requested.Offset();
            const auto planReady = [&](uint64_t begin, uint64_t end) -> std::expected<void, ResidencyPlanningError> {
                if (begin == end) {
                    return {};
                }
                const ByteRegion region = *ByteRegion::Make(begin, end - begin);
                const ByteCoverageView logical = buffer.snapshot->LogicalCoverage();
                const ByteCoverageView coverage =
                    region == logical.BoundingRegion() ? logical : ByteCoverageView::Interval(region);
                auto route =
                    PlanReadyResidency(*buffer.snapshot, service, destination, coverage, services, allowCoherentAlias);
                if (!route) {
                    return std::unexpected(route.error());
                }
                routes.push_back(std::move(*route));
                return {};
            };
            for (const GraphResidencySegment& segment : residency.Segments(buffer.buffer.Describe().id)) {
                if (segment.region.End() <= cursor || requested.End() <= segment.region.Offset()) {
                    continue;
                }
                if (segment.byteFootprint) {
                    return std::unexpected(ResidencyPlanningError::NoRoute);
                }
                const uint64_t graphLocalBegin = std::max(cursor, segment.region.Offset());
                const uint64_t graphLocalEnd = std::min(requested.End(), segment.region.End());
                if (auto gap = planReady(cursor, graphLocalBegin); !gap) {
                    return gap;
                }
                auto route = PlanGraphLocalResidency(
                    segment, service, destination,
                    ByteCoverageView::Interval(*ByteRegion::Make(graphLocalBegin, graphLocalEnd - graphLocalBegin)),
                    services, allowCoherentAlias);
                if (!route) {
                    return std::unexpected(route.error());
                }
                routes.push_back(std::move(*route));
                cursor = graphLocalEnd;
                if (cursor == requested.End()) {
                    return {};
                }
            }
            return planReady(cursor, requested.End());
        }

        [[nodiscard]] std::expected<void, ResidencyPlanningError>
        PlanReadableAffine(const BufferRegions& buffer, ByteCoverageView requested, BackendServiceId service,
                           MemoryLocation destination, const GraphPlanningServices& services,
                           const GraphResidencyState& residency, bool allowCoherentAlias,
                           std::vector<GraphResidencyRoute>& routes) {
            constexpr size_t coverageCapacity = 256;
            const auto planReadyCover = [&](ByteCoverageView target) -> std::expected<void, ResidencyPlanningError> {
                const ResidencyRequest wholeRequest{.service = service,
                                                    .destination = destination,
                                                    .region = target.BoundingRegion(),
                                                    .coverage = target};
                auto whole = PlanResidency(*buffer.snapshot, wholeRequest, {});
                if (whole) {
                    routes.push_back(ToGraphRoute(std::move(*whole)));
                    return {};
                }
                if (whole.error() != ResidencyPlanningError::NoSourceReplica &&
                    whole.error() != ResidencyPlanningError::NoRoute) {
                    return std::unexpected(whole.error());
                }

                std::array<AffineByteFootprint, coverageCapacity> candidateFootprints{};
                std::array<ByteCoverageView, coverageCapacity> candidates{};
                std::array<bool, coverageCapacity> candidateResident{};
                size_t candidateCount = 0;
                for (const ResidentReplica& replica : buffer.snapshot->replicas) {
                    const ByteCoverageView coverage = replica.Coverage();
                    ByteCoverageView candidate;
                    std::optional<AffineByteFootprint> clipped;
                    if (coverage.BorrowedAffineFootprint() != nullptr) {
                        auto intersection =
                            IntersectAffineSubviews(buffer.snapshot->LogicalCoverage(), target, coverage);
                        if (!intersection) {
                            return std::unexpected(ResidencyPlanningError::NoRoute);
                        }
                        if (!*intersection) {
                            continue;
                        }
                        clipped = **intersection;
                        candidate = ByteCoverageView::Affine(*clipped);
                    } else {
                        if (target.Contains(coverage) != CoverageContainment::Yes) {
                            continue;
                        }
                        candidate = coverage;
                    }
                    const std::span<const ByteCoverageView> active{candidates.data(), candidateCount};
                    if (std::ranges::find(active, candidate) != active.end()) {
                        continue;
                    }
                    if (candidateCount == coverageCapacity) {
                        return std::unexpected(ResidencyPlanningError::CapacityExceeded);
                    }
                    if (clipped) {
                        candidateFootprints[candidateCount] = *clipped;
                        candidates[candidateCount] = ByteCoverageView::Affine(candidateFootprints[candidateCount]);
                    } else {
                        candidates[candidateCount] = candidate;
                    }
                    candidateResident[candidateCount] =
                        std::ranges::any_of(buffer.snapshot->replicas, [&](const ResidentReplica& source) {
                            return source.allocation.View().service == service &&
                                   source.allocation.Location() == destination &&
                                   source.Covers(buffer.snapshot->version, candidates[candidateCount]);
                        });
                    ++candidateCount;
                }
                std::array<size_t, coverageCapacity> candidateOrder{};
                for (size_t index = 0; index < candidateCount; ++index) {
                    candidateOrder[index] = index;
                }
                std::sort(candidateOrder.begin(), candidateOrder.begin() + static_cast<std::ptrdiff_t>(candidateCount),
                          [&](size_t leftIndex, size_t rightIndex) {
                              if (candidateResident[leftIndex] != candidateResident[rightIndex]) {
                                  return candidateResident[leftIndex];
                              }
                              const ByteCoverageView left = candidates[leftIndex];
                              const ByteCoverageView right = candidates[rightIndex];
                              if (left.OccupiedByteCount() != right.OccupiedByteCount()) {
                                  return left.OccupiedByteCount() > right.OccupiedByteCount();
                              }
                              return left.BoundingRegion() < right.BoundingRegion();
                          });

                const auto planSelected =
                    [&](std::span<const ByteCoverageView> selected) -> std::expected<void, ResidencyPlanningError> {
                    for (const ByteCoverageView coverage : selected) {
                        auto route = PlanReadyResidency(*buffer.snapshot, service, destination, coverage, services,
                                                        allowCoherentAlias);
                        if (!route) {
                            return std::unexpected(route.error());
                        }
                        routes.push_back(std::move(*route));
                    }
                    return {};
                };

                std::array<AffineByteFootprint, coverageCapacity> selectedFootprints{};
                std::array<ByteCoverageView, coverageCapacity> selected{};
                size_t selectedCount = 0;
                for (size_t candidate = 0; candidate < candidateCount; ++candidate) {
                    const ByteCoverageView coverage = candidates[candidateOrder[candidate]];
                    if (coverage.BorrowedAffineFootprint() == nullptr) {
                        continue;
                    }
                    auto residual = AffineCoverageComplement<coverageCapacity>::Create(
                        target, coverage, {selected.data(), selectedCount});
                    if (!residual) {
                        if (residual.error() == AffineByteFootprintError::CapacityExceeded) {
                            return std::unexpected(ResidencyPlanningError::CapacityExceeded);
                        }
                        return std::unexpected(ResidencyPlanningError::NoRoute);
                    }
                    if (residual->Size() > coverageCapacity - selectedCount) {
                        return std::unexpected(ResidencyPlanningError::CapacityExceeded);
                    }
                    for (const AffineByteFootprint& footprint : residual->Footprints()) {
                        selectedFootprints[selectedCount] = footprint;
                        selected[selectedCount] = ByteCoverageView::Affine(selectedFootprints[selectedCount]);
                        ++selectedCount;
                    }
                }
                const CoverageEquivalence affineCover =
                    ExactCoverageUnion<coverageCapacity>(target, {selected.data(), selectedCount});
                if (affineCover == CoverageEquivalence::Equal) {
                    return planSelected({selected.data(), selectedCount});
                }
                if (affineCover == CoverageEquivalence::Unknown) {
                    return std::unexpected(ResidencyPlanningError::CapacityExceeded);
                }

                const auto targetRegions = target.ProjectCanonicalRegions<coverageCapacity>();
                if (!targetRegions) {
                    return std::unexpected(targetRegions.error() == AffineByteFootprintError::CapacityExceeded
                                               ? ResidencyPlanningError::CapacityExceeded
                                               : ResidencyPlanningError::NoRoute);
                }
                std::array<ByteRegion, coverageCapacity> uncovered{};
                size_t uncoveredCount = targetRegions->Size();
                std::ranges::copy(targetRegions->Regions(), uncovered.begin());
                std::array<ByteRegion, coverageCapacity> selectedRegions{};
                selectedCount = 0;

                for (size_t candidate = 0; candidate < candidateCount && uncoveredCount != 0; ++candidate) {
                    const auto candidateRegions =
                        candidates[candidateOrder[candidate]].ProjectCanonicalRegions<coverageCapacity>();
                    if (!candidateRegions) {
                        if (candidateRegions.error() == AffineByteFootprintError::CapacityExceeded) {
                            return std::unexpected(ResidencyPlanningError::CapacityExceeded);
                        }
                        return std::unexpected(ResidencyPlanningError::NoRoute);
                    }
                    for (const ByteRegion candidateRegion : candidateRegions->Regions()) {
                        std::array<ByteRegion, coverageCapacity> next{};
                        size_t nextCount = 0;
                        for (size_t index = 0; index < uncoveredCount; ++index) {
                            const ByteRegion current = uncovered[index];
                            const uint64_t intersectionBegin = std::max(current.Offset(), candidateRegion.Offset());
                            const uint64_t intersectionEnd = std::min(current.End(), candidateRegion.End());
                            if (intersectionBegin >= intersectionEnd) {
                                if (nextCount == coverageCapacity) {
                                    return std::unexpected(ResidencyPlanningError::CapacityExceeded);
                                }
                                next[nextCount++] = current;
                                continue;
                            }
                            if (selectedCount == coverageCapacity) {
                                return std::unexpected(ResidencyPlanningError::CapacityExceeded);
                            }
                            selectedRegions[selectedCount++] =
                                *ByteRegion::Make(intersectionBegin, intersectionEnd - intersectionBegin);
                            if (current.Offset() < intersectionBegin) {
                                if (nextCount == coverageCapacity) {
                                    return std::unexpected(ResidencyPlanningError::CapacityExceeded);
                                }
                                next[nextCount++] =
                                    *ByteRegion::Make(current.Offset(), intersectionBegin - current.Offset());
                            }
                            if (intersectionEnd < current.End()) {
                                if (nextCount == coverageCapacity) {
                                    return std::unexpected(ResidencyPlanningError::CapacityExceeded);
                                }
                                next[nextCount++] = *ByteRegion::Make(intersectionEnd, current.End() - intersectionEnd);
                            }
                        }
                        uncovered = next;
                        uncoveredCount = nextCount;
                    }
                }
                if (uncoveredCount != 0) {
                    return std::unexpected(ResidencyPlanningError::NoRoute);
                }
                for (size_t index = 0; index < selectedCount; ++index) {
                    selected[index] = ByteCoverageView::Interval(selectedRegions[index]);
                }
                return planSelected({selected.data(), selectedCount});
            };
            std::array<const GraphResidencySegment*, coverageCapacity> segments{};
            std::array<ByteCoverageView, coverageCapacity> coverages{};
            size_t count = 0;
            for (const GraphResidencySegment& segment : residency.Segments(buffer.buffer.Describe().id)) {
                const ByteCoverageView graphLocal = segment.Coverage();
                if (graphLocal.Contains(requested) == CoverageContainment::Yes) {
                    auto route =
                        PlanGraphLocalResidency(segment, service, destination, requested, services, allowCoherentAlias);
                    if (!route) {
                        return std::unexpected(route.error());
                    }
                    routes.push_back(std::move(*route));
                    return {};
                }
                if (requested.Contains(graphLocal) != CoverageContainment::Yes) {
                    if (CoverageRelationWithin(buffer.snapshot->LogicalCoverage(), requested, graphLocal) !=
                        CoverageOverlap::Disjoint) {
                        return std::unexpected(ResidencyPlanningError::NoRoute);
                    }
                    continue;
                }
                if (count == coverageCapacity) {
                    return std::unexpected(ResidencyPlanningError::CapacityExceeded);
                }
                segments[count] = std::addressof(segment);
                coverages[count++] = graphLocal;
            }
            if (count == 0) {
                return planReadyCover(requested);
            }
            const CoverageEquivalence unionResult =
                ExactCoverageUnion<coverageCapacity>(requested, {coverages.data(), count});
            if (unionResult == CoverageEquivalence::Unknown) {
                return std::unexpected(ResidencyPlanningError::CapacityExceeded);
            }

            AffineCoverageComplement<coverageCapacity> complement;
            if (unionResult != CoverageEquivalence::Equal) {
                auto difference =
                    AffineCoverageComplement<coverageCapacity>::Create(requested, {coverages.data(), count});
                if (!difference) {
                    return std::unexpected(difference.error() == AffineByteFootprintError::CapacityExceeded
                                               ? ResidencyPlanningError::CapacityExceeded
                                               : ResidencyPlanningError::NoRoute);
                }
                complement = std::move(*difference);
                if (complement.Size() > coverageCapacity - count) {
                    return std::unexpected(ResidencyPlanningError::CapacityExceeded);
                }
                for (size_t index = 0; index < complement.Size(); ++index) {
                    coverages[count + index] = ByteCoverageView::Affine(complement.Footprints()[index]);
                }
                const CoverageEquivalence exact =
                    ExactCoverageUnion<coverageCapacity>(requested, {coverages.data(), count + complement.Size()});
                if (exact != CoverageEquivalence::Equal) {
                    return std::unexpected(exact == CoverageEquivalence::Unknown
                                               ? ResidencyPlanningError::CapacityExceeded
                                               : ResidencyPlanningError::NoRoute);
                }
            }
            for (size_t index = 0; index < count; ++index) {
                auto route = PlanGraphLocalResidency(*segments[index], service, destination, coverages[index], services,
                                                     allowCoherentAlias);
                if (!route) {
                    return std::unexpected(route.error());
                }
                routes.push_back(std::move(*route));
            }
            for (const AffineByteFootprint& footprint : complement.Footprints()) {
                if (auto ready = planReadyCover(ByteCoverageView::Affine(footprint)); !ready) {
                    return ready;
                }
            }
            return {};
        }

        [[nodiscard]] std::expected<void, ResidencyPlanningError>
        PlanReadableRegion(const BufferRegions& buffer, const ExactRegion& requested, BackendServiceId service,
                           MemoryLocation destination, const GraphPlanningServices& services,
                           const GraphResidencyState& residency, bool allowCoherentAlias,
                           std::vector<GraphResidencyRoute>& routes) {
            if (requested.byteFootprint) {
                return PlanReadableAffine(buffer, requested.Coverage(), service, destination, services, residency,
                                          allowCoherentAlias, routes);
            }
            return PlanReadableInterval(buffer, requested.region, service, destination, services, residency,
                                        allowCoherentAlias, routes);
        }

    } // namespace

    namespace {

        std::expected<DispatchResourceCandidateSet, DispatchCandidateEnumerationError>
        EnumerateDispatchResourceCandidatesImpl(const ExecutionGraph& graph, GraphNodeId node,
                                                const GraphPlanningContext& context,
                                                const GraphPlanningServices& services,
                                                const DispatchEnumerationPolicy& policy,
                                                std::vector<BufferRegions>* preparedBuffers) noexcept {
            if (context.epochSequence == 0 || context.devices.size() != context.snapshots.size() ||
                policy.expectedReuseCount == 0 || !ValidPolicy(policy.memory)) {
                return std::unexpected(
                    DispatchCandidateEnumerationError{DispatchCandidateEnumerationErrorCode::InvalidRequest, node});
            }
            if (!node.Valid() || node.index >= graph.NodeCount()) {
                return std::unexpected(
                    DispatchCandidateEnumerationError{DispatchCandidateEnumerationErrorCode::InvalidNode, node});
            }
            const auto* invocation = std::get_if<KernelInvocation>(&graph.Operation(node));
            if (invocation == nullptr) {
                return std::unexpected(
                    DispatchCandidateEnumerationError{DispatchCandidateEnumerationErrorCode::NotDispatch, node});
            }
            const auto geometry = PlanDispatchGeometry(invocation->Domain());
            if (!geometry) {
                return std::unexpected(
                    DispatchCandidateEnumerationError{geometry.error() == DispatchGeometryError::CapacityExceeded
                                                          ? DispatchCandidateEnumerationErrorCode::CapacityExceeded
                                                          : DispatchCandidateEnumerationErrorCode::InvalidRequest,
                                                      node});
            }
            const auto package = services.packages.Resolve(invocation->ProgramHash());
            if (!package) {
                return std::unexpected(
                    DispatchCandidateEnumerationError{DispatchCandidateEnumerationErrorCode::ProgramUnavailable,
                                                      node,
                                                      {},
                                                      static_cast<uint64_t>(package.error().code)});
            }

            try {
                DispatchResourceCandidateSet result{
                    .node = node,
                    .workload = {.kernelHash = invocation->ProgramHash(),
                                 .requiredFeatures = invocation->RequiredDeviceFeatures(),
                                 .resourceDemand = invocation->ResourceDemand(),
                                 .workItemCount = invocation->Domain().workItemCount,
                                 .expectedReuseCount = policy.expectedReuseCount,
                                 .deterministic = policy.deterministic},
                    .geometry = *geometry};
                std::vector<BufferRegions> buffers;
                buffers.reserve(invocation->ResourceAccesses().size());

                const auto signature = invocation->Signature();
                const auto bindings = invocation->Bindings();
                std::vector<DispatchMemoryBinding> memoryBindings;
                memoryBindings.reserve(invocation->ResourceAccesses().size());
                for (uint32_t index = 0; index < signature.size(); ++index) {
                    if (signature[index].kind != ParameterKind::Buffer) {
                        continue;
                    }
                    const auto* view = std::get_if<BufferView>(&bindings[index]);
                    if (view == nullptr || !*view) {
                        return std::unexpected(DispatchCandidateEnumerationError{
                            DispatchCandidateEnumerationErrorCode::InvalidRequest, node, {index}});
                    }
                    const BufferId id = view->BufferDescriptor().id;
                    auto found = std::ranges::find_if(
                        buffers, [id](const BufferRegions& value) { return value.buffer.Describe().id == id; });
                    if (found == buffers.end()) {
                        buffers.push_back({.buffer = view->LogicalBuffer()});
                        found = std::prev(buffers.end());
                    } else if (!found->buffer.Weak().SharesOwnership(view->LogicalBuffer().Weak())) {
                        return std::unexpected(DispatchCandidateEnumerationError{
                            DispatchCandidateEnumerationErrorCode::InvalidRequest, node, {index}});
                    }
                    found->touched.push_back(view->Region());
                    const AffineByteFootprint footprint = view->Footprint();
                    const bool affineCoverage = !footprint.IsContiguous();
                    if (Readable(signature[index].access)) {
                        found->readable.push_back(
                            {.region = view->Region(),
                             .byteFootprint = affineCoverage ? std::optional{footprint} : std::nullopt});
                    }
                    DispatchMemoryBinding memoryBinding{
                        .argument = {index},
                        .access = signature[index].access,
                        .buffer = view->LogicalBuffer(),
                        .region = view->Region(),
                        .alignment =
                            std::max<uint64_t>(view->BufferDescriptor().alignment, signature[index].minimumAlignment),
                        .publication = policy.memory.publication};
                    if (affineCoverage) {
                        memoryBinding.byteFootprint = footprint;
                    }
                    memoryBindings.push_back(std::move(memoryBinding));
                }

                for (BufferRegions& buffer : buffers) {
                    CanonicalizeRegions(buffer.touched);
                    CanonicalizeExactRegions(buffer.readable);
                    uint64_t bufferBytes = 0;
                    uint64_t largestRegion = 0;
                    for (const ByteRegion region : buffer.touched) {
                        if (auto added = AddBytes(region.Size(), bufferBytes, node); !added) {
                            return std::unexpected(added.error());
                        }
                        largestRegion = std::max(largestRegion, region.Size());
                    }
                    if (auto added = AddBytes(bufferBytes, result.workload.requiredDeviceMemoryBytes, node); !added) {
                        return std::unexpected(added.error());
                    }
                    result.workload.largestAllocationBytes =
                        std::max(result.workload.largestAllocationBytes, largestRegion);
                }
                if (preparedBuffers != nullptr) {
                    *preparedBuffers = std::move(buffers);
                }

                std::vector<CandidateArtifactState> artifacts;
                std::vector<DeviceDestination> destinations;
                artifacts.reserve(services.resources.Queues().size());
                destinations.reserve(context.devices.size());

                for (size_t deviceIndex = 0; deviceIndex < context.devices.size(); ++deviceIndex) {
                    const RuntimeDeviceBinding& device = context.devices[deviceIndex];
                    const DeviceSnapshot& snapshot = context.snapshots[deviceIndex];
                    const auto localSize = CanonicalDispatchLocalSize(invocation->Domain().rank);
                    const uint32_t localInvocations = localSize[0] * localSize[1] * localSize[2];
                    if (!TargetMatches(device) || !ValidDeviceSnapshot(snapshot) ||
                        snapshot.id != device.descriptor.id || snapshot.state != DeviceState::Available ||
                        device.descriptor.maximumWorkgroupInvocations < localInvocations ||
                        !DeviceAdmissible(device, policy.memory)) {
                        continue;
                    }
                    const PhysicalMemoryDescriptor* destination = nullptr;
                    if (!memoryBindings.empty()) {
                        destination = SelectDestination(device, services.resources.Memories(), policy.memory);
                        if (destination == nullptr) {
                            continue;
                        }
                    }

                    bool hasQueue = false;
                    for (const PhysicalCompilerDescriptor& compiler :
                         services.resources.CompilersFor(device.service, device.descriptor.backend)) {
                        const TargetFingerprint target =
                            ComposeTarget(device.target, compiler.compiler, compiler.executableDomain);
                        if (!target.Valid()) {
                            continue;
                        }
                        for (const PhysicalQueueDescriptor& queue : services.resources.QueuesFor(
                                 device.service, device.descriptor.backend, device.descriptor.id)) {
                            if (!queue.queue.Valid() || !queue.capabilities.Supports(BackendCommandKind::Dispatch)) {
                                continue;
                            }
                            const KernelCalibrationKey key{.kernel = invocation->ProgramHash(),
                                                           .target = target,
                                                           .specialization = policy.specialization,
                                                           .queue = queue.queue};
                            const ArtifactKey executable = MakeArtifactKey(ExecutableKey{
                                .lowered = key.kernel, .target = key.target, .specialization = key.specialization});
                            const ArtifactResolutionMode executableResolution =
                                ArtifactResolutionFor(services.artifacts, services.packages, executable, device.service,
                                                      compiler.supportsPersistentArtifacts);
                            bool hasVariant = false;
                            if (queue.dispatchArtifactKind == ArtifactKind::DeviceExecutable) {
                                artifacts.push_back(
                                    {.service = device.service, .candidate = key, .executable = executableResolution});
                                hasVariant = true;
                            } else if (queue.dispatchArtifactKind == ArtifactKind::DevicePipeline) {
                                const SemanticHash bindingLayout =
                                    PipelineBindingLayoutIdentity(invocation->Signature());
                                const auto configurations = services.resources.PipelineConfigurationsFor(
                                    device.service, device.descriptor.backend, device.descriptor.id);
                                for (const PipelineConfiguration& configuration : configurations) {
                                    const PipelineKey pipeline{.executable = ArtifactIdentity(executable),
                                                               .target = target,
                                                               .builder = configuration.factory,
                                                               .state = configuration.state,
                                                               .bindingLayout = bindingLayout};
                                    const ArtifactKey artifact = MakeArtifactKey(pipeline);
                                    const bool supportsPersistence =
                                        SupportsPersistentArtifacts(services.resources, configuration);
                                    artifacts.push_back({.service = device.service,
                                                         .candidate = key,
                                                         .executable = executableResolution,
                                                         .pipeline = PipelineArtifactState{
                                                             .key = pipeline,
                                                             .resolution = ArtifactResolutionFor(
                                                                 services.artifacts, services.packages, artifact,
                                                                 device.service, supportsPersistence)}});
                                    hasVariant = true;
                                }
                            }
                            if (hasVariant) {
                                hasQueue = true;
                            }
                        }
                    }
                    if (hasQueue) {
                        destinations.push_back(
                            {.service = device.service,
                             .device = device.descriptor.id,
                             .destination = destination != nullptr ? std::optional{*destination} : std::nullopt});
                    }
                }
                if (destinations.empty()) {
                    result.bindings = std::move(memoryBindings);
                    return result;
                }

                const DispatchCandidateRequest request{.workload = result.workload,
                                                       .specialization = policy.specialization,
                                                       .devices = context.devices,
                                                       .snapshots = context.snapshots,
                                                       .artifacts = artifacts};
                auto candidates =
                    SynthesizeDispatchCandidates(request, services.resources, services.performance, services.priors);
                if (!candidates) {
                    return std::unexpected(DispatchCandidateEnumerationError{
                        DispatchCandidateEnumerationErrorCode::CandidateSynthesisFailed,
                        node,
                        {},
                        static_cast<uint64_t>(candidates.error())});
                }
                result.bindings = std::move(memoryBindings);
                result.candidates.reserve(candidates->size());
                for (PlacementCandidate& placement : *candidates) {
                    const auto destination = std::ranges::find_if(destinations, [&](const DeviceDestination& value) {
                        return value.service == placement.service && value.device == placement.device.id;
                    });
                    if (destination == destinations.end()) {
                        return std::unexpected(DispatchCandidateEnumerationError{
                            DispatchCandidateEnumerationErrorCode::CandidateSynthesisFailed, node});
                    }
                    result.candidates.push_back(
                        {.placement = std::move(placement), .destination = destination->destination});
                }
                return result;
            } catch (const std::bad_alloc&) {
                return std::unexpected(
                    DispatchCandidateEnumerationError{DispatchCandidateEnumerationErrorCode::OutOfMemory, node});
            }
        }

    } // namespace

    std::expected<DispatchResourceCandidateSet, DispatchCandidateEnumerationError>
    EnumerateDispatchResourceCandidates(const ExecutionGraph& graph, GraphNodeId node,
                                        const GraphPlanningContext& context, const GraphPlanningServices& services,
                                        const DispatchEnumerationPolicy& policy) noexcept {
        return EnumerateDispatchResourceCandidatesImpl(graph, node, context, services, policy, nullptr);
    }

    std::expected<DispatchCandidateSet, DispatchCandidateEnumerationError>
    EnumerateDispatchCandidates(const ExecutionGraph& graph, GraphNodeId node, const GraphPlanningContext& context,
                                const GraphPlanningServices& services, const GraphResidencyState& residency,
                                const DispatchEnumerationPolicy& policy) noexcept {
        std::vector<BufferRegions> buffers;
        auto resources = EnumerateDispatchResourceCandidatesImpl(graph, node, context, services, policy, &buffers);
        if (!resources) {
            return std::unexpected(resources.error());
        }
        try {
            DispatchCandidateSet result{.node = node, .workload = resources->workload, .geometry = resources->geometry};
            for (BufferRegions& buffer : buffers) {
                if (!buffer.readable.empty()) {
                    buffer.snapshot = residency.ReadySnapshot(buffer.buffer.Describe().id);
                    if (buffer.snapshot == nullptr || !residency.SharesBufferOwnership(buffer.buffer)) {
                        return std::unexpected(DispatchCandidateEnumerationError{
                            DispatchCandidateEnumerationErrorCode::InvalidRequest, node});
                    }
                }
            }

            struct ProjectionMemo {
                BackendServiceId service{};
                DeviceId device{};
                std::optional<PhysicalMemoryDescriptor> destination{};
                uint32_t residency = UINT32_MAX;
            };
            std::vector<ProjectionMemo> projections;
            projections.reserve(context.devices.size());
            result.residencies.reserve(context.devices.size());
            result.placements.reserve(resources->candidates.size());

            for (DispatchResourceCandidate& candidate : resources->candidates) {
                auto memo = std::ranges::find_if(projections, [&](const ProjectionMemo& value) {
                    return value.service == candidate.placement.service &&
                           value.device == candidate.placement.device.id && value.destination == candidate.destination;
                });
                if (memo == projections.end()) {
                    ProjectionMemo created{.service = candidate.placement.service,
                                           .device = candidate.placement.device.id,
                                           .destination = candidate.destination};
                    DispatchResidencyProjection projection{.service = candidate.placement.service,
                                                           .device = candidate.placement.device.id,
                                                           .destination = candidate.destination,
                                                           .bindings = resources->bindings,
                                                           .allowCoherentAlias = policy.memory.allowCoherentAlias};
                    bool reachable = resources->bindings.empty() || candidate.destination.has_value();
                    if (reachable) {
                        for (const BufferRegions& buffer : buffers) {
                            for (const ExactRegion& region : buffer.readable) {
                                auto planned = PlanReadableRegion(buffer, region, candidate.placement.service,
                                                                  candidate.destination->location, services, residency,
                                                                  policy.memory.allowCoherentAlias, projection.inputs);
                                if (!planned) {
                                    if (planned.error() == ResidencyPlanningError::OutOfMemory) {
                                        return std::unexpected(DispatchCandidateEnumerationError{
                                            DispatchCandidateEnumerationErrorCode::OutOfMemory,
                                            node,
                                            {},
                                            static_cast<uint64_t>(planned.error())});
                                    }
                                    reachable = false;
                                    break;
                                }
                            }
                            if (!reachable) {
                                break;
                            }
                        }
                    }
                    if (reachable && !Detail::CanAssembleDispatchInputs(projection, services.resources)) {
                        reachable = false;
                    }
                    if (reachable) {
                        if (result.residencies.size() >= UINT32_MAX) {
                            return std::unexpected(DispatchCandidateEnumerationError{
                                DispatchCandidateEnumerationErrorCode::CapacityExceeded, node});
                        }
                        created.residency = static_cast<uint32_t>(result.residencies.size());
                        result.residencies.push_back(std::move(projection));
                    }
                    projections.push_back(std::move(created));
                    memo = std::prev(projections.end());
                }
                if (memo->residency == UINT32_MAX) {
                    continue;
                }
                DispatchResidencyProjection& projection = result.residencies[memo->residency];
                const KernelCalibrationKey key{.kernel = result.workload.kernelHash,
                                               .target = candidate.placement.target,
                                               .specialization = candidate.placement.specialization,
                                               .queue = candidate.placement.queue};
                const auto transfer = ProjectCandidateTransferCost(candidate.placement.service, key, projection.inputs);
                if (!transfer) {
                    return std::unexpected(DispatchCandidateEnumerationError{
                        DispatchCandidateEnumerationErrorCode::ResidencyProjectionFailed,
                        node,
                        {},
                        static_cast<uint64_t>(transfer.error())});
                }
                projection.transfer = {transfer->bytes, transfer->nanoseconds};
                candidate.placement.transfer = CandidateTransferCost{transfer->bytes, transfer->nanoseconds};
                result.placements.push_back(
                    {.placement = std::move(candidate.placement), .residencyProjection = memo->residency});
            }
            return result;
        } catch (const std::bad_alloc&) {
            return std::unexpected(
                DispatchCandidateEnumerationError{DispatchCandidateEnumerationErrorCode::OutOfMemory, node});
        }
    }

    std::expected<DispatchResidencyProjection, DispatchCandidateEnumerationError>
    ProjectDispatchSliceResidency(const KernelInvocation& invocation, GraphNodeId node, const DispatchSlice& slice,
                                  const DispatchResidencyProjection& destination, const GraphPlanningServices& services,
                                  const GraphResidencyState& residency) noexcept {
        if (!node.Valid() || !slice.ValidWithin(invocation.Domain()) || !destination.service.Valid() ||
            !destination.device.Valid() || (!destination.bindings.empty() && !destination.destination) ||
            (destination.destination && (destination.destination->service != destination.service ||
                                         destination.destination->location.device != destination.device))) {
            return std::unexpected(
                DispatchCandidateEnumerationError{DispatchCandidateEnumerationErrorCode::InvalidRequest, node});
        }
        try {
            DispatchResidencyProjection result{.service = destination.service,
                                               .device = destination.device,
                                               .destination = destination.destination,
                                               .allowCoherentAlias = destination.allowCoherentAlias};
            std::vector<BufferRegions> buffers;
            buffers.reserve(invocation.ResourceAccesses().size());
            result.bindings.reserve(destination.bindings.size());
            const auto signature = invocation.Signature();
            const auto invocationBindings = invocation.Bindings();
            const std::span<const uint64_t> origin{slice.origin.data(), slice.rank};
            const std::span<const uint64_t> extents{slice.extents.data(), slice.rank};

            for (uint32_t argument = 0; argument < signature.size(); ++argument) {
                if (signature[argument].kind != ParameterKind::Buffer) {
                    continue;
                }
                const auto* wholeView = std::get_if<BufferView>(&invocationBindings[argument]);
                const auto original = std::ranges::find(destination.bindings, KernelArgumentId{argument},
                                                        &DispatchMemoryBinding::argument);
                if (wholeView == nullptr || !*wholeView || original == destination.bindings.end() ||
                    original->access != signature[argument].access ||
                    original->buffer.Describe().id != wholeView->BufferDescriptor().id) {
                    return std::unexpected(DispatchCandidateEnumerationError{
                        DispatchCandidateEnumerationErrorCode::InvalidRequest, node, {argument}});
                }
                auto shardView = wholeView->SubView(origin, extents);
                if (!shardView) {
                    return std::unexpected(
                        DispatchCandidateEnumerationError{DispatchCandidateEnumerationErrorCode::InvalidRequest,
                                                          node,
                                                          {argument},
                                                          static_cast<uint64_t>(shardView.error())});
                }
                if (!LayoutSatisfies(shardView->Describe().layout, signature[argument].layout)) {
                    return std::unexpected(DispatchCandidateEnumerationError{
                        DispatchCandidateEnumerationErrorCode::InvalidRequest,
                        node,
                        {argument},
                        static_cast<uint64_t>(KernelInvocationErrorCode::LayoutMismatch)});
                }
                const AffineByteFootprint footprint = shardView->Footprint();
                const bool affineCoverage = !footprint.IsContiguous();
                DispatchMemoryBinding binding{.argument = {argument},
                                              .access = original->access,
                                              .buffer = shardView->LogicalBuffer(),
                                              .region = shardView->Region(),
                                              .alignment = original->alignment,
                                              .publication = original->publication};
                if (affineCoverage) {
                    binding.byteFootprint = footprint;
                }
                result.bindings.push_back(std::move(binding));
                if (!Readable(original->access)) {
                    continue;
                }

                const BufferId bufferId = shardView->BufferDescriptor().id;
                auto prepared = std::ranges::find_if(
                    buffers, [bufferId](const BufferRegions& value) { return value.buffer.Describe().id == bufferId; });
                if (prepared == buffers.end()) {
                    buffers.push_back({.buffer = shardView->LogicalBuffer()});
                    prepared = std::prev(buffers.end());
                } else if (!prepared->buffer.Weak().SharesOwnership(shardView->LogicalBuffer().Weak())) {
                    return std::unexpected(DispatchCandidateEnumerationError{
                        DispatchCandidateEnumerationErrorCode::InvalidRequest, node, {argument}});
                }
                prepared->readable.push_back(
                    {.region = shardView->Region(),
                     .byteFootprint = affineCoverage ? std::optional{footprint} : std::nullopt});
            }

            for (BufferRegions& buffer : buffers) {
                CanonicalizeExactRegions(buffer.readable);
                buffer.snapshot = residency.ReadySnapshot(buffer.buffer.Describe().id);
                if (buffer.snapshot == nullptr || !residency.SharesBufferOwnership(buffer.buffer)) {
                    return std::unexpected(
                        DispatchCandidateEnumerationError{DispatchCandidateEnumerationErrorCode::InvalidRequest, node});
                }
                for (const ExactRegion& region : buffer.readable) {
                    auto planned =
                        PlanReadableRegion(buffer, region, destination.service, destination.destination->location,
                                           services, residency, destination.allowCoherentAlias, result.inputs);
                    if (!planned) {
                        const auto code = planned.error() == ResidencyPlanningError::OutOfMemory
                                              ? DispatchCandidateEnumerationErrorCode::OutOfMemory
                                          : planned.error() == ResidencyPlanningError::CapacityExceeded
                                              ? DispatchCandidateEnumerationErrorCode::CapacityExceeded
                                              : DispatchCandidateEnumerationErrorCode::ResidencyProjectionFailed;
                        return std::unexpected(
                            DispatchCandidateEnumerationError{code, node, {}, static_cast<uint64_t>(planned.error())});
                    }
                }
            }
            for (const GraphResidencyRoute& route : result.inputs) {
                if (!route.AlreadyResident()) {
                    if (route.region.Size() > UINT64_MAX - result.transfer.bytes ||
                        route.estimatedNanoseconds > UINT64_MAX - result.transfer.nanoseconds) {
                        return std::unexpected(DispatchCandidateEnumerationError{
                            DispatchCandidateEnumerationErrorCode::CapacityExceeded, node});
                    }
                    result.transfer.bytes += route.region.Size();
                    result.transfer.nanoseconds += route.estimatedNanoseconds;
                }
            }
            if (!Detail::CanAssembleDispatchInputs(result, services.resources)) {
                return std::unexpected(DispatchCandidateEnumerationError{
                    DispatchCandidateEnumerationErrorCode::InputAssemblyUnavailable, node});
            }
            return result;
        } catch (const std::bad_alloc&) {
            return std::unexpected(
                DispatchCandidateEnumerationError{DispatchCandidateEnumerationErrorCode::OutOfMemory, node});
        }
    }

    namespace {

        [[nodiscard]] bool ValidResidencyProjection(const DispatchResidencyProjection& projection) noexcept {
            if (!projection.service.Valid() || !projection.device.Valid()) {
                return false;
            }
            if (projection.bindings.empty()) {
                return !projection.destination && projection.inputs.empty();
            }
            return projection.destination && projection.destination->service == projection.service &&
                   projection.destination->location.Valid() &&
                   projection.destination->location.device == projection.device;
        }

        [[nodiscard]] std::expected<void, GraphResidencyError>
        AppendResidencyUpdate(GraphNodeId node, const DispatchResidencyProjection& projection,
                              const GraphResidencyState& residency, GraphResidencyUpdate& update) {
            if (!ValidResidencyProjection(projection)) {
                return std::unexpected(GraphResidencyError::InvalidRequest);
            }
            for (const GraphResidencyRoute& route : projection.inputs) {
                if (!route.Valid() || route.destinationService != projection.service || !projection.destination ||
                    route.destination != projection.destination->location) {
                    return std::unexpected(GraphResidencyError::InvalidRequest);
                }
                if (route.AlreadyResident()) {
                    continue;
                }
                std::optional<GraphNodeId> producer;
                if (const auto* source = std::get_if<GraphLocalResidencySource>(&route.source)) {
                    if (source->producer == node || source->replica.materializer == node) {
                        return std::unexpected(GraphResidencyError::InvalidRequest);
                    }
                    producer = source->producer;
                }
                update.replicas.push_back({.buffer = route.buffer,
                                           .region = route.region,
                                           .byteFootprint = route.byteFootprint,
                                           .version = route.version,
                                           .producer = producer,
                                           .replica = {.service = route.destinationService,
                                                       .location = route.destination,
                                                       .materializer = node}});
            }
            for (const DispatchMemoryBinding& binding : projection.bindings) {
                if (!Writable(binding.access)) {
                    continue;
                }
                const auto baseVersion = residency.BaseVersion(binding.buffer.Describe().id);
                if (!baseVersion) {
                    return std::unexpected(GraphResidencyError::MissingBuffer);
                }
                if (*baseVersion == std::numeric_limits<uint64_t>::max()) {
                    return std::unexpected(GraphResidencyError::VersionExhausted);
                }
                update.writes.push_back({.buffer = binding.buffer.Describe().id,
                                         .region = binding.region,
                                         .byteFootprint = binding.byteFootprint,
                                         .version = *baseVersion + 1,
                                         .producer = node,
                                         .replica = {.service = projection.service,
                                                     .location = projection.destination->location,
                                                     .materializer = node}});
            }
            return {};
        }

        void CanonicalizeSingleDispatchWrites(std::vector<ProducedResidencyWrite>& writes) {
            std::ranges::sort(writes, {}, [](const ProducedResidencyWrite& write) {
                return std::tuple{write.buffer, write.region.Offset()};
            });
            size_t output = 0;
            for (const ProducedResidencyWrite& write : writes) {
                if (output != 0 && writes[output - 1].buffer == write.buffer &&
                    writes[output - 1].Coverage() == write.Coverage()) {
                    continue;
                }
                if (output != 0 && !writes[output - 1].byteFootprint && !write.byteFootprint &&
                    writes[output - 1].buffer == write.buffer &&
                    write.region.Offset() <= writes[output - 1].region.End()) {
                    const uint64_t end = std::max(writes[output - 1].region.End(), write.region.End());
                    writes[output - 1].region =
                        *ByteRegion::Make(writes[output - 1].region.Offset(), end - writes[output - 1].region.Offset());
                    continue;
                }
                writes[output++] = write;
            }
            writes.resize(output);
        }

        [[nodiscard]] std::expected<void, GraphResidencyError>
        ValidatePartitionedWriteCover(std::span<const DispatchMemoryBinding> logicalBindings,
                                      std::span<const DispatchResidencyProjection> projections) {
            constexpr size_t coverageCapacity = 256;
            if (projections.size() > coverageCapacity) {
                return std::unexpected(GraphResidencyError::CapacityExceeded);
            }
            std::array<ByteCoverageView, coverageCapacity> coverages{};
            bool hasWrite = false;
            for (size_t bindingIndex = 0; bindingIndex < logicalBindings.size(); ++bindingIndex) {
                const DispatchMemoryBinding& logical = logicalBindings[bindingIndex];
                if (!Writable(logical.access)) {
                    continue;
                }
                hasWrite = true;
                const ByteCoverageView target = BindingCoverage(logical);
                for (size_t projectionIndex = 0; projectionIndex < projections.size(); ++projectionIndex) {
                    const ByteCoverageView coverage =
                        BindingCoverage(projections[projectionIndex].bindings[bindingIndex]);
                    for (size_t prior = 0; prior < projectionIndex; ++prior) {
                        const CoverageOverlap relation = CoverageRelationWithin(target, coverages[prior], coverage);
                        if (relation == CoverageOverlap::Overlap) {
                            return std::unexpected(GraphResidencyError::OverlappingWrites);
                        }
                        if (relation == CoverageOverlap::Unknown) {
                            return std::unexpected(GraphResidencyError::IndeterminateCoverage);
                        }
                    }
                    coverages[projectionIndex] = coverage;
                }
                const CoverageEquivalence complete =
                    ExactCoverageUnion<coverageCapacity>(target, {coverages.data(), projections.size()});
                if (complete != CoverageEquivalence::Equal) {
                    return std::unexpected(complete == CoverageEquivalence::Unknown
                                               ? GraphResidencyError::IndeterminateCoverage
                                               : GraphResidencyError::InvalidRequest);
                }
            }
            return hasWrite ? std::expected<void, GraphResidencyError>{}
                            : std::unexpected(GraphResidencyError::InvalidRequest);
        }

        [[nodiscard]] std::expected<GraphResidencyUpdate, GraphResidencyError>
        BuildDispatchResidencyUpdate(GraphNodeId node, const DispatchResidencyProjection& projection,
                                     const GraphResidencyState& residency) {
            GraphResidencyUpdate update;
            update.replicas.reserve(projection.inputs.size());
            update.writes.reserve(projection.bindings.size());
            if (auto appended = AppendResidencyUpdate(node, projection, residency, update); !appended) {
                return std::unexpected(appended.error());
            }
            CanonicalizeSingleDispatchWrites(update.writes);
            return update;
        }

    } // namespace

    std::expected<void, GraphResidencyError>
    CommitDispatchResidencySelection(GraphNodeId node, const DispatchResidencyProjection& projection,
                                     GraphResidencyState& residency) noexcept {
        if (!node.Valid()) {
            return std::unexpected(GraphResidencyError::InvalidRequest);
        }
        try {
            auto update = BuildDispatchResidencyUpdate(node, projection, residency);
            if (!update) {
                return std::unexpected(update.error());
            }
            return residency.Apply(*update);
        } catch (const std::bad_alloc&) {
            return std::unexpected(GraphResidencyError::OutOfMemory);
        }
    }

    std::expected<GraphResidencyState, GraphResidencyError>
    CommitDispatchResidencySelection(GraphNodeId node, const DispatchResidencyProjection& projection,
                                     GraphResidencyState&& residency) noexcept {
        if (!node.Valid()) {
            return std::unexpected(GraphResidencyError::InvalidRequest);
        }
        try {
            auto update = BuildDispatchResidencyUpdate(node, projection, residency);
            if (!update) {
                return std::unexpected(update.error());
            }
            return std::move(residency).Transition(*update);
        } catch (const std::bad_alloc&) {
            return std::unexpected(GraphResidencyError::OutOfMemory);
        }
    }

    std::expected<void, GraphResidencyError> CommitPartitionedDispatchResidencySelection(
        GraphNodeId node, std::span<const DispatchMemoryBinding> logicalBindings,
        std::span<const DispatchResidencyProjection> projections, GraphResidencyState& residency) noexcept {
        if (!node.Valid() || logicalBindings.empty() || projections.size() < 2) {
            return std::unexpected(GraphResidencyError::InvalidRequest);
        }
        try {
            size_t replicaCount = 0;
            size_t writeCount = 0;
            for (const DispatchResidencyProjection& projection : projections) {
                if (projection.bindings.size() != logicalBindings.size() ||
                    !std::ranges::equal(projection.bindings, logicalBindings, SameBindingSchema)) {
                    return std::unexpected(GraphResidencyError::InvalidRequest);
                }
                if (projection.inputs.size() > std::numeric_limits<size_t>::max() - replicaCount ||
                    projection.bindings.size() > std::numeric_limits<size_t>::max() - writeCount) {
                    return std::unexpected(GraphResidencyError::CapacityExceeded);
                }
                replicaCount += projection.inputs.size();
                writeCount += projection.bindings.size();
            }
            if (auto valid = ValidatePartitionedWriteCover(logicalBindings, projections); !valid) {
                return std::unexpected(valid.error());
            }
            GraphResidencyUpdate update;
            update.replicas.reserve(replicaCount);
            update.writes.reserve(writeCount);
            for (const DispatchResidencyProjection& projection : projections) {
                const size_t firstWrite = update.writes.size();
                if (auto appended = AppendResidencyUpdate(node, projection, residency, update); !appended) {
                    return std::unexpected(appended.error());
                }
                if (update.writes.size() == firstWrite) {
                    return std::unexpected(GraphResidencyError::InvalidRequest);
                }
            }
            return residency.Apply(update);
        } catch (const std::bad_alloc&) {
            return std::unexpected(GraphResidencyError::OutOfMemory);
        }
    }

} // namespace Sora::Compute

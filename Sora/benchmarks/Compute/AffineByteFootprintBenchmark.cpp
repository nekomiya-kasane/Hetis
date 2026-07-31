/**
 * @file AffineByteFootprintBenchmark.cpp
 * @brief Control-plane baseline for bounded affine-footprint projection.
 */

#include <BenchmarkHarness.h>
#include <Sora/Compute/Memory/AffineByteFootprint.h>
#include <Sora/Compute/Memory/ByteCoverage.h>
#include <Sora/Compute/Memory/ResidencyTable.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <print>

namespace {

    constexpr size_t kSamples = 9;
    constexpr auto kMinimumSampleTime = std::chrono::milliseconds(50);

    template<size_t Capacity>
    [[clang::noinline]] uint64_t Project(const Sora::Compute::AffineByteFootprint& footprint) noexcept {
        const auto projection = footprint.ProjectCanonicalRegions<Capacity>();
        if (!projection) {
            return static_cast<uint64_t>(projection.error()) + 1;
        }
        uint64_t checksum = projection->Size();
        for (const Sora::Compute::ByteRegion region : projection->Regions()) {
            checksum += region.Offset() + region.Size();
        }
        return checksum;
    }

    [[clang::noinline]] uint64_t Contains(Sora::Compute::ByteCoverageView coverage,
                                          Sora::Compute::ByteRegion requested) noexcept {
        return static_cast<uint64_t>(coverage.Contains(requested));
    }

    [[clang::noinline]] uint64_t Resolve(const Sora::Compute::TranslatedByteCoverage& mapping,
                                         Sora::Compute::ByteRegion requested) noexcept {
        const auto resolved = mapping.Resolve(requested);
        return resolved ? resolved->Offset() + resolved->Size() : static_cast<uint64_t>(resolved.error()) + 1;
    }

    template<size_t Capacity, size_t PartCount>
    [[clang::noinline]] uint64_t
    ExactUnion(Sora::Compute::ByteCoverageView target,
               const std::array<Sora::Compute::ByteCoverageView, PartCount>& parts) noexcept {
        return static_cast<uint64_t>(Sora::Compute::ExactCoverageUnion<Capacity>(target, parts)) + 1;
    }

    template<size_t Capacity, size_t RemovedCount>
    [[clang::noinline]] uint64_t
    Complement(Sora::Compute::ByteCoverageView target,
               const std::array<Sora::Compute::ByteCoverageView, RemovedCount>& removed) noexcept {
        const auto complement = Sora::Compute::AffineCoverageComplement<Capacity>::Create(target, removed);
        if (!complement) {
            return static_cast<uint64_t>(complement.error()) + 1;
        }
        uint64_t checksum = complement->Size();
        for (const Sora::Compute::AffineByteFootprint& footprint : complement->Footprints()) {
            checksum += footprint.OffsetBytes() + footprint.OccupiedByteCount();
        }
        return checksum;
    }

    template<size_t Capacity, size_t RemovedCount>
    [[clang::noinline]] uint64_t
    ComplementWithin(Sora::Compute::ByteCoverageView domain, Sora::Compute::ByteCoverageView target,
                     const std::array<Sora::Compute::ByteCoverageView, RemovedCount>& removed) noexcept {
        const auto complement = Sora::Compute::AffineCoverageComplement<Capacity>::Create(domain, target, removed);
        if (!complement) {
            return static_cast<uint64_t>(complement.error()) + 1;
        }
        uint64_t checksum = complement->Size();
        for (const Sora::Compute::AffineByteFootprint& footprint : complement->Footprints()) {
            checksum += footprint.OffsetBytes() + footprint.OccupiedByteCount();
        }
        return checksum;
    }

    [[clang::noinline]] uint64_t Intersect(Sora::Compute::ByteCoverageView domain, Sora::Compute::ByteCoverageView left,
                                           Sora::Compute::ByteCoverageView right) noexcept {
        const auto intersection = Sora::Compute::IntersectAffineSubviews(domain, left, right);
        if (!intersection) {
            return static_cast<uint64_t>(intersection.error()) + 1;
        }
        return *intersection ? (**intersection).OffsetBytes() + (**intersection).OccupiedByteCount() : 1;
    }

    [[clang::noinline]] uint64_t ReadyLookup(const Sora::Compute::ResidentReplica& replica,
                                             Sora::Compute::ByteCoverageView requested) noexcept {
        const auto offset = replica.ResolveAllocationOffset(requested);
        return replica.Covers(1, requested) && offset ? *offset + 1 : 0;
    }

    void DestroyUint(void* state) noexcept {
        delete static_cast<uint32_t*>(state);
    }

} // namespace

int main(int argc, char**) {
    const uint64_t elementCount = (uint64_t{1} << 20) + static_cast<uint64_t>(argc);
    const std::array extents{elementCount};
    constexpr std::array contiguousStrides{uint64_t{4}};
    constexpr std::array sparseStrides{uint64_t{8}};
    const auto contiguous = Sora::Compute::AffineByteFootprint::Create(extents, contiguousStrides, 4, 0, 4, 1);
    const auto soa =
        Sora::Compute::AffineByteFootprint::Create(extents, contiguousStrides, elementCount * 4 + 64, 0, 4, 4);
    const auto sparse = Sora::Compute::AffineByteFootprint::Create(extents, sparseStrides, 4, 0, 4, 1);
    if (!contiguous || !soa || !sparse) {
        return 2;
    }
    const auto soaCoverage = Sora::Compute::ByteCoverageView::Affine(*soa);
    const auto soaMapping =
        Sora::Compute::TranslatedByteCoverage::Create(soaCoverage, 128, soaCoverage.BoundingRegion().Size() + 128);
    const auto soaScalar = Sora::Compute::ByteRegion::Make(soa->ComponentStrideBytes(), 4);
    if (!soaMapping || !soaScalar) {
        return 3;
    }
    const uint64_t firstCount = elementCount / 2;
    const uint64_t secondCount = elementCount - firstCount;
    const std::array firstExtents{firstCount};
    const std::array secondExtents{secondCount};
    const uint64_t overlapStart = firstCount / 2;
    const std::array overlapExtents{firstCount};
    const auto sparseFirst = Sora::Compute::AffineByteFootprint::Create(firstExtents, sparseStrides, 4, 0, 4, 1);
    const auto sparseSecond =
        Sora::Compute::AffineByteFootprint::Create(secondExtents, sparseStrides, 4, firstCount * 8, 4, 1);
    const auto sparseOverlap =
        Sora::Compute::AffineByteFootprint::Create(overlapExtents, sparseStrides, 4, overlapStart * 8, 4, 1);
    const auto contiguousFirst = Sora::Compute::ByteRegion::Make(0, firstCount * 4);
    const auto contiguousSecond = Sora::Compute::ByteRegion::Make(firstCount * 4, secondCount * 4);
    if (!sparseFirst || !sparseSecond || !sparseOverlap || !contiguousFirst || !contiguousSecond) {
        return 4;
    }
    const std::array sparseParts{Sora::Compute::ByteCoverageView::Affine(*sparseFirst),
                                 Sora::Compute::ByteCoverageView::Affine(*sparseSecond)};
    const std::array contiguousParts{Sora::Compute::ByteCoverageView::Interval(*contiguousFirst),
                                     Sora::Compute::ByteCoverageView::Interval(*contiguousSecond)};

    auto code = Sora::Compute::BackendCodeLease::Retain(std::make_shared<uint32_t>());
    auto service = Sora::Compute::BackendService::Adopt(
        {.id = {0xB032, 1}, .state = new uint32_t{}, .destroy = DestroyUint}, std::move(code));
    if (!service) {
        return 5;
    }
    auto allocation = Sora::Compute::Allocation::Adopt(
        *service, {.location = {.domain = {1, 1}, .device = {2, 2}, .backend = Sora::Compute::BackendKind::Cpu},
                   .memory = {3, 3},
                   .properties = Sora::Compute::MemoryProperty::HostVisible,
                   .sizeBytes = sparse->BoundingRegion().Size(),
                   .alignment = 64,
                   .state = new uint32_t{},
                   .destroy = DestroyUint});
    if (!allocation) {
        return 6;
    }
    auto ready = Sora::Compute::ResidentReplica::Create(std::move(*allocation),
                                                        Sora::Compute::ByteCoverageView::Affine(*sparse), 0, 1);
    if (!ready) {
        return 7;
    }

    uint64_t checksum = 0;
    auto contiguousCase = [&]() noexcept { checksum += Project<1>(*contiguous); };
    auto soaCase = [&]() noexcept { checksum += Project<4>(*soa); };
    auto sparseCase = [&]() noexcept { checksum += Project<8>(*sparse); };
    auto containsCase = [&]() noexcept { checksum += Contains(soaCoverage, *soaScalar); };
    auto mappingCase = [&]() noexcept { checksum += Resolve(*soaMapping, *soaScalar); };
    auto contiguousUnionCase = [&]() noexcept {
        checksum += ExactUnion<4>(Sora::Compute::ByteCoverageView::Affine(*contiguous), contiguousParts);
    };
    auto affineUnionCase = [&]() noexcept {
        checksum += ExactUnion<1>(Sora::Compute::ByteCoverageView::Affine(*sparse), sparseParts);
    };
    const std::array sparseRemoved{sparseParts[0]};
    const std::array overlappingRemoval{Sora::Compute::ByteCoverageView::Affine(*sparseOverlap)};
    auto affineComplementCase = [&]() noexcept {
        checksum += Complement<2>(Sora::Compute::ByteCoverageView::Affine(*sparse), sparseRemoved);
    };
    auto affineSharedDomainComplementCase = [&]() noexcept {
        checksum += ComplementWithin<2>(Sora::Compute::ByteCoverageView::Affine(*sparse),
                                        Sora::Compute::ByteCoverageView::Affine(*sparse), sparseRemoved);
    };
    auto affineSiblingComplementCase = [&]() noexcept {
        checksum +=
            ComplementWithin<2>(Sora::Compute::ByteCoverageView::Affine(*sparse), sparseParts[0], overlappingRemoval);
    };
    auto affineIntersectionCase = [&]() noexcept {
        checksum += Intersect(Sora::Compute::ByteCoverageView::Affine(*sparse), sparseParts[0],
                              Sora::Compute::ByteCoverageView::Affine(*sparseOverlap));
    };
    auto readyLookupCase = [&]() noexcept { checksum += ReadyLookup(*ready, sparseParts[1]); };
    const std::array cases{
        Sora::Benchmark::MakeCase("contiguous", contiguousCase),
        Sora::Benchmark::MakeCase("padded_soa_4", soaCase),
        Sora::Benchmark::MakeCase("sparse_capacity_rejection", sparseCase),
        Sora::Benchmark::MakeCase("affine_scalar_containment", containsCase),
        Sora::Benchmark::MakeCase("affine_scalar_translation", mappingCase),
        Sora::Benchmark::MakeCase("contiguous_exact_union", contiguousUnionCase),
        Sora::Benchmark::MakeCase("affine_partition_exact_union", affineUnionCase),
        Sora::Benchmark::MakeCase("affine_half_complement", affineComplementCase),
        Sora::Benchmark::MakeCase("affine_shared_domain_half_complement", affineSharedDomainComplementCase),
        Sora::Benchmark::MakeCase("affine_sibling_complement", affineSiblingComplementCase),
        Sora::Benchmark::MakeCase("affine_intersection", affineIntersectionCase),
        Sora::Benchmark::MakeCase("ready_affine_lookup", readyLookupCase)};
    const auto timing = Sora::Benchmark::MeasureInterleavedIndependent<kSamples>(cases, 1, kMinimumSampleTime);
    if (checksum == 0) {
        return 3;
    }

    std::println("metric,value,unit");
    std::println("affine_footprint_contiguous_projection,{:.6f},ns/projection", timing.MedianTime(0));
    std::println("affine_footprint_padded_soa_4_projection,{:.6f},ns/projection", timing.MedianTime(1));
    std::println("affine_footprint_sparse_capacity_rejection,{:.6f},ns/rejection", timing.MedianTime(2));
    std::println("byte_coverage_affine_scalar_containment,{:.6f},ns/query", timing.MedianTime(3));
    std::println("byte_coverage_affine_scalar_translation,{:.6f},ns/query", timing.MedianTime(4));
    std::println("byte_coverage_contiguous_exact_union,{:.6f},ns/proof", timing.MedianTime(5));
    std::println("byte_coverage_affine_partition_exact_union,{:.6f},ns/proof", timing.MedianTime(6));
    std::println("byte_coverage_affine_half_complement,{:.6f},ns/difference", timing.MedianTime(7));
    std::println("byte_coverage_affine_shared_domain_half_complement,{:.6f},ns/difference", timing.MedianTime(8));
    std::println("byte_coverage_affine_sibling_complement,{:.6f},ns/difference", timing.MedianTime(9));
    std::println("byte_coverage_affine_intersection,{:.6f},ns/intersection", timing.MedianTime(10));
    std::println("residency_ready_affine_lookup,{:.6f},ns/query", timing.MedianTime(11));
    std::println("byte_coverage_view_size,{},bytes", sizeof(Sora::Compute::ByteCoverageView));
    std::println("translated_byte_coverage_size,{},bytes", sizeof(Sora::Compute::TranslatedByteCoverage));
    std::println("checksum,{},value", checksum);
}

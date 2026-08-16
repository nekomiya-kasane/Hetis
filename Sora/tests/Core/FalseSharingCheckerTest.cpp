#include <Sora/Core/Concurrency/FalseSharingChecker.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

    using Sora::Platform::kCacheLineSize;

    struct SameGenericSingleWriterLine {
        [[= Sora::$::ProducerOwned{}]] std::uint32_t first{};
        [[= Sora::$::ProducerOwned{}]] std::uint32_t second{};
    };

    struct CohesiveWriter : Sora::$::CohesiveSingleWriterContentionDomain {};

    struct StatefulWriter : Sora::$::SingleWriterContentionDomain {
        std::uint32_t state{};
    };

    struct StatefulCoverage : Sora::$::ContentionRegionCoverage {
        std::uint32_t state{};
    };

    struct MixedAnnotation : Sora::$::ProducerOwned, Sora::$::ContentionRegionCoverage {};

    struct SameCohesiveSingleWriterLine {
        [[= CohesiveWriter{}]] std::uint32_t first{};
        [[= CohesiveWriter{}]] std::uint32_t second{};
    };

    struct DistinctWritersLine {
        [[= Sora::$::ProducerOwned{}]] std::uint32_t produced{};
        [[= Sora::$::ConsumerOwned{}]] std::uint32_t consumed{};
    };

    struct ReadWriteLine {
        [[= Sora::$::Immutable{}]] std::uint32_t configuration{};
        [[= Sora::$::ProducerOwned{}]] std::uint32_t progress{};
    };

    struct SameMultiWriterLine {
        [[= Sora::$::Contended{}]] std::uint32_t head{};
        [[= Sora::$::Contended{}]] std::uint32_t tail{};
    };

    struct PlacementDependentConflict {
        [[= Sora::$::Inactive{}]] std::array<std::byte, 56> padding{};
        [[= Sora::$::ProducerOwned{}]] std::uint64_t produced{};
        [[= Sora::$::ConsumerOwned{}]] std::uint64_t consumed{};
    };

    struct PlacementDependentSpan {
        [[= Sora::$::ProducerOwned{}]] std::array<std::uint64_t, 2> values{};
    };

    struct alignas(kCacheLineSize) FullyIsolated {
        [[= Sora::$::ProducerOwned{}]] std::uint64_t produced{};
        [[= Sora::$::ConsumerOwned{}]] alignas(kCacheLineSize) std::uint64_t consumed{};
    };

    struct UnclassifiedMember {
        [[= Sora::$::ProducerOwned{}]] std::uint32_t classified{};
        std::uint32_t missing{};
    };

    struct ClassifiedBase {
        [[= Sora::$::ProducerOwned{}]] std::uint32_t value{};
    };

    struct UnsupportedBaseLayout : ClassifiedBase {
        [[= Sora::$::ConsumerOwned{}]] std::uint32_t value{};
    };

    struct HiddenRoles {
        [[= Sora::$::ProducerOwned{}]] std::uint64_t produced{};
        [[= Sora::$::ConsumerOwned{}]] std::uint64_t consumed{};
    };

    struct alignas(kCacheLineSize) InternallyIsolatedRoles {
        [[= Sora::$::ProducerOwned{}]] std::uint64_t produced{};
        [[= Sora::$::ConsumerOwned{}]] alignas(kCacheLineSize) std::uint64_t consumed{};
    };

    struct alignas(kCacheLineSize) UncoveredCompositeRegion {
        [[= Sora::$::SharedStorage{}]] HiddenRoles roles{};
    };

    struct alignas(kCacheLineSize) CoveredCompositeRegion {
        [[= Sora::$::SharedStorage{}]] [[= Sora::$::InternallyIsolatedContentionRegion{}]] InternallyIsolatedRoles
            roles{};
    };

    template<typename T>
    concept CacheLayoutAnalyzable = requires { Sora::Meta::AnalyzeCacheLayout<T>(); };

} // namespace

TEST_CASE("False-sharing audit distinguishes access policies", "[Sora][Core][Concurrency][FalseSharing]") {
    STATIC_REQUIRE(Sora::Concept::ContentionDomainTag<CohesiveWriter>);
    STATIC_REQUIRE_FALSE(Sora::Concept::ContentionDomainTag<StatefulWriter>);
    STATIC_REQUIRE_FALSE(Sora::Concept::ContentionRegionCoverageTag<StatefulCoverage>);
    STATIC_REQUIRE_FALSE(Sora::Concept::ContentionDomainTag<MixedAnnotation>);
    STATIC_REQUIRE_FALSE(Sora::Concept::ContentionRegionCoverageTag<MixedAnnotation>);
    STATIC_REQUIRE_FALSE(Sora::Meta::AuditInternalFalseSharing<SameGenericSingleWriterLine>());
    STATIC_REQUIRE(Sora::Meta::AuditInternalFalseSharing<SameCohesiveSingleWriterLine>());
    STATIC_REQUIRE_FALSE(Sora::Meta::AuditInternalFalseSharing<DistinctWritersLine>());
    STATIC_REQUIRE_FALSE(Sora::Meta::AuditInternalFalseSharing<ReadWriteLine>());
    STATIC_REQUIRE_FALSE(Sora::Meta::AuditInternalFalseSharing<SameMultiWriterLine>());
}

TEST_CASE("False-sharing audit considers every aligned object placement", "[Sora][Core][Concurrency][FalseSharing]") {
    STATIC_REQUIRE(alignof(PlacementDependentConflict) == alignof(std::uint64_t));
    STATIC_REQUIRE_FALSE(Sora::Meta::AuditInternalFalseSharing<PlacementDependentConflict>());
    STATIC_REQUIRE_FALSE(
        Sora::Traits::IsDomainStartsAlignedToCacheLine<PlacementDependentSpan, Sora::$::ProducerOwned>);
    STATIC_REQUIRE(Sora::Traits::DomainLineSpanOf<PlacementDependentSpan, Sora::$::ProducerOwned> == 2);
}

TEST_CASE("Complete false-sharing audit composes internal and external isolation",
          "[Sora][Core][Concurrency][FalseSharing]") {
    constexpr auto local = Sora::Meta::AnalyzeCacheLayout<SameCohesiveSingleWriterLine>();
    STATIC_REQUIRE(local.directMembersValid);
    STATIC_REQUIRE(local.internalValid);
    STATIC_REQUIRE_FALSE(local.externalValid);
    STATIC_REQUIRE_FALSE(local.valid);

    constexpr auto isolated = Sora::Meta::AnalyzeCacheLayout<FullyIsolated>();
    STATIC_REQUIRE(isolated.internalValid);
    STATIC_REQUIRE(isolated.externalValid);
    STATIC_REQUIRE(isolated.valid);
    STATIC_REQUIRE(Sora::Meta::AuditFalseSharing<FullyIsolated>());
}

TEST_CASE("False-sharing audit rejects incomplete or unsupported classifications",
          "[Sora][Core][Concurrency][FalseSharing]") {
    STATIC_REQUIRE_FALSE(Sora::Meta::AnalyzeCacheLayout<UnclassifiedMember>().classifiedAll);
    STATIC_REQUIRE_FALSE(Sora::Meta::AuditInternalFalseSharing<UnclassifiedMember>());
    STATIC_REQUIRE_FALSE(Sora::Meta::AnalyzeCacheLayout<UnsupportedBaseLayout>().layoutSupported);
    STATIC_REQUIRE_FALSE(Sora::Meta::AuditInternalFalseSharing<UnsupportedBaseLayout>());
    STATIC_REQUIRE_FALSE(CacheLayoutAnalyzable<int>);
}

TEST_CASE("Composite regions require an explicit coverage proof", "[Sora][Core][Concurrency][FalseSharing]") {
    constexpr auto uncovered = Sora::Meta::AnalyzeCacheLayout<UncoveredCompositeRegion>();
    STATIC_REQUIRE(uncovered.directMembersValid);
    STATIC_REQUIRE_FALSE(uncovered.coversAllRegions);
    STATIC_REQUIRE_FALSE(uncovered.internalValid);
    STATIC_REQUIRE_FALSE(uncovered.valid);

    constexpr auto covered = Sora::Meta::AnalyzeCacheLayout<CoveredCompositeRegion>();
    STATIC_REQUIRE(covered.coversAllRegions);
    STATIC_REQUIRE(covered.internalValid);
    STATIC_REQUIRE(covered.valid);
}

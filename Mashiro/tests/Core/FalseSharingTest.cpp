/**
 * @file FalseSharingTest.cpp
 * @brief Compile-time tests for the project-wide false-sharing audit framework.
 *
 * The audit is entirely `consteval`, so every assertion is a `STATIC_REQUIRE` over a representative
 * fixture: correct cross-domain isolation, same-domain co-location, the @ref Immutable exemption,
 * unclassified-member detection, whole-line occupancy, an open user-declared domain, and the
 * targeted `DomainStartsLine` / `DomainLineSpan` queries.
 */
#include "Mashiro/Core/FalseSharing.h"

#include "Support/Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

using namespace Mashiro;
using namespace Mashiro::Concurrency;

// =============================================================================
// Fixtures (namespace scope so they are reflectable)
// =============================================================================

namespace {

    constexpr size_t kLine = Platform::kCacheLineSize;

    /// @brief Producer and consumer state on separate cache lines — the canonical good layout.
    struct alignas(kLine) WellSeparated {
        [[=ProducerOwned{}]] alignas(kLine) std::atomic<uint32_t> tail{0};
        [[=ProducerOwned{}]] uint32_t cachedHead{0};
        [[=ConsumerOwned{}]] alignas(kLine) std::atomic<uint32_t> head{0};
        [[=ConsumerOwned{}]] uint32_t cachedTail{0};
        [[=SharedStorage{}]] alignas(kLine) std::byte slots[kLine];
    };

    /// @brief Two distinct active domains forced onto one line — must be flagged.
    struct Colliding {
        [[=ProducerOwned{}]] std::atomic<uint32_t> tail{0};
        [[=ConsumerOwned{}]] std::atomic<uint32_t> head{0}; // same line as tail
    };

    /// @brief Same domain sharing a line is legitimate (one writer, no conflict).
    struct SameDomainShared {
        [[=ProducerOwned{}]] std::atomic<uint32_t> a{0};
        [[=ProducerOwned{}]] uint32_t b{0}; // same line, same domain — fine
    };

    /// @brief An immutable field co-located with a hot word: exempt from conflict.
    struct WithImmutable {
        [[=Contended{}]] std::atomic<uint64_t> hot{0};
        [[=Immutable{}]] uint64_t config{0}; // read-only after ctor — never false-shares
    };

    /// @brief A member with no domain tag — classification must fail.
    struct Unclassified {
        [[=ProducerOwned{}]] std::atomic<uint32_t> tagged{0};
        uint32_t untagged{0}; // missing a domain
    };

    /// @brief A member with two domain tags — classification must fail.
    struct DoubleTagged {
        [[=ProducerOwned{}]] [[=ConsumerOwned{}]] std::atomic<uint32_t> both{0};
    };

    /// @brief Open vocabulary: a container may declare its own domain.
    struct GpuQueueOwned : ContentionDomain {};
    struct CustomDomain {
        [[=GpuQueueOwned{}]] alignas(kLine) std::atomic<uint32_t> submit{0};
        [[=ConsumerOwned{}]] alignas(kLine) std::atomic<uint32_t> retire{0};
    };

} // namespace

// =============================================================================
// [Internal false sharing] — the headline AuditFalseSharing verdict
// =============================================================================

TEST_CASE("AuditFalseSharing passes when domains are cache-line separated", AUTO_TAG) {
    STATIC_REQUIRE(AuditFalseSharing<WellSeparated>());
    constexpr auto r = AnalyzeCacheLayout<WellSeparated>();
    STATIC_REQUIRE(r.classifiedAll);
    STATIC_REQUIRE(!r.hasConflict);
    STATIC_REQUIRE(r.valid);
}

TEST_CASE("AuditFalseSharing flags two active domains on one line", AUTO_TAG) {
    STATIC_REQUIRE(!AuditFalseSharing<Colliding>());
    constexpr auto r = AnalyzeCacheLayout<Colliding>();
    STATIC_REQUIRE(r.classifiedAll); // both members ARE classified...
    STATIC_REQUIRE(r.hasConflict);   // ...but they conflict
}

TEST_CASE("Same domain may share a cache line", AUTO_TAG) {
    STATIC_REQUIRE(AuditFalseSharing<SameDomainShared>());
}

TEST_CASE("Immutable members are exempt from conflict", AUTO_TAG) {
    STATIC_REQUIRE(AuditFalseSharing<WithImmutable>());
    STATIC_REQUIRE(!AnalyzeCacheLayout<WithImmutable>().hasConflict);
}

// =============================================================================
// [Classification] — every member must carry exactly one domain
// =============================================================================

TEST_CASE("An untagged member fails classification", AUTO_TAG) {
    STATIC_REQUIRE(!AuditFalseSharing<Unclassified>());
    STATIC_REQUIRE(!AnalyzeCacheLayout<Unclassified>().classifiedAll);
}

TEST_CASE("A doubly-tagged member fails classification", AUTO_TAG) {
    STATIC_REQUIRE(!AuditFalseSharing<DoubleTagged>());
    STATIC_REQUIRE(!AnalyzeCacheLayout<DoubleTagged>().classifiedAll);
}

// =============================================================================
// [Open vocabulary] — user-declared domains work with no central change
// =============================================================================

TEST_CASE("A user-declared domain participates in the audit", AUTO_TAG) {
    STATIC_REQUIRE(ContentionDomainTag<GpuQueueOwned>);
    STATIC_REQUIRE(AuditFalseSharing<CustomDomain>());
}

// =============================================================================
// [External false sharing] — whole-line occupancy
// =============================================================================

TEST_CASE("OccupiesWholeLines reflects alignment and size", AUTO_TAG) {
    STATIC_REQUIRE(OccupiesWholeLines<WellSeparated>());
    STATIC_REQUIRE(!OccupiesWholeLines<Colliding>()); // tiny, unaligned to a line
    STATIC_REQUIRE(AnalyzeCacheLayout<WellSeparated>().occupiesWholeLines);
}

// =============================================================================
// [Targeted queries] — DomainStartsLine / DomainLineSpan
// =============================================================================

TEST_CASE("DomainStartsLine locates a domain's first member", AUTO_TAG) {
    STATIC_REQUIRE(DomainStartsLine<WellSeparated, ProducerOwned>());
    STATIC_REQUIRE(DomainStartsLine<WellSeparated, ConsumerOwned>());
    STATIC_REQUIRE(DomainStartsLine<WellSeparated, SharedStorage>());
}

TEST_CASE("DomainLineSpan counts the lines a domain touches", AUTO_TAG) {
    // Producer state (tail + cachedHead) is packed into a single line.
    STATIC_REQUIRE(DomainLineSpan<WellSeparated, ProducerOwned>() == 1);
    STATIC_REQUIRE(DomainLineSpan<WellSeparated, ConsumerOwned>() == 1);
    // A domain with no member reports a span of zero.
    STATIC_REQUIRE(DomainLineSpan<WellSeparated, Contended>() == 0);
}

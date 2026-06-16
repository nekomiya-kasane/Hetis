/**
 * @file TrivialOpsTest.cpp
 * @brief Tests for Math/TrivialOps.h — power-of-two, division, alignment, and bit-width helpers.
 *
 * Each helper is exercised through both `STATIC_REQUIRE` (proving the function folds at compile
 * time and is constant-evaluable on every supported unsigned width) and runtime checks for the
 * bigger value range.
 */
#include "Mashiro/Math/TrivialOps.h"

#include "Support/Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

using namespace Mashiro;
using namespace Mashiro::Math;

// =============================================================================
// Section 1 — Power of two
// =============================================================================

TEST_CASE("IsPow2 — exactly the positive powers of two", AUTO_TAG) {
    STATIC_REQUIRE(IsPow2<std::uint32_t>(1));
    STATIC_REQUIRE(IsPow2<std::uint32_t>(2));
    STATIC_REQUIRE(IsPow2<std::uint32_t>(64));
    STATIC_REQUIRE(IsPow2<std::uint64_t>(1ULL << 40));
    STATIC_REQUIRE_FALSE(IsPow2<std::uint32_t>(0));
    STATIC_REQUIRE_FALSE(IsPow2<std::uint32_t>(3));
    STATIC_REQUIRE_FALSE(IsPow2<std::uint32_t>(1000));
}

TEST_CASE("CeilPow2 — round up to power of two; 0 and 1 map to 1", AUTO_TAG) {
    STATIC_REQUIRE(CeilPow2<std::uint32_t>(0) == 1);
    STATIC_REQUIRE(CeilPow2<std::uint32_t>(1) == 1);
    STATIC_REQUIRE(CeilPow2<std::uint32_t>(2) == 2);
    STATIC_REQUIRE(CeilPow2<std::uint32_t>(3) == 4);
    STATIC_REQUIRE(CeilPow2<std::uint32_t>(7) == 8);
    STATIC_REQUIRE(CeilPow2<std::uint32_t>(8) == 8);
    STATIC_REQUIRE(CeilPow2<std::uint32_t>(9) == 16);
    STATIC_REQUIRE(CeilPow2<std::uint64_t>(0x10000000) == 0x10000000);
}

TEST_CASE("FloorPow2 — round down to power of two; 0 maps to 0", AUTO_TAG) {
    STATIC_REQUIRE(FloorPow2<std::uint32_t>(0) == 0);
    STATIC_REQUIRE(FloorPow2<std::uint32_t>(1) == 1);
    STATIC_REQUIRE(FloorPow2<std::uint32_t>(2) == 2);
    STATIC_REQUIRE(FloorPow2<std::uint32_t>(3) == 2);
    STATIC_REQUIRE(FloorPow2<std::uint32_t>(7) == 4);
    STATIC_REQUIRE(FloorPow2<std::uint32_t>(64) == 64);
    STATIC_REQUIRE(FloorPow2<std::uint32_t>(65) == 64);
}

TEST_CASE("Log2Floor — floor(log2(v)); 0 maps to 0 by convention", AUTO_TAG) {
    STATIC_REQUIRE(Log2Floor<std::uint32_t>(0) == 0);
    STATIC_REQUIRE(Log2Floor<std::uint32_t>(1) == 0);
    STATIC_REQUIRE(Log2Floor<std::uint32_t>(2) == 1);
    STATIC_REQUIRE(Log2Floor<std::uint32_t>(7) == 2);
    STATIC_REQUIRE(Log2Floor<std::uint32_t>(8) == 3);
    STATIC_REQUIRE(Log2Floor<std::uint64_t>(1ULL << 40) == 40);
}

TEST_CASE("Log2Ceil — ceil(log2(v)); 0/1 map to 0", AUTO_TAG) {
    STATIC_REQUIRE(Log2Ceil<std::uint32_t>(0) == 0);
    STATIC_REQUIRE(Log2Ceil<std::uint32_t>(1) == 0);
    STATIC_REQUIRE(Log2Ceil<std::uint32_t>(2) == 1);
    STATIC_REQUIRE(Log2Ceil<std::uint32_t>(3) == 2);
    STATIC_REQUIRE(Log2Ceil<std::uint32_t>(8) == 3);
    STATIC_REQUIRE(Log2Ceil<std::uint32_t>(9) == 4);
}

TEST_CASE("CeilPow2 / FloorPow2 / IsPow2 / Log2 roundtrips", AUTO_TAG) {
    for (std::uint32_t v = 1; v <= 1024; ++v) {
        const auto cu = CeilPow2(v);
        const auto fl = FloorPow2(v);
        REQUIRE(IsPow2(cu));
        REQUIRE(IsPow2(fl));
        REQUIRE(fl <= v);
        REQUIRE(v <= cu);
        REQUIRE(Log2Floor(cu) == Log2Ceil(v));
        REQUIRE(Log2Floor(fl) == Log2Floor(v));
    }
}

// =============================================================================
// Section 2 — Division and rounding
// =============================================================================

TEST_CASE("CeilDiv — exact ceiling integer division", AUTO_TAG) {
    STATIC_REQUIRE(CeilDiv<std::uint32_t>(0, 4) == 0);
    STATIC_REQUIRE(CeilDiv<std::uint32_t>(1, 4) == 1);
    STATIC_REQUIRE(CeilDiv<std::uint32_t>(4, 4) == 1);
    STATIC_REQUIRE(CeilDiv<std::uint32_t>(5, 4) == 2);
    STATIC_REQUIRE(CeilDiv<std::uint32_t>(8, 4) == 2);
    STATIC_REQUIRE(CeilDiv<std::uint32_t>(9, 4) == 3);
}

TEST_CASE("RoundUp / RoundDown — general modulus", AUTO_TAG) {
    STATIC_REQUIRE(RoundUp<std::uint32_t>(0, 7) == 0);
    STATIC_REQUIRE(RoundUp<std::uint32_t>(1, 7) == 7);
    STATIC_REQUIRE(RoundUp<std::uint32_t>(7, 7) == 7);
    STATIC_REQUIRE(RoundUp<std::uint32_t>(8, 7) == 14);
    STATIC_REQUIRE(RoundUp<std::uint32_t>(15, 5) == 15);
    STATIC_REQUIRE(RoundUp<std::uint32_t>(16, 5) == 20);

    STATIC_REQUIRE(RoundDown<std::uint32_t>(0, 7) == 0);
    STATIC_REQUIRE(RoundDown<std::uint32_t>(6, 7) == 0);
    STATIC_REQUIRE(RoundDown<std::uint32_t>(13, 7) == 7);
    STATIC_REQUIRE(RoundDown<std::uint32_t>(14, 7) == 14);
}

// =============================================================================
// Section 3 — Alignment (power-of-two modulus)
// =============================================================================

TEST_CASE("AlignUp / AlignDown / IsAlignedTo on power-of-two alignments", AUTO_TAG) {
    STATIC_REQUIRE(AlignUp<std::uint64_t>(0, 16) == 0);
    STATIC_REQUIRE(AlignUp<std::uint64_t>(1, 16) == 16);
    STATIC_REQUIRE(AlignUp<std::uint64_t>(15, 16) == 16);
    STATIC_REQUIRE(AlignUp<std::uint64_t>(16, 16) == 16);
    STATIC_REQUIRE(AlignUp<std::uint64_t>(17, 16) == 32);

    STATIC_REQUIRE(AlignDown<std::uint64_t>(0, 16) == 0);
    STATIC_REQUIRE(AlignDown<std::uint64_t>(15, 16) == 0);
    STATIC_REQUIRE(AlignDown<std::uint64_t>(16, 16) == 16);
    STATIC_REQUIRE(AlignDown<std::uint64_t>(31, 16) == 16);
    STATIC_REQUIRE(AlignDown<std::uint64_t>(32, 16) == 32);

    STATIC_REQUIRE(IsAlignedTo<std::uint64_t>(0, 8));
    STATIC_REQUIRE(IsAlignedTo<std::uint64_t>(8, 8));
    STATIC_REQUIRE(IsAlignedTo<std::uint64_t>(64, 8));
    STATIC_REQUIRE_FALSE(IsAlignedTo<std::uint64_t>(7, 8));
    STATIC_REQUIRE_FALSE(IsAlignedTo<std::uint64_t>(63, 8));
}

TEST_CASE("AlignUp matches RoundUp on power-of-two alignment", AUTO_TAG) {
    // Establishes the alignment helpers as an optimised special case of the general modulus
    // helpers, validating the property the ALIGNMENT API contract is built on.
    for (std::uint32_t v = 0; v < 256; ++v) {
        for (std::uint32_t a : {1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u}) {
            REQUIRE(AlignUp(v, a) == RoundUp(v, a));
            REQUIRE(AlignDown(v, a) == RoundDown(v, a));
            REQUIRE(IsAlignedTo(v, a) == ((v % a) == 0));
        }
    }
}

// =============================================================================
// Section 4 — Width queries
// =============================================================================

TEST_CASE("BitWidth / ByteWidth", AUTO_TAG) {
    STATIC_REQUIRE(BitWidth<std::uint32_t>(0) == 0);
    STATIC_REQUIRE(BitWidth<std::uint32_t>(1) == 1);
    STATIC_REQUIRE(BitWidth<std::uint32_t>(2) == 2);
    STATIC_REQUIRE(BitWidth<std::uint32_t>(255) == 8);
    STATIC_REQUIRE(BitWidth<std::uint32_t>(256) == 9);

    STATIC_REQUIRE(ByteWidth<std::uint32_t>(0) == 0);
    STATIC_REQUIRE(ByteWidth<std::uint32_t>(1) == 1);
    STATIC_REQUIRE(ByteWidth<std::uint32_t>(255) == 1);
    STATIC_REQUIRE(ByteWidth<std::uint32_t>(256) == 2);
    STATIC_REQUIRE(ByteWidth<std::uint64_t>(1ULL << 16) == 3);
    STATIC_REQUIRE(ByteWidth<std::uint64_t>(1ULL << 40) == 6);
}

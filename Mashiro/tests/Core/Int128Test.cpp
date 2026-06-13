/**
 * @file Int128Test.cpp
 * @brief Tests for the 128-bit integer aliases in Core/Int128.h.
 *
 * `Mashiro::uint128_t` / `Mashiro::int128_t` are thin aliases over the
 * toolchain's `__uint128_t` / `__int128` extension. The build mandates the
 * extension via the `HAVE_INT128` CMake probe, so these tests focus on the
 * contract the rest of the codebase relies on: exact width, signedness, and
 * correct arithmetic / bit behaviour across the full 128-bit range. Every
 * property is checked with `STATIC_REQUIRE` so a regression fails the build.
 */
#include "Mashiro/Core/Int128.h"

#include "Support/Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <climits>
#include <cstdint>

using Mashiro::int128_t;
using Mashiro::uint128_t;

namespace {

    // Highest and lowest 64-bit lanes, used to build wide test values.
    inline constexpr uint64_t kHi = 0x0123456789abcdefULL;
    inline constexpr uint64_t kLo = 0xfedcba9876543210ULL;

    consteval uint128_t Compose(uint64_t hi, uint64_t lo) {
        return (static_cast<uint128_t>(hi) << 64) | static_cast<uint128_t>(lo);
    }

}  // namespace

// =============================================================================
// Width, size, and alignment
// =============================================================================

TEST_CASE("128-bit aliases are exactly 16 bytes / 128 bits", AUTO_TAG) {
    STATIC_REQUIRE(sizeof(uint128_t) == 16);
    STATIC_REQUIRE(sizeof(int128_t) == 16);
    STATIC_REQUIRE(sizeof(uint128_t) * CHAR_BIT == 128);
    STATIC_REQUIRE(sizeof(int128_t) * CHAR_BIT == 128);
    STATIC_REQUIRE(alignof(uint128_t) == alignof(int128_t));
}

// =============================================================================
// Signedness (verified through behaviour, not std trait specialisations)
// =============================================================================

TEST_CASE("int128_t is signed: negatives compare below zero", AUTO_TAG) {
    constexpr int128_t neg = -1;
    STATIC_REQUIRE(neg < 0);
    STATIC_REQUIRE(static_cast<int128_t>(neg) + 1 == 0);
    // Arithmetic right shift preserves the sign bit.
    STATIC_REQUIRE((neg >> 1) == neg);
}

TEST_CASE("uint128_t is unsigned: -1 is the all-ones maximum", AUTO_TAG) {
    constexpr uint128_t allOnes = ~static_cast<uint128_t>(0);
    STATIC_REQUIRE(allOnes > 0);
    STATIC_REQUIRE(allOnes + 1 == 0);  // wraps to zero
    // Top and bottom 64-bit lanes are both saturated.
    STATIC_REQUIRE(static_cast<uint64_t>(allOnes) == ~0ULL);
    STATIC_REQUIRE(static_cast<uint64_t>(allOnes >> 64) == ~0ULL);
}

// =============================================================================
// Arithmetic across the 64-bit lane boundary
// =============================================================================

TEST_CASE("addition carries from the low lane into the high lane", AUTO_TAG) {
    constexpr uint128_t lowMax = static_cast<uint128_t>(~0ULL);
    constexpr uint128_t carried = lowMax + 1;
    STATIC_REQUIRE(static_cast<uint64_t>(carried) == 0);
    STATIC_REQUIRE(static_cast<uint64_t>(carried >> 64) == 1);
}

TEST_CASE("multiplication produces a full 128-bit product", AUTO_TAG) {
    // (2^64 - 1)^2 = 2^128 - 2^65 + 1, which only fits in 128 bits.
    constexpr uint128_t f = static_cast<uint128_t>(~0ULL);
    constexpr uint128_t product = f * f;
    STATIC_REQUIRE(static_cast<uint64_t>(product) == 1ULL);
    STATIC_REQUIRE(static_cast<uint64_t>(product >> 64) == 0xfffffffffffffffeULL);
}

TEST_CASE("division and modulo span both lanes", AUTO_TAG) {
    constexpr uint128_t value = Compose(kHi, kLo);
    STATIC_REQUIRE(value / value == 1);
    STATIC_REQUIRE(value % value == 0);
    // Dividing by 2^64 isolates the high lane; the remainder is the low lane.
    constexpr uint128_t shift64 = static_cast<uint128_t>(1) << 64;
    STATIC_REQUIRE(static_cast<uint64_t>(value / shift64) == kHi);
    STATIC_REQUIRE(static_cast<uint64_t>(value % shift64) == kLo);
}

// =============================================================================
// Bitwise and shift behaviour
// =============================================================================

TEST_CASE("compose / decompose round-trips through both lanes", AUTO_TAG) {
    constexpr uint128_t value = Compose(kHi, kLo);
    STATIC_REQUIRE(static_cast<uint64_t>(value >> 64) == kHi);
    STATIC_REQUIRE(static_cast<uint64_t>(value) == kLo);
}

TEST_CASE("shifting moves bits across the lane boundary", AUTO_TAG) {
    constexpr uint128_t one = 1;
    constexpr uint128_t topBit = one << 127;
    STATIC_REQUIRE(static_cast<uint64_t>(topBit >> 64) == (1ULL << 63));
    STATIC_REQUIRE(static_cast<uint64_t>(topBit) == 0);
    STATIC_REQUIRE((topBit >> 127) == 1);
    // A logical right shift on the unsigned type does not sign-extend.
    STATIC_REQUIRE((topBit >> 1) == (one << 126));
}

TEST_CASE("bitwise operators act on the full 128-bit pattern", AUTO_TAG) {
    constexpr uint128_t value = Compose(kHi, kLo);
    STATIC_REQUIRE((value & ~static_cast<uint128_t>(0)) == value);
    STATIC_REQUIRE((value | 0) == value);
    STATIC_REQUIRE((value ^ value) == 0);
    STATIC_REQUIRE(~~value == value);
    STATIC_REQUIRE(static_cast<uint64_t>(~value) == ~kLo);
    STATIC_REQUIRE(static_cast<uint64_t>(~value >> 64) == ~kHi);
}

// =============================================================================
// Signed range and lane-preserving conversions
// =============================================================================

TEST_CASE("int128_t reaches its two's-complement extremes", AUTO_TAG) {
    // Min = -2^127 (only the sign bit set); Max = 2^127 - 1.
    constexpr int128_t min = static_cast<int128_t>(static_cast<uint128_t>(1) << 127);
    constexpr int128_t max = ~min;
    STATIC_REQUIRE(min < 0);
    STATIC_REQUIRE(max > 0);
    STATIC_REQUIRE(min + max == -1);
    // The magnitude of min is unrepresentable as a positive int128_t; negating
    // it would be signed overflow (UB), so we verify the bit pattern via the
    // unsigned domain instead, where the operation is well defined.
    STATIC_REQUIRE(static_cast<uint128_t>(min) == (static_cast<uint128_t>(1) << 127));
    STATIC_REQUIRE(-(static_cast<uint128_t>(min)) == (static_cast<uint128_t>(1) << 127));
}

TEST_CASE("narrowing conversion keeps the low 64 bits", AUTO_TAG) {
    constexpr uint128_t value = Compose(kHi, kLo);
    STATIC_REQUIRE(static_cast<uint64_t>(value) == kLo);
    STATIC_REQUIRE(static_cast<uint32_t>(value) == static_cast<uint32_t>(kLo));
}

TEST_CASE("widening an unsigned 64-bit value zero-extends", AUTO_TAG) {
    constexpr uint128_t widened = static_cast<uint128_t>(kLo);
    STATIC_REQUIRE(static_cast<uint64_t>(widened >> 64) == 0);
    STATIC_REQUIRE(static_cast<uint64_t>(widened) == kLo);
}

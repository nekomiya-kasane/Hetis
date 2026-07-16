#include <Sora/Core/Memory/MemoryLayout.h>
#include <Sora/Core/Traits/ScopeTraits.h>

#include <catch2/catch_test_macros.hpp>

#include <climits>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

using namespace Sora;

namespace {

    struct DenseWords {
        std::uint32_t first{};
        std::uint32_t second{};
    };

    struct PaddedWord {
        std::uint8_t prefix{};
        std::uint32_t value{};
    };

    struct DenseBitFields {
        unsigned first : 16;
        unsigned second : 16;
    };

    struct PaddedBitField {
        unsigned value : 1;
    };

    struct DenseBase {
        std::uint32_t base{};
    };

    struct DenseDerived : DenseBase {
        std::uint32_t derived{};
    };

    struct Empty {};

    struct OverlappingEmpty {
        NO_UNIQUE_ADDRESS Empty marker{};
        std::uint8_t value{};
    };

    struct EmptyBaseDerived : Empty {
        std::uint8_t value{};
    };

    struct PolymorphicValue {
        virtual ~PolymorphicValue() = default;
        std::uint32_t value{};
    };

    struct VirtualBase {
        std::uint32_t value{};
    };

    struct VirtualDerived : virtual VirtualBase {
        std::uint32_t derived{};
    };

} // namespace

static_assert(Concept::CompactClass<DenseWords>);
static_assert(Concept::PaddedClass<PaddedWord>);
static_assert(Traits::PaddingBits<PaddedWord> == 24);
static_assert(Concept::CompactClass<DenseBitFields>);
static_assert(Concept::PaddedClass<PaddedBitField>);
static_assert(Traits::PaddingBits<PaddedBitField> == 31);
static_assert(Concept::CompactClass<DenseDerived>);
static_assert(Concept::CompactClass<OverlappingEmpty>);
static_assert(Concept::CompactClass<EmptyBaseDerived>);
static_assert(Concept::PaddedClass<Empty>);
static_assert(Traits::PaddingBits<Empty> == sizeof(Empty) * CHAR_BIT);
static_assert(!Concept::CompactClass<PolymorphicValue>);
static_assert(!Concept::PaddedClass<PolymorphicValue>);
static_assert(!Concept::CompactClass<VirtualDerived>);
static_assert(!Concept::PaddedClass<VirtualDerived>);

TEST_CASE("Unsigned layout arithmetic is total and overflow-aware", "[Sora][Core][MemoryLayout]") {
    STATIC_REQUIRE(IsPowerOfTwo(std::uint32_t{64}));
    STATIC_REQUIRE_FALSE(IsPowerOfTwo(std::uint32_t{0}));
    STATIC_REQUIRE(FloorPowerOfTwo(std::uint32_t{65}) == 64);
    STATIC_REQUIRE(TryCeilPowerOfTwo(std::uint32_t{65}) == std::optional<std::uint32_t>{128});
    STATIC_REQUIRE_FALSE(TryCeilPowerOfTwo(std::numeric_limits<std::uint32_t>::max()).has_value());
    STATIC_REQUIRE_FALSE(TryCeilPowerOfTwo(std::uint8_t{129}).has_value());

    STATIC_REQUIRE(TryFloorLog2(std::uint32_t{8}) == std::optional<unsigned>{3});
    STATIC_REQUIRE(TryCeilLog2(std::uint32_t{9}) == std::optional<unsigned>{4});
    STATIC_REQUIRE_FALSE(TryFloorLog2(std::uint32_t{0}).has_value());
    STATIC_REQUIRE_FALSE(TryCeilLog2(std::uint32_t{0}).has_value());
    STATIC_REQUIRE(BitWidth(std::uint32_t{256}) == 9);
    STATIC_REQUIRE(ByteWidth(std::uint32_t{256}) == 2);

    STATIC_REQUIRE(TryCeilDivide(std::uint32_t{9}, std::uint32_t{4}) == std::optional<std::uint32_t>{3});
    STATIC_REQUIRE(TryCeilDivide(std::numeric_limits<std::uint8_t>::max(), std::uint8_t{1}) ==
                   std::optional<std::uint8_t>{std::numeric_limits<std::uint8_t>::max()});
    STATIC_REQUIRE_FALSE(TryCeilDivide(std::uint32_t{9}, std::uint32_t{0}).has_value());
    STATIC_REQUIRE(TryRoundUp(std::uint32_t{16}, std::uint32_t{5}) == std::optional<std::uint32_t>{20});
    STATIC_REQUIRE(TryRoundDown(std::uint32_t{16}, std::uint32_t{5}) == std::optional<std::uint32_t>{15});
    STATIC_REQUIRE_FALSE(TryRoundUp(std::numeric_limits<std::uint32_t>::max(), std::uint32_t{2}).has_value());
    STATIC_REQUIRE(TryRoundUp(std::numeric_limits<std::uint8_t>::max(), std::uint8_t{1}) ==
                   std::optional<std::uint8_t>{std::numeric_limits<std::uint8_t>::max()});
    STATIC_REQUIRE_FALSE(TryRoundUp(std::uint32_t{1}, std::uint32_t{0}).has_value());
    STATIC_REQUIRE_FALSE(TryRoundDown(std::uint32_t{1}, std::uint32_t{0}).has_value());

    STATIC_REQUIRE(AlignDown(31, Align{16}) == 16);
}

TEST_CASE("Align stores non-zero power-of-two alignment compactly", "[Sora][Core][MemoryLayout]") {
    static_assert(Align{}.Value() == 1);
    static_assert(Align{8}.Value() == 8);
    static_assert(Align{8}.Log2() == 3);
    static_assert(Align::Constant<4096>().Value() == 4096);
    static_assert(Align::Of<std::max_align_t>().Value() == alignof(std::max_align_t));

    constexpr auto previous = Align{8}.Previous();
    static_assert(previous.has_value());
    static_assert(previous->Value() == 4);
    static_assert(!Align{}.Previous().has_value());

    REQUIRE(Align{64}.Value() == 64);
    REQUIRE(Align{64}.Log2() == 6);
}

TEST_CASE("MaybeAlign preserves undefined and concrete alignment states", "[Sora][Core][MemoryLayout]") {
    constexpr MaybeAlign undefined;
    constexpr MaybeAlign byteAligned{Align{}};
    constexpr MaybeAlign pageAligned{Align::Constant<4096>()};

    static_assert(!undefined);
    static_assert(undefined.ValueOrOne().Value() == 1);
    static_assert(byteAligned.Value().Value() == 1);
    static_assert(pageAligned.Value().Value() == 4096);

    static_assert(Encode(undefined) == 0);
    static_assert(Encode(Align{1}) == 1);
    static_assert(Encode(Align{4096}) == 13);
    static_assert(DecodeMaybeAlign(0) == MaybeAlign{});
    static_assert(DecodeMaybeAlign(13).Value().Value() == 4096);
}

TEST_CASE("Alignment helpers compute checked and modular alignments", "[Sora][Core][MemoryLayout]") {
    REQUIRE(IsPowerOfTwo(std::uint64_t{8}));
    REQUIRE_FALSE(IsPowerOfTwo(std::uint64_t{0}));
    REQUIRE_FALSE(IsPowerOfTwo(std::uint64_t{12}));

    REQUIRE(IsAligned(32, Align{16}));
    REQUIRE_FALSE(IsAligned(40, Align{16}));
    REQUIRE(IsAligned(40, 0));

    REQUIRE(TryAlignUp(17, Align{8}) == std::optional<uint64_t>{24});
    REQUIRE(TryAlignUp(17, Align{8}, 1) == std::optional<uint64_t>{17});
    REQUIRE(TryAlignUp(5, Align{8}, 7) == std::optional<uint64_t>{7});
    REQUIRE_FALSE(TryAlignUp(std::numeric_limits<uint64_t>::max(), Align{8}).has_value());

    REQUIRE(AlignUpModulo(std::numeric_limits<uint64_t>::max(), Align{8}) == 0);
    REQUIRE(TryOffsetToAlignment(17, Align{8}) == std::optional<uint64_t>{7});
    REQUIRE(OffsetToAlignmentModulo(std::numeric_limits<uint64_t>::max(), Align{8}) == 1);
}

TEST_CASE("Range and common-alignment helpers handle boundary cases", "[Sora][Core][MemoryLayout]") {
    REQUIRE(RangesOverlap(0, 8, 7, 1));
    REQUIRE_FALSE(RangesOverlap(0, 8, 8, 8));
    REQUIRE_FALSE(RangesOverlap(0, 0, 0, 8));

    REQUIRE(SaturatingEnd(std::numeric_limits<uint64_t>::max() - 1, 8) == std::numeric_limits<uint64_t>::max());
    REQUIRE(MinimumAlignment(Align{64}.Value(), 96) == 32);
    REQUIRE(CommonAlignment(Align{64}, 96).Value() == 32);
    REQUIRE(CommonAlignment(Align{64}, 0).Value() == 64);
}

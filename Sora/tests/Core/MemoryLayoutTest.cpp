#include <Sora/Core/Memory/MemoryLayout.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

using namespace Sora;

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
    REQUIRE(IsPowerOfTwo(8));
    REQUIRE_FALSE(IsPowerOfTwo(0));
    REQUIRE_FALSE(IsPowerOfTwo(12));

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

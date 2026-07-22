/**
 * @file FlagsTest.cpp
 * @brief Verify reflected sequential-enum sets, constexpr mutation, set algebra, and declaration-order iteration.
 * @ingroup Testing
 */

#include <Sora/Core/Flags.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <type_traits>

namespace {

    enum class Channel : std::uint8_t {
        Red,
        Green,
        Blue,
        Alpha,
    };

    enum class StartsAtOne : std::uint8_t {
        First = 1,
        Second,
    };

    enum class HasGap : std::uint8_t {
        First,
        Third = 2,
    };

    enum class HasAlias : std::uint8_t {
        First,
        Alias = First,
    };

    enum UnscopedChannel : std::uint8_t {
        UnscopedRed,
        UnscopedGreen,
    };

    enum class SparseChannel : std::uint16_t {
        None = 0,
        Red = 2,
        Green = 7,
        CoverageMask = 0xff,
        Blue = 42,
        All = 0xffff,
    };

    using ChannelSet = Sora::EnumSet<Channel>;

    consteval bool ValidateMutation() {
        ChannelSet set;
        if (!set.Empty() || set.Any() || set.Count() != 0) {
            return false;
        }

        set.Add(Channel::Red).Add({Channel::Blue, Channel::Alpha}).Remove(Channel::Blue).Toggle(Channel::Green);
        if (set.Count() != 3 || !set.Contains(Channel::Red) || !set.Contains(Channel::Green) ||
            !set.Contains(Channel::Alpha) || set.Contains(Channel::Blue)) {
            return false;
        }

        set.Set(Channel::Green, false).Set(Channel::Blue);
        return set == ChannelSet{Channel::Red, Channel::Blue, Channel::Alpha};
    }

    consteval bool ValidateAlgebra() {
        constexpr ChannelSet warm{Channel::Red, Channel::Green};
        constexpr ChannelSet cool{Channel::Green, Channel::Blue};

        if ((warm | cool) != ChannelSet{Channel::Red, Channel::Green, Channel::Blue}) {
            return false;
        }
        if ((warm & cool) != ChannelSet{Channel::Green} || (warm - cool) != ChannelSet{Channel::Red}) {
            return false;
        }
        if ((warm ^ cool) != ChannelSet{Channel::Red, Channel::Blue}) {
            return false;
        }
        return ~ChannelSet{Channel::Red} == ChannelSet{Channel::Green, Channel::Blue, Channel::Alpha};
    }

    consteval bool ValidateStorageRoundTrip() {
        ChannelSet::Storage bits;
        bits.set(static_cast<std::size_t>(Channel::Green));
        bits.set(static_cast<std::size_t>(Channel::Alpha));
        const ChannelSet set{bits};
        return set.Bits() == bits && set.Contains(Channel::Green) && set.Contains(Channel::Alpha);
    }

    static_assert(Sora::Concept::SequentialEnum<Channel>);
    static_assert(Sora::Concept::SequentialEnum<StartsAtOne>);
    static_assert(Sora::Concept::SequentialEnum<HasGap>);
    static_assert(!Sora::Concept::SequentialEnum<HasAlias>);
    static_assert(Sora::Concept::SequentialEnum<UnscopedChannel>);
    static_assert(Sora::Concept::SequentialEnum<SparseChannel>);
    static_assert(Sora::Concept::OrdinalEnum<Channel>);
    static_assert(!Sora::Concept::OrdinalEnum<StartsAtOne>);
    static_assert(!Sora::Concept::OrdinalEnum<HasGap>);
    static_assert(!Sora::Concept::OrdinalEnum<UnscopedChannel>);
    static_assert(!Sora::Concept::OrdinalEnum<SparseChannel>);
    static_assert(Sora::Concept::SpecialEnumerator<^^SparseChannel::None>);
    static_assert(Sora::Concept::SpecialEnumerator<^^SparseChannel::CoverageMask>);
    static_assert(Sora::Concept::SpecialEnumerator<^^SparseChannel::All>);
    static_assert(!Sora::Concept::SpecialEnumerator<^^SparseChannel::Red>);
    static_assert(ChannelSet::kSize == 4);
    static_assert(sizeof(ChannelSet) == sizeof(ChannelSet::Storage));
    static_assert(std::is_trivially_copyable_v<ChannelSet>);
    static_assert(std::is_standard_layout_v<ChannelSet>);
    static_assert(ValidateMutation());
    static_assert(ValidateAlgebra());
    static_assert(ValidateStorageRoundTrip());

} // namespace

TEST_CASE("EnumSet provides allocation-free set queries and mutation", "[Sora.Core.EnumSet]") {
    ChannelSet set{Channel::Red, Channel::Blue};

    REQUIRE(set.Any());
    REQUIRE_FALSE(set.Empty());
    REQUIRE(set.Count() == 2);
    REQUIRE(set.ContainsAny(ChannelSet{Channel::Green, Channel::Blue}));
    REQUIRE(set.ContainsAll(ChannelSet{Channel::Red, Channel::Blue}));
    REQUIRE_FALSE(set.ContainsAll(ChannelSet{Channel::Red, Channel::Alpha}));

    set.Clear();
    REQUIRE(set.Empty());
    REQUIRE(set.Bits().none());
}

TEST_CASE("EnumSet iterates contained values in enum declaration order", "[Sora.Core.EnumSet]") {
    const ChannelSet set{Channel::Alpha, Channel::Red, Channel::Blue};
    constexpr std::array expected{Channel::Red, Channel::Blue, Channel::Alpha};

    REQUIRE(std::ranges::equal(set.Values(), expected));

    std::array<Channel, expected.size()> visited{};
    std::size_t index = 0;
    set.ForEach([&](Channel value) noexcept { visited[index++] = value; });
    REQUIRE(visited == expected);
}

TEST_CASE("EnumSet complements only its reflected enum domain", "[Sora.Core.EnumSet]") {
    const ChannelSet set = ~ChannelSet{Channel::Green};

    REQUIRE(set.Count() == ChannelSet::kSize - 1);
    REQUIRE(set.Contains(Channel::Red));
    REQUIRE_FALSE(set.Contains(Channel::Green));
    REQUIRE(set.Contains(Channel::Blue));
    REQUIRE(set.Contains(Channel::Alpha));
    REQUIRE(ChannelSet::All().Count() == ChannelSet::kSize);
}

TEST_CASE("EnumSet maps sparse ordinary enumerators to dense declaration-order bits", "[Sora.Core.EnumSet]") {
    using SparseSet = Sora::EnumSet<SparseChannel>;
    const SparseSet set{SparseChannel::Blue, SparseChannel::Red};
    constexpr std::array expected{SparseChannel::Red, SparseChannel::Blue};

    STATIC_REQUIRE(SparseSet::kSize == 3);
    REQUIRE(set.Count() == 2);
    REQUIRE(set.Contains(SparseChannel::Red));
    REQUIRE_FALSE(set.Contains(SparseChannel::Green));
    REQUIRE(set.Contains(SparseChannel::Blue));
    REQUIRE(std::ranges::equal(set.Values(), expected));
}

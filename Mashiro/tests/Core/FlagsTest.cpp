/**
 * @file FlagsTest.cpp
 * @brief Tests for reflection-driven enum bitfield helpers.
 */
#include "Mashiro/Core/Flags.h"

#include "Support/Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <cstdint>
#include <type_traits>
#include <vector>

using namespace Mashiro;

namespace {

    enum class Access : std::uint8_t {
        None = 0,
        Read = 1,
        Write = 2,
        Execute = 4,
        Share = 8,
    };

    enum class SparseAccess : std::uint16_t {
        None = 0,
        Low = 0x0001,
        Mid = 0x0020,
        High = 0x8000,
    };

    enum class SignedAccess : std::int8_t {
        None = 0,
        A = 1,
        B = 2,
        C = 64,
    };

    enum class NonBitfield : std::uint8_t {
        None = 0,
        Read = 1,
        ReadWrite = 3,
    };

    enum class SequentialOnly : std::uint8_t {
        Zero = 0,
        One = 1,
        Two = 2,
        Three = 3,
    };

    constexpr auto kAllAccess = Access::Read | Access::Write | Access::Execute | Access::Share;

    template <typename E>
    constexpr auto Raw(E value) noexcept {
        return static_cast<std::underlying_type_t<E>>(value);
    }

} // namespace

TEST_CASE("BitfieldEnum accepts only zero-or-power-of-two enum values", AUTO_TAG) {
    STATIC_REQUIRE(Mashiro::Traits::BitfieldEnum<Access>);
    STATIC_REQUIRE(Mashiro::Traits::BitfieldEnum<SparseAccess>);
    STATIC_REQUIRE(Mashiro::Traits::BitfieldEnum<SignedAccess>);
    STATIC_REQUIRE_FALSE(Mashiro::Traits::BitfieldEnum<NonBitfield>);
    STATIC_REQUIRE_FALSE(Mashiro::Traits::BitfieldEnum<SequentialOnly>);
    STATIC_REQUIRE_FALSE(Mashiro::Traits::BitfieldEnum<int>);

	// TODO: ???
    //STATIC_REQUIRE_FALSE((requires(NonBitfield value) { value | value; }));
    //STATIC_REQUIRE_FALSE((requires(NonBitfield value) { Mashiro::EachFlag(value); }));
}

TEST_CASE("Bitwise operators return the enum type and fold at compile time", AUTO_TAG) {
    STATIC_REQUIRE(std::same_as<decltype(Access::Read | Access::Write), Access>);
    STATIC_REQUIRE(std::same_as<decltype(Access::Read & Access::Write), Access>);
    STATIC_REQUIRE(std::same_as<decltype(Access::Read ^ Access::Write), Access>);
    STATIC_REQUIRE(std::same_as<decltype(~Access::Read), Access>);

    STATIC_REQUIRE(noexcept(Access::Read | Access::Write));
    STATIC_REQUIRE(noexcept(Access::Read & Access::Write));
    STATIC_REQUIRE(noexcept(Access::Read ^ Access::Write));
    STATIC_REQUIRE(noexcept(~Access::Read));

    constexpr auto readWrite = Access::Read | Access::Write;
    STATIC_REQUIRE(Raw(readWrite) == 0x03);
    STATIC_REQUIRE((readWrite & Access::Read) == Access::Read);
    STATIC_REQUIRE((readWrite & Access::Execute) == Access::None);
    STATIC_REQUIRE((readWrite ^ Access::Write) == Access::Read);
    STATIC_REQUIRE((readWrite ^ Access::Execute) == (Access::Read | Access::Write | Access::Execute));
}

TEST_CASE("Compound bitwise operators mutate in place and return the same object", AUTO_TAG) {
    auto flags = Access::Read;

    auto* afterOr = &(flags |= Access::Write);
    REQUIRE(afterOr == &flags);
    REQUIRE(flags == (Access::Read | Access::Write));

    auto* afterAnd = &(flags &= Access::Write);
    REQUIRE(afterAnd == &flags);
    REQUIRE(flags == Access::Write);

    auto* afterXor = &(flags ^= Access::Execute);
    REQUIRE(afterXor == &flags);
    REQUIRE(flags == (Access::Write | Access::Execute));
}

TEST_CASE("Complement is masked to the declared flag space", AUTO_TAG) {
    STATIC_REQUIRE(Mashiro::Traits::kBitfieldMask<Access> == kAllAccess);
    STATIC_REQUIRE(~Access::None == kAllAccess);
    STATIC_REQUIRE(~kAllAccess == Access::None);
    STATIC_REQUIRE(~(Access::Read | Access::Execute) == (Access::Write | Access::Share));

    constexpr auto sparseMask = Mashiro::Traits::kBitfieldMask<SparseAccess>;
    STATIC_REQUIRE(Raw(sparseMask) == 0x8021);
    STATIC_REQUIRE(Raw(~SparseAccess::Low) == 0x8020);
    STATIC_REQUIRE(Raw(~(SparseAccess::Low | SparseAccess::High)) == 0x0020);

    STATIC_REQUIRE(Raw(Mashiro::Traits::kBitfieldMask<SignedAccess>) == 0x43);
    STATIC_REQUIRE(~SignedAccess::A == (SignedAccess::B | SignedAccess::C));
}

TEST_CASE("Query helpers distinguish empty, any, all, and popcount semantics", AUTO_TAG) {
    constexpr auto readWrite = Access::Read | Access::Write;

    STATIC_REQUIRE(IsEmpty(Access::None));
    STATIC_REQUIRE_FALSE(IsEmpty(readWrite));

    STATIC_REQUIRE(HasFlag(readWrite, Access::Read));
    STATIC_REQUIRE(HasFlag(readWrite, Access::Read | Access::Write));
    STATIC_REQUIRE_FALSE(HasFlag(readWrite, Access::Execute));

    STATIC_REQUIRE(HasFlag(Access::None, Access::None));
    STATIC_REQUIRE_FALSE(HasFlag(readWrite, Access::None));
    STATIC_REQUIRE(HasAll(readWrite, Access::None));

    STATIC_REQUIRE(HasAny(readWrite, Access::Write | Access::Execute));
    STATIC_REQUIRE_FALSE(HasAny(readWrite, Access::Execute | Access::Share));

    STATIC_REQUIRE(HasAll(readWrite, Access::Read | Access::Write));
    STATIC_REQUIRE_FALSE(HasAll(readWrite, Access::Read | Access::Execute));

    STATIC_REQUIRE(PopCount(Access::None) == 0);
    STATIC_REQUIRE(PopCount(readWrite) == 2);
    STATIC_REQUIRE(PopCount(kAllAccess) == 4);
}

TEST_CASE("Mutation helpers support multi-bit masks", AUTO_TAG) {
    auto flags = Access::None;

    auto* afterSet = &SetFlag(flags, Access::Read | Access::Write);
    REQUIRE(afterSet == &flags);
    REQUIRE(flags == (Access::Read | Access::Write));

    auto* afterClear = &ClearFlag(flags, Access::Write | Access::Execute);
    REQUIRE(afterClear == &flags);
    REQUIRE(flags == Access::Read);

    auto* afterToggle = &ToggleFlag(flags, Access::Read | Access::Share);
    REQUIRE(afterToggle == &flags);
    REQUIRE(flags == Access::Share);

    ToggleFlag(flags, Access::Execute | Access::Share);
    REQUIRE(flags == Access::Execute);
}

TEST_CASE("EachFlag yields only set bits in ascending bit order", AUTO_TAG) {
    std::vector<Access> flags;
    for (auto flag : EachFlag(Access::Share | Access::Read | Access::Execute)) {
        flags.push_back(flag);
    }

    REQUIRE(flags == std::vector{Access::Read, Access::Execute, Access::Share});

    std::vector<Access> empty;
    for (auto flag : EachFlag(Access::None)) {
        empty.push_back(flag);
    }
    REQUIRE(empty.empty());
}

TEST_CASE("EachFlag works for sparse and high-bit underlying values", AUTO_TAG) {
    std::vector<SparseAccess> flags;
    for (auto flag : EachFlag(SparseAccess::High | SparseAccess::Low | SparseAccess::Mid)) {
        flags.push_back(flag);
    }

    REQUIRE(flags == std::vector{SparseAccess::Low, SparseAccess::Mid, SparseAccess::High});
    REQUIRE(PopCount(SparseAccess::High | SparseAccess::Low) == 2);
}

TEST_CASE("Flags helpers are constexpr-friendly as a full workflow", AUTO_TAG) {
    constexpr auto result = []() consteval {
        auto flags = Access::None;
        SetFlag(flags, Access::Read);
        SetFlag(flags, Access::Share);
        ToggleFlag(flags, Access::Read | Access::Execute);
        ClearFlag(flags, Access::Share);

        int sum = 0;
        for (auto flag : EachFlag(flags)) {
            sum += Raw(flag);
        }

        return HasAll(flags, Access::Execute) &&
               !HasAny(flags, Access::Read | Access::Write | Access::Share) &&
               PopCount(flags) == 1 &&
               sum == Raw(Access::Execute);
    }();

    STATIC_REQUIRE(result);
}

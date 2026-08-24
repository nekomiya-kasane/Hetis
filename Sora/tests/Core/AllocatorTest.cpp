#include <Sora/Core/Memory/Allocator.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <vector>

namespace {

    struct alignas(256) OverAlignedValue {
        uint64_t value = 0;
    };

} // namespace

TEST_CASE("Sora allocator honors raw byte alignment", "[Core][Allocator]") {
    constexpr Sora::Align alignment = Sora::Align::Constant<4096>();
    void* allocation = Sora::AllocateMemory(257, alignment);

    REQUIRE(allocation != nullptr);
    REQUIRE(Sora::IsAddressAligned(allocation, alignment));

    Sora::DeallocateMemory(allocation, 257, alignment);
}

TEST_CASE("Sora standard allocator supports ordinary and over-aligned values", "[Core][Allocator]") {
    std::vector<int, Sora::Allocator<int>> values(128);
    std::iota(values.begin(), values.end(), 1);
    REQUIRE(std::accumulate(values.begin(), values.end(), 0) == 8256);

    std::vector<OverAlignedValue, Sora::Allocator<OverAlignedValue>> alignedValues(4);
    REQUIRE(Sora::IsAddressAligned(alignedValues.data(), Sora::Align::Of<OverAlignedValue>()));
}

TEST_CASE("Sora standard allocator reports impossible array sizes", "[Core][Allocator]") {
    Sora::Allocator<uint64_t> allocator;
    constexpr size_t impossibleCount = std::numeric_limits<size_t>::max() / sizeof(uint64_t) + 1;

    REQUIRE_THROWS_AS(allocator.allocate(impossibleCount), std::bad_array_new_length);
}

TEST_CASE("Sora allocator instances share one allocation domain", "[Core][Allocator]") {
    STATIC_REQUIRE(Sora::Allocator<int>{} == Sora::Allocator<double>{});
    STATIC_REQUIRE(std::allocator_traits<Sora::Allocator<int>>::is_always_equal::value);
}

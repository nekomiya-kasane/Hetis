/**
 * @file SpscChannelTest.cpp
 * @brief Tests for the SPSC channel wake-sequence layer above SpscRingBuffer.
 */
#include "Mashiro/Core/SpscChannel.h"

#include "Support/Meta.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace Mashiro;

TEST_CASE("SpscChannel: successful pushes advance wake sequence and drain by snapshot", AUTO_TAG) {
    SpscChannel<int, 4> channel;

    const std::uint64_t before = channel.Observe();
    REQUIRE(channel.TryPush(1));
    REQUIRE(channel.TryPush(2));
    REQUIRE(channel.Observe() == before + 2);

    std::vector<int> drained;
    const std::uint32_t count = channel.Drain([&drained](int&& value) noexcept { drained.push_back(value); });

    REQUIRE(count == 2);
    REQUIRE(drained == std::vector<int>{1, 2});
    REQUIRE(channel.Empty());
}

TEST_CASE("SpscChannel: predicate signal advances wake sequence without payload", AUTO_TAG) {
    SpscChannel<int, 4> channel;

    const std::uint64_t before = channel.Observe();
    channel.SignalPredicateChanged();

    REQUIRE(channel.Observe() == before + 1);
    REQUIRE(channel.Empty());
}

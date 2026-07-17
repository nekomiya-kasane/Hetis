/**
 * @file SpscChannelTest.cpp
 * @brief Tests for the SPSC channel wake-sequence layer above SpscRingBuffer.
 */
#include "Mashiro/Core/SpscChannel.h"

#include "Support/Meta.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace Mashiro;

TEST_CASE("SpscChannel: successful operations advance only the opposite endpoint's sequence", AUTO_TAG) {
    SpscChannel<int, 4> channel;

    const std::uint64_t readableBefore = channel.ObserveReadable();
    const std::uint64_t writableBefore = channel.ObserveWritable();
    REQUIRE(channel.TryPush(1));
    REQUIRE(channel.TryPush(2));
    REQUIRE(channel.ObserveReadable() == readableBefore + 2);
    REQUIRE(channel.ObserveWritable() == writableBefore);

    std::vector<int> drained;
    const std::uint32_t count = channel.Drain([&drained](int&& value) noexcept { drained.push_back(value); });

    REQUIRE(count == 2);
    REQUIRE(drained == std::vector<int>{1, 2});
    REQUIRE(channel.ObserveReadable() == readableBefore + 2);
    REQUIRE(channel.ObserveWritable() == writableBefore + 1);
    REQUIRE(channel.Empty());
}

TEST_CASE("SpscChannel: external predicate signal advances both directional sequences", AUTO_TAG) {
    SpscChannel<int, 4> channel;

    const std::uint64_t readableBefore = channel.ObserveReadable();
    const std::uint64_t writableBefore = channel.ObserveWritable();
    channel.SignalPredicateChanged();

    REQUIRE(channel.ObserveReadable() == readableBefore + 1);
    REQUIRE(channel.ObserveWritable() == writableBefore + 1);
    REQUIRE(channel.Empty());
}

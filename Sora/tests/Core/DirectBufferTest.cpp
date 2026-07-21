/**
 * @file DirectBufferTest.cpp
 * @brief Verify aligned byte-storage allocation, ownership transfer, and reset semantics.
 * @ingroup Testing
 */

#include <Sora/Core/Memory/DirectBuffer.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>

TEST_CASE("DirectBuffer validates runtime alignments", "[Sora.Core.Memory]") {
    REQUIRE_FALSE(Sora::DirectBuffer::Allocate(1024, 0).has_value());
    REQUIRE_FALSE(Sora::DirectBuffer::Allocate(1024, 3).has_value());

    auto buffer = Sora::DirectBuffer::Allocate(1024, 256);
    REQUIRE(buffer.has_value());
    REQUIRE(buffer->Size() == 1024);
    REQUIRE_FALSE(buffer->Empty());
    REQUIRE(buffer->Data() == buffer->Bytes().data());
    REQUIRE(buffer->Alignment().Value() >= 256);
    REQUIRE(reinterpret_cast<std::uintptr_t>(buffer->Data()) % buffer->Alignment().Value() == 0);
}

TEST_CASE("DirectBuffer supports strong alignments and empty allocations", "[Sora.Core.Memory]") {
    auto empty = Sora::DirectBuffer::Allocate(0, Sora::Align::Constant<4096>());
    REQUIRE(empty.has_value());
    REQUIRE(empty->Empty());
    REQUIRE(empty->Data() == nullptr);
    REQUIRE(empty->Bytes().empty());
    REQUIRE(empty->Alignment() == Sora::Align::Constant<4096>());
}

TEST_CASE("DirectBuffer transfers and releases ownership", "[Sora.Core.Memory]") {
    auto allocated = Sora::DirectBuffer::Allocate(512, Sora::Align::Constant<128>());
    REQUIRE(allocated.has_value());
    std::byte* address = allocated->Data();

    Sora::DirectBuffer moved = std::move(*allocated);
    REQUIRE(moved.Data() == address);
    REQUIRE(allocated->Empty());
    REQUIRE(allocated->Data() == nullptr);

    Sora::DirectBuffer destination;
    destination = std::move(moved);
    REQUIRE(destination.Data() == address);
    REQUIRE(moved.Empty());
    REQUIRE(moved.Data() == nullptr);

    destination.Reset();
    REQUIRE(destination.Empty());
    REQUIRE(destination.Data() == nullptr);
    REQUIRE(destination.Alignment() == Sora::Align{});
}

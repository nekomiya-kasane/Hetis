/**
 * @file GlobalMemoryTest.cpp
 * @brief Verify ownership, borrowing, locking, and transfer of movable global memory.
 * @ingroup Testing
 */

#include <Sora/Core/PAL/GlobalMemory.h>
#include <Sora/Platform.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace PAL = Sora::PAL;

TEST_CASE("Movable global memory exposes a stable scoped byte view", "[Sora.PAL.GlobalMemory]") {
    auto allocated = PAL::OwnedGlobalMemory::Allocate(64, PAL::GlobalMemoryInitialization::Zeroed);
    if constexpr (!Sora::Platform::kIsWindows) {
        REQUIRE_FALSE(allocated.has_value());
        REQUIRE(allocated.error() == Sora::ErrorCode::NotSupported);
        return;
    }

    REQUIRE(allocated.has_value());
    PAL::OwnedGlobalMemory memory = std::move(*allocated);
    REQUIRE(memory);
    REQUIRE(memory.Size().value() >= 64);

    auto locked = memory.Lock();
    REQUIRE(locked.has_value());
    REQUIRE(locked->Size() >= 64);
    REQUIRE(std::ranges::all_of(locked->Bytes().first(64), [](std::byte value) { return value == std::byte{}; }));

    locked->Bytes()[17] = std::byte{0xA5};
    REQUIRE(locked->Unlock().has_value());
    REQUIRE(locked->Empty());

    auto relocked = memory.Lock();
    REQUIRE(relocked.has_value());
    REQUIRE(relocked->Bytes()[17] == std::byte{0xA5});
    REQUIRE(relocked->Unlock().has_value());
}

TEST_CASE("Movable global-memory ownership transfers without changing the native block", "[Sora.PAL.GlobalMemory]") {
    auto allocated = PAL::OwnedGlobalMemory::Allocate(32);
    if constexpr (!Sora::Platform::kIsWindows) {
        REQUIRE_FALSE(allocated.has_value());
        return;
    }

    REQUIRE(allocated.has_value());
    PAL::OwnedGlobalMemory source = std::move(*allocated);
    const PAL::GlobalMemoryHandle original = source.NativeHandle();
    const PAL::GlobalMemoryView borrowed = source.View();

    PAL::OwnedGlobalMemory destination = std::move(source);
    REQUIRE(source.Empty());
    REQUIRE(destination.NativeHandle() == original);
    REQUIRE(borrowed.NativeHandle() == original);

    const PAL::GlobalMemoryHandle released = destination.Release();
    REQUIRE(destination.Empty());
    REQUIRE(released == original);

    PAL::OwnedGlobalMemory adopted = PAL::OwnedGlobalMemory::Adopt(released);
    REQUIRE(adopted.NativeHandle() == original);
    REQUIRE(adopted.Size().has_value());
}

TEST_CASE("Empty and zero-sized global-memory requests fail without native side effects", "[Sora.PAL.GlobalMemory]") {
    const PAL::GlobalMemoryView empty;
    REQUIRE_FALSE(empty.Size().has_value());
    REQUIRE(empty.Size().error() == Sora::ErrorCode::InvalidState);
    REQUIRE_FALSE(empty.Lock().has_value());
    REQUIRE(empty.Lock().error() == Sora::ErrorCode::InvalidState);

    auto zero = PAL::OwnedGlobalMemory::Allocate(0);
    REQUIRE_FALSE(zero.has_value());
    REQUIRE(zero.error() == Sora::ErrorCode::InvalidArgument);
}

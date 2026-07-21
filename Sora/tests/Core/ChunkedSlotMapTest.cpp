/**
 * @file ChunkedSlotMapTest.cpp
 * @brief Verify ChunkedSlotMap handle safety, dense layout, exception guarantees, and growth behavior.
 * @ingroup Testing
 */

#include <Sora/Core/ADT/ChunkedSlotMap.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

    struct Record {
        std::string name;
        int value = 0;
    };

    struct LiveCounter {
        inline static int liveCount = 0;

        int value = 0;

        explicit LiveCounter(int initial) noexcept : value(initial) { ++liveCount; }
        LiveCounter(LiveCounter&& other) noexcept : value(other.value) { ++liveCount; }
        LiveCounter& operator=(LiveCounter&& other) noexcept {
            value = other.value;
            return *this;
        }
        ~LiveCounter() { --liveCount; }
    };

    struct ThrowingConstructor {
        int value = 0;

        explicit ThrowingConstructor(int initial) : value(initial) {
            if (initial < 0) {
                throw std::runtime_error("construction rejected");
            }
        }
        ThrowingConstructor(ThrowingConstructor&&) noexcept = default;
        ThrowingConstructor& operator=(ThrowingConstructor&&) noexcept = default;
    };

    struct ThrowingMove {
        ThrowingMove() = default;
        ThrowingMove(ThrowingMove&&) noexcept(false) {}
        ThrowingMove& operator=(ThrowingMove&&) noexcept(false) { return *this; }
    };

    struct alignas(64) OverAlignedValue {
        std::uint64_t words[8]{};
    };

    static_assert(Sora::ChunkedSlotMapValue<int>);
    static_assert(Sora::ChunkedSlotMapValue<Record>);
    static_assert(!Sora::ChunkedSlotMapValue<ThrowingMove>);
    static_assert(sizeof(Sora::SlotHandle) == 8);

} // namespace

TEST_CASE("ChunkedSlotMap constructs lazily and reserves both sparse and dense storage",
          "[Sora.Core.ADT.ChunkedSlotMap]") {
    Sora::ChunkedSlotMap<int> map;

    REQUIRE(map.Empty());
    REQUIRE(map.Size() == 0);
    REQUIRE(map.Capacity() == 0);
    REQUIRE(map.DenseCapacity() == 0);
    REQUIRE(map.Data() == nullptr);

    map.Reserve(0);
    REQUIRE(map.Capacity() == 0);
    REQUIRE(map.ChunkCount() == 0);

    map.ReserveDense(600);
    REQUIRE(map.Capacity() == 0);
    REQUIRE(map.DenseCapacity() >= 600);

    map.ReserveSparse(600);
    REQUIRE(map.Empty());
    REQUIRE(map.Capacity() >= 600);
    REQUIRE(map.DenseCapacity() >= 600);
    REQUIRE(map.ChunkCount() == 3);
    REQUIRE(map.Data() == nullptr);
}

TEST_CASE("ChunkedSlotMap bulk insertion preserves order and rolls back failures", "[Sora.Core.ADT.ChunkedSlotMap]") {
    constexpr std::array values{3, 5, 8, 13};
    Sora::ChunkedSlotMap<int> integers;
    const std::vector<Sora::SlotHandle> handles = integers.EmplaceRange(values);

    REQUIRE(handles.size() == values.size());
    for (size_t index = 0; index < values.size(); ++index) {
        REQUIRE(*integers.Get(handles[index]) == values[index]);
    }

    Sora::ChunkedSlotMap<ThrowingConstructor> throwing;
    const Sora::SlotHandle stable = throwing.Emplace(21);
    constexpr std::array failingValues{34, -1, 55};
    REQUIRE_THROWS_AS(throwing.EmplaceRange(failingValues), std::runtime_error);
    REQUIRE(throwing.Size() == 1);
    REQUIRE(throwing.Get(stable)->value == 21);
}

TEST_CASE("ChunkedSlotMap inserts, resolves, and mutates values through handles", "[Sora.Core.ADT.ChunkedSlotMap]") {
    Sora::ChunkedSlotMap<Record> map;
    const Sora::SlotHandle first = map.Emplace(Record{.name = "alpha", .value = 11});
    const Sora::SlotHandle second = map.Emplace(Record{.name = "beta", .value = 29});

    REQUIRE(first.IsValid());
    REQUIRE(first != second);
    REQUIRE(map.Size() == 2);
    REQUIRE(map.Get(first)->name == "alpha");
    REQUIRE(map[second].value == 29);

    map[second].value = 31;
    REQUIRE(map.Get(second)->value == 31);
    REQUIRE(map.Get(Sora::SlotHandle::Null()) == nullptr);
}

TEST_CASE("ChunkedSlotMap removal invalidates only the erased handle", "[Sora.Core.ADT.ChunkedSlotMap]") {
    Sora::ChunkedSlotMap<int> map;
    const Sora::SlotHandle first = map.Emplace(10);
    const Sora::SlotHandle middle = map.Emplace(20);
    const Sora::SlotHandle last = map.Emplace(30);

    STATIC_REQUIRE(noexcept(map.Free(first)));
    REQUIRE(map.Free(middle));
    REQUIRE_FALSE(map.Free(middle));
    REQUIRE_FALSE(map.IsAlive(middle));
    REQUIRE(map.Get(middle) == nullptr);
    REQUIRE(map.Get(first) != nullptr);
    REQUIRE(map.Get(last) != nullptr);
    REQUIRE(*map.Get(first) == 10);
    REQUIRE(*map.Get(last) == 30);
}

TEST_CASE("ChunkedSlotMap reuses sparse slots with a new generation", "[Sora.Core.ADT.ChunkedSlotMap]") {
    Sora::ChunkedSlotMap<int, 2> map;
    const Sora::SlotHandle oldHandle = map.Emplace(7);

    REQUIRE(map.Free(oldHandle));
    const Sora::SlotHandle replacement = map.Emplace(9);

    REQUIRE(replacement.index == oldHandle.index);
    REQUIRE(replacement.generation != oldHandle.generation);
    REQUIRE_FALSE(map.IsAlive(oldHandle));
    REQUIRE(map.IsAlive(replacement));
    REQUIRE(*map.Get(replacement) == 9);
}

TEST_CASE("ChunkedSlotMap batch removal handles sparse and compacting paths", "[Sora.Core.ADT.ChunkedSlotMap]") {
    Sora::ChunkedSlotMap<int> map;
    std::vector<Sora::SlotHandle> handles;
    handles.reserve(1'000);
    for (int value = 0; value < 1'000; ++value) {
        handles.push_back(map.Emplace(value));
    }

    const std::array sparseBatch{handles[3], handles[17], handles[3], Sora::SlotHandle::Null()};
    REQUIRE(map.FreeBatch(sparseBatch) == 2);
    REQUIRE_FALSE(map.IsAlive(handles[3]));
    REQUIRE_FALSE(map.IsAlive(handles[17]));

    std::vector<Sora::SlotHandle> compactingBatch;
    for (size_t index = 0; index < handles.size(); ++index) {
        if (index % 5 != 0) {
            compactingBatch.push_back(handles[index]);
        }
    }
    compactingBatch.push_back(handles[1]);
    const size_t removed = map.FreeBatch(compactingBatch);
    REQUIRE(removed == 798);
    REQUIRE(map.Size() == 200);

    for (size_t index = 0; index < handles.size(); index += 5) {
        REQUIRE(map.IsAlive(handles[index]));
        REQUIRE(*map.Get(handles[index]) == static_cast<int>(index));
    }
}

TEST_CASE("ChunkedSlotMap stores values contiguously for range and SIMD processing", "[Sora.Core.ADT.ChunkedSlotMap]") {
    Sora::ChunkedSlotMap<int> map;
    map.Reserve(128);
    for (int value = 0; value < 128; ++value) {
        (void)map.Emplace(value);
    }

    STATIC_REQUIRE(Sora::ChunkedSlotMap<int>::kDenseStride == sizeof(int));
    const std::span<int> values = map.Values();
    REQUIRE(values.size() == 128);
    REQUIRE(values.data() == map.Data());
    for (size_t index = 1; index < values.size(); ++index) {
        REQUIRE(std::addressof(values[index]) == std::addressof(values[0]) + index);
    }
    REQUIRE(std::accumulate(map.begin(), map.end(), 0) == 127 * 128 / 2);
}

TEST_CASE("ChunkedSlotMap ForEach exposes values and their current handles", "[Sora.Core.ADT.ChunkedSlotMap]") {
    Sora::ChunkedSlotMap<int> map;
    const Sora::SlotHandle first = map.Emplace(4);
    const Sora::SlotHandle second = map.Emplace(8);
    const Sora::SlotHandle third = map.Emplace(15);
    REQUIRE(map.Free(second));

    int valueSum = 0;
    map.ForEach([&](int& value) { valueSum += value; });
    REQUIRE(valueSum == 19);

    std::vector<Sora::SlotHandle> handles;
    map.ForEach([&](Sora::SlotHandle handle, int& value) {
        REQUIRE(map.Get(handle) == std::addressof(value));
        handles.push_back(handle);
    });
    REQUIRE(std::ranges::find(handles, first) != handles.end());
    REQUIRE(std::ranges::find(handles, third) != handles.end());
}

TEST_CASE("ChunkedSlotMap const access preserves const qualification", "[Sora.Core.ADT.ChunkedSlotMap]") {
    Sora::ChunkedSlotMap<int> mutableMap;
    const Sora::SlotHandle handle = mutableMap.Emplace(42);
    const Sora::ChunkedSlotMap<int>& map = mutableMap;

    STATIC_REQUIRE(std::same_as<decltype(map.Get(handle)), const int*>);
    STATIC_REQUIRE(std::same_as<decltype(map.Values()), std::span<const int>>);
    REQUIRE(*map.Get(handle) == 42);

    int observed = 0;
    map.ForEach([&](Sora::SlotHandle current, const int& value) {
        REQUIRE(current == handle);
        observed = value;
    });
    REQUIRE(observed == 42);
}

TEST_CASE("ChunkedSlotMap batch lookup preserves input order and reports stale handles",
          "[Sora.Core.ADT.ChunkedSlotMap]") {
    Sora::ChunkedSlotMap<int> map;
    const std::array handles{map.Emplace(2), map.Emplace(3), map.Emplace(5), map.Emplace(7)};
    REQUIRE(map.Free(handles[1]));

    std::array<int*, 4> output{};
    REQUIRE(map.GetBatch(handles, output) == 3);
    REQUIRE(*output[0] == 2);
    REQUIRE(output[1] == nullptr);
    REQUIRE(*output[2] == 5);
    REQUIRE(*output[3] == 7);

    const Sora::ChunkedSlotMap<int>& constMap = map;
    std::array<const int*, 4> constOutput{};
    REQUIRE(constMap.GetBatch(handles, constOutput) == 3);
    REQUIRE(constOutput[1] == nullptr);

    std::array<int*, 3> shortOutput{};
    REQUIRE_THROWS_AS(map.GetBatch(handles, shortOutput), std::invalid_argument);
}

TEST_CASE("ChunkedSlotMap insertion rolls back when value construction throws", "[Sora.Core.ADT.ChunkedSlotMap]") {
    Sora::ChunkedSlotMap<ThrowingConstructor> map;
    const Sora::SlotHandle stable = map.Emplace(17);

    REQUIRE_THROWS_AS(map.Emplace(-1), std::runtime_error);
    REQUIRE(map.Size() == 1);
    REQUIRE(map.IsAlive(stable));
    REQUIRE(map.Get(stable)->value == 17);

    const Sora::SlotHandle next = map.Emplace(23);
    REQUIRE(next.index == stable.index + 1);
    REQUIRE(map.Get(next)->value == 23);
}

TEST_CASE("ChunkedSlotMap destroys each live value exactly once", "[Sora.Core.ADT.ChunkedSlotMap]") {
    REQUIRE(LiveCounter::liveCount == 0);
    {
        Sora::ChunkedSlotMap<LiveCounter> map;
        const Sora::SlotHandle first = map.Emplace(1);
        (void)map.Emplace(2);
        (void)map.Emplace(3);
        REQUIRE(LiveCounter::liveCount == 3);

        REQUIRE(map.Free(first));
        REQUIRE(LiveCounter::liveCount == 2);
        map.Clear();
        REQUIRE(LiveCounter::liveCount == 0);
    }
    REQUIRE(LiveCounter::liveCount == 0);
}

TEST_CASE("ChunkedSlotMap Clear invalidates handles in time proportional to live values",
          "[Sora.Core.ADT.ChunkedSlotMap]") {
    Sora::ChunkedSlotMap<int, 4> map;
    map.Reserve(1024);
    std::array<Sora::SlotHandle, 3> handles{map.Emplace(1), map.Emplace(2), map.Emplace(3)};
    const size_t capacity = map.Capacity();

    map.Clear();
    REQUIRE(map.Empty());
    REQUIRE(map.Capacity() == capacity);
    for (const Sora::SlotHandle handle : handles) {
        REQUIRE_FALSE(map.IsAlive(handle));
    }

    const Sora::SlotHandle replacement = map.Emplace(4);
    REQUIRE(replacement.index == 1);
    REQUIRE(replacement.generation != handles[0].generation);
}

TEST_CASE("ChunkedSlotMap move operations leave the source reusable", "[Sora.Core.ADT.ChunkedSlotMap]") {
    Sora::ChunkedSlotMap<int> source;
    const Sora::SlotHandle original = source.Emplace(41);

    Sora::ChunkedSlotMap<int> destination = std::move(source);
    REQUIRE(*destination.Get(original) == 41);
    REQUIRE(source.Empty());
    const Sora::SlotHandle sourceHandle = source.Emplace(7);
    REQUIRE(*source.Get(sourceHandle) == 7);

    Sora::ChunkedSlotMap<int> assigned;
    (void)assigned.Emplace(99);
    assigned = std::move(destination);
    REQUIRE(*assigned.Get(original) == 41);
    REQUIRE(destination.Empty());
    const Sora::SlotHandle destinationHandle = destination.Emplace(13);
    REQUIRE(*destination.Get(destinationHandle) == 13);
}

TEST_CASE("ChunkedSlotMap supports over-aligned values", "[Sora.Core.ADT.ChunkedSlotMap]") {
    Sora::ChunkedSlotMap<OverAlignedValue> map;
    map.Reserve(8);
    const Sora::SlotHandle handle = map.Emplace();

    REQUIRE(reinterpret_cast<std::uintptr_t>(map.Get(handle)) % alignof(OverAlignedValue) == 0);
    REQUIRE(reinterpret_cast<std::uintptr_t>(map.Data()) % alignof(OverAlignedValue) == 0);
}

TEST_CASE("ChunkedSlotMap grows across chunks and survives mixed stress", "[Sora.Core.ADT.ChunkedSlotMap]") {
    constexpr int kCount = 20'000;
    Sora::ChunkedSlotMap<int, 3> map;
    std::vector<Sora::SlotHandle> handles;
    handles.reserve(kCount);

    for (int value = 0; value < kCount; ++value) {
        handles.push_back(map.Emplace(value));
    }
    for (int index = 1; index < kCount; index += 2) {
        REQUIRE(map.Free(handles[static_cast<size_t>(index)]));
    }
    REQUIRE(map.Size() == kCount / 2);

    for (int index = 0; index < kCount; ++index) {
        const bool shouldBeAlive = index % 2 == 0;
        REQUIRE(map.IsAlive(handles[static_cast<size_t>(index)]) == shouldBeAlive);
        if (shouldBeAlive) {
            REQUIRE(*map.Get(handles[static_cast<size_t>(index)]) == index);
        }
    }

    for (int value = 0; value < kCount / 2; ++value) {
        const Sora::SlotHandle handle = map.Emplace(value + kCount);
        REQUIRE(map.IsAlive(handle));
    }
    REQUIRE(map.Size() == kCount);
}

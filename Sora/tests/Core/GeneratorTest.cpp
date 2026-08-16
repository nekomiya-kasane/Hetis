/**
 * @file GeneratorTest.cpp
 * @brief Validate synchronous generator lifetime, delegation, exception, and allocation semantics.
 * @ingroup Testing
 */
#include <Sora/Core/Concurrency/Generator.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

    using IntGenerator = Sora::Generator<int>;

    struct AllocationCounters {
        size_t allocations = 0;
        size_t deallocations = 0;
        size_t allocatedBytes = 0;
        size_t deallocatedBytes = 0;
    };

    template<typename T>
    class CountingAllocator {
    public:
        using value_type = T;

        CountingAllocator() = default;
        explicit CountingAllocator(AllocationCounters& counters) noexcept : counters_(std::addressof(counters)) {}

        template<typename U>
        CountingAllocator(const CountingAllocator<U>& other) noexcept : counters_(other.Counters()) {}

        [[nodiscard]] T* allocate(size_t count) {
            ++counters_->allocations;
            counters_->allocatedBytes += count * sizeof(T);
            return std::allocator<T>{}.allocate(count);
        }

        void deallocate(T* allocation, size_t count) noexcept {
            ++counters_->deallocations;
            counters_->deallocatedBytes += count * sizeof(T);
            std::allocator<T>{}.deallocate(allocation, count);
        }

        [[nodiscard]] AllocationCounters* Counters() const noexcept { return counters_; }

        template<typename U>
        [[nodiscard]] bool operator==(const CountingAllocator<U>& other) const noexcept {
            return counters_ == other.Counters();
        }

    private:
        AllocationCounters* counters_ = nullptr;
    };

    using Allocator = CountingAllocator<std::byte>;
    using AllocatedGenerator = Sora::Generator<int, void, Allocator>;

    template<typename T>
    class ByteAlignedAllocator {
    public:
        using value_type = T;

        ByteAlignedAllocator() = default;

        template<typename U>
        ByteAlignedAllocator(const ByteAlignedAllocator<U>&) noexcept {}

        [[nodiscard]] T* allocate(size_t count) {
            static_assert(alignof(T) == 1);
            auto* allocation = static_cast<std::byte*>(::operator new(count * sizeof(T) + 1));
            return reinterpret_cast<T*>(allocation + 1);
        }

        void deallocate(T* allocation, size_t) noexcept {
            ::operator delete(reinterpret_cast<std::byte*>(allocation) - 1);
        }

        template<typename U>
        [[nodiscard]] constexpr bool operator==(const ByteAlignedAllocator<U>&) const noexcept {
            return true;
        }
    };

    using MinimallyAlignedAllocator = ByteAlignedAllocator<std::byte>;
    using AlignedGenerator = Sora::Generator<std::uintptr_t, void, MinimallyAlignedAllocator>;
    using OverAlignedGenerator = Sora::Generator<std::uintptr_t, void, MinimallyAlignedAllocator, 64>;
    using GloballyOverAlignedGenerator = Sora::Generator<std::uintptr_t, void, void, 64>;

    template<typename T>
    struct FancyPointer {
        using element_type = T;
        T* address = nullptr;
    };

    template<typename T>
    struct FancyAllocator {
        using value_type = T;
        using pointer = FancyPointer<T>;

        template<typename U>
        struct rebind {
            using other = FancyAllocator<U>;
        };
    };

    IntGenerator LazyValues(bool& started) {
        started = true;
        co_yield 2;
        co_yield 3;
    }

    Sora::Generator<int&> References(std::array<int, 3>& values) {
        for (int& value : values) {
            co_yield value;
        }
    }

    Sora::Generator<std::unique_ptr<int>> MoveOnlyValues() {
        co_yield std::make_unique<int>(5);
        co_yield std::make_unique<int>(8);
    }

    IntGenerator Leaf(int first, int last) {
        for (int value = first; value != last; ++value) {
            co_yield value;
        }
    }

    IntGenerator NestedValues() {
        co_yield -1;
        co_yield Sora::ElementsOf(Leaf(0, 2));
        co_yield Sora::ElementsOf(IntGenerator{});
        auto exhausted = Leaf(8, 8);
        (void)exhausted.begin();
        co_yield Sora::ElementsOf(std::move(exhausted));
        co_yield 2;
    }

    Sora::Generator<int&> DelegatedReferences(std::vector<int>& values) {
        co_yield Sora::ElementsOf(values);
    }

    IntGenerator DelegatedOwningRange() {
        co_yield Sora::ElementsOf(std::array{4, 6, 9});
    }

    IntGenerator DirectFailure() {
        co_yield 1;
        throw std::runtime_error{"direct generator failure"};
    }

    IntGenerator FailingLeaf() {
        co_yield 7;
        throw std::runtime_error{"nested generator failure"};
    }

    IntGenerator NestedFailure() {
        co_yield 6;
        co_yield Sora::ElementsOf(FailingLeaf());
        co_yield 8;
    }

    IntGenerator RecoverNestedFailure() {
        bool recovered = false;
        try {
            co_yield Sora::ElementsOf(FailingLeaf());
        } catch (const std::runtime_error&) {
            recovered = true;
        }
        if (recovered) {
            co_yield 11;
        }
    }

    [[gnu::noinline]] AllocatedGenerator AllocatedValues(std::allocator_arg_t, const Allocator&, int value) {
        co_yield value;
    }

    [[gnu::noinline]] AllocatedGenerator AllocatedDelegation(std::allocator_arg_t, const Allocator& allocator) {
        co_yield Sora::ElementsOf(std::array{1, 2, 3}, allocator);
    }

    [[gnu::noinline]] AlignedGenerator AlignedLocalAddress(std::allocator_arg_t, const MinimallyAlignedAllocator&) {
        alignas(std::max_align_t) std::array<std::byte, alignof(std::max_align_t)> local{};
        co_yield reinterpret_cast<std::uintptr_t>(local.data());
    }

    [[gnu::noinline]] OverAlignedGenerator OverAlignedLocalAddress(std::allocator_arg_t,
                                                                   const MinimallyAlignedAllocator&) {
        alignas(64) std::array<std::byte, 64> local{};
        co_yield reinterpret_cast<std::uintptr_t>(local.data());
    }

    [[gnu::noinline]] GloballyOverAlignedGenerator GloballyOverAlignedLocalAddress() {
        alignas(64) std::array<std::byte, 64> local{};
        co_yield reinterpret_cast<std::uintptr_t>(local.data());
    }

    template<std::ranges::input_range Range>
    [[nodiscard]] auto Collect(Range&& range) {
        using Value = std::ranges::range_value_t<Range>;
        std::vector<Value> values;
        for (auto&& value : range) {
            values.emplace_back(std::forward<decltype(value)>(value));
        }
        return values;
    }

    static_assert(std::ranges::input_range<IntGenerator>);
    static_assert(!std::ranges::forward_range<IntGenerator>);
    static_assert(std::same_as<std::ranges::range_value_t<IntGenerator>, int>);
    static_assert(std::same_as<std::ranges::range_reference_t<IntGenerator>, int&&>);
    static_assert(std::same_as<std::ranges::range_reference_t<Sora::Generator<int&>>, int&>);
    static_assert(!std::copy_constructible<IntGenerator>);
    static_assert(std::movable<IntGenerator>);
    static_assert(Sora::Detail::Gen::ValidGeneratorParams<int, void>);
    static_assert(!Sora::Detail::Gen::ValidGeneratorParams<void, void>);
    static_assert(Sora::Detail::Gen::ValidGeneratorAllocator<Allocator>);
    static_assert(!Sora::Detail::Gen::ValidGeneratorAllocator<FancyAllocator<std::byte>>);

} // namespace

TEST_CASE("Generator is lazy and yields values in order", "[Sora.Core.Concurrency.Generator]") {
    bool started = false;
    auto values = LazyValues(started);
    REQUIRE_FALSE(started);
    REQUIRE(Collect(values) == std::vector{2, 3});
    REQUIRE(started);
}

TEST_CASE("Generator preserves reference identity and mutation", "[Sora.Core.Concurrency.Generator]") {
    std::array values{1, 2, 3};
    for (int& value : References(values)) {
        value *= 4;
    }
    REQUIRE(values == std::array{4, 8, 12});
}

TEST_CASE("Generator transfers move-only yielded values exactly once", "[Sora.Core.Concurrency.Generator]") {
    auto values = Collect(MoveOnlyValues());
    REQUIRE(values.size() == 2);
    REQUIRE(*values[0] == 5);
    REQUIRE(*values[1] == 8);
}

TEST_CASE("Generator delegates nested, empty, and exhausted generators", "[Sora.Core.Concurrency.Generator]") {
    REQUIRE(Collect(NestedValues()) == std::vector{-1, 0, 1, 2});
}

TEST_CASE("Generator delegates borrowed and owning ranges without dangling", "[Sora.Core.Concurrency.Generator]") {
    std::vector values{3, 5, 7};
    for (int& value : DelegatedReferences(values)) {
        ++value;
    }
    REQUIRE(values == std::vector{4, 6, 8});
    REQUIRE(Collect(DelegatedOwningRange()) == std::vector{4, 6, 9});
}

TEST_CASE("Generator propagates direct and delegated exceptions", "[Sora.Core.Concurrency.Generator]") {
    auto direct = DirectFailure();
    auto directIterator = direct.begin();
    REQUIRE(*directIterator == 1);
    REQUIRE_THROWS_AS(++directIterator, std::runtime_error);

    auto nested = NestedFailure();
    auto nestedIterator = nested.begin();
    REQUIRE(*nestedIterator == 6);
    ++nestedIterator;
    REQUIRE(*nestedIterator == 7);
    REQUIRE_THROWS_AS(++nestedIterator, std::runtime_error);
    REQUIRE(Collect(RecoverNestedFailure()) == std::vector{7, 11});
}

TEST_CASE("Generator move operations transfer coroutine ownership", "[Sora.Core.Concurrency.Generator]") {
    auto source = Leaf(1, 4);
    auto moved = std::move(source);
    REQUIRE_FALSE(source);
    REQUIRE(moved);

    IntGenerator assigned;
    assigned = std::move(moved);
    REQUIRE_FALSE(moved);
    REQUIRE(Collect(assigned) == std::vector{1, 2, 3});
}

TEST_CASE("Generator allocator owns and balances every coroutine frame", "[Sora.Core.Concurrency.Generator]") {
    AllocationCounters counters;
    Allocator allocator{counters};
    {
        auto values = AllocatedValues(std::allocator_arg, allocator, 11);
        REQUIRE(counters.allocations == 1);
        REQUIRE(Collect(values) == std::vector{11});
        REQUIRE(counters.deallocations == 0);
    }
    REQUIRE(counters.allocations == counters.deallocations);
    REQUIRE(counters.allocatedBytes == counters.deallocatedBytes);

    {
        auto values = AllocatedDelegation(std::allocator_arg, allocator);
        REQUIRE(Collect(values) == std::vector{1, 2, 3});
    }
    REQUIRE(counters.allocations == counters.deallocations);
    REQUIRE(counters.allocatedBytes == counters.deallocatedBytes);
}

TEST_CASE("Generator aligns frames independently of byte allocator alignment", "[Sora.Core.Concurrency.Generator]") {
    MinimallyAlignedAllocator allocator;
    auto addresses = AlignedLocalAddress(std::allocator_arg, allocator);
    const std::uintptr_t address = *addresses.begin();
    REQUIRE(address % alignof(std::max_align_t) == 0);

    auto overAlignedAddresses = OverAlignedLocalAddress(std::allocator_arg, allocator);
    const std::uintptr_t overAlignedAddress = *overAlignedAddresses.begin();
    REQUIRE(overAlignedAddress % 64 == 0);

    auto globallyOverAlignedAddresses = GloballyOverAlignedLocalAddress();
    const std::uintptr_t globallyOverAlignedAddress = *globallyOverAlignedAddresses.begin();
    REQUIRE(globallyOverAlignedAddress % 64 == 0);
}

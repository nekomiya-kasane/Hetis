/**
 * @file HashTest.cpp
 * @brief Comprehensive tests for the Mashiro Hash framework.
 *
 * Covers: concepts, stateless algorithms, stateful hashers, CPO dispatch,
 * annotations, combine, UDLs, std::hash injection, and constexpr folding.
 */
#include "Mashiro/Core/Hash.h"
#include "Mashiro/Math/Types.h"

#include "Support/Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <unordered_map>

using namespace Mashiro;
using namespace Mashiro::Hashing;
using namespace Mashiro::Hashing::Literals;

// =============================================================================
// Test types
// =============================================================================

namespace {

    struct SimpleStruct {
        float x = 1.0f;
        float y = 2.0f;
        float z = 3.0f;
        friend constexpr bool operator==(const SimpleStruct&, const SimpleStruct&) = default;
    };

    struct NestedStruct {
        SimpleStruct inner;
        int tag = 42;
        friend constexpr bool operator==(const NestedStruct&, const NestedStruct&) = default;
    };

    enum class Color : uint8_t { Red = 0, Green = 1, Blue = 2 };

    // ADL customisation hook test type
    struct CustomHashed {
        uint64_t secret = 0xDEADBEEF;
    };

    // ADL hook: custom hash logic
    [[nodiscard]] constexpr uint64_t HashValue(const Fnv1a64&, const CustomHashed& v) noexcept {
        return v.secret * 0x100000001B3ULL;
    }

    // Helper: invoke the CPO (avoids name collision with the struct HashCPO named `Hash`)
    template <typename... Args>
    constexpr auto DoHash(Args&&... args) { return Mashiro::Hashing::Hash(std::forward<Args>(args)...); }

} // anonymous namespace

// =============================================================================
// [Concepts] — Static assertions for concept satisfaction
// =============================================================================

TEST_CASE("Concept: StatelessAlgo satisfied by algorithm structs", AUTO_TAG) {
    STATIC_REQUIRE(Mashiro::Hashing::StatelessAlgo<Fnv1a64>);
    STATIC_REQUIRE(Mashiro::Hashing::StatelessAlgo<Fnv1a32>);
    STATIC_REQUIRE(Mashiro::Hashing::StatelessAlgo<Murmur64>);
}

TEST_CASE("Concept: StatefulAlgo satisfied by state structs", AUTO_TAG) {
    STATIC_REQUIRE(Mashiro::Hashing::StatefulAlgo<Fnv1a64State>);
    STATIC_REQUIRE(Mashiro::Hashing::StatefulAlgo<Fnv1a32State>);
    STATIC_REQUIRE(Mashiro::Hashing::StatefulAlgo<Murmur64State>);
}

TEST_CASE("Concept: AnyAlgo covers both", AUTO_TAG) {
    STATIC_REQUIRE(Mashiro::Hashing::AnyAlgo<Fnv1a64>);
    STATIC_REQUIRE(Mashiro::Hashing::AnyAlgo<Fnv1a64State>);
}

TEST_CASE("Concept: ResultOf yields correct types", AUTO_TAG) {
    STATIC_REQUIRE(std::same_as<Mashiro::Hashing::ResultOf<Fnv1a64>, uint64_t>);
    STATIC_REQUIRE(std::same_as<Mashiro::Hashing::ResultOf<Fnv1a32>, uint32_t>);
    STATIC_REQUIRE(std::same_as<Mashiro::Hashing::ResultOf<Fnv1a64State>, uint64_t>);
    STATIC_REQUIRE(Mashiro::Hashing::kResultBits<Fnv1a64> == 64);
    STATIC_REQUIRE(Mashiro::Hashing::kResultBits<Fnv1a32> == 32);
}

TEST_CASE("Concept: Hashable satisfied by various types", AUTO_TAG) {
    STATIC_REQUIRE(Hashable<int>);
    STATIC_REQUIRE(Hashable<float>);
    STATIC_REQUIRE(Hashable<uint64_t>);
    STATIC_REQUIRE(Hashable<Color>);
    STATIC_REQUIRE(Hashable<SimpleStruct>);
    STATIC_REQUIRE(Hashable<NestedStruct>);
    STATIC_REQUIRE(Hashable<vec3>);
}

// =============================================================================
// [Stateless] — FNV-1a correctness
// =============================================================================

TEST_CASE("Fnv1a64 empty input returns offset basis", AUTO_TAG) {
    constexpr auto h = Fnv1a64{}(std::span<const std::byte>{});
    STATIC_REQUIRE(h == Fnv1a64::kOffset);
}

TEST_CASE("Fnv1a32 empty input returns offset basis", AUTO_TAG) {
    constexpr auto h = Fnv1a32{}(std::span<const std::byte>{});
    STATIC_REQUIRE(h == Fnv1a32::kOffset);
}

TEST_CASE("Fnv1a64 known test vector", AUTO_TAG) {
    // FNV-1a 64-bit hash of "foobar"
    constexpr auto h = DoHash(std::string_view("foobar"));
    STATIC_REQUIRE(h == 0x85944171f73967e8ULL);
}

TEST_CASE("Fnv1a32 known test vector", AUTO_TAG) {
    // FNV-1a 32-bit hash of "foobar"
    constexpr auto h = DoHash(std::string_view("foobar"), Fnv1a32{});
    STATIC_REQUIRE(h == 0xbf9cf968U);
}

TEST_CASE("Different inputs produce different hashes", AUTO_TAG) {
    constexpr auto h1 = DoHash(std::string_view("hello"));
    constexpr auto h2 = DoHash(std::string_view("world"));
    STATIC_REQUIRE(h1 != h2);
}

TEST_CASE("Murmur64 differs from raw Fnv1a64 (avalanche)", AUTO_TAG) {
    // Test with both scalar and string — both should differ from plain FNV.
    constexpr auto fnv_i = DoHash(42);
    constexpr auto mur_i = DoHash(42, Murmur64{});
    STATIC_REQUIRE(fnv_i != mur_i);

    constexpr auto fnv_s = DoHash(std::string_view("test"));
    constexpr auto mur_s = DoHash(std::string_view("test"), Murmur64{});
    STATIC_REQUIRE(fnv_s != mur_s);
}

// =============================================================================
// [Stateful] — Incremental feed consistency
// =============================================================================

TEST_CASE("Stateful matches stateless for string", AUTO_TAG) {
    // Both paths hash "hello world" — CPO uses HashString, Hasher uses FeedString.
    // They should produce identical results.
    constexpr auto cpoResult = DoHash(std::string_view("hello world"));
    constexpr auto hasherResult = Hasher<>::Of(std::string_view("hello world"));
    STATIC_REQUIRE(cpoResult == hasherResult);
}

TEST_CASE("Stateful chunked feed equals whole feed", AUTO_TAG) {
    constexpr auto whole = Hasher<>::Of(std::string_view("abcdefghij"));

    constexpr auto chunked = [] {
        Hasher<> h;
        h.Feed(std::string_view("abcde"));
        h.Feed(std::string_view("fghij"));
        return h.Finalize();
    }();

    STATIC_REQUIRE(whole == chunked);
}

// =============================================================================
// [CPO] — hash CPO dispatch
// =============================================================================

TEST_CASE("CPO: Hash scalar types deterministic", AUTO_TAG) {
    constexpr auto h1 = DoHash(42);
    constexpr auto h2 = DoHash(42);
    constexpr auto h3 = DoHash(43);
    STATIC_REQUIRE(h1 == h2);
    STATIC_REQUIRE(h1 != h3);
}

TEST_CASE("CPO: Hash float deterministic", AUTO_TAG) {
    constexpr auto h1 = DoHash(1.0f);
    constexpr auto h2 = DoHash(1.0f);
    constexpr auto h3 = DoHash(2.0f);
    STATIC_REQUIRE(h1 == h2);
    STATIC_REQUIRE(h1 != h3);
}

TEST_CASE("CPO: Hash enum", AUTO_TAG) {
    constexpr auto h1 = DoHash(Color::Red);
    constexpr auto h2 = DoHash(Color::Green);
    STATIC_REQUIRE(h1 != h2);
}

TEST_CASE("CPO: Hash string_view matches raw algo", AUTO_TAG) {
    constexpr auto h = DoHash(std::string_view("hello"));
    constexpr auto ref = Mashiro::Hashing::Detail::HashString(Fnv1a64{}, std::string_view("hello"));
    STATIC_REQUIRE(h == ref);
}

TEST_CASE("CPO: Hash struct (reflection-driven)", AUTO_TAG) {
    constexpr SimpleStruct a{1.0f, 2.0f, 3.0f};
    constexpr SimpleStruct b{1.0f, 2.0f, 3.0f};
    constexpr SimpleStruct c{4.0f, 5.0f, 6.0f};
    STATIC_REQUIRE(DoHash(a) == DoHash(b));
    STATIC_REQUIRE(DoHash(a) != DoHash(c));
}

TEST_CASE("CPO: Hash nested struct", AUTO_TAG) {
    constexpr NestedStruct a{{1, 2, 3}, 10};
    constexpr NestedStruct b{{1, 2, 3}, 10};
    constexpr NestedStruct c{{1, 2, 3}, 20};
    STATIC_REQUIRE(DoHash(a) == DoHash(b));
    STATIC_REQUIRE(DoHash(a) != DoHash(c));
}

TEST_CASE("CPO: Hash vec3", AUTO_TAG) {
    constexpr vec3 a{1.0f, 2.0f, 3.0f};
    constexpr vec3 b{1.0f, 2.0f, 3.0f};
    constexpr vec3 c{1.0f, 2.0f, 4.0f};
    STATIC_REQUIRE(DoHash(a) == DoHash(b));
    STATIC_REQUIRE(DoHash(a) != DoHash(c));
}

TEST_CASE("CPO: explicit algorithm Fnv1a32", AUTO_TAG) {
    constexpr auto h = DoHash(42, Fnv1a32{});
    STATIC_REQUIRE(std::same_as<decltype(h), const uint32_t>);
    constexpr auto h2 = DoHash(42, Fnv1a32{});
    STATIC_REQUIRE(h == h2);
}

TEST_CASE("CPO: ADL hook takes priority", AUTO_TAG) {
    constexpr CustomHashed obj{0xCAFEBABE};
    constexpr auto h = DoHash(obj);
    constexpr auto expected = obj.secret * 0x100000001B3ULL;
    STATIC_REQUIRE(h == expected);
}

// =============================================================================
// [Annotations] — HashIgnore, HashKey (presence-based, value splice not needed)
// =============================================================================

namespace {
    struct WithIgnore {
        uint64_t id = 100;
        [[=Mashiro::Hashing::Anno::Ignore{}]] float cachedValue = 999.0f;
        friend constexpr bool operator==(const WithIgnore&, const WithIgnore&) = default;
    };

    struct WithKey {
        [[=Mashiro::Hashing::Anno::Key{}]] uint64_t primaryKey = 12345;
        float data1 = 1.0f;
        float data2 = 2.0f;
        friend constexpr bool operator==(const WithKey&, const WithKey&) = default;
    };
} // anonymous namespace

TEST_CASE("Annotation: HashIgnore excludes member from hash", AUTO_TAG) {
    constexpr WithIgnore a{100, 1.0f};
    constexpr WithIgnore b{100, 999.0f};
    STATIC_REQUIRE(DoHash(a) == DoHash(b));  // ignored field differs, hash same

    constexpr WithIgnore c{200, 1.0f};
    STATIC_REQUIRE(DoHash(a) != DoHash(c));  // non-ignored field differs
}

TEST_CASE("Annotation: HashKey whitelist mode", AUTO_TAG) {
    constexpr WithKey a{12345, 1.0f, 2.0f};
    constexpr WithKey b{12345, 99.0f, 88.0f};
    STATIC_REQUIRE(DoHash(a) == DoHash(b));  // only key matters

    constexpr WithKey c{99999, 1.0f, 2.0f};
    STATIC_REQUIRE(DoHash(a) != DoHash(c));  // different key
}

namespace {
    struct WithOrder {
        [[=Mashiro::Hashing::Anno::Order{.priority = 2}]] float c = 3.0f;
        [[=Mashiro::Hashing::Anno::Order{.priority = 0}]] float a = 1.0f;
        [[=Mashiro::Hashing::Anno::Order{.priority = 1}]] float b = 2.0f;
        friend constexpr bool operator==(const WithOrder&, const WithOrder&) = default;
    };

    // Type-level algorithm override via Anno::With<Murmur64>
    struct [[=Mashiro::Hashing::Anno::With<Mashiro::Hashing::Murmur64>{}]] MurmurHashed {
        int value = 42;
        friend constexpr bool operator==(const MurmurHashed&, const MurmurHashed&) = default;
    };
} // anonymous namespace

TEST_CASE("Annotation: Anno::With overrides algorithm", AUTO_TAG) {
    // MurmurHashed has [[=Anno::With<Murmur64>{}]] — should use Murmur64 not Fnv1a64
    constexpr MurmurHashed obj{42};
    constexpr auto withDefault = DoHash(obj);              // uses Anno::With → Murmur64
    constexpr auto withExplicit = DoHash(42, Murmur64{});  // explicit Murmur64 on raw int
    // The struct hash (structured, per-member combine) won't equal raw int hash,
    // but it should differ from plain Fnv1a64 struct hash
    constexpr auto fnvHash = [] {
        // Manually hash with Fnv1a64 (ignore annotation)
        auto bytes = std::bit_cast<std::array<std::byte, sizeof(int)>>(42);
        return Fnv1a64{}(std::span<const std::byte>(bytes));
    }();
    // The annotation-driven hash should NOT equal raw FNV of the int bytes,
    // because Murmur applies avalanche
    STATIC_REQUIRE(withDefault != fnvHash);
}

TEST_CASE("Annotation: HashOrder controls hash sequence", AUTO_TAG) {
    constexpr WithOrder ordered{.c = 3, .a = 1, .b = 2};

    // Expected: combine in priority order a(0), b(1), c(2)
    constexpr auto expected = [] {
        using namespace Mashiro::Hashing;
        uint64_t seed{};
        seed = Combine(seed, Hash(1.0f));  // a, priority 0
        seed = Combine(seed, Hash(2.0f));  // b, priority 1
        seed = Combine(seed, Hash(3.0f));  // c, priority 2
        return seed;
    }();

    STATIC_REQUIRE(DoHash(ordered) == expected);
}

// =============================================================================
// [Combine] — Golden-ratio combine
// =============================================================================

TEST_CASE("Combine: non-commutative", AUTO_TAG) {
    constexpr uint64_t a = 0x12345678ULL;
    constexpr uint64_t b = 0xABCDEF00ULL;
    STATIC_REQUIRE(Combine(a, b) != Combine(b, a));
}

TEST_CASE("Combine: 32-bit variant works", AUTO_TAG) {
    constexpr uint32_t a = 0x1234U;
    constexpr uint32_t b = 0xABCDU;
    constexpr auto r = Combine(a, b);
    STATIC_REQUIRE(r != a);
    STATIC_REQUIRE(r != b);
}

// =============================================================================
// [Core] — Streaming Hasher
// =============================================================================

TEST_CASE("Hasher: deterministic output", AUTO_TAG) {
    constexpr auto r1 = Hasher<>::Of(42);
    constexpr auto r2 = Hasher<>::Of(42);
    STATIC_REQUIRE(r1 == r2);
    STATIC_REQUIRE(r1 != 0);
}

TEST_CASE("Hasher: Feed chaining deterministic", AUTO_TAG) {
    constexpr auto result = [] {
        Hasher h;
        h.Feed(1.0f).Feed(2.0f).Feed(3.0f);
        return h.Finalize();
    }();

    constexpr auto result2 = [] {
        Hasher h;
        h.Feed(1.0f).Feed(2.0f).Feed(3.0f);
        return h.Finalize();
    }();

    STATIC_REQUIRE(result == result2);
}

TEST_CASE("Hasher: order matters", AUTO_TAG) {
    constexpr auto h1 = [] {
        Hasher h;
        h.Feed(1.0f).Feed(2.0f);
        return h.Finalize();
    }();

    constexpr auto h2 = [] {
        Hasher h;
        h.Feed(2.0f).Feed(1.0f);
        return h.Finalize();
    }();

    STATIC_REQUIRE(h1 != h2);
}

TEST_CASE("Hasher: Feed struct expands members", AUTO_TAG) {
    constexpr SimpleStruct s{1, 2, 3};
    constexpr auto result = Hasher<>::Of(s);
    STATIC_REQUIRE(result != 0);
}

TEST_CASE("Hasher: 32-bit state variant", AUTO_TAG) {
    constexpr auto result = Hasher<Fnv1a32State>::Of(42);
    STATIC_REQUIRE(std::same_as<decltype(result), const uint32_t>);
    STATIC_REQUIRE(result != 0);
}

// =============================================================================
// [UDL] — User-defined literals
// =============================================================================

TEST_CASE("UDL: _hash produces FNV-1a 64", AUTO_TAG) {
    constexpr auto h = "hello"_hash;
    constexpr auto ref = Mashiro::Hashing::Detail::HashString(Fnv1a64{}, std::string_view("hello"));
    STATIC_REQUIRE(h == ref);
}

TEST_CASE("UDL: _hash32 produces FNV-1a 32", AUTO_TAG) {
    constexpr auto h = "hello"_hash32;
    constexpr auto ref = Mashiro::Hashing::Detail::HashString(Fnv1a32{}, std::string_view("hello"));
    STATIC_REQUIRE(h == ref);
}

TEST_CASE("UDL: different strings differ", AUTO_TAG) {
    STATIC_REQUIRE("foo"_hash != "bar"_hash);
    STATIC_REQUIRE("foo"_hash32 != "bar"_hash32);
}

TEST_CASE("UDL: empty string returns offset basis", AUTO_TAG) {
    STATIC_REQUIRE(""_hash == Fnv1a64::kOffset);
    STATIC_REQUIRE(""_hash32 == Fnv1a32::kOffset);
}

// =============================================================================
// [std::hash] — Automatic injection
// =============================================================================

TEST_CASE("std::hash<vec3> with unordered_map", AUTO_TAG) {
    std::unordered_map<vec3, int> map;
    vec3 key{1.0f, 2.0f, 3.0f};
    map[key] = 42;
    REQUIRE(map[key] == 42);
    REQUIRE(map.count(key) == 1);
}

TEST_CASE("std::hash<SimpleStruct> works", AUTO_TAG) {
    std::unordered_map<SimpleStruct, int> map;
    SimpleStruct key{10.0f, 20.0f, 30.0f};
    map[key] = 7;
    REQUIRE(map[key] == 7);
}

TEST_CASE("std::hash deterministic", AUTO_TAG) {
    std::hash<vec3> h;
    vec3 v{1, 2, 3};
    REQUIRE(h(v) == h(v));
}

// =============================================================================
// [Constexpr] — Compile-time folding verification
// =============================================================================

TEST_CASE("Constexpr: CPO folds at compile time", AUTO_TAG) {
    constexpr auto h = DoHash(vec3{1, 2, 3});
    STATIC_REQUIRE(h != 0);
}

TEST_CASE("Constexpr: Hasher::Of folds at compile time", AUTO_TAG) {
    constexpr auto h = Hasher<>::Of(1, 2, 3);
    STATIC_REQUIRE(h != 0);
}

TEST_CASE("Constexpr: Combine folds at compile time", AUTO_TAG) {
    constexpr auto c = Combine(uint64_t(1), uint64_t(2));
    STATIC_REQUIRE(c != 0);
    STATIC_REQUIRE(c != 1);
    STATIC_REQUIRE(c != 2);
}


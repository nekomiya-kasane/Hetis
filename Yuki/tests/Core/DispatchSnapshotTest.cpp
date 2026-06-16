/**
 * @file DispatchSnapshotTest.cpp
 * @brief Tests for the immutable @ref Yuki::DispatchSnapshot and its binary-search lookup.
 *
 * Covers the scaffolding introduced for spec §2.1 of the Yuki Closure Architecture:
 *  - An empty snapshot (count == 0 or null pointer) yields a @c nullptr lookup result.
 *  - A sorted snapshot binary-searches its entries, returning the matching @ref Yuki::DispatchEntry
 *    on hit and @c nullptr on miss (including misses outside the bracketed range).
 *  - @ref Yuki::MetaLinks::dispatch is an atomic pointer to a snapshot, so registrars can later
 *    release-publish and readers acquire-load.
 *
 * The retirement chain (the @c previous link) is built but not exercised here — Task 5 lands the
 * retirement pass and its own test coverage. Payload arms are spelled by name to lock in the
 * union member layout.
 */
#include <Yuki/Core/MetaClass.h>

#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <type_traits>

using namespace Yuki;

TEST_CASE("Empty DispatchSnapshot lookup returns nullptr", AUTO_TAG) {
    DispatchSnapshot empty{0, nullptr, nullptr};
    REQUIRE(Detail::LookupEntry(&empty, Iid{Mashiro::Uuid{1, 0}}) == nullptr);
    // A null snapshot pointer must also short-circuit — readers acquire-load this field on the hot
    // path before any class has registered, so a nullptr deref here would be a runtime crash.
    REQUIRE(Detail::LookupEntry(nullptr, Iid{Mashiro::Uuid{1, 0}}) == nullptr);
}

TEST_CASE("Sorted DispatchSnapshot binary-search hits and misses correctly", AUTO_TAG) {
    DispatchEntry entries[3] = {
        {Iid{Mashiro::Uuid{1, 0}}, DispatchKind::DirectCast,             {.staticOffset = 8}},
        {Iid{Mashiro::Uuid{5, 0}}, DispatchKind::InlineFacade,           {.staticOffset = 16}},
        {Iid{Mashiro::Uuid{9, 0}}, DispatchKind::CodeExtensionSingleton, {.singleton = nullptr}},
    };
    DispatchSnapshot snap{3, entries, nullptr};

    // Hits at all three indices — first, middle, last. The last-entry hit is the case most likely
    // to expose a `hi`-exclusive vs. `hi`-inclusive bug, and the singleton payload arm is only
    // *read* here, locking in the union-by-kind discipline downstream code will depend on.
    auto* e1 = Detail::LookupEntry(&snap, Iid{Mashiro::Uuid{1, 0}});
    REQUIRE(e1 == &entries[0]);  // identity, not copy — caller relies on pointer-into-array.
    REQUIRE(e1->kind == DispatchKind::DirectCast);

    auto* e5 = Detail::LookupEntry(&snap, Iid{Mashiro::Uuid{5, 0}});
    REQUIRE(e5 == &entries[1]);
    REQUIRE(e5->kind == DispatchKind::InlineFacade);

    auto* e9 = Detail::LookupEntry(&snap, Iid{Mashiro::Uuid{9, 0}});
    REQUIRE(e9 == &entries[2]);
    REQUIRE(e9->kind == DispatchKind::CodeExtensionSingleton);
    REQUIRE(e9->payload.singleton == nullptr);

    // Misses below the first entry, strictly between two entries, and above the last entry. The
    // binary-search window is `[lo, hi)`; an off-by-one that fails to pin `lo` at 0 (below-first)
    // or that confuses `hi`-exclusive with `hi`-inclusive (above-last) would surface here.
    REQUIRE(Detail::LookupEntry(&snap, Iid{Mashiro::Uuid{0, 0}}) == nullptr);
    REQUIRE(Detail::LookupEntry(&snap, Iid{Mashiro::Uuid{4, 0}}) == nullptr);
    REQUIRE(Detail::LookupEntry(&snap, Iid{Mashiro::Uuid{10, 0}}) == nullptr);
}

TEST_CASE("MetaLinks dispatch field is std::atomic<const DispatchSnapshot*>", AUTO_TAG) {
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<MetaLinks>().dispatch),
                                  std::atomic<const DispatchSnapshot*>>);
}

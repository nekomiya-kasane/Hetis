/**
 * @file FacadeListTest.cpp
 * @brief Tests for the cardinality-enforcing helpers on Yuki/Core/FacadeList.h.
 *
 * Covers @ref Yuki::FacadeListLookup (free-function form over a @c FacadeListHead) and
 * @ref Yuki::AttachUnique — the iid-keyed CAS dedup primitive that enforces "exactly one Extension
 * instance per (Extension type, closure)" per spec §1.5 and is the shared kernel used by both eager
 * materialisation and the lazy @c SideTableResolver path (spec §5.4 step 2).
 *
 * Nodes are stack-allocated and owned by the test — @ref Yuki::AttachUnique does not allocate, so
 * the tests exercise the CAS plumbing without involving the @c FacadeNode arena.
 */
#include <Yuki/Core/FacadeList.h>
#include <Yuki/Core/MetaClass.h>

#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace Yuki;

namespace {

    /// @brief Cast a stable, distinctive integer pattern into a @c RootObject* used as an identity
    ///        sentinel. The returned pointer is *never dereferenced* by the tests — it only flows
    ///        through @c FacadeNode::facade and out of @c FacadeListLookup, which exercises the read
    ///        path against a non-null payload distinct from anything the implementation could fabricate
    ///        by accident. @c RootObject has a pure-virtual member, so we cannot instantiate one in a
    ///        test fixture; the reinterpret_cast keeps the identity-sentinel intent explicit.
    [[nodiscard]] RootObject* SentinelFacade(std::uintptr_t tag) noexcept {
        return reinterpret_cast<RootObject*>(tag);
    }

    /// @brief Build a fresh node with a chosen IID. @c facade defaults to @c nullptr so the existing
    ///        AttachUnique-winner-identity test (which only cares about node identity) keeps working
    ///        unchanged; callers that want to exercise the read path through @c FacadeListLookup pass
    ///        a non-null @c RootObject* sentinel. @c next always starts null.
    ///        The two-arg @c Uuid ctor lets us spell distinct test IIDs cheaply without parsing.
    [[nodiscard]] FacadeNode MakeNode(std::uint64_t hi,
                                      RootObject* facade = nullptr,
                                      std::uint64_t lo = 0) noexcept {
        return FacadeNode{Iid{Mashiro::Uuid{hi, lo}}, facade, /*next=*/nullptr};
    }

} // namespace

TEST_CASE("FacadeListLookup returns nullptr on empty list", AUTO_TAG) {
    FacadeListHead head;
    REQUIRE(FacadeListLookup(head, Iid{Mashiro::Uuid{1, 0}}) == nullptr);
}

TEST_CASE("AttachUnique inserts when iid is absent", AUTO_TAG) {
    FacadeListHead head;
    RootObject* const facade = SentinelFacade(0xCAFEBABEULL);
    FacadeNode node = MakeNode(42, facade);
    REQUIRE(AttachUnique(head, &node) == &node);
    // Exercise the read path with a non-null payload: a buggy Lookup that always returns nullptr
    // would now fail this assertion, which the prior `nullptr == nullptr` form did not catch.
    REQUIRE(FacadeListLookup(head, Iid{Mashiro::Uuid{42, 0}}) == facade);
    // Walk the chain by hand to assert the node was actually linked.
    REQUIRE(head.Head() == &node);
}

TEST_CASE("AttachUnique returns the existing node when iid is already present", AUTO_TAG) {
    FacadeListHead head;
    FacadeNode first = MakeNode(7);
    FacadeNode second = MakeNode(7);
    REQUIRE(AttachUnique(head, &first) == &first);
    REQUIRE(AttachUnique(head, &second) == &first);
    // Head must still point at the winner — the loser's `next` was never linked in.
    REQUIRE(head.Head() == &first);
    REQUIRE(first.next == nullptr);
}

TEST_CASE("AttachUnique chains distinct iids", AUTO_TAG) {
    FacadeListHead head;
    RootObject* const facadeA = SentinelFacade(0xA1A1A1A1ULL);
    RootObject* const facadeB = SentinelFacade(0xB2B2B2B2ULL);
    FacadeNode a = MakeNode(1, facadeA);
    FacadeNode b = MakeNode(2, facadeB);
    REQUIRE(AttachUnique(head, &a) == &a);
    REQUIRE(AttachUnique(head, &b) == &b);
    // Newest-first push: head should be b, b.next should be a, a.next null.
    REQUIRE(head.Head() == &b);
    REQUIRE(b.next == &a);
    REQUIRE(a.next == nullptr);
    // Both IIDs are reachable through lookup and return their distinct sentinel payloads, so the
    // read path is exercised end-to-end on a chain rather than a single-node head.
    REQUIRE(FacadeListLookup(head, Iid{Mashiro::Uuid{1, 0}}) == facadeA);
    REQUIRE(FacadeListLookup(head, Iid{Mashiro::Uuid{2, 0}}) == facadeB);
}

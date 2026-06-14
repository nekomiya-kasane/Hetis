/**
 * @file RefCountedTest.cpp
 * @brief Tests for opt-in intrusive reference counting on object-model classes.
 *
 * The object model reuses `Mashiro/Core/RefCountedMixin.h` rather than rolling its own: only classes
 * that truly share ownership opt in to `RefCountedAtomic<Self>` (or the single-threaded
 * `RefCounted<Self>`). This file verifies retain/release counting, that the final release destroys
 * the object, and — the key design property — that a non-opt-in `MetaNode` class carries no counter
 * field (a single vptr only), so the stack / `unique_ptr` paths pay nothing.
 */
#include <Yuki/Core/RootObject.h>

#include <Mashiro/Core/RefCountedMixin.h>

#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

using namespace Yuki;
using Mashiro::RefCountedAtomic;

namespace {

    // Shared-ownership Object: opts in to atomic intrusive counting.
    struct [[=Anno::Implementation]] SharedNode
        : MetaNode<SharedNode>, RefCountedAtomic<SharedNode> {
        inline static int liveCount = 0;
        SharedNode() { ++liveCount; }
        ~SharedNode() { --liveCount; }
    };

    // Value-only Object: no counting opt-in — must stay vptr-sized.
    struct [[=Anno::Implementation]] PlainNode
        : MetaNode<PlainNode> {
        double payload{0.0};
    };

} // namespace

// =============================================================================
// Counting lifecycle
// =============================================================================

TEST_CASE("AddRef/Release track the count; final release destroys", AUTO_TAG) {
    SharedNode::liveCount = 0;
    auto* n = new SharedNode{};  // refCount == 1
    REQUIRE(n->RefCount() == 1);
    REQUIRE(SharedNode::liveCount == 1);

    n->AddRef();
    REQUIRE(n->RefCount() == 2);

    n->Release();  // back to 1, no destruction
    REQUIRE(n->RefCount() == 1);
    REQUIRE(SharedNode::liveCount == 1);

    n->Release();  // 0 -> delete
    REQUIRE(SharedNode::liveCount == 0);
}

// =============================================================================
// Storage: opt-in costs a small counter; opting out costs nothing
// =============================================================================

TEST_CASE("Non-opt-in node carries no counter (vptr only)", AUTO_TAG) {
    STATIC_REQUIRE(sizeof(PlainNode) == sizeof(void*) + sizeof(double));
}

TEST_CASE("Opt-in node adds only the counter width", AUTO_TAG) {
    // SharedNode = PlainNode-shaped anchor (vptr) + RefCountedAtomic's uint16 counter.
    // The exact size is layout-dependent, but it must exceed a bare vptr and stay small.
    STATIC_REQUIRE(sizeof(SharedNode) > sizeof(void*));
    STATIC_REQUIRE(sizeof(SharedNode) <= sizeof(void*) + 2 * sizeof(void*));
}

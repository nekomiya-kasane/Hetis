/**
 * @file SnapshotRetirementTest.cpp
 * @brief Tests for the deferred-reclaim scaffolding declared in @ref Yuki/Core/MetaClass.h.
 *
 * Covers the spec §2.3 contract for @ref Yuki::Detail::RetireSnapshot and its peer functions:
 *  - A retired snapshot is owned by the pending list until the next sweep.
 *  - @ref Yuki::Detail::SweepRetirements drains every queued entry and runs each deleter.
 *  - Idle queues sweep cheaply (no-op when nothing is pending).
 *  - Null inputs short-circuit without enqueuing.
 *
 * Snapshots are heap-allocated here so the test deleters can call @c delete; in production the
 * registrar's arena owns the lifetime instead, but the contract — "the deleter freed it" — is the
 * same. We use a local @c std::atomic<int> counter as a witness that the deleter ran exactly once
 * per retired snapshot, which is the property the publish/retire dance relies on.
 *
 * @note These tests touch a process-wide pending list. Catch2 runs test cases sequentially by
 *       default, but each test sweeps at the end so it leaves the list empty for the next test.
 */
#include <Yuki/Core/MetaClass.h>

#include "Meta.h"
#include <catch2/catch_test_macros.hpp>

#include <atomic>

using namespace Yuki;

namespace {

    // Witness counter incremented by the test deleters — gives us a side-channel to assert that
    // each registered deleter ran exactly once per sweep. Reset to zero at the start of every test
    // that uses it.
    std::atomic<int> g_deleterCalls{0};

    /// @brief Test deleter that frees an arena-shape snapshot allocated with @c new/new[].
    void DeleteHeapSnapshot(const DispatchSnapshot* s) noexcept {
        g_deleterCalls.fetch_add(1, std::memory_order_relaxed);
        delete[] s->entries;
        delete s;
    }

    /// @brief Build a one-entry snapshot on the heap so the test deleter can free it.
    [[nodiscard]] const DispatchSnapshot* MakeHeapSnapshot(std::uint64_t iidLo) {
        auto* entries = new DispatchEntry[1]{
            {Iid{Mashiro::Uuid{iidLo, 0}}, DispatchKind::DirectCast, {.staticOffset = 0}},
        };
        return new DispatchSnapshot{1, entries, nullptr};
    }

} // namespace

TEST_CASE("RetireSnapshot enqueues; SweepRetirements drains and runs the deleter", AUTO_TAG) {
    g_deleterCalls.store(0, std::memory_order_relaxed);
    Detail::SweepRetirements();  // start from an empty list.

    const DispatchSnapshot* snap = MakeHeapSnapshot(1);
    Detail::RetireSnapshot(snap, DeleteHeapSnapshot);

    // Before sweep: snapshot is owned by the pending list, deleter has not run.
    REQUIRE(Detail::PendingRetirementCount() == 1);
    REQUIRE(g_deleterCalls.load(std::memory_order_relaxed) == 0);

    Detail::SweepRetirements();

    // After sweep: pending list is drained and the deleter ran exactly once.
    REQUIRE(Detail::PendingRetirementCount() == 0);
    REQUIRE(g_deleterCalls.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("Sweep on an empty pending list is a no-op", AUTO_TAG) {
    g_deleterCalls.store(0, std::memory_order_relaxed);
    Detail::SweepRetirements();  // drain any residue from earlier tests.

    REQUIRE(Detail::PendingRetirementCount() == 0);
    Detail::SweepRetirements();  // sweep when there is nothing to do.
    REQUIRE(Detail::PendingRetirementCount() == 0);
    REQUIRE(g_deleterCalls.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("RetireSnapshot short-circuits on nullptr inputs", AUTO_TAG) {
    g_deleterCalls.store(0, std::memory_order_relaxed);
    Detail::SweepRetirements();

    // Registrars publishing an initial snapshot have no prior pointer to retire; the no-op path
    // must keep the queue empty so the next sweep stays cheap.
    Detail::RetireSnapshot(nullptr, DeleteHeapSnapshot);
    REQUIRE(Detail::PendingRetirementCount() == 0);

    // A null deleter is a programmer error but we tolerate it: skip the enqueue rather than crash
    // a release build inside the registrar's hot publish path.
    const DispatchSnapshot* snap = MakeHeapSnapshot(2);
    Detail::RetireSnapshot(snap, nullptr);
    REQUIRE(Detail::PendingRetirementCount() == 0);

    // Clean up the leaked-on-purpose snapshot since the null-deleter path never enqueued it.
    DeleteHeapSnapshot(snap);
    REQUIRE(g_deleterCalls.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("Multiple retires drain in one sweep", AUTO_TAG) {
    g_deleterCalls.store(0, std::memory_order_relaxed);
    Detail::SweepRetirements();

    Detail::RetireSnapshot(MakeHeapSnapshot(3), DeleteHeapSnapshot);
    Detail::RetireSnapshot(MakeHeapSnapshot(4), DeleteHeapSnapshot);
    Detail::RetireSnapshot(MakeHeapSnapshot(5), DeleteHeapSnapshot);
    REQUIRE(Detail::PendingRetirementCount() == 3);

    Detail::SweepRetirements();
    REQUIRE(Detail::PendingRetirementCount() == 0);
    REQUIRE(g_deleterCalls.load(std::memory_order_relaxed) == 3);
}

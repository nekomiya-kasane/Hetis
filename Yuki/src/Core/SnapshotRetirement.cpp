/**
 * @file SnapshotRetirement.cpp
 * @brief Out-of-line definitions for the dispatch-snapshot retirement scaffolding declared in
 *        @ref Yuki/Core/MetaClass.h.
 *
 * The retirement list is one process-wide queue of @c (snapshot, deleter) pairs. Registrars enqueue
 * the old pointer after publishing a fresh one; the next sweep — driven by @c Registry::Install on
 * any metaclass — drains the queue and runs each deleter. See spec §2.3 for the epoch story.
 *
 * The mutex guards a very short critical section: a @c push_back on enqueue, a @c swap on sweep,
 * and a @c size read on the diagnostic counter. Contention is bounded by the number of registrars
 * that race on @c Install at once (rare in steady state — startup-heavy in practice).
 */
#include <Yuki/Core/MetaClass.h>

#include <mutex>
#include <vector>

namespace Yuki::Detail {

namespace {

    /// @brief One row of the pending-reclaim list — snapshot plus the deleter that frees it.
    struct PendingEntry {
        const DispatchSnapshot* snap;
        SnapshotDeleter         del;
    };

    /// @brief Process-wide mutex guarding the pending list. Function-static for trivial
    ///        construction ordering — first touch initialises, no SIOF risk.
    std::mutex& RetirementMutex() noexcept {
        static std::mutex m;
        return m;
    }

    /// @brief The pending list itself. Same function-static rationale as @ref RetirementMutex.
    std::vector<PendingEntry>& PendingList() noexcept {
        static std::vector<PendingEntry> v;
        return v;
    }

} // namespace

void RetireSnapshot(const DispatchSnapshot* s, SnapshotDeleter d) noexcept {
    if (s == nullptr || d == nullptr) {
        return;
    }
    std::lock_guard guard{RetirementMutex()};
    PendingList().push_back({s, d});
}

void SweepRetirements() noexcept {
    std::vector<PendingEntry> drained;
    {
        std::lock_guard guard{RetirementMutex()};
        drained.swap(PendingList());
    }
    for (const auto& e : drained) {
        e.del(e.snap);
    }
}

std::size_t PendingRetirementCount() noexcept {
    std::lock_guard guard{RetirementMutex()};
    return PendingList().size();
}

} // namespace Yuki::Detail

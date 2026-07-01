/**
 * @file MergedDispatch.h
 * @brief D15 L2 — flattened, iid-sorted dispatch snapshot + binary-search lookup.
 *
 * The L2 read path of the four-level Query cache. @ref MergedDispatchSnapshot is published
 * by Task 18's `Registry::Install<T>` after flattening the class + every inherited base
 * (D16). Lookup is a branch-light binary search returning a pointer into the snapshot's
 * rodata-resident @ref DispatchEntry array, or @c nullptr on miss.
 *
 * Important-wins (D7.3) is resolved here, not at the call site: when two entries share an
 * iid (an Extension over a base impl, for instance), the Important entry is returned even
 * if its insertion order would otherwise lose. The binary search lands on *any* matching
 * entry; the function then linearly scans the equal-iid run for an Important entry.
 *
 * @ingroup Core
 */
#pragma once

#include <Yuki/Core/DispatchEntry.h>

#include <cstddef>

namespace Yuki {

    /// @brief Iid-sorted, flattened dispatch table for a class + its inherited bases (D16).
    struct MergedDispatchSnapshot {
        std::size_t          count{0};
        const DispatchEntry* entries{nullptr};
    };

    /**
     * @brief Binary-search a @ref MergedDispatchSnapshot for @p iid.
     *
     * Returns a pointer to the matching entry, or @c nullptr if not found. Resolves
     * Important-wins (D7.3) by linearly scanning the equal-iid run; the first Important
     * entry beats every non-Important entry sharing the same iid.
     *
     * @param snap Snapshot to search; may be @c nullptr or empty (returns @c nullptr).
     * @param iid  Interface identifier to look up.
     */
    [[nodiscard]] const DispatchEntry* LookupMergedDispatch(const MergedDispatchSnapshot* snap, Iid iid) noexcept;

} // namespace Yuki

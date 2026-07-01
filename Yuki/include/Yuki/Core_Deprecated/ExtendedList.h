/**
 * @file ExtendedList.h
 * @brief D15 L3 / D16 — reverse-edge snapshots + invalidation broadcast.
 *
 * @ref ExtendedListSnapshot is the published reverse edge of @c Anno::Extends — for a
 * nucleus N, the list of every downstream class's @ref MetaLinks whose closure includes N.
 * @ref ImplementedListSnapshot mirrors the shape for @c Anno::Implements. Both are
 * rodata-resident once published by @c Registry::Install<T> (Task 18); the L3 invalidation
 * kernel below walks the extendedBy list and bumps every downstream's cacheEpoch.
 *
 * @ingroup Core
 */
#pragma once

#include <cstddef>

namespace Yuki {

    struct MetaLinks;

    /// @brief Downstream MetaLinks that include this nucleus in their closure (D16).
    struct ExtendedListSnapshot {
        std::size_t              count{0};
        const MetaLinks* const*  downstreams{nullptr};
    };

    /// @brief Provider MetaLinks for a given interface — the implementedBy reverse edge.
    struct ImplementedListSnapshot {
        std::size_t              count{0};
        const MetaLinks* const*  providers{nullptr};
    };

    /// @brief Walk @p snap and call @c BumpCacheEpoch() on each downstream MetaLinks.
    ///
    /// No-op when @p snap is @c nullptr or has @c count == 0. The const-qualification on
    /// the snapshot is honest at the read layer — the kernel @c const_cast s downstream
    /// pointers to call @c BumpCacheEpoch (a non-const member), because the cacheEpoch
    /// store is logically a mutation on the link layer, not on the snapshot.
    void BroadcastInvalidation(const ExtendedListSnapshot* snap) noexcept;

} // namespace Yuki

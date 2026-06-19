/**
 * @file Introspection.h
 * @brief T21 — type-erased introspection over @c RootObject* (spec §2).
 *
 * Five free functions that read the live @c mergedDispatch snapshot of a node's class:
 *
 *   - @ref IidsOf         — list of provided interface IIDs (iid-sorted).
 *   - @ref Provides       — bool: does this closure provide @p iid?
 *   - @ref ProviderClass  — which class registered the provider for @p iid?
 *   - @ref RoleOf         — Yuki role of that provider class.
 *   - @ref TypeOf         — class identity of @p node itself (=@c node->MetaDyn().core).
 *
 * Closure view, not static view: an extension installed after the read-guard takes its
 * snapshot is NOT visible to that call (§2 rule 1). Read-only: no mutation of MetaLinks
 * (§2 rule 2). No vtable bloat: free functions, not methods (§2 rule 3).
 *
 * Important tiebreak (D7.3): when two providers register the same iid and one is marked
 * Important, the Important one wins @c mergedDispatch placement at Install time;
 * @ref ProviderClass and @ref RoleOf therefore return the Important provider. The
 * displaced provider is invisible to introspection (it never reached @c mergedDispatch).
 *
 * Null-tolerance: every function handles @c node == nullptr and nodes whose MetaDynamic
 * is the @c RootObject base sentinel @c {nullptr,nullptr} (test probes, abstract
 * interface bases without Y_OBJECT). On those inputs they return empty / false / nullptr
 * / @c ClassType::None.
 *
 * @ingroup Core
 */
#pragma once

#include <Yuki/Core/Identity.h>

#include <span>

namespace Yuki {

    struct RootObject;
    struct MetaCore;
    struct DispatchEntry;

    /// @brief List the IIDs in @p node's closure-level mergedDispatch (iid-sorted, snapshot).
    ///        Returns an empty span for null nodes or nodes without an installed class.
    [[nodiscard]] std::span<const DispatchEntry> IidsOf(const RootObject* node) noexcept;

    /// @brief True iff @p node's closure provides @p iid (binary-search on mergedDispatch).
    [[nodiscard]] bool Provides(const RootObject* node, Iid iid) noexcept;

    /// @brief Class-level identity of the provider chosen for @p iid (Important-wins).
    ///        Returns @c nullptr if the closure does not provide @p iid.
    [[nodiscard]] const MetaCore* ProviderClass(const RootObject* node, Iid iid) noexcept;

    /// @brief Yuki role of the provider chosen for @p iid. Returns @c ClassType::None if
    ///        the closure does not provide @p iid or the provider has no role.
    [[nodiscard]] ClassType RoleOf(const RootObject* node, Iid iid) noexcept;

    /// @brief Class identity of @p node itself — equal to @c node->MetaDyn().core. Returns
    ///        @c nullptr for null nodes or nodes without an installed Y_OBJECT class.
    [[nodiscard]] const MetaCore* TypeOf(const RootObject* node) noexcept;

}  // namespace Yuki

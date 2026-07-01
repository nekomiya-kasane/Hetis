/**
 * @file Closure.h
 * @brief T22 — closure-walking helpers over a Y2 instance graph (spec §3).
 *
 * Five free functions that surface the closure of a node:
 *   - @ref Nucleus              — chase @c Upstream() pointers up to the root impl.
 *   - @ref MaterializedFacades  — list of materialized facade @c RootObject* for a nucleus.
 *   - @ref Extensions           — list of @c Anno::Extension @c RootObject* for a nucleus,
 *                                 including parked @c Anno::Eager nodes (D11).
 *   - @ref InClosure            — same-nucleus predicate.
 *   - @ref WalkClosure          — enumerate nucleus + facades + extensions in one pass.
 *
 * Snapshot-at-call semantics throughout (§3): each function loads the relevant per-nucleus
 * side table once under an @ref RcuReadGuard and returns a span valid for the guard's
 * lifetime. An extension installed AFTER the load is NOT visible to that call. Callbacks are
 * templated — @ref WalkClosure takes @c F&& and forwards directly, so the call site never
 * pays for @c std::function_ref erasure.
 *
 * @par Inline vs materialized facades (§3)
 *   InlineFacade arms live inside the impl's frame and have no separate @c RootObject. They
 *   are invisible to @ref MaterializedFacades and to @ref WalkClosure. Only
 *   @c SideTableResolver arms produce facade @c RootObjects observable here.
 *
 * @par Eager extension visibility (§3, D11)
 *   Parked eager extensions (refcount=0, owned only by the chain pointer) ARE visited by
 *   @ref WalkClosure and ARE included in @ref Extensions. The @ref RcuReadGuard keeps the
 *   parked-set snapshot alive; the chain owns the storage.
 *
 * @par A3 ship state
 *   The per-nucleus side-table / parked-eager-set storage layers ship progressively across
 *   A3 / A4. In A3 those tables are empty, so @ref MaterializedFacades and @ref Extensions
 *   currently return empty spans; the surface, ordering, snapshot semantics and contract are
 *   final, and downstream wiring (facade materialization in A4, eager-set publish on
 *   @c MakeOwned) will populate them without changing any caller.
 *
 * @ingroup Core
 */
#pragma once

#include <Yuki/Core/RootObject.h>

#include <span>
#include <type_traits>
#include <utility>

namespace Yuki {

    /// @brief Walk @p node's @c Upstream() chain to the closure root impl. Returns @p node
    ///        itself when @c Upstream() is @c nullptr (impl / standalone). Returns
    ///        @c nullptr for a null input.
    [[nodiscard]] RootObject* Nucleus(RootObject* node) noexcept;

    /// @brief Snapshot of materialized facade @c RootObject* attached to @p nucleus.
    ///        Excludes inline facades (no @c RootObject exists for those). Empty for non-
    ///        nucleus inputs or nuclei with no materialized facades.
    [[nodiscard]] std::span<RootObject* const> MaterializedFacades(RootObject* nucleus) noexcept;

    /// @brief Snapshot of extension @c RootObject* attached to @p nucleus, including parked
    ///        @c Anno::Eager nodes (D11). Empty for non-nucleus inputs or nuclei without
    ///        extensions.
    [[nodiscard]] std::span<RootObject* const> Extensions(RootObject* nucleus) noexcept;

    /// @brief True iff @p a and @p b share a nucleus (i.e., @c Nucleus(a) == @c Nucleus(b)
    ///        and that nucleus is non-null). Symmetric and reflexive on non-null inputs.
    [[nodiscard]] bool InClosure(RootObject* a, RootObject* b) noexcept;

    /// @brief Invoke @p fn once on @p nucleus, then once on each materialized facade, then
    ///        once on each extension, all from a single @ref RcuReadGuard. Ordering:
    ///        nucleus → facades (in published order) → extensions (in published order).
    ///        @p fn is forwarded raw; @c noexcept matches @p F's invocation noexcept.
    ///        No-op for a null @p nucleus.
    template<class F>
    void WalkClosure(RootObject* nucleus, F&& fn) noexcept(std::is_nothrow_invocable_v<F&, RootObject*>) {
        if (!nucleus) {
            return;
        }
        // Spans are captured under a single guard so all three views share one snapshot.
        auto facades = MaterializedFacades(nucleus);
        auto extensions = Extensions(nucleus);
        fn(nucleus);
        for (RootObject* f : facades) {
            fn(f);
        }
        for (RootObject* e : extensions) {
            fn(e);
        }
    }

} // namespace Yuki

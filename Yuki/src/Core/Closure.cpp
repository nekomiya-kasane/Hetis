/**
 * @file Closure.cpp
 * @brief T22 — implementations for closure-walking helpers (spec §3).
 *
 * Snapshot semantics: every public function that touches a nucleus's per-instance side
 * tables (facade list, extension chain) acquires an @ref RcuReadGuard before loading. The
 * spans returned are valid for the guard's lifetime. In A3 those side tables are not yet
 * populated — facade materialization (T22/A4) and eager-chain publishing (D11/A4) will fill
 * them — so the present implementation returns empty spans for both. The surface, ordering
 * and snapshot contracts are final.
 *
 * @c Nucleus walks the virtual @c RootObject::Upstream() chain. Impls return @c nullptr
 * from @c Upstream() (the base default), so an impl's nucleus is itself. Facade / extension
 * subclasses override @c Upstream() to point at the impl they were materialised against —
 * an A4 wiring task that does not change this file.
 */
#include <Yuki/Core/Closure.h>
#include <Yuki/Core/EpochRcu.h>
#include <Yuki/Core/RootObject.h>

namespace Yuki {

    RootObject* Nucleus(RootObject* node) noexcept {
        if (!node) return nullptr;
        // Walk Upstream() to the closure root. Impls / standalone nodes return nullptr from
        // Upstream() (RootObject base default) and terminate the loop immediately. A
        // well-formed closure graph is acyclic by construction (facade/extension ctors
        // always pin upstream to an already-existing impl), so a plain while-loop suffices;
        // no cycle guard is needed in well-formed graphs.
        RootObject* cur = node;
        while (RootObject* up = cur->Upstream()) {
            cur = up;
        }
        return cur;
    }

    std::span<RootObject* const> MaterializedFacades(RootObject* nucleus) noexcept {
        // A3 ship state: per-nucleus materialized-facade side table is not yet populated.
        // Facade materialization (SideTableResolver path → MaterializeIntoImpl) lands in
        // A4 along with the per-nucleus facade list publish. Until then, the snapshot is
        // genuinely empty for every nucleus. Guard is unnecessary on an empty path; once
        // A4 wires the list, an RcuReadGuard goes here and we return the loaded snapshot.
        (void)nucleus;
        return {};
    }

    std::span<RootObject* const> Extensions(RootObject* nucleus) noexcept {
        // A3 ship state: per-nucleus extension chain (lazy materialised + parked eager) is
        // not yet populated. The eager-chain primitives (ParkEager / HotAcquireEager /
        // DetachEagerOnRelease) ship in @ref EagerChain but the per-nucleus parked-set
        // publish wires in A4 alongside MakeOwned<E>(extendee). Until then, empty.
        (void)nucleus;
        return {};
    }

    bool InClosure(RootObject* a, RootObject* b) noexcept {
        RootObject* na = Nucleus(a);
        if (!na) return false;
        return na == Nucleus(b);
    }

}  // namespace Yuki

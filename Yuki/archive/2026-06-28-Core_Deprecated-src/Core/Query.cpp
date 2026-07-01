/**
 * @file Query.cpp
 * @brief D15 L2 kernel — folds L1 fingerprint probe + L2 binary search + L3 epoch gate.
 *
 * Companion translation unit for @ref Query.h. The user-facing @c Query<I,T>(node) template
 * stays in the header so the L0 consteval shortcut can fold to a constant; only the runtime
 * kernel is type-erased here.
 */
#include <Yuki/Core/Query.h>

#include <Yuki/Core/Closure.h>
#include <Yuki/Core/MetaCore.h>
#include <Yuki/Core/MetaDynamic.h>

namespace Yuki {

    const DispatchEntry* QueryDynamicRaw(MetaLinks* links, Iid iid) noexcept {
        if (!links) {
            return nullptr;
        }
        const std::uint64_t epoch = links->cacheEpoch.load(std::memory_order_acquire);

        // L1 probe.
        if (auto hit = Probe(links->l1, iid, epoch)) {
            return *hit; // may be nullptr — a cached negative hit.
        }

        // L2 binary search.
        const MergedDispatchSnapshot* merged = links->mergedDispatch.load(std::memory_order_acquire);
        const DispatchEntry* e = LookupMergedDispatch(merged, iid);

        // Publish the (possibly null) result back into L1 with the witnessed epoch.
        Publish(links->l1, iid, e, epoch);
        return e;
    }

    namespace Detail {

        /// @brief Apply a resolved dispatch arm against @p nucleus, returning a +1-owned facet
        ///        (Adopt-shaped) or null. Mirrors the per-arm production in @ref Query but
        ///        type-erased: the InlineFacade pointer is adjusted via @c armOffset rather than a
        ///        statically-typed @c static_cast.
        RootObject* ApplyArmOwned(RootObject* nucleus, const DispatchEntry* e) noexcept {
            switch (e->kind) {
            case DispatchKind::InlineFacade: {
                // The interface subobject lives inside the nucleus frame at armOffset (0 for the
                // single-base chains this object model uses). It shares the nucleus refcount, so
                // Acquire bumps the one metaword.
                auto* facet = reinterpret_cast<RootObject*>(reinterpret_cast<char*>(nucleus) + e->armOffset);
                Acquire(facet);
                return facet;
            }
            case DispatchKind::SideTableResolver: {
                using ResolverFn = RootObject* (*)(RootObject*);
                auto fn = reinterpret_cast<ResolverFn>(e->arm);
                if (!fn) {
                    return nullptr;
                }
                return fn(nucleus); // resolver returns a freshly materialised facade, +1 owned.
            }
            case DispatchKind::CodeExtensionSingleton: {
                using SingletonFn = RootObject* (*)();
                auto fn = reinterpret_cast<SingletonFn>(e->arm);
                if (!fn) {
                    return nullptr;
                }
                RootObject* s = fn();
                if (!s) {
                    return nullptr;
                }
                // External-lifetime singleton — Acquire is a no-op, but keep the +1-owned contract
                // uniform so the caller's matching Release (also a no-op) stays balanced.
                Acquire(s);
                return s;
            }
            }
            return nullptr;
        }

        RootObject* CrossCastOwned(RootObject* from, Iid target) noexcept {
            if (!from) {
                return nullptr;
            }
            RootObject* nucleus = Nucleus(from);
            if (!nucleus) {
                return nullptr;
            }

            // Scope the snapshot reads (dispatch tables + closure side tables) under one guard so the
            // entry / nodes we touch stay live for the duration of the resolution.
            RcuReadGuard guard;

            // 1) Interface target: resolve through the nucleus's merged dispatch (InlineFacade /
            //    SideTableResolver / CodeExtensionSingleton). This is the QueryInterface path.
            if (const DispatchEntry* e = QueryDynamicRaw(nucleus->MetaDyn().links, target)) {
                return ApplyArmOwned(nucleus, e);
            }

            // 2) Concrete-class target (e.g. a down-cast onto the impl itself): impls do not register
            //    their own IID as an interface, so the dispatch miss above is expected. Match on
            //    MetaClass identity across the closure instead — the nucleus first, then its
            //    materialized facades / extensions (empty until A4 populates the side tables). The
            //    first node whose MetaCore::iid equals the target IS-A target by construction.
            RootObject* found = nullptr;
            WalkClosure(nucleus, [&](RootObject* node) noexcept {
                if (found) {
                    return;
                }
                const MetaDynamic& md = node->MetaDyn();
                if (md.core && md.core->iid == target) {
                    found = node;
                }
            });
            if (found) {
                Acquire(found);
            }
            return found;
        }

    } // namespace Detail

} // namespace Yuki

/**
 * @file Query.h
 * @brief D14 / D15 — Query<I>(node) static face + the QueryDynamicRaw L2 kernel.
 *
 * The user-facing entry point of the four-level cache. The static face is templated on
 * the target interface so the L0 consteval shortcut (@ref Detail::IsBoaProvider) can
 * resolve against @c kImplementsArr<Impl> whenever the impl type is statically known; on
 * a miss, the call falls through to the runtime kernel that probes L1, then L2, then
 * publishes the result (positive or negative) back into L1.
 *
 * Per-arm result production (D14):
 *  - @c InlineFacade           — @p node is itself an @c I subobject (D5); static_cast wraps it.
 *  - @c SideTableResolver      — stubbed in A2; resolver invocation lands in A3.
 *  - @c CodeExtensionSingleton — stubbed in A2; HotAcquireEager wiring lands in A3.
 *
 * @ingroup Core
 */
#pragma once

#include <Yuki/Core/ComPtr.h>
#include <Yuki/Core/DispatchEntry.h>
#include <Yuki/Core/EpochRcu.h>
#include <Yuki/Core/FingerprintCache.h>
#include <Yuki/Core/Identity.h>
#include <Yuki/Core/MergedDispatch.h>
#include <Yuki/Core/MetaLinks.h>
#include <Yuki/Core/QueryL0.h>
#include <Yuki/Core/RootObject.h>

#include <type_traits>

namespace Yuki {

    /// @brief Runtime kernel: probe L1, then L2; publish L2 result into L1; return entry or null.
    [[nodiscard]] const DispatchEntry* QueryDynamicRaw(MetaLinks* links, Iid iid) noexcept;

    /**
     * @brief Resolve a @c ComPtr<I> for @p node by walking the four-level cache.
     *
     * Templated on the static type @c T of @p node so L0 can probe @c kImplementsArr<T>
     * and @c Meta().links is reachable without a virtual hook. (The type-erased
     * @c Query<I>(RootObject*) entry point — needed for true closure-walking from a
     * generic node — lands in A3 once Y_OBJECT exposes a virtual MetaLinks accessor.)
     */
    template<class I, class T>
    [[nodiscard]] ComPtr<I> Query(T* node) noexcept {
        if (!node) return {};
        if constexpr (Detail::IsBoaProvider<T, I>()) {
            // L0 fast path: I is statically known to be a base subobject of T.
            return ComPtr<I>(static_cast<I*>(node));
        } else {
            // T23: scope an RCU read guard around the L1/L2 probe + arm invocation so the
            // snapshot pointers we read remain live for the duration of the call. The guard
            // is cheap (one relaxed-load of @c tlSlot->epoch on the nested-call fast path) and
            // gates @c TryReclaim from freeing the snapshots backing @p e.
            RcuReadGuard g;
            MetaLinks* links = T::Meta().links;
            const DispatchEntry* e = QueryDynamicRaw(links, IidOf<I>());
            if (!e) return {};
            switch (e->kind) {
                case DispatchKind::InlineFacade:
                    // D5: an InlineFacade entry guarantees @c node is an @c I subobject. The
                    // compile-time guard keeps the template well-formed for unrelated (T, I)
                    // pairs whose merged dispatch correctly returns null entries — the runtime
                    // branch is unreachable in that case, but the body must still type-check.
                    if constexpr (std::is_base_of_v<I, T>) {
                        return ComPtr<I>(static_cast<I*>(node));
                    } else {
                        return {};
                    }
                case DispatchKind::SideTableResolver: {
                    // T23 §6.2: invoke the registered resolver. Resolver returns +1 reference
                    // to a materialised heap facade, OR @c nullptr if it chose not to
                    // materialise (§6.4). @c ComPtr::Adopt consumes the +1; the empty result
                    // is shape-equivalent to Provides==false.
                    using ResolverFn = RootObject* (*)(RootObject*);
                    auto fn = reinterpret_cast<ResolverFn>(e->arm);
                    if (!fn) return {};
                    RootObject* mat = fn(static_cast<RootObject*>(node));
                    if (!mat) return {};
                    return ComPtr<I>::Adopt(static_cast<I*>(mat));
                }
                case DispatchKind::CodeExtensionSingleton: {
                    // T23 §6.2: invoke the registered singleton thunk. Returns a borrowed ptr
                    // to an external-lifetime singleton (payload carries external sentinel so
                    // @c Acquire is a no-op).
                    using SingletonFn = RootObject* (*)();
                    auto fn = reinterpret_cast<SingletonFn>(e->arm);
                    if (!fn) return {};
                    RootObject* s = fn();
                    if (!s) return {};
                    return ComPtr<I>(static_cast<I*>(s));
                }
            }
            return {};
        }
    }

} // namespace Yuki

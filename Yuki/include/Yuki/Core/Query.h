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
                case DispatchKind::SideTableResolver:
                    // A3 plumbs the resolver invocation; A2 stubs to null.
                    return {};
                case DispatchKind::CodeExtensionSingleton:
                    // A3 routes through HotAcquireEager; A2 stubs to null.
                    return {};
            }
            return {};
        }
    }

} // namespace Yuki

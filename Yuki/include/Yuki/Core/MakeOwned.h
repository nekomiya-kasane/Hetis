#pragma once
/**
 * @file MakeOwned.h
 * @brief D12 canonical owning-construction factory for RootObject-derived T.
 *
 * Single-allocation `new` + `ComPtr::Adopt`. The companion to `ComPtr<T>` (T10): where
 * ComPtr defines the ownership semantics, MakeOwned is the canonical entry point for
 * constructing a freshly-owned instance without ever touching a raw pointer on the call site.
 *
 * @ingroup Core
 */
#include <Yuki/Core/ComPtr.h>
#include <utility>

namespace Yuki {

    /**
     * @brief Allocate a T with `new`, then wrap it in `ComPtr<T>` via `Adopt` (consuming the +1
     *        the constructor produced).
     *
     * @note Why Adopt, not explicit ComPtr(T*): `RootObject(role, arm, external=false)` starts
     *       the refcount at 1 — that is the +1 we are taking ownership of. `ComPtr<T>::Adopt`
     *       consumes it without bumping. Using `explicit ComPtr(T*)` would bump again, leaving
     *       refcount=2 and a guaranteed leak. The ComPtr design pairs Adopt with exactly this
     *       factory shape.
     *
     * @note T's constructor contract: T must derive from `RootObject` and forward to
     *       `RootObject(role, arm, external=false)`. If `external=true` is passed instead, the
     *       refcount stays at the external sentinel and Adopt's ownership is meaningless — the
     *       object will leak when the ComPtr Releases because `TryDecrement` no-ops on external
     *       lifetime. MakeOwned cannot enforce this at compile time; the contract must be upheld
     *       by T's author.
     *
     * @note [[nodiscard]] rationale: discarding the returned ComPtr immediately calls its dtor,
     *       releasing the only +1 and deleting the brand-new T. The attribute prevents this
     *       silent-leak-of-construction pattern where `MakeOwned<T>(...)` is called for its
     *       side-effects and the result is dropped.
     *
     * @tparam T   A type deriving from `RootObject` whose constructor starts refcount at 1
     *             (i.e., `external=false`).
     * @tparam Args Constructor argument types, deduced.
     * @return A `ComPtr<T>` owning the sole +1 on the freshly constructed T.
     */
    template<class T, class... Args>
    [[nodiscard]] ComPtr<T> MakeOwned(Args&&... args) {
        return ComPtr<T>::Adopt(new T(std::forward<Args>(args)...));
    }

} // namespace Yuki

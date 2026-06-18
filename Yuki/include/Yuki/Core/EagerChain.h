/**
 * @file EagerChain.h
 * @brief D11 — eager extension chain ownership + deferred Acquire.
 *
 * Eager extensions are constructed alongside their nucleus but cannot Acquire the
 * extendee at ctor time without breaking the hierarchical invariant: the nucleus's
 * own ctor has not finished, the user's MakeOwned has not adopted the +1 yet, and a
 * cycle would emerge. Y2's resolution: park the extension in the nucleus's eager set
 * with @c refcount = 0. The chain owns parked extensions by raw pointer.
 *
 * The first time user code obtains a @c ComPtr to a parked eager (via Query),
 * @ref HotAcquireEager bumps the extension's refcount to 1 and Acquires the extendee.
 * The user-side last Release lands the extension back at refcount 0;
 * @ref DetachEagerOnRelease un-Acquires the extendee and re-parks. Final teardown of
 * parked extensions happens in the nucleus dtor (A3).
 *
 * @ingroup Core
 */
#pragma once

#include <Yuki/Core/RootObject.h>

#include <cstddef>

namespace Yuki {

    /// @brief Published list of parked eager extensions for a nucleus.
    struct EagerSetSnapshot {
        std::size_t        count{0};
        RootObject* const* parked{nullptr};
    };

    /// @brief Force @p ext's TaggedPayload refcount down to 0 (parked state).
    ///
    /// Called by the eager-chain installer after constructing the extension. @p ext must
    /// have @c refcount == 1 on entry (the post-ctor count from RootObject(external=false)).
    void ParkEager(RootObject* ext) noexcept;

    /// @brief Bump a parked eager's refcount to 1 and Acquire @p extendee.
    ///
    /// Called the first time Query hands out a @c ComPtr to a parked eager.
    /// Precondition: @p ext is currently parked (refcount == 0). On return:
    ///  - @p ext has refcount == 1
    ///  - @p extendee has been Acquired by exactly one ref on behalf of @p ext.
    void HotAcquireEager(RootObject* ext, RootObject* extendee) noexcept;

    /// @brief Re-park @p ext and Release @p extendee.
    ///
    /// Called by the user-side ComPtr last-Release path when an eager's refcount
    /// transitions to 0. Does NOT delete the extension — the chain still owns it.
    void DetachEagerOnRelease(RootObject* ext, RootObject* extendee) noexcept;

} // namespace Yuki

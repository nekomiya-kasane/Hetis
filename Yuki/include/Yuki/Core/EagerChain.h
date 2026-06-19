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
    ///
    /// @pre @p ext MUST be a non-external-lifetime @ref RootObject (constructed with
    ///      @c external=false). Parking an externally-managed RootObject would clobber the
    ///      @c kExternalSentinel refcount window and corrupt external-lifetime tracking;
    ///      @c kDebug builds assert.
    void ParkEager(RootObject* ext) noexcept;

    /// @brief Bump a parked eager's refcount to 1 and Acquire @p extendee.
    ///
    /// Called the first time Query hands out a @c ComPtr to a parked eager.
    /// Precondition: @p ext is currently parked (refcount == 0). On return:
    ///  - @p ext has refcount == 1
    ///  - @p extendee has been Acquired by exactly one ref on behalf of @p ext.
    ///
    /// @warning The @c refcount == 0 precondition is asserted only in @c kDebug builds; in
    ///          release builds a violation is silent and double-Acquires @p extendee on the
    ///          next park/hot cycle, leaking an @p extendee ref.
    void HotAcquireEager(RootObject* ext, RootObject* extendee) noexcept;

    /// @brief Re-park @p ext and Release @p extendee.
    ///
    /// Called by the user-side ComPtr last-Release path when an eager's refcount
    /// transitions to 0. Does NOT delete the extension — the chain still owns it.
    ///
    /// @warning The @c refcount == 1 precondition is asserted only in @c kDebug builds; in
    ///          release builds a violation under-counts @p extendee refs (a Release with no
    ///          paired HotAcquire) and may prematurely free @p extendee.
    void DetachEagerOnRelease(RootObject* ext, RootObject* extendee) noexcept;

    /// @brief T23 §7 — final teardown walker. Called from @c ~RootObject when the dying
    ///        instance is an @c Implementation: delete every parked eager extension owned by
    ///        @p nucleus's @c MetaLinks::eagerSet, clear the slot, retire the snapshot.
    ///
    /// **D11 invariant (§7.3):** live (refcount > 0) eager extensions can never reach this
    /// path. @p ext->refcount >= @p extendee->refcount whenever @p extendee > 0; a live
    /// extension holds the deferred @c Acquire(extendee), so the nucleus cannot reach 0
    /// while any live ext exists. Only parked ones (refcount == 0) survive to here. The
    /// walker @c kDebug-asserts that invariant on each parked entry.
    ///
    /// **Locking:** takes @c links->writerMu so a concurrent @c Install or
    /// @c RegisterSideTable on a base flattening through this metaclass (D16) serialises
    /// cleanly. The dying nucleus is not visible to in-flight @c Query (its refcount just
    /// hit zero, so no @c ComPtr can race) — @c RcuReadGuard protects the snapshot, not the
    /// nucleus itself.
    ///
    /// No-op for null @p nucleus, null @c links, or null @c eagerSet snapshot.
    void DeleteParkedEagers(RootObject* nucleus) noexcept;

    namespace Detail {
        /// @brief Allocate an @ref EagerSetSnapshot with @p count uninitialised slots in a
        ///        single block (header + trailing @c RootObject* array).
        ///
        /// The returned snapshot's @c parked array points at the trailing storage. Both the
        /// header and the array are freed by the single deleter that the dtor walker passes
        /// to @ref RetireSnapshot, so callers do NOT free the array separately. Used by the
        /// A4 eager-set publisher and by the A3 NucleusDtorWalker tests.
        [[nodiscard]] EagerSetSnapshot* AllocEagerSetSnapshot(std::size_t count) noexcept;
    }

} // namespace Yuki

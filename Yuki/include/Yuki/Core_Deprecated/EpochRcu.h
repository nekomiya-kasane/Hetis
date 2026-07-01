/**
 * @file EpochRcu.h
 * @brief T23 — per-thread epoch-RCU reader registration and deferred reclamation.
 *
 * Replaces A2's unbounded @c RetiredPool (a static @c vector<unique_ptr<...>> that grew
 * monotonically) with a bounded reclamation scheme. Three pieces (D16 / spec §4):
 *
 *  - @ref RcuReadGuard : scoped reader; publishes the current global epoch into the
 *                        calling thread's slot on construction, clears on destruction.
 *                        Nested guards are no-ops; only the outermost guard mutates the
 *                        slot. Non-copyable, non-movable.
 *  - @ref RetireSnapshot : writers call this after a release-store of a new snapshot
 *                          pointer. The retiree is appended to a global queue stamped
 *                          with @c gGlobalEpoch.fetch_add(1) at retirement time.
 *  - @ref TryReclaim : computes @c safe = min epoch across active readers and frees every
 *                      retiree whose stamp is strictly less than @c safe.
 *
 * Correctness sketch (§4.5):
 *  A reader publishing @c epoch=E may hold pointers retired at epochs @c ≤E-1 (because
 *  those release-stores happen-before the reader's acquire-load), and CANNOT hold a
 *  pointer retired at epoch @c ≥E. A writer at retire stamp @c S used
 *  @c gGlobalEpoch.fetch_add(1) so any reader that started before that fetch sees
 *  @c epoch ≤ S-1 and is dangerous; any reader started after sees @c epoch ≥ S and
 *  already observed the new snapshot via its acquire-load. The reclaimer uses
 *  @c safe = min(reader.epoch) so dangerous readers block reclamation of their epoch's
 *  retirees.
 *
 * @ingroup Core
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace Yuki {

    /// @brief Scoped reader registration. Outermost guard publishes the global epoch into
    ///        the calling thread's slot; nested guards are no-ops. The slot is claimed
    ///        lazily on first use (CAS into a fixed 64-entry table) and cached in a
    ///        thread-local; release on dtor does not free the slot, only flips its epoch
    ///        back to 0 ("quiescent").
    class [[nodiscard]] RcuReadGuard {
      public:
        RcuReadGuard() noexcept;
        ~RcuReadGuard() noexcept;
        RcuReadGuard(const RcuReadGuard&)            = delete;
        RcuReadGuard& operator=(const RcuReadGuard&) = delete;
        RcuReadGuard(RcuReadGuard&&)                 = delete;
        RcuReadGuard& operator=(RcuReadGuard&&)      = delete;

      private:
        bool wasOuter_;
    };

    /// @brief Enqueue a pointer for deferred reclamation, stamped with a freshly bumped
    ///        global epoch. The writer MUST have published the new snapshot via a
    ///        release-store before calling this so the epoch fence orders correctly.
    ///        @p deleter must be a function suitable for calling on @p ptr.
    void RetireSnapshot(void* ptr, void (*deleter)(void*)) noexcept;

    /// @brief Walk the retire queue and free every retiree whose stamp is strictly less
    ///        than the minimum active reader epoch. Returns the number of items freed.
    ///        Cheap when the queue is empty; called opportunistically from every
    ///        @c Install<E> writer (§4.6 degenerate-case mitigation).
    std::size_t TryReclaim() noexcept;

}  // namespace Yuki

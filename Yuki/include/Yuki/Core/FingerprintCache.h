/**
 * @file FingerprintCache.h
 * @brief D15 L1 — per-MetaLinks 4-slot lock-free fingerprint ring.
 *
 * Sits inside @ref MetaLinks. Caches recent positive *and* negative Query lookups so
 * repeated "does this closure provide @c I?" probes never reach L2. Lock-free reader
 * with @c memory_order_acquire on each slot; writer publishes with @c memory_order_release
 * and a round-robin index incremented with @c memory_order_relaxed.
 *
 * Slot encoding:
 *  - both iid halves zero → empty slot, never a hit
 *  - matching iid, @c entry != nullptr → positive hit
 *  - matching iid, @c entry == nullptr → negative hit (we asked, the closure refused)
 *
 * Stale slots are evicted via the slot's @c epoch — the snapshot of
 * @ref MetaLinks::cacheEpoch at publish time. A probe with a different live epoch evicts
 * the slot (treated as a miss, not a hit). Task 17's @c BroadcastInvalidation bumps the
 * epoch on downstream classes after Install<E>; Task 18 bumps it on the installing class.
 *
 * @par Iid representation
 * Iid wraps a 128-bit @c Mashiro::Uuid; @c std::atomic<Iid> would need a
 * @c __atomic_load_16 / @c __atomic_store_16 libcall that the clang-p2996 toolchain's
 * libc++ does not provide for the @c x86_64-pc-windows-msvc target. The iid is therefore
 * split into two @c std::atomic<std::uint64_t> halves (natively lock-free on x64). The
 * Publish order is `epoch → entry → iidLo → iidHi`, all release; the Probe order is
 * `iidHi → iidLo → epoch → entry`, all acquire. A reader that sees a matching iidHi
 * synchronises-with the Publish's iidHi store and thereby observes the prior epoch,
 * entry, and iidLo stores from the same Publish call.
 *
 * This synchronises-with edge is only sufficient when Probe observes the *new* iidHi.
 * The abstract C++ model permits independent release stores from one thread to be observed
 * out of order by a reader (release ordering pins each store to *its own* atomic, not to
 * sibling atomics), so in principle a Probe could see a stale @c (iidHi, iidLo) pair from
 * a prior publisher AND the new @c (epoch, entry) from a concurrent in-flight Publish,
 * returning the new entry mislabeled as the prior iid. We rely on x86-64 TSO — the only
 * supported target, enforced by the @c static_assert below — where independent stores from
 * one thread are observed in program order by any reader, making the four release stores
 * of a single Publish visible as an atomic group and foreclosing the race. A future port
 * to a weaker target (ARM64 with relaxed reordering) would need either a seqlock retry on
 * iidHi mismatch or a wide-CAS-backed publish.
 *
 * @ingroup Core
 */
#pragma once

#include <Yuki/Core/Identity.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace Yuki {

    struct DispatchEntry;

    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "FingerprintCache split-halves design requires lock-free uint64 atomics.");
    static_assert(std::atomic<const DispatchEntry*>::is_always_lock_free,
                  "FingerprintCache slot.entry must be a lock-free atomic pointer.");

    /** @cond INTERNAL */
    namespace Detail {

        /// @brief Most-significant 64 bits of an @ref Iid's underlying 128-bit value.
        [[nodiscard]] inline constexpr std::uint64_t IidHi(Iid iid) noexcept {
            return static_cast<std::uint64_t>(iid.value.value >> 64);
        }

        /// @brief Least-significant 64 bits of an @ref Iid's underlying 128-bit value.
        [[nodiscard]] inline constexpr std::uint64_t IidLo(Iid iid) noexcept {
            return static_cast<std::uint64_t>(iid.value.value);
        }

    } // namespace Detail
    /** @endcond */

    /**
     * @brief One slot of the L1 fingerprint ring.
     *
     * Four independent atomics — no CAS over the tuple. See the @c Iid-representation
     * note in the file header for the publish/probe ordering rationale. The empty-slot
     * sentinel is `(iidHi == 0 && iidLo == 0)`; a real @ref Iid stamps an RFC-4122 v8
     * nibble, so a legitimate Iid is never all-zero (the synthetic test iids deliberately
     * skip @c Uuid{0,0}).
     */
    struct FingerprintSlot {
        std::atomic<std::uint64_t>         iidHi{0};
        std::atomic<std::uint64_t>         iidLo{0};
        std::atomic<const DispatchEntry*>  entry{nullptr};
        std::atomic<std::uint64_t>         epoch{0};
    };

    /**
     * @brief Four-slot round-robin lock-free fingerprint cache.
     *
     * Embedded inside @ref MetaLinks. @ref witnessEpoch snapshots the host
     * @c MetaLinks::cacheEpoch at the moment the ring was last refreshed (not consulted
     * by @ref Probe / @ref Publish below, which use the slot's own @c epoch — kept for
     * future whole-ring invalidation paths). @ref writeIdx is bumped relaxed; reordering
     * across writers is acceptable because each slot's atomics already carry the
     * happens-before edge readers need.
     */
    struct FingerprintCache {
        static constexpr std::size_t kSlots = 4;
        std::atomic<std::uint64_t> witnessEpoch{0};
        FingerprintSlot            slots[kSlots]{};
        std::atomic<std::uint32_t> writeIdx{0};
    };

    /**
     * @brief Probe the cache for @p iid against @p liveEpoch.
     *
     * @return @c nullopt on miss; an optional containing the cached entry pointer on hit
     *         (the inner pointer may itself be @c nullptr for a cached negative hit).
     *
     * @note Walks all four slots unconditionally — the array is one cache line on x64
     *       and branch-light scanning beats the branch misprediction of an early exit.
     */
    [[nodiscard]] inline std::optional<const DispatchEntry*> Probe(
            FingerprintCache& c, Iid iid, std::uint64_t liveEpoch) noexcept {
        const std::uint64_t qHi = Detail::IidHi(iid);
        const std::uint64_t qLo = Detail::IidLo(iid);
        for (auto& s : c.slots) {
            std::uint64_t sHi = s.iidHi.load(std::memory_order_acquire);
            if (sHi != qHi) continue;
            std::uint64_t sLo = s.iidLo.load(std::memory_order_acquire);
            if (sLo != qLo) continue;
            if (sHi == 0 && sLo == 0) continue;  // empty-slot sentinel
            std::uint64_t e = s.epoch.load(std::memory_order_acquire);
            if (e != liveEpoch) return std::nullopt;
            return s.entry.load(std::memory_order_acquire);
        }
        return std::nullopt;
    }

    /// @brief Publish a (iid, entry, epoch) triple to a round-robin slot.
    inline void Publish(FingerprintCache& c, Iid iid,
                        const DispatchEntry* entry, std::uint64_t epoch) noexcept {
        std::uint32_t idx = c.writeIdx.fetch_add(1, std::memory_order_relaxed)
                          % FingerprintCache::kSlots;
        FingerprintSlot& s = c.slots[idx];
        s.epoch.store(epoch, std::memory_order_release);
        s.entry.store(entry, std::memory_order_release);
        s.iidLo.store(Detail::IidLo(iid), std::memory_order_release);
        s.iidHi.store(Detail::IidHi(iid), std::memory_order_release);
    }

} // namespace Yuki

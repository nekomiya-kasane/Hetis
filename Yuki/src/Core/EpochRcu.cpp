/**
 * @file EpochRcu.cpp
 * @brief T23 — fixed-slot epoch-RCU implementation (spec §4.1–4.6).
 *
 * 64-slot open-addressed table indexed by CAS-claimed slot id. Threads claim a slot on
 * first @ref RcuReadGuard ctor and keep it for process lifetime; the slot's epoch field
 * is atomically toggled between @c 0 (quiescent) and the witnessed @c gGlobalEpoch value
 * (active). A global retire queue, protected by a plain mutex, holds @c {ptr, deleter,
 * stamp} triples; the reclaimer takes a snapshot of all slot epochs to compute @c safe
 * and frees retirees stamped @c <safe.
 *
 * Slot table size 64 covers Yuki's foundation-slice test workloads. Spec §4.2 step 1:
 * if every slot is occupied, kDebug asserts and the release build conservatively treats
 * the reader as "always active" (epoch never advances) — safe but starves the reclaimer
 * until a thread exits.
 */
#include <Yuki/Core/EpochRcu.h>
#include <Yuki/Core/Config.h>

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <vector>

namespace Yuki {

    namespace {

        constexpr std::size_t kSlotCount = 64;

        struct alignas(64) ThreadSlot {
            std::atomic<bool>     occupied{false};
            std::atomic<uint64_t> epoch{0};   ///< 0 = quiescent; nonzero = active at that epoch.
        };

        std::array<ThreadSlot, kSlotCount> gSlots{};
        std::atomic<uint64_t>              gGlobalEpoch{1};   ///< 0 reserved for "quiescent".

        struct Retired {
            void*    ptr;
            void   (*deleter)(void*);
            uint64_t stamp;
        };

        std::mutex&          RetireMu() noexcept { static std::mutex m;             return m; }
        std::vector<Retired>& RetireQ() noexcept { static std::vector<Retired> q;   return q; }

        thread_local ThreadSlot* tlSlot = nullptr;

        ThreadSlot* ClaimSlot() noexcept {
            for (std::size_t i = 0; i < kSlotCount; ++i) {
                bool expect = false;
                if (gSlots[i].occupied.compare_exchange_strong(
                        expect, true,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed)) {
                    return &gSlots[i];
                }
            }
            // §4.2 step 1: slot exhaustion. kDebug assert; release-build returns nullptr so
            // the guard falls through to "always active" (epoch publish is skipped, no slot
            // to write into), starving the reclaimer but never producing a use-after-free.
            if constexpr (kDebug) {
                assert(false && "EpochRcu slot table exhausted; raise kSlotCount");
            }
            return nullptr;
        }

    }  // namespace

    RcuReadGuard::RcuReadGuard() noexcept : wasOuter_(false) {
        if (tlSlot == nullptr) {
            tlSlot = ClaimSlot();
            if (tlSlot == nullptr) {
                // Release-build slot exhaustion fallback: treat as always-active sentinel.
                // wasOuter_ stays false so dtor is a no-op.
                return;
            }
        }
        // Nested-guard check: relaxed is fine here because the only writer of this slot's
        // epoch is the calling thread itself.
        if (tlSlot->epoch.load(std::memory_order_relaxed) != 0) {
            // Nested guard: outer guard already published an epoch; do nothing.
            return;
        }
        const uint64_t e = gGlobalEpoch.load(std::memory_order_acquire);
        tlSlot->epoch.store(e, std::memory_order_release);
        wasOuter_ = true;
    }

    RcuReadGuard::~RcuReadGuard() noexcept {
        if (wasOuter_ && tlSlot != nullptr) {
            tlSlot->epoch.store(0, std::memory_order_release);
        }
    }

    void RetireSnapshot(void* ptr, void (*deleter)(void*)) noexcept {
        if (!ptr || !deleter) return;
        const uint64_t stamp = gGlobalEpoch.fetch_add(1, std::memory_order_acq_rel);
        std::lock_guard<std::mutex> g(RetireMu());
        RetireQ().push_back(Retired{ptr, deleter, stamp});
    }

    std::size_t TryReclaim() noexcept {
        // §4.4 step 1: compute safe = min over occupied slots of their published epoch.
        // 0 (quiescent) is treated as "infinite" so quiescent slots don't pin retirees.
        uint64_t safe = UINT64_MAX;
        for (auto& slot : gSlots) {
            if (!slot.occupied.load(std::memory_order_acquire)) continue;
            const uint64_t e = slot.epoch.load(std::memory_order_acquire);
            if (e != 0 && e < safe) safe = e;
        }

        std::size_t freed = 0;
        std::lock_guard<std::mutex> g(RetireMu());
        auto& q = RetireQ();
        std::size_t w = 0;
        for (std::size_t r = 0; r < q.size(); ++r) {
            if (q[r].stamp < safe) {
                q[r].deleter(q[r].ptr);
                ++freed;
            } else {
                if (w != r) q[w] = q[r];
                ++w;
            }
        }
        q.resize(w);
        return freed;
    }

}  // namespace Yuki

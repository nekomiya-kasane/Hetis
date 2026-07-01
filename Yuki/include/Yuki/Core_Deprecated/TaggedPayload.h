#pragma once

#include <Yuki/Core/Identity.h>
#include <atomic>
#include <cstdint>

namespace Yuki {

    class RootObject;

    struct TaggedPayload {
        static constexpr std::uint16_t kExternalSentinel = 0xFFFF;
        static constexpr std::uint16_t kSaturationLimit = 0xFFFE;

        std::uint64_t word{};

        static constexpr TaggedPayload Make(ClassType r, RootObject* arm, std::uint16_t rc = 1,
                                            bool ever = true) noexcept {
            std::uint64_t w = (static_cast<std::uint64_t>(r) & 0x7) | ((ever ? 1ull : 0ull) << 3) |
                              ((static_cast<std::uint64_t>(rc) & 0xFFFF) << 4) |
                              ((reinterpret_cast<std::uint64_t>(arm) >> 4) << 20);
            return {w};
        }

        static constexpr TaggedPayload MakeExternal(ClassType r, RootObject* arm) noexcept {
            return Make(r, arm, kExternalSentinel, false);
        }

        constexpr ClassType Role() const noexcept { return ClassType(word & 0x7); }
        constexpr bool EverAcquired() const noexcept { return (word >> 3) & 0x1; }
        constexpr std::uint16_t Refcount() const noexcept { return (word >> 4) & 0xFFFF; }
        RootObject* ArmPtr() const noexcept { return reinterpret_cast<RootObject*>((word >> 20) << 4); }
        constexpr bool IsExternalLifetime() const noexcept { return Refcount() == kExternalSentinel; }

        // CAS loop: increment refcount unless saturated/sentinel. Returns false if not allowed.
        static bool TryIncrement(std::atomic<TaggedPayload>& a) noexcept {
            for (auto cur = a.load(std::memory_order_relaxed);;) {
                if (cur.IsExternalLifetime()) {
                    return true; // no-op
                }
                if (cur.Refcount() >= kSaturationLimit) {
                    return false; // saturated
                }
                TaggedPayload nxt = cur;
                nxt.word = (cur.word & ~(0xFFFFull << 4)) | ((std::uint64_t(cur.Refcount() + 1) & 0xFFFF) << 4);
                if (a.compare_exchange_weak(cur, nxt, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                    return true;
                }
            }
        }

        // Returns true iff the decrement transitioned refcount to 0 (caller deletes).
        static bool TryDecrement(std::atomic<TaggedPayload>& a) noexcept {
            for (auto cur = a.load(std::memory_order_relaxed);;) {
                if (cur.IsExternalLifetime()) {
                    return false;
                }
                std::uint16_t rc = cur.Refcount();
                if (rc == 0) {
                    return false;
                }
                TaggedPayload nxt = cur;
                nxt.word = (cur.word & ~(0xFFFFull << 4)) | ((std::uint64_t(rc - 1) & 0xFFFF) << 4);
                if (a.compare_exchange_weak(cur, nxt, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                    return (rc == 1);
                }
            }
        }
    };
    static_assert(sizeof(TaggedPayload) == sizeof(std::uint64_t));
} // namespace Yuki

#pragma once

#include <atomic>

namespace Mashiro {

    /** @brief Monotonic event-count used as a wake plane for queue state changes. */
    class AtomicEventCount {
    public:
        [[nodiscard]] uint64_t Load() const noexcept { return epoch_.load(std::memory_order_acquire); }

        void Wait(uint64_t snapshot) const noexcept { epoch_.wait(snapshot, std::memory_order_acquire); }

        void NotifyOne() noexcept {
            epoch_.fetch_add(1, std::memory_order_release);
            epoch_.notify_one();
        }

        void NotifyAll() noexcept {
            epoch_.fetch_add(1, std::memory_order_release);
            epoch_.notify_all();
        }

    private:
        std::atomic<uint64_t> epoch_{0};
    };

} // namespace Mashiro

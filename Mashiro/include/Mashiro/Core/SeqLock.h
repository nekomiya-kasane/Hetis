/**
 * @brief A sequence lock implementation for synchronizing concurrent access to shared data.
 *
 * - `seq_` is even: no writer is active, readers can safely read the data.
 * - `seq_` is odd: a writer is active, readers must retry.
 */

#pragma once

#include <atomic>

#include "TypeTraits.h"

namespace Mashiro {

    template<typename T>
        requires std::is_trivially_copyable_v<T>
    class SeqLock {
    public:
        void Write(T&& value) {
            seq_.fetch_add(1, std::memory_order_release); // Start write (odd seq)
            data_ = std::forward<T>(value);               // Write data
            seq_.fetch_add(1, std::memory_order_release); // End write (even seq)
        }

        [[nodiscard]] T Read() const {
            T result;
            uint32_t seq;
            do {
                seq = seq_.load(std::memory_order_acquire);
                result = data_;
            } while (seq & 1 || seq != seq_.load(std::memory_order_acquire));
            return result;
        }

    private:
        alignas(Platform::kCacheLineSize) std::atomic<uint32_t> seq_{0};
        T data_{};
    };

} // namespace Mashiro

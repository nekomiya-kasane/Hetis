#pragma once

#include <Sora/Core/Concurrency/FalseSharingChecker.h>
#include <Sora/Core/Concurrency/QueueBatch.h>

#include <atomic>
#include <condition_variable>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Sora {

    namespace Concurrency {

        /**
         * @brief Strongest formally claimed progress guarantee for an eligible storage transfer.
         * @details A prompt `try_*` failure is an API execution property, not evidence of lock-free progress. In
         * particular, a reservation-based FIFO may return promptly while a descheduled reservation owner prevents every
         * useful transfer at the queue head. Sender suspension and backpressure policy are separate properties again.
         */
        enum class QueueProgress : std::uint8_t {
            WaitFree,        /**< Every participating operation completes in a bounded number of its own steps. */
            LockFree,        /**< The system continually completes some operation, though one participant may starve. */
            ObstructionFree, /**< An operation completes when it eventually executes without interference. */
            Unspecified,     /**< No formal progress theorem is claimed beyond the documented API behavior. */
        };

        /** @brief Result of one non-waiting adapter transfer attempt. */
        enum class QueueTransferStatus : std::uint8_t {
            Committed,     /**< The input was transferred and ownership effects are committed. */
            BackPressure,  /**< Transient capacity or ordering pressure prevented a transfer. */
            BatchTooLarge, /**< A batch cannot fit the adapter's fixed element domain. */
            InvalidValue,  /**< The value is outside the storage's permanent value domain. */
        };

        /** @brief Primary adapter declaration. Unsupported storage has no accidental fallback. */
        template<class Buffer>
        struct QueueAdapter {
            using BufferType = Buffer;
            using ValueType = typename Buffer::ValueType;
            using BatchType = QueueBatch<ValueType, Buffer::kCapacity>;

            [[nodiscard]] static QueueTransferStatus TryPush(BufferType& buffer, ValueType& value) noexcept {
                return buffer.TryPush(std::move(value)) ? QueueTransferStatus::Committed
                                                        : QueueTransferStatus::BackPressure;
            }

            [[nodiscard]] static std::optional<ValueType> TryPop(BufferType& buffer) noexcept {
                return buffer.TryPop();
            }

            [[nodiscard]] static QueueTransferStatus TryPushBatch(BufferType& buffer, std::span<ValueType> batch) noexcept {
                return buffer.TryPushBatch(batch) ? QueueTransferStatus::Committed : QueueTransferStatus::BackPressure;
            }

            [[nodiscard]] static bool TryPopBatch(BufferType& buffer, std::span<ValueType> batch) noexcept {
                return buffer.TryPopBatch(batch);
            }

            [[nodiscard]] static bool IsDrained(const BufferType& buffer) noexcept { return buffer.IsDrained(); }
            [[nodiscard]] static std::size_t SizeApprox(const BufferType& buffer) noexcept { return buffer.SizeApprox(); }
            }

            [[nodiscard]] static bool IsDrained(const BufferType& buffer) noexcept;
            [[nodiscard]] static std::size_t SizeApprox(const BufferType& buffer) noexcept;
        };

    } // namespace Concurrency

} // namespace Sora
/**
 * @file QueueError.h
 * @brief Error vocabulary for P2300-native asynchronous queues.
 * @ingroup Concurrency
 */
#pragma once

#include <cstdint>

namespace Mashiro::Concurrency {

    /** @brief Closed set of recoverable queue errors reported through @c set_error. */
    enum class QueueErrorCode : std::uint8_t {
        None,
        Closed,
        Aborted,
        BatchTooLarge,
    };

    /** @brief Lightweight error object carried by queue senders. */
    struct QueueError {
        QueueErrorCode code{QueueErrorCode::None};

        [[nodiscard]] friend constexpr bool operator==(QueueError, QueueError) noexcept = default;

        [[nodiscard]] constexpr explicit operator bool() const noexcept { return code != QueueErrorCode::None; }
    };

    inline constexpr QueueError kQueueClosed{QueueErrorCode::Closed};
    inline constexpr QueueError kQueueAborted{QueueErrorCode::Aborted};
    inline constexpr QueueError kQueueBatchTooLarge{QueueErrorCode::BatchTooLarge};

} // namespace Mashiro::Concurrency

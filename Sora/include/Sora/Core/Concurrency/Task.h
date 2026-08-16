/**
 * @file Task.h
 * @brief Sender-aware coroutine task facade.
 * @ingroup Concurrency
 */
#pragma once

#include <exec/task.hpp>

namespace Sora::Concurrency {

    /**
     * @brief Lazy, move-only coroutine whose promise can await senders.
     *
     * @details The promise adapts every awaited sender through its execution environment. Stop tokens, errors,
     * stopped completion, and start-scheduler affinity therefore remain part of the sender contract instead of being
     * reimplemented by each asynchronous resource. A @c Task is also a sender and starts only when connected and
     * started by a consumer such as @c stdexec::sync_wait.
     *
     * @tparam T Value produced by successful completion, or @c void.
     *
     * @note Coroutine arguments with reference semantics must outlive completion of the task. In particular, a task
     * awaiting an AsyncQueue operation does not extend the lifetime of the queue or its producer/consumer port.
     */
    template<class T = void>
    using Task = stdexec::task<T>;

} // namespace Sora::Concurrency

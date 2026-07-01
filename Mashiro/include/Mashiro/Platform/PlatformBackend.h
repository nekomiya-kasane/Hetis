/**
 * @file PlatformBackend.h
 * @brief Native readiness and event-ingress boundary for the Platform thread.
 *
 * PlatformBackend is the OS-affine leaf below EventPump. It owns native handles, waits for source readiness,
 * coalesces cross-thread wakeups, and drains native queues into @ref Mashiro::SystemEvent values. It does not own
 * semantic state, subscriber fan-out, or manager bookkeeping; EventPump performs those steps after translation.
 *
 * @par Boundary
 * - @ref WaitForAnySource is the readiness plane: one blocking wait over native input, cross-thread wake, and stop.
 * - @ref Wake is the wake plane: any producer can make the Platform thread observe external work.
 * - @ref DrainNative is the payload plane: platform records are translated and synchronously emitted upward.
 *
 * @par Async placement
 * This type deliberately does not expose a sender or coroutine. It is the synchronous OS primitive beneath
 * EventPump::Run, where the loop is lifted into stdexec. Keeping the native bridge below the sender graph prevents
 * OS handles and platform queue rules from leaking into structured-concurrency composition.
 *
 * @ingroup Platform
 */
#pragma once

#include "Mashiro/Platform/EventEmission.h"

#include <stdexec/execution.hpp>

#include <memory>

namespace Mashiro {

    namespace Backend {

        /**
         * @brief Run process-wide platform initialization before native windows are created.
         */
        void Initialize();

    } // namespace Backend

} // namespace Mashiro

namespace Mashiro::Platform {

    struct WindowManager;

    /**
     * @brief Native OS bridge owned by the Platform thread.
     *
     * @par Threading
     * @ref WaitForAnySource and @ref DrainNative are Platform-thread-only. @ref Wake is callable from any thread.
     */
    class PlatformBackend final {
    public:
        PlatformBackend();
        ~PlatformBackend();

        PlatformBackend(const PlatformBackend&)            = delete;
        PlatformBackend& operator=(const PlatformBackend&) = delete;
        PlatformBackend(PlatformBackend&&)                 = delete;
        PlatformBackend& operator=(PlatformBackend&&)      = delete;

        /**
         * @brief Block until native input, cross-thread wake, or @p stop is ready.
         *
         * The wait is level-triggered on the native queue: if the caller does not drain ready input before the next
         * wait, the next wait returns immediately.
         */
        void WaitForAnySource(stdexec::inplace_stop_token stop) const noexcept;

        /**
         * @brief Coalescing wake-up callable from any thread.
         *
         * Multiple concurrent calls collapse to one wake at the wait site. Producers therefore do not need to throttle
         * wake calls.
         */
        void Wake() const noexcept;

        /**
         * @brief Bind the Platform-thread window registry used to resolve native handles.
         *
         * The backend does not own @p windows. The registry lives for the same Platform-thread frame as
         * @ref EventPump::Run, and the backend only reads it while draining native input on that thread.
         */
        void AttachWindowRegistry(WindowManager& windows) const noexcept;

        /**
         * @brief Drain native messages and synchronously emit translated events to @p consume.
         *
         * Non-translatable native records are consumed silently. The backend drains until the OS queue is observed
         * empty; records that arrive later are observed by the next @ref WaitForAnySource.
         */
        void DrainNative(SystemEventConsumerRef consume) const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace Mashiro::Platform

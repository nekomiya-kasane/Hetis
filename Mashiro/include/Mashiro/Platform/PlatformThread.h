/**
 * @file PlatformThread.h
 * @brief The Platform thread: spawns the OS-input apartment, drives the EventPump,
 *        and exposes a stable EventChannel to the rest of the application.
 *
 * The Platform thread is the *only* thread that may talk to the OS input
 * surface (Win32 message queue, Wayland fd, ...). Other threads communicate
 * with platform state via two routes:
 *
 *   - Subscribing to the broadcast: @ref AppChannel returns the application's
 *     EventChannel (allocated at startup), which the Platform thread fills via
 *     @c EventPump::DispatchEvent.
 *   - Cross-thread coroutine: a worker @c co_await's an @c OwnerTask<T> awaiter
 *     that posts its handle to the OwnerExecutor mailbox, the Platform thread
 *     resumes it on its own stack, and the awaiter completes back on the
 *     worker thread when the result is ready.
 *
 * @ingroup Platform
 */
#pragma once

#include "Mashiro/Platform/EventChannel.h"
#include "Mashiro/Platform/PlatformBackend.h"
#include "Mashiro/Platform/ThreadNaming.h"

#include <atomic>
#include <thread>

namespace Mashiro {

    namespace Detail {
        template<class Receiver>
        struct WhenReadyOpState;
    } // namespace Detail

    /**
     * @brief Owns the OS-input thread and the application's view onto it.
     *
     * Lifetime: construct → @ref Run on a dedicated @c std::thread → mainline
     * waits on @ref Ready then uses @ref AppChannel → mainline calls
     * @ref RequestStop → joins the thread.
     */
    class PlatformThread final {
    public:
        enum class State { Idle, Running, Stopped };

        PlatformThread();
        ~PlatformThread();

        PlatformThread(const PlatformThread&) = delete;
        PlatformThread& operator=(const PlatformThread&) = delete;
        PlatformThread(PlatformThread&&) = delete;
        PlatformThread& operator=(PlatformThread&&) = delete;

        /// @brief Run the platform-input loop until @ref RequestStop. Blocks the caller.
        void Run();

        /// @brief Cooperative shutdown signal. Safe from any thread.
        void RequestStop() noexcept;

        /// @brief The application's subscriber endpoint. Non-null only after @ref Ready returns true.
        [[nodiscard]] Platform::EventChannel* AppChannel() const noexcept { return appChannel_; }

        /// @brief True once the Platform thread has finished startup and the
        ///        application channel is ready for consumption.
        [[nodiscard]] bool Ready() const noexcept { return ready_.load(std::memory_order_acquire); }

        /// @brief Sender form of @ref Ready — completes with the application
        ///        channel pointer when it is published, or @c set_stopped when
        ///        the receiver's stop_token fires first. Pointer (rather than
        ///        reference) because @c EventChannel is non-copyable and the
        ///        coroutine suspension boundary requires a decay-copyable value.
        ///
        /// Lost-wake-free by construction: @c Run sequences @c appChannel_ store →
        /// @c ready_.store(release) → @c notify_all. The waiter checks the flag
        /// under the release/acquire link and re-checks after each futex wake.
        /// Stop integration is by @c stop_callback into @c notify_all on the
        /// same atomic — no extra mutex, no condvar, no heap allocation.
        [[nodiscard]] auto WhenReady() noexcept;

    private:
        // Sender op-state internals access the wait surface via helper methods
        // below — keeps @c ready_ private and the wait API a single point of
        // truth. The op-state is defined in PlatformThread.inl.
        template<class R>
        friend struct Detail::WhenReadyOpState;

        void WaitReady() const noexcept { ready_.wait(false, std::memory_order_acquire); }
        void NotifyReadyWaitersForStop() noexcept {
            // Wake every parked waiter without changing the observed value
            // (a true-store would impersonate a real readiness publish).
            ready_.notify_all();
        }

        // "Run() is in flight" (state_) and "channel is published" (ready_)
        // are distinct: state_ flips at Run() entry, ready_ flips after the
        // channel is allocated. WhenReady waits on ready_; double-Run
        // rejection uses state_.
        std::atomic<State> state_{State::Idle};
        std::atomic<bool> ready_{false};
        std::thread::id ownerThread_{};
        Platform::EventChannel* appChannel_{nullptr};

        // Pump and stop_source live in Run()'s stack frame so they share the
        // Platform thread's lifetime exactly. RequestStop posts to a heap-stable
        // bridge object set up in Run(); see PlatformThread.cpp.
        struct StopBridge;
        std::atomic<StopBridge*> stopBridge_{nullptr};
    };

    namespace Detail {

        /// @brief Operation state for @ref Mashiro::PlatformThread::WhenReady.
        ///
        /// Parameterised on @c Receiver so the stop_callback type derived from
        /// the receiver's environment can be installed without type erasure. All
        /// state is inline — connect+start performs no heap allocation.
        template<class Receiver>
        struct WhenReadyOpState {
            PlatformThread* owner;
            Receiver receiver;

            struct OnStop {
                PlatformThread* owner;
                void operator()() const noexcept {
                    // Wake every parked waiter; each will re-check Ready() and
                    // its own stop_token. The publication and stop paths share
                    // ready_ as the single wait surface — see file header.
                    owner->NotifyReadyWaitersForStop();
                }
            };

            using StopToken = stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;
            using StopCallback = typename StopToken::template callback_type<OnStop>;
            std::optional<StopCallback> stopCb;

            void start() noexcept {
                // Fast path: already ready — no wait, no callback.
                if (owner->Ready()) {
                    stdexec::set_value(std::move(receiver), owner->AppChannel());
                    return;
                }

                auto token = stdexec::get_stop_token(stdexec::get_env(receiver));
                stopCb.emplace(token, OnStop{owner});

                // If the callback fired inline (stop already requested), Ready()
                // is still false but notify_all has run; we observe stop_requested
                // on the first iteration and complete via set_stopped.
                for (;;) {
                    if (owner->Ready()) {
                        stopCb.reset();
                        stdexec::set_value(std::move(receiver), owner->AppChannel());
                        return;
                    }
                    if (token.stop_requested()) {
                        stopCb.reset();
                        stdexec::set_stopped(std::move(receiver));
                        return;
                    }
                    owner->WaitReady();
                }
            }
        };

        struct WhenReadySender {
            PlatformThread* owner;

            using sender_concept = stdexec::sender_t;
            // EventChannel is non-copyable / non-movable, so the value type must
            // be the channel pointer rather than a reference. The pointer is
            // decay-copyable, which @c exec::task / @c continues_on require to
            // carry the value across the suspension boundary. Callers dereference
            // at the use site (the channel is guaranteed non-null when Ready()).
            using completion_signatures =
                stdexec::completion_signatures<stdexec::set_value_t(Platform::EventChannel*), stdexec::set_stopped_t()>;

            template<class Receiver>
            auto connect(Receiver&& r) const noexcept -> WhenReadyOpState<std::remove_cvref_t<Receiver>> {
                return WhenReadyOpState<std::remove_cvref_t<Receiver>>{
                    .owner = owner,
                    .receiver = std::forward<Receiver>(r),
                };
            }
        };

    } // namespace Detail

    inline auto PlatformThread::WhenReady() noexcept {
        return Detail::WhenReadySender{this};
    }

} // namespace Mashiro

/**
 * @file PlatformThread.h
 * @brief Owner-thread facade for platform input, cancellation, and application-event readiness.
 *
 * PlatformThread is the public boundary above Platform::EventPump. The thread that enters @ref Run becomes the
 * platform owner thread. Native readiness, OS message translation, and event fan-out stay below EventPump and
 * PlatformBackend; this facade exposes only lifecycle, shutdown, and the application subscriber channel.
 *
 * @ingroup Platform
 */
#pragma once

#include "Mashiro/Platform/EventChannel.h"

#include <stdexec/execution.hpp>

#include <atomic>
#include <cstdint>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

namespace Mashiro {

    namespace Detail {

        template<class Receiver>
        concept PlatformReadyReceiver = requires(Receiver receiver, Platform::EventChannel* channel) {
            typename stdexec::env_of_t<Receiver>;
            typename stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;
            stdexec::set_value(std::move(receiver), channel);
            stdexec::set_stopped(std::move(receiver));
        };

        template<class Receiver>
        struct PlatformReadinessOpState;

        struct PlatformReadinessSender;

    } /* namespace Detail */

    /**
     * @brief Owns the platform-thread execution lifetime and the application event channel.
     *
     * @details The object does not spawn an inner OS-input thread. @ref Run takes over the calling thread and drives
     * the Platform::EventPump until cancellation. Other threads observe startup through @ref WhenReady and request
     * shutdown through @ref RequestStop.
     */
    class PlatformThread final {
        enum class LifecycleState : std::uint8_t { Idle, Running, Stopped };

    public:
        PlatformThread();
        ~PlatformThread();

        PlatformThread(const PlatformThread&) = delete;
        PlatformThread& operator=(const PlatformThread&) = delete;
        PlatformThread(PlatformThread&&) = delete;
        PlatformThread& operator=(PlatformThread&&) = delete;

        /**
         * @brief Drive the platform loop on the calling thread until stop is requested.
         *
         * @note A second call after @ref Run has started is ignored. The object is a single-lifetime owner, not a
         * restartable service.
         */
        void Run();

        /** @brief Request cooperative shutdown. Safe and idempotent from any thread, including before @ref Run. */
        void RequestStop() noexcept;

        /** @brief Published application channel, or @c nullptr before startup and after shutdown. */
        [[nodiscard]] Platform::EventChannel* AppChannel() const noexcept {
            return appChannel_.load(std::memory_order_acquire);
        }

        /** @brief Whether the application channel is currently published. */
        [[nodiscard]] bool Ready() const noexcept { return AppChannel() != nullptr; }

        /**
         * @brief Sender that completes when the application channel is published.
         *
         * @return A sender completing with @c set_value(EventChannel*) or @c set_stopped(). The pointer is stable until
         * @ref Run begins shutdown; clients that consume events should compose their own stop token with the awaiter.
         *
         * @details Readiness uses an atomic pointer for publication and a monotonic epoch for waits. The epoch changes
         * on publish, shutdown, and receiver-stop callbacks, so waiters cannot miss a wake when the observed readiness
         * value itself remains unchanged.
         */
        [[nodiscard]] auto WhenReady() noexcept -> Detail::PlatformReadinessSender;

    private:
        template<class R>
        friend struct Detail::PlatformReadinessOpState;

        [[nodiscard]] bool Stopped() const noexcept {
            return state_.load(std::memory_order_acquire) == LifecycleState::Stopped;
        }

        [[nodiscard]] std::uint64_t LoadReadinessEpoch() const noexcept {
            return readinessEpoch_.load(std::memory_order_acquire);
        }

        void WaitReadinessEpoch(std::uint64_t snapshot) const noexcept {
            readinessEpoch_.wait(snapshot, std::memory_order_acquire);
        }

        void NotifyReadinessWaiters() noexcept {
            readinessEpoch_.fetch_add(1, std::memory_order_release);
            readinessEpoch_.notify_all();
        }

        std::atomic<LifecycleState> state_{LifecycleState::Idle};
        std::atomic<Platform::EventChannel*> appChannel_{nullptr};
        std::atomic<std::uint64_t> readinessEpoch_{0};
        std::thread::id ownerThread_{};

        struct StopBridge;
        std::atomic<bool> stopRequested_{false};
        std::atomic<StopBridge*> stopBridge_{nullptr};
    };

    namespace Detail {

        /**
         * @brief Operation state for @ref PlatformThread::WhenReady.
         *
         * @tparam Receiver Receiver type selected by the surrounding stdexec graph.
         */
        template<class Receiver>
        struct PlatformReadinessOpState {
            PlatformThread* owner;
            Receiver receiver;

            /** @brief Stop callback that wakes waiters through the readiness epoch. */
            struct OnStop {
                PlatformThread* owner;
                void operator()() const noexcept { owner->NotifyReadinessWaiters(); }
            };

            using StopToken = stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;
            using StopCallback = typename StopToken::template callback_type<OnStop>;

            std::optional<StopCallback> stopCallback;

            void start() noexcept {
                if (auto* channel = owner->AppChannel()) {
                    stdexec::set_value(std::move(receiver), channel);
                    return;
                }
                if (owner->Stopped()) {
                    stdexec::set_stopped(std::move(receiver));
                    return;
                }

                auto token = stdexec::get_stop_token(stdexec::get_env(receiver));
                if (token.stop_requested()) {
                    stdexec::set_stopped(std::move(receiver));
                    return;
                }

                stopCallback.emplace(token, OnStop{owner});
                auto snapshot = owner->LoadReadinessEpoch();

                for (;;) {
                    if (auto* channel = owner->AppChannel()) {
                        stopCallback.reset();
                        stdexec::set_value(std::move(receiver), channel);
                        return;
                    }
                    if (owner->Stopped() || token.stop_requested()) {
                        stopCallback.reset();
                        stdexec::set_stopped(std::move(receiver));
                        return;
                    }

                    owner->WaitReadinessEpoch(snapshot);
                    snapshot = owner->LoadReadinessEpoch();
                }
            }
        };

        /** @brief Sender returned by @ref PlatformThread::WhenReady. */
        struct PlatformReadinessSender {
            PlatformThread* owner;

            using sender_concept = stdexec::sender_t;
            using completion_signatures =
                stdexec::completion_signatures<stdexec::set_value_t(Platform::EventChannel*), stdexec::set_stopped_t()>;

            template<class Receiver>
                requires PlatformReadyReceiver<std::remove_cvref_t<Receiver>>
            auto connect(Receiver&& receiver) const
                noexcept(std::is_nothrow_constructible_v<std::remove_cvref_t<Receiver>, Receiver>)
                    -> PlatformReadinessOpState<std::remove_cvref_t<Receiver>> {
                return PlatformReadinessOpState<std::remove_cvref_t<Receiver>>{
                    .owner = owner,
                    .receiver = std::forward<Receiver>(receiver),
                };
            }
        };

    } /* namespace Detail */

    inline auto PlatformThread::WhenReady() noexcept -> Detail::PlatformReadinessSender {
        return Detail::PlatformReadinessSender{this};
    }

} /* namespace Mashiro */

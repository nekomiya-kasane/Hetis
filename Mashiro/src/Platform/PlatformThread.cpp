/**
 * @file PlatformThread.cpp
 * @brief Implementation of @ref Mashiro::PlatformThread.
 *
 * @ingroup Platform
 */
#include "Mashiro/Platform/PlatformThread.h"

#include "Mashiro/Platform/Common.h"
#include "Mashiro/Platform/ManagerSet.h"
#include "Mashiro/Platform/PlatformBackend.h"
#include "Mashiro/Platform/ThreadNaming.h"

#include <stdexec/execution.hpp>

#include <atomic>
#include <memory>
#include <mutex>

namespace Mashiro {

    /**
     * @brief Bridge from cross-thread stop requests to the in-frame platform stop source.
     *
     * The bridge is heap-stable from the caller's perspective because @ref PlatformThread stores only its address in an
     * atomic pointer while @ref Run is active. Requests posted before the bridge is installed are retained by
     * @c PlatformThread::stopRequested_ and replayed immediately after installation.
     */
    struct PlatformThread::StopBridge {
        std::mutex mu;
        stdexec::inplace_stop_source* source = nullptr;
        bool requested = false;

        void Install(stdexec::inplace_stop_source& sourceRef) noexcept {
            std::scoped_lock lock(mu);
            source = &sourceRef;
            if (requested) {
                source->request_stop();
            }
        }

        void Request() noexcept {
            std::scoped_lock lock(mu);
            requested = true;
            if (source) {
                source->request_stop();
            }
        }

        void Detach() noexcept {
            std::scoped_lock lock(mu);
            source = nullptr;
        }
    };

    PlatformThread::PlatformThread() = default;
    PlatformThread::~PlatformThread() = default;

    void PlatformThread::Run() {
        LifecycleState expected = LifecycleState::Idle;
        if (!state_.compare_exchange_strong(expected, LifecycleState::Running)) {
            return;
        }

        SetCurrentThreadName("Platform");
        ownerThread_ = std::this_thread::get_id();
        Backend::Initialize();

        stdexec::inplace_stop_source stopSource;
        stdexec::counting_scope scope;

        StopBridge bridge;
        bridge.Install(stopSource);
        stopBridge_.store(&bridge, std::memory_order_release);
        if (stopRequested_.load(std::memory_order_acquire)) {
            bridge.Request();
        }

        Platform::PlatformPump pump;
        pump.AttachContext(scope, stopSource.get_token());

        Platform::WindowManager windowManager{};
        Platform::Input input{};
        Platform::Ime ime{};
        Platform::Clipboard clipboard{};
        Platform::Cursor cursor{};
        Platform::DragDrop dragDrop{};
        Platform::Dialog dialog{};
        Platform::Surface surface{};
        Platform::Appearance appearance{};
        Platform::Accessibility accessibility{};
        Platform::Gamepad gamepad{};
        Platform::FileWatch fileWatch{};
        Platform::Display display{};
        Platform::Power power{};
        Platform::AudioDevice audioDevice{};

        if (auto result =
                pump.AttachManagers(windowManager, input, ime, clipboard, cursor, dragDrop, dialog, surface, appearance,
                                    accessibility, gamepad, fileWatch, display, power, audioDevice);
            !result) {
            stopBridge_.store(nullptr, std::memory_order_release);
            bridge.Detach();
            state_.store(LifecycleState::Stopped, std::memory_order_release);
            NotifyReadinessWaiters();
            return;
        }

        Platform::EventChannel& channel = pump.AddChannel();
        appChannel_.store(&channel, std::memory_order_release);
        NotifyReadinessWaiters();

        stdexec::sync_wait(stdexec::when_all(pump.Run(), scope.join()));

        appChannel_.store(nullptr, std::memory_order_release);
        stopBridge_.store(nullptr, std::memory_order_release);
        bridge.Detach();
        state_.store(LifecycleState::Stopped, std::memory_order_release);
        NotifyReadinessWaiters();
    }

    void PlatformThread::RequestStop() noexcept {
        stopRequested_.store(true, std::memory_order_release);
        if (auto* bridge = stopBridge_.load(std::memory_order_acquire)) {
            bridge->Request();
        }
    }

} /* namespace Mashiro */
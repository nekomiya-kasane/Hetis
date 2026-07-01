/**
 * @file Main.cpp
 * @brief Playground entry point â€” @c main is the Platform thread (spec Â§12.1).
 *
 * Topology:
 *   - @c main becomes the Platform thread by calling @ref PlatformThread::Run.
 *     This is the sole owner of OS-input affinity and the structural-
 *     concurrency root for every spawned sender.
 *   - One @c std::jthread hosts the application client. It is spawned before
 *     @c Run takes over @c main and is structurally an outer concentric ring
 *     around the Platform thread: its destructor signals the stop_token after
 *     @c Run returns and then joins.
 *   - Shutdown has a single source of truth: @ref PlatformThread::RequestStop.
 *     The client triggers it on @c WindowCloseEvent; the platform's stop
 *     propagates back to the client through the jthread's stop_token when
 *     @c main reaches the end-of-scope dtor.
 *
 * The client coroutine is sender-driven end-to-end:
 *   - @c co_await platform.WhenReady() replaces the previous spin on
 *     @c Ready(); it completes with @c set_value(EventChannel&) once the
 *     channel is published, or @c set_stopped if stop fires first.
 *   - @c co_await events.NextBatch() replaces the previous TryPop / sleep_for
 *     loop; it completes with @c set_value(EventBatch) after atomically draining
 *     the currently committed events, or @c set_stopped on
 *     stop. There is no @c IsClosed poll: stop arrives as a coroutine
 *     completion, not a flag query.
 */
#include "Mashiro/Core/StructuredLogger.h"
#include "Mashiro/Platform/EventChannel.h"
#include "Mashiro/Platform/PlatformThread.h"
#include "Mashiro/Platform/SystemEvent.h"

#include <exec/task.hpp>
#include <stdexec/execution.hpp>

#include <cstdint>
#include <stop_token>
#include <thread>
#include <utility>
#include <variant>

namespace {

    /// @brief Reactive drain of the Platform-broadcast channel.
    ///
    /// Awaits readiness, then processes each @c NextBatch drain. The coroutine
    /// returns via @c co_return when @c WindowCloseEvent is seen (after
    /// signalling @ref PlatformThread::RequestStop), and unwinds via the
    /// task machinery's @c set_stopped path when its stop_token fires while
    /// parked on either awaiter.
    auto AppLoop(Mashiro::PlatformThread& platform) -> exec::task<void> {
        using namespace Mashiro;

        // co_await may complete via set_stopped (stop fired before publish);
        // exec::task propagates that as a cancellation-unwind of this coroutine,
        // so we never see a "null" return â€” we either get a valid pointer or
        // never reach this line.
        auto* events = co_await platform.WhenReady();

        std::uint64_t processed = 0;
        for (;;) {
            auto batch = co_await events->NextBatch();

            bool shouldExit = false;
            for (const SystemEvent& ev : batch) {
                ++processed;
                std::visit(
                    [&]<class P>(const P&) {
                        if constexpr (std::same_as<P, Event::WindowCloseEvent>) {
                            platform.RequestStop();
                            shouldExit = true;
                        }
                    },
                    ev);
            }
            if (shouldExit) {
                break;
            }
        }

        MLOG(Info, App, "processed {} platform events", processed);
        co_return;
    }

} // namespace

int main() {
    using namespace Mashiro;

    auto& logger = StructuredLogger::Instance();
    logger.AddSink(ConsoleSink{});
    logger.StartDrainThread();

    PlatformThread platform;

    // The client jthread is the OUTER concentric ring around the Platform
    // thread's structured-concurrency scope: spawned before Run() takes over
    // main, destroyed (signal stop_token + join) after Run() returns.
    //
    // exec::task is driven on a per-thread run_loop so the coroutine resumes
    // on this jthread's stack. WhenReady / NextBatch park here and complete
    // here; only the producer side runs on the Platform thread.
    std::jthread appWorker{[&platform](std::stop_token st) {
        stdexec::run_loop loop;
        std::stop_callback finishOnStop{st, [&loop] { loop.finish(); }};
        stdexec::sync_wait(stdexec::on(loop.get_scheduler(), AppLoop(platform)));
    }};

    platform.Run();

    // appWorker's dtor signals its stop_token, which finishes its run_loop
    // and joins. By the time logger.Shutdown() runs, every thread that
    // could log is already joined.
    appWorker.request_stop();
    appWorker.join();

    logger.Shutdown();
    return 0;
}

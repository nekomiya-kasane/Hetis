/**
 * @file CommandRuntimeStdexec.h
 * @brief Optional stdexec bridge for the experimental CAD command runtime.
 * @ingroup KernelExperimental
 */
#pragma once

#include "CommandRuntime.h"

#include <string_view>
#include <type_traits>
#include <utility>

#if __has_include(<stdexec/execution.hpp>)
#include <stdexec/execution.hpp>
#define SORA_COMMAND_RUNTIME_HAS_STDEXEC 1
#else
#define SORA_COMMAND_RUNTIME_HAS_STDEXEC 0
#endif

#if SORA_COMMAND_RUNTIME_HAS_STDEXEC && __has_include(<exec/start_detached.hpp>)
#include <exec/start_detached.hpp>
#define SORA_COMMAND_RUNTIME_HAS_STDEXEC_DETACHED 1
#else
#define SORA_COMMAND_RUNTIME_HAS_STDEXEC_DETACHED 0
#endif

namespace Sora::Kernel::Experimental::CommandRuntime::StdexecBridge {

    struct BridgeStatus {
        bool stdexec_available{};
        bool detached_start_available{};
        std::string_view semantics{};
    };

    [[nodiscard]] inline constexpr BridgeStatus Status() noexcept {
        return BridgeStatus{
            .stdexec_available = SORA_COMMAND_RUNTIME_HAS_STDEXEC != 0,
            .detached_start_available = SORA_COMMAND_RUNTIME_HAS_STDEXEC_DETACHED != 0,
            .semantics = "runtime Completion<T> is transported as a stdexec value",
        };
    }

#if SORA_COMMAND_RUNTIME_HAS_STDEXEC

    template<RuntimeSender Sender>
    [[nodiscard]] auto AsCompletionSender(Sender sender) {
        return stdexec::just(std::move(sender)) | stdexec::then([](Sender runtime_sender) mutable {
                   return SyncWait(std::move(runtime_sender));
               });
    }

    template<stdexec::scheduler Scheduler, RuntimeSender Sender>
    [[nodiscard]] auto ScheduleCompletion(Scheduler scheduler, Sender sender) {
        return stdexec::schedule(std::move(scheduler)) |
               stdexec::then([runtime_sender = std::move(sender)]() mutable {
                   return SyncWait(std::move(runtime_sender));
               });
    }

#if SORA_COMMAND_RUNTIME_HAS_STDEXEC_DETACHED

    template<stdexec::scheduler Scheduler, RuntimeSender Sender, typename Callback>
        requires std::move_constructible<Callback>
    void StartDetachedCompletion(Scheduler scheduler, Sender sender, Callback callback) {
        auto task = ScheduleCompletion(std::move(scheduler), std::move(sender)) |
                    stdexec::then([callback = std::move(callback)](auto completion) mutable {
                        callback(std::move(completion));
                    });
        exec::start_detached(std::move(task));
    }

#endif

#endif

} // namespace Sora::Kernel::Experimental::CommandRuntime::StdexecBridge

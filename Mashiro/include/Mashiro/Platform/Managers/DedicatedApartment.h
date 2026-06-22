/**
 * @file DedicatedApartment.h
 * @brief Dedicated-thread Manager skeletons (@c Gamepad, @c FileWatch).
 *
 * Dedicated Managers each own their own @c std::jthread and pump events from a
 * blocking native API into the EventPump's external inbox. Their public API is
 * therefore @c Start(stop, post, scope) — EventPump calls this exactly once
 * during @c AttachManagers. Failure here aborts pump bring-up.
 */
#pragma once

#include "Mashiro/Core/Result.h"
#include "Mashiro/Platform/SystemEvent.h"
#include "Mashiro/Platform/ThreadContract.h"

#include <stdexec/execution.hpp>

#include <functional>

namespace Mashiro::Platform {

    /// @brief Type of the post callback EventPump hands to each Dedicated Manager.
    using ExternalPostFn = std::function<bool(SystemEvent) /*noexcept-by-contract*/>;

    /// @brief XInput / SDL gamepad poll loop (no OS-level event for axis change).
    struct [[=OnDedicatedThread]] Gamepad {
        Result<void> Start(stdexec::inplace_stop_token /*stop*/,
                           ExternalPostFn /*post*/,
                           stdexec::counting_scope& /*scope*/) noexcept {
            return {};
        }
    };

    /// @brief @c ReadDirectoryChangesW / @c inotify watcher.
    struct [[=OnDedicatedThread]] FileWatch {
        Result<void> Start(stdexec::inplace_stop_token /*stop*/,
                           ExternalPostFn /*post*/,
                           stdexec::counting_scope& /*scope*/) noexcept {
            return {};
        }
    };

} // namespace Mashiro::Platform

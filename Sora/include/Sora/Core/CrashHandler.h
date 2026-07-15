/**
 * @file CrashHandler.h
 * @brief RAII ownership of process-wide fatal signal and unhandled native exception handlers.
 * @ingroup Core
 */
#pragma once

#include "Sora/ErrorCode.h"

#include <memory>

namespace Sora {

    /**
     * @brief Exclusive process-wide crash-handler registration.
     *
     * @details On Windows the registration observes unhandled structured exceptions and then preserves normal OS crash
     * processing. On POSIX systems it handles fatal synchronous signals and re-raises them with their default action.
     * The emergency path performs no allocation, locking, symbolization, logging, or iostream operations. Destruction
     * restores every handler that was active before installation.
     *
     * Only one @c CrashHandler may be active in a process. Moving the registration transfers ownership without changing
     * the installed native handlers.
     */
    class CrashHandler {
    public:
        /** @brief Install the native crash handlers and acquire their process-wide ownership. */
        [[nodiscard]] static Result<CrashHandler> Install() noexcept;

        /** @brief Restore the handlers that preceded this registration. */
        ~CrashHandler();

        CrashHandler(const CrashHandler&) = delete;
        CrashHandler& operator=(const CrashHandler&) = delete;

        /** @brief Transfer ownership of an installed registration. */
        CrashHandler(CrashHandler&& other) noexcept;

        /** @brief Restore any currently owned registration, then transfer ownership from @p other. */
        CrashHandler& operator=(CrashHandler&& other) noexcept;

        /** @brief Return whether this object currently owns an installed process registration. */
        [[nodiscard]] explicit operator bool() const noexcept { return state_ != nullptr; }

    private:
        struct State;

        explicit CrashHandler(std::unique_ptr<State> state) noexcept;

        std::unique_ptr<State> state_{};
    };

} // namespace Sora

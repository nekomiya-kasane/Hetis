/**
 * @file CrashHandler.h
 * @brief RAII ownership of process-wide fatal handlers and allocation-free emergency crash output.
 * @details
 * Install once during process startup and retain the returned owner until orderly shutdown. An optional emergency file
 * is opened before native handlers are registered, so the fatal path performs no filesystem lookup or allocation:
 * @code{.cpp}
 * void WriteCrashState(const Sora::CrashContext& context, Sora::CrashStream stream) noexcept {
 *     static_cast<void>(stream.Write("Last submitted frame: "));
 *     static_cast<void>(stream.WriteUnsigned(CurrentFrameIndex()));
 *     static_cast<void>(stream.Write("\n"));
 * }
 *
 * Sora::CrashHandlerOptions options{
 *     .emergencyFile = "crash-emergency.txt",
 *     .callback = WriteCrashState,
 *     .mirrorToStandardError = true,
 * };
 * auto crashHandler = Sora::CrashHandler::Install(options);
 * if (!crashHandler) {
 *     HandleCrashHandlerError(crashHandler.error());
 * }
 * @endcode
 *
 * A callback executes in a corrupted-process context. It must not allocate, throw, lock, symbolize a stack, call the
 * logger, or access data that another thread may be mutating. Only prevalidated immutable state, lock-free atomics, and
 * @ref CrashStream operations are suitable.
 * @ingroup Core
 */
#pragma once

#include "Sora/Core/PAL/NativeCrashStream.h"
#include "Sora/ErrorCode.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace Sora {

    /** @brief Allocation-free PAL writer passed to application crash callbacks. */
    using CrashStream = PAL::NativeCrashStream;

    /** @brief Native crash metadata available without allocation or symbol resolution. */
    struct CrashContext {
        std::string_view reason;          /**< Static platform exception or signal name. */
        uint32_t code = 0;                /**< Native exception code or signal number. */
        uintptr_t faultAddress = 0;       /**< Faulting memory address when available. */
        uintptr_t instructionPointer = 0; /**< Instruction pointer extracted from native machine context. */
    };

    /** @brief Allocation-free function invoked after the built-in crash record has been written. */
    using CrashCallback = void (*)(const CrashContext& context, CrashStream stream) noexcept;

    /** @brief Configuration consumed synchronously while installing native crash handlers. */
    struct CrashHandlerOptions {
        std::filesystem::path emergencyFile{};   /**< Optional eagerly opened emergency dump destination. */
        CrashCallback callback = nullptr;        /**< Optional allocation-free application-state writer. */
        bool mirrorToStandardError = true;       /**< Also emit the built-in record to native standard error. */
        bool suppressSystemErrorDialogs = false; /**< Suppress interactive OS error dialogs while installed. */
    };

    /**
     * @brief Exclusive process-wide crash-handler registration.
     * @details Windows observes unhandled structured exceptions and preserves normal OS crash processing. POSIX
     * targets handle fatal synchronous signals and re-raise them with default disposition. Destruction restores every
     * handler that preceded installation and closes the emergency stream.
     * @note Sanitizer runs that need Sora's native crash record must configure the sanitizer runtime to pass native
     * faults through, for example with @c ASAN_OPTIONS=handle_segv=0.
     * @warning Destroy only during orderly single-threaded shutdown, when no fatal handler can be executing.
     */
    class CrashHandler {
    public:
        /** @brief Install handlers that write the built-in record to standard error. */
        [[nodiscard]] static Result<CrashHandler> Install() noexcept;

        /**
         * @brief Install handlers using @p options and acquire process-wide ownership.
         * @return The registration owner, or an error when ownership, allocation, emergency-file opening, or native
         * handler registration fails.
         */
        [[nodiscard]] static Result<CrashHandler> Install(const CrashHandlerOptions& options) noexcept;

        /** @brief Restore preceding native handlers and close the emergency stream. */
        ~CrashHandler();

        CrashHandler(const CrashHandler&) = delete;
        CrashHandler& operator=(const CrashHandler&) = delete;

        /** @brief Transfer ownership of an installed registration. */
        CrashHandler(CrashHandler&& other) noexcept;

        /** @brief Restore any owned registration, then transfer ownership from @p other. */
        CrashHandler& operator=(CrashHandler&& other) noexcept;

        /** @brief Return whether this object owns an installed process registration. */
        [[nodiscard]] explicit operator bool() const noexcept { return state_ != nullptr; }

    private:
        struct State;

        explicit CrashHandler(std::unique_ptr<State> state) noexcept;

        std::unique_ptr<State> state_{};
    };

} // namespace Sora

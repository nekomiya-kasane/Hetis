/**
 * @file CrashHandler.cpp
 * @brief Allocation-free native crash record and emergency-stream implementation.
 * @details Install and retain @ref Sora::CrashHandler as documented in @ref Sora::CrashHandlerOptions. This source
 * deliberately does not call stack symbolization, logging, formatting, allocation, or locking from a fatal handler.
 * @ingroup Core
 */

#include "Sora/Core/CrashHandler.h"
#include "Sora/Core/PAL/SystemAPI.h"
#include "Sora/Platform.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

#if defined(PLATFORM_WINDOWS)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <Windows.h>
#else
#    include <signal.h>
#    include <unistd.h>
#    include <ucontext.h>
#endif

namespace Sora {

    namespace {

#if defined(PLATFORM_WINDOWS)
        inline constexpr PAL::WindowsSystem::DWord kSuppressedErrorModeFlags =
            SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX;
        inline constexpr PAL::WindowsSystem::DWord kWerNoUiFlag = 32;
        inline constexpr PAL::WindowsSystem::HResult kWerFlagsNotInitialized =
            static_cast<PAL::WindowsSystem::HResult>(-2147023728);
#endif
#    if defined(__clang__)
#        if __has_feature(address_sanitizer)
        inline constexpr bool kInstallUnhandledExceptionFilter = false;
#        else
        inline constexpr bool kInstallUnhandledExceptionFilter = true;
#        endif
#    else
        inline constexpr bool kInstallUnhandledExceptionFilter = true;
#    endif

        struct CrashRuntime {
            const PAL::CrashSystemAPI* systemAPI = nullptr;
            CrashStream standardError{};
            CrashStream emergencyStream{};
            CrashCallback callback = nullptr;
            bool mirrorToStandardError = true;
            bool suppressSystemErrorDialogs = false;
#if defined(PLATFORM_WINDOWS)
            PAL::CrashSystemAPI::ExceptionFilterFunction previousFilter = nullptr;
#endif
        };

        std::atomic<bool> gCrashHandlerInstalled{false};
        std::atomic<CrashRuntime*> gCrashRuntime{nullptr};
        static_assert(std::atomic<CrashRuntime*>::is_always_lock_free);

        /** @brief Fixed-capacity emergency text builder suitable for a corrupted-process path. */
        class EmergencyBuffer {
        public:
            void Append(std::string_view text) noexcept {
                const size_t count = std::min(storage_.size() - size_, text.size());
                for (size_t index = 0; index < count; ++index) {
                    storage_[size_ + index] = text[index];
                }
                size_ += count;
            }

            void AppendHex(uintptr_t value) noexcept {
                constexpr std::string_view digits = "0123456789ABCDEF";
                std::array<char, 2 * sizeof(uintptr_t)> reversed{};
                size_t count = 0;
                do {
                    reversed[count++] = digits[value & 0xF];
                    value >>= 4;
                } while (value != 0 && count < reversed.size());
                Append("0x");
                while (count != 0) {
                    const char digit = reversed[--count];
                    Append(std::string_view{&digit, 1});
                }
            }

            void AppendUnsigned(uint64_t value) noexcept {
                std::array<char, std::numeric_limits<uint64_t>::digits10 + 1> reversed{};
                size_t count = 0;
                do {
                    reversed[count++] = static_cast<char>('0' + value % 10);
                    value /= 10;
                } while (value != 0);
                while (count != 0) {
                    const char digit = reversed[--count];
                    Append(std::string_view{&digit, 1});
                }
            }

            [[nodiscard]] std::string_view View() const noexcept { return {storage_.data(), size_}; }

        private:
            std::array<char, 1024> storage_{};
            size_t size_ = 0;
        };

        void WriteRecord(const CrashContext& context, const CrashRuntime* runtime) noexcept {
            EmergencyBuffer output;
            output.Append("\n=== SORA CRASH RECORD ===\nReason: ");
            output.Append(context.reason);
            output.Append("\nCode: ");
            output.AppendUnsigned(context.code);
            output.Append("\nFault address: ");
            output.AppendHex(context.faultAddress);
            output.Append("\nInstruction pointer: ");
            output.AppendHex(context.instructionPointer);
            output.Append("\n");

            const CrashStream standardError = runtime != nullptr ? runtime->standardError : CrashStream{};
            if (runtime == nullptr || runtime->mirrorToStandardError) {
                [[maybe_unused]] const bool written = standardError.Write(output.View());
            }
            if (runtime != nullptr && runtime->emergencyStream && runtime->emergencyStream != standardError) {
                [[maybe_unused]] const bool written = runtime->emergencyStream.Write(output.View());
            }

            if (runtime != nullptr && runtime->callback != nullptr) {
                const CrashStream callbackStream = runtime->emergencyStream ? runtime->emergencyStream : standardError;
                runtime->callback(context, callbackStream);
            }

            constexpr std::string_view footer = "=== END SORA CRASH RECORD ===\n";
            if (runtime == nullptr || runtime->mirrorToStandardError) {
                [[maybe_unused]] const bool written = standardError.Write(footer);
                standardError.Flush();
            }
            if (runtime != nullptr && runtime->emergencyStream && runtime->emergencyStream != standardError) {
                [[maybe_unused]] const bool written = runtime->emergencyStream.Write(footer);
                runtime->emergencyStream.Flush();
            }
        }

#if defined(PLATFORM_WINDOWS)
        [[nodiscard]] constexpr std::string_view ExceptionName(DWORD code) noexcept {
            switch (code) {
                case EXCEPTION_ACCESS_VIOLATION:
                    return "ACCESS_VIOLATION";
                case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
                    return "ARRAY_BOUNDS_EXCEEDED";
                case EXCEPTION_DATATYPE_MISALIGNMENT:
                    return "DATATYPE_MISALIGNMENT";
                case EXCEPTION_FLT_DIVIDE_BY_ZERO:
                    return "FLOAT_DIVIDE_BY_ZERO";
                case EXCEPTION_FLT_INVALID_OPERATION:
                    return "FLOAT_INVALID_OPERATION";
                case EXCEPTION_ILLEGAL_INSTRUCTION:
                    return "ILLEGAL_INSTRUCTION";
                case EXCEPTION_IN_PAGE_ERROR:
                    return "IN_PAGE_ERROR";
                case EXCEPTION_INT_DIVIDE_BY_ZERO:
                    return "INTEGER_DIVIDE_BY_ZERO";
                case EXCEPTION_INT_OVERFLOW:
                    return "INTEGER_OVERFLOW";
                case EXCEPTION_STACK_OVERFLOW:
                    return "STACK_OVERFLOW";
                default:
                    return "UNKNOWN_STRUCTURED_EXCEPTION";
            }
        }

        [[nodiscard]] uintptr_t InstructionPointer(const CONTEXT* context) noexcept {
            if (context == nullptr) {
                return 0;
            }
#    if defined(_M_X64) || defined(__x86_64__)
            return static_cast<uintptr_t>(context->Rip);
#    elif defined(_M_IX86) || defined(__i386__)
            return static_cast<uintptr_t>(context->Eip);
#    elif defined(_M_ARM64) || defined(__aarch64__)
            return static_cast<uintptr_t>(context->Pc);
#    else
            return 0;
#    endif
        }


        LONG WINAPI HandleUnhandledException(EXCEPTION_POINTERS* exception) noexcept {
            const EXCEPTION_RECORD* record = exception != nullptr ? exception->ExceptionRecord : nullptr;
            const DWORD code = record != nullptr ? record->ExceptionCode : 0;
            const CrashContext context{
                .reason = ExceptionName(code),
                .code = code,
                .faultAddress = record != nullptr ? reinterpret_cast<uintptr_t>(record->ExceptionAddress) : 0,
                .instructionPointer = InstructionPointer(exception != nullptr ? exception->ContextRecord : nullptr)};
            const CrashRuntime* runtime = gCrashRuntime.load(std::memory_order_acquire);
            WriteRecord(context, runtime);
            if (runtime != nullptr && runtime->suppressSystemErrorDialogs) {
                return EXCEPTION_EXECUTE_HANDLER;
            }
            if (runtime != nullptr && runtime->previousFilter != nullptr) {
                return runtime->previousFilter(exception);
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }
#else
        [[nodiscard]] constexpr std::string_view SignalName(int signalNumber) noexcept {
            switch (signalNumber) {
                case SIGABRT:
                    return "SIGABRT";
                case SIGFPE:
                    return "SIGFPE";
                case SIGILL:
                    return "SIGILL";
                case SIGSEGV:
                    return "SIGSEGV";
#    ifdef SIGBUS
                case SIGBUS:
                    return "SIGBUS";
#    endif
                default:
                    return "UNKNOWN_SIGNAL";
            }
        }

        [[nodiscard]] uintptr_t InstructionPointer(const void* nativeContext) noexcept {
            if (nativeContext == nullptr) {
                return 0;
            }
            const auto* context = static_cast<const ucontext_t*>(nativeContext);
#    if defined(PLATFORM_MACOS) && defined(__x86_64__)
            return static_cast<uintptr_t>(context->uc_mcontext->__ss.__rip);
#    elif defined(PLATFORM_MACOS) && defined(__aarch64__)
            return static_cast<uintptr_t>(context->uc_mcontext->__ss.__pc);
#    elif defined(__x86_64__) && defined(REG_RIP)
            return static_cast<uintptr_t>(context->uc_mcontext.gregs[REG_RIP]);
#    elif defined(__i386__) && defined(REG_EIP)
            return static_cast<uintptr_t>(context->uc_mcontext.gregs[REG_EIP]);
#    elif defined(__aarch64__)
            return static_cast<uintptr_t>(context->uc_mcontext.pc);
#    else
            return 0;
#    endif
        }

        void HandleFatalSignal(int signalNumber, siginfo_t* signalInfo, void* nativeContext) noexcept {
            const CrashContext context{
                .reason = SignalName(signalNumber),
                .code = static_cast<uint32_t>(signalNumber),
                .faultAddress = signalInfo != nullptr ? reinterpret_cast<uintptr_t>(signalInfo->si_addr) : 0,
                .instructionPointer = InstructionPointer(nativeContext),
            };
            const CrashRuntime* runtime = gCrashRuntime.load(std::memory_order_acquire);
            WriteRecord(context, runtime);

            if (runtime != nullptr && runtime->systemAPI != nullptr && runtime->systemAPI->raiseSignal != nullptr &&
                runtime->systemAPI->immediateExit != nullptr && runtime->systemAPI->raiseSignal(signalNumber) != 0) {
                runtime->systemAPI->immediateExit(128 + signalNumber);
            }
        }
#endif

    } // namespace

    struct CrashHandler::State {
        CrashRuntime runtime{};
        PAL::OwnedNativeCrashStream emergencyStream{};
#if defined(PLATFORM_WINDOWS)
        PAL::CrashSystemAPI::ExceptionFilterFunction previousFilter = nullptr;
        bool nativeHandlerInstalled = false;
        PAL::WindowsSystem::DWord addedErrorModeFlags = 0;
        PAL::WindowsSystem::DWord addedWerFlags = 0;
#else
        static constexpr size_t kSignalCapacity = 5;

        std::array<int, kSignalCapacity> signals{};
        std::array<struct sigaction, kSignalCapacity> previousActions{};
        size_t installedCount = 0;
#endif

        ~State() noexcept {
#if defined(PLATFORM_WINDOWS)
            if (nativeHandlerInstalled && runtime.systemAPI != nullptr &&
                runtime.systemAPI->setUnhandledExceptionFilter != nullptr) {
                runtime.systemAPI->setUnhandledExceptionFilter(previousFilter);
            }
            if (addedErrorModeFlags != 0 && runtime.systemAPI != nullptr &&
                runtime.systemAPI->getErrorMode != nullptr && runtime.systemAPI->setErrorMode != nullptr) {
                const PAL::WindowsSystem::DWord currentMode = runtime.systemAPI->getErrorMode();
                runtime.systemAPI->setErrorMode(currentMode & ~addedErrorModeFlags);
            }
            if (addedWerFlags != 0 && runtime.systemAPI != nullptr && runtime.systemAPI->getCurrentProcess != nullptr &&
                runtime.systemAPI->werGetFlags != nullptr && runtime.systemAPI->werSetFlags != nullptr) {
                PAL::WindowsSystem::DWord currentFlags = 0;
                if (PAL::WindowsSystem::Succeeded(
                        runtime.systemAPI->werGetFlags(runtime.systemAPI->getCurrentProcess(), &currentFlags))) {
                    runtime.systemAPI->werSetFlags(currentFlags & ~addedWerFlags);
                }
            }
#else
            while (installedCount != 0 && runtime.systemAPI != nullptr && runtime.systemAPI->signalAction != nullptr) {
                --installedCount;
                runtime.systemAPI->signalAction(signals[installedCount], &previousActions[installedCount], nullptr);
            }
#endif
            gCrashRuntime.store(nullptr, std::memory_order_release);
            gCrashHandlerInstalled.store(false, std::memory_order_release);
        }
    };

    Result<CrashHandler> CrashHandler::Install() noexcept {
        return Install(CrashHandlerOptions{});
    }

    Result<CrashHandler> CrashHandler::Install(const CrashHandlerOptions& options) noexcept {
        bool expected = false;
        if (!gCrashHandlerInstalled.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return std::unexpected(ErrorCode::InvalidState);
        }

        std::unique_ptr<State> state;
        try {
            state = std::make_unique<State>();
        } catch (...) {
            gCrashHandlerInstalled.store(false, std::memory_order_release);
            return std::unexpected(ErrorCode::OutOfMemory);
        }

        state->runtime.callback = options.callback;
        state->runtime.mirrorToStandardError = options.mirrorToStandardError;
        const PAL::CrashSystemAPI& systemAPI = PAL::LoadCrashSystemAPI();
        state->runtime.suppressSystemErrorDialogs = options.suppressSystemErrorDialogs;
        state->runtime.systemAPI = &systemAPI;
        state->runtime.standardError = PAL::NativeStandardErrorStream();
        if (!options.emergencyFile.empty()) {
            state->emergencyStream = PAL::OwnedNativeCrashStream::OpenTruncated(options.emergencyFile);
            if (!state->emergencyStream) {
                return std::unexpected(ErrorCode::EmergencyPathInvalid);
            }
            state->runtime.emergencyStream = state->emergencyStream.View();
        }
        gCrashRuntime.store(&state->runtime, std::memory_order_release);

#if defined(PLATFORM_WINDOWS)
        const bool nativeHandlerUnavailable =
            kInstallUnhandledExceptionFilter && systemAPI.setUnhandledExceptionFilter == nullptr;
        const bool dialogControlUnavailable =
            options.suppressSystemErrorDialogs &&
            (systemAPI.getErrorMode == nullptr || systemAPI.setErrorMode == nullptr ||
             systemAPI.getCurrentProcess == nullptr || systemAPI.werGetFlags == nullptr ||
             systemAPI.werSetFlags == nullptr);
        if (nativeHandlerUnavailable || dialogControlUnavailable) {
            return std::unexpected(ErrorCode::CrashHandlerInstallFailed);
        }
        if (options.suppressSystemErrorDialogs) {
            const PAL::WindowsSystem::DWord currentMode = systemAPI.getErrorMode();
            state->addedErrorModeFlags = kSuppressedErrorModeFlags & ~currentMode;
            if (state->addedErrorModeFlags != 0) {
                systemAPI.setErrorMode(currentMode | state->addedErrorModeFlags);
            }
            PAL::WindowsSystem::DWord currentWerFlags = 0;
            const PAL::WindowsSystem::HResult werGetResult =
                systemAPI.werGetFlags(systemAPI.getCurrentProcess(), &currentWerFlags);
            if (!PAL::WindowsSystem::Succeeded(werGetResult) && werGetResult != kWerFlagsNotInitialized) {
                return std::unexpected(ErrorCode::CrashHandlerInstallFailed);
            }
            state->addedWerFlags = kWerNoUiFlag & ~currentWerFlags;
            if (state->addedWerFlags != 0 &&
                !PAL::WindowsSystem::Succeeded(systemAPI.werSetFlags(currentWerFlags | state->addedWerFlags))) {
                return std::unexpected(ErrorCode::CrashHandlerInstallFailed);
            }
        }
        if constexpr (kInstallUnhandledExceptionFilter) {
            state->previousFilter = systemAPI.setUnhandledExceptionFilter(HandleUnhandledException);
            state->runtime.previousFilter = state->previousFilter;
            state->nativeHandlerInstalled = true;
        }
#else
        constexpr std::array fatalSignals = {
            SIGABRT, SIGFPE, SIGILL, SIGSEGV,
#    ifdef SIGBUS
            SIGBUS,
#    endif
        };

        if (systemAPI.signalAction == nullptr || systemAPI.emptySignalSet == nullptr ||
            systemAPI.raiseSignal == nullptr || systemAPI.immediateExit == nullptr) {
            return std::unexpected(ErrorCode::CrashHandlerInstallFailed);
        }
        struct sigaction action{};
        systemAPI.emptySignalSet(reinterpret_cast<PAL::PosixSystem::SignalSet*>(&action.sa_mask));
        action.sa_flags = SA_SIGINFO | SA_RESETHAND;
        action.sa_sigaction = HandleFatalSignal;

        for (int signalNumber : fatalSignals) {
            const size_t index = state->installedCount;
            if (systemAPI.signalAction(signalNumber, &action, &state->previousActions[index]) != 0) {
                state.reset();
                return std::unexpected(ErrorCode::CrashHandlerInstallFailed);
            }
            state->signals[index] = signalNumber;
            ++state->installedCount;
        }
#endif

        return CrashHandler{std::move(state)};
    }

    CrashHandler::CrashHandler(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    CrashHandler::~CrashHandler() = default;

    CrashHandler::CrashHandler(CrashHandler&& other) noexcept = default;

    CrashHandler& CrashHandler::operator=(CrashHandler&& other) noexcept = default;

} // namespace Sora

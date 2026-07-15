/**
 * @file CrashHandler.cpp
 * @brief Allocation-free process crash-handler implementation for Windows and POSIX targets.
 * @ingroup Core
 */

#include "Sora/Core/CrashHandler.h"
#include "Sora/Platform.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
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
#    include <csignal>
#    include <unistd.h>
#endif

namespace Sora {

    namespace {

        std::atomic<bool> gCrashHandlerInstalled{false};

        /** @brief Fixed-capacity emergency text builder suitable for a corrupted-process path. */
        class EmergencyBuffer {
        public:
            void Append(std::string_view text) noexcept {
                const size_t available = storage_.size() - size_;
                const size_t count = std::min(available, text.size());
                for (size_t i = 0; i < count; ++i) {
                    storage_[size_ + i] = text[i];
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
                    char digit = reversed[--count];
                    Append(std::string_view{&digit, 1});
                }
            }

            [[nodiscard]] std::string_view View() const noexcept { return {storage_.data(), size_}; }

        private:
            std::array<char, 512> storage_{};
            size_t size_ = 0;
        };

#if defined(PLATFORM_WINDOWS)
        void WriteEmergency(std::string_view text) noexcept {
            HANDLE stream = ::GetStdHandle(STD_ERROR_HANDLE);
            if (stream != nullptr && stream != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                ::WriteFile(stream, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
            }
        }

        LONG WINAPI HandleUnhandledException(EXCEPTION_POINTERS* exception) noexcept {
            EmergencyBuffer output;
            output.Append("Sora fatal structured exception ");
            if (exception != nullptr && exception->ExceptionRecord != nullptr) {
                output.AppendHex(exception->ExceptionRecord->ExceptionCode);
                output.Append(" at ");
                output.AppendHex(reinterpret_cast<uintptr_t>(exception->ExceptionRecord->ExceptionAddress));
            } else {
                output.Append("with unavailable exception metadata");
            }
            output.Append("\n");
            WriteEmergency(output.View());
            return EXCEPTION_CONTINUE_SEARCH;
        }
#else
        void WriteEmergency(std::string_view text) noexcept {
            while (!text.empty()) {
                const ssize_t written = ::write(STDERR_FILENO, text.data(), text.size());
                if (written > 0) {
                    text.remove_prefix(static_cast<size_t>(written));
                    continue;
                }
                if (written < 0 && errno == EINTR) {
                    continue;
                }
                return;
            }
        }

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
                    return "unknown signal";
            }
        }

        void HandleFatalSignal(int signalNumber, siginfo_t* signalInfo, void*) noexcept {
            EmergencyBuffer output;
            output.Append("Sora fatal signal ");
            output.Append(SignalName(signalNumber));
            if (signalInfo != nullptr) {
                output.Append(" at ");
                output.AppendHex(reinterpret_cast<uintptr_t>(signalInfo->si_addr));
            }
            output.Append("\n");
            WriteEmergency(output.View());

            if (::raise(signalNumber) != 0) {
                ::_exit(128 + signalNumber);
            }
        }
#endif

    } // namespace

    struct CrashHandler::State {
#if defined(PLATFORM_WINDOWS)
        LPTOP_LEVEL_EXCEPTION_FILTER previousFilter = nullptr;
#else
        static constexpr size_t kSignalCapacity = 5;

        std::array<int, kSignalCapacity> signals{};
        std::array<struct sigaction, kSignalCapacity> previousActions{};
        size_t installedCount = 0;
#endif

        ~State() noexcept {
#if defined(PLATFORM_WINDOWS)
            ::SetUnhandledExceptionFilter(previousFilter);
#else
            while (installedCount != 0) {
                --installedCount;
                ::sigaction(signals[installedCount], &previousActions[installedCount], nullptr);
            }
#endif
            gCrashHandlerInstalled.store(false, std::memory_order_release);
        }
    };

    Result<CrashHandler> CrashHandler::Install() noexcept {
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

#if defined(PLATFORM_WINDOWS)
        state->previousFilter = ::SetUnhandledExceptionFilter(HandleUnhandledException);
#else
        constexpr std::array fatalSignals = {
            SIGABRT, SIGFPE, SIGILL, SIGSEGV,
#    ifdef SIGBUS
            SIGBUS,
#    endif
        };

        struct sigaction action{};
        ::sigemptyset(&action.sa_mask);
        action.sa_flags = SA_SIGINFO | SA_RESETHAND;
        action.sa_sigaction = HandleFatalSignal;

        for (int signalNumber : fatalSignals) {
            const size_t index = state->installedCount;
            if (::sigaction(signalNumber, &action, &state->previousActions[index]) != 0) {
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

/**
 * @file Diagnostics.cpp
 * @brief Portable debugger primitives and synchronous assertion reporting implementation.
 * @ingroup Core
 */

#include "Sora/Platform.h"
#include "Sora/Core/Assertion.h"
#include "Sora/Core/Debugging.h"
#include "Sora/Core/StackTrace.h"
#include "Sora/Core/ToString.h"

#include <array>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <system_error>
#include <utility>

#if __has_include(<debugging>)
#    include <debugging>
#endif

#if defined(PLATFORM_WINDOWS)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <Windows.h>
#elif defined(PLATFORM_LINUX)
#    include <csignal>
#    include <cstring>
#elif defined(PLATFORM_MACOS)
#    include <csignal>
#    include <sys/sysctl.h>
#    include <unistd.h>
#endif

namespace Sora::Debug {

    bool IsDebuggerPresent() noexcept {
#if defined(__cpp_lib_debugging) && __cpp_lib_debugging >= 202311L
        return std::is_debugger_present();
#elif defined(PLATFORM_WINDOWS)
        return ::IsDebuggerPresent() != FALSE;
#elif defined(PLATFORM_LINUX)
        std::FILE* status = std::fopen("/proc/self/status", "r");
        if (status == nullptr) {
            return false;
        }

        std::array<char, 256> line{};
        bool attached = false;
        while (std::fgets(line.data(), static_cast<int>(line.size()), status) != nullptr) {
            constexpr std::string_view prefix = "TracerPid:";
            if (std::strncmp(line.data(), prefix.data(), prefix.size()) != 0) {
                continue;
            }
            const char* first = line.data() + prefix.size();
            while (*first == ' ' || *first == '\t') {
                ++first;
            }
            const char* last = line.data() + std::strlen(line.data());
            int tracerPid = 0;
            if (std::from_chars(first, last, tracerPid).ec == std::errc{}) {
                attached = tracerPid != 0;
            }
            break;
        }
        std::fclose(status);
        return attached;
#elif defined(PLATFORM_MACOS)
        int query[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(::getpid())};
        kinfo_proc process{};
        size_t size = sizeof(process);
        if (::sysctl(query, 4, &process, &size, nullptr, 0) != 0) {
            return false;
        }
        return (process.kp_proc.p_flag & P_TRACED) != 0;
#else
        return false;
#endif
    }

    void Breakpoint() noexcept {
#if defined(__cpp_lib_debugging) && __cpp_lib_debugging >= 202311L
        std::breakpoint();
#elif defined(PLATFORM_WINDOWS)
        ::DebugBreak();
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        std::raise(SIGTRAP);
#endif
    }

    void BreakpointIfDebuggerPresent() noexcept {
        if (IsDebuggerPresent()) {
            Breakpoint();
        }
    }

} // namespace Sora::Debug

namespace Sora {

    namespace {

        [[nodiscard]] constexpr uint8_t EncodeSettings(AssertionSettings settings) noexcept {
            const uint8_t action = static_cast<uint8_t>(settings.action);
            return static_cast<uint8_t>(action | (settings.captureStackTrace ? 0x80 : 0));
        }

        [[nodiscard]] constexpr AssertionSettings DecodeSettings(uint8_t value) noexcept {
            return {.action = static_cast<AssertionAction>(value & 0x7F), .captureStackTrace = (value & 0x80) != 0};
        }

        constexpr AssertionSettings kDefaultAssertionSettings{};
        std::atomic<uint8_t> gAssertionSettings{EncodeSettings(kDefaultAssertionSettings)};
        std::atomic<AssertionReporter> gAssertionReporter{nullptr};

        void WriteStderr(std::string_view text) noexcept {
            std::fwrite(text.data(), 1, text.size(), stderr);
            std::fflush(stderr);
        }

        void DefaultAssertionReporter(const AssertionFailure& failure) noexcept {
            try {
                std::string output;
                output.reserve(512 + failure.message.size());
                const std::string_view message =
                    failure.message.empty() ? std::string_view{"<none>"} : std::string_view{failure.message};
                output += std::format("\n{} failed: {}\n  Location: {}:{} in {}\n  Message: {}\n",
                                      ToString(failure.kind), failure.condition, failure.source.file_name(),
                                      failure.source.line(), failure.source.function_name(), message);
                if (!failure.stackTrace.empty()) {
                    output += std::format("StackTrace({} frames)", failure.stackTrace.size());
                    for (size_t i = 0; i < failure.stackTrace.size(); ++i) {
                        output += "\n  ";
                        output += StackTrace::FormatFrame(failure.stackTrace[i], i);
                    }
                    output.push_back('\n');
                }
                WriteStderr(output);
            } catch (...) {
                WriteStderr("\nSora assertion reporting failed while materializing the diagnostic.\n");
            }
        }

        void ApplyAssertionAction(AssertionAction action) noexcept {
            switch (action) {
                case AssertionAction::Continue:
                    return;
                case AssertionAction::Break:
                    Debug::Breakpoint();
                    return;
                case AssertionAction::BreakIfDebuggerOtherwiseAbort:
                    if (Debug::IsDebuggerPresent()) {
                        Debug::Breakpoint();
                        return;
                    }
                    std::abort();
                case AssertionAction::Abort:
                    std::abort();
            }
            std::abort();
        }

    } // namespace

    AssertionSettings SetAssertionSettings(AssertionSettings settings) noexcept {
        const uint8_t previous = gAssertionSettings.exchange(EncodeSettings(settings), std::memory_order_acq_rel);
        return DecodeSettings(previous);
    }

    AssertionSettings GetAssertionSettings() noexcept {
        return DecodeSettings(gAssertionSettings.load(std::memory_order_acquire));
    }

    AssertionReporter SetAssertionReporter(AssertionReporter reporter) noexcept {
        return gAssertionReporter.exchange(reporter, std::memory_order_acq_rel);
    }

    AssertionReporter GetAssertionReporter() noexcept {
        return gAssertionReporter.load(std::memory_order_acquire);
    }

    namespace Detail {

        void ReportAssertionFailure(AssertionKind kind, std::string_view condition, std::string message,
                                    std::source_location source) noexcept {
            thread_local bool reporting = false;
            if (reporting) {
                WriteStderr("\nRecursive Sora assertion failure; aborting without further diagnostics.\n");
                std::abort();
            }
            reporting = true;

            const AssertionSettings settings = GetAssertionSettings();
            StackTrace stackTrace;
            if (settings.captureStackTrace) {
                try {
                    stackTrace = StackTrace::Capture(2, 32);
                } catch (...) {
                    stackTrace = {};
                }
            }
            AssertionFailure failure{.kind = kind,
                                     .condition = condition,
                                     .message = std::move(message),
                                     .source = source,
                                     .stackTrace = stackTrace.Frames()};
            AssertionReporter reporter = GetAssertionReporter();
            (reporter != nullptr ? reporter : DefaultAssertionReporter)(failure);

            reporting = false;
            ApplyAssertionAction(settings.action);
        }

    } // namespace Detail

} // namespace Sora

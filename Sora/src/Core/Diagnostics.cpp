/**
 * @file Diagnostics.cpp
 * @brief Portable debugger primitives and synchronous assertion reporting implementation.
 * @ingroup Core
 */

#include "Sora/Core/Assertion.h"
#include "Sora/Core/Debugging.h"
#include "Sora/Core/StackTrace.h"
#include "Sora/Core/ToString.h"
#include "Sora/Core/ToStyledString.h"
#include <Sora/Core/PAL/SystemAPI.h>

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

namespace Sora::Debug {

    bool IsDebuggerPresent() noexcept {
#if defined(__cpp_lib_debugging) && __cpp_lib_debugging >= 202311L
        return std::is_debugger_present();
#elif defined(PLATFORM_WINDOWS)
        const PAL::DebugSystemAPI& api = PAL::LoadDebugSystemAPI();
        return api.isDebuggerPresent != nullptr && api.isDebuggerPresent() != 0;
#elif defined(PLATFORM_LINUX)
        const PAL::FileSystemAPI& file = PAL::LoadFileSystemAPI();
        if (!PAL::EnsureSystemAPIs(file.open, file.read, file.close)) {
            return false;
        }
        const int descriptor = file.open("/proc/self/status", PAL::PosixSystem::kOpenReadOnly);
        if (descriptor < 0) {
            return false;
        }
        std::array<char, 4096> text{};
        const Sora::ssize_t size = file.read(descriptor, text.data(), text.size());
        const bool closed = file.close(descriptor) == 0;
        if (size <= 0 || !closed) {
            return false;
        }
        constexpr std::string_view prefix = "TracerPid:";
        const std::string_view status{text.data(), static_cast<size_t>(size)};
        const size_t prefixOffset = status.find(prefix);
        if (prefixOffset == std::string_view::npos) {
            return false;
        }
        const size_t valueOffset = status.find_first_not_of(" \t", prefixOffset + prefix.size());
        if (valueOffset == std::string_view::npos) {
            return false;
        }
        const size_t valueEnd = status.find_first_of("\r\n", valueOffset);
        int tracerId = 0;
        const char* first = text.data() + valueOffset;
        const char* last = text.data() + (valueEnd == std::string_view::npos ? status.size() : valueEnd);
        return std::from_chars(first, last, tracerId).ec == std::errc{} && tracerId != 0;
#else
        return false;
#endif
    }

    void Breakpoint() noexcept {
#if defined(PLATFORM_WINDOWS)
        const PAL::DebugSystemAPI& api = PAL::LoadDebugSystemAPI();
        if (api.breakpoint != nullptr) {
            api.breakpoint();
        }
#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
        const PAL::ProcessControlSystemAPI& api = PAL::LoadProcessControlSystemAPI();
        if (api.raiseSignal != nullptr) {
            api.raiseSignal(PAL::PosixSystem::kBreakpointSignal);
        }
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

        [[nodiscard]] std::string FormatStyledFailure(const AssertionFailure& failure) {
            namespace StyledOutput = Styled;
            StyledOutput::StyledStringBuilder builder;
            builder.Raw("\n");
            builder.Text(StyledOutput::StyledRole::Error, ToString(failure.kind));
            builder.Raw(StyledOutput::StyledRole::Error, " failed");
            if (!failure.condition.empty()) {
                builder.Raw(StyledOutput::StyledRole::Punctuation, ": ");
                builder.Text(StyledOutput::StyledRole::Error, failure.condition);
            }

            builder.Raw("\n  ");
            builder.Text(StyledOutput::StyledRole::FieldName, "Location");
            builder.Raw(StyledOutput::StyledRole::Punctuation, ": ");
            builder.Text(StyledOutput::StyledRole::String, failure.source.file_name());
            builder.Raw(StyledOutput::StyledRole::Punctuation, ":");
            builder.Text(StyledOutput::StyledRole::Number, std::format("{}", failure.source.line()));
            builder.Raw(StyledOutput::StyledRole::Plain, " in ");
            builder.Text(StyledOutput::StyledRole::TypeName, failure.source.function_name());

            builder.Raw("\n  ");
            builder.Text(StyledOutput::StyledRole::FieldName, "Message");
            builder.Raw(StyledOutput::StyledRole::Punctuation, ": ");
            if (failure.message.empty()) {
                builder.Text(StyledOutput::StyledRole::Null, "<none>");
            } else {
                builder.Text(StyledOutput::StyledRole::Plain, failure.message);
            }

            if (failure.stackTrace != nullptr && !failure.stackTrace->Empty()) {
                builder.Raw("\n");
                failure.stackTrace->ToStyledString(builder);
            }

            std::string output = std::move(builder).Finish();
            output.push_back('\n');
            return output;
        }

        [[nodiscard]] std::string FormatPlainFailure(const AssertionFailure& failure) {
            const std::string_view message =
                failure.message.empty() ? std::string_view{"<none>"} : std::string_view{failure.message};
            std::string output = std::format("\n{} failed", ToString(failure.kind));
            if (!failure.condition.empty()) {
                output += std::format(": {}", failure.condition);
            }
            output += std::format("\n  Location: {}:{} in {}\n  Message: {}\n", failure.source.file_name(),
                                  failure.source.line(), failure.source.function_name(), message);
            if (failure.stackTrace != nullptr && !failure.stackTrace->Empty()) {
                output += failure.stackTrace->ToString();
                output.push_back('\n');
            }
            return output;
        }

        void DefaultAssertionReporter(const AssertionFailure& failure) noexcept {
            try {
                WriteStderr(FormatStyledFailure(failure));
            } catch (...) {
                try {
                    WriteStderr(FormatPlainFailure(failure));
                } catch (...) {
                    WriteStderr("\nSora assertion reporting failed while materializing the diagnostic.\n");
                }
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
                                    std::source_location source, AssertionDisposition disposition) noexcept {
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
                    stackTrace = StackTrace::Capture({.skipFrames = 2, .maxFrames = 32});
                } catch (...) {
                    stackTrace = {};
                }
            }
            AssertionFailure failure{.kind = kind,
                                     .condition = condition,
                                     .message = std::move(message),
                                     .source = source,
                                     .stackTrace = stackTrace.Empty() ? nullptr : &stackTrace};
            AssertionReporter reporter = GetAssertionReporter();
            (reporter != nullptr ? reporter : DefaultAssertionReporter)(failure);

            reporting = false;
            if (disposition == AssertionDisposition::ApplyConfiguredAction) {
                ApplyAssertionAction(settings.action);
            }
        }

    } // namespace Detail

} // namespace Sora

/**
 * @file StructuredLogger.h
 * @brief Structured diagnostics logger with typed records, value sinks, and compile-time level elimination.
 * @ingroup Core
 */
#pragma once

#include "Sora/Core/LogCategory.h"
#include "Sora/Core/ToString.h"
#include "Sora/Core/ToStyledString.h"
#include "Sora/Core/Traits/EnumTraits.h"
#include "Sora/ErrorCode.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <functional>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace Sora {

    /** @brief Diagnostic severity ordered from most verbose to most severe. */
    enum class LogLevel : uint8_t {
        Trace = 0, /**< Finest-grained execution diagnostics. */
        Debug = 1, /**< Debug-time state useful while developing or investigating a subsystem. */
        Info = 2,  /**< Normal operational information. */
        Warn = 3,  /**< Suspicious condition that did not prevent the current operation from continuing. */
        Error = 4, /**< Recoverable failure. */
        Fatal = 5, /**< Unrecoverable failure; callers normally abort or start crash handling after logging. */
    };

#ifndef SORA_MIN_LOG_LEVEL
#    ifdef NDEBUG
    inline constexpr LogLevel kMinLogLevel = LogLevel::Info;
#    else
    inline constexpr LogLevel kMinLogLevel = LogLevel::Trace;
#    endif
#else
    inline constexpr LogLevel kMinLogLevel = static_cast<LogLevel>(SORA_MIN_LOG_LEVEL);
#endif

    /** @brief Source location normalised to stable string views and 32-bit line/column coordinates. */
    struct SourceLocation {
        std::string_view file;     /**< Full source file path as reported by the compiler. */
        std::string_view function; /**< Function spelling as reported by the compiler. */
        uint32_t line = 0;         /**< 1-based line number. */
        uint32_t column = 0;       /**< 1-based column number when the compiler provides one. */

        /** @brief Capture the caller's current source location. */
        [[nodiscard]] static constexpr SourceLocation
        Current(std::source_location location = std::source_location::current()) noexcept {
            return {location.file_name(), location.function_name(), location.line(), location.column()};
        }

        /** @brief Return the filename component without directories. */
        [[nodiscard]] std::string_view FileName() const noexcept;
    };

    /** @brief Wrapper whose default constructor captures the call site in APIs where a parameter pack follows. */
    struct CallerLocation {
        SourceLocation location; /**< Captured call-site location. */

        /** @brief Capture the caller's location. */
        constexpr CallerLocation(std::source_location loc = std::source_location::current()) noexcept
            : location{loc.file_name(), loc.function_name(), loc.line(), loc.column()} {}
    };

    /** @brief One fully materialised log record passed to sinks. */
    struct LogRecord {
        LogLevel level = LogLevel::Info;                   /**< Severity. */
        LogCategory category = LogCategory::Core;          /**< Semantic domain. */
        SourceLocation source{};                           /**< Source call site. */
        std::chrono::system_clock::time_point timestamp{}; /**< Wall-clock timestamp. */
        uint64_t threadId = 0;                             /**< Process-local operating-system thread identifier. */
        std::string message;                               /**< Owning formatted message payload. */
    };

    /** @brief Console sink writing human-readable records to @c stderr. */
    class ConsoleLogSink {
    public:
        /** @brief Construct a console sink. */
        explicit constexpr ConsoleLogSink(bool styled = true) noexcept : styled_(styled) {}

        /** @brief Write one record. */
        void Write(const LogRecord& record);

        /** @brief Flush the underlying C stream. */
        void Flush() noexcept;

    private:
        bool styled_ = true;
    };

    /** @brief Plain text file sink. */
    class FileLogSink {
    public:
        /** @brief Open @p path in append mode. */
        explicit FileLogSink(std::filesystem::path path);

        /** @brief Close the file if it is open. */
        ~FileLogSink();

        FileLogSink(FileLogSink&& other) noexcept;
        FileLogSink& operator=(FileLogSink&& other) noexcept;
        FileLogSink(const FileLogSink&) = delete;
        FileLogSink& operator=(const FileLogSink&) = delete;

        /** @brief Return whether the file handle is open. */
        [[nodiscard]] bool IsOpen() const noexcept { return file_ != nullptr; }

        /** @brief Return the configured path. */
        [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

        /** @brief Write one record when the file is open. */
        void Write(const LogRecord& record);

        /** @brief Flush the file handle when open. */
        void Flush() noexcept;

    private:
        std::filesystem::path path_{};
        std::FILE* file_ = nullptr;
    };

    /** @brief Newline-delimited JSON file sink. */
    class JsonLogSink {
    public:
        /** @brief Open @p path in append mode. */
        explicit JsonLogSink(std::filesystem::path path);

        /** @brief Close the file if it is open. */
        ~JsonLogSink();

        JsonLogSink(JsonLogSink&& other) noexcept;
        JsonLogSink& operator=(JsonLogSink&& other) noexcept;
        JsonLogSink(const JsonLogSink&) = delete;
        JsonLogSink& operator=(const JsonLogSink&) = delete;

        /** @brief Return whether the file handle is open. */
        [[nodiscard]] bool IsOpen() const noexcept { return file_ != nullptr; }

        /** @brief Write one NDJSON record when the file is open. */
        void Write(const LogRecord& record);

        /** @brief Flush the file handle when open. */
        void Flush() noexcept;

    private:
        std::filesystem::path path_{};
        std::FILE* file_ = nullptr;
    };

    /** @brief Owning callback sink for bridging logs to user code, tests, or external telemetry. */
    class CallbackLogSink {
    public:
        using Callback = std::function<void(const LogRecord&)>;

        /** @brief Construct from an owning callback. */
        explicit CallbackLogSink(Callback callback) : callback_(std::move(callback)) {}

        /** @brief Invoke the callback when present. */
        void Write(const LogRecord& record);

        /** @brief Callback sinks have no buffered state. */
        void Flush() noexcept {}

    private:
        Callback callback_{};
    };

    /** @brief Value sink variant; dispatch uses @c std::visit and does not require virtual inheritance. */
    using LogSink = std::variant<ConsoleLogSink, FileLogSink, JsonLogSink, CallbackLogSink>;

    /** @brief Producer behavior when the thread-local log ring cannot accept another record. */
    enum class LogBackpressurePolicy : uint8_t {
        Drop, /**< Drop the record and increment @ref StructuredLogger::DroppedCount. */
        Block /**< Yield until the ring accepts the record. */
    };

    /** @brief Process-local structured logger. */
    class StructuredLogger {
    public:
        /** @brief Return the default process logger. */
        [[nodiscard]] static StructuredLogger& Default() noexcept;

        /** @brief Construct a logger with built-in category thresholds. */
        StructuredLogger();

        /** @brief Stop the drain thread and flush buffered records. */
        ~StructuredLogger();

        StructuredLogger(const StructuredLogger&) = delete;
        StructuredLogger& operator=(const StructuredLogger&) = delete;

        /** @brief Add a sink. */
        void AddSink(LogSink sink);

        /** @brief Remove all sinks. */
        void ClearSinks();

        /** @brief Set the runtime threshold for @p category. */
        void SetCategoryLevel(LogCategory category, LogLevel level) noexcept;

        /** @brief Return the runtime threshold for @p category. */
        [[nodiscard]] LogLevel GetCategoryLevel(LogCategory category) const noexcept;

        /** @brief Return true when @p level and @p category pass compile-time and runtime filters. */
        [[nodiscard]] bool ShouldLog(LogLevel level, LogCategory category) const noexcept;

        /** @brief Set the producer behavior used when a thread-local log ring is full. */
        void SetBackpressurePolicy(LogBackpressurePolicy policy) noexcept;

        /** @brief Return the producer behavior used when a thread-local log ring is full. */
        [[nodiscard]] LogBackpressurePolicy GetBackpressurePolicy() const noexcept;

        /** @brief Return the number of records dropped since construction or the last reset. */
        [[nodiscard]] uint64_t DroppedCount() const noexcept;

        /** @brief Reset the dropped-record counter. */
        void ResetDroppedCount() noexcept;

        /** @brief Submit a preformatted message. */
        void Submit(LogLevel level, LogCategory category, SourceLocation source, std::string message);

        /** @brief Format and submit a log record. */
        template<typename... Args>
        void Log(CallerLocation caller, LogLevel level, LogCategory category, std::format_string<Args...> format,
                 Args&&... args) {
            if (!ShouldLog(level, category)) {
                return;
            }
            Submit(level, category, caller.location, std::format(format, std::forward<Args>(args)...));
        }

        /** @brief Flush every sink. */
        void Flush();

        /** @brief Stop the background drain thread after draining pending records. */
        void Shutdown();

    private:
        static constexpr size_t kCategoryStorageSize = 64;

        struct ThreadRing;
        struct RegisteredRing {
            ThreadRing* ring = nullptr; /**< Thread-local producer ring. */
            uint64_t threadId = 0;      /**< Stable hash of the producing thread id. */
        };

        void WriteToRing(const LogRecord& record);
        void DispatchSerialized(std::span<const std::byte> payload);
        void DispatchToSinks(const LogRecord& record);
        uint32_t DrainOnce();
        void DrainLoop();
        [[nodiscard]] static ThreadRing& GetThreadRing();

        mutable std::mutex mutex_{};
        std::vector<LogSink> sinks_{};
        std::array<std::atomic<uint8_t>, kCategoryStorageSize> categoryLevels_{};

        std::mutex ringsMutex_{};
        std::vector<RegisteredRing> rings_{};

        std::atomic<LogBackpressurePolicy> backpressurePolicy_{LogBackpressurePolicy::Drop};
        std::atomic<uint64_t> droppedCount_{0};
        std::atomic<bool> running_{false};
        std::atomic<bool> flushRequested_{false};
        std::atomic<bool> flushDone_{false};
        std::thread drainThread_{};
        std::mutex drainMutex_{};
        std::condition_variable drainCv_{};
        std::mutex flushMutex_{};
        std::condition_variable flushCv_{};
    };

    /** @brief Deferred call-site object enabling macro-free syntax: @c LogAt<Info, Core>()("value {}", x). */
    template<LogLevel Level, LogCategory Category>
    struct LogSite {
        SourceLocation source; /**< Captured call-site location. */

        /** @brief Format and submit a record through the default logger. */
        template<typename... Args>
        void operator()(std::format_string<Args...> format, Args&&... args) const {
            if constexpr (static_cast<uint8_t>(Level) >= static_cast<uint8_t>(kMinLogLevel)) {
                if (!StructuredLogger::Default().ShouldLog(Level, Category)) {
                    return;
                }
                StructuredLogger::Default().Submit(Level, Category, source,
                                                   std::format(format, std::forward<Args>(args)...));
            }
        }
    };

    /** @brief Capture a log site for deferred formatting. */
    template<LogLevel Level, LogCategory Category>
    [[nodiscard]] constexpr LogSite<Level, Category>
    LogAt(std::source_location location = std::source_location::current()) noexcept {
        return {{location.file_name(), location.function_name(), location.line(), location.column()}};
    }

    /** @brief Compile-time level/category logging helper compatible with APIs that pass @ref CallerLocation. */
    template<LogLevel Level, LogCategory Category, typename... Args>
    void Log(CallerLocation caller, std::format_string<Args...> format, Args&&... args) {
        if constexpr (static_cast<uint8_t>(Level) >= static_cast<uint8_t>(kMinLogLevel)) {
            StructuredLogger::Default().Log(caller, Level, Category, format, std::forward<Args>(args)...);
        }
    }

    /** @brief Flush the default logger. */
    inline void FlushLogs() {
        StructuredLogger::Default().Flush();
    }

} // namespace Sora

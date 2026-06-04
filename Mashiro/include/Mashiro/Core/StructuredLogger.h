/**
 * @file StructuredLogger.h
 * @brief High-performance structured logging system with zero-contention hot path.
 *
 * Architecture:
 * - Producer threads → thread-local SPSC ring buffer (64 KB).
 * - Background drain thread → merges rings → dispatches to sinks via `std::visit`.
 *
 * Hot path (`Log()`) touches only thread-local memory: no mutex, no atomic CAS,
 * no virtual call, no heap allocation.
 *
 * C++26 features used:
 * - **P2996 reflection** — `LogLevel`/`LogCategory` auto-ToString; annotation-driven
 *   default levels and terminal colors.
 * - **P3385 annotations** — `[[=LogAnno::LevelColor{...}]]` on `LogLevel` enumerators,
 *   `[[=LogAnno::DefaultLevel{N}]]` on `LogCategory` enumerators.
 * - **`std::source_location`** — `SourceLoc` replaces `__FILE__`/`__LINE__` macros.
 * - **`std::print`** — sinks use `std::println` for formatted output.
 *
 * @ingroup Core
 */
#pragma once

#include "Mashiro/Core/SpscRingBuffer.h"
#include "Mashiro/Core/TypeTraits.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <functional>
#include <meta>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace Mashiro {

    // =========================================================================
    // Annotations (P3385)
    // =========================================================================

    /// @brief Annotations for log enum values.
    namespace LogAnno {

        /// @brief Default runtime log level for a category enumerator.
        struct DefaultLevel {
            uint8_t level;
            constexpr bool operator==(const DefaultLevel&) const = default;
        };

        /// @brief Terminal foreground color for a log level enumerator.
        struct LevelColor {
            uint8_t r, g, b;
            bool bold = false;
            bool dim = false;
            constexpr bool operator==(const LevelColor&) const = default;
        };

    } // namespace LogAnno

    // =========================================================================
    // LogLevel
    // =========================================================================

    /// @brief Severity level for log messages (ordered by increasing severity).
    enum class LogLevel : uint8_t {
        [[=LogAnno::LevelColor{128, 128, 128, false, true}]]
        Trace = 0,
        [[=LogAnno::LevelColor{0, 200, 200}]]
        Debug = 1,
        [[=LogAnno::LevelColor{0, 200, 0}]]
        Info  = 2,
        [[=LogAnno::LevelColor{220, 220, 0, true}]]
        Warn  = 3,
        [[=LogAnno::LevelColor{220, 0, 0, true}]]
        Error = 4,
        [[=LogAnno::LevelColor{255, 255, 255, true}]]
        Fatal = 5,
    };

    /// @brief Compile-time minimum log level. Messages below this are eliminated.
#ifndef MASHIRO_MIN_LOG_LEVEL
#    ifdef NDEBUG
    inline constexpr LogLevel kMinLogLevel = LogLevel::Info;
#    else
    inline constexpr LogLevel kMinLogLevel = LogLevel::Trace;
#    endif
#else
    inline constexpr LogLevel kMinLogLevel = static_cast<LogLevel>(MASHIRO_MIN_LOG_LEVEL);
#endif

    // =========================================================================
    // LogCategory
    // =========================================================================

    /**
     * @brief Semantic domain for log messages (runtime-filterable).
     *
     * Annotated with `[[=LogAnno::DefaultLevel{N}]]` for initial runtime level.
     * Users extend via: `inline constexpr LogCategory kMyPlugin = static_cast<LogCategory>(100);`
     */
    enum class LogCategory : uint16_t {
        [[=LogAnno::DefaultLevel{2}]] Core     = 0,
        [[=LogAnno::DefaultLevel{2}]] Render   = 1,
        [[=LogAnno::DefaultLevel{2}]] Resource = 2,
        [[=LogAnno::DefaultLevel{1}]] Scene    = 3,
        [[=LogAnno::DefaultLevel{1}]] Input    = 4,
        [[=LogAnno::DefaultLevel{2}]] Audio    = 5,
        [[=LogAnno::DefaultLevel{2}]] Network  = 6,
        [[=LogAnno::DefaultLevel{1}]] Script   = 7,
        [[=LogAnno::DefaultLevel{1}]] Editor   = 8,
        [[=LogAnno::DefaultLevel{1}]] Physics  = 9,
        [[=LogAnno::DefaultLevel{2}]] UI       = 10,
        [[=LogAnno::DefaultLevel{0}]] App      = 11,
    };

    // =========================================================================
    // Annotation extraction (consteval — zero runtime cost)
    // =========================================================================

    /** @cond INTERNAL */
    namespace Detail::Log {

        consteval auto BuildDefaultLevels() {
            auto enumerators = std::meta::enumerators_of(^^LogCategory);
            std::array<uint8_t, enumerators.size()> levels{};
            for (std::size_t i = 0; i < enumerators.size(); ++i) {
                auto annots = std::meta::annotations_of(enumerators[i], ^^LogAnno::DefaultLevel);
                levels[i] = (annots.size() > 0)
                    ? std::meta::extract<LogAnno::DefaultLevel>(annots[0]).level
                    : 0;
            }
            return levels;
        }

        inline constexpr auto kDefaultCategoryLevels = BuildDefaultLevels();

        consteval LogAnno::LevelColor GetLevelColor(LogLevel level) {
            for (auto e : std::meta::enumerators_of(^^LogLevel)) {
                if (std::meta::constant_of(e) == static_cast<uint8_t>(level)) {
                    auto annots = std::meta::annotations_of(e, ^^LogAnno::LevelColor);
                    if (annots.size() > 0)
                        return std::meta::extract<LogAnno::LevelColor>(annots[0]);
                }
            }
            return {255, 255, 255};
        }

    } // namespace Detail::Log
    /** @endcond */

    // =========================================================================
    // SourceLoc
    // =========================================================================

    /// @brief Lightweight call-site capture via `std::source_location`.
    struct SourceLoc {
        std::string_view file;     ///< Full source file path.
        std::string_view function; ///< Function name.
        uint32_t line = 0;         ///< 1-based line number.

        [[nodiscard]] static constexpr SourceLoc
        Current(std::source_location loc = std::source_location::current()) noexcept {
            return {loc.file_name(), loc.function_name(), static_cast<uint32_t>(loc.line())};
        }

        /// @brief Extract just the filename (no directory path). Returns owning string.
        [[nodiscard]] std::string FileName() const {
            return std::filesystem::path{file}.filename().string();
        }
    };

    // =========================================================================
    // LogEntry
    // =========================================================================

    /// @brief A single resolved log entry passed to sinks.
    struct LogEntry {
        LogLevel level;
        LogCategory category;
        std::string_view message;
        std::string_view file;
        std::string_view func;
        uint32_t line = 0;
        uint64_t timestampNs = 0;
        uint32_t threadId = 0;
    };

    // =========================================================================
    // BackpressurePolicy
    // =========================================================================

    enum class BackpressurePolicy : uint8_t {
        Drop,  ///< Silently drop oldest entries.
        Block, ///< Producer spins until space is available.
    };

    // =========================================================================
    // Sinks (value types — dispatched via std::visit, no virtual)
    // =========================================================================

    /// @name Sinks
    /// @{

    /// @brief Console sink: ANSI-colored output to stderr via tapioca.
    class ConsoleSink {
    public:
        ConsoleSink();
        void Write(const LogEntry& entry);
        void Flush();
    private:
        bool colorEnabled_ = false;
    };

    /// @brief File sink: plain-text to a file.
    class FileSink {
    public:
        explicit FileSink(std::filesystem::path path, bool dailyRotation = false);
        ~FileSink();
        FileSink(FileSink&&) noexcept;
        FileSink& operator=(FileSink&&) noexcept;
        FileSink(const FileSink&) = delete;
        FileSink& operator=(const FileSink&) = delete;
        void Write(const LogEntry& entry);
        void Flush();
        [[nodiscard]] const std::filesystem::path& GetPath() const { return basePath_; }
    private:
        FILE* file_ = nullptr;
        std::filesystem::path basePath_;
        bool rotate_;
    };

    /// @brief JSON sink: one NDJSON line per entry.
    class JsonSink {
    public:
        explicit JsonSink(std::filesystem::path path);
        ~JsonSink();
        JsonSink(JsonSink&&) noexcept;
        JsonSink& operator=(JsonSink&&) noexcept;
        JsonSink(const JsonSink&) = delete;
        JsonSink& operator=(const JsonSink&) = delete;
        void Write(const LogEntry& entry);
        void Flush();
    private:
        FILE* file_ = nullptr;
        std::filesystem::path path_;
    };

    /// @brief Callback sink: bridges to user handler.
    class CallbackSink {
    public:
        using Callback = std::function<void(const LogEntry&)>;
        explicit CallbackSink(Callback cb);
        void Write(const LogEntry& entry);
        void Flush();
    private:
        Callback cb_;
    };

    using LogSink = std::variant<ConsoleSink, FileSink, JsonSink, CallbackSink>;

    /// @}

    /// @brief Logger's internal ring buffer type (64 KB, from SpscRingBuffer.h).
    using LogRing = SpscByteRing<64 * 1024>;

    // =========================================================================
    // StructuredLogger
    // =========================================================================

    /// @brief Core structured logger. Singleton. Zero-contention hot path.
    class StructuredLogger {
    public:
        /// @brief Get the singleton instance.
        static StructuredLogger& Instance();

        /// @brief Register a sink. Call before logging begins.
        void AddSink(LogSink sink);

        /// @brief Remove all sinks.
        void ClearSinks();

        /// @brief Set the runtime log level for a category.
        void SetCategoryLevel(LogCategory cat, LogLevel level);

        /// @brief Get the current runtime log level for a category.
        [[nodiscard]] LogLevel GetCategoryLevel(LogCategory cat) const;

        /// @brief Set the backpressure policy for all ring buffers.
        void SetBackpressurePolicy(BackpressurePolicy policy);

        /// @brief Get the current backpressure policy.
        [[nodiscard]] BackpressurePolicy GetBackpressurePolicy() const;

        /**
         * @brief Log a formatted message (hot path).
         *
         * Formats into a stack-local 512-byte buffer, serialises to the
         * thread-local SPSC ring, and notifies the drain thread. `SourceLoc`
         * is auto-captured via default parameter.
         */
        template <typename... Args>
        void Log(LogLevel level, LogCategory cat,
                 std::format_string<Args...> fmt, Args&&... args,
                 SourceLoc loc = SourceLoc::Current()) {
            if (static_cast<uint8_t>(level) < static_cast<uint8_t>(kMinLogLevel)) return;
            if (static_cast<uint8_t>(level) < static_cast<uint8_t>(GetCategoryLevel(cat))) return;

            auto ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());

            char buf[512];
            auto result = std::format_to_n(buf, sizeof(buf) - 1, fmt, std::forward<Args>(args)...);
            *result.out = '\0';

            WriteToRing(level, cat, loc.file, loc.line, loc.function, ns,
                        std::string_view{buf, static_cast<std::size_t>(result.out - buf)});
        }

        /// @brief Flush all sinks. Blocks until drain processes pending entries.
        void Flush();

        /// @brief Flush + join drain thread.
        void Shutdown();

        /// @brief Start the background drain thread.
        void StartDrainThread();

        /// @brief True if the drain thread is running.
        [[nodiscard]] bool IsRunning() const;

        /// @brief Number of messages dropped since last reset.
        [[nodiscard]] uint64_t GetDroppedCount() const;

        /// @brief Reset the dropped message counter.
        void ResetDroppedCount();

        /// @brief Discard all pending data in registered ring buffers.
        void ResetRings();

        StructuredLogger();
        ~StructuredLogger();
        StructuredLogger(const StructuredLogger&) = delete;
        StructuredLogger& operator=(const StructuredLogger&) = delete;

    private:
        // --- Internal ring management ---
        void WriteToRing(LogLevel level, LogCategory cat, std::string_view file,
                         uint32_t line, std::string_view func,
                         uint64_t timestampNs, std::string_view message);
        void DrainLoop();
        uint32_t DrainOnce();
        void DispatchToSinks(const LogEntry& entry);
        static LogRing& GetThreadRing();

        struct RegisteredRing {
            LogRing* ring = nullptr;
            uint32_t threadId = 0;
        };

        // --- Sinks (guarded by sinksMutex_) ---
        std::mutex sinksMutex_;
        std::vector<LogSink> sinks_;

        // --- Per-thread rings (guarded by ringsMutex_) ---
        std::mutex ringsMutex_;
        std::vector<RegisteredRing> rings_;

        // --- Per-category runtime level filter (cache-line aligned, lock-free) ---
        alignas(Platform::kCacheLineSize)
            std::array<std::atomic<uint8_t>, Traits::EnumEnumeratorsCount<LogCategory>> categoryLevels_;

        // --- Configuration (lock-free atomics) ---
        std::atomic<BackpressurePolicy> policy_{BackpressurePolicy::Drop};
        std::atomic<uint64_t> droppedCount_{0};

        // --- Drain thread state ---
        std::thread drainThread_;
        std::atomic<bool> running_{false};

        // --- Flush synchronisation ---
        std::atomic<bool> flushRequested_{false};
        std::atomic<bool> flushDone_{false};
        std::mutex drainMutex_;
        std::condition_variable drainCv_;
        std::mutex flushMutex_;
        std::condition_variable flushCv_;
    };

    // =========================================================================
    // Free-function log interface (compile-time level elimination)
    // =========================================================================

    /**
     * @brief Log with compile-time level + category. Auto-captures source location.
     *
     * @code
     * using enum LogLevel; using enum LogCategory;
     * Log<Info, Render>("FPS: {}", fps);
     * @endcode
     */
    template <LogLevel Level, LogCategory Cat, typename... Args>
    void Log(std::format_string<Args...> fmt, Args&&... args,
             SourceLoc loc = SourceLoc::Current()) {
        if constexpr (static_cast<uint8_t>(Level) >= static_cast<uint8_t>(kMinLogLevel)) {
            auto& logger = StructuredLogger::Instance();
            if (static_cast<uint8_t>(Level) >= static_cast<uint8_t>(logger.GetCategoryLevel(Cat)))
                logger.Log(Level, Cat, fmt, std::forward<Args>(args)..., loc);
        }
    }

    /// @brief Flush all log sinks.
    inline void LogFlush() { StructuredLogger::Instance().Flush(); }

} // namespace Mashiro

// =============================================================================
// Minimal convenience macro (optional)
// =============================================================================

#define MLOG(level, cat, ...) \
    ::Mashiro::Log<::Mashiro::LogLevel::level, ::Mashiro::LogCategory::cat>(__VA_ARGS__)

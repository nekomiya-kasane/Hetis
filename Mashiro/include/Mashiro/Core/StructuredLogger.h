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
        Trace = 0, ///< Finest-grained diagnostic output.
        Debug = 1, ///< Debug-time information.
        Info  = 2, ///< Normal operational messages.
        Warn  = 3, ///< Potentially harmful conditions.
        Error = 4, ///< Recoverable errors.
        Fatal = 5, ///< Unrecoverable errors (typically followed by abort).
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
     * Default levels are declared in `Detail::Log::kDefaultCategoryLevels[]`.
     * Users extend via: `inline constexpr LogCategory kMyPlugin = static_cast<LogCategory>(100);`
     */
    enum class LogCategory : uint16_t {
        Core     = 0,
        Render   = 1,
        Resource = 2,
        Scene    = 3,
        Input    = 4,
        Audio    = 5,
        Network  = 6,
        Script   = 7,
        Editor   = 8,
        Physics  = 9,
        UI       = 10,
        App      = 11,
    };

    // =========================================================================
    // Log traits — specialise to customise category/level display properties
    // =========================================================================

    /// @brief Default level for a LogCategory. Specialise to override.
    template <LogCategory Cat>
    struct LogCategoryTraits {
        static constexpr LogLevel defaultLevel = LogLevel::Trace;
    };

    template <> struct LogCategoryTraits<LogCategory::Core>     { static constexpr LogLevel defaultLevel = LogLevel::Info;  };
    template <> struct LogCategoryTraits<LogCategory::Render>   { static constexpr LogLevel defaultLevel = LogLevel::Info;  };
    template <> struct LogCategoryTraits<LogCategory::Resource> { static constexpr LogLevel defaultLevel = LogLevel::Info;  };
    template <> struct LogCategoryTraits<LogCategory::Scene>    { static constexpr LogLevel defaultLevel = LogLevel::Debug; };
    template <> struct LogCategoryTraits<LogCategory::Input>    { static constexpr LogLevel defaultLevel = LogLevel::Debug; };
    template <> struct LogCategoryTraits<LogCategory::Audio>    { static constexpr LogLevel defaultLevel = LogLevel::Info;  };
    template <> struct LogCategoryTraits<LogCategory::Network>  { static constexpr LogLevel defaultLevel = LogLevel::Info;  };
    template <> struct LogCategoryTraits<LogCategory::Script>   { static constexpr LogLevel defaultLevel = LogLevel::Debug; };
    template <> struct LogCategoryTraits<LogCategory::Editor>   { static constexpr LogLevel defaultLevel = LogLevel::Debug; };
    template <> struct LogCategoryTraits<LogCategory::Physics>  { static constexpr LogLevel defaultLevel = LogLevel::Debug; };
    template <> struct LogCategoryTraits<LogCategory::UI>       { static constexpr LogLevel defaultLevel = LogLevel::Info;  };
    template <> struct LogCategoryTraits<LogCategory::App>      { static constexpr LogLevel defaultLevel = LogLevel::Trace; };

    /// @brief Display color for a LogLevel. Specialise to override.
    template <LogLevel Level>
    struct LogLevelTraits {
        static constexpr LogAnno::LevelColor color = {255, 255, 255};
    };

    template <> struct LogLevelTraits<LogLevel::Trace> { static constexpr LogAnno::LevelColor color = {128, 128, 128, false, true};  };
    template <> struct LogLevelTraits<LogLevel::Debug> { static constexpr LogAnno::LevelColor color = {0,   200, 200};                };
    template <> struct LogLevelTraits<LogLevel::Info>  { static constexpr LogAnno::LevelColor color = {0,   200, 0};                  };
    template <> struct LogLevelTraits<LogLevel::Warn>  { static constexpr LogAnno::LevelColor color = {220, 220, 0,   true};          };
    template <> struct LogLevelTraits<LogLevel::Error> { static constexpr LogAnno::LevelColor color = {220, 0,   0,   true};          };
    template <> struct LogLevelTraits<LogLevel::Fatal> { static constexpr LogAnno::LevelColor color = {255, 255, 255, true};          };

    /** @cond INTERNAL */
    namespace Detail::Log {

        /// @brief Build default-level array from traits at compile time.
        consteval auto BuildDefaultLevels() {
            constexpr auto N = Traits::EnumEnumeratorsCount<LogCategory>;
            std::array<uint8_t, N> levels{};
            template for (constexpr auto e : std::define_static_array(
                std::meta::enumerators_of(^^LogCategory))) {
                constexpr auto cat = [:e:];
                levels[static_cast<std::size_t>(cat)] =
                    static_cast<uint8_t>(LogCategoryTraits<cat>::defaultLevel);
            }
            return levels;
        }
        inline constexpr auto kDefaultCategoryLevels = BuildDefaultLevels();

        /// @brief Build color array from traits at compile time.
        consteval auto BuildLevelColors() {
            constexpr auto N = Traits::EnumEnumeratorsCount<LogLevel>;
            std::array<LogAnno::LevelColor, N> colors{};
            template for (constexpr auto e : std::define_static_array(
                std::meta::enumerators_of(^^LogLevel))) {
                constexpr auto lvl = [:e:];
                colors[static_cast<std::size_t>(lvl)] = LogLevelTraits<lvl>::color;
            }
            return colors;
        }
        inline constexpr auto kLevelColors = BuildLevelColors();

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

    /// @brief Carries an auto-captured `SourceLoc`.
    ///
    /// Implicitly constructs from nothing via the defaulted
    /// `std::source_location::current()` parameter, capturing the
    /// **caller's** site. Use as the last non-pack parameter:
    /// @code
    ///   template <typename... Args>
    ///   void f(CallerLoc loc, std::format_string<Args...> fmt, Args&&... args);
    ///   // caller: f({}, "hello {}", 42);  — loc captures caller site
    /// @endcode
    struct CallerLoc {
        SourceLoc loc;
        constexpr CallerLoc(std::source_location l = std::source_location::current()) noexcept
            : loc{SourceLoc::Current(l)} {}
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
         * is auto-captured via `CallerLoc`'s implicit constructor.
         *
         * @param caller  Auto-captured caller site (pass `{}` or omit).
         */
        template <typename... Args>
        void Log(CallerLoc caller, LogLevel level, LogCategory cat,
                 std::format_string<Args...> fmt, Args&&... args) {
            if (static_cast<uint8_t>(level) < static_cast<uint8_t>(kMinLogLevel)) return;
            if (static_cast<uint8_t>(level) < static_cast<uint8_t>(GetCategoryLevel(cat))) return;
            char buf[512];
            auto result = std::format_to_n(buf, sizeof(buf) - 1, fmt, std::forward<Args>(args)...);
            *result.out = '\0';
            Submit(level, cat, caller.loc,
                   std::string_view{buf, static_cast<std::size_t>(result.out - buf)});
        }

        /// @brief Non-template entry point (exported from DLL). Called by Log/LogAt.
        void Submit(LogLevel level, LogCategory cat, SourceLoc loc, std::string_view message);

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
    void Log(CallerLoc caller, std::format_string<Args...> fmt, Args&&... args) {
        if constexpr (static_cast<uint8_t>(Level) >= static_cast<uint8_t>(kMinLogLevel)) {
            auto& logger = StructuredLogger::Instance();
            if (static_cast<uint8_t>(Level) >= static_cast<uint8_t>(logger.GetCategoryLevel(Cat)))
                logger.Log(caller, Level, Cat, fmt, std::forward<Args>(args)...);
        }
    }

    /// @brief Flush all log sinks.
    inline void LogFlush() { StructuredLogger::Instance().Flush(); }

} // namespace Mashiro

// =============================================================================
// Minimal convenience macro (optional)
// =============================================================================

#define MLOG(level, cat, ...) \
    ::Mashiro::Log<::Mashiro::LogLevel::level, ::Mashiro::LogCategory::cat>({}, __VA_ARGS__)

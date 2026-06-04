/**
 * @file StackTrace.h
 * @brief Cross-platform stack trace capture and pretty-print utility.
 *
 * Hand-rolled replacement for `<stacktrace>` (not available in libc++):
 * - **Windows**: `CaptureStackBackTrace` + DbgHelp symbol resolution.
 * - **POSIX**: `backtrace` + `dladdr` + `abi::__cxa_demangle`.
 *
 * Output modes:
 * - `ToString()` — plain-text multi-line (for file/JSON sinks).
 * - `PrintColored()` — tapioca-styled box-drawing to stderr.
 * - `FormatCompact()` — single-line per frame (for inline log messages).
 *
 * Thread-safe: DbgHelp symbol resolution is guarded by an internal mutex.
 *
 * Uses `SourceLoc` from `Log.h` for consistent call-site info.
 *
 * @ingroup Core
 */
#pragma once

#include "Mashiro/Core/StructuredLogger.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Mashiro {

    // =========================================================================
    // StackFrame
    // =========================================================================

    /// @brief One resolved frame from a captured stack trace.
    struct StackFrame {
        void* address = nullptr;   ///< Raw instruction pointer.
        std::string symbol;        ///< Demangled function name (or "<unknown>").
        std::string module;        ///< Module / shared library basename.
        std::string sourceFile;    ///< Source file path (empty if unavailable).
        uint32_t sourceLine = 0;   ///< 1-based line number (0 = unknown).
        uint64_t offset = 0;       ///< Byte displacement from function start.
    };

    // =========================================================================
    // StackTrace
    // =========================================================================

    /**
     * @brief Captured and symbol-resolved stack trace.
     *
     * Usage:
     * @code
     * auto trace = StackTrace::Capture();
     * trace.PrintColored("Crash");
     * LOG_ERROR(Core, "Backtrace:\n{}", trace.ToString());
     * @endcode
     */
    class StackTrace {
    public:
        /// @name Capture
        /// @{

        /**
         * @brief Capture the current thread's call stack.
         * @param skipFrames Number of innermost frames to skip (Capture itself always skipped).
         * @param maxFrames Upper bound on captured frames.
         * @return StackTrace with resolved symbols.
         */
        [[nodiscard]] static StackTrace Capture(uint32_t skipFrames = 0, uint32_t maxFrames = 64);

        /// @}

        /// @name Accessors
        /// @{

        /// @brief Access the resolved frames.
        [[nodiscard]] const std::vector<StackFrame>& GetFrames() const noexcept { return frames_; }

        /// @brief Number of captured frames.
        [[nodiscard]] uint32_t Size() const noexcept { return static_cast<uint32_t>(frames_.size()); }

        /// @brief True if no frames were captured.
        [[nodiscard]] bool Empty() const noexcept { return frames_.empty(); }

        /// @}

        /// @name Formatting
        /// @{

        /// @brief Plain-text multi-line representation (no ANSI escapes).
        [[nodiscard]] std::string ToString() const;

        /// @brief Pretty-print to stderr with tapioca box-drawing and colors.
        void PrintColored(std::string_view title = "Stack Trace") const;

        /// @brief Compact single-line format for a single frame.
        [[nodiscard]] static std::string FormatCompact(const StackFrame& frame, uint32_t index);

        /// @}

    private:
        std::vector<StackFrame> frames_;
    };

}  // namespace Mashiro

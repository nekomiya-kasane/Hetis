/**
 * @file StackTrace.h
 * @brief Captured native call stacks with platform symbol resolution and Sora string-rendering integration.
 * @details
 * Capture is explicit and bounded. The result can be rendered through Sora's plain or styled string protocols:
 * @code{.cpp}
 * const Sora::StackTrace trace = Sora::StackTrace::Capture({.maxFrames = 32});
 * for (const Sora::StackFrame& frame : trace.Frames()) {
 *     SubmitDiagnosticAddress(frame.address);
 * }
 * std::println("{}", trace.ToString());
 * @endcode
 *
 * Stack capture and symbolization may allocate and lock; never call this API from a fatal signal or structured
 * exception handler. Use @ref Sora::CrashHandler for corrupted-process emergency reporting.
 * @ingroup Core
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <inplace_vector>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace Sora {

    namespace Styled {

        class StyledStringBuilder;

    } // namespace Styled

    /** @brief One resolved native stack frame. */
    struct StackFrame {
        uintptr_t address = 0;   /**< Raw instruction pointer. */
        std::string symbol;      /**< Demangled function name, or empty when symbol resolution failed. */
        std::string module;      /**< Native module image name, usually a basename. */
        std::string sourceFile;  /**< Source file path when line information is available. */
        uint32_t sourceLine = 0; /**< 1-based source line, or zero when unavailable. */
        uint64_t offset = 0;     /**< Byte displacement from the resolved symbol start. */

        /** @brief Return the most useful human-readable symbol spelling for this frame. */
        [[nodiscard]] std::string_view DisplaySymbol() const noexcept;
    };

    /** @brief Named bounds controlling stack capture. */
    struct StackTraceCaptureOptions {
        uint32_t skipFrames = 0; /**< Additional innermost caller frames to omit. */
        uint32_t maxFrames = 64; /**< Requested frame count, clamped to the container limit. */
    };

    /**
     * @brief Value object containing frames captured from the current thread.
     *
     * @details Capture is intentionally explicit and bounded. Symbol resolution is best-effort: Windows uses
     * DbgHelp through lazy-loaded function pointers, while POSIX targets use native backtrace facilities when present.
     * The type has no dependency on the logger; it only participates in Sora's ToString and ToStyledString protocols.
     */
    class StackTrace {
    public:
        inline static constexpr size_t kMaximumFrameCount = 64; /**< Maximum retained frame count. */
        using ContainerType = std::inplace_vector<StackFrame, kMaximumFrameCount>;

        /**
         * @brief Capture the current thread's call stack.
         * @param[in] options Named skip and capacity bounds.
         */
        [[nodiscard]] static StackTrace Capture(StackTraceCaptureOptions options = {});

        /** @brief Construct an empty stack trace. */
        StackTrace() = default;

        /** @brief Construct from resolved frames. */
        explicit StackTrace(ContainerType frames) noexcept : frames_(std::move(frames)) {}

        /** @brief Return resolved frames in innermost-to-outermost order. */
        [[nodiscard]] std::span<const StackFrame> Frames() const noexcept { return frames_; }

        /** @brief Return the number of captured frames. */
        [[nodiscard]] size_t Size() const noexcept { return frames_.size(); }

        /** @brief Return true when no frame was captured. */
        [[nodiscard]] bool Empty() const noexcept { return frames_.empty(); }

        /** @brief Format one frame as a compact single-line diagnostic. */
        [[nodiscard]] static std::string FormatFrame(const StackFrame& frame, size_t index);

        /** @brief Plain multi-line representation suitable for files and JSON payloads. */
        [[nodiscard]] std::string ToString() const;

        /** @brief Styled terminal representation used by @ref Sora::ToStyledString. */
        void ToStyledString(Sora::Styled::StyledStringBuilder& builder) const;

    private:
        ContainerType frames_{};
    };

} // namespace Sora

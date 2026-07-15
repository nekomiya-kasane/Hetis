/**
 * @file Debugging.h
 * @brief Portable debugger detection and breakpoint primitives.
 * @ingroup Core
 */
#pragma once

namespace Sora::Debug {

    /** @brief Return whether the current process is being traced by a debugger. */
    [[nodiscard]] bool IsDebuggerPresent() noexcept;

    /** @brief Raise a native debugger breakpoint unconditionally. */
    void Breakpoint() noexcept;

    /** @brief Raise a native debugger breakpoint only when @ref IsDebuggerPresent returns true. */
    void BreakpointIfDebuggerPresent() noexcept;

} // namespace Sora::Debug

/**
 * @file ThreadNaming.h
 * @brief Cross-platform thread naming utilities for debugging and diagnostics.
 *
 * Thread names assigned via these functions surface in debuggers (Visual Studio,
 * WinDbg, gdb), profilers (PIX, Tracy, perf), ETW traces, and crash dumps.
 *
 * Backend per platform:
 * - **Windows**: `SetThreadDescription` (Win10 1607+), retrievable with
 *   `GetThreadDescription`. Names are wide strings internally; UTF-8 input is
 *   converted on the boundary.
 * - **POSIX**: `pthread_setname_np` / `pthread_getname_np`. Linux limits names
 *   to 15 bytes plus the null terminator; longer names are truncated.
 *
 * All functions are thread-safe and may be called from any thread.
 *
 * @ingroup Platform
 */

#pragma once

#include <string>

namespace Mashiro {

    /**
     * @brief Set the name of the current thread.
     *
     * @param iName UTF-8 thread name. Must not be null. On Linux, only the
     *              first 15 bytes are retained.
     *
     * @note Thread-safe. Idempotent — later calls overwrite the prior name.
     */
    void SetCurrentThreadName(const char* iName);

    /**
     * @brief Set the name of an arbitrary thread by its native handle.
     *
     * @param iNativeHandle Platform-native thread handle. On Windows this is a
     *                      `HANDLE` (e.g. from `GetCurrentThread()` or
     *                      `std::thread::native_handle()`). On POSIX this is a
     *                      `pthread_t` reinterpreted as `void*`.
     * @param iName         UTF-8 thread name. Must not be null.
     *
     * @note Thread-safe.
     */
    void SetThreadName(void* iNativeHandle, const char* iName);

    /**
     * @brief Retrieve the name of the current thread.
     *
     * @return The thread's name, or an empty string if no name has been set or
     *         the platform does not expose one.
     *
     * @note Thread-safe.
     */
    std::string GetCurrentThreadName();

    /**
     * @brief Retrieve the name of an arbitrary thread by its native handle.
     *
     * @param iNativeHandle Platform-native thread handle (see SetThreadName).
     * @return The thread's name, or an empty string if no name has been set.
     *
     * @note Thread-safe.
     */
    std::string GetThreadName(void* iNativeHandle);

} // namespace Mashiro

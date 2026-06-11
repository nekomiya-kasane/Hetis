#pragma once

#include <string>

namespace Mashiro {

    // Set the name/description of the current thread for debugging purposes.
    // This name appears in debuggers, profilers, and crash dumps.
    // Thread-safe: can be called from any thread.
    void SetCurrentThreadName(const char* iName);

    // Set the name/description of a specific thread by its native handle.
    // On Windows: handle is HANDLE, on POSIX: handle is pthread_t
    void SetThreadName(void* iNativeHandle, const char* iName);

    // Get the name/description of the current thread.
    // Returns empty string if no name was set.
    std::string GetCurrentThreadName();

    // Get the name/description of a specific thread by its native handle.
    // Returns empty string if no name was set.
    std::string GetThreadName(void* iNativeHandle);

} // namespace Mashiro

/**
 * @file ABI.cpp
 * @brief Runtime name demangling — DbgHelp on Windows, libc++abi on POSIX.
 *
 * Implements @ref Sora::Meta::ABI::Runtime. The compile-time `Mangle` direction is
 * header-only `consteval`; this translation unit holds the one runtime piece.
 *
 * Backend selection is a compile-time `#if` — exactly one path compiles, with
 * no runtime branch and no dead backend linked:
 * - **Windows / MSVC ABI** → `UnDecorateSymbolName` (DbgHelp). DbgHelp is not
 *   thread-safe, so calls are serialised behind a private mutex. The header and
 *   import library are present in this sysroot (the project already links
 *   `dbghelp`; see Sora/CMakeLists.txt and StackTrace.cpp), so we include
 *   `<dbghelp.h>` directly rather than hand-binding the entry point.
 * - **POSIX / Itanium ABI** → `abi::__cxa_demangle`; the returned malloc buffer
 *   is freed before returning.
 */

#include "Sora/Core/ABI.h"

#include <string>

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    define NOMINMAX
#    include <windows.h>
#    include <dbghelp.h>
#    include <mutex>
#else
#    include <cstdlib>
#    include <cxxabi.h>
#endif

namespace Sora::Meta::ABI {

#ifdef _WIN32

    namespace {

        /// @brief DbgHelp is not thread-safe; serialise every call.
        std::mutex g_undecorateMutex;

        /// @brief Core demangle: returns nullopt on failure. Accepts both the
        ///        linker-symbol form (`?foo@@YAHXZ`) and the typeid `raw_name()`
        ///        form (`.?AVFoo@@` / `.H`). `UnDecorateSymbolName` wants a
        ///        decorated *name*, so a leading `.` (typeid marker) is dropped;
        ///        a bare builtin tag like `.H` has no textual demangling and is
        ///        reported as failure (there is no human form to recover).
        std::optional<std::string> Undecorate(std::string_view mangled) noexcept {
            if (mangled.empty()) {
                return std::nullopt;
            }

            std::string_view sym = mangled;
            if (sym.front() == '.') {
                sym.remove_prefix(1);
            }
            if (sym.empty() || sym.front() != '?') {
                return std::nullopt;
            }

            // UnDecorateSymbolName needs a NUL-terminated input.
            std::string buf{sym};

            char out[1024];
            std::lock_guard lock(g_undecorateMutex);
            DWORD n = ::UnDecorateSymbolName(buf.c_str(), out, static_cast<DWORD>(sizeof(out)), UNDNAME_COMPLETE);
            if (n == 0) {
                return std::nullopt;
            }
            return std::string(out, n);
        }

    } // anonymous namespace

#else // POSIX / Itanium

    namespace {

        std::optional<std::string> Undecorate(std::string_view mangled) noexcept {
            if (mangled.empty()) {
                return std::nullopt;
            }
            std::string buf{mangled};
            int status = 0;
            char* demangled = abi::__cxa_demangle(buf.c_str(), nullptr, nullptr, &status);
            if (status != 0 || demangled == nullptr) {
                if (demangled) {
                    std::free(demangled);
                }
                return std::nullopt;
            }
            std::string result{demangled};
            std::free(demangled);
            return result;
        }

    } // anonymous namespace

#endif

    std::optional<std::string> TryDemangle(std::string_view mangled) noexcept {
        return Undecorate(mangled);
    }

    std::string Demangle(std::string_view mangled) {
        if (auto r = Undecorate(mangled)) {
            return *r;
        }
        return std::string{mangled};
    }

} // namespace Sora::Meta::ABI::Runtime

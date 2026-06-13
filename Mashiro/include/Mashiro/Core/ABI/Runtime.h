/**
 * @file Runtime.h
 * @brief Runtime demangle wrapper — DbgHelp on Windows, libc++abi on POSIX.
 *
 * The compile-time `Mangle` direction is `consteval` and lives in
 * @ref ABI::Itanium and @ref ABI::MSVC. This file provides the *runtime*
 * inverse: given a mangled symbol string (typically captured from a linker
 * map, crash dump, or `typeid().raw_name()`), recover the human form.
 *
 * Backend selection is a compile-time `#if`:
 * - **Windows / MSVC ABI** → `UnDecorateSymbolName` (DbgHelp). Calls are
 *   serialised behind an internal mutex (DbgHelp is not thread-safe).
 * - **POSIX / Itanium ABI** → `abi::__cxa_demangle`. The returned `malloc`
 *   buffer is freed before this function returns.
 *
 * Exactly one backend compiles per target; there is no runtime branch.
 *
 * @ingroup Core
 */
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace Mashiro::ABI::Runtime {

    /**
     * @brief Demangle @p mangled, or return @p mangled unchanged on failure.
     *
     * Convenient when you need a printable form regardless of whether the
     * input was mangled — e.g. logging or stack-trace pretty-print.
     */
    [[nodiscard]] std::string Demangle(std::string_view mangled);

    /**
     * @brief Demangle @p mangled, or `nullopt` if the platform demangler
     *        could not parse it (unknown encoding, bad input, OOM).
     *
     * Use this when failure must be observable — e.g. round-trip tests that
     * verify a hand-built mangled string is well-formed.
     */
    [[nodiscard]] std::optional<std::string> TryDemangle(std::string_view mangled) noexcept;

} // namespace Mashiro::ABI::Runtime

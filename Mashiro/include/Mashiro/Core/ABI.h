/**
 * @file ABI.h
 * @brief Reflection-driven, compile-time C++ name mangling (Itanium + MSVC).
 *
 * Given a `std::meta::info` (a P2996 reflection of a class, function, or
 * variable), `ABI::Itanium::Mangle(...)` and `ABI::MSVC::Mangle(...)` produce
 * the corresponding mangled symbol name as a static-storage `std::string_view`
 * — the entire computation is `consteval`, with **zero** runtime cost.
 *
 * The two manglers are intentionally independent: the ABIs' compression
 * schemes do not unify (Itanium uses an unbounded base-36 substitution
 * dictionary; MSVC uses two fixed 10-slot back-reference tables), and forcing
 * them through one visitor would tangle the most bug-prone code on each side.
 * They share only `Detail::*` — a stateless reflection-classification layer.
 *
 * Runtime demangling lives in @ref Mashiro::ABI::Runtime — Windows uses
 * DbgHelp's `UnDecorateSymbolName`, POSIX uses `abi::__cxa_demangle`.
 *
 * @see docs/superpowers/specs/2026-06-13-abi-name-mangling-design.md
 * @ingroup Core
 */
#pragma once

#include <meta>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Mashiro::ABI {

    // =========================================================================
    // Public surface
    // =========================================================================

    /// @brief Which ABI's mangling scheme to apply.
    enum class Kind { Itanium, MSVC };

    // =========================================================================
    // Detail — shared, stateless reflection classification
    // =========================================================================

    /** @cond INTERNAL */
    namespace Detail {

        /**
         * @brief Builtin-type tag.
         *
         * Reflection `info` equality compares the *type entity*, not its size,
         * so `^^int != ^^long` even on a 64-bit LLP64 target where they share
         * a layout. We classify a builtin by comparing the dealiased,
         * cv-stripped reflection against a fixed `^^`-reference table; this
         * is the central technique that lets us emit Itanium `i` vs `l`,
         * MSVC `H` vs `J`, and distinguish `char` / `signed char` /
         * `unsigned char` as the three distinct types both ABIs require.
         */
        enum class Builtin {
            None = 0,
            Void, Bool,
            Char, SChar, UChar,
            Char8, Char16, Char32, WChar,
            Short, UShort, Int, UInt, Long, ULong, LongLong, ULongLong,
            Int128, UInt128,
            Float, Double, LongDouble,
            NullPtr,
        };

        /// @brief Classify a reflected type as one of the @ref Builtin tags.
        ///
        /// The input is dealiased and cv-stripped before comparison. Returns
        /// @ref Builtin::None for any non-builtin (class/enum/pointer/...).
        consteval Builtin ClassifyBuiltin(std::meta::info t) {
            t = std::meta::remove_cv(std::meta::dealias(t));
            // Order: most common first; identity comparison is exact and cheap.
            if (t == ^^void)               return Builtin::Void;
            if (t == ^^bool)               return Builtin::Bool;
            if (t == ^^char)               return Builtin::Char;
            if (t == ^^signed char)        return Builtin::SChar;
            if (t == ^^unsigned char)      return Builtin::UChar;
            if (t == ^^char8_t)            return Builtin::Char8;
            if (t == ^^char16_t)           return Builtin::Char16;
            if (t == ^^char32_t)           return Builtin::Char32;
            if (t == ^^wchar_t)            return Builtin::WChar;
            if (t == ^^short)              return Builtin::Short;
            if (t == ^^unsigned short)     return Builtin::UShort;
            if (t == ^^int)                return Builtin::Int;
            if (t == ^^unsigned int)       return Builtin::UInt;
            if (t == ^^long)               return Builtin::Long;
            if (t == ^^unsigned long)      return Builtin::ULong;
            if (t == ^^long long)          return Builtin::LongLong;
            if (t == ^^unsigned long long) return Builtin::ULongLong;
            if (t == ^^__int128)           return Builtin::Int128;
            if (t == ^^unsigned __int128)  return Builtin::UInt128;
            if (t == ^^float)              return Builtin::Float;
            if (t == ^^double)             return Builtin::Double;
            if (t == ^^long double)        return Builtin::LongDouble;
            if (t == ^^decltype(nullptr))  return Builtin::NullPtr;
            return Builtin::None;
        }

        /// @brief Walk `parent_of` until the global namespace, returning the
        ///        chain innermost-first (entity, parent, grandparent, ...,
        ///        last named scope). Mirrors Traits::Detail::ScopeChain.
        consteval std::vector<std::meta::info> ScopeChain(std::meta::info t) {
            std::vector<std::meta::info> chain;
            chain.push_back(t);
            auto parent = std::meta::parent_of(t);
            while (parent != ^^:: && std::meta::has_identifier(parent)) {
                chain.push_back(parent);
                parent = std::meta::parent_of(parent);
            }
            return chain;
        }

        /// @brief Append the base-10 decimal representation of @p n to @p out.
        constexpr void AppendDecimal(std::string& out, size_t n) {
            if (n == 0) { out += '0'; return; }
            char buf[24];
            size_t i = 0;
            while (n > 0) { buf[i++] = char('0' + n % 10); n /= 10; }
            while (i > 0) out += buf[--i];
        }

        /// @brief Append @p n in base-36 using digits 0-9A-Z (Itanium seq-id).
        constexpr void AppendBase36(std::string& out, size_t n) {
            constexpr char kDigits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
            if (n == 0) { out += '0'; return; }
            char buf[16];
            size_t i = 0;
            while (n > 0) { buf[i++] = kDigits[n % 36]; n /= 36; }
            while (i > 0) out += buf[--i];
        }

        /// @brief Append `<n><identifier>` (Itanium source-name production).
        constexpr void AppendSourceName(std::string& out, std::string_view id) {
            AppendDecimal(out, id.size());
            out += id;
        }

    } // namespace Detail
    /** @endcond */

} // namespace Mashiro::ABI

#include "Mashiro/Core/ABI/Itanium.h"
#include "Mashiro/Core/ABI/MSVC.h"
#include "Mashiro/Core/ABI/Runtime.h"

namespace Mashiro::ABI {

    // =========================================================================
    // Generic entry — dispatch on Kind
    // =========================================================================

    /**
     * @brief Mangle @p entity under ABI @p K. `consteval` — folds entirely.
     *
     * @tparam K Either @ref Kind::Itanium or @ref Kind::MSVC.
     * @param entity A reflection of a type, function, or variable.
     * @return A `std::string_view` over static storage (P3491
     *         `define_static_string`), so the result is a stable
     *         compile-time constant.
     */
    template <Kind K>
    [[nodiscard]] consteval std::string_view Mangle(std::meta::info entity) {
        if constexpr (K == Kind::Itanium) return Itanium::Mangle(entity);
        else                              return MSVC::Mangle(entity);
    }

} // namespace Mashiro::ABI

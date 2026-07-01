/**
 * @file ABI.h
 * @brief Reflection-driven, compile-time C++ name mangling for Itanium and MSVC ABIs.
 * @ingroup Core
 *
 * @details Given a @c std::meta::info, @c ABI::Itanium::Mangle(...) and @c ABI::MSVC::Mangle(...) produce the
 * corresponding mangled symbol name as a static-storage @c std::string_view. The entire computation is @c consteval
 * and has no runtime cost.
 *
 * The two manglers are intentionally independent: the ABIs' compression schemes do not unify. Itanium uses an
 * unbounded base-36 substitution dictionary; MSVC uses two fixed 10-slot back-reference tables. Forcing them through
 * one visitor would tangle the most bug-prone code on each side. They share only @c Detail::*, a stateless
 * reflection-classification layer.
 *
 * Runtime demangling lives in @ref Sora::ABI::Runtime. Windows uses DbgHelp's @c UnDecorateSymbolName; POSIX uses
 * @c abi::__cxa_demangle.
 *
 * @see docs/superpowers/specs/2026-06-13-abi-name-mangling-design.md
 */
#pragma once

#include <cstddef>
#include <meta>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Sora::Meta::ABI {

    /** @brief Which ABI's mangling scheme to apply. */
    enum class Kind : uint8_t { Itanium, MSVC };

    /** @cond INTERNAL */
    namespace Detail {

        /**
         * @brief Builtin-type tag.
         *
         * @details Reflection @c info equality compares the type entity, not its size, so @c ^^int and @c ^^long remain
         * distinct even on targets where they share a layout. Builtins are classified by comparing the dealiased,
         * cv-stripped reflection against a fixed @c ^^ reference table; this lets the manglers distinguish Itanium
         * @c i versus @c l, MSVC @c H versus @c J, and @c char / @c signed @c char / @c unsigned @c char.
         */
        enum class Builtin : uint8_t {
            None = 0,
            Void,
            Bool,
            Char,
            SChar,
            UChar,
            Char8,
            Char16,
            Char32,
            WChar,
            Short,
            UShort,
            Int,
            UInt,
            Long,
            ULong,
            LongLong,
            ULongLong,
            Int128,
            UInt128,
            Float,
            Double,
            LongDouble,
            NullPtr,
        };

        /**
         * @brief Classify a reflected type as one of the @ref Builtin tags.
         * @param[in] t Reflected type entity.
         * @return The builtin tag, or @ref Builtin::None for non-builtin types.
         */
        consteval Builtin ClassifyBuiltin(std::meta::info t) {
            t = std::meta::remove_cv(std::meta::dealias(t));
            // Order: most common first; identity comparison is exact and cheap.
            if (t == ^^void) {
                return Builtin::Void;
            }
            if (t == ^^bool) {
                return Builtin::Bool;
            }
            if (t == ^^char) {
                return Builtin::Char;
            }
            if (t == ^^signed char) {
                return Builtin::SChar;
            }
            if (t == ^^unsigned char) {
                return Builtin::UChar;
            }
            if (t == ^^char8_t) {
                return Builtin::Char8;
            }
            if (t == ^^char16_t) {
                return Builtin::Char16;
            }
            if (t == ^^char32_t) {
                return Builtin::Char32;
            }
            if (t == ^^wchar_t) {
                return Builtin::WChar;
            }
            if (t == ^^short) {
                return Builtin::Short;
            }
            if (t == ^^unsigned short) {
                return Builtin::UShort;
            }
            if (t == ^^int) {
                return Builtin::Int;
            }
            if (t == ^^unsigned int) {
                return Builtin::UInt;
            }
            if (t == ^^long) {
                return Builtin::Long;
            }
            if (t == ^^unsigned long) {
                return Builtin::ULong;
            }
            if (t == ^^long long) {
                return Builtin::LongLong;
            }
            if (t == ^^unsigned long long) {
                return Builtin::ULongLong;
            }
            if (t == ^^__int128) {
                return Builtin::Int128;
            }
            if (t == ^^unsigned __int128) {
                return Builtin::UInt128;
            }
            if (t == ^^float) {
                return Builtin::Float;
            }
            if (t == ^^double) {
                return Builtin::Double;
            }
            if (t == ^^long double) {
                return Builtin::LongDouble;
            }
            if (t == ^^decltype(nullptr)) {
                return Builtin::NullPtr;
            }
            return Builtin::None;
        }

        /**
         * @brief Walk @c parent_of until the global namespace.
         * @param[in] t Reflected entity.
         * @return The scope chain in innermost-first order.
         */
        consteval std::vector<std::meta::info> ScopeChain(std::meta::info t) {
            std::vector<std::meta::info> chain;
            chain.push_back(t);
            auto parent = std::meta::parent_of(t);
            while (parent != ^^::&&std::meta::has_identifier(parent)) {
                chain.push_back(parent);
                parent = std::meta::parent_of(parent);
            }
            return chain;
        }

        /**
         * @brief Append the base-10 decimal representation of @p n to @p out.
         * @param[in,out] out Destination string.
         * @param[in] n Value to append.
         */
        constexpr void AppendDecimal(std::string& out, std::size_t n) {
            if (n == 0) {
                out += '0';
                return;
            }
            char buf[24];
            std::size_t i = 0;
            while (n > 0) {
                buf[i++] = static_cast<char>('0' + n % 10);
                n /= 10;
            }
            while (i > 0) {
                out += buf[--i];
            }
        }

        /**
         * @brief Append @p n in base-36 using digits 0-9A-Z.
         * @param[in,out] out Destination string.
         * @param[in] n Value to append.
         */
        constexpr void AppendBase36(std::string& out, std::size_t n) {
            constexpr char kDigits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
            if (n == 0) {
                out += '0';
                return;
            }
            char buf[16];
            std::size_t i = 0;
            while (n > 0) {
                buf[i++] = kDigits[n % 36];
                n /= 36;
            }
            while (i > 0) {
                out += buf[--i];
            }
        }

        /**
         * @brief Append @c <n><identifier>, the Itanium source-name production.
         * @param[in,out] out Destination string.
         * @param[in] id Source identifier.
         */
        constexpr void AppendSourceName(std::string& out, std::string_view id) {
            AppendDecimal(out, id.size());
            out += id;
        }

    } // namespace Detail
    /** @endcond */

} // namespace Sora::Meta::ABI

#include "Sora/Core/ABI/Itanium.h"
#include "Sora/Core/ABI/MSVC.h"

namespace Sora::Meta::ABI {

    /**
     * @brief Mangle @p entity under ABI @p K.
     * @tparam K Either @ref Kind::Itanium or @ref Kind::MSVC.
     * @param[in] entity A reflection of a type, function, or variable.
     * @return A @c std::string_view over static storage, so the result is a stable compile-time constant.
     */
    template<Kind K>
    [[nodiscard]] consteval std::string_view Mangle(std::meta::info entity) {
        if constexpr (K == Kind::Itanium) {
            return Itanium::Mangle(entity);
        } else {
            return MSVC::Mangle(entity);
        }
    }

    /**
     * @brief Demangle @p mangled when the platform demangler accepts it.
     * @param[in] mangled Symbol spelling to demangle.
     * @return Human-readable symbol spelling, or @c std::nullopt when parsing fails.
     */
    [[nodiscard]] std::optional<std::string> TryDemangle(std::string_view mangled) noexcept;

    /**
     * @brief Demangle @p mangled, or return @p mangled unchanged on failure.
     * @param[in] mangled Symbol spelling to demangle.
     * @return Human-readable symbol spelling, or @p mangled when demangling fails.
     */
    [[nodiscard]] std::string Demangle(std::string_view mangled);

} // namespace Sora::Meta::ABI

namespace Sora::ABI {

    using Sora::Meta::ABI::Kind;
    using Sora::Meta::ABI::Mangle;
    using Sora::Meta::ABI::TryDemangle;
    using Sora::Meta::ABI::Demangle;

    namespace Itanium = Sora::Meta::ABI::Itanium;
    namespace MSVC = Sora::Meta::ABI::MSVC;

} // namespace Sora::ABI
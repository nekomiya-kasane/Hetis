/**
 * @file MSVC.h
 * @brief Reflection-driven MSVC x64 ABI name mangling.
 * @ingroup Core
 *
 * @details This consteval mangler follows clang's Microsoft ABI spelling for reflected types, variables, and
 * functions. Type output matches @c typeid(T).raw_name(); variable and function output matches linker symbol spelling.
 * The implementation carries MSVC's two fixed 10-slot back-reference tables through recursive type/name emission.
 */
#pragma once

#include "Sora/Core/ABI.h"

#include <meta>
#include <string>
#include <string_view>
#include <vector>

namespace Sora::Meta::ABI::MSVC {

    /** @cond INTERNAL */
    namespace Detail {

        /**
         * @brief Recursion state - the two MSVC back-reference tables.
         *
         * `nameBackrefs` holds the first ten distinct *name fragments* (each namespace/class identifier in a
         * qualified name) seen. `typeBackrefs` holds the first ten distinct *compound types* (named
         * class/struct/union/enum types as they appear in a parameter or operand position).
         */
        struct State {
            std::vector<std::string_view> nameBackrefs;
            std::vector<std::string_view> typeBackrefs;
        };

        /**
         * @brief Letter for a builtin (matches clang MicrosoftMangle).
         */
        consteval std::string_view BuiltinLetter(ABI::Detail::Builtin b) {
            using B = ABI::Detail::Builtin;
            switch (b) {
            case B::Void:
                return "X";
            case B::Bool:
                return "_N";
            case B::Char:
                return "D";
            case B::SChar:
                return "C";
            case B::UChar:
                return "E";
            case B::Char8:
                return "_Q";
            case B::Char16:
                return "_S";
            case B::Char32:
                return "_U";
            case B::WChar:
                return "_W";
            case B::Short:
                return "F";
            case B::UShort:
                return "G";
            case B::Int:
                return "H";
            case B::UInt:
                return "I";
            case B::Long:
                return "J";
            case B::ULong:
                return "K";
            case B::LongLong:
                return "_J";
            case B::ULongLong:
                return "_K";
            // __int128 / unsigned __int128 verified via clang-cl + llvm-nm:
            //   `__int128 g` -> `?g@@3_LA`, `unsigned __int128 g` -> `?g@@3_MA`.
            case B::Int128:
                return "_L";
            case B::UInt128:
                return "_M";
            case B::Float:
                return "M";
            case B::Double:
                return "N";
            case B::LongDouble:
                return "O";
            case B::NullPtr:
                return "$$T";
            default:
                return {};
            }
        }

        /**
         * @brief Class-kind tag for the named-type encoding.
         *
         * MSVC tags: `T` union, `W` enum (then `4`), `U` struct, `V` class. The struct-vs-class distinction
         * is the C++ *class-key*, which is purely syntactic (a type may even be forward-declared with either
         * key) and is not part of the type's semantic identity. The P2996 reflection in this toolchain
         * exposes no class-key intrinsic, and `display_string_of` drops it too (verified empirically).
         *
         * We approximate with the *default member access* - `struct` defaults to public, `class` to private - by
         * testing the first non-static data member. This is the conventional heuristic; it is correct for the
         * overwhelmingly common case where the first member's access is left at the class-key default. It can
         * misfire on a type whose first member's access is explicitly flipped (e.g. a `struct` opening with
         * a `private:` section), which no reflection facility can currently disambiguate. Empty/member-less
         * classes are reported as `V` (class), matching clang's mangling of an empty `class`/`struct` pair where
         * the distinction is unobservable.
         */
        consteval char ClassKindTag(std::meta::info t) {
            if (std::meta::is_union_type(t)) {
                return 'T';
            }
            if (std::meta::is_enum_type(t)) {
                return 'W'; // followed by '4'
            }
            auto members = std::meta::nonstatic_data_members_of(t, std::meta::access_context::unchecked());
            if (!members.empty() && std::meta::is_public(members[0])) {
                return 'U';
            }
            return 'V';
        }

        /**
         * @brief Append a length-quantified name fragment with back-ref reuse.
         */
        consteval void AppendNameFragment(std::string& out, std::string_view id, State& st) {
            for (size_t i = 0; i < st.nameBackrefs.size(); ++i) {
                if (st.nameBackrefs[i] == id) {
                    out += char('0' + i);
                    return;
                }
            }
            out += id;
            out += '@';
            if (st.nameBackrefs.size() < 10) {
                st.nameBackrefs.push_back(id);
            }
        }

        /**
         * @brief Append the qualified name `frag@frag@...@@` (innermost first,
         *        outermost-namespace fragment is closest to the trailing `@@`).
         */
        consteval void AppendQualifiedName(std::string& out, std::meta::info t, State& st) {
            auto chain = Sora::Meta::ScopeChainOf(t);
            // chain[0] is innermost (the entity itself); MSVC writes innermost first.
            for (auto info : chain) {
                std::string_view id =
                    std::meta::has_identifier(info) ? std::meta::identifier_of(info) : std::string_view{"?A"};
                AppendNameFragment(out, id, st);
            }
            out += '@'; // closes the qualified-name list (paired with last frag's @ -> @@)
        }

        /** @brief Forward declaration for the recursive type encoder. */
        consteval void AppendType(std::string& out, std::meta::info t, State& st);

        /**
         * @brief Encode a class/struct/union/enum operand as `?A<tag><qname>`.
         */
        consteval void AppendNamedType(std::string& out, std::meta::info t, State& st) {
            std::string head;
            head += "?A";
            char tag = ClassKindTag(t);
            head += tag;
            if (tag == 'W') {
                head += '4';
            }
            AppendQualifiedName(head, t, st);
            out += head;
        }

        /**
         * @brief MSVC cv-modifier letter: `A` none, `B` const, `C` volatile, `D` const volatile. Used both for
         *        indirection-pointee cv and for a variable's storage-class code.
         */
        consteval char CvLetter(std::meta::info t) {
            const bool c = std::meta::is_const_type(t);
            const bool v = std::meta::is_volatile_type(t);
            if (c && v) {
                return 'D';
            }
            if (v) {
                return 'C';
            }
            if (c) {
                return 'B';
            }
            return 'A';
        }

        /**
         * @brief Encode any type in operand position.
         */
        consteval void AppendType(std::string& out, std::meta::info t, State& st) {
            t = std::meta::dealias(t);

            // Builtin
            auto b = ABI::Detail::ClassifyBuiltin(t);
            if (b != ABI::Detail::Builtin::None) {
                out += BuiltinLetter(b);
                return;
            }

            // Pointer - `PE<cv><pointee>` (`E` = __ptr64 on x64).
            if (std::meta::is_pointer_type(t)) {
                auto pointee = std::meta::dealias(std::meta::remove_pointer(t));
                out += "PE";
                out += CvLetter(pointee);
                AppendType(out, std::meta::remove_cv(pointee), st);
                return;
            }

            // Lvalue reference - `AE<cv><referent>`.
            if (std::meta::is_lvalue_reference_type(t)) {
                auto referent = std::meta::dealias(std::meta::remove_reference(t));
                out += "AE";
                out += CvLetter(referent);
                AppendType(out, std::meta::remove_cv(referent), st);
                return;
            }

            // Rvalue reference - `$$QE<cv><referent>`.
            if (std::meta::is_rvalue_reference_type(t)) {
                auto referent = std::meta::dealias(std::meta::remove_reference(t));
                out += "$$QE";
                out += CvLetter(referent);
                AppendType(out, std::meta::remove_cv(referent), st);
                return;
            }

            // Class/struct/union/enum: ?A<tag><qname>
            if (std::meta::is_class_type(t) || std::meta::is_enum_type(t) || std::meta::is_union_type(t)) {
                AppendNamedType(out, std::meta::remove_cv(t), st);
                return;
            }
        }

    } // namespace Detail
    /** @endcond */

    /**
     * @brief Mangle a reflected entity under MSVC ABI rules.
     * @param[in] entity Reflected type, variable, or function entity.
     * @return Static-storage mangled symbol spelling.
     */
    [[nodiscard]] consteval std::string_view Mangle(std::meta::info entity) {
        std::string out;
        out.reserve(64);

        if (std::meta::is_type(entity)) {
            // typeid form: leading '.', then the operand encoding.
            out += '.';
            Detail::State st{};
            Detail::AppendType(out, entity, st);
        } else if (std::meta::is_variable(entity)) {
            Detail::State st{};
            out += '?';
            // Innermost name is the variable identifier; rest is the scope path.
            std::string_view id = std::meta::identifier_of(entity);
            Detail::AppendNameFragment(out, id, st);
            // Walk parent scope chain
            auto p = std::meta::parent_of(entity);
            while (p != ^^::&&std::meta::has_identifier(p)) {
                Detail::AppendNameFragment(out, std::meta::identifier_of(p), st);
                p = std::meta::parent_of(p);
            }
            out += '@';
            // Variable: '3' (normal data), then the data type, then storage.
            out += '3';
            auto type = std::meta::dealias(std::meta::type_of(entity));
            Detail::AppendType(out, std::meta::remove_cv(type), st);
            // Storage class. For a plain object it is the variable's own cv letter (`HA`/`HB`/`HC`). For a
            // pointer/reference object MSVC instead emits `E` + the *pointee/referent* cv letter - e.g.
            // `int* p` -> `PEAHEA`, `const int* p` -> `PEBHEB`, `volatile int* p` -> `PECHEC` (verified via
            // llvm-nm on real .obj).
            if (std::meta::is_pointer_type(type)) {
                out += 'E';
                out += Detail::CvLetter(std::meta::dealias(std::meta::remove_pointer(type)));
            } else if (std::meta::is_lvalue_reference_type(type) || std::meta::is_rvalue_reference_type(type)) {
                out += 'E';
                out += Detail::CvLetter(std::meta::dealias(std::meta::remove_reference(type)));
            } else {
                out += Detail::CvLetter(type);
            }
        } else if (std::meta::is_function(entity)) {
            Detail::State st{};
            out += '?';
            std::string_view id = std::meta::identifier_of(entity);
            Detail::AppendNameFragment(out, id, st);
            auto p = std::meta::parent_of(entity);
            const bool isMember = std::meta::is_class_member(entity);
            const bool isStatic = isMember && std::meta::is_static_member(entity);
            while (p != ^^::&&std::meta::has_identifier(p)) {
                Detail::AppendNameFragment(out, std::meta::identifier_of(p), st);
                p = std::meta::parent_of(p);
            }
            out += '@';

            if (!isMember) {
                // Free function: `Y` storage, `A` = __cdecl on x64.
                out += "YA";
            } else if (isStatic) {
                // Static member: `S` storage; no `this`, no cv/ref-quals.
                out += "SA";
            } else {
                // Non-static member. Access+virtual letter, then `E` (__ptr64), an optional ref-qualifier letter,
                // the `this` cv letter, and finally `A` for __cdecl. Verified via llvm-nm:
                //   public          -> Q   protected -> I   private -> A
                //   public virtual  -> U
                //   `&`  -> G   `&&` -> H   (none -> nothing)
                //   cv via CvLetter (A/B/C/D)
                // e.g. `void f() const &` (public) -> `QEGB` + `A`.
                if (std::meta::is_virtual(entity)) {
                    out += 'U';
                } else if (std::meta::is_public(entity)) {
                    out += 'Q';
                } else if (std::meta::is_protected(entity)) {
                    out += 'I';
                } else {
                    out += 'A';
                }
                out += 'E';
                if (std::meta::is_lvalue_reference_qualified(entity)) {
                    out += 'G';
                } else if (std::meta::is_rvalue_reference_qualified(entity)) {
                    out += 'H';
                }
                out += std::meta::is_volatile(entity) ? (std::meta::is_const(entity) ? 'D' : 'C')
                                                      : (std::meta::is_const(entity) ? 'B' : 'A');
                out += 'A'; // __cdecl
            }

            // Return type, then parameters (`X` shortcut for a void list).
            Detail::AppendType(out, std::meta::return_type_of(entity), st);
            auto params = std::meta::parameters_of(entity);
            if (params.empty()) {
                out += 'X';
            } else {
                for (auto p2 : params) {
                    Detail::AppendType(out, std::meta::type_of(p2), st);
                }
                out += '@';
            }
            out += 'Z';
        }

        return std::define_static_string(out);
    }

} // namespace Sora::Meta::ABI::MSVC

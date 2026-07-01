/**
 * @file Itanium.h
 * @brief Reflection-driven Itanium C++ ABI name mangling.
 * @ingroup Core
 *
 * @details This consteval mangler implements the Itanium C++ ABI mangling grammar used by GCC/Clang-style targets. It
 * emits bare type productions for reflected types, @c _Z-prefixed names for variables, and @c _Z-prefixed names plus
 * bare function parameter types for functions. Substitution state is keyed by reflection identity and uses Itanium's
 * base-36 sequence identifiers.
 */
#pragma once

#include "Sora/Core/ABI.h"

#include <meta>
#include <string>
#include <string_view>
#include <vector>

namespace Sora::Meta::ABI::Itanium {

    /** @cond INTERNAL */
    namespace Detail {

        /**
         * @brief Substitution dictionary - candidates in first-appearance order, compared by reflection identity
         *        (`==` on `info`).
         *
         * A *candidate* is anything the ABI says may be substituted: every non-builtin `<type>`, and every `<prefix>`
         * component (each enclosing namespace/class qualifier of a nested name). Builtin types and the
         * standard-substitution abbreviations are never candidates.
         */
        struct State {
            std::vector<std::meta::info> subs;
        };

        // clang-format off
        /**
         * @brief Itanium builtin code (ABI section 5.1.5 "Builtin types").
         */
        consteval std::string_view BuiltinCode(ABI::Detail::Builtin b) {
            using B = ABI::Detail::Builtin;
            switch (b) {
                case B::Void:       return "v";
                case B::Bool:       return "b";
                case B::Char:       return "c";
                case B::SChar:      return "a";
                case B::UChar:      return "h";
                case B::Char8:      return "Du";
                case B::Char16:     return "Ds";
                case B::Char32:     return "Di";
                case B::WChar:      return "w";
                case B::Short:      return "s";
                case B::UShort:     return "t";
                case B::Int:        return "i";
                case B::UInt:       return "j";
                case B::Long:       return "l";
                case B::ULong:      return "m";
                case B::LongLong:   return "x";
                case B::ULongLong:  return "y";
                case B::Int128:     return "n";
                case B::UInt128:    return "o";
                case B::Float:      return "f";
                case B::Double:     return "d";
                case B::LongDouble: return "e";
                case B::NullPtr:    return "Dn";
                default:            return {};
            }
        }
        // clang-format on

        /**
         * @brief Append a substitution reference for dictionary index @p i: index 0 -> `S_`, index n ->
         *        `S<base36(n-1)>_`.
         */
        consteval void AppendSubRef(std::string& out, size_t i) {
            out += 'S';
            if (i > 0) {
                ABI::Detail::AppendBase36(out, i - 1);
            }
            out += '_';
        }

        /**
         * @brief If @p key is already a substitution candidate, emit its reference and return true; otherwise return
         *        false. The caller emits the full encoding and then registers the key.
         */
        consteval bool TryEmitSub(std::string& out, std::meta::info key, State& st) {
            for (size_t i = 0; i < st.subs.size(); ++i) {
                if (st.subs[i] == key) {
                    AppendSubRef(out, i);
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief Register @p key as a new substitution candidate. Callers only register after a miss.
         */
        consteval void AddSub(std::meta::info key, State& st) {
            st.subs.push_back(key);
        }

        /**
         * @brief Standard-substitution abbreviation for a `std::` entity, or empty if none applies. These are fixed
         *        two-letter codes that do not consume a numbered dictionary slot.
         *
         * `St` = `::std`, `Sa` = `std::allocator`, `Ss` = `std::string`, `Sb` = `std::basic_string`,
         * `Si`/`So`/`Sd` = the common iostream specializations. We recognise the namespace and the allocator here; the
         * string/stream abbreviations require template-arg shape checks deferred to the template-id path.
         */
        consteval std::string_view StdAbbrev(std::meta::info t) {
            if (t == ^^std) {
                return "St";
            }
            return {};
        }

        /** @brief Forward declaration for the recursive type encoder. */
        consteval void AppendType(std::string& out, std::meta::info t, State& st);

        /**
         * @brief Emit one prefix component (a namespace or enclosing class) with substitution. @p key is the
         *        reflection of the *scope up to and including* this component, so distinct nesting depths get distinct
         *        dictionary entries. The `std` namespace folds to the standard `St` abbreviation, which never occupies
         *        a numbered slot.
         */
        consteval void AppendPrefixComponent(std::string& out, std::meta::info key, State& st) {
            if (auto ab = StdAbbrev(key); !ab.empty()) {
                out += ab;
                return;
            }
            if (TryEmitSub(out, key, st)) {
                return;
            }
            ABI::Detail::AppendSourceName(out, std::meta::identifier_of(key));
            AddSub(key, st);
        }

        /**
         * @brief Encode a class/struct/union/enum type by name, with full nested-name and substitution handling.
         *
         * Global-scope `A` -> `1A`. Nested `ns::Foo` -> `N2ns3FooE`. The whole type, and every enclosing prefix, are
         * substitution candidates; a repeat of any of them collapses to an `S`-reference.
         */
        consteval void AppendNamedType(std::string& out, std::meta::info t, State& st) {
            // A repeat of the whole type substitutes directly.
            if (TryEmitSub(out, t, st)) {
                return;
            }

            auto chain = ABI::Detail::ScopeChain(t); // innermost-first: [t, ..., outer]
            if (chain.size() == 1) {
                // Unqualified (global-scope) name: bare source-name.
                ABI::Detail::AppendSourceName(out, std::meta::identifier_of(t));
                AddSub(t, st);
                return;
            }

            // Nested name `N <prefix...> <unqualified-name> E`. Walk the chain outermost->innermost; each element is
            // the scope-up-to-that-point and serves as both substitution key and identifier source. The final element
            // is `t` itself, so it is registered here too.
            out += 'N';
            for (size_t i = chain.size(); i-- > 0;) {
                AppendPrefixComponent(out, chain[i], st);
            }
            out += 'E';
        }

        /**
         * @brief Encode any type. Recursive; threads the substitution state.
         *
         * Order of the ABI's `<type>` grammar that we implement:
         *   - cv-qualified type -> `[r]V K <type>` (restrict/volatile/const), and the qualified form is itself a
         *     substitution candidate;
         *   - builtin -> single/double-letter code (never a candidate);
         *   - pointer `P`, lvalue-ref `R`, rvalue-ref `O` -> prefix + pointee, and each compound is a candidate;
         *   - array `A<n>_<elem>` -> candidate;
         *   - class/enum/union -> nested-name with its own substitution logic.
         */
        consteval void AppendType(std::string& out, std::meta::info t, State& st) {
            t = std::meta::dealias(t);

            // Top-level cv qualifiers. The qualified type is a distinct substitution candidate from its unqualified
            // form.
            const bool isConst = std::meta::is_const_type(t);
            const bool isVol = std::meta::is_volatile_type(t);
            if (isConst || isVol) {
                if (TryEmitSub(out, t, st)) {
                    return;
                }
                std::string inner;
                if (isVol) {
                    inner += 'V';
                }
                if (isConst) {
                    inner += 'K';
                }
                State probe = st; // emit qualifier letters, then the base type
                AppendType(inner, std::meta::remove_cv(t), probe);
                st = probe;
                out += inner;
                AddSub(t, st);
                return;
            }

            // Builtin - never substituted, emitted inline.
            auto b = ABI::Detail::ClassifyBuiltin(t);
            if (b != ABI::Detail::Builtin::None) {
                out += BuiltinCode(b);
                return;
            }

            // Pointer / references - compound, each a candidate.
            if (std::meta::is_pointer_type(t)) {
                if (TryEmitSub(out, t, st)) {
                    return;
                }
                out += 'P';
                AppendType(out, std::meta::remove_pointer(t), st);
                AddSub(t, st);
                return;
            }
            if (std::meta::is_lvalue_reference_type(t)) {
                if (TryEmitSub(out, t, st)) {
                    return;
                }
                out += 'R';
                AppendType(out, std::meta::remove_reference(t), st);
                AddSub(t, st);
                return;
            }
            if (std::meta::is_rvalue_reference_type(t)) {
                if (TryEmitSub(out, t, st)) {
                    return;
                }
                out += 'O';
                AppendType(out, std::meta::remove_reference(t), st);
                AddSub(t, st);
                return;
            }

            // Array `A <dimension> _ <element-type>`.
            if (std::meta::is_array_type(t)) {
                if (TryEmitSub(out, t, st)) {
                    return;
                }
                out += 'A';
                ABI::Detail::AppendDecimal(out, std::meta::extent(t, 0));
                out += '_';
                AppendType(out, std::meta::remove_extent(t), st);
                AddSub(t, st);
                return;
            }

            // Class / struct / union / enum - nested name.
            if (std::meta::is_class_type(t) || std::meta::is_enum_type(t) || std::meta::is_union_type(t)) {
                AppendNamedType(out, t, st);
                return;
            }
        }

        /**
         * @brief Encode the *name* of a function or variable entity (the `<name>` production), with nested-name and
         *        substitution.
         *
         * Free/global `g` -> `1g`. Namespaced `ns::f` -> `N2ns1fE`. The entity's own identifier is the innermost
         * component; enclosing namespaces and classes are prefix components (substitution candidates), and the leaf
         * identifier is not itself registered because it cannot recur as a prefix.
         */
        consteval void AppendEntityName(std::string& out, std::meta::info entity, State& st) {
            // Build the scope chain of *enclosing* scopes (exclude the entity).
            std::vector<std::meta::info> prefix;
            auto p = std::meta::parent_of(entity);
            while (p != ^^::&&std::meta::has_identifier(p)) {
                prefix.push_back(p);
                p = std::meta::parent_of(p);
            }
            std::string_view leaf = std::meta::identifier_of(entity);

            if (prefix.empty()) {
                ABI::Detail::AppendSourceName(out, leaf);
                return;
            }
            out += 'N';
            for (size_t i = prefix.size(); i-- > 0;) {
                AppendPrefixComponent(out, prefix[i], st);
            }
            ABI::Detail::AppendSourceName(out, leaf);
            out += 'E';
        }

    } // namespace Detail
    /** @endcond */

    /**
     * @brief Mangle a reflected entity under Itanium ABI rules.
     * @param[in] entity Reflected type, variable, or function entity.
     * @return Static-storage mangled symbol spelling.
     */
    [[nodiscard]] consteval std::string_view Mangle(std::meta::info entity) {
        std::string out;
        out.reserve(64);

        if (std::meta::is_type(entity)) {
            Detail::State st{};
            Detail::AppendType(out, entity, st);
        } else if (std::meta::is_variable(entity)) {
            Detail::State st{};
            out += "_Z";
            Detail::AppendEntityName(out, entity, st);
        } else if (std::meta::is_function(entity)) {
            Detail::State st{};
            out += "_Z";
            Detail::AppendEntityName(out, entity, st);
            // Bare-function-type: parameter types in order. An empty list is encoded as a single `v` (void), per the
            // ABI.
            auto params = std::meta::parameters_of(entity);
            if (params.empty()) {
                out += 'v';
            } else {
                for (auto pr : params) {
                    Detail::AppendType(out, std::meta::type_of(pr), st);
                }
            }
        }

        return std::define_static_string(out);
    }

} // namespace Sora::Meta::ABI::Itanium

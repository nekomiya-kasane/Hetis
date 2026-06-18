/**
 * @file MetaCore.h
 * @brief The rodata layer of the three-layer MetaClass (D18).
 *
 * MetaCore is the immutable, compile-time-baked descriptor for a Yuki class — its IID,
 * reflected display name, role, and flattened (interface, @ref Detail::SealFlags) /
 * (nucleus, eager) edge arrays. Built entirely at constant evaluation from the type's
 * @ref Anno::Implements / @ref Anno::Extends / @ref Anno::Final / @ref Anno::Unique /
 * @ref Anno::Important annotations, then deposited into `.rodata` by static-initialised
 * @c std::array<> arrays whose lifetime is the program's. The two runtime-mutable layers
 * (MetaLinks RCU, MetaDynamic instance) hang off this fixed base in Tasks 8 and 9.
 *
 * Every facility below is @c consteval / @c inline @c constexpr — including
 * @ref Detail::YObjectIsPublic, the predicate Tasks 8 and 9 wrap in a @c static_assert
 * to enforce D3's "kMetaCore must be public" invariant.
 *
 * @ingroup Core
 */
#pragma once

#include <Yuki/Core/Identity.h>

#include <array>
#include <cstddef>
#include <meta>
#include <string_view>
#include <type_traits>

namespace Yuki {

    /// @brief One flattened entry of @ref MetaCore::implements — interface IID + seal flags (D7).
    struct ImplementsInfo {
        Iid              iid;    ///< IID of the implemented interface.
        Detail::SealFlags flags;  ///< Final / Unique / Important seal flags for the (T, I) pair.
    };

    /// @brief One flattened entry of @ref MetaCore::extends — nucleus IID + eager-materialise bit.
    struct ExtendsInfo {
        Iid  nucleusIid;  ///< IID of the extended nucleus type.
        bool eager;       ///< @c true if @ref Anno::Eager wins for the extension declaring this edge.
    };

    /**
     * @brief Rodata-resident metadata for a Yuki class (D18 layer 1 of 3).
     *
     * Plain, structurally-typed POD: the @c std::array<>s backing @ref implements and
     * @ref extends live as inline-constexpr template variables (see @ref Detail::kImplementsArr
     * and @ref Detail::kExtendsArr), so the pointers stamped here are stable for the lifetime
     * of the program. No allocation, no runtime initialisation.
     */
    struct MetaCore {
        Iid                   iid;              ///< This class's IID.
        std::string_view      name;             ///< Reflected display name (qualified).
        ClassType             role;             ///< Object-model role.
        const ImplementsInfo* implements;       ///< Flattened implements array (rodata).
        std::size_t           implementsCount;  ///< Element count of @ref implements.
        const ExtendsInfo*    extends;          ///< Flattened extends array (rodata).
        std::size_t           extendsCount;     ///< Element count of @ref extends.
    };

    /** @cond INTERNAL */
    namespace Detail {

        /// @brief Sum the @c InfoList sizes across every @ref Anno::Implements annotation on @p T.
        template<class T>
        consteval std::size_t CountImplements() {
            std::size_t n = 0;
            for (auto a : std::meta::annotations_of(^^T, ^^Anno::Implements)) {
                n += std::meta::extract<Anno::Implements>(a).ifaces.size();
            }
            return n;
        }

        /// @brief Read @ref SealFlags stamped on @p T for the interface denoted by reflection @p iface.
        ///
        /// @note This *mirrors* @ref Yuki::Detail::SealFlagsFor "SealFlagsFor<T, I>" (Identity.h)
        ///       but takes a @c std::meta::info instead of a type parameter. The duplication is
        ///       necessary, not accidental: the bake loops in @ref MakeImplementsArrayFor iterate
        ///       reflections produced by @c Anno::Implements{InfoList}.ifaces, and you cannot
        ///       template-instantiate over a runtime-iterated reflection at constant evaluation.
        ///       Do not consolidate these two into one.
        ///
        /// @note Parentheses around @c (^^Anno::Final) etc. are load-bearing — see the matching
        ///       note on @ref Yuki::Detail::SealFlagsFor "SealFlagsFor<T, I>" in Identity.h.
        template<class T>
        consteval SealFlags SealFlagsForInfo(std::meta::info iface) {
            SealFlags f{};
            for (auto a : std::meta::annotations_of(^^T)) {
                auto t = std::meta::type_of(a);
                if (t == (^^Anno::Final)
                    && std::meta::extract<Anno::Final>(a).iface == iface) {
                    f.final = true;
                } else if (t == (^^Anno::Unique)
                    && std::meta::extract<Anno::Unique>(a).iface == iface) {
                    f.unique = true;
                } else if (t == (^^Anno::Important)
                    && std::meta::extract<Anno::Important>(a).iface == iface) {
                    f.important = true;
                }
            }
            return f;
        }

        /// @brief Build the flattened (iid, flags) implements array for @p T at constant evaluation.
        template<class T>
        consteval auto MakeImplementsArrayFor() {
            constexpr std::size_t N = CountImplements<T>();
            std::array<ImplementsInfo, N> out{};
            std::size_t i = 0;
            for (auto a : std::meta::annotations_of(^^T, ^^Anno::Implements)) {
                auto im = std::meta::extract<Anno::Implements>(a);
                for (auto p = im.ifaces.begin(); p != im.ifaces.end(); ++p) {
                    out[i].iid   = IidOfMeta(*p);
                    out[i].flags = SealFlagsForInfo<T>(*p);
                    ++i;
                }
            }
            return out;
        }

        /// @brief Sum the @c InfoList sizes across every @ref Anno::Extends annotation on @p T.
        template<class T>
        consteval std::size_t CountExtends() {
            std::size_t n = 0;
            for (auto a : std::meta::annotations_of(^^T, ^^Anno::Extends)) {
                n += std::meta::extract<Anno::Extends>(a).bases.size();
            }
            return n;
        }

        /// @brief Build the flattened (nucleusIid, eager) extends array for @p T at constant evaluation.
        template<class T>
        consteval auto MakeExtendsArrayFor() {
            constexpr std::size_t N = CountExtends<T>();
            constexpr bool eagerT = Anno::IsEager<T>;
            std::array<ExtendsInfo, N> out{};
            std::size_t i = 0;
            for (auto a : std::meta::annotations_of(^^T, ^^Anno::Extends)) {
                auto ex = std::meta::extract<Anno::Extends>(a);
                for (auto p = ex.bases.begin(); p != ex.bases.end(); ++p) {
                    out[i].nucleusIid = IidOfMeta(*p);
                    out[i].eager      = eagerT;
                    ++i;
                }
            }
            return out;
        }

        /// @brief Rodata-resident implements array for @p T (one definition per instantiation).
        template<class T>
        inline constexpr auto kImplementsArr = MakeImplementsArrayFor<T>();

        /// @brief Rodata-resident extends array for @p T (one definition per instantiation).
        template<class T>
        inline constexpr auto kExtendsArr = MakeExtendsArrayFor<T>();

        /**
         * @brief Verify D3's "@c kMetaCore must be public" invariant for @p T.
         *
         * Returns @c true if @p T either does not declare a static member named
         * @c kMetaCore, or declares it in a @c public access section. Wrapped in a
         * @c static_assert by Tasks 8 and 9 (the layers that look @c kMetaCore up by
         * reflection) so that the diagnostic fires at the offending class, not deep
         * inside the link layer.
         *
         * @note Uses @c std::meta::access_context::unchecked() to enumerate members
         *       regardless of the *caller's* access. Using @c ::current() here would
         *       silently hide a private @c kMetaCore from the very predicate meant
         *       to catch it (the caller is namespace scope / sibling code), turning
         *       the invariant into a no-op. @c is_public(m) still reports the actual
         *       declared access of the member, so the predicate fails correctly
         *       when @c kMetaCore was placed in a non-public section.
         */
        template<class T>
        consteval bool YObjectIsPublic() {
            for (auto m : std::meta::members_of(^^T, std::meta::access_context::unchecked())) {
                if (std::meta::is_static_member(m)
                    && std::meta::identifier_of(m) == std::string_view{"kMetaCore"}) {
                    return std::meta::is_public(m);
                }
            }
            return true;
        }

        /// @brief Synthesise the full @ref MetaCore prvalue for @p T from reflection.
        template<class T>
        consteval MetaCore MakeMetaCoreFor() {
            return MetaCore{
                .iid             = IidOf<T>(),
                .name            = std::meta::display_string_of(^^T),
                .role            = ClassTypeOf<T>,
                .implements      = kImplementsArr<T>.data(),
                .implementsCount = kImplementsArr<T>.size(),
                .extends         = kExtendsArr<T>.data(),
                .extendsCount    = kExtendsArr<T>.size(),
            };
        }

        /**
         * @brief Y_OBJECT's NSDMI hook — preserves D3.1's virtual-destructor invariant.
         *
         * Empty struct with a @c consteval ctor whose body asserts that @p T has a virtual
         * destructor. Y_OBJECT plants this as a @c [[no_unique_address]] NSDMI subobject;
         * because NSDMIs are parsed in complete-class context, @p T is complete by the time
         * the @c consteval ctor body runs at constant evaluation. Kept intentionally
         * identical to the Task-5 spelling — Y_OBJECT depends on this shape.
         */
        template<class T>
        struct MetaHook {
            consteval MetaHook() {
                static_assert(std::has_virtual_destructor_v<T>,
                              "Y_OBJECT requires a virtual destructor on the enclosing class");
                static_assert(YObjectIsPublic<T>(),
                              "Y_OBJECT's kMetaCore must be declared in a public section "
                              "(D3 invariant: kMetaCore is the canonical identity hook).");
            }
        };

    } // namespace Detail
    /** @endcond */

} // namespace Yuki

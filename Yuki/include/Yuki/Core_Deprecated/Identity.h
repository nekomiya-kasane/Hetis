/**
 * @file Identity.h
 * @brief Class-role taxonomy for the Yuki object model.
 *
 * A Yuki class declares its role in the object model with a single C++26 annotation,
 * @ref Anno::Meta, e.g.
 *
 * @code
 * [[=Anno::Meta{.type = ClassType::Interface}]]      struct IShape { ... };
 * [[=Anno::Meta{.type = ClassType::Implementation}]] struct CircleImpl : IShape { ... };
 * @endcode
 *
 * Every facility here is `consteval` / `inline constexpr`, so role queries fold to immediate
 * values with **zero** runtime cost. The single source of truth is the @ref ClassType enum;
 * @ref ClassTypeOf reads it back off any type *totally* — it yields @ref ClassType::None for
 * unannotated or non-class types instead of being ill-formed, and the named role concepts
 * (@ref InterfaceClass, @ref ImplementationClass, …) partition types over that result.
 *
 * Stateful vs. stateless extensions are *not* separate roles: the only difference is a reflection
 * question over an Extension's own NSDMs, decided at compile time from the type itself (see
 * @ref Anno::StatelessExtensionClass).
 *
 * This header owns **roles only**. Identity-by-IID, the three-layer @c MetaClass, and the
 * reflection pipeline that bakes a class's metadata into `.rodata` live in `MetaClass.h`; the
 * polymorphic anchor and CRTP injection layer live in `RootObject.h`.
 *
 * @ingroup Core
 */
#pragma once

#include <cstdint>
#include <initializer_list>
#include <meta>
#include <optional>
#include <vector>
#include <span>

#include <Mashiro/Core/TypeTraits.h>
#include <Mashiro/Core/Hash.h>

namespace Yuki {

    // Targeted re-exports — never `using namespace Mashiro` (it collides with Yuki's own `Anno`,
    // `NameOf`, etc.). Pull in only what this header needs, by name.
    namespace Traits = Mashiro::Traits;

    // =========================================================================
    // ClassType — the single source of truth for object-model roles
    // =========================================================================

    /**
     * @brief The role a class plays in the Yuki object model.
     *
     * Doubles as the runtime tag stored in the low three bits of @c RootObject's tagged payload
     * pointer — every value is in `[0, 7]`, so encoding is a single `or`/`and`-mask, never a
     * vcall. Six roles plus the @ref None sentinel fit in three bits with one slot to spare.
     *
     * "Stateful vs. stateless" extension is *not* a role distinction — both are
     * @ref ClassType::Extension. Whether an extension carries per-instance data (and therefore
     * needs a refcount, side-table storage, etc.) is decided by reflection over the NSDMs declared
     * on the Extension class itself — inherited members from the anchor/CRTP base are skipped, so
     * @c sizeof is not consulted; see @ref Anno::StatelessExtensionClass.
     */
    enum class ClassType : uint8_t {
        None           = 0,  ///< Not part of the object model (no @ref Anno::Meta annotation).
        Interface      = 1,  ///< A behaviour contract.
        Implementation = 2,  ///< A main component implementation.
        Extension      = 3,  ///< Extension; stateless vs. stateful is a reflection-based distinction
                             ///< on the Extension's own NSDMs (see @ref Anno::StatelessExtensionClass),
                             ///< not a role.
        Imposter       = 4,  ///< A stand-in / proxy class.
        Bridge         = 5,  ///< A generated TIE/BOA adapter class.
    };
    static_assert(Traits::SequentialEnum<ClassType>);
    static_assert(sizeof(ClassType) == 1);
    static_assert(static_cast<uint8_t>(ClassType::Bridge) <= 0b111,
                  "ClassType must fit in three bits — RootObject payload encoding depends on it.");

    /// @brief Number of distinct @ref ClassType values (including @ref ClassType::None).
    inline constexpr size_t kClassTypeCount = Traits::EnumeratorsCount<ClassType>;

    /// @brief Bit mask covering the three @ref ClassType bits in a tagged @c RootObject payload.
    inline constexpr uintptr_t kClassTypeMask = 0b111u;

    // =========================================================================
    // Iid — 128-bit stable type identifier (RFC-4122 v8 / FNV-1a 128 over type name)
    // =========================================================================

    /**
     * @brief 128-bit stable type identifier.
     *
     * Wraps @c Mashiro::Uuid so Yuki can stamp its own RFC-4122 version nibble on the
     * digest without leaking the underlying hash algorithm into the public API. Equality,
     * ordering, and hashing all forward to the wrapped Uuid. Constructed by @ref IidOf
     * from a type's reflected display name (FNV-1a 128, v8 stamped), so two distinct
     * types — even with the same unqualified name in different namespaces — produce
     * different @c Iid values.
     */
    struct Iid {
        Mashiro::Uuid value{};

        constexpr Iid() noexcept = default;
        explicit constexpr Iid(Mashiro::Uuid u) noexcept : value(u) {}

        constexpr bool operator==(const Iid&) const noexcept = default;
        constexpr auto operator<=>(const Iid&) const noexcept = default;
    };
    static_assert(sizeof(Iid) == 16);

    /** @cond INTERNAL */
    namespace Detail {

        /// @brief Synthesise an @ref Iid from a static type name (FNV-1a 128, v8 stamped).
        consteval Iid IidFromName(std::string_view name) {
            const auto digest = Mashiro::Hashing::Detail::HashString(
                Mashiro::Hashing::Fnv1a128{}, name);
            return Iid{Mashiro::Uuid::FromUint128(digest).WithRfc4122(8)};
        }

    } // namespace Detail
    /** @endcond */

    /// @brief The @ref Iid for the type denoted by reflection @p type.
    ///
    /// Normalises @p type with @c std::meta::dealias before stringifying, so a reflection
    /// captured through an alias (e.g. @c ^^std::remove_cvref_t<IZ>) and a reflection
    /// captured directly (e.g. @c ^^IZ inside an annotation) hash to the same @ref Iid.
    consteval Iid IidOfMeta(std::meta::info type) {
        return Detail::IidFromName(std::meta::display_string_of(std::meta::dealias(type)));
    }

    /// @brief The @ref Iid for type @p T, derived from its reflected display name.
    ///
    /// Routed through @ref IidOfMeta so the @c IidOf<T> and the @c IidOfMeta(^^T) paths
    /// always agree byte-for-byte — the bake pipeline in @ref MetaCore consumes the
    /// reflection-driven path, while user code consumes the type-driven one. Strips
    /// cvref so @c IidOf<const T&> matches @c IidOf<T>.
    template<typename T>
    consteval Iid IidOf() {
        return IidOfMeta(^^std::remove_cvref_t<T>);
    }

    // =========================================================================
    // Anno::Meta — the sole object-model annotation carrier
    // =========================================================================

    namespace Anno {

        /**
         * @brief A *structural* view over a static array of type reflections.
         *
         * `std::span` cannot appear in an annotation value — its members are private, so it is not a
         * structural type. @c InfoList exposes a public pointer/length pair (both structural), making
         * @ref Meta usable as a `[[=Anno::Meta{...}]]` annotation while still reading like a span.
         *
         * Two ways to fill it, both yielding a constant-expression value:
         *   - from a pre-declared static array: `inline constexpr std::meta::info a[] = {...}; ... {a}`;
         *   - inline via a braced list: `{^^IA, ^^IB}` — the initializer-list constructor promotes the
         *     elements to static storage with `std::define_static_array`, so the stored pointer never
         *     dangles. This is what makes `[[=Meta{.implements = {^^IA, ^^IB}}]]` work with no
         *     boilerplate array.
         */
        struct InfoList {
            const std::meta::info* first = nullptr;  ///< Pointer into a static `std::meta::info[]`.
            std::size_t count = 0;                   ///< Number of reflections.

            consteval InfoList() = default;
            /// @brief Adopt a static array of reflections (decays to pointer + length).
            template<std::size_t N>
            consteval InfoList(const std::meta::info (&arr)[N]) noexcept : first(arr), count(N) {}
            /// @brief Adopt an inline braced list, promoting it to static storage.
            consteval InfoList(std::initializer_list<std::meta::info> il)
                : first(std::define_static_array(std::vector<std::meta::info>(il.begin(), il.end())).data()),
                  count(il.size()) {}

            [[nodiscard]] consteval const std::meta::info* begin() const noexcept { return first; }
            [[nodiscard]] consteval const std::meta::info* end() const noexcept { return first + count; }
            [[nodiscard]] consteval std::size_t size() const noexcept { return count; }
            [[nodiscard]] consteval bool empty() const noexcept { return count == 0; }
        };

        /**
         * @brief The full object-model annotation: role plus inheritance edges.
         *
         * @c type is mandatory and names the role. @c extends / @c implements carry the
         * object-model inheritance edges as @ref InfoList views over static `std::meta::info[]`
         * arrays (reflections of the extended / implemented types); the @c MetaClass pipeline
         * splices them back into type reflections at compile time. Both default to empty.
         *
         * Because @ref InfoList holds `std::meta::info` pointers, @c Meta is a *consteval-only*
         * type — it can appear as a prvalue inside an annotation (`[[=Anno::Meta{...}]]`) but cannot
         * be a namespace-scope constant. The edge-less @ref Role shorthands exist for that reason.
         */
        struct Meta {
            ClassType type;       ///< The object-model role (mandatory).
            InfoList extends;     ///< OM types this class extends.
            InfoList implements;  ///< Interfaces this class implements.
        };

        /**
         * @brief The edge-less object-model annotation: role only.
         *
         * Holds nothing but a @ref ClassType (not consteval-only), so it can back the ready-made
         * @ref Interface / @ref Implementation / … constants below. A class that needs `extends` /
         * `implements` edges uses the full @ref Meta instead; both feed the same single source of
         * truth (@ref ClassTypeOf reads either).
         */
        struct Role {
            ClassType type;  ///< The object-model role.
        };

        /**
         * @brief Stackable annotation declaring the interfaces a class implements.
         *
         * Pairs with a role shorthand for a composable, boilerplate-free spelling:
         * `[[=Implementation]] [[=Implements{^^IShape, ^^IDrawable}]] struct CircleImpl { ... };`.
         * The braced list is promoted to static storage by @ref InfoList, so the value is a valid
         * constant expression. Equivalent to the `.implements` field of the full @ref Meta; the
         * @c MetaClass pipeline merges whichever form is present.
         */
        struct Implements {
            InfoList ifaces;  ///< Interfaces this class implements.

            consteval Implements(std::initializer_list<std::meta::info> il) : ifaces(il) {}
            template<std::size_t N>
            consteval Implements(const std::meta::info (&arr)[N]) noexcept : ifaces(arr) {}
        };

        /**
         * @brief Stackable annotation declaring the object-model types a class extends.
         *
         * The `extends` counterpart of @ref Implements:
         * `[[=Extension]] [[=Extends{^^CircleImpl}]] struct ShinyExt { ... };`.
         */
        struct Extends {
            InfoList bases;  ///< OM types this class extends.

            consteval Extends(std::initializer_list<std::meta::info> il) : bases(il) {}
            template<std::size_t N>
            consteval Extends(const std::meta::info (&arr)[N]) noexcept : bases(arr) {}
        };

        /**
         * @brief Inheritance seal: no subclass may re-implement @p iface.
         *
         * Stamped on an @ref Anno::Implementation (or @ref Anno::Extension): a derived class that
         * re-declares the same interface is a hard compile error at MetaCore-bake time. Orthogonal
         * to @ref Unique / @ref Important — those are closure-level seals (one nucleus + its
         * Extensions), whereas @c Final restricts inheritance independently of closure membership.
         */
        struct Final {
            std::meta::info iface;  ///< Interface reflection sealed against re-implementation.
        };

        /**
         * @brief Closure seal: at most one class in this nucleus's closure implements @p iface.
         *
         * The closure registrar enforces uniqueness across the @c Anno::Implementation + its
         * Extensions, diagnosing conflicting providers at bake time.
         */
        struct Unique {
            std::meta::info iface;  ///< Interface reflection sealed against multi-provider closure.
        };

        /**
         * @brief Closure seal: this provider always wins dispatch for @p iface.
         *
         * Resolves ambiguity in favour of the @c Important provider when multiple closure members
         * implement the same interface. Pairs naturally with an Extension that overrides an
         * Implementation's interface.
         */
        struct Important {
            std::meta::info iface;  ///< Interface reflection elevated for closure dispatch.
        };

        /**
         * @brief Field-level marker: this NSDM participates in closure serialization (P2-D4).
         *
         * Empty struct so the annotation reads as a prvalue: `[[=Anno::Pickled{}]]`. Discoverable
         * via reflection today; full @c PicklePump semantics land in Plan B's closure serializer.
         */
        struct Pickled {};

        /// @name Edge-less role shorthands
        /// @brief Ready-made @ref Role values for the common case of a class with no `extends` /
        ///        `implements` edges, so the annotation reads `[[=Anno::Implementation]]` instead of
        ///        the verbose `[[=Anno::Meta{.type = ClassType::Implementation}]]`. When a class does
        ///        carry edges, spell out the full @ref Meta with the relevant fields.
        /// @{
        inline constexpr Role Interface{ClassType::Interface};
        inline constexpr Role Implementation{ClassType::Implementation};
        inline constexpr Role Extension{ClassType::Extension};
        inline constexpr Role Imposter{ClassType::Imposter};
        inline constexpr Role Bridge{ClassType::Bridge};
        /// @}

        // ---------------------------------------------------------------------
        // Eager / Lazy — materialization-timing markers for Extensions
        // ---------------------------------------------------------------------

        /**
         * @brief Marker annotation: materialize this Extension at *closure construction time*.
         *
         * Stamped as `[[=Anno::Eager{}]]` on an `Anno::Extension` class to override the per-statefulness
         * default (the trailing `{}` is required — @c Eager is an empty struct, so the annotation is a
         * prvalue of it). Detected purely by reflection (see @ref IsEager); carries no state of its own. The
         * registrar generated for the Extension consults @ref IsEager / @ref IsLazy to decide whether
         * to enrol the type in the owning Implementation's eager-set or to wire it onto the lazy
         * `SideTableResolver` path.
         *
         * @note Stateless Extensions default to Eager; stateful Extensions default to Lazy. The
         *       annotation is only required when an author wants the *non-default* behaviour, but
         *       spelling it out explicitly is also accepted and reads as documentation at the site
         *       of declaration.
         */
        struct Eager {};

        /**
         * @brief Marker annotation: materialize this Extension at *first query* (lazy resolution).
         *
         * Spelled as `[[=Anno::Lazy{}]]` on an `Anno::Extension` class — the trailing `{}` is required
         * for the same reason as @ref Eager (empty struct ⇒ prvalue spelling). The peer of @ref Eager;
         * see its docs. A class carrying both @ref Eager and @ref Lazy is a user error; downstream
         * introspection treats the situation as Eager wins, but the registrar is the authoritative
         * resolver and is free to diagnose the conflict.
         */
        // TODO(task-8): registrar diagnoses Eager+Lazy conflict.
        struct Lazy {};

        /// @brief `true` if @p T carries an @ref Eager annotation (stackable; checked by type only).
        template<class T>
        inline constexpr bool IsEager = !std::meta::annotations_of(^^T, ^^Eager).empty();

        /// @brief `true` if @p T carries a @ref Lazy annotation (stackable; checked by type only).
        template<class T>
        inline constexpr bool IsLazy = !std::meta::annotations_of(^^T, ^^Lazy).empty();

    } // namespace Anno

    // =========================================================================
    // Reflection core
    // =========================================================================

    /** @cond INTERNAL */
    namespace Detail {

        /**
         * @brief Total read of the @ref ClassType stamped on a type reflection.
         *
         * Accepts either annotation flavour — the edge-less @ref Anno::Role shorthand or the full
         * @ref Anno::Meta — and projects both onto the single @ref ClassType source of truth.
         *
         * @return The annotated role, or @ref ClassType::None when @p type carries neither
         *         annotation. Throws (⇒ hard compile error) when a type is stamped with multiple
         *         object-model annotations that disagree on the role.
         */
        consteval ClassType ReadClassType(std::meta::info type) {
            auto roleOf = [](std::meta::info a) -> ClassType {
                if (std::meta::type_of(a) == ^^Anno::Meta) {
                    return std::meta::extract<Anno::Meta>(a).type;
                }
                return std::meta::extract<Anno::Role>(a).type;
            };
            auto metas = std::meta::annotations_of(type, ^^Anno::Meta);
            auto roles = std::meta::annotations_of(type, ^^Anno::Role);
            if (metas.empty() && roles.empty()) {
                return ClassType::None;
            }
            std::optional<ClassType> result;
            for (auto a : metas) {
                ClassType k = roleOf(a);
                if (result && *result != k) {
                    throw "Yuki::ClassTypeOf: a type carries conflicting object-model roles";
                }
                result = k;
            }
            for (auto a : roles) {
                ClassType k = roleOf(a);
                if (result && *result != k) {
                    throw "Yuki::ClassTypeOf: a type carries conflicting object-model roles";
                }
                result = k;
            }
            return *result;
        }

        consteval bool IsObjectModelType(std::meta::info type) {
            return !std::meta::annotations_of(type, ^^Anno::Meta).empty()
                || !std::meta::annotations_of(type, ^^Anno::Role).empty();
        }

        /**
         * @brief Triple-orthogonal seal state for the (class, interface) pair.
         *
         * Returned by @ref SealFlagsFor; consumed by Task 7's MetaCore bake.
         */
        struct SealFlags {
            bool final = false;      ///< Carries an @ref Anno::Final{^^I} annotation.
            bool unique = false;     ///< Carries an @ref Anno::Unique{^^I} annotation.
            bool important = false;  ///< Carries an @ref Anno::Important{^^I} annotation.
        };

        /**
         * @brief Read the seal flags stamped on class @p T for interface @p I.
         *
         * Walks @c std::meta::annotations_of(^^T), recognising @ref Anno::Final / @ref Anno::Unique
         * / @ref Anno::Important whose @c iface reflection equals @c ^^I. Total — silently returns
         * the zero-initialised result for an untagged pair.
         *
         * @note The parentheses around @c (^^Anno::Final) etc. are load-bearing under clang-p2996:
         *       the splice operator @c ^^ greedily glues onto the next token, so @c ^^Anno::Final
         *       && @c ... parses as @c ^^(Anno::Final && ...). Do not "simplify" them away.
         */
        template<class T, class I>
        consteval SealFlags SealFlagsFor() {
            SealFlags f{};
            for (auto a : std::meta::annotations_of(^^T)) {
                auto t = std::meta::type_of(a);
                if (t == (^^Anno::Final)
                    && std::meta::extract<Anno::Final>(a).iface == (^^I)) {
                    f.final = true;
                } else if (t == (^^Anno::Unique)
                    && std::meta::extract<Anno::Unique>(a).iface == (^^I)) {
                    f.unique = true;
                } else if (t == (^^Anno::Important)
                    && std::meta::extract<Anno::Important>(a).iface == (^^I)) {
                    f.important = true;
                }
            }
            return f;
        }

    } // namespace Detail
    /** @endcond */

    // =========================================================================
    // Public role queries — all total, cvref-insensitive
    // =========================================================================

    /// @brief The object-model role of @p T, or @ref ClassType::None if untagged.
    template<typename T>
    inline constexpr ClassType ClassTypeOf = Detail::ReadClassType(^^std::remove_cvref_t<T>);

    /// @brief `true` if @p T carries an @ref Anno::Meta annotation.
    template<typename T>
    concept ClassTyped = ClassTypeOf<T> != ClassType::None;

    /// @brief `true` if @p T's role is exactly @p K.
    template<typename T, ClassType K>
    concept IsClassType = (ClassTypeOf<T> == K);

    /// @brief An interface contract — a plain interface or a generated bridge adapter.
    template<typename T>
    concept InterfaceClass = IsClassType<T, ClassType::Interface> || IsClassType<T, ClassType::Bridge>;

    /// @brief A generated TIE/BOA adapter class.
    template<typename T>
    concept BridgeClass = IsClassType<T, ClassType::Bridge>;

    /// @brief A stand-in / proxy class. Equivalent to CATObject.
    template<typename T>
    concept ImposterClass = IsClassType<T, ClassType::Imposter>;

    /// @brief A main component implementation.
    template<typename T>
    concept ImplementationClass = IsClassType<T, ClassType::Implementation>;

    /// @brief Either flavour of extension.
    template<typename T>
    concept ExtensionClass = IsClassType<T, ClassType::Extension>;

    namespace Anno {

        /**
         * @brief A *stateless* extension — an extension with no per-instance data; singleton-shareable.
         *
         * The compile-time discriminator that decides storage and refcounting policy: a stateless
         * extension is a `.rodata`-resident singleton whose dispatch entry can be a
         * @c CodeExtensionSingleton; a stateful extension needs side-table storage routed through a
         * @c SideTableResolver. The check uses reflection rather than @c sizeof: every Extension
         * derives transitively from @c RootObject and so unconditionally carries ≥ 2 machine words,
         * making @c sizeof a false discriminator. Instead, count the NSDMs *declared on @p E itself*
         * — inherited members from the anchor/CRTP base are skipped because their @c parent_of
         * reflection differs from @c ^^E.
         */
        template<class E>
        concept StatelessExtensionClass = ExtensionClass<E> && [] consteval {
            std::size_t own = 0;
            template for (constexpr auto m : std::define_static_array(
                              std::meta::nonstatic_data_members_of(^^E, std::meta::access_context::current()))) {
                if (std::meta::parent_of(m) == ^^E) {
                    ++own;
                }
            }
            return own == 0;
        }();

        /// @brief A *stateful* extension — an extension whose layout carries per-instance data.
        ///        Negation of @ref StatelessExtensionClass restricted to @ref ExtensionClass.
        template<class E>
        concept StatefulExtensionClass = ExtensionClass<E> && !StatelessExtensionClass<E>;

    } // namespace Anno

    /// @brief Re-export so unqualified `Yuki::StatelessExtensionClass` keeps working (the historical
    ///        spelling pre-dates the move into `Anno`).
    using Anno::StatelessExtensionClass;
    using Anno::StatefulExtensionClass;

    /// @brief Either flavour of component.
    template<typename T>
    concept ComponentClass = ImplementationClass<T> || ExtensionClass<T>;

    // =========================================================================
    // FacadeKind — per-interface storage policy for the QueryInterface system
    // =========================================================================

    /**
     * @brief How an implementation should hold a facade for a given interface.
     *
     * The compile-time analogue of CATIA's "always-resident TIE vs. installed-on-demand TIE".
     * @ref Hot interfaces are inline-aggregated as subobjects of the implementation, so that
     * @ref Query yields a native interface pointer with a single `static_cast`-equivalent address
     * adjustment and no runtime indirection. @ref Cold interfaces are queried infrequently and
     * synthesised on demand through a runtime-attached @c FacadeList — saving per-instance bytes at
     * the cost of a one-time hash lookup. Default for every interface is @ref Hot; switch to
     * @ref Cold via @ref Anno::InterfaceTraits.
     */
    enum class FacadeKind : uint8_t {
        Hot,   ///< Inline-aggregated facade subobject; zero-overhead Query.
        Cold,  ///< Runtime-attached via the @c FacadeList; cold-path Query.
    };
    static_assert(Traits::SequentialEnum<FacadeKind>);

    namespace Anno {

        /**
         * @brief Per-interface storage-policy override, stamped onto an @ref Anno::Interface class.
         *
         * @code
         * struct [[=Anno::Interface]] [[=Anno::InterfaceTraits{.facade_kind = FacadeKind::Cold}]]
         *        IRareInspect { virtual int Inspect() const = 0; };
         * @endcode
         *
         * One field today (@c facade_kind); structurally extensible without breaking the annotation
         * grammar. Reading is total: @ref FacadeKindOf returns @ref FacadeKind::Hot when no
         * @c InterfaceTraits is present, so authors only spell it out for the rare Cold case.
         */
        struct InterfaceTraits {
            FacadeKind facade_kind = FacadeKind::Hot;  ///< Storage policy. Default: @c Hot.
        };

    } // namespace Anno

    /// @brief The storage policy declared for interface @p I — @ref FacadeKind::Hot by default,
    ///        overridden by @ref Anno::InterfaceTraits when present.
    template<typename I>
    inline constexpr FacadeKind FacadeKindOf = [] consteval {
        const auto annos = std::meta::annotations_of(^^I, ^^Anno::InterfaceTraits);
        if (annos.empty()) {
            return FacadeKind::Hot;
        }
        return std::meta::extract<Anno::InterfaceTraits>(annos[0]).facade_kind;
    }();

    /// @brief @p I is an interface stored as an inline-aggregated facade. Hot is the default.
    template<typename I>
    concept HotInterface = InterfaceClass<I> && (FacadeKindOf<I> == FacadeKind::Hot);

    /// @brief @p I is an interface synthesised on demand via the runtime @c FacadeList.
    template<typename I>
    concept ColdInterface = InterfaceClass<I> && (FacadeKindOf<I> == FacadeKind::Cold);

} // namespace Yuki

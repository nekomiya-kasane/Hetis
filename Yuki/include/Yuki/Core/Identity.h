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
 * Stateful vs. stateless extensions are *not* separate roles: the only difference is a sizeof
 * question, decided at compile time from the type itself (see @ref StatefulExtensionClass).
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
     * needs a refcount, side-table storage, etc.) is decided from `sizeof` at compile time; see
     * @ref StatefulExtensionClass.
     */
    enum class ClassType : uint8_t {
        None           = 0,  ///< Not part of the object model (no @ref Anno::Meta annotation).
        Interface      = 1,  ///< A behaviour contract.
        Implementation = 2,  ///< A main component implementation.
        Extension      = 3,  ///< Extension; statefulness is a `sizeof` distinction, not a role.
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
                : first(std::define_static_array(
                        std::vector<std::meta::info>(il.begin(), il.end())).data()),
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

    /// @brief A *stateful* extension — an extension whose layout carries per-instance data.
    ///
    /// The compile-time discriminator that decides storage and refcounting policy: a stateful
    /// extension needs side-table storage and an opt-in refcount; a stateless one is a
    /// `.rodata`-resident singleton with refcounting that has no meaning. The check is purely
    /// `sizeof`: extensions (which derive from a 1-byte polymorphic-anchor base) carry data
    /// exactly when their footprint exceeds that base.
    template<typename T>
    concept StatefulExtensionClass = ExtensionClass<T> && (sizeof(T) > 1);

    /// @brief A *stateless* extension — an extension with no per-instance data; singleton-shareable.
    template<typename T>
    concept StatelessExtensionClass = ExtensionClass<T> && (sizeof(T) <= 1);

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

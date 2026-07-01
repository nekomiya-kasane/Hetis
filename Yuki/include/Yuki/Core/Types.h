/**
 * @file Types.h
 * @brief Object-model roles, annotations, reflected identity helpers, and class classification.
 * @ingroup Core
 */
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <meta>
#include <optional>
#include <string_view>
#include <type_traits>

#include <Mashiro/Core/TypeTraits.h>

#include "Yuki/Core/IID.h"

namespace Yuki {

    /** @brief Root object base for all objects that participate in Yuki lifetime and QueryInterface. */
    class BaseUnknown;

    /** @brief Immutable runtime metaclass record. */
    class MetaClass;

    /** @brief Raw element constraint used by forward declarations that appear before BaseUnknown is complete. */
    template<class T>
    concept ComClass = 
        std::same_as<std::remove_cvref_t<T>, BaseUnknown> || std::derived_from<std::remove_cvref_t<T>, BaseUnknown>;

    /** @brief Intrusive owning pointer for BaseUnknown-anchored concrete objects and interface facets. */
    template<ComClass T>
    class ComPtr;

    /** @brief Allocate @p T, bind its static metaclass, and return an adopted owning pointer. */
    template<ComClass T, class... Args>
    [[nodiscard]] ComPtr<T> MakeOwned(Args&&... args);

    namespace Detail {

        /** @brief ABI-sized BaseUnknown storage footprint used before BaseUnknown is complete. */
        inline constexpr std::size_t kBaseUnknownObjectBytes = 2 * sizeof(void*);

    } // namespace Detail

    /** @name Object-model roles @{ */

    /** @brief Historical Yuki object-model role of a class declaration. */
    enum class TypeOfClass : uint8_t {
        NothingType = 0x01,        /**< Untagged class; outside the object model. */
        BaseUnknown = 0x02,        /**< Root object anchor. */
        Interface = 0x03,          /**< Interface contract; not a lifetime-owning object. */
        TIE = 0x04,                /**< Bound facet object. */
        TIEchain = 0x05,           /**< Chained bound facet object. */
        Implementation = 0x08,     /**< Primary component implementation. */
        DataExtension = 0x10,      /**< Stateful extension carrying data. */
        CodeExtension = 0x20,      /**< Stateless extension carrying behavior. */
        CacheExtension = 0x40,     /**< Cache-like extension with derived state. */
        TransientExtension = 0x80, /**< Transient provider object. */
    };

    /** @brief Return whether @p type denotes one of the extension roles. */
    [[nodiscard]] constexpr bool IsExtension(TypeOfClass type) noexcept {
        return type == TypeOfClass::DataExtension || type == TypeOfClass::CodeExtension ||
               type == TypeOfClass::CacheExtension || type == TypeOfClass::TransientExtension;
    }

    /** @brief Return whether @p type denotes a TIE role. */
    [[nodiscard]] constexpr bool IsTie(TypeOfClass type) noexcept {
        return type == TypeOfClass::TIE || type == TypeOfClass::TIEchain;
    }

    /** @} */

    /** @brief Strongly typed annotation payloads consumed by P2996/P3385 reflection. */
    namespace Anno {

        /** @brief Declares the direct object-model role of a C++ type. */
        struct Role {
            TypeOfClass type{TypeOfClass::NothingType}; /**< Declared role. */
        };

        /** @brief Declares an extension whose final DataExtension/CodeExtension role is layout-derived. */
        struct ExtensionRole {};

        /** @brief Interface role shorthand. */
        inline constexpr Role Interface{TypeOfClass::Interface};
        /** @brief Implementation role shorthand. */
        inline constexpr Role Implementation{TypeOfClass::Implementation};
        /** @brief Extension role shorthand; final role is computed from sizeof(T) versus BaseUnknown. */
        inline constexpr ExtensionRole Extension{};
        /** @brief Compatibility extension shorthand; final role is computed from object layout. */
        inline constexpr Role DataExtension{TypeOfClass::DataExtension};
        /** @brief Compatibility extension shorthand; final role is computed from object layout. */
        inline constexpr Role CodeExtension{TypeOfClass::CodeExtension};
        /** @brief Cache extension role shorthand. */
        inline constexpr Role CacheExtension{TypeOfClass::CacheExtension};
        /** @brief Transient extension role shorthand. */
        inline constexpr Role TransientExtension{TypeOfClass::TransientExtension};
        /** @brief TIE role shorthand. */
        inline constexpr Role TIE{TypeOfClass::TIE};
        /** @brief TIEchain role shorthand. */
        inline constexpr Role TIEchain{TypeOfClass::TIEchain};

    } // namespace Anno

    namespace Detail {

        /** @brief Read the unique object-model role annotation on @p type, or NothingType when absent. */
        consteval TypeOfClass RoleOfMeta(std::meta::info type) {
            const std::meta::info canonical = std::meta::dealias(type);
            if (canonical == ^^BaseUnknown) {
                return TypeOfClass::BaseUnknown;
            }
            std::optional<TypeOfClass> result;
            auto accept = [&](TypeOfClass current) {
                if (result && *result != current) {
                    throw "Yuki::ClassTypeOf: conflicting object-model role annotations";
                }
                result = current;
            };
            for (auto anno : std::meta::annotations_of(canonical, ^^Anno::Role)) {
                accept(std::meta::extract<Anno::Role>(anno).type);
            }
            for ([[maybe_unused]] auto anno : std::meta::annotations_of(canonical, ^^Anno::ExtensionRole)) {
                accept(TypeOfClass::DataExtension);
            }
            return result.value_or(TypeOfClass::NothingType);
        }

        /** @brief Return the stable IID for the type denoted by reflection @p type. */
        consteval Iid IidOfMeta(std::meta::info type) {
            const std::meta::info canonical = std::meta::dealias(type);
            if (std::optional<Iid> overrideIid = Detail::IidOverrideOfMeta(canonical)) {
                return *overrideIid;
            }
            return Detail::IidFromName(std::meta::display_string_of(canonical));
        }

        /** @brief Return the single direct object-model base type for @p type, or void when none exists. */
        consteval std::meta::info DirectObjectModelBaseTypeOfMeta(std::meta::info type) {
            const TypeOfClass ownRole = RoleOfMeta(type);
            std::optional<std::meta::info> result;
            for (std::meta::info baseSpec : std::meta::bases_of(type, std::meta::access_context::unchecked())) {
                const std::meta::info baseType = std::meta::type_of(baseSpec);
                const TypeOfClass baseRole = RoleOfMeta(baseType);
                if (baseRole == TypeOfClass::NothingType || baseRole == TypeOfClass::BaseUnknown) {
                    continue;
                }
                if (ownRole == TypeOfClass::Interface) {
                    if (baseRole != TypeOfClass::Interface) {
                        continue;
                    }
                } else if (baseRole == TypeOfClass::Interface) {
                    continue;
                }
                if (result) {
                    throw "Yuki::MetaClass: object-model classes must have at most one direct OM base";
                }
                result = baseType;
            }
            return result.value_or(^^void);
        }

        /** @brief Return the single direct object-model base IID for @p type, or nil when none exists. */
        consteval Iid DirectObjectModelBaseIidOfMeta(std::meta::info type) {
            constexpr std::meta::info noBase = ^^void;
            const std::meta::info baseType = DirectObjectModelBaseTypeOfMeta(type);
            return baseType == noBase ? kNilIid : IidOfMeta(baseType);
        }

        /** @brief Return the single direct object-model base IID for @p T, or nil when none exists. */
        template<class T>
        consteval Iid DirectObjectModelBaseIidOf() {
            return DirectObjectModelBaseIidOfMeta(^^std::remove_cvref_t<T>);
        }

    } // namespace Detail

    /** @brief Number of direct C++ base specifiers on @p T, or zero for non-class types. */
    template<class T>
    inline constexpr std::size_t DirectCppBaseCountOf = [] consteval {
        using U = std::remove_cvref_t<T>;
        if constexpr (std::is_class_v<U>) {
            return Mashiro::Traits::BasesCount<U>;
        } else {
            return std::size_t{};
        }
    }();

    /** @brief Return the stable IID for the type denoted by reflection @p type. */
    consteval Iid IidOfMeta(std::meta::info type) {
        return Detail::IidOfMeta(type);
    }

    /** @brief Return the stable IID for C++ type @p T after cvref removal. */
    template<class T>
    consteval Iid IidOf() {
        return Detail::IidOfMeta(^^std::remove_cvref_t<T>);
    }

    /** @brief Return the reflected display name for C++ type @p T after cvref removal. */
    template<class T>
    consteval std::string_view TypeNameOf() noexcept {
        return std::meta::display_string_of(std::meta::dealias(^^std::remove_cvref_t<T>));
    }

    /** @brief Single direct object-model base IID for @p T, or nil when none exists. */
    template<class T>
    inline constexpr Iid DirectObjectModelBaseIid = Detail::DirectObjectModelBaseIidOf<T>();

    namespace Detail {

        /** @brief Compute the final class role after layout-derived extension classification. */
        template<class T>
        consteval TypeOfClass ClassTypeOfImpl() {
            using U = std::remove_cvref_t<T>;
            const TypeOfClass declared = RoleOfMeta(^^U);
            if constexpr (std::derived_from<U, BaseUnknown> && !std::same_as<U, BaseUnknown>) {
                if (declared == TypeOfClass::DataExtension || declared == TypeOfClass::CodeExtension) {
                    return sizeof(U) == kBaseUnknownObjectBytes ? TypeOfClass::CodeExtension
                                                                : TypeOfClass::DataExtension;
                }
            }
            return declared;
        }

    } // namespace Detail

    /** @brief Direct object-model role of @p T; extension declarations are classified by object layout. */
    template<class T>
    inline constexpr TypeOfClass ClassTypeOf = Detail::ClassTypeOfImpl<T>();

} // namespace Yuki

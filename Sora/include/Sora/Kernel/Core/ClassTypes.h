#pragma once

#include "Sora/Core/Traits/AnnotationTraits.h"
#include "Sora/Kernel/Core/Traits.h"

#include <type_traits>

namespace Sora::Kernel {

    /** @name Object-model roles @{ */

    /** @brief Historical Sora object-model role of a class declaration. */
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

    namespace $ {

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

    } // namespace $

    namespace Traits {

        template<typename T>
        inline constexpr TypeOfClass RoleOf = [] consteval {
            if (!std::meta::is_class_type(^^T)) {
                throw std::define_static_string(
                    "Meta::RoleOf: '" + std::string{Sora::Meta::DisplayStringOf(^^T)} +
                    "' is not a class type reflection — only classes participate in Sora object model.");
            }
            const std::meta::info canonical = std::meta::dealias(^^T);
            if (canonical == ^^Sora::Kernel::BaseUnknown) {
                return TypeOfClass::BaseUnknown;
            }

            auto result = Sora::$::GetSingleOptional<$::Role>(canonical);
            return result ? result->type : TypeOfClass::NothingType;
        }();

    } // namespace Traits

    namespace Concept {

        template<typename T>
        concept InterfaceClass = ComClass<T> && Traits::RoleOf<T> == TypeOfClass::Interface;

        template<typename T>
        concept ImplementationClass = ComClass<T> && Traits::RoleOf<T> == TypeOfClass::Implementation;

        template<typename T>
        concept DataExtensionClass = ComClass<T> && Traits::RoleOf<T> == TypeOfClass::DataExtension;

        template<typename T>
        concept CodeExtensionClass = ComClass<T> && Traits::RoleOf<T> == TypeOfClass::CodeExtension;

        template<typename T>
        concept ExtensionClass = ComClass<T> && IsExtension(Traits::RoleOf<T>);

        template<typename T>
        concept ComponentClass = ImplementationClass<T> || ExtensionClass<T> || std::is_same_v<T, BaseUnknown>;

        template<typename T>
        concept TIEClass = ComClass<T> && IsTie(Traits::RoleOf<T>);

    } // namespace Concept

} // namespace Sora::Kernel
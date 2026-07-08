#pragma once

#include <Sora/Core/Traits/AnnotationTraits.h>
#include <Sora/Core/Traits/ScopeTraits.h>
#include <Sora/Core/Traits/TypeTraits.h>
#include <Sora/Core/Traits/InheritanceTraits.h>
#include <Sora/Core/Traits/TypeTraits.h>

#include <Sora/Kernel/Core/Traits.h>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <meta>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Kernel {

    /** @name Object-model roles @{ */

    /** @brief Historical Sora object-model role of a class declaration. */
    enum class TypeOfClass : uint8_t {
        NothingType = 0x01,        /**< Untagged class; outside the object model. */
        Interface = 0x03,          /**< Interface contract; not a lifetime-owning object. */
        TIE = 0x04,                /**< Bound facet object. */
        TIEchain = 0x05,           /**< Chained bound facet object. */
        Implementation = 0x08,     /**< Primary component implementation. */
        DataExtension = 0x10,      /**< Stateful extension carrying data. */
        CodeExtension = 0x20,      /**< Stateless extension carrying behavior. */
        CacheExtension = 0x40,     /**< Cache-like extension with derived state. */
        TransientExtension = 0x80, /**< Transient provider object. */
    };

    [[nodiscard]] constexpr bool IsImplementation(TypeOfClass type) noexcept {
        return type == TypeOfClass::Implementation;
    }

    /** @brief Return whether @p type denotes one of the extension roles. */
    [[nodiscard]] constexpr bool IsExtension(TypeOfClass type) noexcept {
        return type == TypeOfClass::DataExtension || type == TypeOfClass::CodeExtension ||
               type == TypeOfClass::CacheExtension || type == TypeOfClass::TransientExtension;
    }

    [[nodiscard]] constexpr bool IsComponent(TypeOfClass type) noexcept {
        return IsImplementation(type) || IsExtension(type);
    }

    /** @brief Return whether @p type denotes a stateless code extension. */
    [[nodiscard]] constexpr bool IsCodeExtension(TypeOfClass type) noexcept {
        return type == TypeOfClass::CodeExtension;
    }

    [[nodiscard]] constexpr bool IsInterface(TypeOfClass type) noexcept {
        return type == TypeOfClass::Interface;
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

    namespace Meta {

        consteval bool IsComClass(std::meta::info type) {
            type = std::meta::dealias(type);
            if (!std::meta::is_class_type(type)) {
                return false;
            }
            return Sora::Meta::DerivedFrom(type, ^^Sora::Kernel::BaseUnknown) &&
                   Sora::$::GetSingleOptional<$::Role>(type).has_value();
        }

        consteval auto RoleOf(std::meta::info type) {
            if (!IsComClass(type)) {
                throw std::define_static_string(
                    "Meta::RoleOf: '" + std::string{Sora::Meta::DisplayStringOf(type)} +
                    "' is not a class type reflection -- only classes participate in Sora object model.");
            }
            auto result = Sora::$::GetSingleOptional<$::Role>(std::meta::dealias(type));
            return result ? result->type : TypeOfClass::NothingType;
        }

    } // namespace Meta

    namespace Traits {

        template<typename T>
        inline constexpr TypeOfClass RoleOf = Sora::Kernel::Meta::RoleOf(std::meta::dealias(^^T));

    } // namespace Traits

    namespace Concept {

        template<typename T>
        concept InterfaceClass = ComClass<T> && Traits::RoleOf<T> == TypeOfClass::Interface && std::is_abstract_v<T>;

        template<typename T>
        concept ImplementationClass = ComClass<T> && Traits::RoleOf<T> == TypeOfClass::Implementation;

        template<typename T>
        concept DataExtensionClass =
            ComClass<T> && Traits::RoleOf<T> == TypeOfClass::DataExtension && std::is_default_constructible_v<T>;

        template<typename T>
        concept CodeExtensionClass =
            ComClass<T> && Traits::RoleOf<T> == TypeOfClass::CodeExtension && std::is_default_constructible_v<T>;

        template<typename T>
        concept ExtensionClass = ComClass<T> && IsExtension(Traits::RoleOf<T>) && std::is_default_constructible_v<T>;

        template<typename T>
        concept ComponentClass = ImplementationClass<T> || ExtensionClass<T>;

        template<typename T>
        concept QueryTargetClass = InterfaceClass<T> || ExtensionClass<T>;

        template<typename T>
        concept TieClass = ComClass<T> && IsTie(Traits::RoleOf<T>) && std::is_default_constructible_v<T>;

        /** @brief Type that can be used as a string-named virtual implementation class. */
        template<typename T>
        concept VirtualObjectClass = std::is_base_of_v<BaseUnknown, T> && requires { T::kVirtualClassName.view(); };

    } // namespace Concept

    namespace $ {

        /** @brief Declares the interface facets directly provided by an object-model class. */
        template<typename... Interfaces>
        struct Implements {};

        /** @brief Declares the object-model classes extended by an extension class. */
        template<typename... Extendees>
        struct Extends {};

    } // namespace $

    namespace Meta {

        /** @brief Return static-storage direct interface type reflections declared by @ref $::Implements on @p T. */
        template<Concept::ComClass T>
        consteval auto ImplementedInterfaceTypesOf() {
            using namespace std::meta;
            std::vector<info> implements;
            for (const auto& annotation : annotations_of(dealias(^^T))) {
                auto t = type_of(annotation);
                if (!Sora::Meta::IsSpecializationOf<Sora::Kernel::$::Implements>(t)) {
                    continue;
                }

                implements.append_range(template_arguments_of(t) | std::views::transform(dealias));
            }
            return std::define_static_array(implements);
        }

        /** @brief Return static-storage direct extendee type reflections declared by @ref $::Extends on @p T. */
        template<Concept::ComClass T>
        consteval auto ExtendeeTypesOf() {
            using namespace std::meta;
            std::vector<info> extendees;
            for (const auto& annotation : annotations_of(dealias(^^T))) {
                auto t = type_of(annotation);
                if (!Sora::Meta::IsSpecializationOf<Sora::Kernel::$::Extends>(t)) {
                    continue;
                }
                extendees.append_range(template_arguments_of(t) | std::views::transform(dealias));
            }
            return std::define_static_array(extendees);
        }

    } // namespace Meta

    namespace Detail {

        /** @brief Resolve the interface method overridden by the current TIE method. */
        template<std::meta::info CurrentFunction>
        consteval std::meta::info InterfaceMemberOverriddenBy() {
            if (!Sora::Meta::IsRegularMethod(CurrentFunction)) {
                throw std::define_static_string("Core::InterfaceMemberOverriddenBy: current scope is not a regular "
                                                "TIE method.");
            }

            constexpr auto owner = std::meta::parent_of(CurrentFunction);
            if (!std::meta::is_class_type(owner)) {
                throw std::define_static_string("Core::InterfaceMemberOverriddenBy: current method has no class "
                                                "owner.");
            }

            constexpr auto chain = std::define_static_array(Sora::Meta::InheritanceChainOf(owner));
            template for (constexpr auto type : chain) {
                if constexpr (type == owner) {
                    continue;
                }

                template for (constexpr auto member : Sora::Traits::Members<typename [:type:]>) {
                    if constexpr (Sora::Meta::IsSameSignatureMethod(member, CurrentFunction)) {
                        return member;
                    }
                }
            }

            throw std::define_static_string("Core::InterfaceMemberOverriddenBy: no interface method matches the "
                                            "current TIE method in its base-class chain.");
        }

        /** @brief Resolve the implementation method matching the current TIE method in @p Impl's inheritance chain. */
        template<std::meta::info CurrentFunction, Concept::ComponentClass Impl>
        consteval std::meta::info TieTargetOfCurrent() {
            constexpr auto interfaceMember = InterfaceMemberOverriddenBy<CurrentFunction>();
            constexpr auto chain = std::define_static_array(Sora::Meta::InheritanceChainOf(std::meta::dealias(^^Impl)));
            template for (constexpr auto type : chain) {
                template for (constexpr auto member : Sora::Traits::Members<typename [:type:]>) {
                    if constexpr (Sora::Meta::IsSameSignatureMethod(member, interfaceMember)) {
                        return member;
                    }
                }
            }
            throw std::define_static_string("Core::TieTargetOfCurrent: no implementation method matches the "
                                            "interface member in the component inheritance chain.");
        }

        /** @brief Invoke the implementation method corresponding to @p CurrentFunction on a mutable bound target. */
        template<std::meta::info CurrentFunction, Concept::ComponentClass Impl, typename... Args>
        decltype(auto) InvokeTieCurrent(BaseUnknown* target, Args&&... args) {
            constexpr auto targetMember = TieTargetOfCurrent<CurrentFunction, Impl>();
            using Owner = Sora::Meta::InfoType<std::meta::parent_of(targetMember)>;
            constexpr auto method = &[:targetMember:];
            return (static_cast<Owner*>(target)->*method)(std::forward<Args>(args)...);
        }

        /** @brief Invoke the implementation method corresponding to @p CurrentFunction on a const bound target. */
        template<std::meta::info CurrentFunction, Concept::ComponentClass Impl, typename... Args>
        decltype(auto) InvokeTieCurrent(const BaseUnknown* target, Args&&... args) {
            constexpr auto targetMember = TieTargetOfCurrent<CurrentFunction, Impl>();
            using Owner = Sora::Meta::InfoType<Sora::Meta::ParentScopeOf(targetMember)>;
            constexpr auto method = &[:targetMember:];
            return (static_cast<const Owner*>(target)->*method)(std::forward<Args>(args)...);
        }

    } // namespace Detail

} // namespace Sora::Kernel

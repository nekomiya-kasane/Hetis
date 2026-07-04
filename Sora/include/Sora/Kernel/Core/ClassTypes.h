#pragma once

#include "Sora/Core/Traits/AnnotationTraits.h"
#include "Sora/Core/Traits/TypeTraits.h"
#include "Sora/Core/Traits/InheritanceTraits.h"
#include "Sora/Core/Traits/TypeTraits.h"
#include "Sora/Kernel/Core/Traits.h"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <meta>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

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

    namespace Traits {

        template<typename T>
        inline constexpr TypeOfClass RoleOf = [] consteval {
            if (!std::meta::is_class_type(^^T)) {
                throw std::define_static_string(
                    "Meta::RoleOf: '" + std::string{Sora::Meta::DisplayStringOf(^^T)} +
                    "' is not a class type reflection -- only classes participate in Sora object model.");
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
            std::vector<std::meta::info> implements;
            auto allAnnotations = std::meta::annotations_of(std::meta::dealias(^^T));
            for (const auto& annotation : allAnnotations) {
                auto t = std::meta::type_of(annotation);
                if (Sora::Meta::IsSpecializationOf<Sora::Kernel::$::Implements>(t)) {
                    auto params = std::meta::template_arguments_of(t);
                    for (const auto& param : params) {
                        implements.push_back(std::meta::dealias(param));
                    }
                }
            }
            return std::define_static_array(implements);
        }

        /** @brief Return static-storage direct extendee type reflections declared by @ref $::Extends on @p T. */
        template<Concept::ComClass T>
        consteval auto ExtendeeTypesOf() {
            std::vector<std::meta::info> extendees;
            auto allAnnotations = std::meta::annotations_of(std::meta::dealias(^^T));
            for (const auto& annotation : allAnnotations) {
                auto t = std::meta::type_of(annotation);
                if (Sora::Meta::IsSpecializationOf<Sora::Kernel::$::Extends>(t)) {
                    auto params = std::meta::template_arguments_of(t);
                    for (const auto& param : params) {
                        extendees.push_back(std::meta::dealias(param));
                    }
                }
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
                if constexpr (type != owner) {
                    constexpr auto members =
                        std::define_static_array(std::meta::members_of(type, std::meta::access_context::unchecked()));
                    template for (constexpr auto member : members) {
                        if constexpr (Sora::Meta::IsSameSignatureMethod(member, CurrentFunction)) {
                            return member;
                        }
                    }
                }
            }

            throw std::define_static_string("Core::InterfaceMemberOverriddenBy: no interface method matches the "
                                            "current TIE method in its base-class chain.");
        }

        /** @brief Resolve the implementation method matching @p InterfaceMember in @p Impl's inheritance chain. */
        template<std::meta::info InterfaceMember, Concept::ComponentClass Impl>
        consteval std::meta::info TieTargetOfInterfaceMember() {
            constexpr auto chain = std::define_static_array(Sora::Meta::InheritanceChainOf(std::meta::dealias(^^Impl)));
            template for (constexpr auto type : chain) {
                constexpr auto members =
                    std::define_static_array(std::meta::members_of(type, std::meta::access_context::unchecked()));
                template for (constexpr auto member : members) {
                    if constexpr (Sora::Meta::IsSameSignatureMethod(member, InterfaceMember)) {
                        return member;
                    }
                }
            }
            throw std::define_static_string("Core::TieTargetOfInterfaceMember: no implementation method matches the "
                                            "interface member in the component inheritance chain.");
        }

        /** @brief Resolve the implementation method matching the current TIE method in @p Impl's inheritance chain. */
        template<std::meta::info CurrentFunction, Concept::ComponentClass Impl>
        consteval std::meta::info TieTargetOfCurrent() {
            constexpr auto interfaceMember = InterfaceMemberOverriddenBy<CurrentFunction>();
            return TieTargetOfInterfaceMember<interfaceMember, Impl>();
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
            using Owner = Sora::Meta::InfoType<std::meta::parent_of(targetMember)>;
            constexpr auto method = &[:targetMember:];
            return (static_cast<const Owner*>(target)->*method)(std::forward<Args>(args)...);
        }

    } // namespace Detail

    namespace Tie {} // namespace Tie

    namespace Traits {

        /**
         * @brief Return the corresponding TIE class for a given interface type.
         * @tparam Iface Interface type.
         */
        template<Concept::InterfaceClass Iface>
        inline constexpr std::string_view TieClassIdentifierOf = [] consteval {
            return std::define_static_string("Tie$" + std::string{std::meta::identifier_of(^^Iface)});
        }();

    } // namespace Traits

    namespace Meta {

        /**
         * @brief Get the TIE template class type for a given interface type.
         * @tparam Iface Interface type.
         */
        template<Concept::InterfaceClass Iface>
        consteval std::meta::info TieTemplateOf(std::meta::info ns = ^^Sora::Kernel::Tie) {
            if (!Concept::InterfaceClass<Iface>) {
                throw std::define_static_string(
                    "Meta::TieTemplateOf: '" + std::string{Sora::Meta::DisplayStringOf(^^Iface)} +
                    "' is not an interface class type reflection -- only interfaces participate in TIE " +
                    "synthesis.");
            }

            std::string msg;
            for (auto m : std::meta::members_of(ns, std::meta::access_context::current())) {
                if (std::meta::is_class_template(m) &&
                    std::meta::identifier_of(m) == Traits::TieClassIdentifierOf<Iface>) {
                    return m;
                }
            }

            throw std::define_static_string("Meta::TieTemplateOf: no TIE template class found for interface '" +
                                            std::string{Sora::Meta::DisplayStringOf(^^Iface)} + "' in namespace '" +
                                            std::string{Sora::Meta::DisplayStringOf(ns)} + "'\n" + msg);
        }

    } // namespace Meta

    namespace Traits {

        /**
         * @brief Return the corresponding TIE class for a given interface type.
         * @tparam Iface Interface type.
         */
        template<Concept::InterfaceClass Iface, Concept::ComponentClass Impl, std::meta::info NS = ^^Sora::Kernel::Tie>
        using TieClassOf = typename [:[] consteval {
            auto tieTemplate = Meta::TieTemplateOf<Iface>(NS);
            return std::meta::substitute(tieTemplate, {^^Impl});
        }():];

    } // namespace Traits

} // namespace Sora::Kernel

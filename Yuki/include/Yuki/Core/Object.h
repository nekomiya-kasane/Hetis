/**
 * @file Object.h
 * @brief Static object metadata construction and BaseUnknown facet resolver generation.
 * @ingroup Core
 */
#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <meta>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "Yuki/Core/BaseUnknown.h"
#include "Yuki/Core/Dispatcher.h"
#include "Yuki/Core/Meta.h"

namespace Yuki {

    namespace Detail {

        /** @brief Return whether @p provides contains no duplicate interface IIDs. */
        template <std::size_t N>
        consteval bool HasUniqueProvideIids(const std::array<ProviderEntry, N>& provides) noexcept {
            for (std::size_t i = 0; i < N; ++i) {
                for (std::size_t j = i + 1; j < N; ++j) {
                    if (provides[i].interfaceIid == provides[j].interfaceIid) {
                        return false;
                    }
                }
            }
            return true;
        }

        /** @brief Return whether @p iids contains no duplicate values. */
        template <std::size_t N> consteval bool HasUniqueIids(const std::array<Iid, N>& iids) noexcept {
            for (std::size_t i = 0; i < N; ++i) {
                for (std::size_t j = i + 1; j < N; ++j) {
                    if (iids[i] == iids[j]) {
                        return false;
                    }
                }
            }
            return true;
        }

        /** @brief Read the unique provider dispatch policy on @p type, or the inline-facet default. */
        consteval Anno::Dispatch DispatchOfMeta(std::meta::info type) {
            std::optional<Anno::Dispatch> result;
            for (std::meta::info anno : std::meta::annotations_of(std::meta::dealias(type), ^^Anno::Dispatch)) {
                if (result) {
                    throw "Yuki::StaticMetaClass: duplicate Dispatch annotations";
                }
                result = std::meta::extract<Anno::Dispatch>(anno);
            }
            return result.value_or(Anno::Dispatch{});
        }

        /** @brief Return the direct interface type reflections declared by @ref Anno::Implements on @p type. */
        consteval std::vector<std::meta::info> ImplementedInterfaceTypesOfMeta(std::meta::info type) {
            std::vector<std::meta::info> result;
            const std::meta::info canonical = std::meta::dealias(type);
            for (std::meta::info anno : std::meta::annotations_of(canonical, ^^Anno::Implements)) {
                const Anno::Implements implements = std::meta::extract<Anno::Implements>(anno);
                for (const std::meta::info* it = implements.interfaces.begin(); it != implements.interfaces.end();
                     ++it) {
                    const std::meta::info interfaceType = std::meta::dealias(*it);
                    if (RoleOfMeta(interfaceType) != TypeOfClass::Interface) {
                        throw "Yuki::Anno::Implements entries must name Yuki interface types";
                    }
                    result.push_back(interfaceType);
                }
            }
            return result;
        }

        /** @brief Return the direct extendee type reflections declared by @ref Anno::Extends on @p type. */
        consteval std::vector<std::meta::info> ExtendeeTypesOfMeta(std::meta::info type) {
            std::vector<std::meta::info> result;
            const std::meta::info canonical = std::meta::dealias(type);
            for (std::meta::info anno : std::meta::annotations_of(canonical, ^^Anno::Extends)) {
                const Anno::Extends extends = std::meta::extract<Anno::Extends>(anno);
                for (const std::meta::info* it = extends.classes.begin(); it != extends.classes.end(); ++it) {
                    const std::meta::info extendeeType = std::meta::dealias(*it);
                    const TypeOfClass role = RoleOfMeta(extendeeType);
                    if (role == TypeOfClass::NothingType || role == TypeOfClass::BaseUnknown ||
                        role == TypeOfClass::Interface || IsTie(role)) {
                        throw "Yuki::Anno::Extends entries must name concrete object-model component classes";
                    }
                    result.push_back(extendeeType);
                }
            }
            return result;
        }

        /** @brief Return whether @p component has a template facet accessor for @p Interface. */
        template <class Component, class Interface>
        concept HasTemplateFacet = requires(Component& component) {
            { component.template Facet<Interface>() } noexcept -> std::convertible_to<Interface*>;
        };

        /** @brief Return whether @p component has a type-identity facet accessor for @p Interface. */
        template <class Component, class Interface>
        concept HasIdentityFacet = requires(Component& component) {
            { component.Facet(std::type_identity<Interface>{}) } noexcept -> std::convertible_to<Interface*>;
        };

        /** @brief Resolve a typed facet from a complete provider object. */
        template <class Component, class Interface> [[nodiscard]] Interface* FacetOf(Component& component) noexcept {
            if constexpr (std::convertible_to<Component*, Interface*>) {
                return static_cast<Interface*>(&component);
            } else if constexpr (HasTemplateFacet<Component, Interface>) {
                return component.template Facet<Interface>();
            } else if constexpr (HasIdentityFacet<Component, Interface>) {
                return component.Facet(std::type_identity<Interface>{});
            } else {
                static_assert(HasTemplateFacet<Component, Interface> || HasIdentityFacet<Component, Interface>,
                              "Yuki providers must expose Facet<I>() or Facet(type_identity<I>) for each interface.");
            }
        }

        /** @brief Bind an inline facet subobject to the provider nucleus that owns its lifetime. */
        template<class Interface>
        void BindResolvedFacet(Interface* facet, BaseUnknown* provider) noexcept {
            if (!facet || !provider) {
                return;
            }
            BaseUnknown* facetNode = static_cast<BaseUnknown*>(facet);
            BaseUnknown* anchor = provider->Nucleus();
            if (facetNode != provider && facetNode->Nucleus() != anchor) {
                facetNode->BindInlineFacetAnchor(anchor);
            }
        }

        /** @brief Resolve a declared provider edge from @p provider. */
        template<class Component, class Interface>
        [[nodiscard]] BaseUnknown* ResolveFacet(BaseUnknown* provider) noexcept {
            auto* component = static_cast<Component*>(provider);
            Interface* facet = FacetOf<Component, Interface>(*component);
            BindResolvedFacet(facet, provider);
            return facet;
        }

        /** @brief Build one provider entry for @p Component providing @p Interface. */
        template <class Component, class Interface>
        consteval ProviderEntry ProvideEntryFor(Anno::Dispatch dispatch) {
            static_assert(Traits::InterfaceClass<Interface>,
                          "Yuki metaclass providers require BaseUnknown-backed interface facets.");
            if constexpr (!std::convertible_to<Component*, Interface*>) {
                if (dispatch.kind == DispatchKind::Direct) {
                    throw "DispatchKind::Direct requires the component object to be the requested interface";
                }
            }
            return ProviderEntry{IidOf<Interface>(), dispatch.kind, &ResolveFacet<Component, Interface>, nullptr,
                                 dispatch.priority};
        }

        /** @brief Build the static provider table declared by @ref Anno::Implements on @p Self. */
        template <class Self> consteval auto MakeProvideArray() {
            constexpr std::size_t count = ImplementedInterfaceTypesOfMeta(^^std::remove_cvref_t<Self>).size();
            constexpr Anno::Dispatch dispatch = DispatchOfMeta(^^std::remove_cvref_t<Self>);
            constexpr auto interfaces =
                std::define_static_array(ImplementedInterfaceTypesOfMeta(^^std::remove_cvref_t<Self>));
            std::array<ProviderEntry, count> result{};
            std::size_t index = 0;
            template for (constexpr std::meta::info interfaceType : interfaces) {
                using Interface = typename[: interfaceType :];
                result[index++] = ProvideEntryFor<Self, Interface>(dispatch);
            }
            return result;
        }

        /** @brief Build the static extendee IID table declared by @ref Anno::Extends on @p Self. */
        template <class Self> consteval auto MakeExtendeeArray() {
            constexpr std::size_t count = ExtendeeTypesOfMeta(^^std::remove_cvref_t<Self>).size();
            if constexpr (count > 0 && !IsExtension(ClassTypeOf<Self>)) {
                throw "Yuki::Anno::Extends may only be used on extension classes";
            }
            constexpr auto extendees = std::define_static_array(ExtendeeTypesOfMeta(^^std::remove_cvref_t<Self>));
            std::array<Iid, count> result{};
            std::size_t index = 0;
            template for (constexpr std::meta::info extendeeType : extendees) {
                result[index++] = IidOfMeta(extendeeType);
            }
            return result;
        }

        /** @brief Return the direct object-model base metaclass for @p T, or null when there is no base. */
        template <class T> [[nodiscard]] const MetaClass* DirectObjectModelBaseMetaOf() noexcept {
            constexpr std::meta::info baseType = DirectObjectModelBaseTypeOfMeta(^^std::remove_cvref_t<T>);
            if constexpr (baseType == ^^void) {
                return nullptr;
            } else {
                using Base = typename[: baseType :];
                return &StaticMetaClass<Base>();
            }
        }

    } // namespace Detail

    /** @brief Return the static metaclass for @p Self, generated from object-model annotations. */
    template <class Self> [[nodiscard]] const MetaClass& StaticMetaClass() noexcept {
        static_assert(Traits::YObjectClass<Self>,
                      "StaticMetaClass<T>() requires a directly annotated Yuki object class.");

        static constexpr auto kProvides = Detail::MakeProvideArray<Self>();
        static_assert(Detail::HasUniqueProvideIids(kProvides),
                      "A Yuki object class cannot provide the same interface twice.");

        static constexpr auto kExtendees = Detail::MakeExtendeeArray<Self>();
        static_assert(Detail::HasUniqueIids(kExtendees),
                      "A Yuki extension class cannot declare the same extendee twice.");

        static const MetaClass kMeta{ClassTypeOf<Self>,
                                     IidOf<Self>(),
                                     TypeNameOf<Self>(),
                                     std::span<const ProviderEntry>{kProvides.data(), kProvides.size()},
                                     Detail::DirectObjectModelBaseIidOf<Self>(),
                                     Detail::DirectObjectModelBaseMetaOf<Self>(),
                                     std::span<const Iid>{kExtendees.data(), kExtendees.size()}};
        return kMeta;
    }

} // namespace Yuki

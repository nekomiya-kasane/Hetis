/**
 * @file MetaClass.h
 * @brief Immutable metaclass facts and dictionary-materialized object-model links.
 * @ingroup Core
 */
#pragma once

#include "Sora/Core/Traits/InheritanceTraits.h"
#include "Sora/Core/Traits/TypeTraits.h"
#include "Sora/Kernel/Core/Traits.h"
#include "Sora/Kernel/Core/ClassTypes.h"
#include "Sora/Kernel/Core/IID.h"

#include <concepts>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace Sora::Kernel {

    template<Concept::ComClass Provider>
        requires(IsInterface(Traits::RoleOf<Provider>) || IsComponent(Traits::RoleOf<Provider>))
    void RegisterKernelClass();

    /** @brief Runtime dispatch shape for a component-interface provider. */
    enum class DispatchKind : uint8_t {
        Direct = 0,      /**< Interface is the provider object itself. */
        InlineFacet = 1, /**< Facet is stored inline inside the provider frame. */
        BoundFacet = 2,  /**< Facet is a closure-owned bound object. */
    };

    /** @brief One runtime provider edge from a component class to an interface. */
    struct ProviderEntry {
        Iid interfaceIid{};                           /**< Interface provided by this entry. */
        DispatchKind kind{DispatchKind::InlineFacet}; /**< Runtime dispatch shape. */

        FacetFactory factory{};                   /**< Resolver used after the provider object is known. */
        DefaultFactory extensionFactory{};        /**< Allocates the extension provider object when it is absent. */
        std::shared_ptr<MetaClass> providerClass; /**< Class that contributed this provider edge, if known. */
    };

    /** @brief Immutable metaclass facade used by the Core runtime and dictionary. */
    class MetaClass {
    public:
        /** @brief Construct an empty metaclass sentinel. */
        constexpr MetaClass() noexcept = default;

        template<Concept::ComClass T>
        inline static std::shared_ptr<MetaClass> Query() noexcept {
            static std::shared_ptr<MetaClass> kMeta = [] {
                auto meta = std::make_shared<MetaClass>();

                constexpr auto type = Traits::RoleOf<T>;

                meta->type = type;
                meta->iid = Traits::IidOf<T>;
                meta->name = [] consteval {
                    if constexpr (std::meta::has_identifier(^^T)) {
                        return Sora::Meta::IdentifierOf(^^T);
                    } else {
                        return Sora::Meta::DisplayStringOf(^^T);
                    }
                }();

                if constexpr (std::same_as<T, BaseUnknown>) {
                    meta->base = nullptr;
                } else {
                    meta->base = MetaClass::Query<Sora::Traits::DirectBaseType<T, 0>>();
                }

                if constexpr (IsExtension(type)) {
                    template for (constexpr auto ext : Sora::Kernel::Meta::ExtendeeTypesOf<T>()) {
                        using Extendee = Sora::Meta::InfoType<ext>;
                        meta->protensions.emplace(Traits::IidOf<Extendee>, MetaClass::Query<Extendee>());
                    }
                }

                if constexpr (IsComponent(type) || IsInterface(type)) {
                    return Intern(std::move(meta));
                } else {
                    return meta;
                }
            }();
            return kMeta;
        }

        /** @brief Return the declared object-model role. */
        [[nodiscard]] constexpr TypeOfClass GetTypeOfClass() const noexcept { return type; }

        /** @brief Return the class IID. */
        [[nodiscard]] constexpr Iid GetIid() const noexcept { return iid; }

        /** @brief Return the direct object-model base metaclass, or null when it has not been materialized. */
        [[nodiscard]] constexpr const auto& GetDirectBase() const noexcept { return base; }

        /** @brief Return whether this metaclass has a direct object-model base. */
        [[nodiscard]] constexpr bool HasDirectBase() const noexcept { return base != nullptr; }

        /** @brief Return the stable diagnostic class name. */
        [[nodiscard]] constexpr const auto& GetClassName() const noexcept { return name; }

        /** @brief Return role-dependent protension edges contributed to or by this class. */
        [[nodiscard]] constexpr const auto& Protensions() const noexcept { return protensions; }

        /** @brief Return direct provider entries contributed by this class. */
        [[nodiscard]] constexpr const auto& Provides() const noexcept { return provides; }

        /** @brief Return registered classes that directly implement this interface class. */
        [[nodiscard]] constexpr const auto& Implementors() const noexcept { return protensions; }

        /** @brief Find the most-derived direct provider for @p iid in this class inheritance chain. */
        [[nodiscard]] const ProviderEntry* FindProvide(Iid iid) const noexcept;

    private:
        friend class Dictionary;

        template<Concept::ComClass Provider>
            requires(IsInterface(Traits::RoleOf<Provider>) || IsComponent(Traits::RoleOf<Provider>))
        friend void RegisterKernelClass();

        [[nodiscard]] static std::shared_ptr<MetaClass> Intern(std::shared_ptr<MetaClass> meta) noexcept;

        TypeOfClass type{TypeOfClass::NothingType}; /**< Declared object-model role. */
        std::string_view name{};                    /**< Reflected or section-provided diagnostic name. */
        Iid iid{};                                  /**< Class identifier. */

        std::shared_ptr<MetaClass> base; /**< Direct object-model base metaclass, when materialized. */
        std::unordered_map<Iid, std::shared_ptr<MetaClass>> protensions{}; /**< Role-dependent class relation edges. */

        std::unordered_map<Iid, ProviderEntry> provides{}; /**< Direct providers contributed by this class. */
    };

} // namespace Sora::Kernel

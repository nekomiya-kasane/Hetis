/**
 * @file Meta.h
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

namespace Sora::Kernel {

    /** @brief Runtime dispatch shape for a component-interface provider. */
    enum class DispatchKind : uint8_t {
        Direct = 0,                 /**< Interface is the provider object itself. */
        InlineFacet = 1,            /**< Facet is stored inline inside the provider frame. */
        BoundFacet = 2,             /**< Facet is a closure-owned bound object. */
        AttachedExtension = 3,      /**< Facet is provided by an attached extension object. */
        CodeExtensionSingleton = 4, /**< Facet is provided by a singleton code extension. */
    };

    /** @brief One runtime provider edge from a component class to an interface. */
    struct ProviderEntry {
        Iid interfaceIid{};                           /**< Interface provided by this entry. */
        DispatchKind kind{DispatchKind::InlineFacet}; /**< Runtime dispatch shape. */

        FacetFactory factory{};                   /**< Resolver used after the provider object is known. */
        std::shared_ptr<MetaClass> providerClass; /**< Class that contributed the provider, if known. */

        uint32_t priority{2}; /**< Lower value wins when providers are merged. */
    };

    /** @brief Immutable metaclass facade used by the Core runtime and dictionary. */
    class MetaClass : std::enable_shared_from_this<MetaClass> {
    public:
        /** @brief Construct an empty metaclass sentinel. */
        constexpr MetaClass() noexcept = default;

        template<Concept::ComClass T>
        inline static std::shared_ptr<MetaClass> Query() noexcept {
            static std::shared_ptr<MetaClass> kMeta = nullptr;
            if (kMeta) {
                return kMeta->shared_from_this();
            }

            kMeta = std::make_shared<MetaClass>();

            constexpr auto type = Traits::RoleOf<T>;

            kMeta->type = type;
            kMeta->iid = Traits::IidOf<T>;
            kMeta->name = Sora::Meta::IdentifierOf(^^T);

            if constexpr (std::same_as<T, BaseUnknown>) {
                kMeta->base = nullptr;
            } else {
                kMeta->base = MetaClass::Query<Sora::Traits::DirectBaseType<T, 0>>();
            }

            if constexpr (IsExtension(type)) {
                template for (constexpr auto ext : Sora::Kernel::Meta::ExtendeeTypesOf(^^T)) {
                    using Extendee = Sora::Meta::InfoType<ext>;
                    kMeta->protensions.emplace(Traits::IidOf<Extendee>, MetaClass::Query<Extendee>());
                }
            }

            if constexpr (IsComponent(type)) {
                template for (constexpr auto iface : Sora::Kernel::Meta::ImplementedInterfaceTypesOf(^^T)) {
                    using Interface = Sora::Meta::InfoType<iface>;
                    ProviderEntry entry{};
                    entry.interfaceIid = Traits::IidOf<Interface>;
                    if constexpr (std::is_base_of_v<Interface, T>) {
                        entry.kind = DispatchKind::Direct;
                        entry.factory = +[](BaseUnknown* provider) -> BaseUnknown* { return provider; };
                    } else if constexpr (IsCodeExtension(type)) {
                        entry.kind = DispatchKind::CodeExtensionSingleton;
                        // TODO:
                    } else {
                        entry.kind = DispatchKind::BoundFacet;
                        entry.factory = +[](BaseUnknown* provider) -> BaseUnknown* {
                            
                        };
                    }
                    entry.providerClass = kMeta;
                    entry.priority = 0; // TODO: Allow priority to be declared in the class annotation.
                    kMeta->provides.emplace(entry.interfaceIid, entry);
                }
            }

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
        [[nodiscard]] constexpr const auto GetClassName() const noexcept { return name; }

        /** @brief Return components extended by this class. */
        [[nodiscard]] constexpr const auto& Protensions() const noexcept { return protensions; }

        /** @brief Return direct provider entries contributed by this class. */
        [[nodiscard]] constexpr const auto& Provides() const noexcept { return provides; }

        /** @brief Find the highest-priority direct provider for @p iid in this class. */
        [[nodiscard]] const ProviderEntry* FindProvide(Iid iid) const noexcept;

    private:
        friend class Dictionary;

        TypeOfClass type{TypeOfClass::NothingType}; /**< Declared object-model role. */
        std::string_view name{};                    /**< Reflected or section-provided diagnostic name. */
        Iid iid{};                                  /**< Class identifier. */

        std::shared_ptr<MetaClass> base; /**< Direct object-model base metaclass, when materialized. */
        std::unordered_map<Iid, std::shared_ptr<MetaClass>> protensions{}; /**< All extensi */

        std::unordered_map<Iid, ProviderEntry> provides{}; /**< Direct providers contributed by this class. */
    };

} // namespace Sora::Kernel
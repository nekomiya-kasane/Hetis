/**
 * @file ObjectModelInternal.h
 * @brief Private closure graph, provider registration, and bound-facet materialization helpers.
 * @ingroup Core
 */
#pragma once

#include "Sora/Kernel/Core/BaseObject.h"
#include "Sora/Kernel/Core/ClassTypes.h"
#include "Sora/Kernel/Core/IID.h"
#include "Sora/Kernel/Core/MetaClass.h"

#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Kernel {

    namespace Tie {

        /** @brief Private mapping from an interface and provider class to the module-defined TIE class. */
        template<Concept::InterfaceClass Iface, Concept::ComponentClass Component>
        struct TieClass;

        /**
         * @brief Return the bound TIE class that adapts @p Component to @p Iface.
         * @tparam Iface Interface class requested by QueryInterface.
         * @tparam Component Implementation or extension class providing the interface.
         */
        template<Concept::InterfaceClass Iface, Concept::ComponentClass Component>
        using TieClassOf = typename TieClass<Iface, Component>::Type;

        /**
         * @brief Allocate and bind a closure-owned TIE facet for @p provider.
         * @tparam Iface Interface requested from the provider.
         * @tparam Component Concrete implementation or extension type behind @p provider.
         */
        template<Concept::InterfaceClass Iface, Concept::ComponentClass Component>
        [[nodiscard]] BaseUnknown* MakeBoundFacet(BaseUnknown* provider) noexcept {
            using TieClass = TieClassOf<Iface, Component>;
            auto* facet = new (std::nothrow) TieClass;
            if (!facet) {
                return nullptr;
            }
            Detail::BaseUnknownInternal::BindBoundTarget(facet, provider);
            return facet;
        }

    } // namespace Tie

    /**
     * @brief Register the object-model class node and all declaration edges contributed by @p Provider.
     * @tparam Provider Interface, implementation, or extension class whose metadata is being registered.
     */
    template<Concept::ComClass Provider>
        requires(IsInterface(Traits::RoleOf<Provider>) || IsComponent(Traits::RoleOf<Provider>))
    void RegisterKernelClass() {
        auto meta = MetaClass::Query<Provider>();

        if constexpr (IsComponent(Traits::RoleOf<Provider>)) {
            if constexpr (IsExtension(Traits::RoleOf<Provider>)) {
                ProviderEntry selfEntry{};
                selfEntry.targetIid = Traits::IidOf<Provider>;
                selfEntry.kind = DispatchKind::Direct;
                selfEntry.factory = +[](BaseUnknown* provider) noexcept -> BaseUnknown* { return provider; };
                selfEntry.extensionFactory = +[]() -> BaseUnknown* { return new (std::nothrow) Provider; };
                selfEntry.providerClass = meta;
                meta->provides.insert_or_assign(selfEntry.targetIid, std::move(selfEntry));
            }

            template for (constexpr auto iface : Sora::Kernel::Meta::ImplementedInterfaceTypesOf<Provider>()) {
                using Iface = Sora::Meta::InfoType<iface>;

                ProviderEntry entry{};
                entry.targetIid = Traits::IidOf<Iface>;
                if constexpr (std::is_base_of_v<Iface, Provider>) {
                    entry.kind = DispatchKind::Direct;
                    entry.factory = +[](BaseUnknown* provider) noexcept -> BaseUnknown* { return provider; };
                } else {
                    entry.kind = DispatchKind::BoundFacet;
                    entry.factory = +[](BaseUnknown* provider) noexcept -> BaseUnknown* {
                        return Tie::MakeBoundFacet<Iface, Provider>(provider);
                    };
                }

                if constexpr (IsExtension(Traits::RoleOf<Provider>)) {
                    entry.extensionFactory = +[]() -> BaseUnknown* { return new (std::nothrow) Provider; };
                }

                entry.providerClass = meta;
                meta->provides.insert_or_assign(entry.targetIid, std::move(entry));
                MetaClass::Query<Iface>()->protensions.insert_or_assign(Traits::IidOf<Provider>, meta);
            }

            if constexpr (IsExtension(Traits::RoleOf<Provider>)) {
                template for (constexpr auto extendee : Sora::Kernel::Meta::ExtendeeTypesOf<Provider>()) {
                    using Extendee = Sora::Meta::InfoType<extendee>;
                    auto extendeeMeta = MetaClass::Query<Extendee>();
                    extendeeMeta->protensions.insert_or_assign(Traits::IidOf<Provider>, meta);
                }
            }
        }
    }

} // namespace Sora::Kernel

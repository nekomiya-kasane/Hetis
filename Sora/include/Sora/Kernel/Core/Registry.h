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

        /**
         * @brief Register all direct @ref Sora::Kernel::$::Implements interfaces declared by @p Provider.
         * @tparam Provider Implementation or extension class whose direct providers are being registered.
         */
        template<Concept::ComponentClass Provider>
        void RegisterObjectProviders() {
            auto meta = MetaClass::Query<Provider>();

            template for (constexpr auto iface : Sora::Kernel::Meta::ImplementedInterfaceTypesOf<Provider>()) {
                using Iface = Sora::Meta::InfoType<iface>;

                ProviderEntry entry{};
                entry.interfaceIid = Traits::IidOf<Iface>;
                if constexpr (std::is_base_of_v<Iface, Provider>) {
                    entry.kind = DispatchKind::Direct;
                    entry.factory = +[](BaseUnknown* provider) noexcept -> BaseUnknown* { return provider; };
                } else {
                    entry.kind = DispatchKind::BoundFacet;
                    entry.factory = +[](BaseUnknown* provider) noexcept -> BaseUnknown* {
                        return MakeBoundFacet<Iface, Provider>(provider);
                    };
                }

                if constexpr (IsExtension(Traits::RoleOf<Provider>)) {
                    entry.extensionFactory = +[]() -> BaseUnknown* { return new (std::nothrow) Provider; };
                }

                entry.providerClass = meta;
                meta->provides.insert_or_assign(entry.interfaceIid, std::move(entry));
            }

            if constexpr (IsExtension(Traits::RoleOf<Provider>)) {
                template for (constexpr auto extendee : Sora::Kernel::Meta::ExtendeeTypesOf<Provider>()) {
                    using Extendee = Sora::Meta::InfoType<extendee>;
                    auto extendeeMeta = MetaClass::Query<Extendee>();
                    extendeeMeta->protensions.insert_or_assign(Traits::IidOf<Provider>, meta);
                }
            }
        }

    } // namespace Tie

} // namespace Sora::Kernel

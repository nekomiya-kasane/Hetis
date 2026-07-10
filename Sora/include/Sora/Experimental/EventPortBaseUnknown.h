/**
 * @file EventPortBaseUnknown.h
 * @brief BaseUnknown adaptor for the experimental event port.
 * @ingroup Experimental
 *
 * @details This bridge keeps @ref Sora::Experimental::EventPort physically independent from the COM object model.
 * Include this header only when BaseUnknown objects should participate in the experimental event protocol.
 */
#pragma once

#include "Sora/Experimental/EventPort.h"
#include "Sora/Kernel/Core/BaseObject.h"
#include "Sora/Kernel/Core/ComAdaptor.h"
#include "Sora/Kernel/Core/ComPtr.h"
#include "Sora/Kernel/Core/KernelSection.h"
#include "Sora/Kernel/Core/Query.h"
#include "Sora/Kernel/Core/Registry.h"

#include <cassert>
#include <memory>
#include <mutex>

namespace Sora::Kernel {

    template<>
    struct ComAdaptorDecl<Sora::Experimental::EventPort> {
        static constexpr TypeOfClass role = TypeOfClass::DataExtension;
        using Extends = Sora::Traits::TypeList<Sora::Kernel::BaseUnknown>;
        using Implements = Sora::Traits::TypeList<>;
    };

} // namespace Sora::Kernel

namespace Sora::Experimental {

    namespace Detail {

        /** @brief Register the BaseUnknown event-port extension provider for header-only bridge users. */
        inline void EnsureBaseUnknownEventPortExtensionRegistered() {
            static std::once_flag once;
            std::call_once(once, [] {
                Sora::Kernel::RegisterKernelClass<Sora::Kernel::ComAdaptor<Sora::Experimental::EventPort>>();
            });
        }

    } // namespace Detail

} // namespace Sora::Experimental

namespace Sora::Kernel {

    namespace Detail {

        inline constexpr auto kExperimentalEventPortManifest = [] consteval {
            return Sora::Kernel::KernelManifest{
                .classes = {^^Sora::Kernel::ComAdaptor<Sora::Experimental::EventPort>},
            };
        }();

        [[maybe_unused]] inline constinit auto const& kExperimentalEventPortKernelSection =
            Sora::Kernel::KernelSection<kExperimentalEventPortManifest>::anchor;

    } // namespace Detail

    /** @brief ADL adaptor that gives BaseUnknown objects an experimental event port through DataExtension QI. */
    [[nodiscard]] inline Sora::Experimental::EventPort& EventPortOf(BaseUnknown& object) {
        Sora::Experimental::Detail::EnsureBaseUnknownEventPortExtensionRegistered();
        auto* extension = QueryInterface<Sora::Kernel::ComAdaptor<Sora::Experimental::EventPort>>(object);
        assert(extension != nullptr && "ComAdaptor<EventPort> must be available as a DataExtension.");
        return extension->Object();
    }

    /** @brief ADL adaptor that strongly retains a BaseUnknown nucleus while deferred delivery is pending. */
    [[nodiscard]] inline Sora::Experimental::EventLifetimeToken EventLifetimeOf(BaseUnknown& object) {
        BaseUnknown* nucleus = object.Nucleus();
        assert(nucleus != nullptr);
        using Holder = ComPtr<BaseUnknown>;
        auto holder = std::make_shared<Holder>(nucleus);
        return Sora::Experimental::EventLifetimeToken::Retained(nucleus, std::static_pointer_cast<void>(holder));
    }

} // namespace Sora::Kernel

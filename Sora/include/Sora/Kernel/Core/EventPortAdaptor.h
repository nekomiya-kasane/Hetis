/**
 * @file EventPortAdaptor.h
 * @brief BaseUnknown adaptor for the core event port.
 * @ingroup Core
 *
 * @details This bridge keeps @ref Sora::EventPort physically independent from the COM object model.
 * Include this header only when BaseUnknown objects should participate in the core event protocol.
 */
#pragma once

#include "Sora/Core/EventPort.h"
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
    struct ComAdaptorDecl<Sora::EventPort> {
        static constexpr TypeOfClass role = TypeOfClass::DataExtension;
        using Extends = Sora::Traits::TypeList<Sora::Kernel::BaseUnknown>;
        using Implements = Sora::Traits::TypeList<>;
    };

} // namespace Sora::Kernel

namespace Sora {

    namespace Detail {

        /** @brief Register the BaseUnknown event-port extension provider for header-only bridge users. */
        inline void EnsureBaseUnknownEventPortExtensionRegistered() {
            static std::once_flag once;
            std::call_once(once,
                           [] { Sora::Kernel::RegisterKernelClass<Sora::Kernel::ComAdaptor<Sora::EventPort>>(); });
        }

    } // namespace Detail

} // namespace Sora

namespace Sora::Kernel {

    namespace Detail {

        inline constexpr auto kEventPortManifest = [] consteval {
            return Sora::Kernel::KernelManifest{
                .classes = {^^Sora::Kernel::ComAdaptor<Sora::EventPort>},
            };
        }();

        [[maybe_unused]] inline constinit auto const& kEventPortKernelSection =
            Sora::Kernel::KernelSection<kEventPortManifest>::anchor;

    } // namespace Detail

    namespace Detail {

        /** @brief Weak BaseUnknown lifetime state used by the EventPort bridge. */
        struct EventLifetimeState {
            WeakRef weak{}; /**< Weak reference to the observed closure nucleus. */
        };

        /** @brief Try to acquire a BaseUnknown lease for one concrete event delivery. */
        [[nodiscard]] inline Sora::EventObjectLease AcquireEventLifetime(const Sora::EventObjectLifetime& lifetime,
                                                                         Sora::EventLeaseMode) noexcept {
            auto state = static_cast<EventLifetimeState*>(lifetime.CustomState().get());
            if (!state) {
                return {};
            }
            BaseUnknown* nucleus = state->weak.Get();
            if (!nucleus) {
                return {};
            }

            using Holder = ComPtr<BaseUnknown>;
            auto holder = std::make_shared<Holder>(nucleus);
            std::shared_ptr<void> lease{std::move(holder), lifetime.Object()};
            return Sora::EventObjectLease::Owned(std::move(lease));
        }

    } // namespace Detail

    /** @brief ADL adaptor that gives BaseUnknown objects an event port through DataExtension QI. */
    [[nodiscard]] inline Sora::EventPort& EventPortOf(BaseUnknown& object) {
        Sora::Detail::EnsureBaseUnknownEventPortExtensionRegistered();
        auto* extension = QueryInterface<Sora::Kernel::ComAdaptor<Sora::EventPort>>(object);
        assert(extension != nullptr && "ComAdaptor<EventPort> must be available as a DataExtension.");
        return extension->Object();
    }

    /** @brief ADL adaptor that models a BaseUnknown nucleus lifetime without extending it at connection time. */
    [[nodiscard]] inline Sora::EventObjectLifetime MakeEventLifetime(BaseUnknown& object) {
        BaseUnknown* nucleus = object.Nucleus();
        assert(nucleus != nullptr);
        auto state = std::make_shared<Detail::EventLifetimeState>();
        state->weak = nucleus->GetComponentWeakRef();
        return Sora::EventObjectLifetime::Custom(std::addressof(object), std::static_pointer_cast<void>(state),
                                                 &Detail::AcquireEventLifetime);
    }

} // namespace Sora::Kernel

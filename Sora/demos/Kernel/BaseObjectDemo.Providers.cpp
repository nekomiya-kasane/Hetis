/**
 * @file BaseObjectDemo.Providers.cpp
 * @brief Private provider registration unit for the BaseObject demo component classes.
 * @ingroup Core
 */

#include "Common/Private/ClassTies.h"

#include "Sora/Kernel/Core/Registry.h"

namespace {

    /** @brief Register all provider factories contributed by the demo position classes before main. */
    struct ProviderRegistration {
        /** @brief Materialize provider records into each class metaclass. */
        ProviderRegistration() {
            Sora::Kernel::Tie::RegisterObjectProviders<Sora::Kernel::Position2DImpl>();
            Sora::Kernel::Tie::RegisterObjectProviders<Sora::Kernel::Position3DImpl>();
        }
    };

    const ProviderRegistration kProviderRegistration{};

} // namespace

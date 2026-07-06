/**
 * @file BaseObjectDemo.Providers.cpp
 * @brief Provider section emission unit for the BaseObject demo component classes.
 * @ingroup Core
 */

#include "Common/Private/ClassTies.h"

#include "Sora/Kernel/Core/ProviderSection.h"

namespace {

    inline constexpr auto BaseObjectDemoProviders = [] consteval {
        return Sora::Kernel::ProviderManifest{
            .types =
                {
                    ^^Sora::Kernel::Position2DImpl,
                    ^^Sora::Kernel::Position3DImpl,
                    ^^Sora::Kernel::Position2DExtension,
                },
        };
    }();

    consteval {
        Sora::Kernel::ValidateProviderManifest<BaseObjectDemoProviders>();
    }

    [[maybe_unused]] constinit auto const& kBaseObjectDemoProviders =
        Sora::Kernel::KernelSection<BaseObjectDemoProviders>::anchor;

} // namespace

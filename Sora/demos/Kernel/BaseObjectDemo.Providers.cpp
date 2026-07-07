/**
 * @file BaseObjectDemo.Providers.cpp
 * @brief Provider section emission unit for the BaseObject demo component classes.
 * @ingroup Core
 */

#include "Common/Private/ClassTies.h"

#include "Sora/Kernel/Core/KernelSection.h"

namespace {

    inline constexpr auto BaseObjectDemoKernelClasses = [] consteval {
        return Sora::Kernel::KernelManifest{
            .classes =
                {
                    ^^Sora::Kernel::IPosition,
                    ^^Sora::Kernel::I3DPosition,
                    ^^Sora::Kernel::ITag,
                    ^^Sora::Kernel::Position2DImpl,
                    ^^Sora::Kernel::Position3DImpl,
                    ^^Sora::Kernel::PointWithoutInterfaceImpl,
                    ^^Sora::Kernel::FutureExtensiblePointImpl,
                    ^^Sora::Kernel::Position2DExtension,
                    ^^Sora::Kernel::PointTagExtension,
                },
        };
    }();

    consteval {
        Sora::Kernel::ValidateKernelManifest<BaseObjectDemoKernelClasses>();
    }

    [[maybe_unused]] constinit auto const& kBaseObjectDemoKernelClasses =
        Sora::Kernel::KernelSection<BaseObjectDemoKernelClasses>::anchor;

} // namespace

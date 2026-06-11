#pragma once

#include "Mashiro/Core/TypeTraits.h"
#include "Mashiro/Core/Flags.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Mashiro/Geom/Geom.h"
#include "Mashiro/Math/Types.h"

namespace Mashiro {

    namespace Window {

        enum class Flags : uint32_t {
            None = 0,
            Resizable = 1 << 0,
            Minimizable = 1 << 1,
            Maximizable = 1 << 2,
            Decorated = 1 << 3,
            Visible = 1 << 4,
            Focusable = 1 << 5,
            Floating = 1 << 6,
            Transparent = 1 << 7,
            HighDpi = 1 << 8,
        };

        enum class Mode {
            Windowed,
            Fullscreen,
            Borderless,
        };

        enum class PlacementMode {
            Default,
            Centered,
            Explicit,
        };

        struct Placement {
            PlacementMode mode = PlacementMode::Default;
            ivec2 position{};
            uint32_t monitorId = 0;
        };

        struct SizeRange {
            ivec2 min{};
            ivec2 max{};
        };

        enum class DpiAwareness {
            Unaware,
            SystemAware,
            PerMonitorAware,

        };

        struct WindowDesc {
            std::string title = "<Untitled>";

            ivec2 size{1280, 720};
            SizeRange sizeRange{};

            Flags flags = Flags::Resizable | Flags::Visible | Flags::Minimizable |
                          Flags::Maximizable | Flags::HighDpi | Flags::Decorated;
            DpiAwareness dpiAwareness = DpiAwareness::PerMonitorAware;
            
            Mode mode = Mode::Windowed;
            Placement placement{};
        };

        template <typename Impl>
        class WindowManager {
            
        };

    } // namespace Window

} // namespace Mashiro

#pragma once

/**
 * @file Parser.h
 * @brief Runtime parser declarations for sealed Sora command-line programs.
 * @ingroup Core
 */

#include <span>
#include <string_view>

#include <Sora/Core/CLI/Descriptions.h>

namespace Sora::CLI {

    /** @brief Non-owning view of command-line tokens. */
    struct ArgvView {
        std::span<std::string_view const> tokens = {};
    };

    template<typename Program>
    struct ParseViewResult;

} // namespace Sora::CLI

/**
 * @file LogCategory.h
 * @brief Shared semantic domains for logging and profiling diagnostics.
 * @ingroup Core
 */
#pragma once

#include <cstdint>

namespace Sora {

    /** @brief Built-in diagnostic domains; explicitly cast extension values remain supported. */
    enum class LogCategory : uint8_t {
        Core = 0,     /**< Core library, memory, diagnostics, and platform services. */
        Kernel = 1,   /**< Object model and Kernel runtime. */
        Resource = 2, /**< Resource registry, packages, and IO. */
        Render = 3,   /**< Rendering backend and frame graph. */
        Scene = 4,    /**< Scene, entity, and world management. */
        Input = 5,    /**< User input and window events. */
        Audio = 6,    /**< Audio devices and mixing. */
        Network = 7,  /**< Network IO and protocols. */
        Script = 8,   /**< Scripting and dynamic command execution. */
        Editor = 9,   /**< Editor and tooling UI. */
        App = 10,     /**< Application layer. */
    };

} // namespace Sora

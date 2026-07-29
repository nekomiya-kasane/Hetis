/**
 * @file LogCategory.h
 * @brief Shared semantic domains for logging and profiling diagnostics.
 * @ingroup Core
 */
#pragma once

#include <cstdint>

namespace Sora {

    /** @brief Diagnostic severity ordered from most verbose to most severe. */
    enum class LogLevel : uint8_t {
        Trace = 0, /**< Finest-grained execution diagnostics. */
        Debug = 1, /**< Debug-time state useful while developing or investigating a subsystem. */
        Info = 2,  /**< Normal operational information. */
        Warn = 3,  /**< Suspicious condition that did not prevent the current operation from continuing. */
        Error = 4, /**< Recoverable failure. */
        Fatal = 5, /**< Unrecoverable failure; callers normally abort or start crash handling after logging. */
    };

    namespace $ {

        /** @brief Built-in runtime threshold attached to one @ref LogCategory enumerator. */
        struct DefaultLogLevel {
            LogLevel value = LogLevel::Trace; /**< Minimum enabled severity. */
        };

    } // namespace $

    /** @brief Built-in diagnostic domains; explicitly cast extension values remain supported. */
    enum class LogCategory : uint8_t {
        Core [[= $::DefaultLogLevel{LogLevel::Info}]] = 0,         /**< Core library and platform services. */
        Kernel [[= $::DefaultLogLevel{LogLevel::Info}]] = 1,       /**< Object model and Kernel runtime. */
        Resource [[= $::DefaultLogLevel{LogLevel::Info}]] = 2,     /**< Resource registry, packages, and IO. */
        Render [[= $::DefaultLogLevel{LogLevel::Info}]] = 3,       /**< Rendering backend and frame graph. */
        Scene [[= $::DefaultLogLevel{LogLevel::Debug}]] = 4,       /**< Scene, entity, and world management. */
        Input [[= $::DefaultLogLevel{LogLevel::Debug}]] = 5,       /**< User input and window events. */
        Audio [[= $::DefaultLogLevel{LogLevel::Info}]] = 6,        /**< Audio devices and mixing. */
        Network [[= $::DefaultLogLevel{LogLevel::Info}]] = 7,      /**< Network IO and protocols. */
        Script [[= $::DefaultLogLevel{LogLevel::Debug}]] = 8,      /**< Scripting and dynamic commands. */
        Editor [[= $::DefaultLogLevel{LogLevel::Debug}]] = 9,      /**< Editor and tooling UI. */
        App [[= $::DefaultLogLevel{LogLevel::Trace}]] = 10,        /**< Application layer. */
    };

} // namespace Sora

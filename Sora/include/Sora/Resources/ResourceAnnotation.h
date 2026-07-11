/**
 * @file ResourceAnnotation.h
 * @brief C++26 reflection annotations for resource-tree declarations.
 * @ingroup Resources
 */
#pragma once

#include <Sora/Core/FixedString.h>
#include <Sora/Resources/Format.h>

namespace Sora {

    namespace Resources {

        namespace $ {

            /** @brief Annotation payload for reflected namespace and variable resource declarations. */
            struct Resource {
                /**
                 * @brief Resource URI fragment.
                 *
                 * @details On namespaces this is a resource-tree prefix. On variables this is the resource leaf URI.
                 * Absolute @c res://... values reset the base; relative values are resolved against the active
                 * namespace base. Empty values inherit the namespace base and infer the variable leaf from its
                 * identifier.
                 */
                FixedString<256> uri{};
                ResourceType type = ResourceType::Unknown; /**< Semantic type override, or @c Unknown to inherit. */
                FixedString<32> extension{};               /**< Filename suffix used when a variable URI is inferred. */
                constexpr bool operator==(const Resource&) const = default;
            };

        } // namespace $

    } // namespace Resources

    namespace $ {

        inline namespace Resources {

            using Resource = Sora::Resources::$::Resource;

        }

    } // namespace $

} // namespace Sora

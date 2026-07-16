/**
 * @file CommandRuntimeReflection.h
 * @brief Parameter schema generation for the experimental CAD command runtime.
 * @ingroup KernelExperimental
 */
#pragma once

#include "CommandRuntime.h"

#include <concepts>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#if defined(__cpp_reflection) && __has_include(<experimental/meta>)
#include <experimental/meta>
#define SORA_COMMAND_RUNTIME_HAS_STATIC_REFLECTION 1
#else
#define SORA_COMMAND_RUNTIME_HAS_STATIC_REFLECTION 0
#endif

namespace Sora::Kernel::Experimental::CommandRuntime::Reflection {

    struct SchemaGenerationStatus {
        bool static_reflection_available{};
        std::string_view fallback{};
    };

    [[nodiscard]] inline constexpr SchemaGenerationStatus Status() noexcept {
        return SchemaGenerationStatus{
            .static_reflection_available = SORA_COMMAND_RUNTIME_HAS_STATIC_REFLECTION != 0,
            .fallback = "manual CommandParameterSchema()",
        };
    }

    template<typename T>
    concept ManualParameterSchemaProvider = requires {
        { std::remove_cvref_t<T>::CommandParameterSchema() } -> std::same_as<ParameterSchema>;
    };

    template<typename T>
    [[nodiscard]] consteval ParameterValueKind ParameterKindOf() noexcept {
        using Value = std::remove_cvref_t<T>;
        if constexpr (std::same_as<Value, double> || std::same_as<Value, float> || std::same_as<Value, int> ||
                      std::same_as<Value, uint32_t> || std::same_as<Value, uint64_t>) {
            return ParameterValueKind::Number;
        } else if constexpr (std::same_as<Value, bool>) {
            return ParameterValueKind::Boolean;
        } else if constexpr (std::same_as<Value, std::string>) {
            return ParameterValueKind::Text;
        } else {
            return ParameterValueKind::Unknown;
        }
    }

    [[nodiscard]] inline ParameterValueKind KindFromReflectedTypeName(std::string_view type) noexcept {
        if (type == "double" || type == "float" || type == "int" || type == "uint32_t" || type == "uint64_t") {
            return ParameterValueKind::Number;
        }
        if (type == "bool") {
            return ParameterValueKind::Boolean;
        }
        if (type == "std::string") {
            return ParameterValueKind::Text;
        }
        return ParameterValueKind::Unknown;
    }

#if SORA_COMMAND_RUNTIME_HAS_STATIC_REFLECTION

    template<typename T>
    [[nodiscard]] ParameterSchema ReflectParameterSchema() {
        namespace Meta = std::meta;

        ParameterSchema schema{
            .name = std::string(Meta::display_string_of(^^T)),
            .generated_from_reflection = true,
        };
        const auto members = Meta::nonstatic_data_members_of(^^T, Meta::access_context::unchecked());
        for (const auto member : members) {
            const auto type = Meta::display_string_of(Meta::type_of(member));
            schema.fields.push_back(ParameterField{
                .name = std::string(Meta::identifier_of(member)),
                .kind = KindFromReflectedTypeName(type),
                .required = true,
            });
        }
        return schema;
    }

#endif

    template<typename T>
    [[nodiscard]] ParameterSchema SchemaOf() {
        if constexpr (ManualParameterSchemaProvider<T>) {
            ParameterSchema schema = std::remove_cvref_t<T>::CommandParameterSchema();
            schema.generated_from_reflection = false;
            return schema;
        } else {
#if SORA_COMMAND_RUNTIME_HAS_STATIC_REFLECTION
            return ReflectParameterSchema<T>();
#else
            return ParameterSchema{.name = "unavailable", .generated_from_reflection = false};
#endif
        }
    }

} // namespace Sora::Kernel::Experimental::CommandRuntime::Reflection

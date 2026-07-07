#pragma once

/**
 * @file Program.h
 * @brief Program-level entry points for Sora command-line parsing.
 * @ingroup Core
 */

#include <concepts>

#include <Sora/Core/CLI/Descriptions.h>
#include <Sora/Core/CLI/Schema.h>

namespace Sora::CLI {

    template<typename Root>
    struct Program {
        using RootType = Root;

        NormalizedSchema schema = {};
    };

    template<Concept::ProgramRoot Root>
    consteval auto Compile() -> Program<Root> {
        SchemaBuilder<Root> builder;
        Root::BuildSchema(builder);
        return Program<Root>{.schema = builder.Seal()};
    }

    template<typename Root, typename SchemaFactory>
    consteval auto Compile(SchemaFactory factory) -> Program<Root> {
        SchemaBuilder<Root> builder;

        if constexpr (requires { { factory(builder) } -> std::same_as<NormalizedSchema>; }) {
            return Program<Root>{.schema = factory(builder)};
        } else {
            factory(builder);
            return Program<Root>{.schema = builder.Seal()};
        }
    }

} // namespace Sora::CLI

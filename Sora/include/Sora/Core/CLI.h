#pragma once

namespace Mashiro {

    namespace CLI {

        template<typename Root>
        struct Program;

        template<typename Root>
        consteval auto Compile() -> Program<Root>;

        template<typename Root, typename SchemaFactory>
        consteval auto Compile(SchemaFactory) -> Program<Root>;

    } // namespace CLI

} // namespace Mashiro

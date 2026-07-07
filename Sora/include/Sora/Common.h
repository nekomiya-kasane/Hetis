/**
 * @file Meta.h
 * @brief Foundational compile-time metaprogramming primitives.
 * @ingroup Core
 *
 * @details Provides small, domain-agnostic building blocks for P1306 @c template @c for expansion and P2996
 * reflection-based code generation across the Sora codebase. The declarations in this file are pure
 * metaprogramming vocabulary rather than algebraic, geometric, or runtime type-trait facilities.
 *
 * Every facility here is @c consteval or @c constexpr oriented, has no runtime footprint, and is self-contained with
 * respect to its standard-library dependencies.
 */
#pragma once

#include <meta>
#include <ranges>

namespace Sora {

    namespace $ {}

    namespace Meta {

        /**
         * @brief Static index sequence @c [0, @c 1, @c ..., @c N @c - @c 1] for P1306 expansion statements.
         *
         * @details Promoted to static storage through P3491 @c std::define_static_array so it can drive a
         * @c template @c for expansion. The compiler fully unrolls uses with the small extents expected by this layer,
         * producing the same runtime shape as hand-written indexed code.
         *
         * This name intentionally lives at the root metaprogramming namespace so unqualified lookup can find the same
         * sequence from nested implementation namespaces without introducing another forwarding alias.
         *
         * @tparam N Number of indices in the sequence. The intended domain is @c N >= 0.
         *
         * @code{.cpp}
         * template for (constexpr int i : Iota<N>) {
         *     result[i] = a[i] + b[i];
         * }
         * @endcode
         */
        template<int N>
        inline constexpr auto Iota = std::define_static_array(std::views::iota(0, N));

    } // namespace Meta

    namespace Concept {}

    namespace Traits {}

    namespace Detail {}

} // namespace Sora

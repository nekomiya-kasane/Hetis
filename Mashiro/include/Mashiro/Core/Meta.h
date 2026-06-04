/**
 * @file Meta.h
 * @brief Foundational compile-time metaprogramming primitives.
 *
 * Home for the small, domain-agnostic building blocks that drive P1306 `template for`
 * expansion and P2996 reflection-based code generation across the codebase. These are
 * *not* algebraic, geometric, or type-trait facilities — they are pure metaprogramming
 * vocabulary, so they live in `Mashiro::Core` and sit beneath every other layer.
 *
 * Contents:
 *   - `Iota<N>` — a static-storage index sequence `[0, 1, …, N-1]` for `template for`.
 *
 * Everything here is `consteval`/`constexpr`, has zero runtime footprint, and is fully
 * self-contained (pulls its own `<meta>` / `<ranges>` dependencies).
 *
 * Namespace: `Mashiro`.
 *
 * @ingroup Core
 */
#pragma once

#include <meta>
#include <ranges>

namespace Mashiro {

    /**
     * @brief Static index sequence `[0, 1, …, N-1]` for P1306 `template for` expansion.
     *
     * Promoted to static storage via P3491 `define_static_array` so it can drive an
     * expansion statement, which the compiler fully unrolls for the small extents used
     * throughout the math layer — the emitted code is identical to a hand-written,
     * fully-unrolled loop with no runtime overhead.
     *
     * Defined at `Mashiro` scope (deliberately *not* in a nested `Detail` namespace) so
     * an unqualified `Iota` resolves identically from both `Mashiro` (e.g. the `Vec` /
     * `Mat` operators) and `Mashiro::Math` (e.g. `VecOps` / `MatOps`); a nested `Detail`
     * would be shadowed by the unrelated `Mashiro::Math::Detail`.
     *
     * @tparam N Sequence length (non-negative).
     *
     * @code
     * template for (constexpr int i : Iota<N>) {
     *     result[i] = a[i] + b[i];
     * }
     * @endcode
     */
    template<int N>
    inline constexpr auto Iota = std::define_static_array(std::views::iota(0, N));

} // namespace Mashiro

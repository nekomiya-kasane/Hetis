/**
 * @file Int128.h
 * @brief 128-bit integer aliases over the compiler's `__int128` extension.
 *
 * Exposes @ref Mashiro::uint128_t and @ref Mashiro::int128_t as project-wide
 * names for the toolchain's native 128-bit integer types. These are **not**
 * ISO C++ types; they are a compiler extension (`__int128` / `__uint128_t`)
 * that the build mandates — see the `HAVE_INT128` probe in
 * `cmake/ReflectionFeatureProbes.cmake`, which fails configuration loudly on a
 * toolchain that lacks them. Centralising the spelling here keeps the rest of
 * the codebase free of the leading-underscore extension names and gives a
 * single place to document the contract.
 *
 * The 128-bit hash tier (`Hashing::Fnv1a128`, `Hashing::Uuid`) relies on native
 * 128-bit arithmetic, as may any other wide-integer code; these aliases give it
 * a stable, extension-name-free spelling.
 *
 * @ingroup Core
 */
#pragma once

#include <climits>

namespace Mashiro {

    /// ReSharper disable once CppInconsistentNaming
    /// @brief Unsigned 128-bit integer (compiler extension `__uint128_t`).
    using uint128_t = __uint128_t;

    /// ReSharper disable once CppInconsistentNaming
    /// @brief Signed 128-bit integer (compiler extension `__int128`).
    using int128_t = __int128;

    static_assert(sizeof(uint128_t) == 16,
                  "Mashiro::uint128_t must be a 16-byte integer");
    static_assert(sizeof(int128_t) == 16,
                  "Mashiro::int128_t must be a 16-byte integer");
    static_assert(sizeof(uint128_t) * CHAR_BIT == 128,
                  "Mashiro::uint128_t must be exactly 128 bits wide");

}  // namespace Mashiro

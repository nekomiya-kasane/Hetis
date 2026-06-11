/**
 * @file Hash.h
 * @brief Reflection-driven, annotation-aware constexpr hash framework.
 *
 * Provides a unified hash infrastructure with:
 * - **Stateless algorithms** (one-shot, zero state, suitable for consteval).
 * - **Stateful hashers** (incremental feed/finalize, cache-friendly streaming).
 * - **Annotations** (`[[=Anno::With<Algo>{}]]`, `[[=Anno::Ignore{}]]`,
 *   `[[=Anno::Key{}]]`, `[[=Anno::Order{N}]]`) for per-type / per-member control.
 * - **CPO** `Hashing::Hash(value)` that auto-dispatches via ADL → annotation →
 *   reflection → trivial byte-hash fallback.
 * - **Automatic `std::hash<T>`** injection for all Hashable types.
 *
 * All paths are fully `constexpr`. Compile-time folding is guaranteed when inputs
 * are constant expressions.
 *
 * @ingroup Core
 */
#pragma once

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <meta>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include "Mashiro/Core/TypeTraits.h"

namespace Mashiro::Hashing {

    // =========================================================================
    // Concepts (declared early so Anno::With can constrain its parameter)
    // =========================================================================

    /// @name Algorithm concepts
    /// @{

    /// @brief Stateless functor-style algorithm: empty callable, byte span → unsigned.
    template <typename F>
    concept StatelessAlgo = requires(const F& func, std::span<const std::byte> data) {
        { func(data) } noexcept -> std::unsigned_integral;
    } && std::is_empty_v<F> && std::is_trivially_copyable_v<F>;

    /// @brief Stateful incremental hasher with Feed/Finalize/Seed protocol.
    template <typename H>
    concept StatefulAlgo = requires(H h, std::span<const std::byte> data) {
        typename H::ResultType;
        requires std::unsigned_integral<typename H::ResultType>;
        { H::Seed() } noexcept -> std::same_as<H>;
        { h.Feed(data) } noexcept;
        { h.Finalize() } noexcept -> std::same_as<typename H::ResultType>;
    } && std::is_trivially_copyable_v<H>;

    /// @brief Either stateless or stateful algorithm.
    template <typename T>
    concept AnyAlgo = StatelessAlgo<T> || StatefulAlgo<T>;

    /// @brief Result type of an algorithm.
    namespace Detail {
        template <typename A, bool IsStateless> struct ResultOfImpl;
        template <typename A> struct ResultOfImpl<A, true> {
            using type = decltype(std::declval<const A&>()(std::span<const std::byte>{}));
        };
        template <typename A> struct ResultOfImpl<A, false> {
            using type = typename A::ResultType;
        };
    }
    template <AnyAlgo A>
    using ResultOf = typename Detail::ResultOfImpl<A, StatelessAlgo<A>>::type;

    /// @brief Result size in bits.
    template <AnyAlgo A>
    inline constexpr size_t kResultBits = sizeof(ResultOf<A>) * 8;

    /// @}

    // =========================================================================
    // Annotations (P3385) — isolated in namespace Anno
    // =========================================================================

    /**
     * @brief Annotation types for controlling hash behaviour.
     *
     * All annotation types live in this sub-namespace to clearly distinguish
     * them from regular types. Usage:
     * @code
     * struct [[=Anno::With<Fnv1a32>{}]] PackedVertex {
     *     [[=Anno::Order{0}]] vec3 position;
     *     [[=Anno::Order{1}]] vec3 normal;
     *     [[=Anno::Ignore{}]] float lodBias;
     * };
     * @endcode
     */
    namespace Anno {

        /// @brief Exclude a member from hash computation.
        struct Ignore {
            constexpr bool operator==(const Ignore&) const = default;
        };

        /// @brief Whitelist mode: when any member carries this, only annotated members participate.
        struct Key {
            constexpr bool operator==(const Key&) const = default;
        };

        /**
         * @brief Control member hash order (ascending priority).
         *
         * Members without this annotation sort after those with it
         * (priority = INT_MAX). Among annotated members, lower priority
         * values are hashed first.
         */
        struct Order {
            int priority = 0;
            constexpr bool operator==(const Order&) const = default;
        };

        /**
         * @brief Specify the hash algorithm for a type or member.
         *
         * The template parameter is the algorithm type (must satisfy
         * `AnyAlgo`). At hash time the CPO extracts the algorithm type
         * via `template_of` / `template_arguments_of` reflection and
         * dispatches to it.
         *
         * @tparam A Algorithm type (e.g. `Fnv1a32`, `Murmur64`).
         */
        template <AnyAlgo A>
        struct With {
            constexpr bool operator==(const With&) const = default;
        };

    } // namespace Anno

    // =========================================================================
    // Stateless Algorithms
    // =========================================================================

    /// @name Stateless algorithms
    /// @{

    /// @brief FNV-1a 64-bit (stateless, one-shot).
    struct Fnv1a64 {
        using ResultType = uint64_t;
        static constexpr uint64_t kOffset = 14695981039346656037ULL;
        static constexpr uint64_t kPrime  = 1099511628211ULL;

        [[nodiscard]] constexpr uint64_t operator()(
            std::span<const std::byte> data) const noexcept {
            uint64_t h = kOffset;
            for (std::byte b : data) {
                h ^= static_cast<uint64_t>(b);
                h *= kPrime;
            }
            return h;
        }
    };

    /// @brief FNV-1a 32-bit (stateless, one-shot).
    struct Fnv1a32 {
        using ResultType = uint32_t;
        static constexpr uint32_t kOffset = 2166136261U;
        static constexpr uint32_t kPrime  = 16777619U;

        [[nodiscard]] constexpr uint32_t operator()(
            std::span<const std::byte> data) const noexcept {
            uint32_t h = kOffset;
            for (std::byte b : data) {
                h ^= static_cast<uint32_t>(b);
                h *= kPrime;
            }
            return h;
        }
    };

    /// @brief FNV-1a 64-bit + Murmur3 fmix64 avalanche finalizer.
    struct Murmur64 {
        using ResultType = uint64_t;

        [[nodiscard]] constexpr uint64_t operator()(
            std::span<const std::byte> data) const noexcept {
            uint64_t h = Fnv1a64{}(data);
            h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
            h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
            h ^= h >> 33;
            return h;
        }
    };

    /// @brief Default algorithm used when none is specified.
    using DefaultAlgo = Fnv1a64;

    /// @}

    // =========================================================================
    // Stateful Hashers
    // =========================================================================

    /// @name Stateful hashers
    /// @{

    /// @brief Stateful FNV-1a 64-bit (incremental feed).
    struct Fnv1a64State {
        using ResultType = uint64_t;
        uint64_t state = Fnv1a64::kOffset;

        [[nodiscard]] static constexpr Fnv1a64State Seed() noexcept { return {}; }

        constexpr void Feed(std::span<const std::byte> chunk) noexcept {
            for (std::byte b : chunk) {
                state ^= static_cast<uint64_t>(b);
                state *= Fnv1a64::kPrime;
            }
        }

        [[nodiscard]] constexpr uint64_t Finalize() const noexcept { return state; }
    };

    /// @brief Stateful FNV-1a 32-bit (incremental feed).
    struct Fnv1a32State {
        using ResultType = uint32_t;
        uint32_t state = Fnv1a32::kOffset;

        [[nodiscard]] static constexpr Fnv1a32State Seed() noexcept { return {}; }

        constexpr void Feed(std::span<const std::byte> chunk) noexcept {
            for (std::byte b : chunk) {
                state ^= static_cast<uint32_t>(b);
                state *= Fnv1a32::kPrime;
            }
        }

        [[nodiscard]] constexpr uint32_t Finalize() const noexcept { return state; }
    };

    /// @brief Stateful Murmur64 (FNV-1a 64 feed + fmix64 on finalize).
    struct Murmur64State {
        using ResultType = uint64_t;
        Fnv1a64State inner{};

        [[nodiscard]] static constexpr Murmur64State Seed() noexcept { return {}; }

        constexpr void Feed(std::span<const std::byte> chunk) noexcept {
            inner.Feed(chunk);
        }

        [[nodiscard]] constexpr uint64_t Finalize() const noexcept {
            uint64_t h = inner.Finalize();
            h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
            h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
            h ^= h >> 33;
            return h;
        }
    };

    /// @}

    // =========================================================================
    // Combine
    // =========================================================================

    /// @brief Golden-ratio hash combine, auto-selects constants by bit width.
    template <std::unsigned_integral U>
    [[nodiscard]] constexpr U Combine(U seed, U value) noexcept {
        if constexpr (sizeof(U) >= 8) {
            constexpr U k = 0x9e3779b97f4a7c15ULL;
            return seed ^ (value + k + (seed << 12) + (seed >> 4));
        } else if constexpr (sizeof(U) >= 4) {
            constexpr U k = 0x9e3779b9U;
            return seed ^ (value + k + (seed << 6) + (seed >> 2));
        } else {
            constexpr U k = 0x9e37;
            return seed ^ (value + k + (seed << 3) + (seed >> 1));
        }
    }

    // =========================================================================
    // Internal helpers
    // =========================================================================

    /** @cond INTERNAL */
    namespace Detail {

        /// @brief Invoke a stateless or stateful algo on a byte span.
        template <AnyAlgo A>
        [[nodiscard]] constexpr ResultOf<A> InvokeAlgo(const A& algo, std::span<const std::byte> data) noexcept {
            if constexpr (StatelessAlgo<A>) {
                return algo(data);
            } else {
                auto state = A::Seed();
                state.Feed(data);
                return state.Finalize();
            }
        }

        /// @brief Constexpr hash of a string_view (char-by-char, avoids byte cast).
        template <StatelessAlgo A>
        [[nodiscard]] constexpr ResultOf<A> HashString(const A&, std::string_view sv) noexcept {
            using R = ResultOf<A>;
            if constexpr (std::same_as<A, Fnv1a64>) {
                uint64_t h = Fnv1a64::kOffset;
                for (char c : sv) { h ^= static_cast<uint64_t>(static_cast<unsigned char>(c)); h *= Fnv1a64::kPrime; }
                return h;
            } else if constexpr (std::same_as<A, Fnv1a32>) {
                uint32_t h = Fnv1a32::kOffset;
                for (char c : sv) { h ^= static_cast<uint32_t>(static_cast<unsigned char>(c)); h *= Fnv1a32::kPrime; }
                return h;
            } else {
                uint64_t h = Fnv1a64::kOffset;
                for (char c : sv) { h ^= static_cast<uint64_t>(static_cast<unsigned char>(c)); h *= Fnv1a64::kPrime; }
                if constexpr (std::same_as<A, Murmur64>) {
                    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
                    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
                    h ^= h >> 33;
                }
                return static_cast<R>(h);
            }
        }

        /// @brief Stateful string feed (constexpr-safe, char-by-char).
        template <StatefulAlgo S>
        constexpr void FeedString(S& state, std::string_view sv) noexcept {
            for (char c : sv) {
                std::byte b = static_cast<std::byte>(static_cast<unsigned char>(c));
                state.Feed(std::span<const std::byte>(&b, 1));
            }
        }

        /// @brief Check if an entity has a specific annotation type.
        ///
        /// Thin alias for @ref Mashiro::Traits::Anno::Has — kept locally so
        /// existing call sites read naturally.
        template <typename Ann>
        consteval bool HasAnnotation(std::meta::info entity) {
            return ::Mashiro::Traits::Anno::Has<Ann>(entity);
        }

        /// @brief Extract annotation value of type Ann from entity, or `nullopt`.
        template <typename Ann>
        consteval std::optional<Ann> GetAnnotation(std::meta::info entity) {
            return ::Mashiro::Traits::Anno::Get<Ann>(entity);
        }

        /// @brief Check if entity has an Anno::With<X> annotation (any specialisation).
        consteval bool HasWithAnnotation(std::meta::info entity) {
            for (auto a : std::meta::annotations_of(entity)) {
                auto t = type_of(a);
                if (std::meta::has_template_arguments(t) && std::meta::template_of(t) == ^^Anno::With)
                    return true;
            }
            return false;
        }

        /// @brief Extract the algorithm type from Anno::With<A>, or return defaultAlgo.
        consteval std::meta::info GetAlgoFor(std::meta::info entity, std::meta::info defaultAlgo) {
            for (auto a : std::meta::annotations_of(entity)) {
                auto t = type_of(a);
                if (std::meta::has_template_arguments(t) && std::meta::template_of(t) == ^^Anno::With)
                    return std::meta::template_arguments_of(t)[0];
            }
            return defaultAlgo;
        }

        /**
         * @brief Get filtered and sorted non-static data members of T for hashing.
         *
         * Applies:
         * 1. `[[=Anno::Ignore{}]]` — member excluded.
         * 2. `[[=Anno::Key{}]]` whitelist — if any member has it, only those participate.
         * 3. `[[=Anno::Order{.priority=N}]]` — stable sort by ascending priority.
         *
         * Implemented in terms of @ref Mashiro::Traits::Anno::SelectMembers, the
         * subsystem-agnostic selector — the three policy tags are passed in as
         * template arguments.
         */
        template <typename T>
        consteval auto HashableMembers() {
            return ::Mashiro::Traits::Anno::SelectMembers<T, Anno::Ignore, Anno::Key, Anno::Order>();
        }

    } // namespace Detail
    /** @endcond */

    // =========================================================================
    // Hashable concept
    // =========================================================================

    /// @brief A type that can be hashed by algorithm A.
    template <typename T, typename A = DefaultAlgo>
    concept Hashable = AnyAlgo<A> && (
        requires(const A& algo, const T& v) {
            { HashValue(algo, v) } -> std::same_as<ResultOf<A>>;
        } ||
        std::convertible_to<T, std::string_view> ||
        std::convertible_to<T, std::span<const std::byte>> ||
        std::is_arithmetic_v<T> || std::is_enum_v<T> ||
        (std::is_class_v<T> && !std::is_union_v<T> && requires { sizeof(T); }) ||
        std::is_trivially_copyable_v<T>
    );

    // =========================================================================
    // CPO: Hashing::Hash
    // =========================================================================

    /// @brief Customisation-point object for hashing any Hashable value.
    inline constexpr struct HashCPO {

        /// @brief Hash a value using algorithm @p algo (default: Fnv1a64).
        template <AnyAlgo A = DefaultAlgo, typename T>
            requires Hashable<T, A>
        [[nodiscard]] constexpr ResultOf<A> operator()(const T& value, const A& algo = {}) const noexcept {
            return Dispatch<A>(value, algo);
        }

    private:
        template <AnyAlgo A, typename T>
        [[nodiscard]] constexpr ResultOf<A> Dispatch(const T& value, const A& algo) const noexcept {
            using R = ResultOf<A>;

            // 1. ADL hook
            if constexpr (requires { { HashValue(algo, value) } -> std::same_as<R>; }) {
                return HashValue(algo, value);
            }
            // 2. string-like
            else if constexpr (std::convertible_to<T, std::string_view>) {
                auto sv = std::string_view(value);
                if constexpr (StatelessAlgo<A>) {
                    return Detail::HashString(algo, sv);
                } else {
                    auto state = A::Seed();
                    Detail::FeedString(state, sv);
                    return state.Finalize();
                }
            }
            // 3. byte span
            else if constexpr (std::convertible_to<T, std::span<const std::byte>>) {
                return Detail::InvokeAlgo(algo, std::span<const std::byte>(value));
            }
            // 4. scalar / enum
            else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) {
                auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
                return Detail::InvokeAlgo(algo, std::span<const std::byte>(bytes));
            }
            // 5. class → reflection-driven recursive structured hash
            //    Respects [[=Anno::With<X>{}]] on the type to override algorithm.
            else if constexpr (std::is_class_v<T> && !std::is_union_v<T>) {
                // Check type-level Anno::With<X> annotation
                constexpr auto effectiveAlgoInfo = Detail::GetAlgoFor(^^T, ^^A);
                using EffectiveAlgo = typename [:effectiveAlgoInfo:];

                // Ensure result types are compatible (same width)
                if constexpr (sizeof(ResultOf<EffectiveAlgo>) == sizeof(R)) {
                    EffectiveAlgo effAlgo{};
                    ResultOf<EffectiveAlgo> seed{};
                    template for (constexpr auto m : std::define_static_array(Detail::HashableMembers<T>())) {
                        auto memberHash = Dispatch<EffectiveAlgo>(value.[:m:], effAlgo);
                        seed = Combine(seed, memberHash);
                    }
                    return static_cast<R>(seed);
                } else {
                    // Width mismatch: fall back to caller's algo
                    R seed{};
                    template for (constexpr auto m : std::define_static_array(Detail::HashableMembers<T>())) {
                        R memberHash = Dispatch<A>(value.[:m:], algo);
                        seed = Combine(seed, memberHash);
                    }
                    return seed;
                }
            }
            // 6. trivially copyable fallback
            else if constexpr (std::is_trivially_copyable_v<T>) {
                auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
                return Detail::InvokeAlgo(algo, std::span<const std::byte>(bytes));
            }
            else {
                static_assert(false, "Type is not Hashable");
            }
        }

    } Hash;

    // =========================================================================
    // Streaming Hasher
    // =========================================================================

    /**
     * @brief Stateful incremental hasher for streaming / large objects.
     *
     * @code
     * Hashing::Hasher h;
     * h.Feed(position).Feed(rotation).Feed(scale);
     * auto result = h.Finalize();
     * @endcode
     */
    template <StatefulAlgo State = Fnv1a64State>
    struct Hasher {
        using ResultType = typename State::ResultType;

        State state = State::Seed();

        /// @brief Feed a single Hashable value (recursively expanded).
        template <typename T>
        constexpr Hasher& Feed(const T& value) noexcept {
            FeedImpl(value);
            return *this;
        }

        /// @brief Feed raw bytes directly.
        constexpr Hasher& FeedBytes(std::span<const std::byte> data) noexcept {
            state.Feed(data);
            return *this;
        }

        /// @brief Finalize and return the hash value.
        [[nodiscard]] constexpr ResultType Finalize() const noexcept {
            return state.Finalize();
        }

        /// @brief Convenience: feed all values and finalize in one call.
        template <typename... Ts>
        [[nodiscard]] static constexpr ResultType Of(const Ts&... values) noexcept {
            Hasher h;
            (h.FeedImpl(values), ...);
            return h.Finalize();
        }

    private:
        template <typename T>
        constexpr void FeedImpl(const T& value) noexcept {
            if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) {
                auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
                state.Feed(bytes);
            }
            else if constexpr (std::convertible_to<T, std::string_view>) {
                auto sv = std::string_view(value);
                Detail::FeedString(state, sv);
            }
            else if constexpr (std::is_class_v<T> && !std::is_union_v<T>) {
                template for (constexpr auto m : std::define_static_array(Detail::HashableMembers<T>())) {
                    FeedImpl(value.[:m:]);
                }
            }
            else if constexpr (std::is_trivially_copyable_v<T>) {
                auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
                state.Feed(bytes);
            }
        }
    };

    // =========================================================================
    // User-defined literals
    // =========================================================================

    namespace Literals {

        /// @brief Compile-time 64-bit string hash: `"hello"_hash`.
        [[nodiscard]] consteval uint64_t operator""_hash(const char* str, size_t len) noexcept {
            return Detail::HashString(Fnv1a64{}, std::string_view{str, len});
        }

        /// @brief Compile-time 32-bit string hash: `"hello"_hash32`.
        [[nodiscard]] consteval uint32_t operator""_hash32(const char* str, size_t len) noexcept {
            return Detail::HashString(Fnv1a32{}, std::string_view{str, len});
        }

    } // namespace Literals

} // namespace Mashiro::Hashing

// =============================================================================
// std::hash automatic injection
// =============================================================================

/// @brief Auto-specialise `std::hash` for any type satisfying `Mashiro::Hashing::Hashable`.
template <typename T>
    requires (Mashiro::Hashing::Hashable<T> &&
              !std::is_arithmetic_v<T> &&
              !std::convertible_to<T, std::string_view>)
struct std::hash<T> {
    [[nodiscard]] constexpr size_t operator()(const T& value) const noexcept {
        if constexpr (sizeof(size_t) == 8) {
            return static_cast<size_t>(Mashiro::Hashing::Hash(value));
        } else {
            return static_cast<size_t>(
                Mashiro::Hashing::Hash(value, Mashiro::Hashing::Fnv1a32{}));
        }
    }
};

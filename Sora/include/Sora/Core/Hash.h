/**
 * @file Hash.h
 * @brief Reflection-driven, annotation-aware constexpr hash framework.
 *
 * Provides a unified hash infrastructure with:
 * - @c StatelessAlgo algorithms (one-shot, zero state, suitable for consteval).
 * - @c StatefulAlgo hashers (incremental feed/finalize, cache-friendly streaming).
 * - @c $ annotations (@c [[=$::With<Algo>{}]], @c [[=$::Ignore{}]],
 * @c [[=$::Key{}]], @c [[=$::Order{N}]]) for per-type / per-member control.
 * - CPO @c Hashing::Hash(value) that auto-dispatches via ADL -> annotation ->
 * reflection -> trivial byte-hash fallback.
 * - Automatic @c std::hash<T> injection for all Hashable types.
 *
 * All paths are fully @c constexpr. Compile-time folding is guaranteed when inputs are constant expressions.
 *
 * @ingroup Core
 */
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <meta>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "Sora/Core/Int128.h"
#include "Sora/Core/StringUtils.h"
#include "Sora/Core/Traits/TypeTraits.h"
#include "Sora/Core/Traits/AnnotationTraits.h"

namespace Sora::Hashing {

    // =========================================================================
    // Uuid: RFC-4122 universally unique identifier (a 128-bit semantic value)
    // =========================================================================

    /**
     * @brief Thrown by @ref Uuid::Parse on a malformed RFC-4122 string. As the parser is @c constexpr, reaching a
     * throw during constant evaluation (e.g. a bad @c _uuid literal) is a hard compile error.
     */
    class UuidParseError : public std::invalid_argument {
    public:
        using std::invalid_argument::invalid_argument;
    };

    /**
     * @brief Why an RFC-4122 string failed to parse.
     */
    enum class UuidParseErrc : uint8_t {
        Ok = 0,            /**< Parsed successfully. */
        Empty,             /**< Input (after stripping wrappers) was empty. */
        BadLength,         /**< Body was not the canonical 36 characters. */
        MissingHyphen,     /**< Expected @c - at a group boundary (pos 8/13/18/23). */
        InvalidHexDigit,   /**< A non-hex character appeared in a hex group. */
        UnterminatedBrace, /**< A leading @c { had no matching trailing @c }. */
    };

    /**
     * @brief Static human-readable description for a @ref UuidParseErrc.
     */
    [[nodiscard]] constexpr std::string_view UuidParseErrcMessage(UuidParseErrc e) noexcept {
        switch (e) {
            case UuidParseErrc::Ok:
                return "ok";
            case UuidParseErrc::Empty:
                return "empty UUID string";
            case UuidParseErrc::BadLength:
                return "UUID body must be 36 characters (8-4-4-4-12)";
            case UuidParseErrc::MissingHyphen:
                return "expected '-' at group boundary";
            case UuidParseErrc::InvalidHexDigit:
                return "invalid hexadecimal digit";
            case UuidParseErrc::UnterminatedBrace:
                return "missing closing '}'";
        }
        return "unknown UUID parse error";
    }

    /**
     * @brief An RFC-4122 universally unique identifier; a distinct 128-bit type.
     *
     * Wraps a single @ref Sora::uint128_t held big-endian (byte 0 is the most significant), so the canonical
     * 8-4-4-4-12 string reads straight off the value. It is a structural, trivially-copyable type with total ordering.
     *
     * @c Uuid carries *identity* semantics and is deliberately separate from the raw hash result of @ref Fnv1a128 (a
     * plain @c uint128_t): a hash is a number, a UUID is a name. Convert explicitly, for example @c Uuid{"x"_hash128},
     * when you intend to derive a name-style identifier from a digest.
     */
    struct Uuid {
        uint128_t value = 0; /**< Big-endian 128-bit value (byte 0 = most significant). */

        constexpr Uuid() noexcept = default;
        explicit constexpr Uuid(uint128_t v) noexcept : value(v) {}
        /**
         * @brief Compose from most/least-significant 64-bit halves (big-endian).
         */
        constexpr Uuid(uint64_t hi, uint64_t lo) noexcept
            : value((static_cast<uint128_t>(hi) << 64) | static_cast<uint128_t>(lo)) {}

        /**
         * @brief Compose from RFC-4122 fields: 8-4-4 and trailing 8 bytes (clock_seq + node).
         */
        constexpr Uuid(uint32_t timeLow, uint16_t timeMid, uint16_t timeHiAndVersion,
                       std::array<uint8_t, 8> tail) noexcept
            : value((static_cast<uint128_t>(timeLow) << 96) | (static_cast<uint128_t>(timeMid) << 80) |
                    (static_cast<uint128_t>(timeHiAndVersion) << 64) | (static_cast<uint128_t>(tail[0]) << 56) |
                    (static_cast<uint128_t>(tail[1]) << 48) | (static_cast<uint128_t>(tail[2]) << 40) |
                    (static_cast<uint128_t>(tail[3]) << 32) | (static_cast<uint128_t>(tail[4]) << 24) |
                    (static_cast<uint128_t>(tail[5]) << 16) | (static_cast<uint128_t>(tail[6]) << 8) |
                    static_cast<uint128_t>(tail[7])) {}

        constexpr bool operator==(const Uuid&) const = default;
        constexpr auto operator<=>(const Uuid&) const = default;

        /**
         * @brief The underlying 128-bit value.
         */
        [[nodiscard]] constexpr uint128_t ToUint128() const noexcept { return value; }
        /**
         * @brief Mutable reference to the underlying 128-bit value.
         */
        [[nodiscard]] constexpr uint128_t& AsUint128() noexcept { return value; }
        /**
         * @brief Const reference to the underlying 128-bit value.
         */
        [[nodiscard]] constexpr const uint128_t& AsUint128() const noexcept { return value; }
        /**
         * @brief Wrap a native 128-bit integer.
         */
        [[nodiscard]] static constexpr Uuid FromUint128(uint128_t v) noexcept { return Uuid{v}; }

        /**
         * @brief The all-zero (nil) UUID.
         */
        [[nodiscard]] static constexpr Uuid Nil() noexcept { return {}; }

        /**
         * @brief @c true if this is the nil UUID.
         */
        [[nodiscard]] constexpr bool IsNil() const noexcept { return value == 0; }

        /**
         * @brief The byte at index @p i in big-endian order (0 = most significant).
         */
        [[nodiscard]] constexpr uint8_t Byte(size_t i) const noexcept {
            return static_cast<uint8_t>(value >> ((15 - (i & 15)) * 8));
        }

        /**
         * @brief RFC-4122 version nibble (bits 12..15 of the time_hi field).
         */
        [[nodiscard]] constexpr uint8_t Version() const noexcept { return static_cast<uint8_t>((value >> 76) & 0xF); }

        /**
         * @brief RFC-4122 variant bits (top bits of the clock_seq_hi byte).
         */
        [[nodiscard]] constexpr uint8_t Variant() const noexcept { return static_cast<uint8_t>((value >> 62) & 0x3); }

        /**
         * @brief Stamp the RFC-4122 version and variant fields onto this digest.
         *
         * Overwrites the version nibble with @p version and sets the variant to the RFC-4122 form (10xx). The digest
         * is FNV-based rather than MD5/SHA-1, so the default version is 8 ("custom"). The remaining 122 bits are left
         * as the hash output.
         */
        [[nodiscard]] constexpr Uuid WithRfc4122(uint8_t version = 8) const noexcept {
            uint128_t v = value;
            v &= ~(static_cast<uint128_t>(0xF) << 76);
            v |= static_cast<uint128_t>(version & 0xF) << 76;
            v &= ~(static_cast<uint128_t>(0x3) << 62);
            v |= static_cast<uint128_t>(0x2) << 62;
            return Uuid{v};
        }

        /**
         * @brief Canonical 8-4-4-4-12 lowercase hex string.
         */
        [[nodiscard]] constexpr std::array<char, 36> ToChars() const noexcept {
            constexpr char kHex[] = "0123456789abcdef";
            std::array<char, 36> out{};
            size_t pos = 0;
            for (size_t i = 0; i < 16; ++i) {
                if (i == 4 || i == 6 || i == 8 || i == 10) {
                    out[pos++] = '-';
                }
                uint8_t b = Byte(i);
                out[pos++] = kHex[b >> 4];
                out[pos++] = kHex[b & 0xF];
            }
            return out;
        }

        /**
         * @brief Canonical 8-4-4-4-12 lowercase hex string as a @c std::string.
         */
        [[nodiscard]] std::string ToString() const {
            auto c = ToChars();
            return std::string(c.data(), c.size());
        }

        /**
         * @brief Parse strictly canonical 8-4-4-4-12 hex (no wrappers).
         *
         * The lowest-level parser: @p sv must be exactly the 36-character body. Returns the parsed value or the
         * specific @ref UuidParseErrc.
         */
        [[nodiscard]] static constexpr std::expected<Uuid, UuidParseErrc> ParseCanonical(std::string_view sv) noexcept {
            if (sv.empty()) {
                return std::unexpected(UuidParseErrc::Empty);
            }
            if (sv.size() != 36) {
                return std::unexpected(UuidParseErrc::BadLength);
            }
            uint128_t v = 0;
            for (size_t i = 0; i < sv.size(); ++i) {
                if (i == 8 || i == 13 || i == 18 || i == 23) {
                    if (sv[i] != '-') {
                        return std::unexpected(UuidParseErrc::MissingHyphen);
                    }
                    continue;
                }
                int n = Sora::Ascii::HexValue(sv[i]);
                if (n < 0) {
                    return std::unexpected(UuidParseErrc::InvalidHexDigit);
                }
                v = (v << 4) | static_cast<uint128_t>(n);
            }
            return Uuid{v};
        }

        /**
         * @brief Parse an RFC-4122 string, tolerating a @c urn:uuid: prefix and/or surrounding @c { } braces, then a
         * strict canonical body.
         *
         * Both wrappers are optional and the @c urn:uuid: scheme is matched case-insensitively (RFC 4122 section 3).
         * Returns the value or the specific @ref UuidParseErrc. @c constexpr, so usable in constant evaluation and
         * reusable at runtime with no allocation.
         */
        [[nodiscard]] static constexpr auto Parse(std::string_view sv) noexcept -> std::expected<Uuid, UuidParseErrc> {
            if (sv.empty()) {
                return std::unexpected(UuidParseErrc::Empty);
            }

            // Optional "urn:uuid:" scheme prefix (case-insensitive).
            constexpr std::string_view kUrn = "urn:uuid:";
            if (sv.size() >= kUrn.size()) {
                bool match = true;
                for (size_t i = 0; i < kUrn.size(); ++i) {
                    if (Sora::Ascii::ToLower(sv[i]) != kUrn[i]) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    sv.remove_prefix(kUrn.size());
                }
            }

            // Optional surrounding braces.
            if (!sv.empty() && sv.front() == '{') {
                if (sv.back() != '}') {
                    return std::unexpected(UuidParseErrc::UnterminatedBrace);
                }
                sv.remove_prefix(1);
                sv.remove_suffix(1);
            }

            return ParseCanonical(sv);
        }

        /**
         * @brief Parse, throwing @ref UuidParseError on failure. Reusable at runtime; in constant evaluation a failure
         * is a compile error.
         */
        [[nodiscard]] static constexpr Uuid ParseOrThrow(std::string_view sv) {
            auto r = Parse(sv);
            if (!r) {
                throw UuidParseError(std::string(UuidParseErrcMessage(r.error())));
            }
            return *r;
        }

        /**
         * @brief Parse an RFC-4122 string (with optional wrappers), or @c nullopt.
         */
        [[nodiscard]] static constexpr std::optional<Uuid> FromString(std::string_view sv) noexcept {
            auto r = Parse(sv);
            return r ? std::optional<Uuid>(*r) : std::nullopt;
        }
    };
    static_assert(sizeof(Uuid) == 16);
    static_assert(std::is_trivially_copyable_v<Uuid>);

    // =========================================================================
    // Concepts (declared early so $::With can constrain its parameter)
    // =========================================================================

    /**
     * @name Algorithm concepts
     * @{
     */

    /**
     * @brief Stateless functor-style algorithm: empty callable, byte span -> unsigned.
     */
    template<typename F>
    concept StatelessAlgo = requires(const F& func, std::span<const std::byte> data) {
        { func(data) } noexcept -> std::unsigned_integral;
    } && std::is_empty_v<F> && std::is_trivially_copyable_v<F>;

    /**
     * @brief Stateful incremental hasher with Feed/Finalize/Seed protocol.
     */
    template<typename H>
    concept StatefulAlgo = requires(H h, std::span<const std::byte> data) {
        typename H::ResultType;
        requires std::unsigned_integral<typename H::ResultType>;
        { H::Seed() } noexcept -> std::same_as<H>;
        { h.Feed(data) } noexcept;
        { h.Finalize() } noexcept -> std::same_as<typename H::ResultType>;
    } && std::is_trivially_copyable_v<H>;

    /**
     * @brief Either stateless or stateful algorithm.
     */
    template<typename T>
    concept AnyAlgo = StatelessAlgo<T> || StatefulAlgo<T>;

    namespace Detail {

        template<typename A, bool IsStateless>
        struct ResultOfImpl;

        template<typename A>
        struct ResultOfImpl<A, true> {
            using type = decltype(std::declval<const A&>()(std::span<const std::byte>{}));
        };

        template<typename A>
        struct ResultOfImpl<A, false> {
            using type = typename A::ResultType;
        };

    } // namespace Detail

    /**
     * @brief Result type of an algorithm.
     */
    template<AnyAlgo A>
    using ResultOf = typename Detail::ResultOfImpl<A, StatelessAlgo<A>>::type;

    /**
     * @brief Result size in bits.
     */
    template<AnyAlgo A>
    inline constexpr size_t kResultBits = sizeof(ResultOf<A>) * 8;

    /**
     * @}
     */

    // =========================================================================
    // Annotations (P3385) ; isolated in namespace $
    // =========================================================================

    /**
     * @brief Annotation types for controlling hash behaviour.
     *
     * All annotation types live in this sub-namespace to clearly distinguish them from regular types. Usage:
     * @code{.cpp}
     * struct [[=$::With<Fnv1a32>{}]] PackedVertex {
     *     [[=$::Order{0}]] vec3 position;
     *     [[=$::Order{1}]] vec3 normal;
     *     [[=$::Ignore{}]] float lodBias;
     * };
     * @endcode
     */
    namespace $ {

        /**
         * @brief Exclude a member from hash computation.
         */
        struct Ignore {
            constexpr bool operator==(const Ignore&) const = default;
        };

        /**
         * @brief Whitelist mode: when any member carries this, only annotated members participate.
         */
        struct Key {
            constexpr bool operator==(const Key&) const = default;
        };

        /**
         * @brief Control member hash order (ascending priority).
         *
         * Members without this annotation sort after those with it (priority = INT_MAX). Among annotated members,
         * lower priority values are hashed first.
         */
        struct Order {
            int priority = 0;
            constexpr bool operator==(const Order&) const = default;
        };

        /**
         * @brief Specify the hash algorithm for a type or member.
         *
         * The template parameter is the algorithm type (must satisfy @ref AnyAlgo). At hash time the CPO extracts the
         * algorithm type via @c template_of / @c template_arguments_of reflection and dispatches to it.
         *
         * @tparam A Algorithm type, such as @ref Fnv1a32 or @ref Murmur64.
         */
        template<AnyAlgo A>
        struct With {
            constexpr bool operator==(const With&) const = default;
        };

    } // namespace $

} // namespace Sora::Hashing

namespace Sora::$::Hashing {

    using Ignore = Sora::Hashing::$::Ignore;
    using Key = Sora::Hashing::$::Key;
    using Order = Sora::Hashing::$::Order;

    template<Sora::Hashing::AnyAlgo A>
    using With = Sora::Hashing::$::With<A>;

} // namespace Sora::$::Hashing

namespace Sora::Hashing {

    // =========================================================================
    // Stateless Algorithms
    // =========================================================================

    /**
     * @name Stateless algorithms
     * @{
     */

    /**
     * @brief FNV-1a 32-bit (stateless, one-shot).
     */
    struct Fnv1a32 {
        using ResultType = uint32_t;
        static constexpr uint32_t kOffset = 2166136261U;
        static constexpr uint32_t kPrime = 16777619U;

        [[nodiscard]] constexpr ResultType operator()(std::span<const std::byte> data) const noexcept {
            ResultType h = kOffset;
            for (std::byte b : data) {
                h ^= static_cast<ResultType>(b);
                h *= kPrime;
            }
            return h;
        }
    };

    /**
     * @brief FNV-1a 64-bit (stateless, one-shot).
     */
    struct Fnv1a64 {
        using ResultType = uint64_t;
        static constexpr ResultType kOffset = 14695981039346656037ULL;
        static constexpr ResultType kPrime = 1099511628211ULL;

        [[nodiscard]] constexpr ResultType operator()(std::span<const std::byte> data) const noexcept {
            ResultType h = kOffset;
            for (std::byte b : data) {
                h ^= static_cast<ResultType>(b);
                h *= kPrime;
            }
            return h;
        }
    };

    /**
     * @brief FNV-1a 64-bit + Murmur3 fmix64 avalanche finalizer.
     */
    struct Murmur64 {
        using ResultType = uint64_t;

        [[nodiscard]] constexpr ResultType operator()(std::span<const std::byte> data) const noexcept {
            ResultType h = Fnv1a64{}(data);
            h ^= h >> 33;
            h *= 0xff51afd7ed558ccdULL;
            h ^= h >> 33;
            h *= 0xc4ceb9fe1a85ec53ULL;
            h ^= h >> 33;
            return h;
        }
    };

    /**
     * @brief Default algorithm used when none is specified.
     */
    using DefaultAlgo = Fnv1a64;

    /**
     * @brief FNV-1a 128-bit (stateless, one-shot). Result is a raw 128-bit hash.
     */
    struct Fnv1a128 {
        using ResultType = uint128_t;

        /**
         * @brief 128-bit FNV offset basis (0x6c62272e07bb0142'62b821756295c58d).
         */
        static constexpr ResultType kOffset =
            (static_cast<ResultType>(0x6c62272e07bb0142ULL) << 64) | 0x62b821756295c58dULL;
        /**
         * @brief 128-bit FNV prime (2^88 + 2^8 + 0x3b).
         */
        static constexpr ResultType kPrime =
            (static_cast<ResultType>(0x0000000001000000ULL) << 64) | 0x000000000000013bULL;

        [[nodiscard]] constexpr ResultType operator()(std::span<const std::byte> data) const noexcept {
            ResultType h = kOffset;
            for (std::byte b : data) {
                h ^= static_cast<ResultType>(static_cast<uint8_t>(b));
                h *= kPrime;
            }
            return h;
        }
    };

    /**
     * @}
     */

    // =========================================================================
    // Stateful Hashers
    // =========================================================================

    /**
     * @name Stateful hashers
     * @{
     */

    /**
     * @brief Stateful FNV-1a 32-bit (incremental feed).
     */
    struct Fnv1a32State {
        using ResultType = Fnv1a32::ResultType;
        ResultType state = Fnv1a32::kOffset;

        [[nodiscard]] static constexpr Fnv1a32State Seed() noexcept { return {}; }

        constexpr void FeedByte(std::byte byte) noexcept {
            state ^= static_cast<ResultType>(byte);
            state *= Fnv1a32::kPrime;
        }

        constexpr void Feed(std::span<const std::byte> chunk) noexcept {
            for (std::byte byte : chunk) {
                FeedByte(byte);
            }
        }

        [[nodiscard]] constexpr ResultType Finalize() const noexcept { return state; }
    };

    /**
     * @brief Stateful FNV-1a 64-bit (incremental feed).
     */
    struct Fnv1a64State {
        using ResultType = Fnv1a64::ResultType;
        ResultType state = Fnv1a64::kOffset;

        [[nodiscard]] static constexpr Fnv1a64State Seed() noexcept { return {}; }

        constexpr void FeedByte(std::byte byte) noexcept {
            state ^= static_cast<ResultType>(byte);
            state *= Fnv1a64::kPrime;
        }

        constexpr void Feed(std::span<const std::byte> chunk) noexcept {
            for (std::byte byte : chunk) {
                FeedByte(byte);
            }
        }

        [[nodiscard]] constexpr ResultType Finalize() const noexcept { return state; }
    };

    /**
     * @brief Stateful Murmur64 (FNV-1a 64 feed + fmix64 on finalize).
     */
    struct Murmur64State {
        using ResultType = Murmur64::ResultType;
        Fnv1a64State inner{};

        [[nodiscard]] static constexpr Murmur64State Seed() noexcept { return {}; }

        constexpr void FeedByte(std::byte byte) noexcept { inner.FeedByte(byte); }

        constexpr void Feed(std::span<const std::byte> chunk) noexcept { inner.Feed(chunk); }

        [[nodiscard]] constexpr ResultType Finalize() const noexcept {
            uint64_t h = inner.Finalize();
            h ^= h >> 33;
            h *= 0xff51afd7ed558ccdULL;
            h ^= h >> 33;
            h *= 0xc4ceb9fe1a85ec53ULL;
            h ^= h >> 33;
            return h;
        }
    };

    /**
     * @brief Stateful FNV-1a 128-bit (incremental feed). Result is a raw 128-bit hash.
     */
    struct Fnv1a128State {
        using ResultType = uint128_t;
        ResultType state = Fnv1a128::kOffset;

        [[nodiscard]] static constexpr Fnv1a128State Seed() noexcept { return {}; }

        constexpr void FeedByte(std::byte byte) noexcept {
            state ^= static_cast<ResultType>(static_cast<uint8_t>(byte));
            state *= Fnv1a128::kPrime;
        }

        constexpr void Feed(std::span<const std::byte> chunk) noexcept {
            for (std::byte byte : chunk) {
                FeedByte(byte);
            }
        }

        [[nodiscard]] constexpr ResultType Finalize() const noexcept { return state; }
    };

    /**
     * @}
     */

    // =========================================================================
    // Combine
    // =========================================================================

    /**
     * @brief Golden-ratio hash combine, auto-selects constants by bit width.
     */
    template<std::unsigned_integral U>
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

    /** @brief Hash a byte-like range with @p State, preserving constexpr legality for non-@c std::byte storage. */
    template<StatefulAlgo State = Fnv1a64State, std::ranges::input_range Range>
        requires Concept::ByteLike<std::remove_cvref_t<std::ranges::range_value_t<Range>>>
    [[nodiscard]] constexpr typename State::ResultType HashByteRange(const Range& bytes) noexcept {
        auto state = State::Seed();
        for (auto&& byte : bytes) {
            const std::byte b = [&] {
                using Byte = std::remove_cvref_t<decltype(byte)>;
                if constexpr (std::same_as<Byte, std::byte>) {
                    return byte;
                } else {
                    return std::byte{static_cast<unsigned char>(byte)};
                }
            }();
            if constexpr (requires { state.FeedByte(b); }) {
                state.FeedByte(b);
            } else {
                state.Feed(std::span<const std::byte>{&b, 1});
            }
        }
        return state.Finalize();
    }

    /** @brief Hash a byte-like range while treating @p zeroSize bytes from @p zeroOffset as zero. */
    template<StatefulAlgo State = Fnv1a64State, std::ranges::input_range Range>
        requires Concept::ByteLike<std::remove_cvref_t<std::ranges::range_value_t<Range>>> &&
                 std::ranges::sized_range<Range>
    [[nodiscard]] constexpr typename State::ResultType HashByteRangeWithZeroRange(const Range& bytes, size_t zeroOffset,
                                                                                  size_t zeroSize) noexcept {
        auto state = State::Seed();
        using Byte = std::remove_cvref_t<std::ranges::range_value_t<Range>>;
        const size_t size = static_cast<size_t>(std::ranges::size(bytes));
        const size_t boundedOffset = std::min(zeroOffset, size);
        const size_t boundedSize = std::min(zeroSize, size - boundedOffset);
        const size_t zeroEnd = boundedOffset + boundedSize;
        size_t i = 0;
        for (auto&& byte : bytes) {
            const bool zero = i >= boundedOffset && i < zeroEnd;
            const std::byte b = [&] {
                if (zero) {
                    return std::byte{0};
                }
                if constexpr (std::same_as<std::remove_cv_t<Byte>, std::byte>) {
                    return byte;
                } else {
                    return std::byte{static_cast<unsigned char>(byte)};
                }
            }();
            if constexpr (requires { state.FeedByte(b); }) {
                state.FeedByte(b);
            } else {
                state.Feed(std::span<const std::byte>{&b, 1});
            }
            ++i;
        }
        return state.Finalize();
    }

    // =========================================================================
    // Internal helpers
    // =========================================================================

    /** @cond INTERNAL */
    namespace Detail {

        /**
         * @brief Invoke a stateless or stateful algo on a byte span.
         */
        template<AnyAlgo A>
        [[nodiscard]] constexpr ResultOf<A> InvokeAlgo(const A& algo, std::span<const std::byte> data) noexcept {
            if constexpr (StatelessAlgo<A>) {
                return algo(data);
            } else {
                auto state = A::Seed();
                state.Feed(data);
                return state.Finalize();
            }
        }

        /**
         * @brief Constexpr hash of a string_view.
         *
         * Both paths delegate to the algorithm's own byte-span operator, so the result is identical for every algo
         * (Murmur64 included). At runtime the span aliases @p sv directly (zero copy). During constant evaluation @c
         * reinterpret_cast is unavailable, so the chars are first materialised into a byte buffer, the only
         * constexpr-legal way to obtain the span.
         */
        template<StatelessAlgo A>
        [[nodiscard]] constexpr ResultOf<A> HashString(const A& algo, std::string_view sv) noexcept {
            if consteval {
                auto bytes = sv | std::views::transform([](char c) {
                                 return static_cast<std::byte>(static_cast<unsigned char>(c));
                             }) |
                             std::ranges::to<std::vector>();
                return algo(std::span<const std::byte>{bytes.data(), bytes.size()});
            } else {
                return algo(std::span<const std::byte>{reinterpret_cast<const std::byte*>(sv.data()), sv.size()});
            }
        }

        /**
         * @brief Stateful string feed (constexpr-safe, char-by-char).
         */
        template<StatefulAlgo S>
        constexpr void FeedString(S& state, std::string_view sv) noexcept {
            for (char c : sv) {
                std::byte b = static_cast<std::byte>(static_cast<unsigned char>(c));
                state.Feed(std::span<const std::byte>(&b, 1));
            }
        }

        /**
         * @brief Extract the algorithm type from $::With<A>, or return defaultAlgo.
         */
        consteval std::meta::info GetAlgoFor(std::meta::info entity, std::meta::info defaultAlgo) {
            for (auto a : std::meta::annotations_of(entity)) {
                auto t = type_of(a);
                if (std::meta::has_template_arguments(t) && std::meta::template_of(t) == ^^$::With) {
                    return std::meta::template_arguments_of(t)[0];
                }
            }
            return defaultAlgo;
        }

        /**
         * @brief Get filtered and sorted non-static data members of T for hashing.
         *
         * Applies:
         * 1. @c [[=$::Ignore{}]] excludes the member.
         * 2. @c [[=$::Key{}]] enables whitelist mode; if any member has it, only those participate.
         * 3. @c [[=$::Order{.priority=N}]] defines stable sort by ascending priority.
         *
         * The selector starts from @ref Sora::Traits::DataMembers so only object fields can reach member splicing.
         */
        template<typename T>
            requires(std::is_class_v<T> && !std::is_union_v<T>)
        consteval auto GetHashableMembers() {
            std::vector<std::meta::info> result;
            bool hasKey = false;
            for (auto m : Traits::DataMembers<T>) {
                if (Sora::$::Has<$::Key>(m)) {
                    hasKey = true;
                    break;
                }
            }
            for (auto m : Traits::DataMembers<T>) {
                if (Sora::$::Has<$::Ignore>(m)) {
                    continue;
                }
                if (hasKey && !Sora::$::Has<$::Key>(m)) {
                    continue;
                }
                result.push_back(m);
            }
            constexpr int kSentinel = 0x7FFFFFFF;
            auto priorityOf = [](std::meta::info m) -> int {
                auto o = Sora::$::GetSingleOptional<$::Order>(m);
                return o ? o->priority : kSentinel;
            };
            for (size_t i = 1; i < result.size(); ++i) {
                auto key = result[i];
                int pk = priorityOf(key);
                size_t j = i;
                while (j > 0 && priorityOf(result[j - 1]) > pk) {
                    result[j] = result[j - 1];
                    --j;
                }
                result[j] = key;
            }
            return std::define_static_array(result);
        }

    } // namespace Detail
    /** @endcond */

    // =========================================================================
    // Hashable concept
    // =========================================================================

    /**
     * @brief A type that can be hashed by algorithm A.
     */
    template<typename T, typename A = DefaultAlgo>
    concept Hashable =
        AnyAlgo<A> &&
        (requires(const A& algo, const T& v) {
            { HashValue(algo, v) } -> std::same_as<ResultOf<A>>;
        } || std::convertible_to<T, std::string_view> || std::convertible_to<T, std::span<const std::byte>> ||
         std::is_arithmetic_v<T> || std::is_enum_v<T> ||
         (std::is_class_v<T> && !std::is_union_v<T> && requires { sizeof(T); }) || std::is_trivially_copyable_v<T>);

    // =========================================================================
    // CPO: Hashing::Hash
    // =========================================================================

    namespace HashCPOFn {

        struct HashCPO {

            /**
             * @brief Hash a value using algorithm @p algo (default: Fnv1a64).
             */
            template<AnyAlgo A = DefaultAlgo, typename T>
                requires Hashable<T, A>
            [[nodiscard]] constexpr ResultOf<A> operator()(const T& value, const A& algo = {}) const noexcept {
                return Dispatch<A>(value, algo);
            }

        private:
            template<AnyAlgo A, typename T>
            [[nodiscard]] constexpr ResultOf<A> Dispatch(const T& value, const A& algo) const noexcept {
                using R = ResultOf<A>;

                // 1. ADL hook
                if constexpr (requires {
                                  { HashValue(algo, value) } -> std::same_as<R>;
                              }) {
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
                // 5. class -> reflection-driven recursive structured hash
                //    Respects [[=$::With<X>{}]] on the type to override algorithm.
                else if constexpr (std::is_class_v<T> && !std::is_union_v<T>) {
                    // Check type-level $::With<X> annotation
                    constexpr auto effectiveAlgoInfo = Detail::GetAlgoFor(^^T, ^^A);
                    using EffectiveAlgo = typename [:effectiveAlgoInfo:];

                    // Ensure result types are compatible (same width)
                    if constexpr (sizeof(ResultOf<EffectiveAlgo>) == sizeof(R)) {
                        EffectiveAlgo effAlgo{};
                        ResultOf<EffectiveAlgo> seed{};
                        template for (constexpr auto m : Detail::GetHashableMembers<T>()) {
                            auto memberHash = Dispatch<EffectiveAlgo>(value.[:m:], effAlgo);
                            seed = Combine(seed, memberHash);
                        }
                        return static_cast<R>(seed);
                    } else {
                        // Width mismatch: fall back to caller's algo
                        R seed{};
                        template for (constexpr auto m : Detail::GetHashableMembers<T>()) {
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
                } else {
                    static_assert(false, "Type is not Hashable");
                }
            }
        };

    } // namespace HashCPOFn

    /**
     * @brief Customisation-point object for hashing any Hashable value.
     */
    inline constexpr HashCPOFn::HashCPO Hash;

    // =========================================================================
    // Streaming Hasher
    // =========================================================================

    /**
     * @brief Stateful incremental hasher for streaming / large objects.
     *
     * @code{.cpp}
     * Hashing::Hasher h;
     * h.Feed(position).Feed(rotation).Feed(scale);
     * auto result = h.Finalize();
     * @endcode
     */
    template<StatefulAlgo State = Fnv1a64State>
    struct Hasher {
        using ResultType = typename State::ResultType;

        State state = State::Seed();

        /**
         * @brief Feed a single Hashable value (recursively expanded).
         */
        template<typename T>
        constexpr Hasher& Feed(const T& value) noexcept {
            FeedImpl(value);
            return *this;
        }

        /**
         * @brief Feed raw bytes directly.
         */
        constexpr Hasher& FeedBytes(std::span<const std::byte> data) noexcept {
            state.Feed(data);
            return *this;
        }

        /**
         * @brief Finalize and return the hash value.
         */
        [[nodiscard]] constexpr ResultType Finalize() const noexcept { return state.Finalize(); }

        /**
         * @brief Convenience: feed all values and finalize in one call.
         */
        template<typename... Ts>
        [[nodiscard]] static constexpr ResultType Of(const Ts&... values) noexcept {
            Hasher h;
            (h.FeedImpl(values), ...);
            return h.Finalize();
        }

    private:
        template<typename T>
        constexpr void FeedImpl(const T& value) noexcept {
            if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) {
                auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
                state.Feed(bytes);
            } else if constexpr (std::convertible_to<T, std::string_view>) {
                auto sv = std::string_view(value);
                Detail::FeedString(state, sv);
            } else if constexpr (std::is_class_v<T> && !std::is_union_v<T>) {
                template for (constexpr auto m : Detail::GetHashableMembers<T>()) {
                    FeedImpl(value.[:m:]);
                }
            } else if constexpr (std::is_trivially_copyable_v<T>) {
                auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
                state.Feed(bytes);
            }
        }
    };

    // =========================================================================
    // User-defined literals
    // =========================================================================

    namespace Literals {

        /**
         * @brief Compile-time 32-bit string hash: @c "hello"_hash32.
         */
        [[nodiscard]] consteval uint32_t operator""_hash32(const char* str, size_t len) noexcept {
            return Detail::HashString(Fnv1a32{}, std::string_view{str, len});
        }

        /**
         * @brief Compile-time 64-bit string hash: @c "hello"_hash.
         */
        [[nodiscard]] consteval uint64_t operator""_hash(const char* str, size_t len) noexcept {
            return Detail::HashString(Fnv1a64{}, std::string_view{str, len});
        }

        /**
         * @brief Compile-time 128-bit string hash digest: @c "hello"_hash128.
         */
        [[nodiscard]] consteval uint128_t operator""_hash128(const char* str, size_t len) noexcept {
            return Detail::HashString(Fnv1a128{}, std::string_view{str, len});
        }

        /**
         * @brief Parse an RFC-4122 literal into a @ref Uuid: @c "..."_uuid.
         *
         * Accepts the canonical 8-4-4-4-12 form with an optional @c urn:uuid: prefix and/or surrounding @c { }. A
         * malformed literal makes @ref Uuid::ParseOrThrow throw inside this @c consteval, which the standard turns
         * into a compile error pinpointing the bad literal. The same parser is @c constexpr, so it is equally usable
         * at runtime.
         */
        [[nodiscard]] consteval Uuid operator""_uuid(const char* str, size_t len) {
            return Uuid::ParseOrThrow(std::string_view{str, len});
        }

    } // namespace Literals

} // namespace Sora::Hashing

namespace Sora {

    using Hashing::Uuid;

    namespace $ {

        namespace Hashing {

            using Ignore = Sora::Hashing::$::Ignore;
            using Key = Sora::Hashing::$::Key;
            using Order = Sora::Hashing::$::Order;
            template<Sora::Hashing::AnyAlgo A>
            using With = Sora::Hashing::$::With<A>;

        } // namespace Hashing

    } // namespace $

    namespace Concept {

        template<typename T, typename A = Hashing::DefaultAlgo>
        concept HashableClass = Hashing::Hashable<T, A>;

    }

} // namespace Sora

// =============================================================================
// std::hash automatic injection
// =============================================================================

/**
 * @brief Auto-specialise @c std::hash for any type satisfying @c Sora::Hashing::Hashable.
 */
template<typename T>
    requires(Sora::Hashing::Hashable<T> && !std::is_arithmetic_v<T> && !std::convertible_to<T, std::string_view>)
struct std::hash<T> {
    [[nodiscard]] constexpr size_t operator()(const T& value) const noexcept {
        if constexpr (sizeof(size_t) == 8) {
            return static_cast<size_t>(Sora::Hashing::Hash(value));
        } else {
            return static_cast<size_t>(Sora::Hashing::Hash(value, Sora::Hashing::Fnv1a32{}));
        }
    }
};

// =============================================================================
// std::formatter: canonical 8-4-4-4-12 rendering of a Uuid
// =============================================================================

/**
 * @brief Format a @c Uuid in canonical RFC-4122 string form.
 * @todo support format_context arguments
 */
template<>
struct std::formatter<Sora::Uuid> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    auto format(const Sora::Uuid& id, std::format_context& ctx) const {
        auto chars = id.ToChars();
        return std::copy(chars.begin(), chars.end(), ctx.out());
    }
};

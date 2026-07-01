/**
 * @file IID.h
 * @brief Stable IID values, reflected IID synthesis, and annotation payloads for object-model edges.
 * @ingroup Core
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <meta>
#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>

#include <Mashiro/Core/Hash.h>

namespace Yuki {

    /**
     * @brief IID
     * @{
     */

    /** @brief A 128-bit stable interface/class identifier used by the Yuki object model. */
    struct Iid {
        uint64_t hi{}; /**< Most-significant 64 bits in the same big-endian order as @ref Mashiro::Uuid. */
        uint64_t lo{}; /**< Least-significant 64 bits in the same big-endian order as @ref Mashiro::Uuid. */

        /** @brief Construct the nil identifier. */
        constexpr Iid() noexcept = default;

        /** @brief Construct from explicit most/least-significant halves. */
        constexpr Iid(uint64_t high, uint64_t low) noexcept : hi{high}, lo{low} {}

        /** @brief Adapt a Mashiro UUID into the Yuki ABI shape without changing the bit pattern. */
        explicit constexpr Iid(Mashiro::Uuid uuid) noexcept
            : hi{static_cast<uint64_t>(uuid.ToUint128() >> 64)}, lo{static_cast<uint64_t>(uuid.ToUint128())} {}

        /** @brief Rebuild the semantic UUID value represented by this IID. */
        [[nodiscard]] constexpr Mashiro::Uuid ToUuid() const noexcept { return Mashiro::Uuid{hi, lo}; }

        /** @brief Tell if the IID is the nil identifier. */
        [[nodiscard]] constexpr bool IsNil() const noexcept { return hi == 0 && lo == 0; }

        constexpr bool operator==(const Iid&) const noexcept = default;
        constexpr auto operator<=>(const Iid&) const noexcept = default;
    };

    static_assert(sizeof(Iid) == 16);
    static_assert(std::is_trivially_copyable_v<Iid>);

    /** @brief The all-zero identifier. */
    inline constexpr Iid kNilIid{};

    /** @brief Return whether @p iid is the nil identifier. */
    [[nodiscard]] constexpr bool IsNil(Iid iid) noexcept {
        return iid.hi == 0 && iid.lo == 0;
    }

    /** @} */

    namespace Anno {

        /**
         * @brief Structural, annotation-safe view over a static list of reflected C++ types.
         *
         * P3385 annotations require structural values. `std::span` is not structural, so Core stores a public
         * pointer/length pair. The initializer-list constructor promotes inline reflection lists into static storage,
         * making declarations such as `[[=Yuki::Anno::Implements{^^IPosition, ^^IName}]]` lifetime-safe.
         */
        struct InfoList {
            const std::meta::info* first{}; /**< First reflected type in static storage, or null for empty. */
            std::size_t count{};            /**< Number of reflected types in @ref first. */

            /** @brief Construct an empty type-reflection list. */
            consteval InfoList() noexcept = default;

            /** @brief Adopt an existing static array of type reflections. */
            template <std::size_t N>
            consteval InfoList(const std::meta::info (&items)[N]) noexcept : first{items}, count{N} {}

            /** @brief Promote an inline braced list of type reflections into static storage. */
            consteval InfoList(std::initializer_list<std::meta::info> items)
                : first{std::define_static_array(std::vector<std::meta::info>(items.begin(), items.end())).data()},
                  count{items.size()} {}

            /** @brief Return an iterator to the first reflected type. */
            [[nodiscard]] consteval const std::meta::info* begin() const noexcept { return first; }

            /** @brief Return an iterator one past the last reflected type. */
            [[nodiscard]] consteval const std::meta::info* end() const noexcept { return first + count; }

            /** @brief Return the number of reflected types. */
            [[nodiscard]] consteval std::size_t size() const noexcept { return count; }

            /** @brief Return whether the list is empty. */
            [[nodiscard]] consteval bool empty() const noexcept { return count == 0; }
        };

        /** @brief Explicitly supplies the IID for a type instead of using the reflected name hash. */
        struct IidOverride {
            Iid value{}; /**< Replacement identifier. */
        };

        /** @brief Declares the interface facets directly provided by an object-model class. */
        struct Implements {
            InfoList interfaces{}; /**< Reflected interface types implemented by the annotated class. */

            /** @brief Construct from an inline braced list of reflected interface types. */
            consteval Implements(std::initializer_list<std::meta::info> items) : interfaces{items} {}

            /** @brief Construct from an existing static array of reflected interface types. */
            template <std::size_t N>
            consteval Implements(const std::meta::info (&items)[N]) noexcept : interfaces{items} {}
        };

        /** @brief Declares the object-model classes extended by an extension class. */
        struct Extends {
            InfoList classes{}; /**< Reflected implementation/component classes extended by the annotated class. */

            /** @brief Construct from an inline braced list of reflected extendee classes. */
            consteval Extends(std::initializer_list<std::meta::info> items) : classes{items} {}

            /** @brief Construct from an existing static array of reflected extendee classes. */
            template <std::size_t N> consteval Extends(const std::meta::info (&items)[N]) noexcept : classes{items} {}
        };

    } // namespace Anno

    namespace Detail {

        /** @brief Synthesize an IID from a reflected type name via Mashiro FNV-1a 128 and UUID v8 stamping. */
        consteval Iid IidFromName(std::string_view name) noexcept {
            const auto digest = Mashiro::Hashing::Hash(name, Mashiro::Hashing::Fnv1a128{});
            return Iid{Mashiro::Uuid::FromUint128(digest).WithRfc4122(8)};
        }

        /** @brief Read an optional explicit IID override from annotations on @p type. */
        consteval std::optional<Iid> IidOverrideOfMeta(std::meta::info type) {
            std::optional<Iid> result;
            for (auto anno : std::meta::annotations_of(std::meta::dealias(type), ^^Anno::IidOverride)) {
                if (result) {
                    throw "Yuki::IidOfMeta: duplicate IidOverride annotations";
                }
                result = std::meta::extract<Anno::IidOverride>(anno).value;
            }
            return result;
        }

    } // namespace Detail

} // namespace Yuki

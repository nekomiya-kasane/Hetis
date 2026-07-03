/**
 * @file IID.h
 * @brief Stable IID values, reflected IID synthesis, and annotation payloads for object-model edges.
 * @ingroup Core
 */
#pragma once

#include "Sora/Core/Traits/AnnotationTraits.h"
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <meta>
#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>

#include <Sora/Core/Hash.h>
#include <Sora/Core/Polymorphism.h>
#include <Sora/Core/Traits/MetaInfoList.h>

namespace Sora::Kernel {

    /**
     * @brief IID
     * @{
     */

    /** @brief A 128-bit stable interface/class identifier used by the Sora object model. */
    struct Iid : Uuid {
        constexpr Iid() noexcept = default;
        explicit constexpr Iid(uint128_t v) noexcept : Uuid(v) {}
        /** @brief Compose from most/least-significant 64-bit halves (big-endian). */
        constexpr Iid(uint64_t hi, uint64_t lo) noexcept : Uuid(hi, lo) {}

        /** @brief Compose from RFC-4122 fields: 8-4-4 and trailing 8 bytes (clock_seq + node). */
        constexpr Iid(uint32_t timeLow, uint16_t timeMid, uint16_t timeHiAndVersion,
                      std::array<uint8_t, 8> tail) noexcept
            : Uuid(timeLow, timeMid, timeHiAndVersion, tail) {}

        constexpr bool operator==(const Iid&) const noexcept = default;
        constexpr auto operator<=>(const Iid&) const noexcept = default;
    };

    static_assert(sizeof(Iid) == 16);
    static_assert(std::is_trivially_copyable_v<Iid>);

    /** @brief The all-zero identifier. */
    inline constexpr Iid kNilIid{};

    /** @brief Return whether @p iid is the nil identifier. */
    [[nodiscard]] constexpr bool IsNil(Iid iid) noexcept {
        return iid == kNilIid;
    }

    /** @} */

    namespace $ {

        /** @brief Explicitly supplies the IID for a type instead of using the reflected name hash. */
        struct IidOverride {
            Iid value{}; /**< Replacement identifier. */
        };

        /** @brief Declares the interface facets directly provided by an object-model class. */
        struct Implements {
            Meta::InfoList interfaces{}; /**< Reflected interface types implemented by the annotated class. */

            /** @brief Construct from an inline braced list of reflected interface types. */
            consteval Implements(std::initializer_list<std::meta::info> items) : interfaces{items} {}

            /** @brief Construct from an existing static array of reflected interface types. */
            template<std::size_t N>
            consteval Implements(const std::meta::info (&items)[N]) noexcept : interfaces{items} {}
        };

        /** @brief Declares the object-model classes extended by an extension class. */
        struct Extends {
            Meta::InfoList
                classes{}; /**< Reflected implementation/component classes extended by the annotated class. */

            /** @brief Construct from an inline braced list of reflected extendee classes. */
            consteval Extends(std::initializer_list<std::meta::info> items) : classes{items} {}

            /** @brief Construct from an existing static array of reflected extendee classes. */
            template<std::size_t N>
            consteval Extends(const std::meta::info (&items)[N]) noexcept : classes{items} {}
        };

    } // namespace $

    namespace Traits {

        template<typename T>
            requires(std::meta::is_class_type(^^T))
        inline constexpr Iid IidOf = [] consteval {
            auto iid = Sora::$::GetSingleOptional<$::IidOverride>(^^T);
            return iid.has_value() ? iid.value().value : Iid{Sora::Traits::AbiDigestOf<T>};
        }();

    } // namespace Traits

} // namespace Sora::Kernel

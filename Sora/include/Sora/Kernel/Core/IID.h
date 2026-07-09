/**
 * @file IID.h
 * @brief Stable IID values, reflected IID synthesis, and annotation payloads for object-model edges.
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/Traits/AnnotationTraits.h>
#include <Sora/Core/Hash.h>
#include <Sora/Core/Polymorphism.h>
#include <Sora/Core/Traits/ScopeTraits.h>

#include <Sora/Kernel/Core/ClassTypes.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <meta>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

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

    } // namespace $

    namespace Traits {

        template<typename T>
        consteval Iid ReflectedClassIidOf() {
            Sora::Hashing::Fnv1a128State h;
            constexpr std::string_view name = Sora::Meta::ScopeChainIdentifierOf(^^T, "::");
            auto bytes = Sora::Meta::BytesOf(name);
            h.Feed(std::span<const std::byte>{bytes.data(), bytes.size()});
            return Iid{h.Finalize()};
        }

        template<typename T>
            requires(std::meta::is_class_type(^^T))
        inline constexpr Iid IidOf = [] consteval {
            static_assert(!Concept::VirtualObjectClass<T>,
                          "IidOf: T is a virtual object class, please include VirtualObject.h to get a stable IID.");
            if constexpr (IsTie(Traits::RoleOf<T>)) {
                return Sora::Kernel::Traits::IidOf<typename Sora::Traits::DirectBaseType<T, 0>>;
            } else if constexpr (IsInterface(Traits::RoleOf<T>)) {
                auto iid = Sora::$::GetSingleOptional<$::IidOverride>(^^T);
                if (iid.has_value()) {
                    return iid.value().value;
                }
                return Iid{Sora::Traits::AbiDigestOf<T>};
            } else {
                auto iid = Sora::$::GetSingleOptional<$::IidOverride>(^^T);
                if (iid.has_value()) {
                    return iid.value().value;
                }
                return ReflectedClassIidOf<T>();
            }
        }();

    } // namespace Traits

} // namespace Sora::Kernel

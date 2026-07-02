/**
 * @file EnumTraits.h
 * @brief Reflection-based enum traits, enum concepts, and enum metadata tables.
 * @ingroup Core
 *
 * @details Provides compile-time enum introspection helpers built on C++26 static reflection. The facilities in this
 * file classify enums, expose enumerator names and values, and provide small conversion utilities without runtime
 * registration tables.
 */
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <meta>
#include <optional>
#include <set>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Sora {

    namespace Concept {

        /** @brief Type that is an enumeration. */
        template<typename T>
        concept Enum = std::is_enum_v<T>;

        /** @brief Scoped enumeration type that is not implicitly convertible to its underlying type. */
        template<typename T>
        concept EnumClass = std::is_enum_v<T> && !std::is_convertible_v<T, std::underlying_type_t<T>>;

    } // namespace Concept

    namespace Meta {

        /**
         * @brief Return a static reflection array containing the enumerators of @p iMeta.
         * @param[in] iMeta Reflection of an enum type.
         * @return Static-storage array of enumerator reflections.
         */
        consteval auto EnumeratorsOf(std::meta::info iMeta) {
            return std::define_static_array(std::meta::enumerators_of(iMeta));
        }

    } // namespace Meta

    namespace Traits {

        /** @brief Number of enumerators in @p T. */
        template<Concept::Enum T>
        inline constexpr size_t EnumeratorsCount = Meta::EnumeratorsOf(^^T).size();

    } // namespace Traits

    namespace Concept {

        /** @brief Enum whose non-special enumerators are all zero or single-bit values. */
        template<typename T>
        concept BitfieldEnum = Enum<T> && Traits::EnumeratorsCount<T> > 0 && [] consteval {
            using U = std::make_unsigned_t<std::underlying_type_t<T>>;
            template for (constexpr auto e : Meta::EnumeratorsOf(^^T)) {
                if (std::meta::display_string_of(e) == "All" || std::meta::display_string_of(e) == "None") {
                    continue;
                }
                if (std::meta::display_string_of(e).contains("Mask")) {
                    continue;
                }
                auto v = static_cast<U>([:e:]);
                if (v != 0 && !std::has_single_bit(v)) {
                    return false;
                }
            }
            return true;
        }();

        /** @brief Enum whose non-special enumerators have unique underlying values. */
        template<typename T>
        concept SequentialEnum = Enum<T> && Traits::EnumeratorsCount<T> > 0 && [] consteval {
            using U = std::underlying_type_t<T>;
            std::set<U> values;
            template for (constexpr auto e : Meta::EnumeratorsOf(^^T)) {
                if (std::meta::display_string_of(e) == "All" || std::meta::display_string_of(e) == "None") {
                    continue;
                }
                if (std::meta::display_string_of(e).contains("Mask")) {
                    continue;
                }
                if (values.contains(static_cast<U>([:e:]))) {
                    return false;
                }
                values.insert(static_cast<U>([:e:]));
            }

            return true;
        }();

    } // namespace Concept

    namespace Traits {

        /** @brief All-valid-bits mask for a @ref Concept::BitfieldEnum, computed as the OR of all enumerators. */
        template<Concept::BitfieldEnum E>
        inline constexpr auto kBitfieldMask = static_cast<E>([] consteval {
            using U = std::make_unsigned_t<std::underlying_type_t<E>>;
            U mask{};
            template for (constexpr auto e : Meta::EnumeratorsOf(^^E)) {
                mask |= static_cast<U>([:e:]);
            }
            return mask;
        }());

        /** @brief Static array containing every enumerator value of @p E in declaration order. */
        template<Concept::Enum E>
        inline constexpr auto kEnumValues = [] consteval {
            return []<size_t... I>(std::index_sequence<I...>) {
                return std::array<E, sizeof...(I)>{std::meta::extract<E>(Meta::EnumeratorsOf(^^E)[I])...};
            }(std::make_index_sequence<Traits::EnumeratorsCount<E>>{});
        }();

        /** @brief Static array containing every enumerator source identifier of @p E in declaration order. */
        template<Concept::Enum E>
        inline constexpr auto kEnumNames = [] consteval {
            return []<size_t... I>(std::index_sequence<I...>) {
                return std::array<std::string_view, sizeof...(I)>{
                    std::meta::identifier_of(Meta::EnumeratorsOf(^^E)[I])...};
            }(std::make_index_sequence<Traits::EnumeratorsCount<E>>{});
        }();

    } // namespace Traits

    namespace Meta {

        /**
         * @brief Convert an enumerator source identifier to an enum value.
         * @tparam E Enum type to inspect.
         * @param[in] name Enumerator source identifier.
         * @return Matching enum value, or @c std::nullopt when no enumerator has @p name.
         */
        template<Concept::Enum E>
        [[nodiscard]] constexpr std::optional<E> EnumCast(std::string_view name) noexcept {
            template for (constexpr auto e : Meta::EnumeratorsOf(^^E)) {
                if (std::meta::identifier_of(e) == name) {
                    return std::meta::extract<E>(e);
                }
            }
            return std::nullopt;
        }

    } // namespace Meta

} // namespace Sora

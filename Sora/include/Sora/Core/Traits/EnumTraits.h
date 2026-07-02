/**
 * @file EnumTraits.h
 * @brief Reflection-based enum traits and utility types.
 *
 * @par Enum reflection
 * - `Enumerators<T>` / `EnumeratorsCount<T>` — reflected enumerators.
 * - `EnumUnderlying<E>` / `EnumValues<E>` / `EnumNames<E>` — value/name tables.
 * - `EnumName(value)` / `EnumCast<E>(name)` — runtime value<->name conversion.
 * - `SequentialEnum<T>` / `BitfieldEnum<E>` / `kBitfieldMask<E>` — categorisation.
 * @ingroup Core
 */
#pragma once

#include <algorithm>
#include <bit>
#include <meta>
#include <type_traits>
#include <set>
#include <utility>
#include <vector>

namespace Sora {

    namespace Concept {

        template<typename T>
        concept Enum = std::is_enum_v<T>;

        template<typename T>
        concept EnumClass = std::is_enum_v<T> && !std::is_convertible_v<T, std::underlying_type_t<T>>;

    } // namespace Concept

    namespace Meta {

        consteval auto EnumeratorsOf(std::meta::info iMeta) {
            return std::define_static_array(std::meta::enumerators_of(iMeta));
        }

    }

    namespace Traits {

        /// @brief Number of enumerators in @p T.
        template<Concept::Enum T>
        inline constexpr size_t EnumeratorsCount = Meta::EnumeratorsOf(^^T).size();

    } // namespace Traits

    namespace Concept {

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

        /// @brief All-valid-bits mask for a BitfieldEnum (OR of all enumerators).
        template <Concept::BitfieldEnum E>
        inline constexpr auto kBitfieldMask = static_cast<E>([] consteval {
            using U = std::make_unsigned_t<std::underlying_type_t<E>>;
            U mask{};
            template for (constexpr auto e : Meta::EnumeratorsOf(^^E)) {
                mask |= static_cast<U>([:e:]);
            }
            return mask;
        }());

        template <Concept::Enum E>
        inline constexpr auto kEnumValues = [] consteval {
            return []<size_t... I>(std::index_sequence<I...>) {
                return std::array<E, sizeof...(I)>{std::meta::extract<E>(Meta::EnumeratorsOf(^^E)[I])...};
            }(std::make_index_sequence<Traits::EnumeratorsCount<E>>{});
        }();

        template <Concept::Enum E>
        inline constexpr auto kEnumNames = [] consteval {
            return []<size_t... I>(std::index_sequence<I...>) {
                return std::array<std::string_view, sizeof...(I)>{std::meta::identifier_of(Meta::EnumeratorsOf(^^E)[I])...};
            }(std::make_index_sequence<Traits::EnumeratorsCount<E>>{});
        }();

    } // namespace Traits

    namespace Meta {

        template <Concept::Enum E>
        [[nodiscard]] constexpr std::optional<E> EnumCast(std::string_view name) noexcept {
            template for (constexpr auto e : Meta::EnumeratorsOf(^^E)) {
                if (std::meta::identifier_of(e) == name)
                    return std::meta::extract<E>(e);
            }
            return std::nullopt;
        }

    }

} // namespace Sora
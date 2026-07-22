/**
 * @file EnumTraits.h
 * @brief Reflection-based enum traits, enum concepts, names, descriptions, and string conversion helpers.
 * @ingroup Core
 *
 * @details Provides compile-time enum introspection helpers built on C++26 static reflection. The facilities in this
 * file classify enums, expose enumerator names and values, handle annotation-backed descriptions, and centralise enum
 * string conversion so higher layers such as ToString and JSON do not duplicate enum reflection logic.
 */
#pragma once

#include "Sora/Core/FixedString.h"
#include "Sora/Core/StringUtils.h"
#include "Sora/Core/Traits/TypeTraits.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <format>
#include <meta>
#include <optional>
#include <string>
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
        concept EnumClass = std::is_scoped_enum_v<T>;

        /** @brief An unscoped C-style enumeration. */
        template<typename T>
        concept UnscopedEnum = std::is_enum_v<T> && !std::is_scoped_enum_v<T>;

        /**
         * @brief Reflection of an enumerator reserved for aggregate or sentinel semantics.
         * @tparam Info Enumerator reflection to classify.
         */
        template<std::meta::info Info>
        concept SpecialEnumerator = std::meta::is_enumerator(Info) && [] consteval {
            constexpr std::string_view name = std::meta::identifier_of(Info);
            return name == "All" || name == "None" || name.contains("Mask");
        }();

    } // namespace Concept

    namespace Meta {

        /**
         * @brief Return a static reflection array containing the enumerators of @p iMeta.
         * @param[in] iMeta Reflection of an enum type.
         * @return Static-storage array of enumerator reflections.
         */
        consteval auto EnumeratorsOf(std::meta::info iMeta) {
            return std::define_static_array(std::meta::enumerators_of(std::meta::dealias(iMeta)));
        }

        /**
         * @brief Return the reflection of the enumerator with the given name in @p iMeta.
         * @param[in] iMeta Reflection of an enum type.
         * @param[in] name Name of the enumerator to find.
         * @return Reflection of the matching enumerator.
         */
        consteval auto GetEnumeratorMetaByNameOf(std::meta::info iMeta, std::string_view name) {
            for (std::meta::info enumerator : EnumeratorsOf(iMeta)) {
                if (std::meta::identifier_of(enumerator) == name) {
                    return enumerator;
                }
            }
            throw std::define_static_string(
                "Sora::Meta::GetEnumeratorMetaByNameOf: no enumerator with the given name exists.");
        }

        /**
         * @brief Return the reflection of the enumerator with the given value in @p iMeta.
         * @param[in] iMeta Reflection of an enum type.
         * @param[in] value Value of the enumerator to find.
         * @return Reflection of the matching enumerator.
         */
        template<Sora::Concept::Enum E>
        consteval std::meta::info GetEnumeratorMetaOf(E value) {
            for (std::meta::info enumerator : EnumeratorsOf(^^E)) {
                if (std::meta::extract<E>(enumerator) == value) {
                    return enumerator;
                }
            }
            throw std::define_static_string(
                "Sora::Meta::GetEnumeratorMetaOf: no enumerator with the given value exists.");
        }

    } // namespace Meta

    namespace Traits {

        /** @brief Number of enumerators in @p T. */
        template<Concept::Enum T>
        inline constexpr size_t EnumeratorsCountOf = Meta::EnumeratorsOf(^^T).size();

        /** @brief Static array containing every enumerator value of @p T in declaration order. */
        template<Concept::Enum T>
        inline constexpr std::array<T, Traits::EnumeratorsCountOf<T>> EnumeratorsArrOf = [] consteval {
            return []<size_t... I>(std::index_sequence<I...>) {
                return std::array<T, sizeof...(I)>{std::meta::extract<T>(Meta::EnumeratorsOf(^^T)[I])...};
            }(std::make_index_sequence<Traits::EnumeratorsCountOf<T>>{});
        }();

        /** @brief Number of enumerators excluding aggregate and sentinel enumerators. */
        template<Concept::Enum T>
        inline constexpr size_t OrdinaryEnumeratorsCountOf = [] consteval {
            size_t count = 0;
            template for (constexpr auto enumerator : Meta::EnumeratorsOf(^^T)) {
                if constexpr (!Concept::SpecialEnumerator<enumerator>) {
                    ++count;
                }
            }
            return count;
        }();

        /** @brief Ordinary enumerator values in declaration order. */
        template<Concept::Enum T>
        inline constexpr std::array<T, OrdinaryEnumeratorsCountOf<T>> OrdinaryEnumeratorsArrOf = [] consteval {
            std::array<T, OrdinaryEnumeratorsCountOf<T>> result{};
            size_t index = 0;
            template for (constexpr auto enumerator : Meta::EnumeratorsOf(^^T)) {
                if constexpr (!Concept::SpecialEnumerator<enumerator>) {
                    result[index++] = std::meta::extract<T>(enumerator);
                }
            }
            return result;
        }();

    } // namespace Traits

    namespace $ {

        /** @brief Human-readable description annotation for enum values and other reflected declarations. */
        struct Description {
            FixedString<256> text{}; /**< Description text. */

            constexpr Description() = default;

            template<size_t N>
            consteval Description(const char (&value)[N]) : text(value) {}
        };

    } // namespace $

    namespace Meta {

        /**
         * @brief Return the @ref Sora::$::Description annotation for @p e, or an empty string when none is present.
         * @param[in] e Reflection of an enumerator or declaration.
         * @return Description text, or empty string when no description is present.
         */
        consteval std::string_view DescriptionOf(std::meta::info e) {
            auto annotations = std::meta::annotations_of(e, ^^Sora::$::Description);
            auto desc = std::ranges::find_if(
                annotations, [](std::meta::info a) { return std::meta::type_of(a) == ^^Sora::$::Description; });
            if (desc != annotations.end()) {
                return std::define_static_string(std::meta::extract<Sora::$::Description>(*desc).text.view());
            }
            return std::string_view{};
        }

    } // namespace Meta

    namespace Concept {

        /** @brief Enum whose ordinary enumerators are all zero or single-bit values. */
        template<typename T>
        concept BitfieldEnum = Enum<T> && Traits::EnumeratorsCountOf<T> > 0 && [] consteval {
            using U = std::make_unsigned_t<std::underlying_type_t<T>>;
            template for (constexpr auto e : Meta::EnumeratorsOf(^^T)) {
                if constexpr (SpecialEnumerator<e>) {
                    continue;
                }
                const auto v = static_cast<U>([:e:]);
                if (v != 0 && !std::has_single_bit(v)) {
                    return false;
                }
            }
            return true;
        }();

        /** @brief Enum whose ordinary enumerators have unique underlying values. */
        template<typename T>
        concept SequentialEnum = Enum<T> && Traits::EnumeratorsCountOf<T> > 0 && [] consteval {
            using U = std::underlying_type_t<T>;
            std::array<U, Traits::OrdinaryEnumeratorsCountOf<T>> values{};
            size_t count = 0;
            template for (constexpr auto e : Meta::EnumeratorsOf(^^T)) {
                if constexpr (SpecialEnumerator<e>) {
                    continue;
                }
                const U value = static_cast<U>([:e:]);
                if (std::ranges::find(values.begin(), values.begin() + count, value) != values.begin() + count) {
                    return false;
                }
                values[count++] = value;
            }
            return true;
        }();

        /** @brief Scoped sequential enum whose ordinary values are exactly zero-based declaration ordinals. */
        template<typename T>
        concept OrdinalEnum = SequentialEnum<T> && EnumClass<T> && [] consteval {
            using U = std::underlying_type_t<T>;
            size_t expected = 0;
            template for (constexpr auto e : Meta::EnumeratorsOf(^^T)) {
                if constexpr (SpecialEnumerator<e>) {
                    continue;
                }
                if (static_cast<U>([:e:]) != static_cast<U>(expected)) {
                    return false;
                }
                ++expected;
            }
            return true;
        }();

    } // namespace Concept

    namespace Traits {

        /** @brief All-valid-bits mask for a @ref Concept::BitfieldEnum, computed as the OR of all enumerators. */
        template<Concept::BitfieldEnum E>
        inline constexpr auto BitfieldMask = static_cast<E>([] consteval {
            using U = std::make_unsigned_t<std::underlying_type_t<E>>;
            U mask{};
            template for (constexpr auto e : Meta::EnumeratorsOf(^^E)) {
                mask |= static_cast<U>([:e:]);
            }
            return mask;
        }());

        /** @brief Static array containing every enumerator value of @p E in declaration order. */
        template<Concept::Enum E>
        inline constexpr auto EnumValues = EnumeratorsArrOf<E>;

        /** @brief Static array containing every enumerator source identifier of @p E in declaration order. */
        template<Concept::Enum E>
        inline constexpr auto EnumNames = [] consteval {
            return []<size_t... I>(std::index_sequence<I...>) {
                return std::array<std::string_view, sizeof...(I)>{
                    std::meta::identifier_of(Meta::EnumeratorsOf(^^E)[I])...};
            }(std::make_index_sequence<Traits::EnumeratorsCountOf<E>>{});
        }();

        /**
         * @brief Return whether @p value is an enumerator declared by @p E.
         * @details The check is exact: a value whose underlying representation is not declared by the enum is
         * invalid, even when its underlying integer happens to be within an apparent range. Bitmask combinations
         * therefore require a separate bitfield validation policy.
         * @tparam E Enum type to inspect.
         * @param[in] value Enum value to validate.
         * @return @c true when @p value exactly equals one declared enumerator; otherwise @c false.
         */
        template<Concept::Enum E>
        [[nodiscard]] constexpr bool IsValidEnumValue(E value) noexcept {
            template for (constexpr auto enumerator : Meta::EnumeratorsOf(^^E)) {
                if ([:enumerator:] == value) {
                    return true;
                }
            }
            return false;
        }

        /** @brief Return the source identifier of @p value, or an empty view when no exact enumerator exists. */
        template<Concept::Enum E>
        [[nodiscard]] constexpr std::string_view EnumIdentifier(E value) noexcept {
            template for (constexpr auto e : Meta::EnumeratorsOf(^^E)) {
                if ([:e:] == value) {
                    return Meta::IdentifierOf(e);
                }
            }
            return {};
        }

        /** @brief Return the display name of @p value, or an empty view when no exact enumerator exists. */
        template<Concept::Enum E>
        [[nodiscard]] constexpr std::string_view EnumDisplayName(E value) noexcept {
            template for (constexpr auto e : Meta::EnumeratorsOf(^^E)) {
                if ([:e:] == value) {
                    return Meta::DisplayStringOf(e);
                }
            }
            return {};
        }

        /** @brief Return the description annotation of @p value, or an empty view when absent or not exact. */
        template<Concept::Enum E>
        [[nodiscard]] constexpr std::string_view EnumDescription(E value) noexcept {
            template for (constexpr auto e : Meta::EnumeratorsOf(^^E)) {
                if ([:e:] == value) {
                    return Meta::DescriptionOf(e);
                }
            }
            return {};
        }

        /** @brief Return a stable view for exact enum values, or the enum type name for unknown values. */
        template<Concept::Enum E>
        [[nodiscard]] constexpr std::string_view EnumToStringView(E value) noexcept {
            if (const std::string_view exact = EnumDisplayName(value); !exact.empty()) {
                return exact;
            }
            return Traits::TypeName<E>;
        }

        /** @brief Convert @p value to a display string, decomposing bitfield enums when possible. */
        template<Concept::Enum E>
        [[nodiscard]] std::string EnumToString(E value) {
            using Raw = std::underlying_type_t<E>;
            if (const std::string_view exact = EnumDisplayName(value); !exact.empty()) {
                return std::string(exact);
            }
            if constexpr (Concept::BitfieldEnum<E>) {
                using U = std::make_unsigned_t<Raw>;
                U remaining = static_cast<U>(value);
                std::string out;
                template for (constexpr auto e : Meta::EnumeratorsOf(^^E)) {
                    constexpr auto flag = static_cast<U>([:e:]);
                    if constexpr (flag != U{0}) {
                        if ((remaining & flag) == flag) {
                            if (!out.empty()) {
                                out += " | ";
                            }
                            out += Meta::DisplayStringOf(e);
                            remaining &= static_cast<U>(~flag);
                        }
                    }
                }
                if (!out.empty() && remaining == U{0}) {
                    return out;
                }
            }
            return std::format("{}(unknown:{})", Traits::TypeName<E>, static_cast<Raw>(value));
        }

    } // namespace Traits

    namespace Meta {

        /**
         * @brief Convert an enumerator token to an enum value.
         * @details Matches source identifiers, display strings, and description annotations for exact enumerators.
         * @tparam E Enum type to inspect.
         * @param[in] token Enumerator token.
         * @return Matching enum value, or @c std::nullopt when no enumerator has @p token.
         */
        template<Concept::Enum E>
        [[nodiscard]] constexpr std::optional<E> EnumCastToken(std::string_view token) noexcept {
            token = Sora::Ascii::String::Trim(token);
            template for (constexpr auto e : Meta::EnumeratorsOf(^^E)) {
                if (token == Meta::IdentifierOf(e) || token == Meta::DisplayStringOf(e) ||
                    token == Meta::DescriptionOf(e)) {
                    return std::meta::extract<E>(e);
                }
            }
            return std::nullopt;
        }

        /**
         * @brief Convert an enum display string to an enum value.
         * @details For bitfield enums, accepts @c | separated tokens using identifiers, display strings, or
         * descriptions. The token @c 0 is accepted as the empty flag set.
         * @tparam E Enum type to inspect.
         * @param[in] name Enumerator or bitfield display string.
         * @return Matching enum value, or @c std::nullopt when parsing fails.
         */
        template<Concept::Enum E>
        [[nodiscard]] constexpr std::optional<E> EnumCast(std::string_view name) noexcept {
            if (auto exact = EnumCastToken<E>(name); exact.has_value()) {
                return exact;
            }
            if constexpr (Concept::BitfieldEnum<E>) {
                using U = std::make_unsigned_t<std::underlying_type_t<E>>;
                U acc{};
                bool sawToken = false;
                size_t pos = 0;
                while (pos <= name.size()) {
                    size_t bar = name.find('|', pos);
                    if (bar == std::string_view::npos) {
                        bar = name.size();
                    }
                    const std::string_view token = Sora::Ascii::String::Trim(name.substr(pos, bar - pos));
                    if (!token.empty() && token != "0") {
                        auto value = EnumCastToken<E>(token);
                        if (!value.has_value()) {
                            return std::nullopt;
                        }
                        acc |= static_cast<U>(*value);
                        sawToken = true;
                    }
                    if (bar == name.size()) {
                        break;
                    }
                    pos = bar + 1;
                }
                if (sawToken || Sora::Ascii::String::Trim(name) == "0") {
                    return static_cast<E>(acc);
                }
            }
            return std::nullopt;
        }

    } // namespace Meta

    /** @brief Return an enum value description, falling back to the normal enum string representation. */
    template<Concept::Enum E>
    [[nodiscard]] std::string DescriptionOf(E value) {
        if (const std::string_view desc = Traits::EnumDescription(value); !desc.empty()) {
            return std::string(desc);
        }
        return Traits::EnumToString(value);
    }

} // namespace Sora

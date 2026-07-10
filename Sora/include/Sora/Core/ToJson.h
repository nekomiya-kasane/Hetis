/**
 * @file ToJson.h
 * @brief Reflection-driven JSON conversion built on nlohmann/json and C++26 annotations.
 * @ingroup Core
 *
 * @details Provides @ref Sora::ToJson and @ref Sora::FromJson. Generic object conversion uses P2996 reflection and
 * the shared display annotations @ref Sora::$::Ignore and @ref Sora::$::Rename. JSON-only annotations live under
 * @ref Sora::$::Serialization so ordinary string rendering does not depend on JSON absence, ordering, flattening, or
 * enum encoding policy.
 */
#pragma once

#include "Sora/Core/ToString.h"
#include "Sora/Core/Traits.h"
#include "Sora/Core/Traits/AnnotationTraits.h"
#include "Sora/Core/Traits/EnumTraits.h"
#include "Sora/Core/Traits/TypeTraits.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <meta>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace Sora {

    /** @brief The JSON value type used by Sora. */
    using json = nlohmann::json;

    /** @brief Insertion-order-preserving JSON value type used for golden-file output. */
    using ordered_json = nlohmann::ordered_json;

    namespace $::Serialization {

        /** @brief Whitelist mode: when any member carries this annotation, only keyed members participate. */
        struct Key {
            constexpr bool operator==(const Key&) const = default;
        };

        /** @brief JSON parse policy: a missing key keeps the member's current or value-initialized value. */
        struct Optional {
            constexpr bool operator==(const Optional&) const = default;
        };

        /** @brief JSON parse policy: a missing key is an error. */
        struct Required {
            constexpr bool operator==(const Required&) const = default;
        };

        /** @brief JSON object emission ordering; lower priority emits earlier. */
        struct Order {
            int priority = 0;
            constexpr bool operator==(const Order&) const = default;
        };

        /** @brief Splice a nested JSON object into its parent object during class serialization. */
        struct Flatten {
            constexpr bool operator==(const Flatten&) const = default;
        };

        /** @brief Enum-only JSON policy: serialize as the underlying integer value. */
        struct AsInt {
            constexpr bool operator==(const AsInt&) const = default;
        };

        /** @brief Enum-only JSON policy: serialize as the reflected enumerator name. */
        struct AsString {
            constexpr bool operator==(const AsString&) const = default;
        };

        /** @brief Control whether default-valued members are emitted. */
        struct EmitDefault {
            bool emit = true;
            constexpr bool operator==(const EmitDefault&) const = default;
        };

    } // namespace $::Serialization

    namespace Hook {

        /**
         * @brief Open customisation point for @ref ToJson and @ref FromJson.
         *
         * @details Specialise this hook when a type needs a JSON representation that cannot be derived structurally.
         * A specialisation provides @c static @c json @c ToJson(const @c T&) and may also provide
         * @c static @c void @c FromJson(const @c json&, @c T&). The primary template is intentionally undefined.
         *
         * @tparam T Cv-unqualified type to customise.
         */
        template<typename T>
        struct ToJsonHook;

    } // namespace Hook

    namespace Json {

        namespace Detail {

            template<typename T>
            [[nodiscard]] json ToJsonImpl(T&& value);

            template<typename T>
            void FromJsonImpl(const json& input, T& output);

            template<typename T>
            inline constexpr bool IsChronoDuration = false;

            template<typename Rep, typename Period>
            inline constexpr bool IsChronoDuration<std::chrono::duration<Rep, Period>> = true;

            template<typename T>
            concept ChronoDuration = IsChronoDuration<std::remove_cvref_t<T>>;

            template<typename T>
            inline constexpr bool IsChronoTimePoint = false;

            template<typename Clock, typename Duration>
            inline constexpr bool IsChronoTimePoint<std::chrono::time_point<Clock, Duration>> = true;

            template<typename T>
            concept ChronoTimePoint = IsChronoTimePoint<std::remove_cvref_t<T>>;

            template<typename T>
            concept StringKeyedAssociative =
                std::ranges::range<T> && requires(std::ranges::range_reference_t<T> entry) {
                    entry.first;
                    entry.second;
                    std::string_view(entry.first);
                };

            template<typename T>
            concept ReflectableClass =
                std::is_class_v<std::remove_cvref_t<T>> && !std::is_union_v<std::remove_cvref_t<T>> &&
                !std::same_as<std::remove_cvref_t<T>, std::filesystem::path> && !Sora::Concept::StringLike<T> &&
                !ChronoDuration<T> && !ChronoTimePoint<T> && !Sora::Concept::OptionalLike<T> &&
                !Concept::VariantLikeClass<std::remove_cvref_t<T>> &&
                !Concept::TupleLikeClass<std::remove_cvref_t<T>> && !std::ranges::range<T>;

            namespace ADL {

                /** @brief Poison pill for unqualified JSON emission lookup. */
                void ToJson() = delete;

                /** @brief Poison pill for unqualified JSON parse lookup. */
                void FromJson() = delete;

                template<typename T>
                concept HasFreeToJson = requires(T&& value) {
                    { ToJson(std::forward<T>(value)) } -> std::convertible_to<json>;
                };

                template<typename T>
                concept HasFreeFromJson = requires(const json& input, T& output) { FromJson(input, output); };

            } // namespace ADL

            template<typename T>
            concept HasMemberToJson = requires(const T& value) {
                { value.ToJson() } -> std::convertible_to<json>;
            };

            template<typename T>
            concept HasMemberFromJson = requires(const json& input) {
                { T::FromJson(input) } -> std::convertible_to<T>;
            };

            template<typename T>
            concept HasHookToJson = requires(const T& value) {
                { Hook::ToJsonHook<T>::ToJson(value) } -> std::convertible_to<json>;
            };

            template<typename T>
            concept HasHookFromJson =
                requires(const json& input, T& output) { Hook::ToJsonHook<T>::FromJson(input, output); };

            template<typename T>
            concept HasNlohmannToJson = requires(json& output, const T& value) { ::nlohmann::to_json(output, value); };

            template<typename T>
            concept HasNlohmannFromJson =
                requires(const json& input, T& output) { ::nlohmann::from_json(input, output); };

            /** @brief Return JSON field order for reflected @p member. */
            consteval int OrderOf(std::meta::info member) {
                if ($::Has<$::Serialization::Order>(member)) {
                    return $::GetSingle<$::Serialization::Order>(member).priority;
                }
                return 0;
            }

            /** @brief Return the JSON field name for reflected member @p M. */
            template<std::meta::info M>
            consteval std::string_view FieldNameOf() {
                std::string_view name = Sora::Meta::IdentifierOrDisplayStringOf(M);
                template for (constexpr auto a : std::define_static_array(std::meta::annotations_of(M))) {
                    using A = typename [:std::meta::type_of(a):];
                    if constexpr (requires { A::name; }) {
                        name = A::name;
                    }
                }
                return name;
            }

            /** @brief Return JSON-participating data members after ignore/key filtering and order sorting. */
            template<typename T>
            consteval auto JsonMembers() {
                std::vector<std::meta::info> selected;
                bool whitelist = false;
                template for (constexpr auto m : Traits::DataMembers<T>) {
                    if constexpr ($::Has<$::Serialization::Key>(m)) {
                        whitelist = true;
                    }
                }
                template for (constexpr auto m : Traits::DataMembers<T>) {
                    if constexpr (!$::Has<$::Serialization::Ignore>(m)) {
                        if (!whitelist || $::Has<$::Serialization::Key>(m)) {
                            selected.push_back(m);
                        }
                    }
                }
                for (std::size_t i = 1; i < selected.size(); ++i) {
                    auto current = selected[i];
                    std::size_t j = i;
                    while (j > 0 && OrderOf(selected[j - 1]) > OrderOf(current)) {
                        selected[j] = selected[j - 1];
                        --j;
                    }
                    selected[j] = current;
                }
                return std::define_static_array(selected);
            }

            /** @brief Return whether type @p T emits default-valued fields unless a member overrides it. */
            template<typename T>
            consteval bool TypeEmitsDefaults() {
                return $::Has<$::Serialization::EmitDefault>(^^T)
                           ? $::GetSingle<$::Serialization::EmitDefault>(^^T).emit
                           : true;
            }

        } // namespace Detail

        /** @brief Convert enum @p value to reflected-name JSON without consulting integer policy annotations. */
        template<typename E>
        [[nodiscard]] json EnumToNameJson(E value) {
            using U = std::underlying_type_t<E>;
            if constexpr (Concept::BitfieldEnum<E>) {
                auto remaining = static_cast<U>(value);
                std::string out;
                template for (constexpr auto e : Meta::EnumeratorsOf(std::meta::dealias(^^E))) {
                    constexpr auto flag = static_cast<U>([:e:]);
                    if constexpr (flag != U{0}) {
                        if ((remaining & flag) == flag) {
                            if (!out.empty()) {
                                out += '|';
                            }
                            out += Meta::IdentifierOf(e);
                            remaining &= static_cast<U>(~flag);
                        }
                    }
                }
                return json(out.empty() ? "0" : out);
            } else {
                template for (constexpr auto e : Meta::EnumeratorsOf(std::meta::dealias(^^E))) {
                    if ([:e:] == value) {
                        return json(std::string(Meta::IdentifierOf(e)));
                    }
                }
                return json(static_cast<U>(value));
            }
        }

        /** @brief Convert enum @p value to JSON according to enum annotations. */
        template<typename E>
        [[nodiscard]] json EnumToJson(E value) {
            static_assert(!($::Has<$::Serialization::AsInt>(^^E) && $::Has<$::Serialization::AsString>(^^E)),
                          "Enum JSON policy cannot be both AsInt and AsString");
            using U = std::underlying_type_t<E>;
            if constexpr ($::Has<$::Serialization::AsInt>(^^E)) {
                return json(static_cast<U>(value));
            } else {
                return EnumToNameJson(value);
            }
        }

        /** @brief Parse enum @p output from a JSON string or integer. */
        template<typename E>
        void EnumFromJson(const json& input, E& output) {
            using U = std::underlying_type_t<E>;
            if (input.is_number_integer()) {
                output = static_cast<E>(input.template get<U>());
                return;
            }
            if (!input.is_string()) {
                throw nlohmann::json::type_error::create(302, "expected string or integer for enum", &input);
            }

            std::string text = input.template get<std::string>();
            std::string_view sv{text};
            if constexpr (Concept::BitfieldEnum<E>) {
                U acc{};
                std::size_t pos = 0;
                while (pos <= sv.size()) {
                    std::size_t bar = sv.find('|', pos);
                    if (bar == std::string_view::npos) {
                        bar = sv.size();
                    }
                    auto token = sv.substr(pos, bar - pos);
                    while (!token.empty() && token.front() == ' ') {
                        token.remove_prefix(1);
                    }
                    while (!token.empty() && token.back() == ' ') {
                        token.remove_suffix(1);
                    }
                    if (!token.empty() && token != "0") {
                        bool matched = false;
                        template for (constexpr auto e : Meta::EnumeratorsOf(std::meta::dealias(^^E))) {
                            if (!matched && token == Meta::IdentifierOf(e)) {
                                acc |= static_cast<U>([:e:]);
                                matched = true;
                            }
                        }
                        if (!matched) {
                            throw nlohmann::json::other_error::create(
                                501, "unknown enumerator '" + std::string(token) + "'", &input);
                        }
                    }
                    pos = bar + 1;
                }
                output = static_cast<E>(acc);
            } else {
                bool matched = false;
                template for (constexpr auto e : Meta::EnumeratorsOf(std::meta::dealias(^^E))) {
                    if (!matched && sv == Meta::IdentifierOf(e)) {
                        output = [:e:];
                        matched = true;
                    }
                }
                if (!matched) {
                    throw nlohmann::json::other_error::create(501, "unknown enumerator '" + std::string(sv) + "'",
                                                              &input);
                }
            }
        }

        /** @brief Convert reflectable class @p value to a JSON object. */
        template<typename T>
        [[nodiscard]] json ClassToJson(const T& value) {
            json object = json::object();
            template for (constexpr auto m : Detail::JsonMembers<T>()) {
                using Member = typename [:std::meta::type_of(m):];
                const auto& field = value.[:m:];

                if constexpr ($::Has<$::Serialization::Flatten>(m)) {
                    json sub = Detail::ToJsonImpl(field);
                    if (sub.is_object()) {
                        for (auto it = sub.begin(); it != sub.end(); ++it) {
                            object[it.key()] = std::move(it.value());
                        }
                    } else {
                        object[std::string(Detail::FieldNameOf<m>())] = std::move(sub);
                    }
                } else {
                    constexpr bool memberEmitDefault = [] consteval {
                        if constexpr ($::Has<$::Serialization::EmitDefault>(m)) {
                            return $::GetSingle<$::Serialization::EmitDefault>(m).emit;
                        } else {
                            return Detail::TypeEmitsDefaults<T>();
                        }
                    }();
                    if constexpr (!memberEmitDefault && std::equality_comparable<Member>) {
                        if (field == Member{}) {
                            continue;
                        }
                    }
                    if constexpr (std::is_enum_v<Member> && $::Has<$::Serialization::AsInt>(m)) {
                        static_assert(!$::Has<$::Serialization::AsString>(m),
                                      "Enum member JSON policy cannot be both AsInt and AsString");
                        object[std::string(Detail::FieldNameOf<m>())] =
                            static_cast<std::underlying_type_t<Member>>(field);
                    } else if constexpr (std::is_enum_v<Member> && $::Has<$::Serialization::AsString>(m)) {
                        object[std::string(Detail::FieldNameOf<m>())] = EnumToNameJson(field);
                    } else {
                        object[std::string(Detail::FieldNameOf<m>())] = Detail::ToJsonImpl(field);
                    }
                }
            }
            return object;
        }

        /** @brief Parse reflectable class @p output from JSON object @p input. */
        template<typename T>
        void ClassFromJson(const json& input, T& output) {
            if (!input.is_object()) {
                throw nlohmann::json::type_error::create(302, "expected JSON object", &input);
            }
            template for (constexpr auto m : Detail::JsonMembers<T>()) {
                using Member = typename [:std::meta::type_of(m):];
                Member& field = output.[:m:];

                if constexpr ($::Has<$::Serialization::Flatten>(m)) {
                    Detail::FromJsonImpl(input, field);
                } else {
                    auto key = std::string(Detail::FieldNameOf<m>());
                    auto it = input.find(key);
                    if (it == input.end()) {
                        if constexpr ($::Has<$::Serialization::Required>(m)) {
                            throw nlohmann::json::out_of_range::create(403, "missing required key '" + key + "'",
                                                                       &input);
                        }
                    } else if constexpr (std::is_enum_v<Member> && $::Has<$::Serialization::AsInt>(m)) {
                        static_assert(!$::Has<$::Serialization::AsString>(m),
                                      "Enum member JSON policy cannot be both AsInt and AsString");
                        field = static_cast<Member>(it->template get<std::underlying_type_t<Member>>());
                    } else if constexpr (std::is_enum_v<Member> && $::Has<$::Serialization::AsString>(m)) {
                        EnumFromJson(*it, field);
                    } else {
                        Detail::FromJsonImpl(*it, field);
                    }
                }
            }
        }

        namespace Detail {

            template<typename T>
            [[nodiscard]] json ToJsonImpl(T&& value) {
                using U = std::remove_cvref_t<T>;
                if constexpr (HasHookToJson<U>) {
                    return Hook::ToJsonHook<U>::ToJson(value);
                } else if constexpr (ADL::HasFreeToJson<U>) {
                    return ToJson(std::forward<T>(value));
                } else if constexpr (HasMemberToJson<U>) {
                    return value.ToJson();
                } else if constexpr (std::is_null_pointer_v<U>) {
                    return json(nullptr);
                } else if constexpr (std::same_as<U, bool> || std::is_arithmetic_v<U>) {
                    return json(value);
                } else if constexpr (std::is_enum_v<U>) {
                    return EnumToJson(value);
                } else if constexpr (std::same_as<U, std::filesystem::path>) {
                    return json(value.generic_string());
                } else if constexpr (Sora::Concept::StringLike<T>) {
                    return json(std::string(std::string_view(value)));
                } else if constexpr (ChronoDuration<U>) {
                    return json(std::chrono::duration_cast<std::chrono::nanoseconds>(value).count());
                } else if constexpr (ChronoTimePoint<U>) {
                    return json(std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count());
                } else if constexpr (Sora::Concept::OptionalLike<U>) {
                    return value.has_value() ? ToJsonImpl(*value) : json(nullptr);
                } else if constexpr (Concept::VariantLikeClass<U>) {
                    return std::visit([](const auto& alternative) -> json { return ToJsonImpl(alternative); }, value);
                } else if constexpr (Sora::Concept::ByteRange<U>) {
                    json array = json::array();
                    for (auto byte : value) {
                        array.push_back(static_cast<std::uint8_t>(byte));
                    }
                    return array;
                } else if constexpr (StringKeyedAssociative<U>) {
                    json object = json::object();
                    for (const auto& [key, mapped] : value) {
                        object[std::string(std::string_view(key))] = ToJsonImpl(mapped);
                    }
                    return object;
                } else if constexpr (Concept::TupleLikeClass<U> && !std::ranges::range<U>) {
                    json array = json::array();
                    std::apply([&](const auto&... elems) { (array.push_back(ToJsonImpl(elems)), ...); }, value);
                    return array;
                } else if constexpr (std::ranges::range<U>) {
                    json array = json::array();
                    for (const auto& element : value) {
                        array.push_back(ToJsonImpl(element));
                    }
                    return array;
                } else if constexpr (ReflectableClass<U>) {
                    if constexpr (HasNlohmannToJson<U>) {
                        return json(value);
                    } else {
                        return ClassToJson(value);
                    }
                } else if constexpr (HasNlohmannToJson<U>) {
                    return json(value);
                } else {
                    static_assert(false, "Type cannot be serialized to JSON");
                }
            }

            template<typename T>
            void FromJsonImpl(const json& input, T& output) {
                using U = std::remove_cvref_t<T>;
                if constexpr (HasHookFromJson<U>) {
                    Hook::ToJsonHook<U>::FromJson(input, output);
                } else if constexpr (ADL::HasFreeFromJson<U>) {
                    FromJson(input, output);
                } else if constexpr (HasMemberFromJson<U>) {
                    output = U::FromJson(input);
                } else if constexpr (std::is_null_pointer_v<U>) {
                    if (!input.is_null()) {
                        throw nlohmann::json::type_error::create(302, "expected null", &input);
                    }
                } else if constexpr (std::same_as<U, bool>) {
                    output = input.template get<bool>();
                } else if constexpr (std::is_arithmetic_v<U>) {
                    output = input.template get<U>();
                } else if constexpr (std::is_enum_v<U>) {
                    EnumFromJson(input, output);
                } else if constexpr (std::same_as<U, std::string>) {
                    output = input.template get<std::string>();
                } else if constexpr (std::same_as<U, std::filesystem::path>) {
                    output = std::filesystem::path(input.template get<std::string>());
                } else if constexpr (ChronoDuration<U>) {
                    output =
                        std::chrono::duration_cast<U>(std::chrono::nanoseconds(input.template get<std::int64_t>()));
                } else if constexpr (ChronoTimePoint<U>) {
                    output = U(std::chrono::duration_cast<typename U::duration>(
                        std::chrono::nanoseconds(input.template get<std::int64_t>())));
                } else if constexpr (Sora::Concept::OptionalLike<U>) {
                    if (input.is_null()) {
                        output.reset();
                    } else {
                        typename U::value_type temp{};
                        FromJsonImpl(input, temp);
                        output = std::move(temp);
                    }
                } else if constexpr (Concept::VariantLikeClass<U>) {
                    bool matched = false;
                    [&]<std::size_t... I>(std::index_sequence<I...>) {
                        (
                            [&] {
                                if (matched) {
                                    return;
                                }
                                try {
                                    std::variant_alternative_t<I, U> alternative{};
                                    FromJsonImpl(input, alternative);
                                    output = std::move(alternative);
                                    matched = true;
                                } catch (...) {
                                }
                            }(),
                            ...);
                    }(std::make_index_sequence<std::variant_size_v<U>>{});
                    if (!matched) {
                        throw nlohmann::json::type_error::create(302, "no variant alternative matched", &input);
                    }
                } else if constexpr (Sora::Concept::ByteRange<U>) {
                    output.clear();
                    for (const auto& element : input) {
                        output.push_back(
                            static_cast<std::ranges::range_value_t<U>>(element.template get<std::uint8_t>()));
                    }
                } else if constexpr (StringKeyedAssociative<U>) {
                    output.clear();
                    for (auto it = input.begin(); it != input.end(); ++it) {
                        typename U::mapped_type mapped{};
                        FromJsonImpl(it.value(), mapped);
                        output.emplace(typename U::key_type(it.key()), std::move(mapped));
                    }
                } else if constexpr (Concept::TupleLikeClass<U> && !std::ranges::range<U>) {
                    if (!input.is_array() || input.size() != std::tuple_size_v<U>) {
                        throw nlohmann::json::type_error::create(302, "expected fixed-size JSON array for tuple",
                                                                 &input);
                    }
                    [&]<std::size_t... I>(std::index_sequence<I...>) {
                        (FromJsonImpl(input[I], std::get<I>(output)), ...);
                    }(std::make_index_sequence<std::tuple_size_v<U>>{});
                } else if constexpr (std::ranges::range<U>) {
                    output.clear();
                    for (const auto& element : input) {
                        typename U::value_type temp{};
                        FromJsonImpl(element, temp);
                        output.push_back(std::move(temp));
                    }
                } else if constexpr (ReflectableClass<U>) {
                    if constexpr (HasNlohmannFromJson<U>) {
                        output = input.template get<U>();
                    } else {
                        ClassFromJson(input, output);
                    }
                } else if constexpr (HasNlohmannFromJson<U>) {
                    output = input.template get<U>();
                } else {
                    static_assert(false, "Type cannot be deserialized from JSON");
                }
            }

            /** @brief CPO functor that implements @c ToJson(value). */
            struct ToJsonFn {
                template<typename T>
                [[nodiscard]] json operator()(T&& value) const {
                    return ToJsonImpl(std::forward<T>(value));
                }
            };

        } // namespace Detail

    } // namespace Json

    namespace Json {

        /** @brief Customisation-point object that converts supported values to @c Sora::json. */
        inline constexpr Json::Detail::ToJsonFn ToJson{};

        /** @brief Parse JSON into @p T and return the parsed value. */
        template<typename T>
        [[nodiscard]] T FromJson(const json& input) {
            T output{};
            Json::Detail::FromJsonImpl(input, output);
            return output;
        }

        /** @brief Parse JSON into existing object @p output. */
        template<typename T>
        void FromJson(const json& input, T& output) {
            Json::Detail::FromJsonImpl(input, output);
        }

    } // namespace Json

    using Json::FromJson;
    using Json::ToJson;

} // namespace Sora

namespace nlohmann {

    /** @brief Bridge nlohmann/json ADL serialization to Sora reflection-driven JSON conversion. */
    template<typename T>
        requires(Sora::Json::Detail::ReflectableClass<T> || std::is_enum_v<T>) &&
                (!Sora::Json::Detail::HasNlohmannToJson<T>)
    struct adl_serializer<T> {
        static void to_json(Sora::json& output, const T& value) { output = Sora::ToJson(value); }
        static void from_json(const Sora::json& input, T& output) { Sora::FromJson(input, output); }
    };

} // namespace nlohmann

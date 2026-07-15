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

#include <Sora/Core/ToString.h>
#include <Sora/Core/Traits.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <meta>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace Sora {

    /** @brief The JSON value type used by Sora. */
    using Json = nlohmann::json;

    /** @brief Insertion-order-preserving JSON value type used for golden-file output. */
    using OrderedJson = nlohmann::ordered_json;

    namespace $::Serialization {

        /** @brief Whitelist mode: when any member carries this annotation, only keyed members participate. */
        struct Key {
            constexpr bool operator==(const Key&) const = default;
        };

        /** @brief JSON deserialization policy: a missing key is an error instead of preserving the current value. */
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
         * A specialisation provides @c static @c Json @c ToJson(const @c T&) and may also provide
         * @c static @c void @c FromJson(const @c Json&, @c T&). The primary template is intentionally undefined.
         *
         * @tparam T Cv-unqualified type to customise.
         */
        template<typename T>
        struct ToJsonHook;

    } // namespace Hook

    namespace Meta {

        /** @brief Return whether reflected @p type is a @c std::chrono::duration specialization. */
        consteval bool IsChronoDuration(std::meta::info type) {
            type = std::meta::dealias(std::meta::remove_cvref(type));
            return std::meta::is_class_type(type) && std::meta::has_template_arguments(type) &&
                   std::meta::template_of(type) == ^^std::chrono::duration;
        }

        /** @brief Return whether reflected @p type is a @c std::chrono::time_point specialization. */
        consteval bool IsChronoTimePoint(std::meta::info type) {
            type = std::meta::dealias(std::meta::remove_cvref(type));
            return std::meta::is_class_type(type) && std::meta::has_template_arguments(type) &&
                   std::meta::template_of(type) == ^^std::chrono::time_point;
        }

        /** @brief Return JSON field order for reflected @p member. */
        consteval int JsonMemberOrderOf(std::meta::info member) {
            if ($::Has<$::Serialization::Order>(member)) {
                return $::GetSingle<$::Serialization::Order>(member).priority;
            }
            return 0;
        }

        /** @brief Return JSON-participating data members after ignore/key filtering and order sorting. */
        template<typename T>
        consteval auto JsonSerializableMembersOf() {
            std::vector<std::meta::info> selected;
            bool whitelist = false;
            template for (constexpr auto member : Sora::Traits::DataMembers<T>) {
                if constexpr ($::Has<$::Serialization::Key>(member)) {
                    whitelist = true;
                }
            }
            template for (constexpr auto member : Sora::Traits::DataMembers<T>) {
                if constexpr (!$::Has<$::Serialization::Ignore>(member)) {
                    if (!whitelist || $::Has<$::Serialization::Key>(member)) {
                        selected.push_back(member);
                    }
                }
            }
            for (std::size_t i = 1; i < selected.size(); ++i) {
                auto current = selected[i];
                std::size_t j = i;
                while (j > 0 && JsonMemberOrderOf(selected[j - 1]) > JsonMemberOrderOf(current)) {
                    selected[j] = selected[j - 1];
                    --j;
                }
                selected[j] = current;
            }
            return std::define_static_array(selected);
        }

        /** @brief Return whether type @p T emits default-valued fields unless a member overrides it. */
        template<typename T>
        consteval bool JsonTypeEmitsDefaults() {
            return $::Has<$::Serialization::EmitDefault>(^^T) ? $::GetSingle<$::Serialization::EmitDefault>(^^T).emit
                                                              : true;
        }

    } // namespace Meta

    /** @cond INTERNAL */
    namespace Detail::Json {

        template<typename T>
        [[nodiscard]] Sora::Json ToJsonImpl(T&& value);

        template<typename T>
        void FromJsonImpl(const Sora::Json& input, T& output);

        namespace ADL {

            /** @brief Poison pill for unqualified JSON emission lookup. */
            void ToJson() = delete;

            /** @brief Poison pill for unqualified JSON parse lookup. */
            void FromJson() = delete;

            template<typename T>
            concept HasFreeToJson = requires(T&& value) {
                { ToJson(std::forward<T>(value)) } -> std::convertible_to<Sora::Json>;
            };

            template<typename T>
            concept HasFreeFromJson = requires(const Sora::Json& input, T& output) {
                { FromJson(input, output) } -> std::same_as<void>;
            };

        } // namespace ADL

        template<typename T>
        concept HasMemberToJson = requires(const T& value) {
            { value.ToJson() } -> std::convertible_to<Sora::Json>;
        };

        template<typename T>
        concept HasMemberFromJson = requires(const Sora::Json& input) {
            { T::FromJson(input) } -> std::convertible_to<T>;
        };

        template<typename T>
        concept HasHookToJson = requires(const T& value) {
            { Hook::ToJsonHook<T>::ToJson(value) } -> std::convertible_to<Sora::Json>;
        };

        template<typename T>
        concept HasHookFromJson = requires(const Sora::Json& input, T& output) {
            { Hook::ToJsonHook<T>::FromJson(input, output) } -> std::same_as<void>;
        };

    } // namespace Detail::Json
    /** @endcond */

    namespace Concept {

        /** @brief Type representing a duration through @c std::chrono::duration. */
        template<typename T>
        concept ChronoDuration = Meta::IsChronoDuration(^^std::remove_cvref_t<T>);

        /** @brief Type representing a time point through @c std::chrono::time_point. */
        template<typename T>
        concept ChronoTimePoint = Meta::IsChronoTimePoint(^^std::remove_cvref_t<T>);

        /** @brief Range of JSON key-value entries whose keys are convertible to @c std::string_view. */
        template<typename T>
        concept JsonStringKeyedAssociative =
            std::ranges::range<T> && requires(std::ranges::range_reference_t<T> entry) {
                entry.first;
                entry.second;
                std::string_view(entry.first);
            };

        /** @brief Complete class represented structurally as a JSON object by C++26 reflection. */
        template<typename T>
        concept JsonReflectableClass =
            std::is_class_v<std::remove_cvref_t<T>> && !std::is_union_v<std::remove_cvref_t<T>> &&
            !std::same_as<std::remove_cvref_t<T>, std::filesystem::path> && !StringLike<T> && !ChronoDuration<T> &&
            !ChronoTimePoint<T> && !OptionalLike<T> && !VariantLikeClass<std::remove_cvref_t<T>> &&
            !TupleLikeClass<std::remove_cvref_t<T>> && !std::ranges::range<T>;

        /** @brief Type with a directly callable nlohmann @c to_json customization. */
        template<typename T>
        concept NlohmannJsonSerializable = requires(const std::remove_cvref_t<T>& value) { Sora::Json(value); };

        /** @brief Type with a directly callable nlohmann @c from_json customization. */
        template<typename T>
        concept NlohmannJsonDeserializable = requires(const Sora::Json& input, std::remove_cvref_t<T>& output) {
            { input.get_to(output) } -> std::same_as<std::remove_cvref_t<T>&>;
        };

        /** @brief Type explicitly customizing JSON serialization through a hook, ADL, or member function. */
        template<typename T>
        concept JsonCustomSerializable =
            Detail::Json::HasHookToJson<std::remove_cvref_t<T>> || Detail::Json::ADL::HasFreeToJson<T> ||
            Detail::Json::HasMemberToJson<std::remove_cvref_t<T>>;

        /** @brief Type explicitly customizing JSON deserialization through a hook, ADL, or static member. */
        template<typename T>
        concept JsonCustomDeserializable = Detail::Json::HasHookFromJson<std::remove_cvref_t<T>> ||
                                           Detail::Json::ADL::HasFreeFromJson<std::remove_cvref_t<T>> ||
                                           Detail::Json::HasMemberFromJson<std::remove_cvref_t<T>>;

    } // namespace Concept

    /** @cond INTERNAL */
    namespace Detail::Json {

        /** @brief Convert enum @p value to reflected-name JSON without consulting integer policy annotations. */
        template<typename E>
        [[nodiscard]] Sora::Json EnumToNameJson(E value) {
            using U = std::underlying_type_t<E>;
            if constexpr (Sora::Concept::BitfieldEnum<E>) {
                auto remaining = static_cast<U>(value);
                std::string out;
                template for (constexpr auto e : Sora::Meta::EnumeratorsOf(std::meta::dealias(^^E))) {
                    constexpr auto flag = static_cast<U>([:e:]);
                    if constexpr (flag != U{0}) {
                        if ((remaining & flag) == flag) {
                            if (!out.empty()) {
                                out += '|';
                            }
                            out += Sora::Meta::IdentifierOf(e);
                            remaining &= static_cast<U>(~flag);
                        }
                    }
                }
                return Sora::Json(out.empty() ? "0" : out);
            } else {
                template for (constexpr auto e : Sora::Meta::EnumeratorsOf(std::meta::dealias(^^E))) {
                    if ([:e:] == value) {
                        return Sora::Json(std::string(Sora::Meta::IdentifierOf(e)));
                    }
                }
                return Sora::Json(static_cast<U>(value));
            }
        }

        /** @brief Convert enum @p value to JSON according to enum annotations. */
        template<typename E>
        [[nodiscard]] Sora::Json EnumToJson(E value) {
            static_assert(!($::Has<$::Serialization::AsInt>(^^E) && $::Has<$::Serialization::AsString>(^^E)),
                          "Enum JSON policy cannot be both AsInt and AsString");
            using U = std::underlying_type_t<E>;
            if constexpr ($::Has<$::Serialization::AsInt>(^^E)) {
                return Sora::Json(static_cast<U>(value));
            } else {
                return EnumToNameJson(value);
            }
        }

        /** @brief Parse enum @p output from a JSON string or integer. */
        template<typename E>
        void EnumFromJson(const Sora::Json& input, E& output) {
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
            if constexpr (Sora::Concept::BitfieldEnum<E>) {
                U acc{};
                std::size_t pos = 0;
                while (pos <= sv.size()) {
                    std::size_t bar = sv.find('|', pos);
                    if (bar == std::string_view::npos) {
                        bar = sv.size();
                    }
                    auto token = Ascii::Trim(sv.substr(pos, bar - pos));
                    if (!token.empty() && token != "0") {
                        bool matched = false;
                        template for (constexpr auto e : Sora::Meta::EnumeratorsOf(std::meta::dealias(^^E))) {
                            if (!matched && token == Sora::Meta::IdentifierOf(e)) {
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
                template for (constexpr auto e : Sora::Meta::EnumeratorsOf(std::meta::dealias(^^E))) {
                    if (!matched && sv == Sora::Meta::IdentifierOf(e)) {
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
        [[nodiscard]] Sora::Json ClassToJson(const T& value) {
            Sora::Json object = Sora::Json::object();
            template for (constexpr auto m : Meta::JsonSerializableMembersOf<T>()) {
                using Member = typename [:std::meta::type_of(m):];
                const auto& field = value.[:m:];

                if constexpr ($::Has<$::Serialization::Flatten>(m)) {
                    Sora::Json sub = ToJsonImpl(field);
                    if (sub.is_object()) {
                        for (auto it = sub.begin(); it != sub.end(); ++it) {
                            object[it.key()] = std::move(it.value());
                        }
                    } else {
                        object[std::string(Sora::Meta::SerializationFieldNameOf<m>())] = std::move(sub);
                    }
                } else {
                    constexpr bool memberEmitDefault = [] consteval {
                        if constexpr ($::Has<$::Serialization::EmitDefault>(m)) {
                            return $::GetSingle<$::Serialization::EmitDefault>(m).emit;
                        } else {
                            return Meta::JsonTypeEmitsDefaults<T>();
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
                        object[std::string(Sora::Meta::SerializationFieldNameOf<m>())] =
                            static_cast<std::underlying_type_t<Member>>(field);
                    } else if constexpr (std::is_enum_v<Member> && $::Has<$::Serialization::AsString>(m)) {
                        object[std::string(Sora::Meta::SerializationFieldNameOf<m>())] = EnumToNameJson(field);
                    } else {
                        object[std::string(Sora::Meta::SerializationFieldNameOf<m>())] = ToJsonImpl(field);
                    }
                }
            }
            return object;
        }

        /** @brief Parse reflectable class @p output from JSON object @p input. */
        template<typename T>
        void ClassFromJson(const Sora::Json& input, T& output) {
            if (!input.is_object()) {
                throw nlohmann::json::type_error::create(302, "expected JSON object", &input);
            }
            template for (constexpr auto m : Meta::JsonSerializableMembersOf<T>()) {
                using Member = typename [:std::meta::type_of(m):];
                Member& field = output.[:m:];

                if constexpr ($::Has<$::Serialization::Flatten>(m)) {
                    FromJsonImpl(input, field);
                } else {
                    auto key = std::string(Sora::Meta::SerializationFieldNameOf<m>());
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
                        FromJsonImpl(*it, field);
                    }
                }
            }
        }

        template<std::size_t I = 0, typename Variant>
        [[nodiscard]] bool TryVariantFromJson(const Sora::Json& input, Variant& output) {
            if constexpr (I == std::variant_size_v<Variant>) {
                return false;
            } else {
                using Alternative = std::variant_alternative_t<I, Variant>;
                if constexpr (std::default_initializable<Alternative>) {
                    try {
                        Alternative alternative{};
                        FromJsonImpl(input, alternative);
                        output = std::move(alternative);
                        return true;
                    } catch (const nlohmann::json::exception&) {
                        return false;
                    }
                }
                return TryVariantFromJson<I + 1>(input, output);
            }
        }

        template<typename T>
        [[nodiscard]] Sora::Json ToJsonImpl(T&& value) {
            using U = std::remove_cvref_t<T>;
            if constexpr (HasHookToJson<U>) {
                return Hook::ToJsonHook<U>::ToJson(value);
            } else if constexpr (ADL::HasFreeToJson<T>) {
                return ToJson(std::forward<T>(value));
            } else if constexpr (HasMemberToJson<U>) {
                return value.ToJson();
            } else if constexpr (std::is_null_pointer_v<U>) {
                return Sora::Json(nullptr);
            } else if constexpr (std::same_as<U, bool> || std::is_arithmetic_v<U>) {
                return Sora::Json(value);
            } else if constexpr (std::is_enum_v<U>) {
                return EnumToJson(value);
            } else if constexpr (std::same_as<U, std::filesystem::path>) {
                return Sora::Json(Sora::ToString(value));
            } else if constexpr (Sora::Concept::NarrowStringLike<T> || Sora::Concept::Utf8StringLike<T> ||
                                 Sora::Concept::Utf16StringLike<T> || Sora::Concept::Utf32StringLike<T> ||
                                 Sora::Concept::WideStringLike<T>) {
                return Sora::Json(Sora::ToString(std::forward<T>(value)));
            } else if constexpr (Concept::ChronoDuration<U>) {
                return Sora::Json(std::chrono::duration_cast<std::chrono::nanoseconds>(value).count());
            } else if constexpr (Concept::ChronoTimePoint<U>) {
                return Sora::Json(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count());
            } else if constexpr (Sora::Concept::OptionalLike<U>) {
                return value.has_value() ? ToJsonImpl(*value) : Sora::Json(nullptr);
            } else if constexpr (Sora::Concept::VariantLikeClass<U>) {
                return std::visit([](const auto& alternative) -> Sora::Json { return ToJsonImpl(alternative); }, value);
            } else if constexpr (Sora::Concept::ByteRange<U>) {
                Sora::Json array = Sora::Json::array();
                for (auto byte : value) {
                    array.push_back(static_cast<std::uint8_t>(byte));
                }
                return array;
            } else if constexpr (Concept::JsonStringKeyedAssociative<U>) {
                Sora::Json object = Sora::Json::object();
                for (const auto& [key, mapped] : value) {
                    object[std::string(std::string_view(key))] = ToJsonImpl(mapped);
                }
                return object;
            } else if constexpr (Sora::Concept::TupleLikeClass<U> && !std::ranges::range<U>) {
                Sora::Json array = Sora::Json::array();
                std::apply([&](const auto&... elems) { (array.push_back(ToJsonImpl(elems)), ...); }, value);
                return array;
            } else if constexpr (std::ranges::range<U>) {
                Sora::Json array = Sora::Json::array();
                for (const auto& element : value) {
                    array.push_back(ToJsonImpl(element));
                }
                return array;
            } else if constexpr (Concept::JsonReflectableClass<U> || Concept::NlohmannJsonSerializable<U>) {
                if constexpr (Concept::NlohmannJsonSerializable<U>) {
                    return Sora::Json(value);
                } else {
                    return ClassToJson(value);
                }
            } else {
                static_assert(false, "Type cannot be serialized to JSON");
            }
        }

        template<typename T>
        void FromJsonImpl(const Sora::Json& input, T& output) {
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
            } else if constexpr (std::same_as<U, std::u8string> || std::same_as<U, std::u16string> ||
                                 std::same_as<U, std::u32string> || std::same_as<U, std::wstring> ||
                                 std::same_as<U, std::filesystem::path>) {
                const auto& text = input.template get_ref<const std::string&>();
                auto decoded = Sora::FromString(std::in_place_type<U>, text);
                if (!decoded) {
                    throw nlohmann::json::type_error::create(302, "invalid UTF-8 JSON string", &input);
                }
                output = std::move(*decoded);
            } else if constexpr (Concept::ChronoDuration<U>) {
                output = std::chrono::duration_cast<U>(std::chrono::nanoseconds(input.template get<std::int64_t>()));
            } else if constexpr (Concept::ChronoTimePoint<U>) {
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
            } else if constexpr (Sora::Concept::VariantLikeClass<U>) {
                if (!TryVariantFromJson(input, output)) {
                    throw nlohmann::json::type_error::create(302, "no variant alternative matched", &input);
                }
            } else if constexpr (Sora::Concept::ByteRange<U>) {
                if (!input.is_array()) {
                    throw nlohmann::json::type_error::create(302, "expected JSON array for byte range", &input);
                }
                output.clear();
                for (const auto& element : input) {
                    output.push_back(static_cast<std::ranges::range_value_t<U>>(element.template get<std::uint8_t>()));
                }
            } else if constexpr (Concept::JsonStringKeyedAssociative<U>) {
                if (!input.is_object()) {
                    throw nlohmann::json::type_error::create(302, "expected JSON object for string-keyed map", &input);
                }
                output.clear();
                for (auto it = input.begin(); it != input.end(); ++it) {
                    typename U::mapped_type mapped{};
                    FromJsonImpl(it.value(), mapped);
                    output.emplace(typename U::key_type(it.key()), std::move(mapped));
                }
            } else if constexpr (Sora::Concept::TupleLikeClass<U> && !std::ranges::range<U>) {
                if (!input.is_array() || input.size() != std::tuple_size_v<U>) {
                    throw nlohmann::json::type_error::create(302, "expected fixed-size JSON array for tuple", &input);
                }
                [&]<std::size_t... I>(std::index_sequence<I...>) {
                    (FromJsonImpl(input[I], std::get<I>(output)), ...);
                }(std::make_index_sequence<std::tuple_size_v<U>>{});
            } else if constexpr (std::ranges::range<U>) {
                if (!input.is_array()) {
                    throw nlohmann::json::type_error::create(302, "expected JSON array for range", &input);
                }
                output.clear();
                for (const auto& element : input) {
                    typename U::value_type temp{};
                    FromJsonImpl(element, temp);
                    output.push_back(std::move(temp));
                }
            } else if constexpr (Concept::JsonReflectableClass<U> || Concept::NlohmannJsonDeserializable<U>) {
                if constexpr (Concept::NlohmannJsonDeserializable<U>) {
                    input.get_to(output);
                } else {
                    ClassFromJson(input, output);
                }
            } else {
                static_assert(false, "Type cannot be deserialized from JSON");
            }
        }

    } // namespace Detail::Json
    /** @endcond */

    namespace CPO {

        /** @brief Function object implementing the @ref Sora::ToJson customization point. */
        struct ToJsonFn {
            template<typename T>
            [[nodiscard]] Json operator()(T&& value) const {
                return Detail::Json::ToJsonImpl(std::forward<T>(value));
            }
        };

        /** @brief Function object implementing the @ref Sora::FromJson customization point. */
        struct FromJsonFn {
            template<std::default_initializable T>
            [[nodiscard]] T operator()(std::in_place_type_t<T>, const Json& input) const {
                T output{};
                Detail::Json::FromJsonImpl(input, output);
                return output;
            }

            template<typename T>
            void operator()(const Json& input, T& output) const {
                Detail::Json::FromJsonImpl(input, output);
            }
        };

    } // namespace CPO

    /** @brief Customisation-point object that converts supported values to @c Sora::Json. */
    inline constexpr CPO::ToJsonFn ToJson{};

    /** @brief Customisation-point object that deserializes JSON into an explicitly tagged or existing value. */
    inline constexpr CPO::FromJsonFn FromJson{};

} // namespace Sora

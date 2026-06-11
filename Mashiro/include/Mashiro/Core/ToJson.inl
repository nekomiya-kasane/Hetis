/**
 * @file ToJson.inl
 * @brief Implementation of @ref Mashiro::ToJson and @ref Mashiro::FromJson.
 *
 * Included from @ref ToJson.h. Split out so the public header stays
 * navigable.
 *
 * @ingroup Core
 */
#pragma once

// clang-format off

namespace Mashiro {

    /** @cond INTERNAL */
    namespace Json::Detail {

        // Forward declarations.
        template <typename T> [[nodiscard]] json ToJsonImpl(T&& iValue);
        template <typename T> void FromJsonImpl(const json& iJson, T& oValue);

        // -------------------------------------------------------------------
        // Enum dispatch — name(s) ↔ integer
        // -------------------------------------------------------------------

        template <typename E>
        consteval bool EmitEnumAsInt() {
            return Has<Anno::AsInt>(^^E);
        }

        template <typename E>
        [[nodiscard]] json EnumToJson(E iValue) {
            using U = std::underlying_type_t<E>;
            if constexpr (EmitEnumAsInt<E>()) {
                return json(static_cast<U>(iValue));
            } else if constexpr (Traits::BitfieldEnum<E>) {
                // Decompose value into '|' separated names.
                auto remaining = static_cast<U>(iValue);
                std::string out;
                template for (constexpr auto e :
                              std::define_static_array(
                                  std::meta::enumerators_of(std::meta::dealias(^^E)))) {
                    constexpr auto flag = static_cast<U>([:e:]);
                    if constexpr (flag != U{0}) {
                        if ((remaining & flag) == flag) {
                            if (!out.empty()) out += '|';
                            out += std::meta::identifier_of(e);
                            remaining &= static_cast<U>(~flag);
                        }
                    }
                }
                if (out.empty()) out = "0";
                return json(out);
            } else {
                template for (constexpr auto e :
                              std::define_static_array(
                                  std::meta::enumerators_of(std::meta::dealias(^^E)))) {
                    if ([:e:] == iValue) {
                        return json(std::string(std::meta::identifier_of(e)));
                    }
                }
                return json(static_cast<U>(iValue));
            }
        }

        template <typename E>
        void EnumFromJson(const json& iJson, E& oValue) {
            using U = std::underlying_type_t<E>;
            if (iJson.is_number_integer()) {
                oValue = static_cast<E>(iJson.template get<U>());
                return;
            }
            if (!iJson.is_string()) {
                throw nlohmann::json::type_error::create(302, "expected string or integer for enum", &iJson);
            }
            std::string_view sv = iJson.template get<std::string_view>();
            if constexpr (Traits::BitfieldEnum<E>) {
                U acc{};
                size_t pos = 0;
                while (pos <= sv.size()) {
                    size_t bar = sv.find('|', pos);
                    if (bar == std::string_view::npos) bar = sv.size();
                    auto tok = sv.substr(pos, bar - pos);
                    while (!tok.empty() && tok.front() == ' ') tok.remove_prefix(1);
                    while (!tok.empty() && tok.back() == ' ') tok.remove_suffix(1);
                    if (!tok.empty()) {
                        if (tok == "0") {
                            // explicit zero — leave acc untouched
                        } else {
                            bool matched = false;
                            template for (constexpr auto e :
                                          std::define_static_array(
                                              std::meta::enumerators_of(
                                                  std::meta::dealias(^^E)))) {
                                if (!matched && tok == std::meta::identifier_of(e)) {
                                    acc |= static_cast<U>([:e:]);
                                    matched = true;
                                }
                            }
                            if (!matched)
                                throw nlohmann::json::other_error::create(
                                    501,
                                    "unknown enumerator '" + std::string(tok) + "'",
                                    &iJson);
                        }
                    }
                    pos = bar + 1;
                }
                oValue = static_cast<E>(acc);
                return;
            } else {
                bool matched = false;
                template for (constexpr auto e :
                              std::define_static_array(
                                  std::meta::enumerators_of(std::meta::dealias(^^E)))) {
                    if (!matched && sv == std::meta::identifier_of(e)) {
                        oValue = [:e:];
                        matched = true;
                    }
                }
                if (!matched)
                    throw nlohmann::json::other_error::create(
                        501, "unknown enumerator '" + std::string(sv) + "'", &iJson);
            }
        }

        // -------------------------------------------------------------------
        // Class object — reflection-driven NSDM dump
        // -------------------------------------------------------------------

        template <typename T>
        consteval bool TypeEmitsDefaults() {
            auto a = Get<Anno::EmitDefault>(^^T);
            return a ? a->emit : true;
        }

        template <typename T>
        [[nodiscard]] json ClassToJson(const T& iValue) {
            json obj = json::object();
            template for (constexpr auto m :
                          std::define_static_array(JsonMembers<T>())) {
                using MemberT = typename [:std::meta::type_of(m):];
                const auto& field = iValue.[:m:];

                if constexpr (Has<Anno::Flatten>(m)) {
                    json sub = ToJsonImpl(field);
                    if (sub.is_object()) {
                        for (auto it = sub.begin(); it != sub.end(); ++it) {
                            obj[it.key()] = std::move(it.value());
                        }
                    } else {
                        obj[std::string(FieldNameOf<m>())] = std::move(sub);
                    }
                } else {
                    constexpr bool memberEmitDefault =
                        Get<Anno::EmitDefault>(m).value_or(
                            Anno::EmitDefault{TypeEmitsDefaults<T>()}).emit;
                    if constexpr (!memberEmitDefault) {
                        if constexpr (std::equality_comparable<MemberT>) {
                            if (field == MemberT{}) continue;
                        }
                    }
                    if constexpr (std::is_enum_v<MemberT> && Has<Anno::AsInt>(m)) {
                        using U = std::underlying_type_t<MemberT>;
                        obj[std::string(FieldNameOf<m>())] = static_cast<U>(field);
                    } else {
                        obj[std::string(FieldNameOf<m>())] = ToJsonImpl(field);
                    }
                }
            }
            return obj;
        }

        template <typename T>
        void ClassFromJson(const json& iJson, T& oValue) {
            if (!iJson.is_object())
                throw nlohmann::json::type_error::create(
                    302, "expected JSON object", &iJson);
            template for (constexpr auto m :
                          std::define_static_array(JsonMembers<T>())) {
                using MemberT = typename [:std::meta::type_of(m):];
                MemberT& field = oValue.[:m:];

                if constexpr (Has<Anno::Flatten>(m)) {
                    FromJsonImpl(iJson, field);
                } else {
                    auto key = std::string(FieldNameOf<m>());
                    auto it = iJson.find(key);
                    if (it == iJson.end()) {
                        if constexpr (Has<Anno::Required>(m))
                            throw nlohmann::json::out_of_range::create(
                                403, "missing required key '" + key + "'", &iJson);
                        // else: leave at value-init.
                    } else {
                        if constexpr (std::is_enum_v<MemberT> && Has<Anno::AsInt>(m)) {
                            using U = std::underlying_type_t<MemberT>;
                            field = static_cast<MemberT>(it->template get<U>());
                        } else {
                            FromJsonImpl(*it, field);
                        }
                    }
                }
            }
        }

        // -------------------------------------------------------------------
        // Core dispatch
        // -------------------------------------------------------------------

        template <typename T>
        [[nodiscard]] json ToJsonImpl(T&& iValue) {
            using U = std::remove_cvref_t<T>;

            // 1. ADL ToJson(v) — Mashiro hook
            if constexpr (ADL::HasFreeToJson<U>) {
                return ToJson(std::forward<T>(iValue));
            }
            // 2. v.ToJson() — member hook
            else if constexpr (HasMemberToJson<U>) {
                return iValue.ToJson();
            }
            // 3. nullptr
            else if constexpr (std::is_null_pointer_v<U>) {
                return json(nullptr);
            }
            // 4. bool
            else if constexpr (std::same_as<U, bool>) {
                return json(iValue);
            }
            // 5. integral / floating
            else if constexpr (std::is_arithmetic_v<U>) {
                return json(iValue);
            }
            // 6. enum
            else if constexpr (std::is_enum_v<U>) {
                return EnumToJson(iValue);
            }
            // 7. string-like
            else if constexpr (StringLike<U>) {
                return json(std::string(std::string_view(iValue)));
            }
            // 8. filesystem path
            else if constexpr (FilesystemPath<U>) {
                return json(iValue.generic_string());
            }
            // 9. chrono::duration → integral count + unit-tagged object
            else if constexpr (ChronoDuration<U>) {
                using Ns = std::chrono::nanoseconds;
                return json(std::chrono::duration_cast<Ns>(iValue).count());
            }
            // 10. chrono::time_point → ns since epoch
            else if constexpr (ChronoTimePoint<U>) {
                using Ns = std::chrono::nanoseconds;
                return json(
                    std::chrono::duration_cast<Ns>(iValue.time_since_epoch()).count());
            }
            // 11. optional → null or unwrapped
            else if constexpr (IsOptional<U>::value) {
                if (!iValue.has_value()) return json(nullptr);
                return ToJsonImpl(*iValue);
            }
            // 12. variant → visit
            else if constexpr (IsVariant<U>::value) {
                return std::visit(
                    [](const auto& alt) -> json { return ToJsonImpl(alt); }, iValue);
            }
            // 13. byte span → array of small uints
            else if constexpr (std::ranges::range<U> &&
                               ByteSpan<U>) {
                json arr = json::array();
                for (auto b : iValue)
                    arr.push_back(static_cast<uint8_t>(b));
                return arr;
            }
            // 14. associative range with string-like key → object
            else if constexpr (AssociativeRange<U>) {
                json obj = json::object();
                for (const auto& [k, v] : iValue)
                    obj[std::string(std::string_view(k))] = ToJsonImpl(v);
                return obj;
            }
            // 15. tuple-like (must come before generic range so `std::array` keeps its
            //     natural array form, but tuple/pair go through here)
            else if constexpr (Traits::TupleLike<U> && !std::ranges::range<U>) {
                json arr = json::array();
                std::apply(
                    [&](const auto&... elems) {
                        (arr.push_back(ToJsonImpl(elems)), ...);
                    },
                    iValue);
                return arr;
            }
            // 16. range → array
            else if constexpr (std::ranges::range<U>) {
                json arr = json::array();
                for (const auto& e : iValue) arr.push_back(ToJsonImpl(e));
                return arr;
            }
            // 17. reflectable class
            else if constexpr (ReflectableClass<U>) {
                if constexpr (HasNlohmannToJson<U>) {
                    return json(iValue);
                } else {
                    return ClassToJson(iValue);
                }
            }
            // 18. nlohmann's own to_json (e.g. user already wrote one in a 3rd-party lib)
            else if constexpr (HasNlohmannToJson<U>) {
                return json(iValue);
            }
            else {
                static_assert(false, "Type cannot be serialised to JSON");
            }
        }

        template <typename T>
        void FromJsonImpl(const json& iJson, T& oValue) {
            using U = std::remove_cvref_t<T>;

            if constexpr (ADL::HasFreeFromJson<U>) {
                FromJson(iJson, oValue);
            }
            else if constexpr (HasMemberFromJson<U>) {
                oValue = U::FromJson(iJson);
            }
            else if constexpr (std::is_null_pointer_v<U>) {
                if (!iJson.is_null())
                    throw nlohmann::json::type_error::create(302, "expected null", &iJson);
            }
            else if constexpr (std::same_as<U, bool>) {
                oValue = iJson.template get<bool>();
            }
            else if constexpr (std::is_arithmetic_v<U>) {
                oValue = iJson.template get<U>();
            }
            else if constexpr (std::is_enum_v<U>) {
                EnumFromJson(iJson, oValue);
            }
            else if constexpr (std::same_as<U, std::string>) {
                oValue = iJson.template get<std::string>();
            }
            else if constexpr (FilesystemPath<U>) {
                oValue = std::filesystem::path(iJson.template get<std::string>());
            }
            else if constexpr (ChronoDuration<U>) {
                using Ns = std::chrono::nanoseconds;
                oValue = std::chrono::duration_cast<U>(Ns(iJson.template get<int64_t>()));
            }
            else if constexpr (ChronoTimePoint<U>) {
                using Ns = std::chrono::nanoseconds;
                oValue = U(std::chrono::duration_cast<typename U::duration>(
                    Ns(iJson.template get<int64_t>())));
            }
            else if constexpr (IsOptional<U>::value) {
                if (iJson.is_null()) {
                    oValue.reset();
                } else {
                    typename U::value_type tmp{};
                    FromJsonImpl(iJson, tmp);
                    oValue = std::move(tmp);
                }
            }
            else if constexpr (IsVariant<U>::value) {
                // Try alternatives in declaration order; first that succeeds wins.
                bool ok = false;
                [&]<size_t... I>(std::index_sequence<I...>) {
                    ([&] {
                        if (ok) return;
                        try {
                            std::variant_alternative_t<I, U> alt{};
                            FromJsonImpl(iJson, alt);
                            oValue = std::move(alt);
                            ok = true;
                        } catch (...) {
                        }
                    }(), ...);
                }(std::make_index_sequence<std::variant_size_v<U>>{});
                if (!ok)
                    throw nlohmann::json::type_error::create(
                        302, "no variant alternative matched", &iJson);
            }
            else if constexpr (std::ranges::range<U> &&
                               ByteSpan<U>) {
                if constexpr (requires { oValue.clear(); oValue.push_back(std::byte{}); }) {
                    oValue.clear();
                    for (const auto& e : iJson)
                        oValue.push_back(std::byte{e.template get<uint8_t>()});
                } else {
                    size_t i = 0;
                    for (const auto& e : iJson) {
                        if (i >= std::ranges::size(oValue)) break;
                        std::ranges::data(oValue)[i++] = std::byte{e.template get<uint8_t>()};
                    }
                }
            }
            else if constexpr (AssociativeRange<U>) {
                oValue.clear();
                for (auto it = iJson.begin(); it != iJson.end(); ++it) {
                    typename U::mapped_type v{};
                    FromJsonImpl(it.value(), v);
                    oValue.emplace(typename U::key_type(it.key()), std::move(v));
                }
            }
            else if constexpr (Traits::TupleLike<U> && !std::ranges::range<U>) {
                if (!iJson.is_array() ||
                    iJson.size() != std::tuple_size_v<U>)
                    throw nlohmann::json::type_error::create(
                        302, "expected fixed-size JSON array for tuple", &iJson);
                [&]<size_t... I>(std::index_sequence<I...>) {
                    (FromJsonImpl(iJson[I], std::get<I>(oValue)), ...);
                }(std::make_index_sequence<std::tuple_size_v<U>>{});
            }
            else if constexpr (std::ranges::range<U>) {
                if constexpr (requires { oValue.clear(); oValue.emplace_back(); }) {
                    oValue.clear();
                    for (const auto& e : iJson) {
                        typename U::value_type tmp{};
                        FromJsonImpl(e, tmp);
                        oValue.push_back(std::move(tmp));
                    }
                } else {
                    size_t i = 0;
                    for (const auto& e : iJson) {
                        if (i >= std::ranges::size(oValue)) break;
                        FromJsonImpl(e, std::ranges::data(oValue)[i++]);
                    }
                }
            }
            else if constexpr (ReflectableClass<U>) {
                if constexpr (HasNlohmannFromJson<U>) {
                    oValue = iJson.template get<U>();
                } else {
                    ClassFromJson(iJson, oValue);
                }
            }
            else if constexpr (HasNlohmannFromJson<U>) {
                oValue = iJson.template get<U>();
            }
            else {
                static_assert(false, "Type cannot be deserialised from JSON");
            }
        }

        // -------------------------------------------------------------------
        // CPO functors
        // -------------------------------------------------------------------

        struct ToJsonFn {
            template <typename T>
            [[nodiscard]] json operator()(T&& iValue) const {
                return ToJsonImpl(std::forward<T>(iValue));
            }
        };

        struct FromJsonFn {
            template <typename T>
            [[nodiscard]] T operator()(const json& iJson) const {
                T tmp{};
                FromJsonImpl(iJson, tmp);
                return tmp;
            }
            template <typename T>
            void operator()(const json& iJson, T& oValue) const {
                FromJsonImpl(iJson, oValue);
            }
        };

    } // namespace Json::Detail
    /** @endcond */

    /// @brief Customisation-point object: convert any value to `nlohmann::json`.
    inline constexpr Json::Detail::ToJsonFn ToJson{};

    /// @brief Customisation-point object: parse `nlohmann::json` into @p T.
    ///
    /// @code
    /// MyType v = FromJson<MyType>(j);
    /// FromJson(j, v); // in-place form
    /// @endcode
    template <typename T>
    [[nodiscard]] constexpr T FromJson(const json& iJson) {
        T tmp{};
        Json::Detail::FromJsonImpl(iJson, tmp);
        return tmp;
    }

    template <typename T>
    constexpr void FromJson(const json& iJson, T& oValue) {
        Json::Detail::FromJsonImpl(iJson, oValue);
    }

} // namespace Mashiro

// =====================================================================
// Bridge: nlohmann::adl_serializer specialisation for any reflectable
// Mashiro type. Lets `json j = v;` and `j.get<T>()` work transparently.
// =====================================================================

namespace nlohmann {

    template <typename T>
        requires (Mashiro::Json::Detail::ReflectableClass<T> ||
                  std::is_enum_v<T>) &&
                 (!Mashiro::Json::Detail::HasNlohmannToJson<T>)
    struct adl_serializer<T> {
        static void to_json(Mashiro::json& j, const T& v) {
            j = Mashiro::ToJson(v);
        }
        static void from_json(const Mashiro::json& j, T& v) {
            Mashiro::FromJson(j, v);
        }
    };

} // namespace nlohmann

// clang-format on

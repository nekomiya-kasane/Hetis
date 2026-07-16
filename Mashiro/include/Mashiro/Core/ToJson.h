/**
 * @file ToJson.h
 * @brief Reflection-driven, annotation-rich JSON serialisation built on
 *        nlohmann/json 3.12.
 *
 * Two customisation-point objects:
 * - `ToJson(v)`         → `nlohmann::json`
 * - `FromJson<T>(j)`    → `T`   (also `FromJson(j, v)` in-place form)
 *
 * Plus an `nlohmann::adl_serializer<T>` partial specialisation so any
 * reflectable Mashiro type implicitly works with the entire nlohmann/json
 * API: `json j = v;`, `j.template get<T>()`, `j["k"] = v;`, `dump()`,
 * `parse()`, JSON Pointer, JSON Patch, `ordered_json`, BSON / CBOR /
 * MessagePack / UBJSON binary formats — all transparent.
 *
 * @section dispatch Dispatch priority (`ToJson(v)`)
 *  0. `ToJsonHook<T>` specialisation — open customisation point.
 *  1. ADL `ToJson(v)` — Mashiro hook (returns `json`).
 *  2. Member `v.ToJson()`.
 *  3. nullptr / arithmetic / bool — direct conversion.
 *  4. Scoped enum — name(s); bitfield enums decompose into `"A|B|C"`;
 *     `[[=Json::Anno::AsInt{}]]` switches to integer encoding.
 *  5. `std::string` / `std::string_view` / convertibles — JSON string.
 *  6. `std::filesystem::path` — UTF-8 generic string.
 *  7. `std::chrono::duration` / `time_point` — int64 nanosecond count.
 *  8. `std::optional<T>` — null when empty, else recurse.
 *  9. `std::variant<...>` — visit and recurse on the active alternative.
 * 10. byte range — JSON array of `uint8_t`.
 * 11. Associative range with string-like key — JSON object.
 * 12. Tuple-like (`std::pair`, `std::tuple`) — JSON array.
 * 13. Range — JSON array.
 * 14. Reflectable class — JSON object via P2996 reflection of NSDM.
 *
 * `FromJson<T>(j)` is the symmetric inverse with the same priority order.
 *
 * @section anno Member / type annotations (`Json::Anno`)
 * - `Ignore`              — exclude member.
 * - `Key`                 — whitelist mode (only Key-tagged members emit).
 * - `Rename<"new_name">`  — JSON field-name override (NTTP via FixedString).
 * - `Optional` / `Required` — absence policy on parse.
 * - `Order{N}`            — emission order (ascending priority).
 * - `Flatten`             — splice nested object into parent object.
 * - `AsInt` / `AsString`  — enum encoding choice.
 * - `EmitDefault{false}`  — skip members equal to value-init.
 *
 * @ingroup Core
 */
#pragma once

// clang-format off

#include <nlohmann/json.hpp>

#include <array>
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

#include "Mashiro/Core/FixedString.h"
#include "Mashiro/Core/TypeTraits.h"

namespace Mashiro {

    /// @brief The `nlohmann::json` alias used throughout Mashiro.
    using json = nlohmann::json;

    /// @brief Insertion-order-preserving variant; useful for golden-file output.
    using ordered_json = nlohmann::ordered_json;

    /**
     * @brief Open customisation point for @ref ToJson / @ref FromJson — specialise to teach a type
     *        how to (de)serialise.
     *
     * The @ref Mashiro::ToJson / @ref Mashiro::FromJson names are customisation-point *objects* /
     * function templates at namespace scope, so a same-named free function or hidden friend inside
     * `namespace Mashiro` collides with them. This trait sidesteps that: users **specialise the class
     * template** instead of writing anything called `ToJson`, decoupling the hook from the name and
     * from any namespace. It is also the only mechanism that can target a whole template family by
     * **partial** specialisation (e.g. every `DumbPtr<W>`), which ADL overloads cannot express.
     *
     * Lives in @ref Mashiro::Hook, the namespace that gathers every open customisation point — type
     * `Mashiro::Hook::` in an IDE to discover what can be hooked.
     *
     * Provide a `static` member `ToJson(const T&) -> json` to emit, and optionally
     * `static void FromJson(const json&, T&)` to parse (omit it for one-way / output-only types).
     * The primary template is left undefined: an unspecialised @p T means "no hook", and dispatch
     * falls through to the generic reflection machinery.
     *
     * @code
     * // One-way (emit only) partial specialisation for a Mashiro-owned template type:
     * template<class W> struct Mashiro::Hook::ToJsonHook<Mashiro::DumbPtr<W>> {
     *     static Mashiro::json ToJson(Mashiro::DumbPtr<W> p) {
     *         return p ? Mashiro::json(std::format("{}", static_cast<const void*>(p.Get())))
     *                  : Mashiro::json(nullptr);
     *     }
     * };
     * @endcode
     *
     * @tparam T The (cv-unqualified) type to customise. Highest dispatch priority in @ref ToJson.
     */
    namespace Hook {
        template <typename T>
        struct ToJsonHook;
    } // namespace Hook

    // =========================================================================
    // Annotations
    // =========================================================================

    namespace Json::Anno {

        /// @brief Exclude this member from JSON serialisation entirely.
        struct Ignore { constexpr bool operator==(const Ignore&) const = default; };

        /// @brief Whitelist mode: when any member carries this, only Key-tagged
        ///        members participate.
        struct Key { constexpr bool operator==(const Key&) const = default; };

        /// @brief During @ref FromJson, a missing key uses the member's value-init.
        struct Optional { constexpr bool operator==(const Optional&) const = default; };

        /// @brief During @ref FromJson, a missing key throws `nlohmann::json::out_of_range`.
        struct Required { constexpr bool operator==(const Required&) const = default; };

        /// @brief Emission ordering — lower priority emits earlier.
        struct Order {
            int priority = 0;
            constexpr bool operator==(const Order&) const = default;
        };

        /// @brief Splice a nested object's members into the parent object.
        ///
        /// @code
        /// struct Common { int seq; uint64_t ts; };
        /// struct Event { [[=Json::Anno::Flatten{}]] Common header; std::string body; };
        /// // → {"seq":1,"ts":42,"body":"..."}   (no nested "header")
        /// @endcode
        struct Flatten { constexpr bool operator==(const Flatten&) const = default; };

        /// @brief Enum-only: serialise as integer instead of name.
        struct AsInt { constexpr bool operator==(const AsInt&) const = default; };

        /// @brief Enum-only: serialise as name (the default; explicit form for symmetry).
        struct AsString { constexpr bool operator==(const AsString&) const = default; };

        /// @brief Skip emitting members equal to value-initialised state.
        ///
        /// Place on a member to opt that one in; place on the type to opt the
        /// whole struct in. Defaulted to `true` so the annotation reads as
        /// "do skip defaults".
        struct EmitDefault {
            bool emit = true;
            constexpr bool operator==(const EmitDefault&) const = default;
        };

        /// @brief Override the JSON field name used for this member.
        ///
        /// Templated on a @ref Mashiro::FixedString so the literal becomes
        /// part of the type, satisfying the C++26 annotation structural-type
        /// constraint:
        /// @code
        /// [[=Json::Anno::Rename<"display">{}]] std::string label;
        /// @endcode
        template <FixedString<256> S>
        struct Rename {
            static constexpr std::string_view name = S.view();
            constexpr bool operator==(const Rename&) const = default;
        };

    } // namespace Json::Anno

} // namespace Mashiro

// =========================================================================
// Specialise PayloadTrait for Json::Anno::Rename<S> so generic code can
// pull the renamed field name out of the annotation. Lives in the
// Traits namespace so it shares the discovery point with the rest of the
// reflection helpers.
// =========================================================================

namespace Mashiro::Traits::Anno {

    template <FixedString<256> S>
    struct PayloadTrait<Json::Anno::Rename<S>> {
        static constexpr bool             value = true;
        static constexpr std::string_view payload = S.view();
    };

} // namespace Mashiro::Traits::Anno

namespace Mashiro {

    /** @cond INTERNAL */
    namespace Json::Detail {

        // -------------------------------------------------------------------
        // Annotation aliases — keep call sites concise.
        // -------------------------------------------------------------------

        /// @brief Members participating in JSON, filtered + sorted by Order.
        template <typename T>
        consteval auto JsonMembers() {
            return Traits::Anno::SelectMembers<T, Anno::Ignore, Anno::Key, Anno::Order>();
        }

        // -------------------------------------------------------------------
        // FieldName — honour Rename<"...">.
        // -------------------------------------------------------------------

        template <std::meta::info M>
        consteval std::string_view FieldNameOf() {
            std::string_view ret = Traits::IdentifierOf(M);
            template for (constexpr auto a :
                          std::define_static_array(std::meta::annotations_of(M))) {
                using TA = typename [:std::meta::type_of(a):];
                if constexpr (Traits::Anno::PayloadTrait<TA>::value) {
                    ret = Traits::Anno::PayloadTrait<TA>::payload;
                }
            }
            return ret;
        }

        // -------------------------------------------------------------------
        // ADL hooks (Mashiro convention) and member hooks.
        // -------------------------------------------------------------------

        namespace ADL {

            void ToJson() = delete;   ///< Poison pill.
            void FromJson() = delete; ///< Poison pill.

            template <typename T>
            concept HasFreeToJson = requires(T&& v) {
                { ToJson(std::forward<T>(v)) } -> std::convertible_to<json>;
            };

            template <typename T>
            concept HasFreeFromJson = requires(const json& j) {
                { FromJson(j, std::declval<T&>()) };
            };

        } // namespace ADL

        template <typename T>
        concept HasMemberToJson = requires(const T& v) {
            { v.ToJson() } -> std::convertible_to<json>;
        };

        template <typename T>
        concept HasMemberFromJson = requires(const json& j) {
            { T::FromJson(j) } -> std::convertible_to<T>;
        };

        /// @brief A usable @ref Mashiro::Hook::ToJsonHook specialisation with an emit hook exists for @p T.
        template <typename T>
        concept HasHookToJson = requires(const T& v) {
            { Hook::ToJsonHook<T>::ToJson(v) } -> std::convertible_to<json>;
        };

        /// @brief A @ref Mashiro::Hook::ToJsonHook specialisation with a parse hook exists for @p T.
        template <typename T>
        concept HasHookFromJson = requires(const json& j, T& v) {
            Hook::ToJsonHook<T>::FromJson(j, v);
        };

        /// @brief Detects nlohmann's own free `to_json(j, v)` /
        ///        `from_json(j, v)` for a type. Used so we don't double-wrap
        ///        types that already have a serialiser.
        template <typename T>
        concept HasNlohmannToJson = requires(json& j, const T& v) {
            ::nlohmann::to_json(j, v);
        };

        template <typename T>
        concept HasNlohmannFromJson = requires(const json& j, T& v) {
            ::nlohmann::from_json(j, v);
        };

        // -------------------------------------------------------------------
        // What counts as a "reflectable class" for the JSON object branch.
        // -------------------------------------------------------------------

        template <typename T>
        concept ReflectableClass =
            std::is_class_v<std::remove_cvref_t<T>> &&
            !std::is_union_v<std::remove_cvref_t<T>> &&
            !std::ranges::range<std::remove_cvref_t<T>> &&
            !Traits::TupleLike<std::remove_cvref_t<T>>;

    } // namespace Json::Detail
    /** @endcond */

    /** @cond INTERNAL */
    namespace Json::Detail {

        // -------------------------------------------------------------------
        // Forward declarations (mutual recursion between scalar/class paths).
        // -------------------------------------------------------------------

        template <typename T> [[nodiscard]] json ToJsonImpl(T&& iValue);
        template <typename T> void              FromJsonImpl(const json& iJson, T& oValue);

        // -------------------------------------------------------------------
        // Enum dispatch — name(s) ↔ integer.
        // -------------------------------------------------------------------

        template <typename E>
        [[nodiscard]] json EnumToJson(E iValue) {
            using U = std::underlying_type_t<E>;
            if constexpr (Traits::Anno::Has<Anno::AsInt>(^^E)) {
                return json(static_cast<U>(iValue));
            } else if constexpr (Traits::BitfieldEnum<E>) {
                auto remaining = static_cast<U>(iValue);
                std::string out;
                template for (constexpr auto e : Traits::Enumerators<E>) {
                    constexpr auto flag = static_cast<U>([:e:]);
                    if constexpr (flag != U{0}) {
                        if ((remaining & flag) == flag) {
                            if (!out.empty()) out += '|';
                            out += Traits::IdentifierOf(e);
                            remaining &= static_cast<U>(~flag);
                        }
                    }
                }
                if (out.empty()) out = "0";
                return json(out);
            } else {
                template for (constexpr auto e : Traits::Enumerators<E>) {
                    if ([:e:] == iValue) return json(std::string(Traits::IdentifierOf(e)));
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
                throw nlohmann::json::type_error::create(
                    302, "expected string or integer for enum", &iJson);
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
                    while (!tok.empty() && tok.back()  == ' ') tok.remove_suffix(1);
                    if (!tok.empty() && tok != "0") {
                        bool matched = false;
                        template for (constexpr auto e : Traits::Enumerators<E>) {
                            if (!matched && tok == Traits::IdentifierOf(e)) {
                                acc |= static_cast<U>([:e:]);
                                matched = true;
                            }
                        }
                        if (!matched)
                            throw nlohmann::json::other_error::create(
                                501, "unknown enumerator '" + std::string(tok) + "'", &iJson);
                    }
                    pos = bar + 1;
                }
                oValue = static_cast<E>(acc);
                return;
            } else {
                bool matched = false;
                template for (constexpr auto e : Traits::Enumerators<E>) {
                    if (!matched && sv == Traits::IdentifierOf(e)) {
                        oValue  = [:e:];
                        matched = true;
                    }
                }
                if (!matched)
                    throw nlohmann::json::other_error::create(
                        501, "unknown enumerator '" + std::string(sv) + "'", &iJson);
            }
        }

        // -------------------------------------------------------------------
        // Reflectable class dispatch.
        // -------------------------------------------------------------------

        template <typename T>
        consteval bool TypeEmitsDefaults() {
            auto a = Traits::Anno::Get<Anno::EmitDefault>(^^T);
            return a ? a->emit : true;
        }

        template <typename T>
        [[nodiscard]] json ClassToJson(const T& iValue) {
            json obj = json::object();
            template for (constexpr auto m :
                          std::define_static_array(JsonMembers<T>())) {
                using MemberT = typename [:std::meta::type_of(m):];
                const auto& field = iValue.[:m:];

                if constexpr (Traits::Anno::Has<Anno::Flatten>(m)) {
                    json sub = ToJsonImpl(field);
                    if (sub.is_object()) {
                        for (auto it = sub.begin(); it != sub.end(); ++it)
                            obj[it.key()] = std::move(it.value());
                    } else {
                        obj[std::string(FieldNameOf<m>())] = std::move(sub);
                    }
                } else {
                    constexpr bool memberEmitDefault =
                        Traits::Anno::Get<Anno::EmitDefault>(m).value_or(
                            Anno::EmitDefault{TypeEmitsDefaults<T>()}).emit;
                    if constexpr (!memberEmitDefault) {
                        if constexpr (std::equality_comparable<MemberT>) {
                            if (field == MemberT{}) continue;
                        }
                    }
                    if constexpr (std::is_enum_v<MemberT> && Traits::Anno::Has<Anno::AsInt>(m)) {
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
                throw nlohmann::json::type_error::create(302, "expected JSON object", &iJson);
            template for (constexpr auto m :
                          std::define_static_array(JsonMembers<T>())) {
                using MemberT = typename [:std::meta::type_of(m):];
                MemberT& field = oValue.[:m:];

                if constexpr (Traits::Anno::Has<Anno::Flatten>(m)) {
                    FromJsonImpl(iJson, field);
                } else {
                    auto key = std::string(FieldNameOf<m>());
                    auto it  = iJson.find(key);
                    if (it == iJson.end()) {
                        if constexpr (Traits::Anno::Has<Anno::Required>(m))
                            throw nlohmann::json::out_of_range::create(
                                403, "missing required key '" + key + "'", &iJson);
                    } else if constexpr (std::is_enum_v<MemberT> && Traits::Anno::Has<Anno::AsInt>(m)) {
                        using U = std::underlying_type_t<MemberT>;
                        field = static_cast<MemberT>(it->template get<U>());
                    } else {
                        FromJsonImpl(*it, field);
                    }
                }
            }
        }

    } // namespace Json::Detail
    /** @endcond */

    /** @cond INTERNAL */
    namespace Json::Detail {

        // -------------------------------------------------------------------
        // Core dispatch.
        // -------------------------------------------------------------------

        template <typename T>
        [[nodiscard]] json ToJsonImpl(T&& iValue) {
            using U = std::remove_cvref_t<T>;
            if      constexpr (HasHookToJson<U>)                 return Hook::ToJsonHook<U>::ToJson(iValue);
            else if constexpr (ADL::HasFreeToJson<U>)            return ToJson(std::forward<T>(iValue));
            else if constexpr (HasMemberToJson<U>)               return iValue.ToJson();
            else if constexpr (std::is_null_pointer_v<U>)        return json(nullptr);
            else if constexpr (std::same_as<U, bool>)            return json(iValue);
            else if constexpr (std::is_arithmetic_v<U>)          return json(iValue);
            else if constexpr (std::is_enum_v<U>)                return EnumToJson(iValue);
            else if constexpr (Traits::FilesystemPath<U>)        return json(iValue.generic_string());
            else if constexpr (Traits::StringViewConvertible<U>) return json(std::string(std::string_view(iValue)));
            else if constexpr (Traits::ChronoDuration<U>) {
                return json(std::chrono::duration_cast<std::chrono::nanoseconds>(iValue).count());
            }
            else if constexpr (Traits::ChronoTimePoint<U>) {
                return json(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                iValue.time_since_epoch()).count());
            }
            else if constexpr (Traits::StdOptional<U>) {
                if (!iValue.has_value()) return json(nullptr);
                return ToJsonImpl(*iValue);
            }
            else if constexpr (Traits::StdVariant<U>) {
                return std::visit(
                    [](const auto& alt) -> json { return ToJsonImpl(alt); }, iValue);
            }
            else if constexpr (Traits::ByteRange<U>) {
                json arr = json::array();
                for (auto b : iValue) arr.push_back(static_cast<uint8_t>(b));
                return arr;
            }
            else if constexpr (Traits::StringKeyedAssociative<U>) {
                json obj = json::object();
                for (const auto& [k, v] : iValue)
                    obj[std::string(std::string_view(k))] = ToJsonImpl(v);
                return obj;
            }
            else if constexpr (Traits::TupleLike<U> && !std::ranges::range<U>) {
                json arr = json::array();
                std::apply(
                    [&](const auto&... elems) {
                        (arr.push_back(ToJsonImpl(elems)), ...);
                    }, iValue);
                return arr;
            }
            else if constexpr (std::ranges::range<U>) {
                json arr = json::array();
                for (const auto& e : iValue) arr.push_back(ToJsonImpl(e));
                return arr;
            }
            else if constexpr (ReflectableClass<U>) {
                if constexpr (HasNlohmannToJson<U>) return json(iValue);
                else                                return ClassToJson(iValue);
            }
            else if constexpr (HasNlohmannToJson<U>)              return json(iValue);
            else static_assert(false, "Type cannot be serialised to JSON");
        }

        template <typename T>
        void FromJsonImpl(const json& iJson, T& oValue) {
            using U = std::remove_cvref_t<T>;
            if      constexpr (HasHookFromJson<U>)        Hook::ToJsonHook<U>::FromJson(iJson, oValue);
            else if constexpr (ADL::HasFreeFromJson<U>)   FromJson(iJson, oValue);
            else if constexpr (HasMemberFromJson<U>)      oValue = U::FromJson(iJson);
            else if constexpr (std::is_null_pointer_v<U>) {
                if (!iJson.is_null())
                    throw nlohmann::json::type_error::create(302, "expected null", &iJson);
            }
            else if constexpr (std::same_as<U, bool>)         oValue = iJson.template get<bool>();
            else if constexpr (std::is_arithmetic_v<U>)       oValue = iJson.template get<U>();
            else if constexpr (std::is_enum_v<U>)             EnumFromJson(iJson, oValue);
            else if constexpr (std::same_as<U, std::string>)  oValue = iJson.template get<std::string>();
            else if constexpr (Traits::FilesystemPath<U>)     oValue = std::filesystem::path(iJson.template get<std::string>());
            else if constexpr (Traits::ChronoDuration<U>) {
                oValue = std::chrono::duration_cast<U>(
                    std::chrono::nanoseconds(iJson.template get<int64_t>()));
            }
            else if constexpr (Traits::ChronoTimePoint<U>) {
                oValue = U(std::chrono::duration_cast<typename U::duration>(
                    std::chrono::nanoseconds(iJson.template get<int64_t>())));
            }
            else if constexpr (Traits::StdOptional<U>) {
                if (iJson.is_null()) { oValue.reset(); return; }
                typename U::value_type tmp{};
                FromJsonImpl(iJson, tmp);
                oValue = std::move(tmp);
            }
            else if constexpr (Traits::StdVariant<U>) {
                bool ok = false;
                [&]<size_t... I>(std::index_sequence<I...>) {
                    ([&] {
                        if (ok) return;
                        try {
                            std::variant_alternative_t<I, U> alt{};
                            FromJsonImpl(iJson, alt);
                            oValue = std::move(alt);
                            ok = true;
                        } catch (...) {}
                    }(), ...);
                }(std::make_index_sequence<std::variant_size_v<U>>{});
                if (!ok)
                    throw nlohmann::json::type_error::create(
                        302, "no variant alternative matched", &iJson);
            }
            else if constexpr (Traits::ByteRange<U>) {
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
            else if constexpr (Traits::StringKeyedAssociative<U>) {
                oValue.clear();
                for (auto it = iJson.begin(); it != iJson.end(); ++it) {
                    typename U::mapped_type v{};
                    FromJsonImpl(it.value(), v);
                    oValue.emplace(typename U::key_type(it.key()), std::move(v));
                }
            }
            else if constexpr (Traits::TupleLike<U> && !std::ranges::range<U>) {
                if (!iJson.is_array() || iJson.size() != std::tuple_size_v<U>)
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
                if constexpr (HasNlohmannFromJson<U>) oValue = iJson.template get<U>();
                else                                  ClassFromJson(iJson, oValue);
            }
            else if constexpr (HasNlohmannFromJson<U>) oValue = iJson.template get<U>();
            else static_assert(false, "Type cannot be deserialised from JSON");
        }

        // -------------------------------------------------------------------
        // CPO functors.
        // -------------------------------------------------------------------

        struct ToJsonFn {
            template <typename T>
            [[nodiscard]] json operator()(T&& v) const {
                return ToJsonImpl(std::forward<T>(v));
            }
        };

        struct FromJsonFn {
            template <typename T>
            [[nodiscard]] T operator()(const json& j) const {
                T tmp{};
                FromJsonImpl(j, tmp);
                return tmp;
            }
            template <typename T>
            void operator()(const json& j, T& v) const { FromJsonImpl(j, v); }
        };

    } // namespace Json::Detail
    /** @endcond */

    /// @brief Customisation-point object: convert any value to `nlohmann::json`.
    inline constexpr Json::Detail::ToJsonFn ToJson{};

    /// @brief Parse JSON into @p T (returning form).
    /// @code
    /// MyType v = FromJson<MyType>(j);
    /// FromJson(j, v); // in-place form
    /// @endcode
    template <typename T>
    [[nodiscard]] constexpr T FromJson(const json& j) {
        T tmp{};
        Json::Detail::FromJsonImpl(j, tmp);
        return tmp;
    }

    /// @brief Parse JSON into @p oValue (in-place form).
    template <typename T>
    constexpr void FromJson(const json& j, T& oValue) {
        Json::Detail::FromJsonImpl(j, oValue);
    }

} // namespace Mashiro

// =====================================================================
// nlohmann::adl_serializer<T> partial specialisation: lets `json j = v;`
// and `j.template get<T>()` work transparently for any reflectable
// Mashiro type or scoped enum that does not already have its own
// `to_json` / `from_json`.
// =====================================================================

namespace nlohmann {
    template <typename T>
        requires (Mashiro::Json::Detail::ReflectableClass<T> || std::is_enum_v<T>) &&
                 (!Mashiro::Json::Detail::HasNlohmannToJson<T>)
    struct adl_serializer<T> {
        static void to_json  (Mashiro::json& j, const T& v) { j = Mashiro::ToJson(v); }
        static void from_json(const Mashiro::json& j, T& v) { Mashiro::FromJson(j, v); }
    };
} // namespace nlohmann

// clang-format on

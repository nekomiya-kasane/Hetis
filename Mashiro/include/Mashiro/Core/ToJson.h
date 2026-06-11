/**
 * @file ToJson.h
 * @brief Reflection-driven, annotation-rich JSON serialisation built on
 *        nlohmann/json 3.12.
 *
 * Provides two customisation-point objects:
 * - `ToJson(v)`         → `nlohmann::json`
 * - `FromJson<T>(j)`    → `T`
 *
 * Plus an `nlohmann::adl_serializer<T>` specialisation so any reflectable
 * Mashiro type implicitly works with the entire nlohmann/json API
 * (`json j = v;`, `j.get<T>()`, `j["k"] = v;`, ranges, structured bindings,
 * `dump()`, `parse()`, JSON Pointer, JSON Patch, ordered_json, BSON/CBOR/
 * MessagePack/UBJSON binary formats — all transparent).
 *
 * @section dispatch Dispatch priority
 * @par `ToJson(v)`
 *  1. ADL `to_json(json&, v)` — native nlohmann hook.
 *  2. ADL `ToJson(v)` — Mashiro hook (returns `json`).
 *  3. Member `v.ToJson()`.
 *  4. nullptr / arithmetic / string-like — direct conversion.
 *  5. `std::filesystem::path` — string form (UTF-8).
 *  6. `std::chrono::*` — ISO-8601 / count form.
 *  7. `std::optional<T>` / pointer-like — null when empty.
 *  8. `std::variant<...>` — visit.
 *  9. Scoped enum — name(s) (bitfield decomposition) or integer
 *     (with `[[=Json::Anno::AsInt{}]]`).
 * 10. Tuple-like (`std::pair`, `std::tuple`, `std::array`) — JSON array.
 * 11. Range — JSON array (associative ranges → object when keys are strings).
 * 12. `std::byte` span / `std::vector<std::byte>` — binary (CBOR-friendly).
 * 13. Reflectable class — JSON object via P2996 reflection of NSDM.
 * 14. Trivially-copyable POD — base64 string fallback.
 *
 * @par `FromJson<T>(j)`
 * Symmetric inverse with the same priorities.
 *
 * @section anno Member / type annotations
 * Place on type or NSDM:
 * - `[[=Json::Anno::Ignore{}]]`            — exclude from JSON.
 * - `[[=Json::Anno::Key{}]]`               — whitelist mode (only keys participate).
 * - `[[=Json::Anno::Rename{"new_name"}]]`  — JSON field name override.
 * - `[[=Json::Anno::Optional{}]]`          — tolerate missing key during parse.
 * - `[[=Json::Anno::Required{}]]`          — throw if missing during parse.
 * - `[[=Json::Anno::Order{N}]]`            — emission order (ascending).
 * - `[[=Json::Anno::Flatten{}]]`           — splice nested object into parent.
 * - `[[=Json::Anno::AsInt{}]]`             — enum: emit as integer.
 * - `[[=Json::Anno::AsString{}]]`          — enum: emit as name (default).
 * - `[[=Json::Anno::EmitDefault{false}]]`  — skip members equal to value-init.
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

    namespace Json::Anno {

        /// @brief Exclude this member from JSON serialisation entirely.
        struct Ignore { constexpr bool operator==(const Ignore&) const = default; };

        /// @brief Whitelist mode: when any member carries this, only Key-annotated members participate.
        struct Key { constexpr bool operator==(const Key&) const = default; };

        /// @brief Override the JSON field name used for this member.
        ///
        /// `Rename` is templated on a @ref Mashiro::FixedString so the literal
        /// becomes part of the type, satisfying the C++26 annotation
        /// "structural type" constraint:
        /// @code
        /// [[=Json::Anno::Rename<"display">{}]] std::string label;
        /// @endcode
        template <FixedString S>
        struct Rename {
            /// The renamed field, accessible at compile time via reflection.
            static constexpr std::string_view name = S.view();
            constexpr bool operator==(const Rename&) const = default;
        };

        /// @brief During @ref FromJson, a missing key uses the member's value-init.
        struct Optional { constexpr bool operator==(const Optional&) const = default; };

        /// @brief During @ref FromJson, a missing key throws `nlohmann::json::out_of_range`.
        struct Required { constexpr bool operator==(const Required&) const = default; };

        /// @brief Emission ordering — lower priority emits earlier.
        struct Order {
            int priority = 0;
            constexpr bool operator==(const Order&) const = default;
        };

        /**
         * @brief Splice a nested object's members into the parent object.
         *
         * @code
         * struct Common { int seq; uint64_t ts; };
         * struct Event { [[=Json::Anno::Flatten{}]] Common header; std::string body; };
         * // → {"seq": 1, "ts": 42, "body": "..."}   (no nested "header")
         * @endcode
         */
        struct Flatten { constexpr bool operator==(const Flatten&) const = default; };

        /// @brief Enum-only: serialise as integer instead of name.
        struct AsInt { constexpr bool operator==(const AsInt&) const = default; };

        /// @brief Enum-only: serialise as name (the default; explicit form is provided for symmetry).
        struct AsString { constexpr bool operator==(const AsString&) const = default; };

        /**
         * @brief Skip emitting members equal to value-initialised state.
         *
         * Useful for keeping persisted configs minimal. Defaulted to `true` for
         * the annotated member; place on the type to opt the entire struct in.
         */
        struct EmitDefault {
            bool emit = true;
            constexpr bool operator==(const EmitDefault&) const = default;
        };

    } // namespace Json::Anno

    /** @cond INTERNAL */
    namespace Json::Detail {

        // -------------------------------------------------------------------
        // Annotation helpers
        // -------------------------------------------------------------------

        /// @brief `true` if @p ent carries any annotation of type @p A.
        template <typename A>
        consteval bool Has(std::meta::info ent) {
            return std::meta::annotations_of(ent, ^^A).size() > 0;
        }

        /// @brief Extract the first annotation of type @p A on @p ent, or `nullopt`.
        template <typename A>
        consteval std::optional<A> Get(std::meta::info ent) {
            auto annots = std::meta::annotations_of(ent, ^^A);
            if (annots.size() > 0) return std::meta::extract<A>(annots[0]);
            return std::nullopt;
        }

        /// @brief Some member of @p T has @ref Anno::Key (engages whitelist mode).
        template <typename T>
        consteval bool IsKeyMode() {
            for (auto m : std::meta::nonstatic_data_members_of(
                              ^^T, std::meta::access_context::unchecked()))
                if (Has<Anno::Key>(m)) return true;
            return false;
        }

        /// @brief Members participating in JSON, sorted by `Anno::Order` priority.
        template <typename T>
        consteval auto JsonMembers() {
            auto all = std::meta::nonstatic_data_members_of(
                ^^T, std::meta::access_context::unchecked());
            std::vector<std::meta::info> result;
            for (auto m : all) {
                if (Has<Anno::Ignore>(m)) continue;
                if (IsKeyMode<T>() && !Has<Anno::Key>(m)) continue;
                result.push_back(m);
            }
            for (size_t i = 1; i < result.size(); ++i) {
                auto key = result[i];
                int  pk = Get<Anno::Order>(key).value_or(Anno::Order{0x7FFFFFFF}).priority;
                size_t j = i;
                while (j > 0) {
                    int pj = Get<Anno::Order>(result[j - 1])
                                 .value_or(Anno::Order{0x7FFFFFFF}).priority;
                    if (pj <= pk) break;
                    result[j] = result[j - 1];
                    --j;
                }
                result[j] = key;
            }
            return result;
        }

        /** @cond INTERNAL */
        /// Helper trait: pull the FixedString NTTP from `Rename<S>` and
        /// expose its string_view. Specialised below.
        template <typename T>
        struct RenameTrait {
            static constexpr bool value = false;
        };
        template <FixedString S>
        struct RenameTrait<Anno::Rename<S>> {
            static constexpr bool             value = true;
            static constexpr std::string_view name  = S.view();
        };
        /** @endcond */

        /// @brief JSON field name for a member (honours `Anno::Rename<"...">`).
        ///
        /// Returns the renamed field name when present, otherwise the
        /// member's source identifier. Implemented via `template for` so the
        /// annotation type is splice-able.
        template <std::meta::info M>
        consteval std::string_view FieldNameOf() {
            std::string_view ret = std::meta::identifier_of(M);
            template for (constexpr auto a :
                          std::define_static_array(
                              std::meta::annotations_of(M))) {
                using TA = typename [:std::meta::type_of(a):];
                if constexpr (RenameTrait<TA>::value) {
                    ret = RenameTrait<TA>::name;
                }
            }
            return ret;
        }

        // -------------------------------------------------------------------
        // ADL hooks
        // -------------------------------------------------------------------

        namespace ADL {
            void ToJson() = delete;   ///< Poison pill — disables unrelated ADL hits.
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

        /// @brief Detects nlohmann's own to_json customisation (free ADL or
        ///        primary `adl_serializer` template), independent of our own
        ///        partial specialisation — using the unqualified call form
        ///        avoids the constraint recursion that occurs when the test
        ///        instantiates `adl_serializer<T>` whose own constraints
        ///        reference this concept.
        template <typename T>
        concept HasNlohmannToJson = requires(json& j, const T& v) {
            ::nlohmann::to_json(j, v);
        };

        template <typename T>
        concept HasNlohmannFromJson = requires(const json& j, T& v) {
            ::nlohmann::from_json(j, v);
        };

        // -------------------------------------------------------------------
        // Type categorisation
        // -------------------------------------------------------------------

        template <typename T> struct IsOptional : std::false_type {};
        template <typename U> struct IsOptional<std::optional<U>> : std::true_type {};

        template <typename T> struct IsVariant : std::false_type {};
        template <typename... U> struct IsVariant<std::variant<U...>> : std::true_type {};

        template <typename T>
        concept ChronoDuration = requires {
            typename T::rep; typename T::period;
            requires std::same_as<T, std::chrono::duration<typename T::rep, typename T::period>>;
        };

        template <typename T>
        concept ChronoTimePoint = requires {
            typename T::clock; typename T::duration;
            requires std::same_as<
                T, std::chrono::time_point<typename T::clock, typename T::duration>>;
        };

        template <typename T>
        concept FilesystemPath = std::same_as<std::remove_cvref_t<T>, std::filesystem::path>;

        template <typename T>
        concept ByteSpan = std::ranges::range<T> &&
                           std::same_as<std::ranges::range_value_t<T>, std::byte>;

        template <typename T>
        concept StringLike =
            std::convertible_to<T, std::string_view> &&
            !std::is_same_v<std::remove_cvref_t<T>, std::filesystem::path>;

        template <typename T>
        concept AssociativeRange = std::ranges::range<T> &&
            requires { typename std::remove_cvref_t<T>::key_type; } &&
            std::convertible_to<typename std::remove_cvref_t<T>::key_type, std::string_view>;

        template <typename T>
        concept ReflectableClass =
            std::is_class_v<std::remove_cvref_t<T>> &&
            !std::is_union_v<std::remove_cvref_t<T>> &&
            !std::ranges::range<std::remove_cvref_t<T>> &&
            !Traits::TupleLike<std::remove_cvref_t<T>>;

    } // namespace Json::Detail
    /** @endcond */

} // namespace Mashiro

// clang-format on

#include "Mashiro/Core/ToJson.inl"

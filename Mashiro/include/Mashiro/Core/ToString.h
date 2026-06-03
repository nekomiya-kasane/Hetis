/**
 * @file ToString.h
 * @brief Reflection-driven, extensible string conversion framework.
 *
 * Provides three customisation-point objects:
 * - `ToString(v)`     → `std::string`       (always owning).
 * - `ToStringView(v)` → `std::string_view`   (zero-alloc for enums, bools, …).
 * - `Str(v)`          → one of the above, preferring the view when safe.
 *
 * And the inverse:
 * - `FromString<T>(sv)` → `T`.
 *
 * Dispatch priority (ToString):
 *  1. ADL `ToString(v)`
 *  2. `v.ToString()`
 *  3. `nullptr` / `bool` / `char` / string-like literals
 *  4. Scoped enum → reflection-based name (with bitmask decomposition)
 *  5. Range → `[elem, elem, …]`
 *  6. Tuple-like → `(elem, elem, …)`
 *  7. Variant → visit
 *  8. Pointer → `Type*(0xaddr)`
 *  9. `std::to_string` / `operator<<`
 * 10. Complete class → reflection member dump `Type{a=…, b=…}`
 * 11. Fallback → `<Type at 0xaddr>`
 *
 * @ingroup Core
 */
#pragma once

// clang-format off

#include <meta>
#include <concepts>
#include <format>
#include <ostream>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <sstream>
#include <variant>

#include "Mashiro/Core/TypeTraits.h"

namespace Mashiro {

    /// @brief Parse a value of type @p T from its string representation.
    template<typename T>
    [[nodiscard]] constexpr T FromString(std::string_view iString);

    /// @brief Compile-time human-readable name of @p T (via P2996 reflection).
    template<typename T>
    [[nodiscard]] std::string_view TypeName() {
        return std::meta::display_string_of(^^T);
    }

    /// @brief Convert any string-like value to an owning `std::string`.
    template<typename T>
    [[nodiscard]] constexpr std::string MakeString(T&& iValue) {
        if constexpr (std::same_as<std::remove_cvref_t<T>, std::string>) {
            return std::forward<T>(iValue);
        } else {
            return std::string(std::forward<T>(iValue));
        }
    }

    /** @cond INTERNAL */
    namespace Detail {

        /// @name ADL hook detection
        /// @{
        namespace ADL {

            namespace FreeToString {
                void ToString() = delete; ///< Poison pill.

                /// @brief True if `ToString(v)` is found via ADL.
                template<typename T>
                concept Available = requires(T&& iValue) {
                    { ToString(std::forward<T>(iValue)) } -> std::convertible_to<std::string>;
                };

                template<typename T> requires Available<T>
                [[nodiscard]] constexpr std::string Invoke(T&& iValue) {
                    return MakeString(ToString(std::forward<T>(iValue)));
                }
            } // namespace FreeToString

            namespace FreeToStringView {
                void ToStringView() = delete; ///< Poison pill.

                /// @brief True if `ToStringView(v)` is found via ADL.
                template<typename T>
                concept Available = requires(T&& iValue) {
                    { ToStringView(std::forward<T>(iValue)) } -> std::convertible_to<std::string_view>;
                };

                template<typename T> requires Available<T>
                [[nodiscard]] constexpr std::string_view Invoke(T&& iValue) {
                    return ToStringView(std::forward<T>(iValue));
                }
            } // namespace FreeToStringView

            namespace FreeFromString {
                void FromString() = delete; ///< Poison pill.

                /// @brief True if `FromString<T>(sv)` is found via ADL.
                template <typename T>
                concept Available = requires(std::string_view iString) {
                    { FromString<T>(iString) } -> std::convertible_to<T>;
                };

                template <typename T> requires Available<T>
                [[nodiscard]] constexpr T Invoke(std::string_view iString) {
                    return FromString<T>(iString);
                }
            } // namespace FreeFromString

        } // namespace ADL
        /// @}

        /// @brief `v.ToString()` member detected.
        template <typename T>
        concept HasMemberToString = requires(T&& iValue) {
            { std::forward<T>(iValue).ToString() } -> std::convertible_to<std::string>;
        };

        /// @brief `v.ToStringView()` member detected.
        template <typename T>
        concept HasMemberToStringView = requires(T&& iValue) {
            { std::forward<T>(iValue).ToStringView() } -> std::convertible_to<std::string_view>;
        };

        /// @brief `T::FromString(sv)` static member detected.
        template <typename T>
        concept HasMemberFromString = requires {
            { T::FromString(std::string_view{}) } -> std::convertible_to<T>;
        };

        /**
         * @brief Types whose `ToStringView` result is a view over static / stable
         *        storage (won't dangle). Everything else must go through `ToString`.
         */
        template <typename T>
        concept ViewStringable = ADL::FreeToStringView::Available<std::remove_cvref_t<T>> ||
                                 HasMemberToStringView<std::remove_cvref_t<T>> ||
                                 std::is_null_pointer_v<std::remove_cvref_t<T>> ||
                                 std::same_as<std::remove_cvref_t<T>, bool> ||
                                 std::is_enum_v<std::remove_cvref_t<T>>;

    /// @brief Core dispatch: value → owning `std::string`.
    template <typename T>
    [[nodiscard]] constexpr std::string ToStringImpl(T&& iValue) {
        using U = std::remove_cvref_t<T>;

        // 1. free ToString(...) via ADL
        if constexpr (Detail::ADL::FreeToString::Available<U>) {
            return Detail::ADL::FreeToString::Invoke(std::forward<T>(iValue));
        } 
        // 2. object.ToString()
        else if constexpr (Detail::HasMemberToString<U>) {
            return MakeString(std::forward<T>(iValue).ToString());
        } 
        // 3. nullptr
        else if constexpr (std::is_null_pointer_v<U>) {
            return "nullptr";
        } 
        // 4. bool
        else if constexpr (std::same_as<U, bool>) {
            return iValue ? "true" : "false";
        }
        // 5. char
        else if constexpr (std::same_as<U, char>) {
            return std::string(1, iValue);
        }
        // 6. string-like types
        else if constexpr (std::is_convertible_v<U, std::string> || std::is_convertible_v<U, std::string_view>) {
            return MakeString(std::forward<T>(iValue));
        } 
        // 7. enum
        else if constexpr (std::is_enum_v<U>) {
            static_assert(std::meta::is_enumerable_type(std::meta::dealias(^^U)));
            using Underlying = std::underlying_type_t<U>;

            // a. exact match
            template for (constexpr auto e : std::define_static_array(std::meta::enumerators_of(std::meta::dealias(^^U)))) {
                if ([:e:] == iValue) {
                    return std::string(std::meta::display_string_of(e));
                }
            }

            // b. bitmask decomposition
            std::stringstream ss;
            bool first = true;
            auto remaining = static_cast<Underlying>(iValue);

            template for (constexpr auto e : std::define_static_array(std::meta::enumerators_of(std::meta::dealias(^^U)))) {
                constexpr auto flag = static_cast<Underlying>([:e:]);
                if constexpr (flag != Underlying{0}) {
                    if ((remaining & flag) == flag) {
                        ss << (first ? "" : " | ") << std::meta::display_string_of(e);
                        first = false;
                        remaining &= static_cast<Underlying>(~flag);
                    }
                }
            }

            if (!first && remaining == Underlying{0}) {
                return ss.str();
            }

            // c. fallback
            return std::format("{}(unknown:{})", TypeName<U>(), static_cast<Underlying>(iValue));
        }
        // 8. range
        else if constexpr (std::ranges::range<U>) {
            std::stringstream ss;
            ss << "[";

            bool first = true;

            for (auto&& item : iValue) {
                ss << (first ? "" : ", ") << ToStringImpl(item);
                first = false;
            }

            ss << "]";
            return ss.str();
        }
        // 9. tuple
        else if constexpr (Traits::TupleLike<U>) {
            static_assert(!std::ranges::range<U>);

            std::stringstream ss;
            ss << "(";

            bool first = false;

            std::apply([&]<typename... Ts>(Ts&&... args) {
                ((ss << (first ? ", " : "") << ToStringImpl(std::forward<decltype(args)>(args)), first = true), ...);
            }, iValue);

            ss << ")";
            return ss.str();
        }
        // 10. variant
        else if constexpr (Traits::VariantLike<U>) {
            if (iValue.valueless_by_exception()) {
                return std::format("{}(valueless)", TypeName<U>());
            }

            return std::visit(
                [](auto&& iAlternative) -> std::string {
                    return ToStringImpl(std::forward<decltype(iAlternative)>(iAlternative));
                },
                std::forward<T>(iValue));
        }
        // 11. pointer
        else if constexpr (std::is_pointer_v<U>) {
            if (iValue == nullptr) {
                return std::format("{}*(nullptr)", TypeName<std::remove_pointer_t<U>>());
            } else {
                return std::format("{}*({:p})", TypeName<std::remove_pointer_t<U>>(), static_cast<const void*>(iValue));
            }
        }
        // 12. std::to_string(A)
        else if constexpr (requires(T&& iValue) { std::to_string(iValue); }) {
            return std::to_string(std::forward<T>(iValue));
        }
        // 13. ostream operator
        else if constexpr (requires(std::ostream& os, T&& v) { os << std::forward<T>(v); }) {
            std::stringstream ss;
            ss << std::forward<T>(iValue);
            return ss.str();
        }
        // 14. complete class
        else if constexpr (std::is_class_v<U> && !std::is_union_v<U> && requires { sizeof(U); }) {
            std::stringstream ss;
            ss << TypeName<U>() << " {";

            template for (bool first = true; constexpr auto m : std::define_static_array(
                std::meta::nonstatic_data_members_of(^^U, std::meta::access_context::unchecked())
            )) {
                if (!first) {
                    ss << ", ";
                }
                first = false;

                if constexpr (std::meta::has_identifier(m)) {
                    ss << std::meta::identifier_of(m);
                }
                else {
                    ss << std::meta::display_string_of(m);
                }

                if constexpr (std::meta::is_bit_field(m)) {
                    ss << "=" << ToStringImpl(auto(iValue.[:m:]));
                } else {
                    ss << "=" << ToStringImpl(iValue.[:m:]);
                }
            }

            ss << "}";
            return ss.str();
        }
        // 15. fallback
        else {
            return std::format("<{} at {:p}>", TypeName<U>(), static_cast<const void*>(std::addressof(iValue)));
        }
    }

    /// @brief Core dispatch: value → non-owning `std::string_view` (limited types).
    template <typename T>
    [[nodiscard]] constexpr std::string_view ToStringViewImpl(T&& iValue) {
        using U = std::remove_cvref_t<T>;

        // 1. free ToStringView(...) via ADL
        if constexpr (Detail::ADL::FreeToStringView::Available<U>) {
            return Detail::ADL::FreeToStringView::Invoke(std::forward<T>(iValue));
        } 
        // 2. object.ToStringView()
        else if constexpr (Detail::HasMemberToStringView<U>) {
            return std::forward<T>(iValue).ToStringView();
        } 
        // 3. nullptr
        else if constexpr (std::is_null_pointer_v<U>) {
            return "nullptr";
        } 
        // 4. bool
        else if constexpr (std::same_as<U, bool>) {
            return iValue ? "true" : "false";
        }
        // 5. enum
        else if constexpr (std::is_enum_v<U>) {
            static_assert(std::meta::is_enumerable_type(std::meta::dealias(^^U)));
            using Underlying = std::underlying_type_t<U>;

            // a. exact match
            template for (constexpr auto e : std::define_static_array(std::meta::enumerators_of(std::meta::dealias(^^U)))) {
                if ([:e:] == iValue) {
                    return std::string_view(std::meta::display_string_of(e));
                }
            }

            // b. fallback
            return TypeName<U>();
        }
        // 6. fallback
        else {
            static_assert(false, "Unsupported type for ToStringView");
        }
    }
    
    /// @brief CPO functor: `ToString(v)` → `std::string`.
    struct ToStringFn {
        template <typename T>
        [[nodiscard]] constexpr std::string operator()(T&& iValue) const {
            return ToStringImpl(std::forward<T>(iValue));
        }
    };

    /// @brief CPO functor: `ToStringView(v)` → `std::string_view`.
    struct ToStringViewFn {
        template <typename T>
        [[nodiscard]] constexpr std::string_view operator()(T&& iValue) const {
            return ToStringViewImpl(std::forward<T>(iValue));
        }
    };

    } // namespace Detail
    /** @endcond */

    /// @brief Customisation-point object: convert any value to `std::string`.
    inline constexpr Detail::ToStringFn     ToString{};
    /// @brief Customisation-point object: convert to `std::string_view` (zero-alloc, limited types).
    inline constexpr Detail::ToStringViewFn ToStringView{};

    /**
     * @brief Smart string conversion: returns `string_view` when safe, `string` otherwise.
     * @tparam T Deduced value type.
     * @return `std::string_view` for ViewStringable types, `std::string` for everything else.
     */
    template <typename T>
    [[nodiscard]] constexpr auto Str(T&& iValue) {
        if constexpr (Detail::ViewStringable<T>) {
            return ToStringView(std::forward<T>(iValue));
        } else {
            return ToString(std::forward<T>(iValue));
        }
    }

    /**
     * @brief Parse a value of type @p T from its string representation.
     *
     * Dispatch priority mirrors ToString in reverse:
     *  1. ADL `FromString<T>(sv)` → 2. `T::FromString(sv)` →
     *  3. `nullptr` → 4. pointer → 5. `bool` → 6. enum (reflection match) → 7. fail.
     *
     * @tparam T Target type.
     * @param iString Input string.
     * @return Parsed value of type @p T.
     */
    template <typename T> [[nodiscard]] constexpr T FromString(std::string_view iString) {
        using U = std::remove_cvref_t<T>;

        // 1. free FromString(...) via ADL
        if constexpr (Detail::ADL::FreeFromString::Available<U>) {
            return Detail::ADL::FreeFromString::Invoke<T>(iString);
        } 
        // 2. object.FromString()
        else if constexpr (Detail::HasMemberFromString<U>) {
            return T::FromString(iString);
        } 
        // 3. nullptr
        else if constexpr (std::is_null_pointer_v<U>) {
            return {};
        } 
        // 4. pointer
        else if constexpr (std::is_pointer_v<U>) {
            if (iString == "nullptr") {
                return nullptr;
            } else {
                return reinterpret_cast<T>(std::stoull(std::string(iString)));
            }
        }
        // 5. bool
        else if constexpr (std::same_as<U, bool>) {
            return iString == "true";
        }
        // 6. enum
        else if constexpr (std::is_enum_v<U>) {
            static_assert(std::meta::is_enumerable_type(std::meta::dealias(^^U)));

            // a. exact match
            template for (constexpr auto e : std::define_static_array(std::meta::enumerators_of(std::meta::dealias(^^U)))) {
                if (iString == std::string_view(std::meta::display_string_of(e))) {
                    return [:e:];
                }
            }

            // b. fallback
            return static_cast<T>(std::stoull(std::string(iString)));
        }
        // 7. fallback
        else {
            static_assert(false, "Unsupported type for ToStringView");
        }
    }

    /// @brief Convenience alias: parse a scoped enum from its display name.
    template<typename T> requires std::is_enum_v<T> T Enum(std::string_view iString) {
        return FromString<T>(iString);
    }

} // namespace Mashiro

// clang-format on

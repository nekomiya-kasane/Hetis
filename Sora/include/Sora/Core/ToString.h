/**
 * @file ToString.h
 * @brief Reflection-driven, extensible string conversion framework.
 * @ingroup Core
 *
 * @details Provides three customisation-point objects: @ref ToString returns an owning @c std::string,
 * @ref ToStringView returns a non-owning @c std::string_view for stable cases, and @ref Str chooses the cheapest safe
 * representation. @ref FromString is the inverse parsing entry point. Dispatch prefers explicit hooks and ADL before
 * falling back to reflection, standard formatting facilities, pointer rendering, and final address-based diagnostics.
 */
#pragma once

#include <meta>
#include <concepts>
#include <format>
#include <memory>
#include <ostream>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <sstream>
#include <variant>

#include "Sora/Core/Traits.h"
#include "Sora/Core/FixedString.h"

namespace Sora {

    namespace $ {

        /** @brief Exclude this member from generic object display and JSON emission. */
        struct Ignore {
            constexpr bool operator==(const Ignore&) const = default;
        };

        /**
         * @brief Override the display or serialized field name used for this member.
         * @tparam S Compile-time field name.
         */
        template<FixedString S>
        struct Rename {
            static constexpr std::string_view name = S.view();
            constexpr bool operator==(const Rename&) const = default;
        };

    } // namespace $

    /** @brief Parse a value of type @p T from its string representation. */
    template<typename T>
    [[nodiscard]] constexpr T FromString(std::string_view iString);

    /**
     * @brief Open customisation point for @ref ToString.
     *
     * @details @ref Sora::ToString is a customisation-point object, not a namespace function. A same-named free
     * function or hidden friend declared directly inside @c namespace @c Sora would collide with that object. This hook
     * keeps user customisation independent from the CPO name and also supports partial specialisation for whole
     * template families, which ADL overloads and member functions cannot express.
     *
     * Specialisations provide a @c static @c ToString(const @c T&) member returning @c std::string. The primary
     * template is intentionally undefined, so an unspecialised @p T simply means "no hook" and dispatch falls
     * through to lower priority branches.
     *
     * @tparam T Cv-unqualified type to customise. This hook has the highest @ref ToString dispatch priority.
     *
     * @code{.cpp}
     * template<>
     * struct Sora::Hook::ToStringHook<UserApp::Money> {
     *     static std::string ToString(const UserApp::Money& m) {
     *         return "$" + std::to_string(m.cents);
     *     }
     * };
     * @endcode
     */
    namespace Hook {

        template<typename T>
        struct ToStringHook;

    } // namespace Hook

    namespace $ {

        /**
         * @brief Annotation that asks reflective string conversion to print the pointee instead of the pointer address.
         *
         * @code{.cpp}
         * struct Node {
         *     int value;
         *     [[=DerefPrint{}]] Node* next;
         * };
         * @endcode
         */
        struct DerefPrint {
            constexpr bool operator==(const DerefPrint&) const = default;
        };

    } // namespace $

    /** @brief Convert any string-like value to an owning @c std::string. */
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

        /** @name ADL hook detection @{ */
        namespace ADL {

            namespace FreeToString {
                /** @brief Poison pill that keeps ordinary lookup from finding a fallback overload. */
                void ToString() = delete;

                /** @brief True if @c ToString(v) is found via ADL. */
                template<typename T>
                concept Available = requires(T&& iValue) {
                    { ToString(std::forward<T>(iValue)) } -> std::convertible_to<std::string>;
                };

                template<typename T>
                    requires Available<T>
                [[nodiscard]] constexpr std::string Invoke(T&& iValue) {
                    return MakeString(ToString(std::forward<T>(iValue)));
                }
            } // namespace FreeToString

            namespace FreeToStringView {
                /** @brief Poison pill that keeps ordinary lookup from finding a fallback overload. */
                void ToStringView() = delete;

                /** @brief True if @c ToStringView(v) is found via ADL. */
                template<typename T>
                concept Available = requires(T&& iValue) {
                    { ToStringView(std::forward<T>(iValue)) } -> std::convertible_to<std::string_view>;
                };

                template<typename T>
                    requires Available<T>
                [[nodiscard]] constexpr std::string_view Invoke(T&& iValue) {
                    return ToStringView(std::forward<T>(iValue));
                }
            } // namespace FreeToStringView

            namespace FreeFromString {
                /** @brief Poison pill that keeps ordinary lookup from finding a fallback overload. */
                void FromString() = delete;

                /** @brief True if @c FromString<T>(sv) is found via ADL. */
                template<typename T>
                concept Available = requires(std::string_view iString) {
                    { FromString<T>(iString) } -> std::convertible_to<T>;
                };

                template<typename T>
                    requires Available<T>
                [[nodiscard]] constexpr T Invoke(std::string_view iString) {
                    return FromString<T>(iString);
                }
            } // namespace FreeFromString

        } // namespace ADL
        /** @} */

        /** @brief Detects a @c v.ToString() member. */
        template<typename T>
        concept HasMemberToString = requires(T&& iValue) {
            { std::forward<T>(iValue).ToString() } -> std::convertible_to<std::string>;
        };

        /** @brief Detects a @c v.ToStringView() member. */
        template<typename T>
        concept HasMemberToStringView = requires(T&& iValue) {
            { std::forward<T>(iValue).ToStringView() } -> std::convertible_to<std::string_view>;
        };

        /** @brief Detects a @c T::FromString(sv) static member. */
        template<typename T>
        concept HasMemberFromString = requires {
            { T::FromString(std::string_view{}) } -> std::convertible_to<T>;
        };

        /** @brief Detects a usable @ref Sora::Hook::ToStringHook specialisation for @p T. */
        template<typename T>
        concept HasHookToString = requires(const T& iValue) {
            { Hook::ToStringHook<T>::ToString(iValue) } -> std::convertible_to<std::string>;
        };

        /**
         * @brief Types whose @c ToStringView result is a view over static / stable
         *        storage (won't dangle). Everything else must go through @c ToString.
         */
        template<typename T>
        concept ViewStringable =
            ADL::FreeToStringView::Available<std::remove_cvref_t<T>> || HasMemberToStringView<std::remove_cvref_t<T>> ||
            std::is_null_pointer_v<std::remove_cvref_t<T>> || std::same_as<std::remove_cvref_t<T>, bool> ||
            std::is_enum_v<std::remove_cvref_t<T>>;

        /** @brief Return the display name used for reflected field @p M. */
        template<std::meta::info M>
        consteval std::string_view FieldNameOf() {
            std::string_view name = std::meta::has_identifier(M) ? Meta::IdentifierOf(M) : Meta::DisplayStringOf(M);
            template for (constexpr auto a : std::define_static_array(std::meta::annotations_of(M))) {
                using A = typename [:std::meta::type_of(a):];
                if constexpr (requires { A::name; }) {
                    name = A::name;
                }
            }
            return name;
        }
        /** @brief Core dispatch from a value to an owning @c std::string. */
        template<typename T>
        [[nodiscard]] constexpr std::string ToStringImpl(T&& iValue) {
            using U = std::remove_cvref_t<T>;

            // 0. ToStringHook<U> specialisation, highest-priority open customisation point.
            if constexpr (Detail::HasHookToString<U>) {
                return MakeString(Hook::ToStringHook<U>::ToString(iValue));
            }
            // 1. free ToString(...) via ADL
            else if constexpr (Detail::ADL::FreeToString::Available<U>) {
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
                template for (constexpr auto e : Sora::Meta::EnumeratorsOf(std::meta::dealias(^^U))) {
                    if ([:e:] == iValue) {
                        return std::string(Sora::Meta::DisplayStringOf(e));
                    }
                }

                // b. bitmask decomposition
                std::stringstream ss;
                bool first = true;
                auto remaining = static_cast<Underlying>(iValue);

                template for (constexpr auto e : Sora::Meta::EnumeratorsOf(std::meta::dealias(^^U))) {
                    constexpr auto flag = static_cast<Underlying>([:e:]);
                    if constexpr (flag != Underlying{0}) {
                        if ((remaining & flag) == flag) {
                            ss << (first ? "" : " | ") << Sora::Meta::DisplayStringOf(e);
                            first = false;
                            remaining &= static_cast<Underlying>(~flag);
                        }
                    }
                }

                if (!first && remaining == Underlying{0}) {
                    return ss.str();
                }

                // c. fallback
                return std::format("{}(unknown:{})", Traits::TypeName<U>, static_cast<Underlying>(iValue));
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
            else if constexpr (Concept::TupleLikeClass<U>) {
                static_assert(!std::ranges::range<U>);

                std::stringstream ss;
                ss << "(";

                bool first = false;

                std::apply(
                    [&]<typename... Ts>(Ts&&... args) {
                        ((ss << (first ? ", " : "") << ToStringImpl(std::forward<decltype(args)>(args)), first = true),
                         ...);
                    },
                    iValue);

                ss << ")";
                return ss.str();
            }
            // 10. variant
            else if constexpr (Concept::VariantLikeClass<U>) {
                if (iValue.valueless_by_exception()) {
                    return std::format("{}(valueless)", Traits::TypeName<U>);
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
                    return std::format("{}*(nullptr)", Traits::TypeName<std::remove_pointer_t<U>>);
                } else {
                    return std::format("{}*({:p})", Traits::TypeName<std::remove_pointer_t<U>>,
                                       static_cast<const void*>(iValue));
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
                ss << Traits::TypeName<U> << " {";

                template for (bool first = true; constexpr auto m : Traits::DataMembers<U>) {
                    if constexpr (!$::Has<$::Ignore>(m)) {
                        if (!first) {
                            ss << ", ";
                        }
                        first = false;

                        ss << FieldNameOf<m>();

                        if constexpr (std::meta::is_bit_field(m)) {
                            ss << "=" << ToStringImpl(auto(iValue.[:m:]));
                        } else if constexpr (std::is_pointer_v<typename [:std::meta::type_of(m):]> &&
                                             $::Has<$::DerefPrint>(m)) {
                            auto* ptr = iValue.[:m:];
                            if (ptr == nullptr) {
                                ss << "=nullptr";
                            } else {
                                ss << "=" << ToStringImpl(*ptr);
                            }
                        } else {
                            ss << "=" << ToStringImpl(iValue.[:m:]);
                        }
                    }
                }

                ss << "}";
                return ss.str();
            }
            // 15. fallback
            else {
                return std::format("<{} at {:p}>", Traits::TypeName<U>,
                                   static_cast<const void*>(std::addressof(iValue)));
            }
        }

        /** @brief Core dispatch from a value to a non-owning @c std::string_view for supported stable cases. */
        template<typename T>
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
                template for (constexpr auto e : Meta::EnumeratorsOf(std::meta::dealias(^^U))) {
                    if ([:e:] == iValue) {
                        return std::string_view(Meta::DisplayStringOf(e));
                    }
                }

                // b. fallback
                return Traits::TypeName<U>;
            }
            // 6. fallback
            else {
                static_assert(false, "Unsupported type for ToStringView");
            }
        }

        /** @brief CPO functor that implements @c ToString(v) -> @c std::string. */
        struct ToStringFn {
            template<typename T>
            [[nodiscard]] constexpr std::string operator()(T&& iValue) const {
                return ToStringImpl(std::forward<T>(iValue));
            }
        };

        /** @brief CPO functor that implements @c ToStringView(v) -> @c std::string_view. */
        struct ToStringViewFn {
            template<typename T>
            [[nodiscard]] constexpr std::string_view operator()(T&& iValue) const {
                return ToStringViewImpl(std::forward<T>(iValue));
            }
        };

    } // namespace Detail
    /** @endcond */

    /** @brief Customisation-point object that converts any supported value to @c std::string. */
    inline constexpr Detail::ToStringFn ToString{};
    /** @brief Customisation-point object that converts supported stable values to @c std::string_view. */
    inline constexpr Detail::ToStringViewFn ToStringView{};

    /**
     * @brief Smart string conversion: returns @c string_view when safe, @c string otherwise.
     * @tparam T Deduced value type.
     * @return @c std::string_view for ViewStringable types, @c std::string for everything else.
     */
    template<typename T>
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
     * @details Dispatch priority mirrors @ref ToString in reverse: ADL @c FromString<T>(sv), static
     * @c T::FromString(sv), @c nullptr, pointer, @c bool, enum reflection match, then a hard failure.
     *
     * @tparam T Target type.
     * @param[in] iString Input string.
     * @return Parsed value of type @p T.
     */
    template<typename T>
    [[nodiscard]] constexpr T FromString(std::string_view iString) {
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
            template for (constexpr auto e : Meta::EnumeratorsOf(std::meta::dealias(^^U))) {
                if (iString == std::string_view(Meta::DisplayStringOf(e))) {
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

    /** @brief Convenience alias that parses a scoped enum from its display name. */
    template<typename T>
        requires std::is_enum_v<T>
    T Enum(std::string_view iString) {
        return FromString<T>(iString);
    }

} // namespace Sora

/**
 * @brief Auto-specialise @c std::formatter for Sora-reflectable types.
 *
 * @details Enables @c std::format("{}", value) for scoped enums, complete classes with reflectable members, and
 * types with an ADL @c ToString() overload or @c .ToString() member. Arithmetic types, string-like types, and
 * standard library types that already have formatters are excluded.
 */
template<typename T>
    requires(!std::is_arithmetic_v<T> && !std::convertible_to<T, std::string_view> &&
             !std::convertible_to<T, std::string> &&
             (std::is_enum_v<T> || (std::is_class_v<T> && !std::is_union_v<T> && requires { sizeof(T); })))
struct std::formatter<T> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    auto format(const T& value, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", Sora::ToString(value));
    }
};
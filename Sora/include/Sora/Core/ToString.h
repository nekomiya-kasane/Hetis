/**
 * @file ToString.h
 * @brief Reflection-driven UTF-8 string conversion and strict deserialization.
 * @ingroup Core
 *
 * @details @ref ToString materializes a UTF-8 display representation, @ref ToStringView exposes stable text without
 * allocation, and @ref Str chooses the cheapest safe representation. @ref FromString is the single fallible inverse
 * operation and reports the project-wide @ref ErrorCode rather than maintaining a parallel text-codec error model.
 */
#pragma once

#include "Sora/Core/FixedString.h"
#include "Sora/Core/StringUtils.h"
#include "Sora/Core/Traits.h"
#include "Sora/Core/Unicode.h"
#include "Sora/ErrorCode.h"

#include <array>
#include <charconv>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <meta>
#include <optional>
#include <ostream>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <typeinfo>
#include <type_traits>
#include <utility>
#include <variant>

namespace Sora {

    namespace $::Serialization {

        /** @brief Exclude this member from generic object display and JSON emission. */
        struct Ignore {
            constexpr bool operator==(const Ignore&) const = default;
        };

        /**
         * @brief Override the display or serialized field name used for this member.
         * @tparam S Compile-time field name.
         */
        template<auto S>
            requires Concept::FixedStringLike<decltype(S)>
        struct Rename {
            static constexpr std::string_view name = S.view();
            constexpr bool operator==(const Rename&) const = default;
        };

        /**
         * @brief Ask reflective string conversion to print a pointee instead of its pointer address.
         * @details Polymorphic base pointers use a callable virtual @c ToString when available; otherwise rendering
         * appends an explicit incomplete-view diagnostic whenever the dynamic object is or may be more derived.
         */
        struct DerefPrint {
            constexpr bool operator==(const DerefPrint&) const = default;
        };

    } // namespace $::Serialization

    /**
     * @brief Open specialization point for custom owning string conversion.
     *
     * @details A specialization provides a static @c ToString(const T&) member returning an owning string. This hook
     * has priority over ADL and member conversion and supports customization of complete template families.
     *
     * @tparam T Cv-unqualified type to customize.
     */
    namespace Hook {

        template<typename T>
        struct ToStringHook;

    } // namespace Hook

    namespace Meta {

        /** @brief Return the display or serialization name of reflected field @p Member. */
        template<std::meta::info Member>
        consteval std::string_view SerializationFieldNameOf() {
            std::string_view name = IdentifierOrDisplayStringOf(Member);
            template for (constexpr auto annotation : std::define_static_array(std::meta::annotations_of(Member))) {
                constexpr auto type = std::meta::dealias(std::meta::type_of(annotation));
                if constexpr (std::meta::has_template_arguments(type) &&
                              std::meta::template_of(type) == ^^$::Serialization::Rename) {
                    using Rename = typename [:type:];
                    name = Rename::name;
                }
            }
            return name;
        }

        /** @brief Return whether @p T's hierarchy declares a virtual nullary @c ToString member. */
        template<typename T>
        inline constexpr bool HasVirtualToStringMember = [] consteval {
            using U = std::remove_cv_t<T>;
            if constexpr (requires { sizeof(U); }) {
                return HasVirtualMemberFunctionInHierarchy(^^U, "ToString", 0);
            } else {
                return false;
            }
        }();

    } // namespace Meta

    /** @cond INTERNAL */
    namespace Detail {

        inline constexpr auto kDisplayUnicodePolicy = Unicode::InvalidSequencePolicy::Replace;

        /** @brief Materialize an owning string while preserving an existing string allocation. */
        template<typename T>
        [[nodiscard]] constexpr std::string MakeString(T&& value) {
            if constexpr (std::same_as<std::remove_cvref_t<T>, std::string>) {
                return std::forward<T>(value);
            } else {
                return std::string(std::forward<T>(value));
            }
        }

        namespace ADL {

            namespace FreeToString {

                void ToString() = delete;

                template<typename T>
                concept Available = requires(T&& value) {
                    { ToString(std::forward<T>(value)) } -> std::convertible_to<std::string>;
                };

                template<typename T>
                    requires Available<T>
                [[nodiscard]] constexpr std::string Invoke(T&& value) {
                    return MakeString(ToString(std::forward<T>(value)));
                }

            } // namespace FreeToString

            namespace FreeToStringView {

                void ToStringView() = delete;

                template<typename T>
                concept Available = requires(T&& value) {
                    { ToStringView(std::forward<T>(value)) } -> std::convertible_to<std::string_view>;
                };

                template<typename T>
                    requires Available<T>
                [[nodiscard]] constexpr std::string_view Invoke(T&& value) {
                    return ToStringView(std::forward<T>(value));
                }

            } // namespace FreeToStringView

            namespace FreeFromString {

                void FromString() = delete;

                template<typename T>
                concept Available = requires(T& value, std::string_view text) {
                    FromString(value, text);
                    requires std::same_as<decltype(FromString(value, text)), bool> ||
                                 std::same_as<decltype(FromString(value, text)), VoidResult>;
                };

                template<typename T>
                    requires Available<T>
                [[nodiscard]] constexpr auto Invoke(T& value, std::string_view text) {
                    return FromString(value, text);
                }

            } // namespace FreeFromString

        } // namespace ADL

        template<typename T>
        concept HasMemberToString = requires(T&& value) {
            { std::forward<T>(value).ToString() } -> std::convertible_to<std::string>;
        };

        template<typename T>
        concept HasMemberToStringView = requires(T&& value) {
            { std::forward<T>(value).ToStringView() } -> std::convertible_to<std::string_view>;
        };

        template<typename T>
        concept HasHookToString = requires(const T& value) {
            { Hook::ToStringHook<T>::ToString(value) } -> std::convertible_to<std::string>;
        };

        template<typename T, typename R>
        concept StringDeserializationResult =
            std::same_as<std::remove_cvref_t<R>, T> || std::same_as<std::remove_cvref_t<R>, std::optional<T>> ||
            std::same_as<std::remove_cvref_t<R>, Result<T>>;

        template<typename T>
        concept HasStaticFromString =
            requires(std::string_view text) { requires StringDeserializationResult<T, decltype(T::FromString(text))>; };

        template<typename T>
        concept SafeNarrowView =
            std::same_as<std::remove_cvref_t<T>, std::string_view> ||
            (std::same_as<std::remove_cvref_t<T>, std::string> && std::is_lvalue_reference_v<T>) ||
            (std::is_pointer_v<std::remove_reference_t<T>> &&
             std::same_as<std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<T>>>, char>) ||
            (std::is_array_v<std::remove_reference_t<T>> &&
             std::same_as<std::remove_cv_t<std::remove_extent_t<std::remove_reference_t<T>>>, char>);

    } // namespace Detail
    /** @endcond */

    namespace Traits {

        /** @brief Whether @p T has a built-in strict UTF-8 string deserializer. */
        template<typename T>
        inline constexpr bool BuiltinStringDeserializable =
            std::same_as<std::remove_cvref_t<T>, std::string_view> ||
            std::same_as<std::remove_cvref_t<T>, std::string> || std::same_as<std::remove_cvref_t<T>, std::u8string> ||
            std::same_as<std::remove_cvref_t<T>, std::u16string> ||
            std::same_as<std::remove_cvref_t<T>, std::u32string> ||
            std::same_as<std::remove_cvref_t<T>, std::wstring> ||
            std::same_as<std::remove_cvref_t<T>, std::filesystem::path> ||
            std::is_arithmetic_v<std::remove_cvref_t<T>> || std::is_enum_v<std::remove_cvref_t<T>>;

    } // namespace Traits

    namespace Concept {

        /** @brief Type that explicitly customizes @ref ToString through a hook, ADL, or member function. */
        template<typename T>
        concept CustomStringFormattable = Detail::HasHookToString<std::remove_cvref_t<T>> ||
                                          Detail::ADL::FreeToString::Available<T> || Detail::HasMemberToString<T>;

        /** @brief Type with a canonical, non-reflective @ref ToString representation. */
        template<typename T>
        concept StringFormattable =
            CustomStringFormattable<T> || NarrowStringLike<T> || Utf8StringLike<T> || Utf16StringLike<T> ||
            Utf32StringLike<T> || WideStringLike<T> || std::same_as<std::remove_cvref_t<T>, std::filesystem::path> ||
            std::is_arithmetic_v<std::remove_cvref_t<T>> || std::is_enum_v<std::remove_cvref_t<T>>;

        /**
         * @brief A runtime lvalue compatible with the declared type of a reflected named variable.
         * @tparam Variable Reflection of the variable declaration.
         * @tparam T Runtime lvalue type supplied for rendering.
         */
        template<std::meta::info Variable, typename T>
        concept ReflectedVariableValue =
            std::meta::is_variable(Variable) && std::meta::has_identifier(Variable) &&
            std::meta::remove_reference(std::meta::type_of(Variable)) == std::meta::remove_reference(^^T);

        /** @brief Type accepted by strict @ref FromString deserialization. */
        template<typename T>
        concept StringDeserializable =
            Traits::BuiltinStringDeserializable<T> || Detail::HasStaticFromString<std::remove_cvref_t<T>> ||
            (std::default_initializable<std::remove_cvref_t<T>> &&
             Detail::ADL::FreeFromString::Available<std::remove_cvref_t<T>>);

        /** @brief Value category from which @ref ToStringView can expose stable storage without allocation. */
        template<typename T>
        concept StringViewable = Detail::ADL::FreeToStringView::Available<T> || Detail::HasMemberToStringView<T> ||
                                 Detail::SafeNarrowView<T> || std::is_null_pointer_v<std::remove_cvref_t<T>> ||
                                 std::same_as<std::remove_cvref_t<T>, bool> || std::is_enum_v<std::remove_cvref_t<T>>;

        /** @brief Complete non-scalar type covered by Sora's generated formatter and stream insertion bridge. */
        template<typename T>
        concept AutoDisplayable =
            !std::is_arithmetic_v<std::remove_cvref_t<T>> &&
            !std::convertible_to<std::remove_cvref_t<T>, std::string_view> &&
            !std::convertible_to<std::remove_cvref_t<T>, std::string> &&
            (std::is_enum_v<std::remove_cvref_t<T>> ||
             (std::is_class_v<std::remove_cvref_t<T>> && !std::is_union_v<std::remove_cvref_t<T>> &&
              requires { sizeof(std::remove_cvref_t<T>); }));

    } // namespace Concept

    /** @cond INTERNAL */
    namespace Detail {

        enum class PointeeViewCompleteness : uint8_t {
            Complete,
            Incomplete,
            Indeterminate,
        };

        template<typename T>
        [[nodiscard]] constexpr std::string ToStringImpl(T&& value);

        template<typename T>
        [[nodiscard]] constexpr std::string ReflectiveClassToString(const T& value);

        /** @brief Classify whether a statically typed pointer view can represent the complete dynamic object. */
        template<typename Pointer>
        [[nodiscard]] PointeeViewCompleteness PointeeCompleteness(Pointer pointer) {
            using Pointee = std::remove_cv_t<std::remove_pointer_t<Pointer>>;
            if constexpr (!std::is_class_v<Pointee>) {
                return PointeeViewCompleteness::Complete;
            } else if constexpr (!requires { sizeof(Pointee); }) {
                return PointeeViewCompleteness::Indeterminate;
            } else if constexpr (std::is_final_v<Pointee> ||
                                 (Meta::HasVirtualToStringMember<Pointee> && HasMemberToString<decltype(*pointer)>)) {
                return PointeeViewCompleteness::Complete;
            } else if constexpr (std::is_polymorphic_v<Pointee>) {
                return typeid(*pointer) == typeid(Pointee) ? PointeeViewCompleteness::Complete
                                                           : PointeeViewCompleteness::Incomplete;
            } else {
                return PointeeViewCompleteness::Indeterminate;
            }
        }

        /** @brief Render an explicitly dereferenced pointer without silently hiding a possible dynamic subobject. */
        template<typename Pointer>
        [[nodiscard]] constexpr std::string DereferencedPointerToString(Pointer pointer) {
            using Pointee = std::remove_cv_t<std::remove_pointer_t<Pointer>>;
            std::string result;
            if constexpr (std::is_class_v<Pointee> && Meta::HasVirtualToStringMember<Pointee> &&
                          HasMemberToString<decltype(*pointer)>) {
                result = MakeString(pointer->ToString());
            } else {
                result = ToStringImpl(*pointer);
            }

            switch (PointeeCompleteness(pointer)) {
                case PointeeViewCompleteness::Complete:
                    break;
                case PointeeViewCompleteness::Incomplete:
                    result += std::format(" <incomplete: dynamic object rendered through non-virtual {} view>",
                                          Traits::TypeName<Pointee>);
                    break;
                case PointeeViewCompleteness::Indeterminate:
                    result += std::format(" <possibly incomplete: dynamic type is not observable through {}>",
                                          Traits::TypeName<Pointee>);
                    break;
            }
            return result;
        }

        template<typename T>
            requires std::integral<T> && (!std::same_as<T, bool>) && (!std::same_as<T, char>)
        [[nodiscard]] constexpr Result<T> IntegerFromString(std::string_view text) noexcept {
            T value{};
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 10);
            if (error == std::errc::result_out_of_range) {
                return std::unexpected(ErrorCode::OutOfRange);
            }
            if (error != std::errc{} || end != text.data() + text.size()) {
                return std::unexpected(ErrorCode::InvalidSyntax);
            }
            return value;
        }

        template<std::floating_point T>
        [[nodiscard]] constexpr Result<T> FloatingPointFromString(std::string_view text) noexcept {
            T value{};
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
            if (error == std::errc::result_out_of_range) {
                return std::unexpected(ErrorCode::OutOfRange);
            }
            if (error != std::errc{} || end != text.data() + text.size()) {
                return std::unexpected(ErrorCode::InvalidSyntax);
            }
            return value;
        }

        template<typename T>
            requires std::integral<T> || std::floating_point<T>
        [[nodiscard]] constexpr std::string ArithmeticToString(T value) {
            std::array<char, 128> buffer{};
            const auto result = [&] {
                if constexpr (std::floating_point<T>) {
                    return std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                         std::chars_format::general);
                } else {
                    return std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
                }
            }();
            return std::string(buffer.data(), result.ptr);
        }

        template<typename T>
        [[nodiscard]] constexpr std::string ToStringImpl(T&& value) {
            using U = std::remove_cvref_t<T>;

            if constexpr (HasHookToString<U>) {
                return MakeString(Hook::ToStringHook<U>::ToString(value));
            } else if constexpr (ADL::FreeToString::Available<T>) {
                return ADL::FreeToString::Invoke(std::forward<T>(value));
            } else if constexpr (HasMemberToString<T>) {
                return MakeString(std::forward<T>(value).ToString());
            } else if constexpr (std::is_null_pointer_v<U>) {
                return "nullptr";
            } else if constexpr (std::same_as<U, bool>) {
                return value ? "true" : "false";
            } else if constexpr (std::same_as<U, char>) {
                return std::string(1, value);
            } else if constexpr (Concept::NarrowStringLike<T>) {
                return std::string(std::string_view{std::forward<T>(value)});
            } else if constexpr (Concept::Utf8StringLike<T>) {
                return Unicode::Utf8BytesToString(std::u8string_view{std::forward<T>(value)});
            } else if constexpr (Concept::Utf16StringLike<T>) {
                return *Unicode::Utf16ToUtf8<kDisplayUnicodePolicy>(std::u16string_view{std::forward<T>(value)});
            } else if constexpr (Concept::Utf32StringLike<T>) {
                return *Unicode::Utf32ToUtf8<kDisplayUnicodePolicy>(std::u32string_view{std::forward<T>(value)});
            } else if constexpr (Concept::WideStringLike<T>) {
                return *Unicode::WideToUtf8<kDisplayUnicodePolicy>(std::wstring_view{std::forward<T>(value)});
            } else if constexpr (std::same_as<U, std::filesystem::path>) {
                if constexpr (std::same_as<std::filesystem::path::value_type, wchar_t>) {
                    return *Unicode::WideToUtf8<kDisplayUnicodePolicy>(std::wstring_view{value.native()});
                } else {
                    return value.native();
                }
            } else if constexpr (std::is_enum_v<U>) {
                return Traits::EnumToString(value);
            } else if constexpr (std::integral<U> || std::floating_point<U>) {
                return ArithmeticToString(value);
            } else if constexpr (std::ranges::range<U>) {
                std::string result = "[";
                bool first = true;
                for (auto&& item : value) {
                    if (!first) {
                        result += ", ";
                    }
                    first = false;
                    result += ToStringImpl(item);
                }
                result += "]";
                return result;
            } else if constexpr (Concept::TupleLikeClass<U>) {
                std::string result = "(";
                bool first = true;
                std::apply(
                    [&]<typename... Elements>(Elements&&... elements) {
                        ((result += std::exchange(first, false) ? "" : ", ",
                          result += ToStringImpl(std::forward<Elements>(elements))),
                         ...);
                    },
                    value);
                result += ")";
                return result;
            } else if constexpr (Concept::VariantLikeClass<U>) {
                if (value.valueless_by_exception()) {
                    return std::format("{}(valueless)", Traits::TypeName<U>);
                }
                return std::visit(
                    [](auto&& alternative) { return ToStringImpl(std::forward<decltype(alternative)>(alternative)); },
                    std::forward<T>(value));
            } else if constexpr (std::is_pointer_v<U>) {
                if (value == nullptr) {
                    return std::format("{}*(nullptr)", Traits::TypeName<std::remove_pointer_t<U>>);
                } else if constexpr (std::is_object_v<std::remove_pointer_t<U>>) {
                    return std::format("{}*({:p})", Traits::TypeName<std::remove_pointer_t<U>>,
                                       static_cast<const void*>(value));
                } else {
                    return std::format("{}*(function)", Traits::TypeName<std::remove_pointer_t<U>>);
                }
            } else if constexpr (!Concept::AutoDisplayable<U> &&
                                 requires(std::ostream& stream, T&& item) { stream << std::forward<T>(item); }) {
                std::ostringstream stream;
                stream << std::forward<T>(value);
                return std::move(stream).str();
            } else if constexpr (std::is_class_v<U> && !std::is_union_v<U> && requires { sizeof(U); }) {
                return ReflectiveClassToString(value);
            } else if constexpr (std::is_object_v<U>) {
                return std::format("<{} at {:p}>", Traits::TypeName<U>,
                                   static_cast<const void*>(std::addressof(value)));
            } else {
                return std::string(Traits::TypeName<U>);
            }
        }

        template<typename T>
        [[nodiscard]] constexpr std::string ReflectiveClassToString(const T& value) {
            using U = std::remove_cv_t<T>;
            std::string result = std::string(Traits::TypeName<U>) + " {";
            bool first = true;
            template for (constexpr auto member : Traits::DataMembers<U>) {
                if constexpr (!$::Has<$::Serialization::Ignore>(member)) {
                    if (!std::exchange(first, false)) {
                        result += ", ";
                    }
                    result += Meta::SerializationFieldNameOf<member>();
                    result += "=";
                    if constexpr (std::meta::is_bit_field(member)) {
                        result += ToStringImpl(auto(value.[:member:]));
                    } else if constexpr (std::is_pointer_v<typename [:std::meta::type_of(member):]> &&
                                         $::Has<$::Serialization::DerefPrint>(member)) {
                        auto* pointer = value.[:member:];
                        result += pointer == nullptr ? "nullptr" : DereferencedPointerToString(pointer);
                    } else {
                        result += ToStringImpl(value.[:member:]);
                    }
                }
            }
            template for (constexpr auto base : Meta::BasesOf(^^U, std::meta::access_context::unprivileged())) {
                using Base = typename [:std::meta::type_of(base):];
                if (!std::exchange(first, false)) {
                    result += ", ";
                }
                result += "base[";
                result += Traits::TypeName<Base>;
                result += "]=";
                if constexpr (std::is_convertible_v<const U*, const Base*>) {
                    result += ReflectiveClassToString(static_cast<const Base&>(value));
                } else {
                    result += "<incomplete: ambiguous base subobject>";
                }
            }
            if constexpr (Meta::HasInaccessibleDirectBases(^^U)) {
                if (!std::exchange(first, false)) {
                    result += ", ";
                }
                result += "base[<inaccessible>]=<incomplete>";
            }
            result += "}";
            return result;
        }

        template<typename T>
        [[nodiscard]] constexpr std::string_view ToStringViewImpl(T&& value) {
            using U = std::remove_cvref_t<T>;
            if constexpr (ADL::FreeToStringView::Available<T>) {
                return ADL::FreeToStringView::Invoke(std::forward<T>(value));
            } else if constexpr (HasMemberToStringView<T>) {
                return std::forward<T>(value).ToStringView();
            } else if constexpr (SafeNarrowView<T>) {
                return std::string_view{std::forward<T>(value)};
            } else if constexpr (std::is_null_pointer_v<U>) {
                return "nullptr";
            } else if constexpr (std::same_as<U, bool>) {
                return value ? "true" : "false";
            } else if constexpr (std::is_enum_v<U>) {
                return Traits::EnumToStringView(value);
            } else {
                static_assert(false, "Type does not expose a stable string view.");
            }
        }

        template<typename T>
            requires Concept::StringDeserializable<T>
        [[nodiscard]] constexpr Result<T> FromStringImpl(std::string_view text) {
            if constexpr (HasStaticFromString<T>) {
                auto&& parsed = T::FromString(text);
                using Parsed = std::remove_cvref_t<decltype(parsed)>;
                if constexpr (std::same_as<Parsed, std::optional<T>>) {
                    if (!parsed) {
                        return std::unexpected(ErrorCode::InvalidSyntax);
                    }
                    return std::move(*parsed);
                } else {
                    return std::forward<decltype(parsed)>(parsed);
                }
            } else if constexpr (std::default_initializable<T> && ADL::FreeFromString::Available<T>) {
                T value{};
                auto status = ADL::FreeFromString::Invoke(value, text);
                if constexpr (std::same_as<decltype(status), bool>) {
                    return status ? Result<T>{std::move(value)} : std::unexpected(ErrorCode::InvalidSyntax);
                } else {
                    if (!status) {
                        return std::unexpected(status.error());
                    }
                    return value;
                }
            } else if constexpr (std::same_as<T, bool>) {
                if (Ascii::EqualsIgnoreCase(text, "true") || Ascii::EqualsIgnoreCase(text, "yes") ||
                    Ascii::EqualsIgnoreCase(text, "on") || text == "1") {
                    return true;
                }
                if (Ascii::EqualsIgnoreCase(text, "false") || Ascii::EqualsIgnoreCase(text, "no") ||
                    Ascii::EqualsIgnoreCase(text, "off") || text == "0") {
                    return false;
                }
                return std::unexpected(ErrorCode::InvalidSyntax);
            } else if constexpr (std::same_as<T, char>) {
                return text.size() == 1 ? Result<char>{text.front()} : std::unexpected(ErrorCode::InvalidSyntax);
            } else if constexpr (std::integral<T>) {
                return IntegerFromString<T>(text);
            } else if constexpr (std::floating_point<T>) {
                return FloatingPointFromString<T>(text);
            } else if constexpr (std::is_enum_v<T>) {
                if (auto value = Meta::EnumCast<T>(text); value) {
                    return *value;
                }
                using Underlying = std::underlying_type_t<T>;
                auto underlying = IntegerFromString<Underlying>(text);
                return underlying ? Result<T>{static_cast<T>(*underlying)} : std::unexpected(underlying.error());
            } else if constexpr (std::same_as<T, std::string_view>) {
                if (auto valid = Unicode::ValidateUtf8(text); !valid) {
                    return std::unexpected(valid.error());
                }
                return text;
            } else if constexpr (std::same_as<T, std::string>) {
                if (auto valid = Unicode::ValidateUtf8(text); !valid) {
                    return std::unexpected(valid.error());
                }
                return std::string{text};
            } else if constexpr (std::same_as<T, std::u8string>) {
                if (auto valid = Unicode::ValidateUtf8(text); !valid) {
                    return std::unexpected(valid.error());
                }
                std::u8string result;
                result.reserve(text.size());
                for (char codeUnit : text) {
                    result.push_back(static_cast<char8_t>(codeUnit));
                }
                return result;
            } else if constexpr (std::same_as<T, std::u16string>) {
                return Unicode::Utf8ToUtf16(text);
            } else if constexpr (std::same_as<T, std::u32string>) {
                return Unicode::Utf8ToUtf32(text);
            } else if constexpr (std::same_as<T, std::wstring>) {
                return Unicode::Utf8ToWide(text);
            } else if constexpr (std::same_as<T, std::filesystem::path>) {
                if constexpr (std::same_as<std::filesystem::path::value_type, wchar_t>) {
                    auto native = Unicode::Utf8ToWide(text);
                    return native ? Result<T>{T{std::move(*native)}} : std::unexpected(native.error());
                } else {
                    if (auto valid = Unicode::ValidateUtf8(text); !valid) {
                        return std::unexpected(valid.error());
                    }
                    return T{std::string{text}};
                }
            } else {
                static_assert(false, "Type is not deserializable from a string.");
            }
        }

    } // namespace Detail
    /** @endcond */

    namespace CPO {

        /** @brief Function object implementing the @ref Sora::ToString customization point. */
        struct ToStringFn {
            template<typename T>
            [[nodiscard]] constexpr std::string operator()(T&& value) const {
                return Detail::ToStringImpl(std::forward<T>(value));
            }
        };

        /** @brief Function object implementing the @ref Sora::ToStringView customization point. */
        struct ToStringViewFn {
            template<Concept::StringViewable T>
            [[nodiscard]] constexpr std::string_view operator()(T&& value) const {
                return Detail::ToStringViewImpl(std::forward<T>(value));
            }
        };

        /** @brief Function object implementing the @ref Sora::FromString customization point. */
        struct FromStringFn {
            template<Concept::StringDeserializable T>
            [[nodiscard]] constexpr Result<T> operator()(std::in_place_type_t<T>, std::string_view text) const {
                return Detail::FromStringImpl<T>(text);
            }
        };

    } // namespace CPO

    /** @brief Customization-point object that converts a supported value to an owning UTF-8 string. */
    inline constexpr CPO::ToStringFn ToString{};

    /** @brief Customization-point object that exposes a stable string view without allocation. */
    inline constexpr CPO::ToStringViewFn ToStringView{};

    /** @brief Customization-point object that strictly deserializes UTF-8 text to an explicitly tagged target type. */
    inline constexpr CPO::FromStringFn FromString{};

    namespace Meta {

        /**
         * @brief Render a reflected named variable and its current value as an unambiguous diagnostic binding.
         * @details The reflection supplies the source name and declared type while @p value supplies the current
         * runtime value. C++ reflection does not carry an automatic variable's address, so both inputs are necessary.
         * @tparam Variable Reflection of the variable declaration.
         * @tparam T Declared runtime value type, including cv-qualification.
         * @param[in] value Current value of the reflected variable.
         * @return Diagnostic text in the form @c variable[name @c : @c type @c = @c value].
         */
        template<std::meta::info Variable, typename T, typename AbiToken = ReflectedVariableToken<Variable>>
            requires Concept::ReflectedVariableValue<Variable, T> &&
                     std::same_as<AbiToken, ReflectedVariableToken<Variable>>
        [[nodiscard]] std::string ToString(T& value) {
            return std::format("variable[{} : {} = {}]", IdentifierOf(Variable), DisplayStringOf(TypeOf(Variable)),
                               Sora::ToString(value));
        }

    } // namespace Meta

    /** @brief Return a stable string view when safe and otherwise materialize an owning string. */
    template<typename T>
    [[nodiscard]] constexpr auto Str(T&& value) {
        if constexpr (Concept::StringViewable<T>) {
            return ToStringView(std::forward<T>(value));
        } else {
            return ToString(std::forward<T>(value));
        }
    }

} // namespace Sora

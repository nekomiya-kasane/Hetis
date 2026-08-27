/**
 * @file MetaError.h
 * @brief Reflection-native error values and META-APPLY short-circuit laws.
 * @ingroup Core
 *
 * @details A meta algorithm returns a `std::meta::info`. The result may be a
 * normal entity or a specialization of @ref Sora::Meta::Error::MetaError. An
 * error stores reflected entities as structural `auto` non-type arguments;
 * this permits dynamic compile-time construction from reflection ranges.
 */
#pragma once

#include <array>
#include <cstddef>
#include <meta>
#include <string>
#include <type_traits>
#include <vector>

#include <Sora/Core/Traits/MetaInfo.h>

namespace Sora {

    namespace Meta {

        inline namespace Error {

            /** @brief Neutral marker used when a computation has no error. */
            struct MetaSuccess {};

            /**
             * @brief Reflection-native error carrying a category and provenance.
             * @tparam Tag A reflected error-category entity, normally produced by `^^TagType`.
             * @tparam Contexts Reflected templates, values, types, members, or intermediate results.
             * @code{.cpp}
             * struct MissingMember {};
             * using Failure = Sora::Meta::InfoType<
             *     ^^Sora::Meta::Error::MetaError<^^MissingMember, ^^int>>;
             * @endcode
             */
            template<auto Tag, auto... Contexts>
            struct MetaError {};

            /** @brief Category for an invalid template application. */
            struct NonInvocableTag {};

            /** @brief Category for a valid application whose result is not a type. */
            struct NonTypeResultTag {};

            /** @brief Category for a predicate whose reflected result is not `bool`. */
            struct NonBooleanPredResultTag {};

            /** @brief Category for an algorithm result that is not a reflected InfoList value. */
            struct NonInfoListResultTag {};

            /** @brief Category for an input that is not the required sequence representation. */
            struct InvalidSequenceTag {};

            /** @brief Category for a multi-sequence operation whose lengths differ. */
            struct LengthMismatchTag {};

            /** @brief Category for a sequence operation whose requested bounds are outside the input. */
            struct OutOfRangeTag {};

        } // namespace Error

    } // namespace Meta

    namespace Meta {

        /**
         * @brief Build a reflected error from a tag and a compile-time context range.
         * @param[in] tag Reflection of the category entity.
         * @param[in] contexts Provenance reflections in source order.
         * @return Reflection of `Error::MetaError<tag, contexts...>`.
         * @par Example
         * @code{.cpp}
         * constexpr auto error = Sora::Meta::MakeMetaError(^^Sora::Meta::Error::NonInvocableTag, {^^int});
         * static_assert(Sora::Meta::IsMetaError(error));
         * @endcode
         */
        [[nodiscard]] consteval std::meta::info MakeMetaError(std::meta::info tag,
                                                              std::vector<std::meta::info> contexts = {}) {
            std::vector<std::meta::info> arguments;
            arguments.reserve(contexts.size() + 1);
            arguments.push_back(std::meta::reflect_constant(tag));
            for (const auto context : contexts) {
                arguments.push_back(std::meta::reflect_constant(context));
            }
            return std::meta::substitute(^^Error::MetaError, arguments);
        }

        /**
         * @brief Test whether @p info reflects an `Error::MetaError` specialization.
         * @param[in] info Entity reflection to inspect.
         * @return `true` when the dealiased entity is a native meta error.
         * @par Example
         * @code{.cpp}
         * constexpr auto error = Sora::Meta::MakeMetaError(^^Sora::Meta::Error::NonInvocableTag);
         * static_assert(Sora::Meta::IsMetaError(error));
         * @endcode
         */
        [[nodiscard]] consteval bool IsMetaError(std::meta::info info) noexcept {
            info = std::meta::dealias(info);
            return std::meta::is_type(info) && std::meta::is_class_type(info) &&
                   std::meta::has_template_arguments(info) && std::meta::template_of(info) == ^^Error::MetaError;
        }

        /**
         * @brief Test whether @p info is the neutral success marker.
         * @par Example
         * @code{.cpp}
         * static_assert(Sora::Meta::IsMetaSuccess(^^Sora::Meta::Error::MetaSuccess));
         * @endcode
         */
        [[nodiscard]] consteval bool IsMetaSuccess(std::meta::info info) noexcept {
            return std::meta::dealias(info) == ^^Error::MetaSuccess;
        }

        /**
         * @brief Return the category reflection stored in @p error.
         * @param[in] error Reflected `Error::MetaError` to inspect.
         * @return Reflected category entity supplied as the error's first template argument.
         * @pre `IsMetaError(error)` is `true`.
         * @throws A translation-time diagnostic when @p error is not a `MetaError`.
         * @par Example
         * @code{.cpp}
         * constexpr auto error = Sora::Meta::MakeMetaError(^^Sora::Meta::Error::OutOfRangeTag);
         * static_assert(Sora::Meta::ErrorTag(error) == ^^Sora::Meta::Error::OutOfRangeTag);
         * @endcode
         */
        [[nodiscard]] consteval std::meta::info ErrorTag(std::meta::info error) {
            if (!IsMetaError(error)) {
                throw std::define_static_string("Meta::ErrorTag requires Error::MetaError.");
            }
            return std::meta::extract<std::meta::info>(
                std::meta::template_arguments_of(std::meta::dealias(error)).front());
        }

        /**
         * @brief Return error provenance in source order.
         * @param[in] error Reflected `Error::MetaError` to inspect.
         * @return Mutable compile-time vector of the context reflections after the category.
         * @pre `IsMetaError(error)` is `true`.
         * @throws A translation-time diagnostic when @p error is not a `MetaError`.
         * @par Example
         * @code{.cpp}
         * constexpr auto error = Sora::Meta::MakeMetaError(^^Sora::Meta::Error::OutOfRangeTag, {^^int});
         * static_assert(Sora::Meta::ErrorContexts(error).size() == 1);
         * @endcode
         */
        [[nodiscard]] consteval std::vector<std::meta::info> ErrorContexts(std::meta::info error) {
            if (!IsMetaError(error)) {
                throw std::define_static_string("Meta::ErrorContexts requires Error::MetaError.");
            }
            auto arguments = std::meta::template_arguments_of(std::meta::dealias(error));
            arguments.erase(arguments.begin());
            for (auto& argument : arguments) {
                argument = std::meta::extract<std::meta::info>(argument);
            }
            return arguments;
        }

        /**
         * @brief Return the number of provenance reflections carried by @p error.
         * @param[in] error Reflected `Error::MetaError` to inspect.
         * @return Number of context reflections, excluding the error category.
         * @pre `IsMetaError(error)` is `true`.
         * @throws A translation-time diagnostic when @p error is not a `MetaError`.
         * @par Example
         * @code{.cpp}
         * constexpr auto error = Sora::Meta::MakeMetaError(^^Sora::Meta::Error::OutOfRangeTag, {^^int});
         * static_assert(Sora::Meta::ErrorContextCount(error) == 1);
         * @endcode
         */
        [[nodiscard]] consteval std::size_t ErrorContextCount(std::meta::info error) {
            return ErrorContexts(error).size();
        }

        /**
         * @brief Return the first error in @p Inputs, or an empty reflection when none exists.
         * @details This is the META-APPLY short-circuit law.
         * @tparam Inputs Reflections inspected from left to right.
         * @return First native meta error unchanged, or `std::meta::info{}` when every input is error-free.
         * @par Example
         * @code{.cpp}
         * constexpr auto error = Sora::Meta::MakeMetaError(^^Sora::Meta::Error::NonInvocableTag);
         * static_assert(Sora::Meta::FirstMetaError<^^int, error>() == error);
         * @endcode
         */
        template<std::meta::info... Inputs>
        [[nodiscard]] consteval std::meta::info FirstMetaError() noexcept {
            for (const auto input : std::array<std::meta::info, sizeof...(Inputs)>{Inputs...}) {
                if (IsMetaError(input)) {
                    return input;
                }
            }
            return {};
        }

        /**
         * @brief Return a stable diagnostic description for a reflected error.
         * @param[in] error Reflected `Error::MetaError` to format.
         * @return Human-readable category and context sequence.
         * @pre `IsMetaError(error)` is `true`.
         * @throws A translation-time diagnostic when @p error is not a `MetaError`.
         * @par Example
         * @code{.cpp}
         * constexpr auto error = Sora::Meta::MakeMetaError(^^Sora::Meta::Error::NonInvocableTag, {^^int});
         * constexpr auto text = Sora::Meta::ErrorDescription(error);
         * static_assert(!text.empty());
         * @endcode
         */
        [[nodiscard]] consteval std::string ErrorDescription(std::meta::info error) {
            if (!IsMetaError(error)) {
                throw std::define_static_string("Meta::ErrorDescription requires Error::MetaError.");
            }
            std::string result{"MetaError<"};
            result += std::string{std::meta::display_string_of(ErrorTag(error))};
            result += ">(";
            bool first = true;
            for (const auto context : ErrorContexts(error)) {
                if (!first) {
                    result += ", ";
                }
                first = false;
                result += std::string{std::meta::display_string_of(context)};
            }
            result += ')';
            return result;
        }

        /**
         * @brief Return the diagnostic description of a type-level meta error.
         * @tparam Error Type-level `MetaError` specialization.
         * @return Human-readable compile-time description in stable source order.
         * @pre `Error` satisfies `Concept::MetaErrorType`.
         * @par Example
         * @code{.cpp}
         * using Failure = Sora::Meta::InfoType<
         *     Sora::Meta::MakeMetaError(^^Sora::Meta::Error::NonInvocableTag)>;
         * constexpr auto text = Sora::Meta::ErrorDescriptionOf<Failure>();
         * static_assert(!text.empty());
         * @endcode
         */
        template<typename Error>
        [[nodiscard]] consteval std::string ErrorDescriptionOf() {
            static_assert(IsMetaError(std::meta::dealias(^^std::remove_cvref_t<Error>)));
            return ErrorDescription(^^std::remove_cvref_t<Error>);
        }

    } // namespace Meta

    namespace Concept {

        /**
         * @brief True when @p Info reflects a native meta error.
         * @tparam Info Reflection to inspect.
         * @par Example
         * @code{.cpp}
         * constexpr auto error = Sora::Meta::MakeMetaError(^^Sora::Meta::Error::NonInvocableTag);
         * static_assert(Sora::Concept::MetaError<error>);
         * @endcode
         */
        template<std::meta::info Info>
        concept MetaError = Sora::Meta::IsMetaError(Info);

        /**
         * @brief True when @p T is a type-level native meta error.
         * @tparam T Type to inspect after cv-reference removal and dealiasing.
         * @par Example
         * @code{.cpp}
         * using Failure = Sora::Meta::InfoType<
         *     Sora::Meta::MakeMetaError(^^Sora::Meta::Error::NonInvocableTag)>;
         * static_assert(Sora::Concept::MetaErrorType<Failure>);
         * @endcode
         */
        template<typename T>
        concept MetaErrorType = Sora::Meta::IsMetaError(std::meta::dealias(^^std::remove_cvref_t<T>));

        /**
         * @brief True when @p Info reflects the neutral success marker.
         * @tparam Info Reflection to inspect.
         * @par Example
         * @code{.cpp}
         * static_assert(Sora::Concept::MetaSuccess<^^Sora::Meta::Error::MetaSuccess>);
         * @endcode
         */
        template<std::meta::info Info>
        concept MetaSuccess = Sora::Meta::IsMetaSuccess(Info);

        /**
         * @brief True when @p Info is not a meta error.
         * @tparam Info Reflection to inspect.
         * @par Example
         * @code{.cpp}
         * static_assert(Sora::Concept::NoMetaError<^^int>);
         * @endcode
         */
        template<std::meta::info Info>
        concept NoMetaError = !MetaError<Info>;

        /**
         * @brief True when @p T is not a type-level meta error.
         * @tparam T Type to inspect.
         * @par Example
         * @code{.cpp}
         * static_assert(Sora::Concept::NoMetaErrorType<int>);
         * @endcode
         */
        template<typename T>
        concept NoMetaErrorType = !Concept::MetaErrorType<T>;

        /**
         * @brief True when at least one input reflection is an error.
         * @tparam Inputs Reflections inspected as one error domain.
         * @par Example
         * @code{.cpp}
         * constexpr auto error = Sora::Meta::MakeMetaError(^^Sora::Meta::Error::NonInvocableTag);
         * static_assert(Sora::Concept::HasMetaError<^^int, error>);
         * @endcode
         */
        template<std::meta::info... Inputs>
        concept HasMetaError = (MetaError<Inputs> || ...);

        /**
         * @brief True when every input reflection is error-free.
         * @tparam Inputs Reflections inspected as one error domain.
         * @par Example
         * @code{.cpp}
         * static_assert(Sora::Concept::HasNoMetaError<^^int, ^^double>);
         * @endcode
         */
        template<std::meta::info... Inputs>
        concept HasNoMetaError = (!MetaError<Inputs> && ...);

    } // namespace Concept

    namespace Traits {

        /**
         * @brief First-error projection for a reflection pack, with `MetaSuccess` as the neutral result.
         * @tparam Inputs Reflections inspected from left to right.
         * @par Example
         * @code{.cpp}
         * constexpr auto error = Sora::Meta::MakeMetaError(^^Sora::Meta::Error::NonInvocableTag);
         * static_assert(Sora::Traits::FirstMetaError<^^int, error> == error);
         * @endcode
         */
        template<std::meta::info... Inputs>
        inline constexpr std::meta::info FirstMetaError = [] consteval {
            constexpr auto result = Meta::FirstMetaError<Inputs...>();
            return result == std::meta::info{} ? ^^Meta::Error::MetaSuccess : result;
        }();

        /**
         * @brief Type-level first-error projection with `MetaSuccess` as the neutral result.
         * @tparam Types Ordinary C++ types inspected from left to right.
         * @par Example
         * @code{.cpp}
         * using Result = Sora::Traits::FirstMetaErrorOf<int, double>;
         * static_assert(std::same_as<Result, Sora::Meta::Error::MetaSuccess>);
         * @endcode
         */
        template<typename... Types>
        using FirstMetaErrorOf = Meta::InfoType<FirstMetaError<^^Types...>>;

        /**
         * @brief Error-category reflection of a native meta error.
         * @tparam Error Reflected native meta error.
         * @par Example
         * @code{.cpp}
         * constexpr auto error = Sora::Meta::MakeMetaError(^^Sora::Meta::Error::OutOfRangeTag);
         * static_assert(Sora::Traits::ErrorTag<error> == ^^Sora::Meta::Error::OutOfRangeTag);
         * @endcode
         */
        template<std::meta::info Error>
            requires Concept::MetaError<Error>
        inline constexpr std::meta::info ErrorTag = Meta::ErrorTag(Error);

        /**
         * @brief Static-storage provenance reflections of a native meta error.
         * @tparam Error Reflected native meta error.
         * @note The returned array has static storage duration and preserves source order.
         * @par Example
         * @code{.cpp}
         * constexpr auto error = Sora::Meta::MakeMetaError(^^Sora::Meta::Error::OutOfRangeTag, {^^int});
         * static_assert(Sora::Traits::ErrorContexts<error>.size() == 1);
         * @endcode
         */
        template<std::meta::info Error>
            requires Concept::MetaError<Error>
        inline constexpr auto ErrorContexts = std::define_static_array(Meta::ErrorContexts(Error));

        /**
         * @brief Number of provenance reflections carried by a native meta error.
         * @tparam Error Reflected native meta error.
         * @par Example
         * @code{.cpp}
         * constexpr auto error = Sora::Meta::MakeMetaError(^^Sora::Meta::Error::OutOfRangeTag, {^^int});
         * static_assert(Sora::Traits::ErrorContextCount<error> == 1);
         * @endcode
         */
        template<std::meta::info Error>
            requires Concept::MetaError<Error>
        inline constexpr std::size_t ErrorContextCount = Meta::ErrorContextCount(Error);

        /**
         * @brief Number of provenance reflections carried by a type-level meta error.
         * @tparam Error Type-level native meta error.
         * @par Example
         * @code{.cpp}
         * using Failure = Sora::Meta::InfoType<
         *     Sora::Meta::MakeMetaError(^^Sora::Meta::Error::OutOfRangeTag, {^^int})>;
         * static_assert(Sora::Traits::ErrorContextCountOf<Failure> == 1);
         * @endcode
         */
        template<typename Error>
            requires Concept::MetaErrorType<Error>
        inline constexpr std::size_t ErrorContextCountOf = Meta::ErrorContextCount(^^std::remove_cvref_t<Error>);

    } // namespace Traits

} // namespace Sora

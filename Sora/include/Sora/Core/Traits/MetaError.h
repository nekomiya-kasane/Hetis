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
            struct NonBooleanPredicateResultTag {};

        } // namespace Error

    } // namespace Meta

    namespace Meta {

        /**
         * @brief Build a reflected error from a tag and a compile-time context range.
         * @param[in] tag Reflection of the category entity.
         * @param[in] contexts Provenance reflections in source order.
         * @return Reflection of `Error::MetaError<tag, contexts...>`.
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
         */
        [[nodiscard]] consteval bool IsMetaError(std::meta::info info) noexcept {
            info = std::meta::dealias(info);
            return std::meta::is_type(info) && std::meta::is_class_type(info) &&
                   std::meta::has_template_arguments(info) && std::meta::template_of(info) == ^^Error::MetaError;
        }

        /** @brief Test whether @p info is the neutral success marker. */
        [[nodiscard]] consteval bool IsMetaSuccess(std::meta::info info) noexcept {
            return std::meta::dealias(info) == ^^Error::MetaSuccess;
        }

        /**
         * @brief Return the category reflection stored in @p error.
         * @pre `IsMetaError(error)` is `true`.
         */
        [[nodiscard]] consteval std::meta::info ErrorTagInfo(std::meta::info error) {
            if (!IsMetaError(error)) {
                throw std::define_static_string("Meta::ErrorTagInfo requires Error::MetaError.");
            }
            return std::meta::extract<std::meta::info>(
                std::meta::template_arguments_of(std::meta::dealias(error)).front());
        }

        /**
         * @brief Return error provenance in source order.
         * @pre `IsMetaError(error)` is `true`.
         */
        [[nodiscard]] consteval std::vector<std::meta::info> ErrorContextInfos(std::meta::info error) {
            if (!IsMetaError(error)) {
                throw std::define_static_string("Meta::ErrorContextInfos requires Error::MetaError.");
            }
            auto arguments = std::meta::template_arguments_of(std::meta::dealias(error));
            arguments.erase(arguments.begin());
            for (auto& argument : arguments) {
                argument = std::meta::extract<std::meta::info>(argument);
            }
            return arguments;
        }

        /** @brief Return the number of provenance reflections in @p error. */
        [[nodiscard]] consteval std::size_t ErrorContextCount(std::meta::info error) {
            return ErrorContextInfos(error).size();
        }

        /**
         * @brief Return the first error in @p Inputs, or an empty reflection when none exists.
         * @details This is the META-APPLY short-circuit law.
         */
        template<std::meta::info... Inputs>
        [[nodiscard]] consteval std::meta::info FirstMetaErrorInfo() noexcept {
            for (const auto input : std::array<std::meta::info, sizeof...(Inputs)>{Inputs...}) {
                if (IsMetaError(input)) {
                    return input;
                }
            }
            return {};
        }

        /**
         * @brief Return a stable diagnostic description for a reflected error.
         * @pre `IsMetaError(error)` is `true`.
         */
        [[nodiscard]] consteval std::string ErrorDescription(std::meta::info error) {
            if (!IsMetaError(error)) {
                throw std::define_static_string("Meta::ErrorDescription requires Error::MetaError.");
            }
            std::string result{"MetaError<"};
            result += std::string{std::meta::display_string_of(ErrorTagInfo(error))};
            result += ">(";
            bool first = true;
            for (const auto context : ErrorContextInfos(error)) {
                if (!first) {
                    result += ", ";
                }
                first = false;
                result += std::string{std::meta::display_string_of(context)};
            }
            result += ')';
            return result;
        }

        /** @brief Return the diagnostic description of a type-level meta error. */
        template<typename Error>
        [[nodiscard]] consteval std::string ErrorDescriptionOf() {
            static_assert(IsMetaError(std::meta::dealias(^^std::remove_cvref_t<Error>)));
            return ErrorDescription(^^std::remove_cvref_t<Error>);
        }

    } // namespace Meta

    namespace Concept {

        /** @brief True when @p Info reflects a native meta error. */
        template<std::meta::info Info>
        concept MetaError = Sora::Meta::IsMetaError(Info);

        /** @brief True when @p T is a type-level native meta error. */
        template<typename T>
        concept MetaErrorType = Sora::Meta::IsMetaError(std::meta::dealias(^^std::remove_cvref_t<T>));

        /** @brief True when @p Info reflects the neutral success marker. */
        template<std::meta::info Info>
        concept MetaSuccess = Sora::Meta::IsMetaSuccess(Info);

        /** @brief True when @p Info is not a meta error. */
        template<std::meta::info Info>
        concept NoMetaError = !MetaError<Info>;

        /** @brief True when at least one input reflection is an error. */
        template<std::meta::info... Inputs>
        concept HasMetaError = (MetaError<Inputs> || ...);

        /** @brief True when every input reflection is error-free. */
        template<std::meta::info... Inputs>
        concept HasNoMetaError = (!MetaError<Inputs> && ...);

    } // namespace Concept

    namespace Traits {

        /** @brief First-error projection for a reflection pack; empty means `MetaSuccess`. */
        template<std::meta::info... Inputs>
        inline constexpr std::meta::info FirstMetaError = [] consteval {
            constexpr auto result = Meta::FirstMetaErrorInfo<Inputs...>();
            return result == std::meta::info{} ? ^^Meta::Error::MetaSuccess : result;
        }();

        /** @brief First-error projection for ordinary C++ types; empty means `MetaSuccess`. */
        template<typename... Types>
        using FirstMetaErrorOf = Meta::InfoType<FirstMetaError<^^Types...>>;

        /** @brief Error-category reflection of a native meta error. */
        template<std::meta::info Error>
            requires Concept::MetaError<Error>
        inline constexpr std::meta::info ErrorTag = Meta::ErrorTagInfo(Error);

        /** @brief Provenance reflections of a native meta error. */
        template<std::meta::info Error>
            requires Concept::MetaError<Error>
        inline constexpr auto ErrorContexts = std::define_static_array(Meta::ErrorContextInfos(Error));

        /** @brief Number of provenance reflections carried by a native meta error. */
        template<std::meta::info Error>
            requires Concept::MetaError<Error>
        inline constexpr std::size_t ErrorContextCount = Meta::ErrorContextCount(Error);

        /** @brief Number of provenance reflections carried by a type-level meta error. */
        template<typename Error>
            requires Concept::MetaErrorType<Error>
        inline constexpr std::size_t ErrorContextCountOf = Meta::ErrorContextCount(^^std::remove_cvref_t<Error>);

    } // namespace Traits

} // namespace Sora

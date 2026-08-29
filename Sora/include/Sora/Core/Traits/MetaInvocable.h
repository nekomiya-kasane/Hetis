/**
 * @file MetaInvocable.h
 * @brief C++26 reflection-native template application primitives.
 * @ingroup Core
 *
 * @details A meta-function is a reflected template and application is `std::meta::substitute`. This header owns the
 * single-invocation result and error protocol; sequence and higher-order algorithms live in `MetaOperators.h`.
 */
#pragma once

#include <meta>
#include <ranges>
#include <vector>

#include <Sora/Core/Traits/MetaError.h>
#include <Sora/Core/Traits/MetaInfo.h>

namespace Sora {

    namespace Meta {

        /**
         * @brief Apply a reflected template and propagate its first error input.
         *
         * @details
         * @f[
         * I(F,A)=
         * \begin{cases} E_i & E_i\text{ is the first error in }(F,A),\\
         * \operatorname{substitute}(F,A) & \operatorname{can\_substitute}(F,A),\\
         * \operatorname{MetaError}(\mathrm{NonInvocable},F,A) & \text{otherwise.}
         * \end{cases}
         * @f]
         *
         * @par Example
         * @code{.cpp}
         * constexpr auto pairInfo = Sora::Meta::Invoke<^^std::pair, ^^int, ^^double>;
         * static_assert(pairInfo == ^^std::pair<int, double>);
         *
         * constexpr auto invalidInfo = Sora::Meta::Invoke<^^std::pair, ^^int>;
         * static_assert(Sora::Meta::IsMetaError(invalidInfo));
         * @endcode
         * @tparam Function Reflected template to apply.
         * @tparam Arguments Reflected application arguments.
         * @return Substitution result, first input error, or `NonInvocable` with invocation provenance.
         */
        template<std::meta::info Function, std::meta::info... Arguments>
        inline constexpr std::meta::info Invoke = [] consteval {
            if constexpr (IsMetaError(Function)) {
                return Function;
            } else if constexpr (!std::meta::is_template(Function)) {
                return MakeMetaError(^^Error::NonInvocable, {Function});
            } else if constexpr (FirstMetaError<Arguments...>() != std::meta::info{}) {
                return FirstMetaError<Arguments...>();
            } else if constexpr (std::meta::can_substitute(Function, {Arguments...})) {
                return std::meta::substitute(Function, {Arguments...});
            } else {
                return MakeMetaError(^^Error::NonInvocable, {Function, Arguments...});
            }
        }();

        /**
         * @brief Invoke @p Function or return @p Default when application fails.
         * @tparam Default Reflected fallback returned for any `MetaError` result.
         * @tparam Function Reflected template to apply.
         * @tparam Arguments Reflected application arguments.
         * @return Successful substitution result or @p Default.
         * @details A failed application is represented by the supplied fallback; no runtime branch or exception is
         * introduced.
         * @par Example
         * @code{.cpp}
         * static_assert(Sora::Meta::InvokeOr<^^void, ^^std::pair, ^^int> == ^^void);
         * static_assert(Sora::Meta::InvokeOr<^^void, ^^std::type_identity_t, ^^int> == ^^int);
         * @endcode
         */
        template<std::meta::info Default, std::meta::info Function, std::meta::info... Arguments>
        inline constexpr std::meta::info InvokeOr = [] consteval {
            constexpr auto result = Invoke<Function, Arguments...>;
            return IsMetaError(result) ? Default : result;
        }();

        /**
         * @brief True when applying @p Function does not produce a meta error.
         * @tparam Function Reflected template to apply.
         * @tparam Arguments Reflected application arguments.
         * @par Example
         * @code{.cpp}
         * static_assert(Sora::Meta::Invocable<^^std::pair, ^^int, ^^double>);
         * static_assert(!Sora::Meta::Invocable<^^std::pair, ^^int>);
         * @endcode
         */
        template<std::meta::info Function, std::meta::info... Arguments>
        concept Invocable = !IsMetaError(Invoke<Function, Arguments...>);

        /**
         * @brief True when the invocation succeeds and produces a type entity.
         * @tparam Function Reflected template to apply.
         * @tparam Arguments Ordinary C++ types reflected for application.
         * @par Example
         * @code{.cpp}
         * static_assert(Sora::Meta::InvocableReturningType<^^std::pair, int, double>);
         * static_assert(!Sora::Meta::InvocableReturningType<^^std::pair, int>);
         * @endcode
         */
        template<std::meta::info Function, typename... Arguments>
        concept InvocableReturningType
            = Invocable<Function, ^^Arguments...> && std::meta::is_type(Invoke<Function, ^^Arguments...>);

        /**
         * @brief True when a reflected template can be substituted with @p Arguments.
         * @tparam Function Reflected template to inspect.
         * @tparam Arguments Reflected application arguments.
         * @return `true` when direct substitution is well-formed; input `MetaError` values are not propagated.
         * @par Example
         * @code{.cpp}
         * static_assert(Sora::Meta::DirectlyInvocable<^^std::pair, ^^int, ^^double>);
         * static_assert(!Sora::Meta::DirectlyInvocable<^^std::pair, ^^int>);
         * @endcode
         */
        template<std::meta::info Function, std::meta::info... Arguments>
        concept DirectlyInvocable
            = std::meta::is_template(Function) && std::meta::can_substitute(Function, {Arguments...});

        namespace Detail {

            /**
             * @brief Convert a successful result to a type or a typed-result error.
             * @details Internal helper; callers should use @ref InvokeResultType or @ref InvokeResult.
             * @tparam Result Reflected invocation result to validate.
             * @tparam Function Reflected function recorded in failure provenance.
             * @tparam Arguments Reflected arguments recorded in failure provenance.
             * @return @p Result when it is a type/error, otherwise a `NonTypeResult` error.
             * @par Example
             * @code{.cpp}
             * constexpr auto type = Sora::Meta::Detail::RequireType<^^int, ^^std::type_identity, ^^int>();
             * static_assert(type == ^^int);
             * @endcode
             */
            template<std::meta::info Result, std::meta::info Function, std::meta::info... Arguments>
            [[nodiscard]] consteval std::meta::info RequireType() {
                if constexpr (IsMetaError(Result) || std::meta::is_type(Result)) {
                    return Result;
                } else {
                    return MakeMetaError(^^Error::NonTypeResult, {Function, Arguments...});
                }
            }

        } // namespace Detail

        /**
         * @brief Type projection of @ref Invoke; a non-type result becomes `NonTypeResult`.
         * @tparam Function Reflected template to apply.
         * @tparam Arguments Reflected application arguments.
         * @return Resulting C++ type or the corresponding type-level `MetaError` specialization.
         * @details Recoverable substitution failures are preserved as types and do not throw a diagnostic.
         * @par Example
         * @code{.cpp}
         * using Result = Sora::Meta::InvokeResultType<^^std::type_identity_t, ^^int>;
         * static_assert(std::same_as<Result, int>);
         * @endcode
         */
        template<std::meta::info Function, std::meta::info... Arguments>
        using InvokeResultType
            = InfoType<Detail::RequireType<Invoke<Function, Arguments...>, Function, Arguments...>()>;

        /**
         * @brief Invoke a reflected template with ordinary C++ type arguments.
         * @tparam Function Reflected template to apply.
         * @tparam Arguments Ordinary C++ argument types.
         * @return Resulting C++ type, or the reflected `MetaError` type for a failed/non-type result.
         * @details Recoverable substitution failures remain type-level `MetaError` specializations and are not thrown.
         * @code{.cpp}
         * using Pair = Sora::Meta::InvokeResult<^^std::pair, int, double>;
         * static_assert(std::same_as<Pair, std::pair<int, double>>);
         * @endcode
         */
        template<std::meta::info Function, typename... Arguments>
        using InvokeResult = InvokeResultType<Function, ^^Arguments...>;

        /**
         * @brief Invoke a reflected non-template function with reflected arguments.
         *
         * @details
         * This operation is deliberately distinct from @ref Invoke: it uses
         * `std::meta::reflect_invoke` and therefore executes a reflected function,
         * while @ref Invoke uses `std::meta::substitute` to apply a reflected
         * template. Each input reflection is passed to the function as a
         * `std::meta::info` value. The function's normal C++ invocability rules
         * apply, including default and variadic parameters.
         *
         * The first input @ref MetaError is propagated unchanged. Invalid
         * functions, empty argument reflections, and incompatible argument lists
         * produce @c Error::NonInvocable. A successful invocation preserves the
         * complete reflected result: it may denote a type, value, function, or
         * another reflection entity.
         *
         * @note Reflection currently exposes no query for constant-evaluation
         *     availability. Passing a runtime-only function therefore remains
         *     a translation-time failure at `std::meta::reflect_invoke`.
         *
         * @param[in] function Reflection of a non-template function.
         * @param[in] arguments Reflections passed as `std::meta::info` values in
         *     source order.
         * @return Reflected invocation result, propagated input error, or a
         *     reflection-native @c Error::NonInvocable.
         * @par Example
         * @code{.cpp}
         * consteval std::meta::info Identity(std::meta::info value) noexcept { return value; }
         * constexpr auto result = Sora::Meta::InvokeFunction(^^Identity, {^^int});
         * static_assert(result == ^^int); // input: ^^int, output: ^^int
         * @endcode
         */
        [[nodiscard]] consteval std::meta::info InvokeFunction(std::meta::info function,
                                                               std::vector<std::meta::info> arguments = {}) {
            if (IsMetaError(function)) {
                return function;
            }

            std::vector<std::meta::info> context;
            context.reserve(arguments.size() + 1);
            context.push_back(function);
            for (const auto argument : arguments) {
                context.push_back(argument);
                if (IsMetaError(argument)) {
                    return argument;
                }
            }
            if (function == std::meta::info{} || !std::meta::is_function(function)) {
                return MakeMetaError(^^Error::NonInvocable, context);
            }
            for (const auto argument : arguments) {
                if (argument == std::meta::info{}) {
                    return MakeMetaError(^^Error::NonInvocable, context);
                }
            }

            const std::vector<std::meta::info> argumentTypes(arguments.size(), ^^std::meta::info);
            if (!std::meta::is_invocable_type(std::meta::type_of(function), argumentTypes)) {
                return MakeMetaError(^^Error::NonInvocable, context);
            }

            std::vector<std::meta::info> reflectedArguments
                = arguments
                  | std::views::transform([](std::meta::info arg) { return std::meta::reflect_constant(arg); })
                  | std::ranges::to<std::vector>();
            const auto result = std::meta::reflect_invoke(function, reflectedArguments);
            // A function returning std::meta::info yields a value reflection whose payload is the entity handle.
            // Unwrap that transport value so callers receive the same reflection they returned from the function.
            if (std::meta::dealias(std::meta::return_type_of(function)) == std::meta::dealias(^^std::meta::info)) {
                return std::meta::extract<std::meta::info>(result);
            }
            return result;
        }

        /**
         * @brief Test whether a reflected function accepts the supplied reflected arguments.
         * @tparam Function Reflected non-template function.
         * @tparam Arguments Reflected arguments passed as `std::meta::info` values.
         * @return @c true when invocation succeeds without a reflection-native error.
         * @par Example
         * @code{.cpp}
         * consteval std::meta::info Identity(std::meta::info value) noexcept { return value; }
         * static_assert(Sora::Meta::FunctionInvocable<^^Identity, ^^int>);
         * static_assert(!Sora::Meta::FunctionInvocable<^^Identity>);
         * @endcode
         */
        template<std::meta::info Function, std::meta::info... Arguments>
        concept FunctionInvocable = !IsMetaError(InvokeFunction(Function, {Arguments...}));

        /**
         * @brief Compile-time reflected result of invoking a non-template function.
         * @tparam Function Reflected non-template function.
         * @tparam Arguments Reflected arguments passed as `std::meta::info` values.
         * @return The complete reflected result. The result may denote a type,
         *     value, function, or another entity; no type projection is applied.
         * @par Example
         * @code{.cpp}
         * consteval int IsIntegral(std::meta::info type) noexcept {
         *     return std::meta::is_integral_type(type) ? 1 : 0;
         * }
         * constexpr auto result = Sora::Meta::FunctionResult<^^IsIntegral, ^^int>;
         * static_assert(std::meta::extract<int>(result) == 1); // input: ^^int, output: 1
         * @endcode
         */
        template<std::meta::info Function, std::meta::info... Arguments>
        inline constexpr std::meta::info FunctionResult = InvokeFunction(Function, {Arguments...});

    } // namespace Meta

} // namespace Sora

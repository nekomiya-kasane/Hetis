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
         * #include <meta>
         * #include <utility>
         * #include <Sora/Core/Traits/MetaInvocable.h>
         *
         * constexpr auto pairInfo = Sora::Meta::Invoke<^^std::pair, ^^int, ^^double>;
         * static_assert(pairInfo == ^^std::pair<int, double>);
         *
         * constexpr auto invalidInfo = Sora::Meta::Invoke<^^std::pair, ^^int>;
         * static_assert(Sora::Meta::IsMetaError(invalidInfo));
         * @endcode
         * @tparam Function Reflected template to apply.
         * @tparam Arguments Reflected application arguments.
         * @return Substitution result, first input error, or `NonInvocableTag` with invocation provenance.
         */
        template<std::meta::info Function, std::meta::info... Arguments>
        inline constexpr std::meta::info Invoke = [] consteval {
            constexpr auto error = FirstMetaError<Function, Arguments...>();
            if constexpr (error != std::meta::info{}) {
                return error;
            } else if constexpr (std::meta::is_template(Function) &&
                                 std::meta::can_substitute(Function, {Arguments...})) {
                return std::meta::substitute(Function, {Arguments...});
            } else {
                return MakeMetaError(^^Error::NonInvocableTag, {Function, Arguments...});
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
         * #include <meta>
         * #include <type_traits>
         * #include <utility>
         * #include <Sora/Core/Traits/MetaInvocable.h>
         *
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
         * #include <meta>
         * #include <utility>
         * #include <Sora/Core/Traits/MetaInvocable.h>
         *
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
         * #include <meta>
         * #include <utility>
         * #include <Sora/Core/Traits/MetaInvocable.h>
         *
         * static_assert(Sora::Meta::InvocableReturningType<^^std::pair, int, double>);
         * static_assert(!Sora::Meta::InvocableReturningType<^^std::pair, int>);
         * @endcode
         */
        template<std::meta::info Function, typename... Arguments>
        concept InvocableReturningType =
            Invocable<Function, ^^Arguments...> && std::meta::is_type(Invoke<Function, ^^Arguments...>);

        /**
         * @brief True when a reflected template can be substituted with @p Arguments.
         * @tparam Function Reflected template to inspect.
         * @tparam Arguments Reflected application arguments.
         * @return `true` when direct substitution is well-formed; input `MetaError` values are not propagated.
         */
        template<std::meta::info Function, std::meta::info... Arguments>
        concept DirectlyInvocable =
            std::meta::is_template(Function) && std::meta::can_substitute(Function, {Arguments...});

        namespace Detail {

            /**
             * @brief Convert a successful result to a type or a typed-result error.
             * @details Internal helper; callers should use @ref InvokeResultType or @ref InvokeResult.
             * @tparam Result Reflected invocation result to validate.
             * @tparam Function Reflected function recorded in failure provenance.
             * @tparam Arguments Reflected arguments recorded in failure provenance.
             * @return @p Result when it is a type/error, otherwise a `NonTypeResultTag` error.
             * @par Example
             * @code{.cpp}
             * #include <meta>
             * #include <type_traits>
             * #include <Sora/Core/Traits/MetaInvocable.h>
             *
             * constexpr auto type = Sora::Meta::Detail::RequireType<^^int, ^^std::type_identity, ^^int>();
             * static_assert(type == ^^int);
             * @endcode
             */
            template<std::meta::info Result, std::meta::info Function, std::meta::info... Arguments>
            [[nodiscard]] consteval std::meta::info RequireType() {
                if constexpr (IsMetaError(Result) || std::meta::is_type(Result)) {
                    return Result;
                } else {
                    return MakeMetaError(^^Error::NonTypeResultTag, {Function, Arguments...});
                }
            }

        } // namespace Detail

        /**
         * @brief Type projection of @ref Invoke; a non-type result becomes `NonTypeResultTag`.
         * @tparam Function Reflected template to apply.
         * @tparam Arguments Reflected application arguments.
         * @return Resulting C++ type or the corresponding type-level `MetaError` specialization.
         * @details Recoverable substitution failures are preserved as types and do not throw a diagnostic.
         * @par Example
         * @code{.cpp}
         * #include <concepts>
         * #include <meta>
         * #include <type_traits>
         * #include <Sora/Core/Traits/MetaInvocable.h>
         *
         * using Result = Sora::Meta::InvokeResultType<^^std::type_identity_t, ^^int>;
         * static_assert(std::same_as<Result, int>);
         * @endcode
         */
        template<std::meta::info Function, std::meta::info... Arguments>
        using InvokeResultType =
            InfoType<Detail::RequireType<Invoke<Function, Arguments...>, Function, Arguments...>()>;

        /**
         * @brief Invoke a reflected template with ordinary C++ type arguments.
         * @tparam Function Reflected template to apply.
         * @tparam Arguments Ordinary C++ argument types.
         * @return Resulting C++ type, or the reflected `MetaError` type for a failed/non-type result.
         * @details Recoverable substitution failures remain type-level `MetaError` specializations and are not thrown.
         * @code{.cpp}
         * #include <concepts>
         * #include <meta>
         * #include <utility>
         * #include <Sora/Core/Traits/MetaInvocable.h>
         *
         * using Pair = Sora::Meta::InvokeResult<^^std::pair, int, double>;
         * static_assert(std::same_as<Pair, std::pair<int, double>>);
         * @endcode
         */
        template<std::meta::info Function, typename... Arguments>
        using InvokeResult = InvokeResultType<Function, ^^Arguments...>;

    } // namespace Meta

} // namespace Sora

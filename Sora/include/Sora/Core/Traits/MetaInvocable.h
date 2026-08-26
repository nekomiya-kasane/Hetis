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
         * \[
         * I(F,A)=
         * \begin{cases} E_i & E_i\text{ is the first error in }(F,A),\\
         * \operatorname{substitute}(F,A) & \operatorname{can\_substitute}(F,A),\\
         * \operatorname{MetaError}(\mathrm{NonInvocable},F,A) & \text{otherwise.}
         * \end{cases}
         * \]
         *
         * @par Example
         * @code{.cpp}
         * constexpr auto pairInfo = Meta::Invoke<^^std::pair, ^^int, ^^double>;
         * static_assert(pairInfo == ^^std::pair<int, double>);
         *
         * constexpr auto invalidInfo = Meta::Invoke<^^std::pair, ^^int>;
         * static_assert(Meta::IsMetaError(invalidInfo));
         * @endcode
         */
        template<std::meta::info Function, std::meta::info... Arguments>
        inline constexpr std::meta::info Invoke = [] consteval {
            constexpr auto error = FirstMetaErrorInfo<Function, Arguments...>();
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
         * @par Example
         * @code{.cpp}
         * static_assert(Meta::InvokeOr<^^void, ^^std::pair, ^^int> == ^^void);
         * static_assert(Meta::InvokeOr<^^void, ^^std::type_identity_t, ^^int> == ^^int);
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
         * static_assert(Meta::Invocable<^^std::pair, ^^int, ^^double>);
         * static_assert(!Meta::Invocable<^^std::pair, ^^int>);
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
         * static_assert(Meta::InvocableReturningType<^^std::pair, int, double>);
         * static_assert(!Meta::InvocableReturningType<^^std::pair, int>);
         * @endcode
         */
        template<std::meta::info Function, typename... Arguments>
        concept InvocableReturningType =
            Invocable<Function, ^^Arguments...> && std::meta::is_type(Invoke<Function, ^^Arguments...>);

        /** @brief True when a reflected template can be substituted with @p Arguments. */
        template<std::meta::info Function, std::meta::info... Arguments>
        concept DirectlyInvocable = std::meta::is_template(Function) &&
                                    std::meta::can_substitute(Function, {Arguments...});

        namespace Detail {

            /**
             * @brief Convert a successful result to a type or a typed-result error.
             * @par Example
             * @code{.cpp}
             * constexpr auto type = Detail::RequireType<^^int, ^^std::type_identity, ^^int>();
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
         * @par Example
         * @code{.cpp}
         * using Result = Meta::InvokeResultType<^^std::type_identity_t, ^^int>;
         * static_assert(std::same_as<Result, int>);
         * @endcode
         */
        template<std::meta::info Function, std::meta::info... Arguments>
        using InvokeResultType =
            InfoType<Detail::RequireType<Invoke<Function, Arguments...>, Function, Arguments...>()>;

        /**
         * @brief Invoke a reflected template with ordinary C++ type arguments.
         * @code{.cpp}
         * using Pair = Sora::Meta::InvokeResult<^^std::pair, int, double>;
         * static_assert(std::same_as<Pair, std::pair<int, double>>);
         * @endcode
         */
        template<std::meta::info Function, typename... Arguments>
        using InvokeResult = InvokeResultType<Function, ^^Arguments...>;

    } // namespace Meta

} // namespace Sora

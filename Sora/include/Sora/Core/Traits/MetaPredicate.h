/**
 * @file MetaPredicate.h
 * @brief Reflection-native compile-time predicate protocol.
 * @ingroup Core
 *
 * @details A Sora meta predicate is a reflected `consteval` function with the signature `bool(std::meta::info)`. The
 * argument is the reflection of a type or another compile-time entity, not the entity's spliced C++ type. Keeping this
 * protocol separate from template substitution makes the ownership of ordinary reflection calls explicit.
 */
#pragma once

#include <meta>

#include <Sora/Core/Traits/MetaError.h>

namespace Sora::Meta {

    /**
     * @brief Test whether @p predicate satisfies the native predicate signature.
     * @return `true` exactly for callable function entities returning `bool` from `std::meta::info`.
     */
    [[nodiscard]] consteval bool IsMetaPredicate(std::meta::info predicate) {
        if (!std::meta::is_function(predicate)) {
            return false;
        }
        return std::meta::return_type_of(predicate) ==
               (^^bool)&&std::meta::is_invocable_type(std::meta::type_of(predicate), {^^std::meta::info});
    }

    /**
     * @brief Invoke a reflected unary predicate and return its reflected bool value.
     *
     * @details For a predicate (P) and reflected argument (A), the valid
     * result is
     *
     * \f[
     *   P(A) = \operatorname{reflect\_invoke}
     *       (P, \{\operatorname{reflect\_constant}(A)\})
     * \f]
     *
     * The result is a reflected `bool` value. Invalid predicates and non-bool results are represented as `MetaError`,
     * so list algorithms can preserve the first failing context instead of silently treating failure as false.
     *
     * @param[in] predicate Reflection of a `consteval bool(std::meta::info)` function.
     * @param[in] argument Reflection supplied to the predicate.
     * @return Reflected bool value or a reflection-native error.
     * @code{.cpp}
     * consteval bool IsClass(std::meta::info type) noexcept {
     *     return std::meta::is_class_type(type);
     * }
     * constexpr auto result = Sora::Meta::InvokePredicate(^^IsClass, ^^int);
     * static_assert(!std::meta::extract<bool>(result));
     * @endcode
     */
    [[nodiscard]] consteval std::meta::info InvokePredicate(std::meta::info predicate, std::meta::info argument) {
        if (IsMetaError(predicate)) {
            return predicate;
        }
        if (IsMetaError(argument)) {
            return argument;
        }
        if (!IsMetaPredicate(predicate)) {
            return MakeMetaError(^^Error::NonInvocableTag, {predicate, argument});
        }

        const auto result = std::meta::reflect_invoke(predicate, {std::meta::reflect_constant(argument)});
        if (!std::meta::is_value(result) || std::meta::type_of(result) != ^^bool) {
            return MakeMetaError(^^Error::NonBooleanPredicateResultTag, {predicate, argument, result});
        }
        return result;
    }

    /**
     * @brief Extract the bool result of a reflected predicate invocation.
     * @pre `InvokePredicate(@p predicate, @p argument)` is not a `MetaError`.
     * @throws A translation-time diagnostic when the predicate contract is violated.
     */
    [[nodiscard]] consteval bool EvaluatePredicate(std::meta::info predicate, std::meta::info argument) {
        const auto result = InvokePredicate(predicate, argument);
        if (IsMetaError(result)) {
            throw std::define_static_string("Meta::EvaluatePredicate requires a callable bool predicate.");
        }
        return std::meta::extract<bool>(result);
    }

    /**
     * @brief Extract a validated bool from a predicate algorithm result.
     * @pre @p result is a reflected bool value and not a `MetaError`.
     * @throws A translation-time diagnostic when the result is invalid.
     */
    [[nodiscard]] consteval bool RequirePredicateResult(std::meta::info result) {
        if (IsMetaError(result) || !std::meta::is_value(result) || std::meta::type_of(result) != ^^bool) {
            throw std::define_static_string("Meta::RequirePredicateResult requires a reflected bool result.");
        }
        return std::meta::extract<bool>(result);
    }

} // namespace Sora::Meta

namespace Sora::Concept {

    /** @brief True when @p Predicate reflects `bool(std::meta::info)`. */
    template<std::meta::info Predicate>
    concept MetaPredicate = Sora::Meta::IsMetaPredicate(Predicate);

    /**
     * @brief True when @p Predicate accepts `std::meta::info` and returns bool.
     * @tparam Predicate Reflected predicate function.
     * @tparam Argument Reflected argument used for the contract check.
     */
    template<std::meta::info Predicate, std::meta::info Argument>
    concept PredicateFor = !Sora::Meta::IsMetaError(Sora::Meta::InvokePredicate(Predicate, Argument));

} // namespace Sora::Concept

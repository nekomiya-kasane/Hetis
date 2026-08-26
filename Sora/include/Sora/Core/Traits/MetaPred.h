/**
 * @file MetaPred.h
 * @brief Reflection-native compile-time predicate protocol for arbitrary arity.
 * @ingroup Core
 *
 * @details A Sora meta predicate is a reflected function returning @c bool whose parameters can all accept
 * @c std::meta::info. Predicates of every arity use the same invocation and signature-validation path. The function
 * must additionally be callable during constant evaluation; current C++26 reflection exposes no query for that
 * declaration property, so violating it is a translation-time failure at `reflect_invoke`, not a `MetaError`.
 */
#pragma once

#include <array>
#include <cstddef>
#include <concepts>
#include <meta>
#include <type_traits>
#include <vector>

#include <Sora/Core/Traits/MetaError.h>

namespace Sora::Meta {

    /**
     * @brief Test whether @p pred satisfies the arbitrary-arity meta-predicate protocol.
     * @return @c true when @p pred is a function returning @c bool and every parameter accepts @c std::meta::info.
     */
    [[nodiscard]] consteval bool IsMetaPred(std::meta::info pred) {
        if (!std::meta::is_function(pred) || std::meta::return_type_of(pred) != ^^bool) {
            return false;
        }

        const auto parameters = std::meta::parameters_of(pred);
        const std::vector<std::meta::info> arguments(parameters.size(), ^^std::meta::info);
        return std::meta::is_invocable_type(std::meta::type_of(pred), arguments);
    }

    /**
     * @brief Invoke a reflected predicate with zero or more reflected arguments.
     *
     * @details Meta errors propagate from left to right. A non-function or incompatible arity produces
     * @c Error::NonInvocableTag; a successful invocation returning a non-bool value produces
     * @c Error::NonBooleanPredResultTag.
     *
     * @param[in] pred Reflected predicate function.
     * @param[in] arguments Reflections supplied to @p pred in source order.
     * @return Reflected bool value or a reflection-native error.
     * @code{.cpp}
     * consteval bool Same(std::meta::info left, std::meta::info right) noexcept {
     *     return left == right;
     * }
     * constexpr auto result = Sora::Meta::InvokePred(^^Same, ^^int, ^^int);
     * static_assert(std::meta::extract<bool>(result));
     * @endcode
     */
    template<typename... Arguments>
        requires(std::same_as<std::remove_cvref_t<Arguments>, std::meta::info> && ...)
    [[nodiscard]] consteval std::meta::info InvokePred(std::meta::info pred, Arguments... arguments) {
        const std::array<std::meta::info, sizeof...(Arguments)> inputs{arguments...};
        if (IsMetaError(pred)) {
            return pred;
        }
        for (const auto input : inputs) {
            if (IsMetaError(input)) {
                return input;
            }
        }

        std::vector<std::meta::info> context{pred};
        context.append_range(inputs);
        if (!std::meta::is_function(pred)) {
            return MakeMetaError(^^Error::NonInvocableTag, context);
        }

        const std::vector<std::meta::info> argumentTypes(inputs.size(), ^^std::meta::info);
        if (!std::meta::is_invocable_type(std::meta::type_of(pred), argumentTypes)) {
            return MakeMetaError(^^Error::NonInvocableTag, context);
        }

        std::vector<std::meta::info> reflectedArguments;
        reflectedArguments.reserve(inputs.size());
        for (const auto input : inputs) {
            reflectedArguments.push_back(std::meta::reflect_constant(input));
        }
        const auto result = std::meta::reflect_invoke(pred, reflectedArguments);
        if (!std::meta::is_value(result) || std::meta::type_of(result) != ^^bool) {
            context.push_back(result);
            return MakeMetaError(^^Error::NonBooleanPredResultTag, context);
        }
        return result;
    }

    /**
     * @brief Extract the bool result of an arbitrary-arity reflected predicate invocation.
     * @throws A translation-time diagnostic when the predicate contract is violated.
     */
    template<typename... Arguments>
        requires(std::same_as<std::remove_cvref_t<Arguments>, std::meta::info> && ...)
    [[nodiscard]] consteval bool EvaluatePred(std::meta::info pred, Arguments... arguments) {
        const auto result = InvokePred(pred, arguments...);
        if (IsMetaError(result)) {
            throw std::define_static_string("Meta::EvaluatePred requires an invocable bool predicate.");
        }
        return std::meta::extract<bool>(result);
    }

    /**
     * @brief Extract a validated bool from a predicate algorithm result.
     * @pre @p result is a reflected bool value and not a @c MetaError.
     * @throws A translation-time diagnostic when @p result is invalid.
     */
    [[nodiscard]] consteval bool RequirePredResult(std::meta::info result) {
        if (IsMetaError(result) || !std::meta::is_value(result) || std::meta::type_of(result) != ^^bool) {
            throw std::define_static_string("Meta::RequirePredResult requires a reflected bool result.");
        }
        return std::meta::extract<bool>(result);
    }

} // namespace Sora::Meta

namespace Sora::Traits {

    /** @brief Declared parameter count of reflected meta predicate @p Pred. */
    template<std::meta::info Pred>
        requires(Sora::Meta::IsMetaPred(Pred))
    inline constexpr std::size_t PredArity = std::meta::parameters_of(Pred).size();

} // namespace Sora::Traits

namespace Sora::Concept {

    /** @brief True when @p Pred reflects an arbitrary-arity meta predicate. */
    template<std::meta::info Pred>
    concept MetaPred = Sora::Meta::IsMetaPred(Pred);

    /** @brief True when @p Pred accepts exactly the reflected @p Arguments and returns bool. */
    template<std::meta::info Pred, std::meta::info... Arguments>
    concept PredFor = !Sora::Meta::IsMetaError(Sora::Meta::InvokePred(Pred, Arguments...));

} // namespace Sora::Concept

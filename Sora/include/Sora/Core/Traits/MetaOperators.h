/**
 * @file MetaOperators.h
 * @brief Reflection-native higher-order functions and sequence algebra.
 * @ingroup Core
 *
 * @details Collection algorithms operate on `Meta::InfoList`. A successful collection result is a reflected
 * `InfoList` value; failure is a reflected `MetaError` type. `ToTypeList` is the sole collection-to-type-pack
 * boundary. This keeps reflection traversal, error propagation, and C++ type formation as separate layers.
 *
 * Every algorithm obeys the first-error law: public arguments are inspected from left to right, and a sequence
 * argument is inspected recursively in source order before the next argument. Empty folds are the sole exception:
 * they return their initial entity without validating the unused combiner. `Meta::Map` and its peers form the
 * `InfoList` algorithm kernel. Structural set operations live in `MetaInfoList.h`; higher-order operations such as
 * `FlatMap` and conditional replacement live here. `Meta::Types` contains the reflected `Traits::TypeList` adapters,
 * keeping the type boundary explicit.
 *
 * C++26 reflection does not expose a template's parameter list, so parameter kind, arity, and constraints are
 * validated by the first actual substitution. Empty `Map` and `Zip` validate only the template category; empty folds
 * return their initial entity without inspecting the unused combiner. Predicates can be fully validated even for
 * empty input because their parameters are reflectable. `Map` and `Zip` dealias produced entities; `Filter` preserves
 * accepted input reflections exactly.
 *
 * @par InfoList pipeline
 * @code{.cpp}
 * constexpr Sora::Meta::InfoList input{^^int, ^^double, ^^const double};
 * constexpr auto decayed = Sora::Meta::RequireInfoList(Sora::Meta::Map(^^std::decay_t, input));
 * constexpr auto unique = Sora::Meta::Unique(decayed);
 * constexpr auto result = Sora::Meta::Apply(^^std::variant, unique);
 *
 * static_assert(result == ^^std::variant<int, double>);
 * @endcode
 *
 * @par TypeList pipeline
 * @code{.cpp}
 * using Input = Sora::Traits::TypeList<int, double, const double>;
 * using Result = Sora::Traits::Apply<^^std::variant,
 *     Sora::Traits::Unique<Sora::Traits::Map<^^std::decay_t, Input>>>;
 *
 * static_assert(std::same_as<Result, std::variant<int, double>>);
 * @endcode
 *
 * @par Higher-order composition and errors
 * @code{.cpp}
 * constexpr auto decayPointer = Sora::Meta::Compose<^^std::add_pointer_t, ^^std::decay_t>;
 * using Pointer = Sora::Meta::InvokeResult<decayPointer, const int&>;
 * constexpr auto error = Sora::Meta::Map(^^std::pair, Sora::Meta::InfoList{^^int});
 *
 * static_assert(std::same_as<Pointer, int*>);
 * static_assert(Sora::Meta::IsMetaError(error));
 * @endcode
 */
#pragma once

#include <array>
#include <cstddef>
#include <meta>
#include <ranges>
#include <span>
#include <type_traits>
#include <vector>

#include <Sora/Core/Traits/MetaInfoList.h>
#include <Sora/Core/Traits/MetaInvocable.h>
#include <Sora/Core/Traits/MetaPred.h>
#include <Sora/Core/Traits/TypeList.h>

namespace Sora::Meta {

    /**
     * @brief Reflect an InfoList as the success value of a collection algorithm.
     * @param[in] list Static reflection sequence to wrap.
     * @return Reflection of the structural @p list value.
     */
    [[nodiscard]] consteval std::meta::info Reflect(const InfoList& list) {
        return std::meta::reflect_constant(list);
    }

    /**
     * @brief Return whether @p result is a reflected InfoList value.
     * @param[in] result Reflection to inspect.
     * @return `true` only for an object or value whose unqualified type is `InfoList`.
     */
    [[nodiscard]] consteval bool IsInfoList(std::meta::info result) noexcept {
        if (!std::meta::is_object(result) && !std::meta::is_value(result)) {
            return false;
        }
        return std::meta::remove_cvref(std::meta::type_of(result)) == ^^InfoList;
    }

    /**
     * @brief Extract an InfoList success value or issue a translation-time diagnostic.
     * @param[in] result Reflected `InfoList` object returned by a collection algorithm.
     * @return Extracted static sequence view.
     * @throws A translation-time diagnostic when @p result is not a reflected `InfoList`.
     */
    [[nodiscard]] consteval InfoList RequireInfoList(std::meta::info result) {
        if (!IsInfoList(result)) {
            throw std::define_static_string("Meta::RequireInfoList requires a reflected InfoList value.");
        }
        return std::meta::extract<InfoList>(result);
    }

    /**
     * @brief Extract a reflected `std::size_t` counting result or issue a translation-time diagnostic.
     * @param[in] result Reflected value returned by a counting algorithm.
     * @return Extracted count.
     * @throws A translation-time diagnostic for errors or values of another type.
     */
    [[nodiscard]] consteval std::size_t RequireSizeResult(std::meta::info result) {
        if (IsMetaError(result) || !std::meta::is_value(result) ||
            std::meta::dealias(std::meta::remove_cvref(std::meta::type_of(result))) !=
                std::meta::dealias(^^std::size_t)) {
            throw std::define_static_string("Meta::RequireSizeResult requires a reflected size_t result.");
        }
        return std::meta::extract<const std::size_t>(result);
    }

    /**
     * @brief Preserve a reflected type or convert a non-type result into a recoverable meta error.
     * @param[in] result Reflection produced by a type-level operation.
     * @param[in] context Optional provenance entities placed before a non-type result in the error context.
     * @return @p result when it is a type or `MetaError`; otherwise `NonTypeResultTag` with provenance.
     * @par Example
     * @code{.cpp}
     * constexpr auto value = std::meta::reflect_constant(42);
     * constexpr auto result = Sora::Meta::RequireTypeResult(value, {^^int});
     * static_assert(Sora::Meta::IsMetaError(result));
     * @endcode
     */
    [[nodiscard]] consteval std::meta::info RequireTypeResult(std::meta::info result, const InfoList& context = {}) {
        if (IsMetaError(result) || std::meta::is_type(result)) {
            return result;
        }
        std::vector<std::meta::info> values(context.begin(), context.end());
        values.push_back(result);
        return MakeMetaError(^^Error::NonTypeResultTag, values);
    }

    /**
     * @brief Return the first MetaError in @p values, or an empty reflection.
     * @param[in] values Sequence inspected from front to back.
     * @return First error unchanged, or `std::meta::info{}` when every entity is error-free.
     * @par Complexity Linear in the inspected prefix.
     */
    [[nodiscard]] consteval std::meta::info FirstMetaError(const InfoList& values) noexcept {
        for (const auto value : values) {
            if (IsMetaError(value)) {
                return value;
            }
        }
        return {};
    }

    namespace Detail {

        /**
         * @brief Apply a reflected template after validating its function and argument reflections.
         * @param[in] function Reflected template candidate.
         * @param[in] arguments Reflected arguments in call order.
         * @return Substitution result or a recoverable `MetaError`.
         */
        [[nodiscard]] consteval std::meta::info Apply(std::meta::info function,
                                                      std::span<const std::meta::info> arguments) {
            if (IsMetaError(function)) {
                return function;
            }
            if (!std::meta::is_template(function)) {
                return MakeMetaError(^^Error::NonInvocableTag, {function});
            }
            for (const auto argument : arguments) {
                if (IsMetaError(argument)) {
                    return argument;
                }
            }
            if (!std::meta::can_substitute(function, arguments)) {
                std::vector<std::meta::info> context{function};
                context.append_range(arguments);
                return MakeMetaError(^^Error::NonInvocableTag, context);
            }
            return std::meta::substitute(function, arguments);
        }

    } // namespace Detail

    /**
     * @brief Invoke a reflected template with a dynamically assembled argument list.
     * @param[in] function Reflected template to substitute.
     * @param[in] arguments Reflected template arguments in source order.
     * @return Substitution result, the first input error, or `NonInvocableTag` with invocation provenance.
     */
    [[nodiscard]] consteval std::meta::info Apply(std::meta::info function, const InfoList& arguments) {
        return Detail::Apply(function, {arguments.begin(), arguments.size()});
    }

    namespace Detail {

        /** @brief Validate that a reflection denotes a template entity. */
        [[nodiscard]] consteval std::meta::info ValidateTemplate(std::meta::info function) {
            if (IsMetaError(function)) {
                return function;
            }
            return std::meta::is_template(function) ? std::meta::info{}
                                                    : MakeMetaError(^^Error::NonInvocableTag, {function});
        }

        /** @brief Validate the unary boolean predicate protocol used by collection algorithms. */
        [[nodiscard]] consteval std::meta::info ValidateUnaryPred(std::meta::info pred) {
            if (IsMetaError(pred)) {
                return pred;
            }
            if (!std::meta::is_function(pred) || std::meta::parameters_of(pred).size() != 1 ||
                !std::meta::is_invocable_type(std::meta::type_of(pred), {^^std::meta::info})) {
                return MakeMetaError(^^Error::NonInvocableTag, {pred});
            }
            return std::meta::return_type_of(pred) == ^^bool ? std::meta::info{}
                                                             : MakeMetaError(^^Error::NonBooleanPredResultTag, {pred});
        }

        /**
         * @brief Compose reflected templates from right to left for one invocation.
         * @param[in] functions Non-empty function sequence.
         * @param[in] arguments Initial invocation arguments.
         * @return Final reflection or the first recoverable error.
         */
        [[nodiscard]] consteval std::meta::info ComposeInvoke(const InfoList& functions, const InfoList& arguments) {
            if (functions.empty()) {
                return MakeMetaError(^^Error::NonInvocableTag);
            }
            for (const auto function : functions) {
                if (const auto error = ValidateTemplate(function); error != std::meta::info{}) {
                    return error;
                }
            }
            if (const auto error = FirstMetaError(arguments); error != std::meta::info{}) {
                return error;
            }
            auto result = Apply(functions.back(), arguments);
            for (std::size_t index = functions.size() - 1; index > 0 && !IsMetaError(result); --index) {
                result = Apply(functions[index - 1], InfoList{result});
            }
            return result;
        }

        /**
         * @brief Map one reflected template over arguments and pass results to a continuation.
         * @param[in] function Pointwise reflected template.
         * @param[in] continuation Reflected template receiving transformed arguments.
         * @param[in] arguments Invocation arguments.
         * @return Continuation result or the first recoverable error.
         */
        [[nodiscard]] consteval std::meta::info TransformInvoke(std::meta::info function, std::meta::info continuation,
                                                                const InfoList& arguments) {
            if (const auto error = ValidateTemplate(function); error != std::meta::info{}) {
                return error;
            }
            if (const auto error = ValidateTemplate(continuation); error != std::meta::info{}) {
                return error;
            }
            if (const auto error = FirstMetaError(arguments); error != std::meta::info{}) {
                return error;
            }
            std::vector<std::meta::info> transformed;
            transformed.reserve(arguments.size());
            for (const auto argument : arguments) {
                const std::array invocationArguments{argument};
                const auto result = Apply(function, invocationArguments);
                if (IsMetaError(result)) {
                    return result;
                }
                transformed.push_back(result);
            }
            return Apply(continuation, transformed);
        }

        /**
         * @brief Flatten `InfoList` values produced by a reflected unary consteval function.
         * @param[in] function Reflected unary function returning `InfoList`.
         * @param[in] input Source entities.
         * @return Reflected flattened `InfoList`, or the first recoverable mapping/projection error.
         */
        [[nodiscard]] consteval std::meta::info FlatMapInfoLists(std::meta::info function, const InfoList& input) {
            if (IsMetaError(function)) {
                return function;
            }
            if (!std::meta::is_function(function)) {
                return MakeMetaError(^^Error::NonInvocableTag, {function});
            }
            const auto parameters = std::meta::parameters_of(function);
            if (parameters.size() != 1 ||
                !std::meta::is_invocable_type(std::meta::type_of(function), {^^std::meta::info})) {
                return MakeMetaError(^^Error::NonInvocableTag, {function});
            }
            if (std::meta::return_type_of(function) != ^^InfoList) {
                return MakeMetaError(^^Error::NonInfoListResultTag, {function});
            }
            if (const auto error = FirstMetaError(input); error != std::meta::info{}) {
                return error;
            }

            std::vector<std::meta::info> output;
            for (const auto value : input) {
                const std::array arguments{std::meta::reflect_constant(value)};
                const auto mapped = std::meta::reflect_invoke(function, arguments);
                if (IsMetaError(mapped)) {
                    return mapped;
                }
                if (!IsInfoList(mapped)) {
                    return MakeMetaError(^^Error::NonInfoListResultTag, {function, value, mapped});
                }
                const auto mappedList = RequireInfoList(mapped);
                if (const auto error = FirstMetaError(mappedList); error != std::meta::info{}) {
                    return error;
                }
                output.insert(output.end(), mappedList.begin(), mappedList.end());
            }
            return Reflect(output.empty() ? InfoList{} : Promote(output));
        }

    } // namespace Detail

    /**
     * @brief Apply a unary reflected template to every entity.
     * @param[in] function Reflected template accepting one reflected entity.
     * @param[in] input Source sequence; entities need not be types.
     * @return Reflected `InfoList` of dealiased results, or the first `MetaError`.
     * @par Complexity Linear in @p input, excluding template substitution cost.
     */
    [[nodiscard]] consteval std::meta::info Map(std::meta::info function, const InfoList& input) {
        if (IsMetaError(function)) {
            return function;
        }
        if (const auto error = Detail::ValidateTemplate(function); error != std::meta::info{}) {
            return error;
        }
        if (const auto error = FirstMetaError(input); error != std::meta::info{}) {
            return error;
        }
        std::vector<std::meta::info> output;
        output.reserve(input.size());
        for (const auto value : input) {
            const std::array arguments{value};
            const auto mapped = Detail::Apply(function, arguments);
            if (IsMetaError(mapped)) {
                return mapped;
            }
            output.push_back(std::meta::dealias(mapped));
        }
        return Reflect(Promote(output));
    }

    /**
     * @brief Retain entities accepted by a reflected unary predicate.
     * @param[in] pred Reflected function satisfying the unary meta-predicate protocol.
     * @param[in] input Source sequence.
     * @return Reflected stable subsequence, `NonInvocableTag`, `NonBooleanPredResultTag`, or the first input error.
     * @par Complexity Linear in @p input.
     */
    [[nodiscard]] consteval std::meta::info Filter(std::meta::info pred, const InfoList& input) {
        if (IsMetaError(pred)) {
            return pred;
        }
        if (const auto error = Detail::ValidateUnaryPred(pred); error != std::meta::info{}) {
            return error;
        }
        if (const auto error = FirstMetaError(input); error != std::meta::info{}) {
            return error;
        }
        std::vector<std::meta::info> output;
        output.reserve(input.size());
        for (const auto value : input) {
            const auto accepted = InvokePred(pred, value);
            if (IsMetaError(accepted)) {
                return accepted;
            }
            if (std::meta::extract<bool>(accepted)) {
                output.push_back(value);
            }
        }
        return Reflect(Promote(output));
    }

    /**
     * @brief Flatten the reflected `InfoList` returned for every input entity.
     * @param[in] function Reflected unary consteval function accepting `std::meta::info` and returning `InfoList`.
     * @param[in] input Source sequence; entities need not be types.
     * @return Concatenated reflected `InfoList`, the first mapper error, `NonInfoListResultTag`, or a nested input
     * error.
     * @details The mapper result for each entity is validated before the next entity is visited. Nested `MetaError`
     * values are propagated unchanged instead of being silently flattened. Type-pack mapping belongs to
     * `Meta::Types::FlatMap`, so this value-level operation has exactly one codomain.
     * @par Example
     * @code{.cpp}
     * consteval Sora::Meta::InfoList Duplicate(std::meta::info entity) { return {entity, entity}; }
     * constexpr auto result = Sora::Meta::FlatMap(^^Duplicate, Sora::Meta::InfoList{^^int});
     * static_assert(Sora::Meta::Equal(Sora::Meta::RequireInfoList(result),
     *                                  Sora::Meta::InfoList{^^int, ^^int}));
     * @endcode
     * @note As with the meta-predicate protocol, @p function must be invocable during constant evaluation. C++26
     * reflection cannot query that property, so violating it is a translation-time failure rather than `MetaError`.
     * @par Complexity Linear in input and produced sequence lengths, excluding substitution cost.
     */
    [[nodiscard]] consteval std::meta::info FlatMap(std::meta::info function, const InfoList& input) {
        return Detail::FlatMapInfoLists(function, input);
    }

    /**
     * @brief Replace every entity accepted by a reflected unary predicate.
     * @param[in] pred Reflected unary meta predicate.
     * @param[in] replacement Entity inserted for each accepted input.
     * @param[in] input Source sequence.
     * @return Reflected stable sequence, or the first predicate/input error.
     * @par Example
     * @code{.cpp}
     * consteval bool Integral(std::meta::info type) noexcept { return std::meta::is_integral_type(type); }
     * constexpr auto result = Sora::Meta::ReplaceIf(^^Integral, ^^bool,
     *                                               Sora::Meta::InfoList{^^int, ^^double});
     * @endcode
     */
    [[nodiscard]] consteval std::meta::info ReplaceIf(std::meta::info pred, std::meta::info replacement,
                                                      const InfoList& input) {
        if (const auto error = Detail::ValidateUnaryPred(pred); error != std::meta::info{}) {
            return error;
        }
        if (IsMetaError(replacement)) {
            return replacement;
        }
        if (const auto error = FirstMetaError(input); error != std::meta::info{}) {
            return error;
        }
        std::vector<std::meta::info> output;
        output.reserve(input.size());
        for (const auto value : input) {
            const auto accepted = InvokePred(pred, value);
            if (IsMetaError(accepted)) {
                return accepted;
            }
            output.push_back(std::meta::extract<bool>(accepted) ? replacement : value);
        }
        return Reflect(Promote(output));
    }

    /**
     * @brief Remove every entity accepted by a reflected unary predicate.
     * @param[in] pred Reflected unary meta predicate.
     * @param[in] input Source sequence.
     * @return Reflected stable subsequence, or the first predicate/input error.
     * @par Example
     * @code{.cpp}
     * consteval bool Integral(std::meta::info type) noexcept { return std::meta::is_integral_type(type); }
     * constexpr auto result = Sora::Meta::RemoveIf(^^Integral,
     *                                              Sora::Meta::InfoList{^^int, ^^double});
     * static_assert(Sora::Meta::RequireInfoList(result).front() == ^^double);
     * @endcode
     */
    [[nodiscard]] consteval std::meta::info RemoveIf(std::meta::info pred, const InfoList& input) {
        if (IsMetaError(pred)) {
            return pred;
        }
        if (const auto error = Detail::ValidateUnaryPred(pred); error != std::meta::info{}) {
            return error;
        }
        if (const auto error = FirstMetaError(input); error != std::meta::info{}) {
            return error;
        }
        std::vector<std::meta::info> output;
        output.reserve(input.size());
        for (const auto value : input) {
            const auto accepted = InvokePred(pred, value);
            if (IsMetaError(accepted)) {
                return accepted;
            }
            if (!std::meta::extract<bool>(accepted)) {
                output.push_back(value);
            }
        }
        return Reflect(output.empty() ? InfoList{} : Promote(output));
    }

    /**
     * @brief Zip equal-length sequences through a reflected binary template.
     * @param[in] function Reflected template accepting one entity from each input.
     * @param[in] left First source sequence.
     * @param[in] right Second source sequence.
     * @return Reflected `InfoList`, `LengthMismatchTag`, an invocation error, or the first input error.
     * @par Complexity Linear in the common input length.
     */
    [[nodiscard]] consteval std::meta::info Zip(std::meta::info function, const InfoList& left, const InfoList& right) {
        if (IsMetaError(function)) {
            return function;
        }
        if (const auto error = Detail::ValidateTemplate(function); error != std::meta::info{}) {
            return error;
        }
        if (const auto error = FirstMetaError(left); error != std::meta::info{}) {
            return error;
        }
        if (const auto error = FirstMetaError(right); error != std::meta::info{}) {
            return error;
        }
        if (left.size() != right.size()) {
            return MakeMetaError(^^Error::LengthMismatchTag, {function, std::meta::reflect_constant(left.size()),
                                                              std::meta::reflect_constant(right.size())});
        }
        std::vector<std::meta::info> output;
        output.reserve(left.size());
        for (std::size_t index = 0; index < left.size(); ++index) {
            const std::array arguments{left[index], right[index]};
            const auto mapped = Detail::Apply(function, arguments);
            if (IsMetaError(mapped)) {
                return mapped;
            }
            output.push_back(std::meta::dealias(mapped));
        }
        return Reflect(Promote(output));
    }

    /**
     * @brief Left-fold a reflected binary template over @p input.
     * @param[in] function Reflected binary template invoked as `function(accumulator, entity)`.
     * @param[in] initial Initial accumulator entity; it need not be a type.
     * @param[in] input Entities visited from front to back.
     * @return Final accumulator, or the first error encountered.
     */
    [[nodiscard]] consteval std::meta::info FoldLeft(std::meta::info function, std::meta::info initial,
                                                     const InfoList& input) {
        if (input.empty()) {
            return initial;
        }
        if (const auto error = Detail::ValidateTemplate(function); error != std::meta::info{}) {
            return error;
        }
        if (IsMetaError(initial)) {
            return initial;
        }
        if (const auto error = FirstMetaError(input); error != std::meta::info{}) {
            return error;
        }
        auto accumulator = initial;
        for (const auto value : input) {
            const std::array arguments{accumulator, value};
            accumulator = Detail::Apply(function, arguments);
            if (IsMetaError(accumulator)) {
                return accumulator;
            }
        }
        return accumulator;
    }

    /**
     * @brief Right-fold a reflected binary template over @p input.
     * @param[in] function Reflected binary template invoked as `function(entity, accumulator)`.
     * @param[in] initial Initial accumulator entity; it need not be a type.
     * @param[in] input Entities visited from back to front.
     * @return Final accumulator, or the first error in validation and source order.
     */
    [[nodiscard]] consteval std::meta::info FoldRight(std::meta::info function, std::meta::info initial,
                                                      const InfoList& input) {
        if (input.empty()) {
            return initial;
        }
        if (const auto error = Detail::ValidateTemplate(function); error != std::meta::info{}) {
            return error;
        }
        if (IsMetaError(initial)) {
            return initial;
        }
        if (const auto error = FirstMetaError(input); error != std::meta::info{}) {
            return error;
        }
        auto accumulator = initial;
        for (std::size_t index = input.size(); index > 0; --index) {
            const std::array arguments{input[index - 1], accumulator};
            accumulator = Detail::Apply(function, arguments);
            if (IsMetaError(accumulator)) {
                return accumulator;
            }
        }
        return accumulator;
    }

    /**
     * @brief Return the first entity accepted by @p pred, or @p fallback.
     * @param[in] pred Reflected unary meta predicate.
     * @param[in] input Source sequence inspected from front to back.
     * @param[in] fallback Entity returned when no element is accepted.
     * @return Matching entity, @p fallback, or the first predicate/input error.
     */
    [[nodiscard]] consteval std::meta::info FindFirst(std::meta::info pred, const InfoList& input,
                                                      std::meta::info fallback = ^^void) {
        if (IsMetaError(pred)) {
            return pred;
        }
        if (const auto error = Detail::ValidateUnaryPred(pred); error != std::meta::info{}) {
            return error;
        }
        if (const auto error = FirstMetaError(input); error != std::meta::info{}) {
            return error;
        }
        if (IsMetaError(fallback)) {
            return fallback;
        }
        for (const auto value : input) {
            const auto accepted = InvokePred(pred, value);
            if (IsMetaError(accepted)) {
                return accepted;
            }
            if (std::meta::extract<bool>(accepted)) {
                return value;
            }
        }
        return fallback;
    }

    /**
     * @brief Return the final entity accepted by @p pred, or @p fallback.
     * @param[in] pred Reflected unary meta predicate.
     * @param[in] input Source sequence inspected in source order.
     * @param[in] fallback Entity returned when no element is accepted.
     * @return Last matching entity, @p fallback, or the first predicate/input error.
     */
    [[nodiscard]] consteval std::meta::info FindLast(std::meta::info pred, const InfoList& input,
                                                     std::meta::info fallback = ^^void) {
        if (IsMetaError(pred)) {
            return pred;
        }
        if (const auto error = Detail::ValidateUnaryPred(pred); error != std::meta::info{}) {
            return error;
        }
        if (const auto error = FirstMetaError(input); error != std::meta::info{}) {
            return error;
        }
        if (IsMetaError(fallback)) {
            return fallback;
        }
        auto result = fallback;
        for (const auto value : input) {
            const auto accepted = InvokePred(pred, value);
            if (IsMetaError(accepted)) {
                return accepted;
            }
            if (std::meta::extract<bool>(accepted)) {
                result = value;
            }
        }
        return result;
    }

    /**
     * @brief Return whether all entities satisfy @p pred.
     * @param[in] pred Reflected unary meta predicate.
     * @param[in] input Source sequence.
     * @return Reflected bool, or the first predicate/input error. Empty input yields `true` after validating @p pred.
     */
    [[nodiscard]] consteval std::meta::info AllOf(std::meta::info pred, const InfoList& input) {
        if (IsMetaError(pred)) {
            return pred;
        }
        if (const auto error = Detail::ValidateUnaryPred(pred); error != std::meta::info{}) {
            return error;
        }
        if (const auto error = FirstMetaError(input); error != std::meta::info{}) {
            return error;
        }
        for (const auto value : input) {
            const auto accepted = InvokePred(pred, value);
            if (IsMetaError(accepted)) {
                return accepted;
            }
            if (!std::meta::extract<bool>(accepted)) {
                return std::meta::reflect_constant(false);
            }
        }
        return std::meta::reflect_constant(true);
    }

    /**
     * @brief Return whether any entity satisfies @p pred.
     * @param[in] pred Reflected unary meta predicate.
     * @param[in] input Source sequence.
     * @return Reflected bool, or the first predicate/input error. Empty input yields `false` after validating @p pred.
     */
    [[nodiscard]] consteval std::meta::info AnyOf(std::meta::info pred, const InfoList& input) {
        if (IsMetaError(pred)) {
            return pred;
        }
        if (const auto error = Detail::ValidateUnaryPred(pred); error != std::meta::info{}) {
            return error;
        }
        if (const auto error = FirstMetaError(input); error != std::meta::info{}) {
            return error;
        }
        for (const auto value : input) {
            const auto accepted = InvokePred(pred, value);
            if (IsMetaError(accepted)) {
                return accepted;
            }
            if (std::meta::extract<bool>(accepted)) {
                return std::meta::reflect_constant(true);
            }
        }
        return std::meta::reflect_constant(false);
    }

    /**
     * @brief Return whether no entity satisfies @p pred.
     * @param[in] pred Reflected unary meta predicate.
     * @param[in] input Source sequence.
     * @return Negated result of @ref AnyOf, preserving errors unchanged.
     */
    [[nodiscard]] consteval std::meta::info NoneOf(std::meta::info pred, const InfoList& input) {
        const auto result = AnyOf(pred, input);
        return IsMetaError(result) ? result : std::meta::reflect_constant(!std::meta::extract<bool>(result));
    }

    /**
     * @brief Count entities satisfying @p pred.
     * @param[in] pred Reflected unary meta predicate.
     * @param[in] input Source sequence.
     * @return Reflected `std::size_t`, or the first predicate/input error.
     */
    [[nodiscard]] consteval std::meta::info CountIf(std::meta::info pred, const InfoList& input) {
        if (IsMetaError(pred)) {
            return pred;
        }
        if (const auto error = Detail::ValidateUnaryPred(pred); error != std::meta::info{}) {
            return error;
        }
        if (const auto error = FirstMetaError(input); error != std::meta::info{}) {
            return error;
        }
        std::size_t count = 0;
        for (const auto value : input) {
            const auto accepted = InvokePred(pred, value);
            if (IsMetaError(accepted)) {
                return accepted;
            }
            count += std::meta::extract<bool>(accepted);
        }
        return std::meta::reflect_constant(count);
    }

    /**
     * @brief Project a collection result into a reflected `Traits::TypeList`.
     * @param[in] result Reflected `InfoList` success value or `MetaError`.
     * @return Reflected `TypeList`, the input error, `NonInfoListResultTag`, or `NonTypeResultTag`.
     */
    [[nodiscard]] consteval std::meta::info ToTypeList(std::meta::info result) {
        if (IsMetaError(result)) {
            return result;
        }
        if (!IsInfoList(result)) {
            return MakeMetaError(^^Error::NonInfoListResultTag, {result});
        }
        const auto list = RequireInfoList(result);
        if (const auto error = FirstMetaError(list); error != std::meta::info{}) {
            return error;
        }
        for (const auto entity : list) {
            if (!std::meta::is_type(entity)) {
                return MakeMetaError(^^Error::NonTypeResultTag, {entity});
            }
        }
        return ToTypeList(list);
    }

    /**
     * @brief Project the non-static data-member types of a reflected type into a reflected `TypeList`.
     * @param[in] type Reflected class or aggregate type.
     * @return Reflected member `TypeList`, the input error, `NonTypeResultTag` for a non-type reflection, or
     * `NonInvocableTag` for a type that has no class-member domain.
     */
    [[nodiscard]] consteval std::meta::info DataMemberTypes(std::meta::info type) {
        if (IsMetaError(type)) {
            return type;
        }
        if (!std::meta::is_type(type)) {
            return MakeMetaError(^^Error::NonTypeResultTag, {type});
        }
        if (!std::meta::is_class_type(type)) {
            return MakeMetaError(^^Error::NonInvocableTag, {type});
        }
        const auto members = std::meta::nonstatic_data_members_of(type, std::meta::access_context::unchecked()) |
                             std::views::transform(std::meta::type_of);
        return ToTypeList(members);
    }

    namespace Detail {

        /** @brief Validated view of one reflected `TypeList`. */
        struct TypeListInspection {
            InfoList types{};
            std::meta::info error{};

            /** @brief Return whether validation succeeded. */
            [[nodiscard]] consteval explicit operator bool() const noexcept { return error == std::meta::info{}; }
        };

        /**
         * @brief Validate one reflected `TypeList` and expose its error-free elements.
         * @param[in] list Reflected sequence to inspect.
         * @param[in] context Provenance used when @p list is not a `TypeList`.
         * @return Validated elements, or the first top-level/nested error.
         */
        [[nodiscard]] consteval TypeListInspection InspectTypeList(std::meta::info list, const InfoList& context) {
            if (IsMetaError(list)) {
                return {.error = list};
            }
            if (!IsTypeList(list)) {
                return {.error = MakeMetaError(
                            ^^Error::InvalidSequenceTag,
                            std::vector<std::meta::info>{context.begin(), context.end()})};
            }
            const auto types = ToInfoList(list);
            return {.types = types, .error = FirstMetaError(types)};
        }

    } // namespace Detail

    namespace Types {

        /**
         * @brief Concatenate reflected TypeLists in source order.
         * @param[in] lists Sequence whose elements must each reflect a `Traits::TypeList` specialization.
         * @return Reflected concatenated `TypeList`, `InvalidSequenceTag`, or the first error in flattened source
         * order.
         */
        [[nodiscard]] consteval std::meta::info Concat(const InfoList& lists) {
            std::vector<std::meta::info> output;
            for (const auto list : lists) {
                const auto inspected = Detail::InspectTypeList(list, InfoList{list});
                if (!inspected) {
                    return inspected.error;
                }
                output.append_range(inspected.types);
            }
            return ToTypeList(output);
        }

        /**
         * @brief Union reflected `TypeList` operands with stable first-occurrence order.
         * @param[in] lists Sequence of reflected `Traits::TypeList` specializations.
         * @return Reflected union `TypeList`, or the first validation/error result in flattened source order.
         * @par Example
         * @code{.cpp}
         * constexpr auto result = Sora::Meta::Types::SetUnion(
         *     Sora::Meta::InfoList{^^Sora::Traits::TypeList<int>, ^^Sora::Traits::TypeList<int, double>});
         * @endcode
         */
        [[nodiscard]] consteval std::meta::info SetUnion(const InfoList& lists) {
            std::vector<std::meta::info> types;
            for (const auto list : lists) {
                const auto inspected = Detail::InspectTypeList(list, InfoList{list});
                if (!inspected) {
                    return inspected.error;
                }
                types.append_range(inspected.types);
            }
            return ToTypeList(Sora::Meta::Unique(Promote(types)));
        }

        /**
         * @brief Intersect reflected `TypeList` operands from left to right.
         * @param[in] left Reflected `TypeList` whose order is preserved.
         * @param[in] right Reflected `TypeList` used for membership.
         * @return Reflected intersection `TypeList`, or the first validation/error result.
         * @par Example
         * @code{.cpp}
         * constexpr auto result = Sora::Meta::Types::SetIntersection(
         *     ^^Sora::Traits::TypeList<int, double>, ^^Sora::Traits::TypeList<double, char>);
         * @endcode
         */
        [[nodiscard]] consteval std::meta::info SetIntersection(std::meta::info left, std::meta::info right) {
            const InfoList context{left, right};
            const auto leftList = Detail::InspectTypeList(left, context);
            if (!leftList) {
                return leftList.error;
            }
            const auto rightList = Detail::InspectTypeList(right, context);
            if (!rightList) {
                return rightList.error;
            }
            return ToTypeList(Sora::Meta::SetIntersection(leftList.types, rightList.types));
        }

        /**
         * @brief Subtract the right reflected `TypeList` from the left one.
         * @param[in] left Reflected `TypeList` whose order is preserved.
         * @param[in] right Reflected `TypeList` used for membership.
         * @return Reflected difference `TypeList`, or the first validation/error result.
         * @par Example
         * @code{.cpp}
         * constexpr auto result = Sora::Meta::Types::SetDifference(
         *     ^^Sora::Traits::TypeList<int, double>, ^^Sora::Traits::TypeList<double, char>);
         * @endcode
         */
        [[nodiscard]] consteval std::meta::info SetDifference(std::meta::info left, std::meta::info right) {
            const InfoList context{left, right};
            const auto leftList = Detail::InspectTypeList(left, context);
            if (!leftList) {
                return leftList.error;
            }
            const auto rightList = Detail::InspectTypeList(right, context);
            if (!rightList) {
                return rightList.error;
            }
            return ToTypeList(Sora::Meta::SetDifference(leftList.types, rightList.types));
        }

        /**
         * @brief Return the number of types in a reflected `TypeList`.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Reflected `std::size_t`, or the input `MetaError`/`InvalidSequenceTag`.
         * @par Example
         * @code{.cpp}
         * constexpr auto count = Sora::Meta::Types::Length(^^Sora::Traits::TypeList<int, double>);
         * static_assert(std::meta::extract<std::size_t>(count) == 2);
         * @endcode
         */
        [[nodiscard]] consteval std::meta::info Length(std::meta::info list) {
            const auto inspected = Detail::InspectTypeList(list, InfoList{list});
            if (!inspected) {
                return inspected.error;
            }
            return std::meta::reflect_constant(inspected.types.size());
        }

        /**
         * @brief Return the type at @p index in a reflected `TypeList`.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @param[in] index Zero-based type index.
         * @return Reflected type, or the input `MetaError`, `InvalidSequenceTag`, or `OutOfRangeTag` when @p index
         * is outside the list.
         * @par Example
         * @code{.cpp}
         * constexpr auto type = Sora::Meta::Types::At(^^Sora::Traits::TypeList<int, double>, 1);
         * static_assert(type == ^^double);
         * @endcode
         */
        [[nodiscard]] consteval std::meta::info At(std::meta::info list, std::size_t index) {
            const auto inspected = Detail::InspectTypeList(list, InfoList{list});
            if (!inspected) {
                return inspected.error;
            }
            if (index >= inspected.types.size()) {
                return MakeMetaError(^^Error::OutOfRangeTag, {list});
            }
            return inspected.types[index];
        }

        /**
         * @brief Return the first type in a reflected `TypeList`.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Its first reflected type, or the input `MetaError`, `InvalidSequenceTag`, or `OutOfRangeTag`
         * for an empty list.
         * @par Example
         * @code{.cpp}
         * constexpr auto type = Sora::Meta::Types::Front(^^Sora::Traits::TypeList<int, double>);
         * static_assert(type == ^^int);
         * @endcode
         */
        [[nodiscard]] consteval std::meta::info Front(std::meta::info list) {
            return At(list, 0);
        }

        /**
         * @brief Return the final type in a reflected `TypeList`.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Its final reflected type, or the input `MetaError`, `InvalidSequenceTag`, or `OutOfRangeTag`
         * for an empty list.
         * @par Example
         * @code{.cpp}
         * constexpr auto type = Sora::Meta::Types::Back(^^Sora::Traits::TypeList<int, double>);
         * static_assert(type == ^^double);
         * @endcode
         */
        [[nodiscard]] consteval std::meta::info Back(std::meta::info list) {
            const auto inspected = Detail::InspectTypeList(list, InfoList{list});
            if (!inspected) {
                return inspected.error;
            }
            if (inspected.types.empty()) {
                return MakeMetaError(^^Error::OutOfRangeTag, {list});
            }
            return inspected.types.back();
        }

        /**
         * @brief Return a reflected `TypeList` in reverse order.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Reversed reflected `TypeList`, or the input `MetaError`, `InvalidSequenceTag`, or nested
         * projection error.
         * @par Example
         * @code{.cpp}
         * constexpr auto result = Sora::Meta::Types::Reverse(^^Sora::Traits::TypeList<int, double>);
         * static_assert(result == ^^Sora::Traits::TypeList<double, int>);
         * @endcode
         * @par Complexity Linear in the number of types.
         */
        [[nodiscard]] consteval std::meta::info Reverse(std::meta::info list) {
            const auto inspected = Detail::InspectTypeList(list, InfoList{list});
            if (!inspected) {
                return inspected.error;
            }
            return ToTypeList(Sora::Meta::Reverse(inspected.types));
        }

        /**
         * @brief Remove duplicate types from a reflected `TypeList`.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Stable first-occurrence `TypeList`, or the input `MetaError`, `InvalidSequenceTag`, or nested
         * projection error.
         * @par Example
         * @code{.cpp}
         * constexpr auto result = Sora::Meta::Types::Unique(^^Sora::Traits::TypeList<int, double, int>);
         * static_assert(result == ^^Sora::Traits::TypeList<int, double>);
         * @endcode
         * @par Complexity Quadratic in the number of types because identity is checked pairwise.
         */
        [[nodiscard]] consteval std::meta::info Unique(std::meta::info list) {
            const auto inspected = Detail::InspectTypeList(list, InfoList{list});
            if (!inspected) {
                return inspected.error;
            }
            return ToTypeList(Sora::Meta::Unique(inspected.types));
        }

        /**
         * @brief Return the first @p count types of a reflected `TypeList`.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @param[in] count Number of requested types; values beyond the length are clamped.
         * @return Reflected prefix `TypeList`, or the input `MetaError`, `InvalidSequenceTag`, or nested projection
         * error. An empty input or a zero @p count produces an empty list.
         * @par Example
         * @code{.cpp}
         * constexpr auto result = Sora::Meta::Types::Take(^^Sora::Traits::TypeList<int, double>, 1);
         * static_assert(result == ^^Sora::Traits::TypeList<int>);
         * @endcode
         */
        [[nodiscard]] consteval std::meta::info Take(std::meta::info list, std::size_t count) {
            const auto inspected = Detail::InspectTypeList(list, InfoList{list});
            if (!inspected) {
                return inspected.error;
            }
            return ToTypeList(Sora::Meta::Take(inspected.types, count));
        }

        /**
         * @brief Drop the first @p count types of a reflected `TypeList`.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @param[in] count Number of types to skip; values beyond the length produce an empty list.
         * @return Reflected suffix `TypeList`, or the input `MetaError`, `InvalidSequenceTag`, or nested projection
         * error. Dropping at least the list length produces an empty list.
         * @par Example
         * @code{.cpp}
         * constexpr auto result = Sora::Meta::Types::Drop(^^Sora::Traits::TypeList<int, double>, 1);
         * static_assert(result == ^^Sora::Traits::TypeList<double>);
         * @endcode
         */
        [[nodiscard]] consteval std::meta::info Drop(std::meta::info list, std::size_t count) {
            const auto inspected = Detail::InspectTypeList(list, InfoList{list});
            if (!inspected) {
                return inspected.error;
            }
            return ToTypeList(Sora::Meta::Drop(inspected.types, count));
        }

        /**
         * @brief Return an exact bounded slice of a reflected `TypeList`.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @param[in] offset Zero-based first type.
         * @param[in] count Number of selected types.
         * @return Reflected slice, or `OutOfRangeTag` when bounds are invalid.
         * @note Unlike `Take` and `Drop`, @p offset plus @p count must fit exactly in the source list.
         * @par Example
         * @code{.cpp}
         * constexpr auto result = Sora::Meta::Types::Slice(
         *     ^^Sora::Traits::TypeList<int, double, char>, 1, 2);
         * static_assert(result == ^^Sora::Traits::TypeList<double, char>);
         * @endcode
         */
        [[nodiscard]] consteval std::meta::info Slice(std::meta::info list, std::size_t offset, std::size_t count) {
            const auto inspected = Detail::InspectTypeList(list, InfoList{list});
            if (!inspected) {
                return inspected.error;
            }
            if (offset > inspected.types.size() || count > inspected.types.size() - offset) {
                return MakeMetaError(^^Error::OutOfRangeTag, {list});
            }
            return ToTypeList(Sora::Meta::Slice(inspected.types, offset, count));
        }

        /**
         * @brief Prepend a reflected type to a reflected `TypeList`.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @param[in] value Reflected type to insert before the existing sequence.
         * @return Reflected `TypeList`, the first input error, `InvalidSequenceTag`, or `NonTypeResultTag` when
         * @p value is not a type.
         * @par Example
         * @code{.cpp}
         * constexpr auto result = Sora::Meta::Types::PushFront(^^Sora::Traits::TypeList<int>, ^^bool);
         * static_assert(result == ^^Sora::Traits::TypeList<bool, int>);
         * @endcode
         */
        [[nodiscard]] consteval std::meta::info PushFront(std::meta::info list, std::meta::info value) {
            const auto inspected = Detail::InspectTypeList(list, InfoList{list, value});
            if (!inspected) {
                return inspected.error;
            }
            if (IsMetaError(value)) {
                return value;
            }
            if (!std::meta::is_type(value)) {
                return MakeMetaError(^^Error::NonTypeResultTag, {value});
            }
            return ToTypeList(Sora::Meta::PushFront(inspected.types, value));
        }

        /**
         * @brief Append a reflected type to a reflected `TypeList`.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @param[in] value Reflected type to append after the existing sequence.
         * @return Reflected `TypeList`, the first input error, `InvalidSequenceTag`, or `NonTypeResultTag` when
         * @p value is not a type.
         * @par Example
         * @code{.cpp}
         * constexpr auto result = Sora::Meta::Types::PushBack(^^Sora::Traits::TypeList<int>, ^^bool);
         * static_assert(result == ^^Sora::Traits::TypeList<int, bool>);
         * @endcode
         */
        [[nodiscard]] consteval std::meta::info PushBack(std::meta::info list, std::meta::info value) {
            const auto inspected = Detail::InspectTypeList(list, InfoList{list, value});
            if (!inspected) {
                return inspected.error;
            }
            if (IsMetaError(value)) {
                return value;
            }
            if (!std::meta::is_type(value)) {
                return MakeMetaError(^^Error::NonTypeResultTag, {value});
            }
            return ToTypeList(Sora::Meta::PushBack(inspected.types, value));
        }

        /**
         * @brief Remove the first type from a reflected `TypeList`.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Reflected suffix; an empty input yields an empty list, while input and nested errors are propagated.
         * @par Example
         * @code{.cpp}
         * constexpr auto result = Sora::Meta::Types::PopFront(^^Sora::Traits::TypeList<int, double>);
         * static_assert(result == ^^Sora::Traits::TypeList<double>);
         * @endcode
         */
        [[nodiscard]] consteval std::meta::info PopFront(std::meta::info list) {
            return Drop(list, 1);
        }

        /**
         * @brief Remove the final type from a reflected `TypeList`.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Reflected prefix; an empty input yields an empty list, while input and nested errors are propagated.
         * @par Example
         * @code{.cpp}
         * constexpr auto result = Sora::Meta::Types::PopBack(^^Sora::Traits::TypeList<int, double>);
         * static_assert(result == ^^Sora::Traits::TypeList<int>);
         * @endcode
         */
        [[nodiscard]] consteval std::meta::info PopBack(std::meta::info list) {
            const auto inspected = Detail::InspectTypeList(list, InfoList{list});
            if (!inspected) {
                return inspected.error;
            }
            return ToTypeList(Sora::Meta::PopBack(inspected.types));
        }

        /**
         * @brief Test whether a reflected `TypeList` contains @p needle.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @param[in] needle Reflected entity compared by reflection identity.
         * @return Reflected `bool`, or the first input/sequence error.
         * @par Example
         * @code{.cpp}
         * constexpr auto result = Sora::Meta::Types::Contains(^^Sora::Traits::TypeList<int, double>, ^^double);
         * static_assert(std::meta::extract<bool>(result));
         * @endcode
         */
        [[nodiscard]] consteval std::meta::info Contains(std::meta::info list, std::meta::info needle) {
            const auto inspected = Detail::InspectTypeList(list, InfoList{list, needle});
            if (!inspected) {
                return inspected.error;
            }
            if (IsMetaError(needle)) {
                return needle;
            }
            if (!std::meta::is_type(needle)) {
                return MakeMetaError(^^Error::NonTypeResultTag, {needle});
            }
            return std::meta::reflect_constant(Sora::Meta::Contains(inspected.types, needle));
        }

        /**
         * @brief Return the first index of @p needle in a reflected `TypeList`.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @param[in] needle Reflected entity to find.
         * @return Reflected `std::size_t`; a missing value is `TypeList::npos`.
         * @par Example
         * @code{.cpp}
         * constexpr auto result = Sora::Meta::Types::IndexOf(^^Sora::Traits::TypeList<int, double>, ^^double);
         * static_assert(std::meta::extract<std::size_t>(result) == 1);
         * @endcode
         */
        [[nodiscard]] consteval std::meta::info IndexOf(std::meta::info list, std::meta::info needle) {
            const auto inspected = Detail::InspectTypeList(list, InfoList{list, needle});
            if (!inspected) {
                return inspected.error;
            }
            if (IsMetaError(needle)) {
                return needle;
            }
            if (!std::meta::is_type(needle)) {
                return MakeMetaError(^^Error::NonTypeResultTag, {needle});
            }
            return std::meta::reflect_constant(Sora::Meta::IndexOf(inspected.types, needle));
        }

        /**
         * @brief Count occurrences of @p needle in a reflected `TypeList`.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @param[in] needle Reflected entity to count.
         * @return Reflected `std::size_t`, or the first input/sequence error.
         * @par Example
         * @code{.cpp}
         * constexpr auto result = Sora::Meta::Types::Count(^^Sora::Traits::TypeList<int, int>, ^^int);
         * static_assert(std::meta::extract<std::size_t>(result) == 2);
         * @endcode
         */
        [[nodiscard]] consteval std::meta::info Count(std::meta::info list, std::meta::info needle) {
            const auto inspected = Detail::InspectTypeList(list, InfoList{list, needle});
            if (!inspected) {
                return inspected.error;
            }
            if (IsMetaError(needle)) {
                return needle;
            }
            if (!std::meta::is_type(needle)) {
                return MakeMetaError(^^Error::NonTypeResultTag, {needle});
            }
            return std::meta::reflect_constant(Sora::Meta::Count(inspected.types, needle));
        }

        /**
         * @brief Test whether every type in a reflected `TypeList` occurs exactly once.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Reflected `bool`; empty lists are unique, while nested errors are propagated.
         * @par Example
         * @code{.cpp}
         * constexpr auto result = Sora::Meta::Types::IsUnique(^^Sora::Traits::TypeList<int, double>);
         * static_assert(std::meta::extract<bool>(result));
         * @endcode
         */
        [[nodiscard]] consteval std::meta::info IsUnique(std::meta::info list) {
            const auto inspected = Detail::InspectTypeList(list, InfoList{list});
            if (!inspected) {
                return inspected.error;
            }
            return std::meta::reflect_constant(Sora::Meta::IsUnique(inspected.types));
        }

        /**
         * @brief Compare two reflected `TypeList` sequences element by element.
         * @param[in] left First reflected `Traits::TypeList` specialization.
         * @param[in] right Second reflected `Traits::TypeList` specialization.
         * @return Reflected `bool`, or the first input/sequence error.
         * @par Example
         * @code{.cpp}
         * constexpr auto result = Sora::Meta::Types::Equal(^^Sora::Traits::TypeList<int>,
         *                                                   ^^Sora::Traits::TypeList<int>);
         * static_assert(std::meta::extract<bool>(result));
         * @endcode
         */
        [[nodiscard]] consteval std::meta::info Equal(std::meta::info left, std::meta::info right) {
            const InfoList context{left, right};
            const auto leftList = Detail::InspectTypeList(left, context);
            if (!leftList) {
                return leftList.error;
            }
            const auto rightList = Detail::InspectTypeList(right, context);
            if (!rightList) {
                return rightList.error;
            }
            return std::meta::reflect_constant(Sora::Meta::Equal(leftList.types, rightList.types));
        }

        /**
         * @brief Apply a reflected template to all types in a reflected TypeList.
         * @param[in] function Reflected variadic template.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Reflected type result, `NonTypeResultTag`, `InvalidSequenceTag`, an invocation error, or the first
         * input error.
         */
        [[nodiscard]] consteval std::meta::info Apply(std::meta::info function, std::meta::info list) {
            if (IsMetaError(function)) {
                return function;
            }
            if (const auto error = Detail::ValidateTemplate(function); error != std::meta::info{}) {
                return error;
            }
            const auto inspected = Detail::InspectTypeList(list, InfoList{function, list});
            if (!inspected) {
                return inspected.error;
            }
            return RequireTypeResult(Sora::Meta::Apply(function, inspected.types), {function, list});
        }

        /**
         * @brief Map a reflected unary template over a reflected TypeList.
         * @param[in] function Reflected unary template.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Reflected `TypeList`; non-type mapped results produce `NonTypeResultTag`.
         */
        [[nodiscard]] consteval std::meta::info Map(std::meta::info function, std::meta::info list) {
            if (IsMetaError(function)) {
                return function;
            }
            if (const auto error = Detail::ValidateTemplate(function); error != std::meta::info{}) {
                return error;
            }
            const auto inspected = Detail::InspectTypeList(list, InfoList{function, list});
            if (!inspected) {
                return inspected.error;
            }
            return ToTypeList(Sora::Meta::Map(function, inspected.types));
        }

        /**
         * @brief Flatten a reflected `TypeList` produced by a unary type mapper.
         * @param[in] function Reflected unary alias template returning a `Traits::TypeList` specialization.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Concatenated `TypeList`, or the first mapper/projection error.
         */
        [[nodiscard]] consteval std::meta::info FlatMap(std::meta::info function, std::meta::info list) {
            if (IsMetaError(function)) {
                return function;
            }
            if (const auto error = Detail::ValidateTemplate(function); error != std::meta::info{}) {
                return error;
            }
            const auto inspected = Detail::InspectTypeList(list, InfoList{function, list});
            if (!inspected) {
                return inspected.error;
            }
            const auto input = inspected.types;
            std::vector<std::meta::info> output;
            for (const auto value : input) {
                const auto mapped = Detail::Apply(function, std::array{value});
                if (IsMetaError(mapped)) {
                    return mapped;
                }
                if (!IsTypeList(mapped)) {
                    return MakeMetaError(^^Error::InvalidSequenceTag, {function, value, mapped});
                }
                const auto mappedTypes = ToInfoList(mapped);
                if (const auto error = FirstMetaError(mappedTypes); error != std::meta::info{}) {
                    return error;
                }
                output.append_range(mappedTypes);
            }
            return ToTypeList(output);
        }

        /**
         * @brief Filter a reflected TypeList with a reflected unary predicate.
         * @param[in] pred Reflected unary meta predicate.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Reflected filtered `TypeList`, predicate error, sequence error, or the first input error.
         */
        [[nodiscard]] consteval std::meta::info Filter(std::meta::info pred, std::meta::info list) {
            if (IsMetaError(pred)) {
                return pred;
            }
            if (const auto error = Detail::ValidateUnaryPred(pred); error != std::meta::info{}) {
                return error;
            }
            const auto inspected = Detail::InspectTypeList(list, InfoList{pred, list});
            if (!inspected) {
                return inspected.error;
            }
            return ToTypeList(Sora::Meta::Filter(pred, inspected.types));
        }

        /**
         * @brief Replace every type accepted by a reflected predicate.
         * @param[in] pred Reflected unary meta predicate.
         * @param[in] replacement Reflected replacement type.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Reflected `TypeList`, or the first predicate/projection error.
         */
        [[nodiscard]] consteval std::meta::info ReplaceIf(std::meta::info pred, std::meta::info replacement,
                                                          std::meta::info list) {
            if (const auto error = Detail::ValidateUnaryPred(pred); error != std::meta::info{}) {
                return error;
            }
            if (IsMetaError(replacement)) {
                return replacement;
            }
            if (!std::meta::is_type(replacement)) {
                return MakeMetaError(^^Error::NonTypeResultTag, {pred, replacement});
            }
            const auto inspected = Detail::InspectTypeList(list, InfoList{pred, replacement, list});
            if (!inspected) {
                return inspected.error;
            }
            return ToTypeList(Sora::Meta::ReplaceIf(pred, replacement, inspected.types));
        }

        /**
         * @brief Remove every type accepted by a reflected predicate.
         * @param[in] pred Reflected unary meta predicate.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Reflected filtered `TypeList`, or the first predicate/projection error.
         */
        [[nodiscard]] consteval std::meta::info RemoveIf(std::meta::info pred, std::meta::info list) {
            if (IsMetaError(pred)) {
                return pred;
            }
            if (const auto error = Detail::ValidateUnaryPred(pred); error != std::meta::info{}) {
                return error;
            }
            const auto inspected = Detail::InspectTypeList(list, InfoList{pred, list});
            if (!inspected) {
                return inspected.error;
            }
            return ToTypeList(Sora::Meta::RemoveIf(pred, inspected.types));
        }

        /**
         * @brief Zip two reflected TypeLists with a reflected binary template.
         * @param[in] function Reflected binary template.
         * @param[in] left First reflected `Traits::TypeList`.
         * @param[in] right Second reflected `Traits::TypeList`.
         * @return Reflected result `TypeList`, length/invocation/type error, or the first input error.
         */
        [[nodiscard]] consteval std::meta::info Zip(std::meta::info function, std::meta::info left,
                                                    std::meta::info right) {
            if (IsMetaError(function)) {
                return function;
            }
            if (const auto error = Detail::ValidateTemplate(function); error != std::meta::info{}) {
                return error;
            }
            const InfoList context{function, left, right};
            const auto leftList = Detail::InspectTypeList(left, context);
            if (!leftList) {
                return leftList.error;
            }
            const auto rightList = Detail::InspectTypeList(right, context);
            if (!rightList) {
                return rightList.error;
            }
            return ToTypeList(Sora::Meta::Zip(function, leftList.types, rightList.types));
        }

        /**
         * @brief Replace every occurrence of @p oldType in a reflected TypeList.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @param[in] oldType Reflected type to replace.
         * @param[in] newType Reflected replacement type.
         * @return Reflected result `TypeList`, validation error, or the first input error.
         */
        [[nodiscard]] consteval std::meta::info Replace(std::meta::info list, std::meta::info oldType,
                                                        std::meta::info newType) {
            const auto inspected = Detail::InspectTypeList(list, InfoList{list, oldType, newType});
            if (!inspected) {
                return inspected.error;
            }
            if (IsMetaError(oldType)) {
                return oldType;
            }
            if (!std::meta::is_type(oldType)) {
                return MakeMetaError(^^Error::NonTypeResultTag, {oldType, newType});
            }
            if (IsMetaError(newType)) {
                return newType;
            }
            if (!std::meta::is_type(newType)) {
                return MakeMetaError(^^Error::NonTypeResultTag, {oldType, newType});
            }
            return ToTypeList(Sora::Meta::Replace(inspected.types, oldType, newType));
        }

        /**
         * @brief Remove every occurrence of @p needle from a reflected TypeList.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @param[in] needle Reflected type to remove.
         * @return Reflected result `TypeList`, validation error, or the first input error.
         */
        [[nodiscard]] consteval std::meta::info Remove(std::meta::info list, std::meta::info needle) {
            const auto inspected = Detail::InspectTypeList(list, InfoList{list, needle});
            if (!inspected) {
                return inspected.error;
            }
            if (IsMetaError(needle)) {
                return needle;
            }
            if (!std::meta::is_type(needle)) {
                return MakeMetaError(^^Error::NonTypeResultTag, {needle});
            }
            return ToTypeList(Sora::Meta::Remove(inspected.types, needle));
        }

        /**
         * @brief Construct a reflected TypeList containing @p count copies of @p value.
         * @param[in] count Number of elements.
         * @param[in] value Reflected type to repeat.
         * @return Reflected result `TypeList`, `NonTypeResultTag`, or @p value when it is already an error.
         */
        [[nodiscard]] consteval std::meta::info Fill(std::size_t count, std::meta::info value) {
            if (IsMetaError(value)) {
                return value;
            }
            if (!std::meta::is_type(value)) {
                return MakeMetaError(^^Error::NonTypeResultTag, {value});
            }
            return ToTypeList(std::vector<std::meta::info>(count, value));
        }

        /**
         * @brief Discard all elements of a reflected `TypeList`.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Empty reflected `TypeList`, or the first input/sequence error.
         * @note Nested `MetaError` elements are propagated before clearing so the first-error law is preserved.
         */
        [[nodiscard]] consteval std::meta::info Clear(std::meta::info list) {
            const auto inspected = Detail::InspectTypeList(list, InfoList{list});
            if (!inspected) {
                return inspected.error;
            }
            return ToTypeList(InfoList{});
        }

        /**
         * @brief Left-fold a reflected binary template over a reflected `TypeList`.
         * @param[in] function Reflected binary template invoked as `function(accumulator, element)`.
         * @param[in] initial Initial reflected type.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Final reflected type, or the first validation, nested, invocation, or non-type result error.
         * @par Complexity Linear in the list length, excluding substitution cost.
         */
        [[nodiscard]] consteval std::meta::info FoldLeft(std::meta::info function, std::meta::info initial,
                                                         std::meta::info list) {
            if (!IsMetaError(list) && IsTypeList(list) && ToInfoList(list).empty()) {
                if (IsMetaError(initial)) {
                    return initial;
                }
                if (!std::meta::is_type(initial)) {
                    return MakeMetaError(^^Error::NonTypeResultTag, {function, initial});
                }
                return initial;
            }
            if (const auto error = Detail::ValidateTemplate(function); error != std::meta::info{}) {
                return error;
            }
            if (IsMetaError(initial)) {
                return initial;
            }
            if (!std::meta::is_type(initial)) {
                return MakeMetaError(^^Error::NonTypeResultTag, {function, initial});
            }
            if (IsMetaError(list)) {
                return list;
            }
            if (!IsTypeList(list)) {
                return MakeMetaError(^^Error::InvalidSequenceTag, {function, initial, list});
            }
            const auto types = ToInfoList(list);
            if (const auto error = FirstMetaError(types); error != std::meta::info{}) {
                return error;
            }
            return RequireTypeResult(Sora::Meta::FoldLeft(function, initial, types), {function, initial, list});
        }

        /**
         * @brief Right-fold a reflected binary template over a reflected `TypeList`.
         * @param[in] function Reflected binary template invoked as `function(element, accumulator)`.
         * @param[in] initial Initial reflected type.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Final reflected type, or the first validation, nested, invocation, or non-type result error.
         * @par Complexity Linear in the list length, excluding substitution cost.
         */
        [[nodiscard]] consteval std::meta::info FoldRight(std::meta::info function, std::meta::info initial,
                                                          std::meta::info list) {
            if (!IsMetaError(list) && IsTypeList(list) && ToInfoList(list).empty()) {
                if (IsMetaError(initial)) {
                    return initial;
                }
                if (!std::meta::is_type(initial)) {
                    return MakeMetaError(^^Error::NonTypeResultTag, {function, initial});
                }
                return initial;
            }
            if (const auto error = Detail::ValidateTemplate(function); error != std::meta::info{}) {
                return error;
            }
            if (IsMetaError(initial)) {
                return initial;
            }
            if (!std::meta::is_type(initial)) {
                return MakeMetaError(^^Error::NonTypeResultTag, {function, initial});
            }
            if (IsMetaError(list)) {
                return list;
            }
            if (!IsTypeList(list)) {
                return MakeMetaError(^^Error::InvalidSequenceTag, {function, initial, list});
            }
            const auto types = ToInfoList(list);
            if (const auto error = FirstMetaError(types); error != std::meta::info{}) {
                return error;
            }
            return RequireTypeResult(Sora::Meta::FoldRight(function, initial, types), {function, initial, list});
        }

        /**
         * @brief Find the first type accepted by a reflected predicate.
         * @param[in] pred Reflected unary boolean predicate.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @param[in] fallback Reflected type returned when no element matches.
         * @return Matching type, @p fallback, or the first validation/error result.
         * @par Example
         * @code{.cpp}
         * consteval bool Integral(std::meta::info type) { return std::meta::is_integral_type(type); }
         * constexpr auto result = Sora::Meta::Types::FindFirst(
         *     ^^Integral, ^^Sora::Traits::TypeList<double, int>);
         * static_assert(result == ^^int);
         * @endcode
         */
        [[nodiscard]] consteval std::meta::info FindFirst(std::meta::info pred, std::meta::info list,
                                                          std::meta::info fallback = ^^void) {
            if (IsMetaError(pred)) {
                return pred;
            }
            if (const auto error = Detail::ValidateUnaryPred(pred); error != std::meta::info{}) {
                return error;
            }
            const auto inspected = Detail::InspectTypeList(list, InfoList{pred, list, fallback});
            if (!inspected) {
                return inspected.error;
            }
            if (IsMetaError(fallback)) {
                return fallback;
            }
            if (!std::meta::is_type(fallback)) {
                return MakeMetaError(^^Error::NonTypeResultTag, {pred, fallback});
            }
            return RequireTypeResult(Sora::Meta::FindFirst(pred, inspected.types, fallback), {pred, list, fallback});
        }

        /**
         * @brief Find the final type accepted by a reflected predicate.
         * @param[in] pred Reflected unary boolean predicate.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @param[in] fallback Reflected type returned when no element matches.
         * @return Matching type, @p fallback, or the first validation/error result.
         * @see FindFirst
         */
        [[nodiscard]] consteval std::meta::info FindLast(std::meta::info pred, std::meta::info list,
                                                         std::meta::info fallback = ^^void) {
            if (IsMetaError(pred)) {
                return pred;
            }
            if (const auto error = Detail::ValidateUnaryPred(pred); error != std::meta::info{}) {
                return error;
            }
            const auto inspected = Detail::InspectTypeList(list, InfoList{pred, list, fallback});
            if (!inspected) {
                return inspected.error;
            }
            if (IsMetaError(fallback)) {
                return fallback;
            }
            if (!std::meta::is_type(fallback)) {
                return MakeMetaError(^^Error::NonTypeResultTag, {pred, fallback});
            }
            return RequireTypeResult(Sora::Meta::FindLast(pred, inspected.types, fallback), {pred, list, fallback});
        }

        /**
         * @brief Test whether every type in a reflected `TypeList` satisfies a predicate.
         * @param[in] pred Reflected unary boolean predicate.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Reflected bool; empty lists yield `true` after predicate validation.
         */
        [[nodiscard]] consteval std::meta::info AllOf(std::meta::info pred, std::meta::info list) {
            if (IsMetaError(pred)) {
                return pred;
            }
            if (const auto error = Detail::ValidateUnaryPred(pred); error != std::meta::info{}) {
                return error;
            }
            const auto inspected = Detail::InspectTypeList(list, InfoList{pred, list});
            if (!inspected) {
                return inspected.error;
            }
            return Sora::Meta::AllOf(pred, inspected.types);
        }

        /**
         * @brief Test whether any type in a reflected `TypeList` satisfies a predicate.
         * @param[in] pred Reflected unary boolean predicate.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Reflected `bool`; empty lists yield `false`, while validation errors are propagated unchanged.
         * @par Example
         * @code{.cpp}
         * constexpr auto result = Sora::Meta::Types::AnyOf(
         *     ^^IsIntegral, ^^Sora::Traits::TypeList<double, int>);
         * static_assert(std::meta::extract<bool>(result));
         * @endcode
         */
        [[nodiscard]] consteval std::meta::info AnyOf(std::meta::info pred, std::meta::info list) {
            if (IsMetaError(pred)) {
                return pred;
            }
            if (const auto error = Detail::ValidateUnaryPred(pred); error != std::meta::info{}) {
                return error;
            }
            const auto inspected = Detail::InspectTypeList(list, InfoList{pred, list});
            if (!inspected) {
                return inspected.error;
            }
            return Sora::Meta::AnyOf(pred, inspected.types);
        }

        /**
         * @brief Test whether no type in a reflected `TypeList` satisfies a predicate.
         * @param[in] pred Reflected unary boolean predicate.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Reflected `bool`; empty lists yield `true`, while validation errors are propagated unchanged.
         * @par Example
         * @code{.cpp}
         * constexpr auto result = Sora::Meta::Types::NoneOf(
         *     ^^IsIntegral, ^^Sora::Traits::TypeList<double, float>);
         * static_assert(std::meta::extract<bool>(result));
         * @endcode
         */
        [[nodiscard]] consteval std::meta::info NoneOf(std::meta::info pred, std::meta::info list) {
            const auto result = AnyOf(pred, list);
            return IsMetaError(result) ? result : std::meta::reflect_constant(!std::meta::extract<bool>(result));
        }

        /**
         * @brief Count types in a reflected `TypeList` satisfying a predicate.
         * @param[in] pred Reflected unary boolean predicate.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Reflected `std::size_t`, or the first validation/error result.
         */
        [[nodiscard]] consteval std::meta::info CountIf(std::meta::info pred, std::meta::info list) {
            if (IsMetaError(pred)) {
                return pred;
            }
            if (const auto error = Detail::ValidateUnaryPred(pred); error != std::meta::info{}) {
                return error;
            }
            const auto inspected = Detail::InspectTypeList(list, InfoList{pred, list});
            if (!inspected) {
                return inspected.error;
            }
            return Sora::Meta::CountIf(pred, inspected.types);
        }

    } // namespace Types

    namespace Detail {

        /** @brief Alias-template closure that prepends fixed reflected arguments. */
        template<std::meta::info Function, std::meta::info... Front>
        struct BindFrontClosure {
            template<typename... Arguments>
            using Apply = InvokeResultType<Function, Front..., ^^Arguments...>;
        };

        /** @brief Alias-template closure that appends fixed reflected arguments. */
        template<std::meta::info Function, std::meta::info... Back>
        struct BindBackClosure {
            template<typename... Arguments>
            using Apply = InvokeResultType<Function, ^^Arguments..., Back...>;
        };

        /** @brief Alias-template closure for right-to-left composition. */
        template<std::meta::info... Functions>
        struct ComposeClosure {
            template<typename... Arguments>
            using Apply = InfoType<RequireTypeResult(
                Detail::ComposeInvoke(InfoList{Functions...}, InfoList{^^Arguments...}), InfoList{Functions...})>;
        };

        /** @brief Alias-template closure for pointwise transform followed by continuation. */
        template<std::meta::info Function, std::meta::info Continuation>
        struct TransformClosure {
            template<typename... Arguments>
            using Apply =
                InfoType<RequireTypeResult(Detail::TransformInvoke(Function, Continuation, InfoList{^^Arguments...}),
                                           InfoList{Function, Continuation})>;
        };

        /** @brief Alias-template closure that always yields one reflected result. */
        template<std::meta::info Result>
        struct ConstantClosure {
            template<typename...>
            using Apply = InfoType<RequireTypeResult(Result, InfoList{Result})>;
        };

        /** @brief Alias-template closure that expands one reflected TypeList argument. */
        template<std::meta::info Function>
        struct UncurryClosure {
            template<typename List>
            using Apply = InfoType<RequireTypeResult(Types::Apply(Function, ^^List), InfoList{Function, ^^List})>;
        };

    } // namespace Detail

    /**
     * @brief Select @p Then when @p Condition is true, otherwise @p Else.
     * @tparam Condition Compile-time branch condition.
     * @tparam Then Type selected for `true`.
     * @tparam Else Type selected for `false`.
     */
    template<bool Condition, typename Then, typename Else>
    using If = std::conditional_t<Condition, Then, Else>;

    /**
     * @brief Select @p Else when @p Condition is true, otherwise @p Then.
     * @tparam Condition Compile-time branch condition.
     * @tparam Then Type selected for `false`.
     * @tparam Else Type selected for `true`.
     */
    template<bool Condition, typename Then, typename Else>
    using IfNot = std::conditional_t<Condition, Else, Then>;

    /**
     * @brief Return a reflected function with leading arguments bound.
     * @tparam Function Reflected template invoked by the closure.
     * @tparam Front Reflections inserted before call-site type arguments.
     * @return Reflection of an alias-template closure preserving recoverable invocation errors.
     */
    template<std::meta::info Function, std::meta::info... Front>
    inline constexpr std::meta::info BindFront = ^^Detail::BindFrontClosure<Function, Front...>::template Apply;

    /**
     * @brief Return a reflected function with trailing arguments bound.
     * @tparam Function Reflected template invoked by the closure.
     * @tparam Back Reflections inserted after call-site type arguments.
     * @return Reflection of an alias-template closure preserving recoverable invocation errors.
     */
    template<std::meta::info Function, std::meta::info... Back>
    inline constexpr std::meta::info BindBack = ^^Detail::BindBackClosure<Function, Back...>::template Apply;

    /**
     * @brief Compose one or more reflected templates right-to-left.
     * @tparam Functions Non-empty sequence `F, G, ...` producing `F(G(...(arguments)))`.
     * @return Reflection of the composed alias-template closure.
     * @details Every intermediate result must be a type because the closure is exposed as an alias template.
     */
    template<std::meta::info... Functions>
        requires(sizeof...(Functions) > 0)
    inline constexpr std::meta::info Compose = ^^Detail::ComposeClosure<Functions...>::template Apply;

    /**
     * @brief Transform every argument and pass the results to @p Continuation.
     * @tparam Function Reflected unary template applied pointwise.
     * @tparam Continuation Reflected variadic template receiving transformed results.
     * @return Reflection of the transform-then-continue alias-template closure.
     */
    template<std::meta::info Function, std::meta::info Continuation = ^^Traits::TypeList>
    inline constexpr std::meta::info Transform = ^^Detail::TransformClosure<Function, Continuation>::template Apply;

    /**
     * @brief Return a reflected function that always yields @p Result.
     * @tparam Result Reflected type returned for every invocation; non-types become `NonTypeResultTag`.
     * @return Reflection of a constant alias-template closure.
     */
    template<std::meta::info Result>
    inline constexpr std::meta::info Constant = ^^Detail::ConstantClosure<Result>::template Apply;

    /**
     * @brief Return a reflected function that applies @p Function to one TypeList argument.
     * @tparam Function Reflected variadic template receiving the list's types.
     * @return Reflection of an alias-template closure accepting one `TypeList` type.
     */
    template<std::meta::info Function>
    inline constexpr std::meta::info Uncurry = ^^Detail::UncurryClosure<Function>::template Apply;

} // namespace Sora::Meta

namespace Sora::Traits {

    /**
     * @brief Number of types in @p List.
     * @tparam List `TypeList` specialization to inspect.
     * @return Element count after reflected sequence validation.
     * @pre @p List contains no type-level `MetaError`; unlike `Meta::Types::Length`, this value interface cannot carry
     * a reflected error and therefore issues a translation-time diagnostic when validation fails.
     */
    template<typename List>
        requires Concept::TypeList<List>
    inline constexpr std::size_t Length = Meta::RequireSizeResult(Meta::Types::Length(^^List));

    /**
     * @brief Return the @p Index-th type of @p List.
     * @tparam List `TypeList` specialization to inspect.
     * @tparam Index Zero-based type index.
     * @return Selected type or the first nested `MetaError` type.
     */
    template<typename List, std::size_t Index>
        requires(Concept::TypeList<List> && Index < std::remove_cvref_t<List>::size)
    using At = Meta::InfoType<Meta::Types::At(^^List, Index)>;

    /**
     * @brief First type of a non-empty @p List.
     * @tparam List Non-empty `TypeList` specialization.
     * @return First type or the first nested `MetaError` type.
     */
    template<typename List>
        requires(Concept::TypeList<List> && std::remove_cvref_t<List>::size > 0)
    using Front = Meta::InfoType<Meta::Types::Front(^^List)>;

    /**
     * @brief Final type of a non-empty @p List.
     * @tparam List Non-empty `TypeList` specialization.
     * @return Final type or the first nested `MetaError` type.
     */
    template<typename List>
        requires(Concept::TypeList<List> && std::remove_cvref_t<List>::size > 0)
    using Back = Meta::InfoType<Meta::Types::Back(^^List)>;

    /**
     * @brief All but the first type of @p List.
     * @tparam List `TypeList` specialization to transform.
     * @return Resulting `TypeList`; an empty input remains empty.
     */
    template<typename List>
        requires Concept::TypeList<List>
    using PopFront = Meta::InfoType<Meta::Types::PopFront(^^List)>;

    /**
     * @brief First @p Count types of @p List, clamped to its length.
     * @tparam List `TypeList` specialization to transform.
     * @tparam Count Maximum number of types to retain.
     * @return Resulting prefix `TypeList`; @p Count is clamped to the input length.
     */
    template<typename List, std::size_t Count>
        requires Concept::TypeList<List>
    using Take = Meta::InfoType<Meta::Types::Take(^^List, Count)>;

    /**
     * @brief Types after the first @p Count entries, clamped to its length.
     * @tparam List `TypeList` specialization to transform.
     * @tparam Count Number of leading types to discard.
     * @return Resulting suffix `TypeList`; @p Count is clamped to the input length.
     */
    template<typename List, std::size_t Count>
        requires Concept::TypeList<List>
    using Drop = Meta::InfoType<Meta::Types::Drop(^^List, Count)>;

    /**
     * @brief Exact bounded slice of @p List.
     * @tparam List `TypeList` specialization to transform.
     * @tparam Offset Zero-based first type.
     * @tparam Count Number of types to copy.
     * @return Resulting bounded `TypeList`; invalid bounds are rejected by the constraint.
     */
    template<typename List, std::size_t Offset, std::size_t Count>
        requires(Concept::TypeList<List> && Offset <= Length<List> && Count <= Length<List> - Offset)
    using Slice = Meta::InfoType<Meta::Types::Slice(^^List, Offset, Count)>;

    /**
     * @brief Empty type list with no dependency on @p List's elements.
     * @tparam List `TypeList` specialization whose element types are discarded.
     * @return `TypeList<>`.
     */
    template<typename List>
        requires Concept::TypeList<List>
    using Clear = Meta::InfoType<Meta::Types::Clear(^^List)>;

    /**
     * @brief TypeList containing @p Count copies of @p Type.
     * @tparam Count Number of copies.
     * @tparam Type Type to repeat.
     * @return `TypeList` with @p Count copies, or a `MetaError` type for a non-type result.
     * @details The result is a `MetaError` type only when the reflected value is not a type; invalid @p Type itself
     * is diagnosed by the type system before this alias can be formed.
     */
    template<std::size_t Count, typename Type>
    using Fill = Meta::InfoType<Meta::Types::Fill(Count, ^^Type)>;

    /**
     * @brief Concatenate type lists in source order.
     * @tparam Lists `TypeList` specializations to concatenate.
     * @return A `TypeList` type; malformed intermediate reflection becomes a `MetaError` type.
     */
    template<typename... Lists>
        requires(Concept::TypeList<Lists> && ...)
    using Concat = Meta::InfoType<Meta::Types::Concat(Meta::InfoList{^^Lists...})>;

    /**
     * @brief Stable union of `TypeList` operands, retaining first occurrences.
     * @tparam Lists `TypeList` specializations to unite.
     * @return Resulting `TypeList` or a `MetaError` type for invalid input.
     * @par Example
     * @code{.cpp}
     * using Result = Sora::Traits::SetUnion<Sora::Traits::TypeList<int, double>,
     *                                      Sora::Traits::TypeList<double, char>>;
     * static_assert(std::same_as<Result, Sora::Traits::TypeList<int, double, char>>);
     * @endcode
     */
    template<typename... Lists>
        requires(Concept::TypeList<Lists> && ...)
    using SetUnion = Meta::InfoType<Meta::Types::SetUnion(Meta::InfoList{^^Lists...})>;

    /**
     * @brief Apply reflected unary template @p Function to every type in @p List.
     * @tparam Function Reflected unary template.
     * @tparam List `TypeList` specialization to transform.
     * @return Resulting `TypeList` or a `MetaError` type when substitution fails or yields a non-type.
     */
    template<std::meta::info Function, typename List>
        requires Concept::TypeList<List>
    using Map = Meta::InfoType<Meta::Types::Map(Function, ^^List)>;

    /**
     * @brief Flatten a unary type mapper over @p List.
     * @tparam Function Reflected unary template returning a `TypeList`.
     * @tparam List `TypeList` specialization to transform.
     * @return Concatenated `TypeList` or a `MetaError` type when mapping fails.
     * @par Example
     * @code{.cpp}
     * template<typename T>
     * using Expand = Sora::Traits::TypeList<T, const T>;
     * using Result = Sora::Traits::FlatMap<^^Expand, Sora::Traits::TypeList<int>>;
     * @endcode
     */
    template<std::meta::info Function, typename List>
        requires Concept::TypeList<List>
    using FlatMap = Meta::InfoType<Meta::Types::FlatMap(Function, ^^List)>;

    /**
     * @brief Retain types in @p List accepted by unary @p Pred.
     * @tparam Pred Reflected unary meta predicate.
     * @tparam List `TypeList` specialization to filter.
     * @return Resulting `TypeList` or a `MetaError` type. Predicate protocol violations are represented in that type.
     */
    template<std::meta::info Pred, typename List>
        requires Concept::TypeList<List>
    using Filter = Meta::InfoType<Meta::Types::Filter(Pred, ^^List)>;

    /**
     * @brief Replace every type in @p List accepted by @p Pred.
     * @tparam Pred Reflected unary meta predicate.
     * @tparam Replacement Replacement type.
     * @tparam List `TypeList` specialization to transform.
     * @return Resulting `TypeList` or a `MetaError` type.
     * @par Example
     * @code{.cpp}
     * consteval bool Integral(std::meta::info type) noexcept { return std::meta::is_integral_type(type); }
     * using Result = Sora::Traits::ReplaceIf<^^Integral, bool,
     *                                       Sora::Traits::TypeList<int, double>>;
     * @endcode
     */
    template<std::meta::info Pred, typename Replacement, typename List>
        requires Concept::TypeList<List>
    using ReplaceIf = Meta::InfoType<Meta::Types::ReplaceIf(Pred, ^^Replacement, ^^List)>;

    /**
     * @brief Remove every type in @p List accepted by @p Pred.
     * @tparam Pred Reflected unary meta predicate.
     * @tparam List `TypeList` specialization to transform.
     * @return Resulting `TypeList` or a `MetaError` type.
     * @par Example
     * @code{.cpp}
     * consteval bool Integral(std::meta::info type) noexcept { return std::meta::is_integral_type(type); }
     * using Result = Sora::Traits::RemoveIf<^^Integral,
     *                                      Sora::Traits::TypeList<int, double>>;
     * @endcode
     */
    template<std::meta::info Pred, typename List>
        requires Concept::TypeList<List>
    using RemoveIf = Meta::InfoType<Meta::Types::RemoveIf(Pred, ^^List)>;

    /**
     * @brief Invoke reflected @p Function with every type in @p List.
     * @tparam Function Reflected variadic template.
     * @tparam List `TypeList` specialization supplying invocation arguments.
     * @return The substituted type or a `MetaError` type; no exception is used for recoverable meta failure.
     */
    template<std::meta::info Function, typename List>
        requires Concept::TypeList<List>
    using Apply = Meta::InfoType<Meta::Types::Apply(Function, ^^List)>;

    /**
     * @brief Left fold reflected @p Function over @p List.
     * @tparam Function Reflected binary template.
     * @tparam Initial Initial accumulator type.
     * @tparam List `TypeList` specialization to fold.
     * @return Final type or a `MetaError` type from the first failed invocation.
     */
    template<std::meta::info Function, typename Initial, typename List>
        requires Concept::TypeList<List>
    using FoldLeft = Meta::InfoType<Meta::Types::FoldLeft(Function, ^^Initial, ^^List)>;

    /**
     * @brief Right fold reflected @p Function over @p List.
     * @tparam Function Reflected binary template.
     * @tparam Initial Initial accumulator type.
     * @tparam List `TypeList` specialization to fold.
     * @return Final type or a `MetaError` type from the first failed invocation.
     */
    template<std::meta::info Function, typename Initial, typename List>
        requires Concept::TypeList<List>
    using FoldRight = Meta::InfoType<Meta::Types::FoldRight(Function, ^^Initial, ^^List)>;

    /**
     * @brief First index of @p Type in @p List, or @ref Meta::InfoList::npos.
     * @tparam List `TypeList` specialization to inspect.
     * @tparam Type Reflected type to find.
     * @return Zero-based first matching index, or @ref Meta::InfoList::npos.
     */
    template<typename List, typename Type>
        requires Concept::TypeList<List>
    inline constexpr std::size_t IndexOf = Meta::RequireSizeResult(Meta::Types::IndexOf(^^List, ^^Type));

    /**
     * @brief Whether @p List contains @p Type.
     * @tparam List `TypeList` specialization to inspect.
     * @tparam Type Reflected type to find.
     * @return `true` when @p Type occurs in @p List.
     */
    template<typename List, typename Type>
        requires Concept::TypeList<List>
    inline constexpr bool Contains = Meta::RequirePredResult(Meta::Types::Contains(^^List, ^^Type));

    /**
     * @brief Number of occurrences of @p Type in @p List.
     * @tparam List `TypeList` specialization to inspect.
     * @tparam Type Reflected type to count.
     * @return Number of occurrences; zero for an empty list.
     */
    template<typename List, typename Type>
        requires Concept::TypeList<List>
    inline constexpr std::size_t Count = Meta::RequireSizeResult(Meta::Types::Count(^^List, ^^Type));

    /**
     * @brief Compare two `TypeList` sequences element by element.
     * @tparam Left First `TypeList` specialization.
     * @tparam Right Second `TypeList` specialization.
     * @return `true` when both lists have equal length and equal type reflections in the same order.
     * @par Example
     * @code{.cpp}
     * static_assert(Sora::Traits::Equal<Sora::Traits::TypeList<int, double>,
     *                                  Sora::Traits::TypeList<int, double>>);
     * @endcode
     */
    template<typename Left, typename Right>
        requires(Concept::TypeList<Left> && Concept::TypeList<Right>)
    inline constexpr bool Equal = Meta::RequirePredResult(Meta::Types::Equal(^^Left, ^^Right));

    /**
     * @brief First type in @p List accepted by @p Pred, or `void` when no type matches.
     * @tparam Pred Reflected unary meta predicate.
     * @tparam List `TypeList` specialization to inspect.
     * @tparam Fallback Type returned when no element matches.
     * @return A matching type, @p Fallback, or a `MetaError` type for predicate/input failure.
     * @par Example
     * @code{.cpp}
     * consteval bool IsVoid(std::meta::info info) { return info == ^^void; }
     * using Match = Sora::Traits::FindFirst<^^IsVoid, Sora::Traits::TypeList<int, void>>;
     * static_assert(std::same_as<Match, void>);
     * @endcode
     */
    template<std::meta::info Pred, typename List, typename Fallback = void>
        requires Concept::TypeList<List>
    using FindFirst = Meta::InfoType<Meta::Types::FindFirst(Pred, ^^List, ^^Fallback)>;

    /**
     * @brief Final type in @p List accepted by @p Pred, or `void` when no type matches.
     * @tparam Pred Reflected unary meta predicate.
     * @tparam List `TypeList` specialization to inspect.
     * @tparam Fallback Type returned when no element matches.
     * @return A matching type, @p Fallback, or a `MetaError` type for predicate/input failure.
     * @par Example
     * @code{.cpp}
     * consteval bool IsVoid(std::meta::info info) { return info == ^^void; }
     * using Match = Sora::Traits::FindLast<^^IsVoid, Sora::Traits::TypeList<int, void>>;
     * static_assert(std::same_as<Match, void>);
     * @endcode
     */
    template<std::meta::info Pred, typename List, typename Fallback = void>
        requires Concept::TypeList<List>
    using FindLast = Meta::InfoType<Meta::Types::FindLast(Pred, ^^List, ^^Fallback)>;

    /**
     * @brief Prepend @p Type to @p List.
     * @tparam List `TypeList` specialization to transform.
     * @tparam Type Type to insert.
     * @return Resulting `TypeList` or a `MetaError` type if projection fails.
     */
    template<typename List, typename Type>
        requires Concept::TypeList<List>
    using PushFront = Meta::InfoType<Meta::Types::PushFront(^^List, ^^Type)>;

    /**
     * @brief Append @p Type to @p List.
     * @tparam List `TypeList` specialization to transform.
     * @tparam Type Type to insert.
     * @return Resulting `TypeList` or a `MetaError` type if projection fails.
     */
    template<typename List, typename Type>
        requires Concept::TypeList<List>
    using PushBack = Meta::InfoType<Meta::Types::PushBack(^^List, ^^Type)>;

    /**
     * @brief All but the final type of @p List.
     * @tparam List `TypeList` specialization to transform.
     * @return Resulting `TypeList`; an empty input remains empty.
     */
    template<typename List>
        requires Concept::TypeList<List>
    using PopBack = Meta::InfoType<Meta::Types::PopBack(^^List)>;

    /**
     * @brief Types of @p List in reverse order.
     * @tparam List `TypeList` specialization to transform.
     * @return Resulting `TypeList` or a `MetaError` type if the projection fails.
     */
    template<typename List>
        requires Concept::TypeList<List>
    using Reverse = Meta::InfoType<Meta::Types::Reverse(^^List)>;

    /**
     * @brief Remove duplicate types while retaining first occurrences.
     * @tparam List `TypeList` specialization to transform.
     * @return Stable unique `TypeList` or a `MetaError` type if the projection fails.
     */
    template<typename List>
        requires Concept::TypeList<List>
    using Unique = Meta::InfoType<Meta::Types::Unique(^^List)>;

    /**
     * @brief Whether every type in @p List occurs exactly once.
     * @tparam List `TypeList` specialization to inspect.
     * @return `true` for an empty or duplicate-free list.
     */
    template<typename List>
        requires Concept::TypeList<List>
    inline constexpr bool IsUnique = Meta::RequirePredResult(Meta::Types::IsUnique(^^List));

    /**
     * @brief Replace every @p Old type in @p List with @p New.
     * @tparam List `TypeList` specialization to transform.
     * @tparam Old Type to replace.
     * @tparam New Replacement type.
     * @return Resulting `TypeList` or a `MetaError` type for invalid reflected type arguments.
     */
    template<typename List, typename Old, typename New>
        requires Concept::TypeList<List>
    using Replace = Meta::InfoType<Meta::Types::Replace(^^List, ^^Old, ^^New)>;

    /**
     * @brief Remove every occurrence of @p Needle from @p List.
     * @tparam List `TypeList` specialization to transform.
     * @tparam Needle Type to remove.
     * @return Resulting `TypeList` or a `MetaError` type for invalid reflected type arguments.
     */
    template<typename List, typename Needle>
        requires Concept::TypeList<List>
    using Remove = Meta::InfoType<Meta::Types::Remove(^^List, ^^Needle)>;

    /**
     * @brief Zip @p Left and @p Right through reflected binary @p Function.
     * @tparam Function Reflected binary template.
     * @tparam Left First `TypeList` specialization.
     * @tparam Right Second `TypeList` specialization.
     * @return Resulting `TypeList`; unequal lengths and failed substitutions become `MetaError` types.
     */
    template<std::meta::info Function, typename Left, typename Right>
        requires(Concept::TypeList<Left> && Concept::TypeList<Right>)
    using Zip = Meta::InfoType<Meta::Types::Zip(Function, ^^Left, ^^Right)>;

    /**
     * @brief Stable intersection of two `TypeList` operands.
     * @tparam Left First `TypeList`; its order is preserved.
     * @tparam Right Second `TypeList` used for membership.
     * @return Resulting intersection `TypeList` or a `MetaError` type.
     * @par Example
     * @code{.cpp}
     * using Result = Sora::Traits::SetIntersection<Sora::Traits::TypeList<int, double>,
     *                                             Sora::Traits::TypeList<double, char>>;
     * @endcode
     */
    template<typename Left, typename Right>
        requires(Concept::TypeList<Left> && Concept::TypeList<Right>)
    using SetIntersection = Meta::InfoType<Meta::Types::SetIntersection(^^Left, ^^Right)>;

    /**
     * @brief Stable difference of two `TypeList` operands.
     * @tparam Left First `TypeList`; its order is preserved.
     * @tparam Right Types removed from @p Left.
     * @return Resulting difference `TypeList` or a `MetaError` type.
     * @par Example
     * @code{.cpp}
     * using Result = Sora::Traits::SetDifference<Sora::Traits::TypeList<int, double>,
     *                                           Sora::Traits::TypeList<double, char>>;
     * @endcode
     */
    template<typename Left, typename Right>
        requires(Concept::TypeList<Left> && Concept::TypeList<Right>)
    using SetDifference = Meta::InfoType<Meta::Types::SetDifference(^^Left, ^^Right)>;

    /**
     * @brief Whether every type in @p List satisfies @p Pred.
     * @tparam Pred Reflected unary meta predicate.
     * @tparam List `TypeList` specialization to inspect.
     * @return `true` for an empty list or when every type is accepted.
     */
    template<std::meta::info Pred, typename List>
        requires Concept::TypeList<List>
    inline constexpr bool AllOf = Meta::RequirePredResult(Meta::Types::AllOf(Pred, ^^List));

    /**
     * @brief Whether any type in @p List satisfies @p Pred.
     * @tparam Pred Reflected unary meta predicate.
     * @tparam List `TypeList` specialization to inspect.
     * @return `true` when at least one type is accepted; `false` for an empty list.
     */
    template<std::meta::info Pred, typename List>
        requires Concept::TypeList<List>
    inline constexpr bool AnyOf = Meta::RequirePredResult(Meta::Types::AnyOf(Pred, ^^List));

    /**
     * @brief Whether no type in @p List satisfies @p Pred.
     * @tparam Pred Reflected unary meta predicate.
     * @tparam List `TypeList` specialization to inspect.
     * @return `true` when no type is accepted; `true` for an empty list.
     */
    template<std::meta::info Pred, typename List>
        requires Concept::TypeList<List>
    inline constexpr bool NoneOf = Meta::RequirePredResult(Meta::Types::NoneOf(Pred, ^^List));

    /**
     * @brief Number of types in @p List satisfying @p Pred.
     * @tparam Pred Reflected unary meta predicate.
     * @tparam List `TypeList` specialization to inspect.
     * @return Number of accepted types; zero for an empty list.
     */
    template<std::meta::info Pred, typename List>
        requires Concept::TypeList<List>
    inline constexpr std::size_t CountIf = Meta::RequireSizeResult(Meta::Types::CountIf(Pred, ^^List));

    /**
     * @brief Project non-static data-member types of @p Type into a `TypeList`.
     * @tparam Type Aggregate/class type whose non-static data members are projected.
     * @return Member-type pack represented as a `TypeList` C++ type.
     * @details This is a reflection projection, not the general `Meta::ToTypeList` conversion. The latter converts an
     * arbitrary `InfoList` of reflected types; this alias first enumerates the members of @p Type and then projects
     * their declared types.
     * @par Example
     * @code{.cpp}
     * struct Vertex { float x; float y; };
     * using Members = Sora::Traits::DataMemberTypes<Vertex>;
     * static_assert(std::same_as<Members, Sora::Traits::TypeList<float, float>>);
     * @endcode
     */
    template<typename Type>
    using DataMemberTypes = Meta::InfoType<Meta::DataMemberTypes(^^Type)>;

} // namespace Sora::Traits

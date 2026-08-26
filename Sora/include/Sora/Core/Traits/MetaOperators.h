/**
 * @file MetaOperators.h
 * @brief Reflection-native higher-order functions and sequence algebra.
 * @ingroup Core
 *
 * @details Collection algorithms operate on `Meta::InfoList`. A successful collection result is a reflected
 * `InfoList` value; failure is a reflected `MetaError` type. `ToTypeList` is the sole collection-to-type-pack
 * boundary. This keeps reflection traversal, error propagation, and C++ type formation as separate layers.
 *
 * Every algorithm obeys the first-error law: the first `MetaError` in function, explicit arguments, then sequence
 * order is returned unchanged. Callables are validated even for empty sequences, so emptiness cannot hide an invalid
 * template or predicate. `Meta::Map` and its peers form the `InfoList` algorithm kernel. `Meta::Types` contains the
 * reflected `Traits::TypeList` adapters, keeping braced `InfoList` calls unambiguous while preserving short names.
 *
 * `Map`, `Zip`, and folds validate template category before traversing an empty input. C++26 reflection does not
 * expose a template's parameter list, so parameter kind, arity, and constraints are validated by the first actual
 * substitution; an empty input has no such substitution. Predicate parameter reflection is available and predicates
 * are therefore fully validated even for empty input. `Map` and `Zip` dealias produced entities; `Filter` preserves
 * the accepted input reflections exactly.
 *
 * @par InfoList pipeline
 * @code{.cpp}
 * #include <concepts>
 * #include <meta>
 * #include <type_traits>
 * #include <variant>
 *
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
 * #include <concepts>
 * #include <meta>
 * #include <type_traits>
 * #include <variant>
 *
 * using Input = Sora::Traits::TypeList<int, double, const double>;
 * using Result = Sora::Traits::Apply<^^std::variant,
 *     Sora::Traits::Unique<Sora::Traits::Map<^^std::decay_t, Input>>>;
 *
 * static_assert(std::same_as<Result, std::variant<int, double>>);
 * @endcode
 *
 * @par Higher-order composition and errors
 * @code{.cpp}
 * #include <concepts>
 * #include <meta>
 * #include <type_traits>
 *
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
     * @return `true` only for an object whose unqualified type is `InfoList`.
     */
    [[nodiscard]] consteval bool IsInfoList(std::meta::info result) noexcept {
        if (!std::meta::is_object(result)) {
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

    /**
     * @brief Invoke a reflected template with a dynamically assembled argument list.
     * @param[in] function Reflected template to substitute.
     * @param[in] arguments Reflected template arguments in source order.
     * @return Substitution result, the first input error, or `NonInvocableTag` with invocation provenance.
     */
    [[nodiscard]] consteval std::meta::info Apply(std::meta::info function, const InfoList& arguments) {
        if (IsMetaError(function)) {
            return function;
        }
        if (const auto error = FirstMetaError(arguments); error != std::meta::info{}) {
            return error;
        }
        if (!std::meta::is_template(function) || !std::meta::can_substitute(function, arguments)) {
            std::vector<std::meta::info> context{function};
            context.append_range(arguments);
            return MakeMetaError(^^Error::NonInvocableTag, context);
        }
        return std::meta::substitute(function, arguments);
    }

    namespace Detail {

        [[nodiscard]] consteval std::meta::info ValidateTemplate(std::meta::info function) {
            if (IsMetaError(function)) {
                return function;
            }
            return std::meta::is_template(function) ? std::meta::info{}
                                                     : MakeMetaError(^^Error::NonInvocableTag, {function});
        }

        [[nodiscard]] consteval std::meta::info ValidateTemplate(std::meta::info function,
                                                                  std::size_t arity) {
            if (const auto error = ValidateTemplate(function); error != std::meta::info{}) {
                return error;
            }
            std::vector<std::meta::info> arguments(arity, ^^void);
            return std::meta::can_substitute(function, arguments)
                       ? std::meta::info{}
                       : MakeMetaError(^^Error::NonInvocableTag, {function});
        }

        [[nodiscard]] consteval std::meta::info ValidateUnaryPred(std::meta::info pred) {
            if (IsMetaError(pred)) {
                return pred;
            }
            if (!std::meta::is_function(pred) || std::meta::parameters_of(pred).size() != 1 ||
                !std::meta::is_invocable_type(std::meta::type_of(pred), {^^std::meta::info})) {
                return MakeMetaError(^^Error::NonInvocableTag, {pred});
            }
            return std::meta::return_type_of(pred) == ^^bool
                       ? std::meta::info{}
                       : MakeMetaError(^^Error::NonBooleanPredResultTag, {pred});
        }

        [[nodiscard]] consteval std::meta::info RequireTypeInfo(std::meta::info result,
                                                                const InfoList& context) {
            if (IsMetaError(result) || std::meta::is_type(result)) {
                return result;
            }
            std::vector<std::meta::info> values(context.begin(), context.end());
            values.push_back(result);
            return MakeMetaError(^^Error::NonTypeResultTag, values);
        }

        [[nodiscard]] consteval std::meta::info ComposeInvoke(const InfoList& functions,
                                                              const InfoList& arguments) {
            if (functions.empty()) {
                return MakeMetaError(^^Error::NonInvocableTag);
            }
            if (const auto error = FirstMetaError(functions); error != std::meta::info{}) {
                return error;
            }
            if (const auto error = FirstMetaError(arguments); error != std::meta::info{}) {
                return error;
            }
            for (const auto function : functions) {
                if (const auto error = ValidateTemplate(function); error != std::meta::info{}) {
                    return error;
                }
            }
            auto result = Apply(functions.back(), arguments);
            for (std::size_t index = functions.size() - 1; index > 0 && !IsMetaError(result); --index) {
                result = Apply(functions[index - 1], InfoList{result});
            }
            return result;
        }

        [[nodiscard]] consteval std::meta::info TransformInvoke(std::meta::info function,
                                                                std::meta::info continuation,
                                                                const InfoList& arguments) {
            if (IsMetaError(function) || IsMetaError(continuation)) {
                return IsMetaError(function) ? function : continuation;
            }
            if (const auto error = FirstMetaError(arguments); error != std::meta::info{}) {
                return error;
            }
            if (const auto error = ValidateTemplate(function); error != std::meta::info{}) {
                return error;
            }
            if (const auto error = ValidateTemplate(continuation); error != std::meta::info{}) {
                return error;
            }
            std::vector<std::meta::info> transformed;
            transformed.reserve(arguments.size());
            for (const auto argument : arguments) {
                const auto result = Apply(function, InfoList{argument});
                if (IsMetaError(result)) {
                    return result;
                }
                transformed.push_back(result);
            }
            return Apply(continuation, Promote(transformed));
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
        if (const auto error = FirstMetaError(input); error != std::meta::info{}) {
            return error;
        }
        if (const auto error = Detail::ValidateTemplate(function, 1); error != std::meta::info{}) {
            return error;
        }
        std::vector<std::meta::info> output;
        output.reserve(input.size());
        for (const auto value : input) {
            const std::array arguments{value};
            const auto mapped = Apply(function, Promote(arguments));
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
        if (const auto error = FirstMetaError(input); error != std::meta::info{}) {
            return error;
        }
        if (const auto error = Detail::ValidateUnaryPred(pred); error != std::meta::info{}) {
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
     * @brief Zip equal-length sequences through a reflected binary template.
     * @param[in] function Reflected template accepting one entity from each input.
     * @param[in] left First source sequence.
     * @param[in] right Second source sequence.
     * @return Reflected `InfoList`, `LengthMismatchTag`, an invocation error, or the first input error.
     * @par Complexity Linear in the common input length.
     */
    [[nodiscard]] consteval std::meta::info Zip(std::meta::info function, const InfoList& left,
                                                const InfoList& right) {
        if (IsMetaError(function)) {
            return function;
        }
        if (const auto error = FirstMetaError(left); error != std::meta::info{}) {
            return error;
        }
        if (const auto error = FirstMetaError(right); error != std::meta::info{}) {
            return error;
        }
        if (const auto error = Detail::ValidateTemplate(function, 2); error != std::meta::info{}) {
            return error;
        }
        if (left.size() != right.size()) {
            return MakeMetaError(^^Error::LengthMismatchTag, {function});
        }
        std::vector<std::meta::info> output;
        output.reserve(left.size());
        for (std::size_t index = 0; index < left.size(); ++index) {
            const std::array arguments{left[index], right[index]};
            const auto mapped = Apply(function, Promote(arguments));
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
        if (IsMetaError(function) || IsMetaError(initial)) {
            return IsMetaError(function) ? function : initial;
        }
        if (const auto error = FirstMetaError(input); error != std::meta::info{}) {
            return error;
        }
        if (const auto error = Detail::ValidateTemplate(function, 2); error != std::meta::info{}) {
            return error;
        }
        auto accumulator = initial;
        for (const auto value : input) {
            const std::array arguments{accumulator, value};
            accumulator = Apply(function, Promote(arguments));
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
        if (IsMetaError(function) || IsMetaError(initial)) {
            return IsMetaError(function) ? function : initial;
        }
        if (const auto error = FirstMetaError(input); error != std::meta::info{}) {
            return error;
        }
        if (const auto error = Detail::ValidateTemplate(function, 2); error != std::meta::info{}) {
            return error;
        }
        auto accumulator = initial;
        for (std::size_t index = input.size(); index > 0; --index) {
            const std::array arguments{input[index - 1], accumulator};
            accumulator = Apply(function, Promote(arguments));
            if (IsMetaError(accumulator)) {
                return accumulator;
            }
        }
        return accumulator;
    }

    /**
     * @brief Return the first entity accepted by @p pred, or @p fallback.
     * @param[in] input Source sequence inspected from front to back.
     * @param[in] pred Reflected unary meta predicate.
     * @param[in] fallback Entity returned when no element is accepted.
     * @return Matching entity, @p fallback, or the first predicate/input error.
     */
    [[nodiscard]] consteval std::meta::info FindFirst(const InfoList& input, std::meta::info pred,
                                                      std::meta::info fallback = ^^void) {
        if (IsMetaError(pred) || IsMetaError(fallback)) {
            return IsMetaError(pred) ? pred : fallback;
        }
        if (const auto error = FirstMetaError(input); error != std::meta::info{}) {
            return error;
        }
        if (const auto error = Detail::ValidateUnaryPred(pred); error != std::meta::info{}) {
            return error;
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
     * @param[in] input Source sequence inspected in source order.
     * @param[in] pred Reflected unary meta predicate.
     * @param[in] fallback Entity returned when no element is accepted.
     * @return Last matching entity, @p fallback, or the first predicate/input error.
     */
    [[nodiscard]] consteval std::meta::info FindLast(const InfoList& input, std::meta::info pred,
                                                     std::meta::info fallback = ^^void) {
        if (IsMetaError(pred) || IsMetaError(fallback)) {
            return IsMetaError(pred) ? pred : fallback;
        }
        if (const auto error = FirstMetaError(input); error != std::meta::info{}) {
            return error;
        }
        if (const auto error = Detail::ValidateUnaryPred(pred); error != std::meta::info{}) {
            return error;
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
     * @param[in] input Source sequence.
     * @param[in] pred Reflected unary meta predicate.
     * @return Reflected bool, or the first predicate/input error. Empty input yields `true` after validating @p pred.
     */
    [[nodiscard]] consteval std::meta::info AllOf(const InfoList& input, std::meta::info pred) {
        if (IsMetaError(pred)) {
            return pred;
        }
        if (const auto error = FirstMetaError(input); error != std::meta::info{}) {
            return error;
        }
        if (const auto error = Detail::ValidateUnaryPred(pred); error != std::meta::info{}) {
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
     * @param[in] input Source sequence.
     * @param[in] pred Reflected unary meta predicate.
     * @return Reflected bool, or the first predicate/input error. Empty input yields `false` after validating @p pred.
     */
    [[nodiscard]] consteval std::meta::info AnyOf(const InfoList& input, std::meta::info pred) {
        if (IsMetaError(pred)) {
            return pred;
        }
        if (const auto error = FirstMetaError(input); error != std::meta::info{}) {
            return error;
        }
        if (const auto error = Detail::ValidateUnaryPred(pred); error != std::meta::info{}) {
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
     * @param[in] input Source sequence.
     * @param[in] pred Reflected unary meta predicate.
     * @return Negated result of @ref AnyOf, preserving errors unchanged.
     */
    [[nodiscard]] consteval std::meta::info NoneOf(const InfoList& input, std::meta::info pred) {
        const auto result = AnyOf(input, pred);
        return IsMetaError(result) ? result : std::meta::reflect_constant(!std::meta::extract<bool>(result));
    }

    /**
     * @brief Count entities satisfying @p pred.
     * @param[in] input Source sequence.
     * @param[in] pred Reflected unary meta predicate.
     * @return Reflected `std::size_t`, or the first predicate/input error.
     */
    [[nodiscard]] consteval std::meta::info CountIf(const InfoList& input, std::meta::info pred) {
        if (IsMetaError(pred)) {
            return pred;
        }
        if (const auto error = FirstMetaError(input); error != std::meta::info{}) {
            return error;
        }
        if (const auto error = Detail::ValidateUnaryPred(pred); error != std::meta::info{}) {
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

    namespace Types {

        /**
         * @brief Concatenate reflected TypeLists in source order.
         * @param[in] lists Sequence whose elements must each reflect a `Traits::TypeList` specialization.
         * @return Reflected concatenated `TypeList`, `InvalidSequenceTag`, or the first input error.
         */
        [[nodiscard]] consteval std::meta::info Concat(const InfoList& lists) {
            if (const auto error = FirstMetaError(lists); error != std::meta::info{}) {
                return error;
            }
            std::vector<std::meta::info> output;
            for (const auto list : lists) {
                if (!IsTypeList(list)) {
                    return MakeMetaError(^^Error::InvalidSequenceTag, {list});
                }
                output.append_range(ToInfoList(list));
            }
            return ToTypeList(output);
        }

        /**
         * @brief Apply a reflected template to all types in a reflected TypeList.
         * @param[in] function Reflected variadic template.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Substitution result, `InvalidSequenceTag`, an invocation error, or the first input error.
         */
        [[nodiscard]] consteval std::meta::info Apply(std::meta::info function, std::meta::info list) {
            if (IsMetaError(function) || IsMetaError(list)) {
                return IsMetaError(function) ? function : list;
            }
            if (!IsTypeList(list)) {
                return MakeMetaError(^^Error::InvalidSequenceTag, {function, list});
            }
            return Sora::Meta::Apply(function, ToInfoList(list));
        }

        /**
         * @brief Map a reflected unary template over a reflected TypeList.
         * @param[in] function Reflected unary template.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Reflected `TypeList`; non-type mapped results produce `NonTypeResultTag`.
         */
        [[nodiscard]] consteval std::meta::info Map(std::meta::info function, std::meta::info list) {
            if (IsMetaError(function) || IsMetaError(list)) {
                return IsMetaError(function) ? function : list;
            }
            if (!IsTypeList(list)) {
                return MakeMetaError(^^Error::InvalidSequenceTag, {function, list});
            }
            return ToTypeList(Sora::Meta::Map(function, ToInfoList(list)));
        }

        /**
         * @brief Filter a reflected TypeList with a reflected unary predicate.
         * @param[in] pred Reflected unary meta predicate.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Reflected filtered `TypeList`, predicate error, sequence error, or the first input error.
         */
        [[nodiscard]] consteval std::meta::info Filter(std::meta::info pred, std::meta::info list) {
            if (IsMetaError(pred) || IsMetaError(list)) {
                return IsMetaError(pred) ? pred : list;
            }
            if (!IsTypeList(list)) {
                return MakeMetaError(^^Error::InvalidSequenceTag, {pred, list});
            }
            return ToTypeList(Sora::Meta::Filter(pred, ToInfoList(list)));
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
            if (IsMetaError(function) || IsMetaError(left) || IsMetaError(right)) {
                return IsMetaError(function) ? function : (IsMetaError(left) ? left : right);
            }
            if (!IsTypeList(left) || !IsTypeList(right)) {
                return MakeMetaError(^^Error::InvalidSequenceTag, {function, left, right});
            }
            return ToTypeList(Sora::Meta::Zip(function, ToInfoList(left), ToInfoList(right)));
        }

        /**
         * @brief Replace every occurrence of @p oldType in a reflected TypeList.
         * @param[in] oldType Reflected type to replace.
         * @param[in] newType Reflected replacement type.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Reflected result `TypeList`, validation error, or the first input error.
         */
        [[nodiscard]] consteval std::meta::info Replace(std::meta::info oldType, std::meta::info newType,
                                                        std::meta::info list) {
            if (IsMetaError(oldType) || IsMetaError(newType) || IsMetaError(list)) {
                return IsMetaError(oldType) ? oldType : (IsMetaError(newType) ? newType : list);
            }
            if (!IsTypeList(list)) {
                return MakeMetaError(^^Error::InvalidSequenceTag, {oldType, newType, list});
            }
            if (!std::meta::is_type(oldType) || !std::meta::is_type(newType)) {
                return MakeMetaError(^^Error::NonTypeResultTag, {oldType, newType});
            }
            return ToTypeList(Sora::Meta::Replace(ToInfoList(list), oldType, newType));
        }

        /**
         * @brief Remove every occurrence of @p needle from a reflected TypeList.
         * @param[in] needle Reflected type to remove.
         * @param[in] list Reflected `Traits::TypeList` specialization.
         * @return Reflected result `TypeList`, validation error, or the first input error.
         */
        [[nodiscard]] consteval std::meta::info Remove(std::meta::info needle, std::meta::info list) {
            if (IsMetaError(needle) || IsMetaError(list)) {
                return IsMetaError(needle) ? needle : list;
            }
            if (!IsTypeList(list)) {
                return MakeMetaError(^^Error::InvalidSequenceTag, {needle, list});
            }
            if (!std::meta::is_type(needle)) {
                return MakeMetaError(^^Error::NonTypeResultTag, {needle});
            }
            return ToTypeList(Sora::Meta::Remove(ToInfoList(list), needle));
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

    } // namespace Types

    namespace Detail {

        template<std::meta::info Function, std::meta::info... Front>
        struct BindFrontClosure {
            template<typename... Arguments>
            using Apply = InvokeResultType<Function, Front..., ^^Arguments...>;
        };

        template<std::meta::info Function, std::meta::info... Back>
        struct BindBackClosure {
            template<typename... Arguments>
            using Apply = InvokeResultType<Function, ^^Arguments..., Back...>;
        };

        template<std::meta::info... Functions>
        struct ComposeClosure {
            template<typename... Arguments>
            using Apply = InfoType<Detail::RequireTypeInfo(
                Detail::ComposeInvoke(InfoList{Functions...}, InfoList{^^Arguments...}), InfoList{Functions...})>;
        };

        template<std::meta::info Function, std::meta::info Continuation>
        struct TransformClosure {
            template<typename... Arguments>
            using Apply = InfoType<Detail::RequireTypeInfo(
                Detail::TransformInvoke(Function, Continuation, InfoList{^^Arguments...}),
                InfoList{Function, Continuation})>;
        };

        template<std::meta::info Result>
        struct ConstantClosure {
            template<typename...>
            using Apply = InfoType<Detail::RequireTypeInfo(Result, InfoList{Result})>;
        };

        template<std::meta::info Function>
        struct UncurryClosure {
            template<typename List>
            using Apply = InfoType<Detail::RequireTypeInfo(
                Types::Apply(Function, ^^List), InfoList{Function, ^^List})>;
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
     */
    template<std::meta::info Function, std::meta::info... Front>
    inline constexpr std::meta::info BindFront = ^^Detail::BindFrontClosure<Function, Front...>::template Apply;

    /**
     * @brief Return a reflected function with trailing arguments bound.
     * @tparam Function Reflected template invoked by the closure.
     * @tparam Back Reflections inserted after call-site type arguments.
     */
    template<std::meta::info Function, std::meta::info... Back>
    inline constexpr std::meta::info BindBack = ^^Detail::BindBackClosure<Function, Back...>::template Apply;

    /**
     * @brief Compose one or more reflected templates right-to-left.
     * @tparam Functions Non-empty sequence `F, G, ...` producing `F(G(...(arguments)))`.
     * @details Every intermediate result must be a type because the closure is exposed as an alias template.
     */
    template<std::meta::info... Functions>
        requires(sizeof...(Functions) > 0)
    inline constexpr std::meta::info Compose = ^^Detail::ComposeClosure<Functions...>::template Apply;

    /**
     * @brief Transform every argument and pass the results to @p Continuation.
     * @tparam Function Reflected unary template applied pointwise.
     * @tparam Continuation Reflected variadic template receiving transformed results.
     */
    template<std::meta::info Function, std::meta::info Continuation = ^^Traits::TypeList>
    inline constexpr std::meta::info Transform = ^^Detail::TransformClosure<Function, Continuation>::template Apply;

    /**
     * @brief Return a reflected function that always yields @p Result.
     * @tparam Result Reflected type returned for every invocation; non-types become `NonTypeResultTag`.
     */
    template<std::meta::info Result>
    inline constexpr std::meta::info Constant = ^^Detail::ConstantClosure<Result>::template Apply;

    /**
     * @brief Return a reflected function that applies @p Function to one TypeList argument.
     * @tparam Function Reflected variadic template receiving the list's types.
     */
    template<std::meta::info Function>
    inline constexpr std::meta::info Uncurry = ^^Detail::UncurryClosure<Function>::template Apply;

    /**
     * @brief Number of values in a reflection pack.
     * @tparam Values Reflected pack elements.
     * @return Number of pack elements.
     */
    template<std::meta::info... Values>
    inline constexpr std::size_t Size = sizeof...(Values);

    /**
     * @brief Whether @p Needle occurs in a reflection pack.
     * @tparam Needle Reflected entity to find.
     * @tparam Values Reflected pack elements.
     * @return `true` when @p Needle occurs at least once.
     */
    template<std::meta::info Needle, std::meta::info... Values>
    [[nodiscard]] consteval bool Contains() noexcept {
        return Contains(InfoList{Values...}, Needle);
    }

    /**
     * @brief Number of occurrences of @p Needle in a reflection pack.
     * @tparam Needle Reflected entity to count.
     * @tparam Values Reflected pack elements.
     * @return Number of equal entities.
     */
    template<std::meta::info Needle, std::meta::info... Values>
    [[nodiscard]] consteval std::size_t Count() noexcept {
        return Count(InfoList{Values...}, Needle);
    }

    /**
     * @brief Return whether every reflected argument satisfies @p Pred.
     * @tparam Pred Reflected unary predicate.
     * @tparam Values Reflected pack elements.
     * @return `true` for an empty pack and when all elements are accepted.
     */
    template<std::meta::info Pred, std::meta::info... Values>
    [[nodiscard]] consteval bool AllOf() {
        return RequirePredResult(AllOf(InfoList{Values...}, Pred));
    }

    /**
     * @brief Return whether at least one reflected argument satisfies @p Pred.
     * @tparam Pred Reflected unary predicate.
     * @tparam Values Reflected pack elements.
     * @return `false` for an empty pack and when no element is accepted.
     */
    template<std::meta::info Pred, std::meta::info... Values>
    [[nodiscard]] consteval bool AnyOf() {
        return RequirePredResult(AnyOf(InfoList{Values...}, Pred));
    }

    /**
     * @brief Return whether no reflected argument satisfies @p Pred.
     * @tparam Pred Reflected unary predicate.
     * @tparam Values Reflected pack elements.
     * @return Negation of @ref AnyOf for the pack.
     */
    template<std::meta::info Pred, std::meta::info... Values>
    [[nodiscard]] consteval bool NoneOf() {
        return RequirePredResult(NoneOf(InfoList{Values...}, Pred));
    }

    /**
     * @brief Return the number of reflected arguments satisfying @p Pred.
     * @tparam Pred Reflected unary predicate.
     * @tparam Values Reflected pack elements.
     * @return Number of accepted elements.
     */
    template<std::meta::info Pred, std::meta::info... Values>
    [[nodiscard]] consteval std::size_t CountIf() {
        return RequireSizeResult(CountIf(InfoList{Values...}, Pred));
    }

    /**
     * @brief Return @p Needle when present, otherwise @p fallback.
     * @tparam Needle Reflected entity to find.
     * @tparam Values Reflected pack elements.
     * @param[in] fallback Reflection returned when @p Needle is absent.
     * @return @p Needle or @p fallback.
     */
    template<std::meta::info Needle, std::meta::info... Values>
    [[nodiscard]] consteval std::meta::info Find(std::meta::info fallback = {}) noexcept {
        return Contains(InfoList{Values...}, Needle) ? Needle : fallback;
    }

    /**
     * @brief Return the first index of @p Needle, or @ref InfoList::npos.
     * @tparam Needle Reflected entity to find.
     * @tparam Values Reflected pack elements.
     * @return Zero-based index, or @ref InfoList::npos when absent.
     */
    template<std::meta::info Needle, std::meta::info... Values>
    [[nodiscard]] consteval std::size_t IndexOf() noexcept {
        return IndexOf(InfoList{Values...}, Needle);
    }

    /**
     * @brief Return the first reflected argument, or an empty reflection.
     * @tparam Values Reflected pack elements.
     * @return First element, or `std::meta::info{}` for an empty pack.
     */
    template<std::meta::info... Values>
    [[nodiscard]] consteval std::meta::info Front() noexcept {
        if constexpr (sizeof...(Values) == 0) {
            return {};
        } else {
            return std::array{Values...}.front();
        }
    }

    /**
     * @brief Return the final reflected argument, or an empty reflection.
     * @tparam Values Reflected pack elements.
     * @return Final element, or `std::meta::info{}` for an empty pack.
     */
    template<std::meta::info... Values>
    [[nodiscard]] consteval std::meta::info Back() noexcept {
        if constexpr (sizeof...(Values) == 0) {
            return {};
        } else {
            return std::array{Values...}.back();
        }
    }

    /**
     * @brief Return the sole reflected argument, constrained to zero or one input.
     * @tparam Values Zero or one reflected element.
     * @return The element, or `std::meta::info{}` when no element was supplied.
     */
    template<std::meta::info... Values>
        requires(sizeof...(Values) <= 1)
    [[nodiscard]] consteval std::meta::info Single() noexcept {
        return Front<Values...>();
    }

    /**
     * @brief Return the sole reflected argument, or @p Fallback when empty.
     * @tparam Fallback Reflected fallback value.
     * @tparam Values Zero or one reflected element.
     * @return The supplied element, or @p Fallback for an empty pack.
     */
    template<std::meta::info Fallback, std::meta::info... Values>
        requires(sizeof...(Values) <= 1)
    [[nodiscard]] consteval std::meta::info SingleOr() noexcept {
        return sizeof...(Values) == 0 ? Fallback : Front<Values...>();
    }

    /**
     * @brief Return the @p Index-th reflected argument.
     * @tparam Index Zero-based pack index.
     * @tparam Values Reflected pack elements.
     * @return Element at @p Index.
     */
    template<std::size_t Index, std::meta::info... Values>
        requires(Index < sizeof...(Values))
    [[nodiscard]] consteval std::meta::info At() noexcept {
        return std::array{Values...}[Index];
    }

} // namespace Sora::Meta

namespace Sora::Traits {

    /**
     * @brief All but the first type of @p List.
     * @tparam List `TypeList` specialization to transform.
     */
    template<typename List>
        requires Concept::TypeList<List>
    using PopFront = Meta::InfoType<Meta::ToTypeList(Meta::Drop(Meta::ToInfoList(^^List), 1))>;

    /**
     * @brief First @p Count types of @p List, clamped to its length.
     * @tparam List `TypeList` specialization to transform.
     * @tparam Count Maximum number of types to retain.
     */
    template<typename List, std::size_t Count>
        requires Concept::TypeList<List>
    using Take = Meta::InfoType<Meta::ToTypeList(Meta::Take(Meta::ToInfoList(^^List), Count))>;

    /**
     * @brief Types after the first @p Count entries, clamped to its length.
     * @tparam List `TypeList` specialization to transform.
     * @tparam Count Number of leading types to discard.
     */
    template<typename List, std::size_t Count>
        requires Concept::TypeList<List>
    using Drop = Meta::InfoType<Meta::ToTypeList(Meta::Drop(Meta::ToInfoList(^^List), Count))>;

    /**
     * @brief Exact bounded slice of @p List.
     * @tparam List `TypeList` specialization to transform.
     * @tparam Offset Zero-based first type.
     * @tparam Count Number of types to copy.
     */
    template<typename List, std::size_t Offset, std::size_t Count>
        requires(Concept::TypeList<List> && Offset <= Length<List> &&
                 Count <= Length<List> - Offset)
    using Slice = Meta::InfoType<Meta::ToTypeList(Meta::Slice(Meta::ToInfoList(^^List), Offset, Count))>;

    /**
     * @brief Empty type list with no dependency on @p List's elements.
     * @tparam List `TypeList` specialization whose element types are discarded.
     * @return `TypeList<>`.
     */
    template<typename List>
        requires Concept::TypeList<List>
    using Clear = TypeList<>;

    /**
     * @brief TypeList containing @p Count copies of @p Type.
     * @tparam Count Number of copies.
     * @tparam Type Type to repeat.
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
     * @brief Apply reflected unary template @p Function to every type in @p List.
     * @tparam Function Reflected unary template.
     * @tparam List `TypeList` specialization to transform.
     * @return Resulting `TypeList` or a `MetaError` type when substitution fails or yields a non-type.
     */
    template<std::meta::info Function, typename List>
        requires Concept::TypeList<List>
    using Map = Meta::InfoType<Meta::Types::Map(Function, ^^List)>;

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
    using FoldLeft = Meta::InfoType<Meta::FoldLeft(Function, ^^Initial, Meta::ToInfoList(^^List))>;

    /**
     * @brief Right fold reflected @p Function over @p List.
     * @tparam Function Reflected binary template.
     * @tparam Initial Initial accumulator type.
     * @tparam List `TypeList` specialization to fold.
     * @return Final type or a `MetaError` type from the first failed invocation.
     */
    template<std::meta::info Function, typename Initial, typename List>
        requires Concept::TypeList<List>
    using FoldRight = Meta::InfoType<Meta::FoldRight(Function, ^^Initial, Meta::ToInfoList(^^List))>;

    /**
     * @brief First index of @p Type in @p List, or @ref Meta::InfoList::npos.
     * @tparam List `TypeList` specialization to inspect.
     * @tparam Type Reflected type to find.
     */
    template<typename List, typename Type>
        requires Concept::TypeList<List>
    inline constexpr std::size_t IndexOf = Meta::IndexOf(Meta::ToInfoList(^^List), ^^Type);

    /**
     * @brief Whether @p List contains @p Type.
     * @tparam List `TypeList` specialization to inspect.
     * @tparam Type Reflected type to find.
     */
    template<typename List, typename Type>
        requires Concept::TypeList<List>
    inline constexpr bool Contains = Meta::Contains(Meta::ToInfoList(^^List), ^^Type);

    /**
     * @brief Number of occurrences of @p Type in @p List.
     * @tparam List `TypeList` specialization to inspect.
     * @tparam Type Reflected type to count.
     */
    template<typename List, typename Type>
        requires Concept::TypeList<List>
    inline constexpr std::size_t Count = Meta::Count(Meta::ToInfoList(^^List), ^^Type);

    /**
     * @brief First type in @p List accepted by @p Pred, or `void` when no type matches.
     * @tparam Pred Reflected unary meta predicate.
     * @tparam List `TypeList` specialization to inspect.
     * @return A matching type, `void`, or a `MetaError` type for predicate/input failure.
     * @par Example
     * @code{.cpp}
     * using Match = Sora::Traits::FindFirst<^^std::is_class, Sora::Traits::TypeList<int, void>>;
     * static_assert(std::same_as<Match, void>);
     * @endcode
     */
    template<std::meta::info Pred, typename List>
        requires Concept::TypeList<List>
    using FindFirst = Meta::InfoType<Meta::FindFirst(Meta::ToInfoList(^^List), Pred)>;

    /**
     * @brief Final type in @p List accepted by @p Pred, or `void` when no type matches.
     * @tparam Pred Reflected unary meta predicate.
     * @tparam List `TypeList` specialization to inspect.
     * @return A matching type, `void`, or a `MetaError` type for predicate/input failure.
     * @par Example
     * @code{.cpp}
     * using Match = Sora::Traits::FindLast<^^std::is_class, Sora::Traits::TypeList<int, void>>;
     * static_assert(std::same_as<Match, void>);
     * @endcode
     */
    template<std::meta::info Pred, typename List>
        requires Concept::TypeList<List>
    using FindLast = Meta::InfoType<Meta::FindLast(Meta::ToInfoList(^^List), Pred)>;

    /**
     * @brief Prepend @p Type to @p List.
     * @tparam Type Type to insert.
     * @tparam List `TypeList` specialization to transform.
     * @return Resulting `TypeList` or a `MetaError` type if projection fails.
     */
    template<typename Type, typename List>
        requires Concept::TypeList<List>
    using PushFront = Meta::InfoType<Meta::ToTypeList(Meta::PushFront(Meta::ToInfoList(^^List), ^^Type))>;

    /**
     * @brief Append @p Type to @p List.
     * @tparam List `TypeList` specialization to transform.
     * @tparam Type Type to insert.
     * @return Resulting `TypeList` or a `MetaError` type if projection fails.
     */
    template<typename List, typename Type>
        requires Concept::TypeList<List>
    using PushBack = Meta::InfoType<Meta::ToTypeList(Meta::PushBack(Meta::ToInfoList(^^List), ^^Type))>;

    /**
     * @brief All but the final type of @p List.
     * @tparam List `TypeList` specialization to transform.
     * @return Resulting `TypeList`; an empty input remains empty.
     */
    template<typename List>
        requires Concept::TypeList<List>
    using PopBack = Meta::InfoType<Meta::ToTypeList(Meta::PopBack(Meta::ToInfoList(^^List)))>;

    /**
     * @brief Types of @p List in reverse order.
     * @tparam List `TypeList` specialization to transform.
     * @return Resulting `TypeList` or a `MetaError` type if the projection fails.
     */
    template<typename List>
        requires Concept::TypeList<List>
    using Reverse = Meta::InfoType<Meta::ToTypeList(Meta::Reverse(Meta::ToInfoList(^^List)))>;

    /**
     * @brief Remove duplicate types while retaining first occurrences.
     * @tparam List `TypeList` specialization to transform.
     * @return Stable unique `TypeList` or a `MetaError` type if the projection fails.
     */
    template<typename List>
        requires Concept::TypeList<List>
    using Unique = Meta::InfoType<Meta::ToTypeList(Meta::Unique(Meta::ToInfoList(^^List)))>;

    /**
     * @brief Whether every type in @p List occurs exactly once.
     * @tparam List `TypeList` specialization to inspect.
     */
    template<typename List>
        requires Concept::TypeList<List>
    inline constexpr bool IsUnique = Meta::IsUnique(Meta::ToInfoList(^^List));

    /**
     * @brief Replace every @p Old type in @p List with @p New.
     * @tparam Old Type to replace.
     * @tparam New Replacement type.
     * @tparam List `TypeList` specialization to transform.
     * @return Resulting `TypeList` or a `MetaError` type for invalid reflected type arguments.
     */
    template<typename Old, typename New, typename List>
        requires Concept::TypeList<List>
    using Replace = Meta::InfoType<Meta::Types::Replace(^^Old, ^^New, ^^List)>;

    /**
     * @brief Remove every occurrence of @p Needle from @p List.
     * @tparam Needle Type to remove.
     * @tparam List `TypeList` specialization to transform.
     * @return Resulting `TypeList` or a `MetaError` type for invalid reflected type arguments.
     */
    template<typename Needle, typename List>
        requires Concept::TypeList<List>
    using Remove = Meta::InfoType<Meta::Types::Remove(^^Needle, ^^List)>;

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
     * @brief Whether every type in @p List satisfies @p Pred.
     * @tparam Pred Reflected unary meta predicate.
     * @tparam List `TypeList` specialization to inspect.
     */
    template<std::meta::info Pred, typename List>
        requires Concept::TypeList<List>
    inline constexpr bool AllOf = Meta::RequirePredResult(Meta::AllOf(Meta::ToInfoList(^^List), Pred));

    /**
     * @brief Whether any type in @p List satisfies @p Pred.
     * @tparam Pred Reflected unary meta predicate.
     * @tparam List `TypeList` specialization to inspect.
     */
    template<std::meta::info Pred, typename List>
        requires Concept::TypeList<List>
    inline constexpr bool AnyOf = Meta::RequirePredResult(Meta::AnyOf(Meta::ToInfoList(^^List), Pred));

    /**
     * @brief Whether no type in @p List satisfies @p Pred.
     * @tparam Pred Reflected unary meta predicate.
     * @tparam List `TypeList` specialization to inspect.
     */
    template<std::meta::info Pred, typename List>
        requires Concept::TypeList<List>
    inline constexpr bool NoneOf =
        Meta::RequirePredResult(Meta::NoneOf(Meta::ToInfoList(^^List), Pred));

    /**
     * @brief Number of types in @p List satisfying @p Pred.
     * @tparam Pred Reflected unary meta predicate.
     * @tparam List `TypeList` specialization to inspect.
     */
    template<std::meta::info Pred, typename List>
        requires Concept::TypeList<List>
    inline constexpr std::size_t CountIf =
        Meta::RequireSizeResult(Meta::CountIf(Meta::ToInfoList(^^List), Pred));

    /**
     * @brief TypeList of non-static data-member types of @p Type.
     * @tparam Type Aggregate/class type whose non-static data members are projected.
     * @return Reflected member-type pack represented as a `TypeList` C++ type.
     */
    template<typename Type>
    using ToTypeList = Meta::InfoType<Meta::ToTypeList(Meta::Promote(
        std::meta::nonstatic_data_members_of(^^Type, std::meta::access_context::unchecked()) |
        std::views::transform(std::meta::type_of)))>;

} // namespace Sora::Traits

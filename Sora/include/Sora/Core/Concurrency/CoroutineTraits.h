/**
 * @file Traits.h
 * @brief Compile-time concepts and reflection queries for C++ coroutine protocols.
 * @ingroup Core
 *
 * @details The header separates coroutine handles, awaiter/awaitable expressions, and promise capabilities. Concepts
 * model expression validity for a concrete promise, while reflection exposes overload-independent declaration
 * metadata. Every facility is compile-time only and introduces no runtime state or dispatch.
 */
#pragma once

#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <meta>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <Sora/Core/Traits/InheritanceTraits.h>
#include <Sora/Core/Traits/MetaInfoList.h>
#include <Sora/Core/Traits/TypeTraits.h>

namespace Sora {

    namespace Concept {

        /**
         * @brief Recognize any @c std::coroutine_handle specialization after cv/ref removal.
         * @tparam T Candidate type.
         */
        template<typename T>
        concept CoroutineHandle =
            Sora::Meta::IsSpecializationOf<std::coroutine_handle>(std::meta::dealias(^^std::remove_cvref_t<T>));

        /**
         * @brief Recognize the type-erased @c std::coroutine_handle<> specialization.
         * @tparam T Candidate type.
         */
        template<typename T>
        concept TypeErasedCoroutineHandle = std::same_as<std::remove_cvref_t<T>, std::coroutine_handle<>>;

        /**
         * @brief Recognize a promise-typed @c std::coroutine_handle specialization.
         * @tparam T Candidate type.
         */
        template<typename T>
        concept TypedCoroutineHandle = CoroutineHandle<T> && !TypeErasedCoroutineHandle<T>;

        /**
         * @brief Recognize an exact result type permitted for @c await_suspend.
         *
         * @details The coroutine language requires a prvalue of exactly @c void, @c bool, or
         * @c std::coroutine_handle<Z>. References and cv-qualified handle objects are rejected.
         * @tparam T Candidate result type.
         */
        template<typename T>
        concept AwaitSuspendResult = std::same_as<T, void> || std::same_as<T, bool> ||
                                     (std::same_as<T, std::remove_cvref_t<T>> && CoroutineHandle<T>);

    } // namespace Concept

    namespace Meta {

        /**
         * @brief Return non-template @c await_suspend overloads with language-valid signatures.
         *
         * @details A selected function must have one coroutine-handle parameter and return @c void, @c bool, or a
         * coroutine handle. Function templates cannot be classified before substitution and are therefore validated
         * by @c Concept::AwaiterFor instead. Results preserve derived-first hierarchy order and have static storage.
         *
         * @param[in] awaiter Reflected awaiter class type.
         * @param[in] requireNoexcept Whether matching declarations must be non-throwing.
         * @return Static reflection sequence of matching member functions.
         */
        consteval InfoList AwaitSuspendFunctionsOf(std::meta::info awaiter, bool requireNoexcept = false) {
            awaiter = std::meta::dealias(awaiter);
            if (!std::meta::is_class_type(awaiter)) {
                return {};
            }
            std::vector<std::meta::info> suspenders;
            Sora::Meta::WalkHierarchyMembers(
                awaiter,
                [&suspenders, requireNoexcept](std::size_t, std::meta::info member) {
                    if (!std::meta::is_function(member) || !std::meta::has_identifier(member) ||
                        std::meta::identifier_of(member) != "await_suspend" ||
                        (requireNoexcept && !std::meta::is_noexcept(member))) {
                        return true;
                    }

                    const auto parameters = Sora::Meta::ParamTypesOf(member);
                    if (parameters.size() != 1 || !Sora::Meta::IsSpecializationOf<std::coroutine_handle>(
                                                      std::meta::dealias(parameters.front()))) {
                        return true;
                    }

                    const auto returnType = std::meta::dealias(std::meta::return_type_of(member));
                    if (returnType == ^^void || returnType == ^^bool ||
                        Sora::Meta::IsSpecializationOf<std::coroutine_handle>(returnType)) {
                        suspenders.push_back(member);
                    }
                    return true;
                },
                std::meta::access_context::unchecked());
            return suspenders.empty() ? InfoList{} : Promote(suspenders);
        }

        /**
         * @brief Return promise functions or function templates with a specified identifier.
         * @param[in] promise Reflected promise class type.
         * @param[in] identifier Capability name such as @c return_void or @c await_transform.
         * @return Static reflection sequence preserving derived-first hierarchy order.
         */
        consteval InfoList CoroutineFunctionsNamed(std::meta::info promise, std::string_view identifier) {
            promise = std::meta::dealias(promise);
            if (!std::meta::is_class_type(promise)) {
                return {};
            }
            std::vector<std::meta::info> functions;
            Sora::Meta::WalkHierarchyMembers(
                promise,
                [&functions, identifier](std::size_t, std::meta::info member) {
                    if ((std::meta::is_function(member) || std::meta::is_function_template(member)) &&
                        std::meta::has_identifier(member) && std::meta::identifier_of(member) == identifier) {
                        functions.push_back(member);
                    }
                    return true;
                },
                std::meta::access_context::unchecked());
            return functions.empty() ? InfoList{} : Promote(functions);
        }

        /**
         * @brief Return every completion/yield declaration exposed by a promise.
         *
         * @details @c yield_value is orthogonal to @c return_void and @c return_value. A generator commonly declares
         * both @c yield_value and @c return_void, so this query deliberately does not impose mutual exclusion.
         *
         * @param[in] promise Reflected promise class type.
         * @return Static sequence ordered as @c return_value, @c return_void, then @c yield_value declarations.
         */
        consteval InfoList CoroutineReturnFunctionsOf(std::meta::info promise) {
            return Concat(CoroutineFunctionsNamed(promise, "return_value"),
                          CoroutineFunctionsNamed(promise, "return_void"),
                          CoroutineFunctionsNamed(promise, "yield_value"));
        }

        /**
         * @brief Select a promise's primary completion declaration for coarse classification.
         *
         * @details Selection precedence is @c return_value, @c return_void, then @c yield_value. Use
         * @c CoroutineReturnFunctionsOf when every capability matters.
         *
         * @param[in] promise Reflected promise class type.
         * @return First declaration in the documented precedence order.
         * @throws A translation-time diagnostic when the promise exposes no completion or yield declaration.
         */
        consteval std::meta::info CoroutineReturnFunctionOf(std::meta::info promise) {
            const auto functions = CoroutineReturnFunctionsOf(promise);
            if (!functions.empty()) {
                return functions.front();
            }
            throw std::define_static_string("Promise type must declare return_value, return_void, or yield_value.");
        }

    } // namespace Meta

    namespace Concept {

        /**
         * @brief Recognize an awaiter for a coroutine with promise @p Promise.
         *
         * @details The compiler invokes all three awaiter operations on an lvalue. @c await_ready must be contextually
         * convertible to @c bool, while @c await_suspend must accept @c coroutine_handle<Promise> and produce one of
         * the three exact result categories permitted by [expr.await].
         *
         * @tparam T Candidate awaiter type, possibly cv/ref-qualified.
         * @tparam Promise Promise type of the awaiting coroutine; @c void models a type-erased continuation.
         */
        template<typename T, typename Promise>
        concept AwaiterFor =
            requires(std::remove_reference_t<T>& awaiter, std::coroutine_handle<Promise> continuation) {
                { awaiter.await_ready() } -> std::convertible_to<bool>;
                awaiter.await_resume();
                awaiter.await_suspend(continuation);
            } &&
            AwaitSuspendResult<decltype(std::declval<std::remove_reference_t<T>&>().await_suspend(
                std::declval<std::coroutine_handle<Promise>>()))>;

        /**
         * @brief Recognize an awaiter with a language-valid protocol.
         *
         * @details Unlike @c AwaiterFor, this concept does not require @c await_suspend to accept a particular
         * continuation handle. It accepts any non-template @c await_suspend declaration with one coroutine-handle
         * parameter and a language-valid return type.
         *
         * @tparam T Candidate awaiter type, possibly cv/ref-qualified.
         */
        template<typename T>
        concept Awaiter = requires(std::remove_reference_t<T>& awaiter) {
            { awaiter.await_ready() } -> std::convertible_to<bool>;
            awaiter.await_resume();
        } && !Meta::AwaitSuspendFunctionsOf(^^std::remove_cvref_t<T>).empty();

        /**
         * @brief Recognize an awaiter whose protocol operations cannot throw for @p Promise.
         * @tparam T Candidate awaiter type, possibly cv/ref-qualified.
         * @tparam Promise Promise type of the awaiting coroutine.
         */
        template<typename T, typename Promise>
        concept NothrowAwaiterFor = AwaiterFor<T, Promise> && requires(std::remove_reference_t<T>& awaiter,
                                                                       std::coroutine_handle<Promise> continuation) {
            requires noexcept(awaiter.await_ready());
            requires noexcept(awaiter.await_suspend(continuation));
            requires noexcept(awaiter.await_resume());
        };

        /**
         * @brief Recognize a non-throwing awaiter accepting a type-erased continuation handle.
         * @tparam T Candidate awaiter type, possibly cv/ref-qualified.
         */
        template<typename T>
        concept NothrowAwaiter = Awaiter<T> && requires(std::remove_reference_t<T>& awaiter) {
            requires noexcept(awaiter.await_ready());
            requires noexcept(awaiter.await_resume());
        } && [] consteval {
            auto func = Meta::AwaitSuspendFunctionsOf(^^std::remove_cvref_t<T>, true);
            return !func.empty() &&
                   std::ranges::all_of(func, [](std::meta::info member) { return std::meta::is_noexcept(member); });
        };

        /**
         * @brief Recognize one exact @c await_suspend parameter/return signature.
         * @tparam T Candidate awaiter type.
         * @tparam R Required return type.
         * @tparam Arg Required argument type.
         */
        template<typename T, typename R, typename Arg>
        concept HasAwaitSuspendOfSignature = requires(std::remove_reference_t<T>& awaiter) {
            { awaiter.await_suspend(std::declval<Arg>()) } -> std::same_as<R>;
        };

    } // namespace Concept

    /** @cond INTERNAL */
    namespace Detail::CoroutineTraits {

        struct AdlLookupAnchor {};
        void operator co_await(AdlLookupAnchor) = delete;

    } // namespace Detail::CoroutineTraits
    /** @endcond */

    namespace Concept {

        template<typename T>
        concept HasCoAwaitOperatorMember = requires(T&& awaitable) { std::forward<T>(awaitable).operator co_await(); };

        template<typename T>
        concept HasADLCoAwaitOperator = requires(T&& awaitable) { operator co_await(std::forward<T>(awaitable)); };

        template<typename T, typename Promise>
        concept MemberAwaitableFor = HasCoAwaitOperatorMember<T> && requires(T&& awaitable) {
            { std::forward<T>(awaitable).operator co_await() } -> Sora::Concept::AwaiterFor<Promise>;
        };

        template<typename T, typename Promise>
        concept NothrowMemberAwaitableFor = MemberAwaitableFor<T, Promise> && requires(T&& awaitable) {
            requires noexcept(std::forward<T>(awaitable).operator co_await());
            requires Sora::Concept::NothrowAwaiterFor<decltype(std::forward<T>(awaitable).operator co_await()),
                                                      Promise>;
        };

        template<typename T, typename Promise>
        concept AdlAwaitableFor = HasADLCoAwaitOperator<T> && requires(T&& awaitable) {
            { operator co_await(std::forward<T>(awaitable)) } -> Sora::Concept::AwaiterFor<Promise>;
        };

        template<typename T, typename Promise>
        concept NothrowAdlAwaitableFor = AdlAwaitableFor<T, Promise> && requires(T&& awaitable) {
            requires noexcept(operator co_await(std::forward<T>(awaitable)));
            requires Sora::Concept::NothrowAwaiterFor<decltype(operator co_await(std::forward<T>(awaitable))), Promise>;
        };

        /**
         * @brief Recognize a direct awaiter or one produced by member/ADL @c operator co_await for @p Promise.
         *
         * @details The direct awaiter is considered only when neither operator form is viable, matching the
         * language transformation. A type with both viable member and ADL operators is rejected conservatively
         * because the complete overload set is call-site dependent; the concrete @c co_await expression remains
         * authoritative.
         *
         * @tparam T Candidate awaitable expression type.
         * @tparam Promise Promise type of the awaiting coroutine.
         */
        template<typename T, typename Promise>
        concept AwaitableFor =
            (!Sora::Concept::HasCoAwaitOperatorMember<T> && !Sora::Concept::HasADLCoAwaitOperator<T> &&
             AwaiterFor<T, Promise>) ||
            (Sora::Concept::MemberAwaitableFor<T, Promise> && !Sora::Concept::HasADLCoAwaitOperator<T>) ||
            (!Sora::Concept::HasCoAwaitOperatorMember<T> && Sora::Concept::AdlAwaitableFor<T, Promise>);

        /**
         * @brief Recognize an awaitable compatible with a type-erased continuation handle.
         * @tparam T Candidate awaitable expression type.
         */
        template<typename T>
        concept AwaitableType = AwaitableFor<T, void>;

        /**
         * @brief Recognize a non-throwing direct, member, or ADL awaitable for @p Promise.
         * @details Like @c AwaitableFor, this predicate conservatively rejects simultaneous member and ADL
         * operators.
         * @tparam T Candidate awaitable expression type.
         * @tparam Promise Promise type of the awaiting coroutine.
         */
        template<typename T, typename Promise>
        concept NothrowAwaitableFor =
            (!Sora::Concept::HasCoAwaitOperatorMember<T> && !Sora::Concept::HasADLCoAwaitOperator<T> &&
             NothrowAwaiterFor<T, Promise>) ||
            (Sora::Concept::NothrowMemberAwaitableFor<T, Promise> && !Sora::Concept::HasADLCoAwaitOperator<T>) ||
            (!Sora::Concept::HasCoAwaitOperatorMember<T> && Sora::Concept::NothrowAdlAwaitableFor<T, Promise>);

        /**
         * @brief Recognize a non-throwing awaitable compatible with a type-erased continuation handle.
         * @tparam T Candidate awaitable expression type.
         */
        template<typename T>
        concept NothrowAwaitableType = NothrowAwaitableFor<T, void>;

        /**
         * @brief Recognize a type that is either an awaiter or an awaitable.
         * @tparam T Candidate type.
         */
        template<typename T>
        concept AwaiterOrAwaitable = Sora::Concept::Awaiter<T> || Sora::Concept::AwaitableType<T>;

        /**
         * @brief Recognize a type that is either an awaiter or an awaitable for @p Promise.
         * @tparam T Candidate type.
         * @tparam Promise Promise type of the awaiting coroutine.
         */
        template<typename T, typename Promise>
        concept AwaiterOrAwaitableFor =
            Sora::Concept::AwaiterFor<T, Promise> || Sora::Concept::AwaitableFor<T, Promise>;

    } // namespace Concept

    namespace Traits {

        /**
         * @brief Result type of invoking @c await_resume on an awaiter lvalue.
         * @tparam T Awaiter type, possibly cv/ref-qualified.
         */
        template<typename T>
        using AwaitResultType = decltype(std::declval<std::remove_reference_t<T>&>().await_resume());

        /**
         * @brief Return type of @c await_suspend for an awaiter and promise pair.
         * @tparam T Awaiter type, possibly cv/ref-qualified.
         * @tparam Promise Promise type of the awaiting coroutine.
         */
        template<typename T, typename Promise = void>
            requires Sora::Concept::AwaiterFor<T, Promise>
        using AwaitSuspendResultType = decltype(std::declval<std::remove_reference_t<T>&>().await_suspend(
            std::declval<std::coroutine_handle<Promise>>()));

    } // namespace Traits

    namespace Concept {

        /**
         * @brief Recognize the core operations required from a coroutine promise type.
         *
         * @details Completion operations are intentionally separate: which of @c return_void and @c return_value is
         * required depends on the coroutine body, while @c yield_value is orthogonal. Initial and final suspend
         * results are checked against the candidate promise's typed continuation; the complete final await
         * expression must be non-throwing. A promise declaring both return forms is rejected.
         * @tparam T Candidate promise type.
         */
        template<typename T>
        concept PromiseType =
            requires(std::remove_reference_t<T>& promise) {
                promise.get_return_object();
                promise.unhandled_exception();
                { promise.final_suspend() } noexcept;
            } &&
            requires(std::remove_reference_t<T>& promise) {
                requires AwaitableFor<decltype(promise.initial_suspend()), std::remove_cvref_t<T>>;
                requires NothrowAwaitableFor<decltype(promise.final_suspend()), std::remove_cvref_t<T>>;
            } &&
            !(!Sora::Meta::CoroutineFunctionsNamed(^^std::remove_cvref_t<T>, "return_value").empty() &&
              !Sora::Meta::CoroutineFunctionsNamed(^^std::remove_cvref_t<T>, "return_void").empty());

        /**
         * @brief Promise declares at least one @c return_value function or function template.
         * @tparam T Candidate promise type.
         */
        template<typename T>
        concept PromiseTypeWithValueReturn =
            PromiseType<T> && !Sora::Meta::CoroutineFunctionsNamed(^^std::remove_cvref_t<T>, "return_value").empty();

        /**
         * @brief Promise supports the nullary @c return_void expression.
         * @tparam T Candidate promise type.
         */
        template<typename T>
        concept PromiseTypeWithVoidReturn =
            PromiseType<T> && requires(std::remove_reference_t<T>& promise) { promise.return_void(); };

        /**
         * @brief Promise declares at least one @c yield_value function or function template.
         * @tparam T Candidate promise type.
         */
        template<typename T>
        concept PromiseTypeWithYieldReturn =
            PromiseType<T> && !Sora::Meta::CoroutineFunctionsNamed(^^std::remove_cvref_t<T>, "yield_value").empty();

        /**
         * @brief Promise declares at least one @c await_transform function or function template.
         * @tparam T Candidate promise type.
         */
        template<typename T>
        concept PromiseTypeWithTransformer =
            PromiseType<T> && !Sora::Meta::CoroutineFunctionsNamed(^^std::remove_cvref_t<T>, "await_transform").empty();

        /**
         * @brief Promise accepts @p Value through @c return_value.
         * @tparam T Candidate promise type.
         * @tparam Value Argument type and value category supplied to @c return_value.
         */
        template<typename T, typename Value>
        concept ValueReturningPromiseFor =
            PromiseType<T> && requires(std::remove_reference_t<T>& promise, Value&& value) {
                promise.return_value(std::forward<Value>(value));
            };

        /**
         * @brief Promise converts @p Value through @c yield_value to an awaitable for the same promise.
         * @tparam T Candidate promise type.
         * @tparam Value Argument type and value category supplied to @c yield_value.
         */
        template<typename T, typename Value>
        concept YieldingPromiseFor = PromiseType<T> && requires(std::remove_reference_t<T>& promise, Value&& value) {
            requires AwaitableFor<decltype(promise.yield_value(std::forward<Value>(value))), std::remove_cvref_t<T>>;
        };

        /**
         * @brief Promise converts @p Awaitable through @c await_transform to an awaitable for the same promise.
         * @tparam T Candidate promise type.
         * @tparam Awaitable Argument type and value category supplied to @c await_transform.
         */
        template<typename T, typename Awaitable>
        concept TransformingPromiseFor =
            PromiseType<T> && requires(std::remove_reference_t<T>& promise, Awaitable&& awaitable) {
                requires AwaitableFor<decltype(promise.await_transform(std::forward<Awaitable>(awaitable))),
                                      std::remove_cvref_t<T>>;
            };

        /**
         * @brief Recognize a coroutine return object exposing a conforming nested @c promise_type.
         * @tparam T Candidate coroutine return-object type.
         */
        template<typename T>
        concept CoroutineType = requires { typename std::remove_cvref_t<T>::promise_type; } &&
                                PromiseType<typename std::remove_cvref_t<T>::promise_type>;

        /**
         * @brief Coroutine return object whose promise declares @c return_value.
         * @tparam T Candidate coroutine return-object type.
         */
        template<typename T>
        concept CoroutineTypeWithValueReturn =
            CoroutineType<T> && PromiseTypeWithValueReturn<typename std::remove_cvref_t<T>::promise_type>;

        /**
         * @brief Coroutine return object whose promise supports @c return_void.
         * @tparam T Candidate coroutine return-object type.
         */
        template<typename T>
        concept CoroutineTypeWithVoidReturn =
            CoroutineType<T> && PromiseTypeWithVoidReturn<typename std::remove_cvref_t<T>::promise_type>;

        /**
         * @brief Coroutine return object whose promise declares @c yield_value.
         * @tparam T Candidate coroutine return-object type.
         */
        template<typename T>
        concept CoroutineTypeWithYieldReturn =
            CoroutineType<T> && PromiseTypeWithYieldReturn<typename std::remove_cvref_t<T>::promise_type>;

        /**
         * @brief Coroutine return object whose promise declares @c await_transform.
         * @tparam T Candidate coroutine return-object type.
         */
        template<typename T>
        concept CoroutineTypeWithTransformer =
            CoroutineType<T> && PromiseTypeWithTransformer<typename std::remove_cvref_t<T>::promise_type>;

        /**
         * @brief Recognize a coroutine return object with a syntactically declared yield capability.
         * @details Use @c YieldingPromiseFor with a concrete yielded type when expression-level validation is
         * needed.
         * @tparam T Candidate coroutine return-object type.
         */
        template<typename T>
        concept GeneratorType = CoroutineTypeWithYieldReturn<T>;

    } // namespace Concept

    namespace Traits {

        /**
         * @brief Promise type associated with a coroutine return object.
         * @tparam T Conforming coroutine return-object type.
         */
        template<Sora::Concept::CoroutineType T>
        using PromiseType = typename std::remove_cvref_t<T>::promise_type;

        /** @brief Classification of an exact @c await_suspend result category. */
        enum class AwaitSuspendSignature : std::uint8_t {
            VoidHandle,   ///< @c await_suspend returns @c void.
            BoolHandle,   ///< @c await_suspend returns @c bool.
            HandleHandle, ///< @c await_suspend returns a coroutine handle.
        };

        /**
         * @brief Classify the @c await_suspend result for an awaiter and promise pair.
         * @tparam T Conforming awaiter type.
         * @tparam Promise Promise type of the awaiting coroutine.
         */
        template<typename T, typename Promise = void>
            requires Sora::Concept::AwaiterFor<T, Promise>
        inline constexpr AwaitSuspendSignature AwaitSuspendSignatureOf = [] consteval {
            using Result = AwaitSuspendResultType<T, Promise>;
            if constexpr (std::same_as<Result, void>) {
                return AwaitSuspendSignature::VoidHandle;
            } else if constexpr (std::same_as<Result, bool>) {
                return AwaitSuspendSignature::BoolHandle;
            } else {
                return AwaitSuspendSignature::HandleHandle;
            }
        }();

        /** @brief Classification of a promise's primary completion/yield declaration. */
        enum class CoroutineReturnFunction : std::uint8_t {
            ReturnValue, ///< Primary declaration is @c return_value.
            ReturnVoid,  ///< Primary declaration is @c return_void.
            YieldValue,  ///< Primary declaration is @c yield_value.
        };

        /**
         * @brief Classify a promise by its primary completion/yield declaration.
         * @details Precedence is @c return_value, @c return_void, then @c yield_value.
         * @tparam T Conforming promise type.
         */
        template<Sora::Concept::PromiseType T>
        inline constexpr CoroutineReturnFunction CoroutineReturnFunctionOf = [] consteval {
            const auto member = Sora::Meta::CoroutineReturnFunctionOf(^^std::remove_cvref_t<T>);
            const auto identifier = std::meta::identifier_of(member);
            if (identifier == "return_value") {
                return CoroutineReturnFunction::ReturnValue;
            }
            if (identifier == "return_void") {
                return CoroutineReturnFunction::ReturnVoid;
            }
            if (identifier == "yield_value") {
                return CoroutineReturnFunction::YieldValue;
            }
            throw std::define_static_string("Unknown coroutine completion function.");
        }();

    } // namespace Traits

} // namespace Sora

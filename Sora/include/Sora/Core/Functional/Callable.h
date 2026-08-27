/**
 * @file Callable.h
 * @brief Zero-overhead callable adaptors and composition.
 * @ingroup Core
 */
#pragma once

#include <Sora/Platform.h>

#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

namespace Sora {

    /** @brief Marker base for Sora pipeable callable closures. */
    struct AdaptorClosureBase {};

    namespace Concept {

        /** @brief Sora pipeable callable closure. */
        template<typename T>
        concept AdaptorClosure = std::derived_from<std::remove_cvref_t<T>, AdaptorClosureBase>;

        /** @brief Value that is not itself a Sora adaptor closure. */
        template<typename T>
        concept NonAdaptorClosure = !AdaptorClosure<T>;

    } // namespace Concept

    /**
     * @brief Pipeable callable owning one decayed function object.
     * @details Invocation preserves the closure's cv-ref category, allowing mutable and move-only callables without
     * type erasure, allocation, or a cv-ref overload set. @p F must be an object type. The stored callable is
     * initialized by value and remains owned by the closure for its complete lifetime.
     * @tparam F Stored callable object type.
     * @code{.cpp}
     * auto increment = Sora::Adaptor{[](int value) { return value + 1; }};
     * auto input = 4;
     * auto result = increment(input); // 5
     * @endcode
     */
    template<typename F>
        requires std::is_object_v<F>
    struct Adaptor : AdaptorClosureBase {
        /**
         * @brief Construct from the callable that this closure owns.
         * @param[in] function Callable object moved into the adaptor.
         */
        constexpr explicit Adaptor(F function) noexcept(std::is_nothrow_move_constructible_v<F>)
            : function(std::move(function)) {}

        SORA_NO_UNIQUE_ADDRESS F function;

        template<typename Self, typename... Arguments>
            requires std::invocable<decltype((std::forward<Self>(std::declval<Self>()).function)), Arguments...>
        constexpr decltype(auto) operator()(this Self&& self, Arguments&&... arguments) noexcept(
            std::is_nothrow_invocable_v<decltype((std::forward<Self>(self).function)), Arguments...>) {
            return std::invoke(std::forward<Self>(self).function, std::forward<Arguments>(arguments)...);
        }
    };

    template<typename F>
    Adaptor(F) -> Adaptor<F>;

    /**
     * @brief Invoke a Sora adaptor closure with a piped value.
     * @details Both operands retain their cv-ref categories, so an owning rvalue range can be transferred into an
     * adapter while an lvalue range remains borrowed. The closure must be invocable with the value.
     * @param[in] value Input value or range.
     * @param[in] adaptor Pipeable closure to invoke.
     * @return The adaptor invocation result, preserving its declared value category.
     * @code{.cpp}
     * auto values = std::array{1, 2, 3};
     * auto doubled = values | Sora::Fn([](const auto& input) {
     *     return input[1] * 2;
     * });
     * // doubled == 4
     * @endcode
     */
    template<Concept::NonAdaptorClosure Value, Concept::AdaptorClosure Closure>
        requires std::invocable<Closure, Value>
    [[nodiscard]] constexpr decltype(auto) operator|(Value&& value, Closure&& adaptor) noexcept(
        std::is_nothrow_invocable_v<Closure, Value>) {
        return std::invoke(std::forward<Closure>(adaptor), std::forward<Value>(value));
    }

    /**
     * @brief Wrap @p function in an owning pipeable closure.
     * @details The callable is stored as @c std::decay_t<F>; it must be constructible from @p function. No type
     * erasure or allocation is introduced. Use @c std::ref when the closure must explicitly refer to an existing
     * callable instead of owning it.
     * @tparam F Callable type before decay.
     * @param[in] function Callable to copy or move into the closure.
     * @return A pipeable closure that owns the decayed callable.
     * @code{.cpp}
     * auto addOne = Sora::Fn([](int value) { return value + 1; });
     * auto input = 4;
     * auto result = input | addOne; // 5
     * @endcode
     */
    template<typename F>
        requires std::constructible_from<std::decay_t<F>, F>
    [[nodiscard]] constexpr auto Fn(F&& function) noexcept(std::is_nothrow_constructible_v<std::decay_t<F>, F>) {
        return Adaptor<std::decay_t<F>>{std::forward<F>(function)};
    }

    namespace Detail {

        struct IdentityFunction {
            template<typename T>
            [[nodiscard]] constexpr T&& operator()(T&& value) const noexcept {
                return std::forward<T>(value);
            }
        };

        template<typename V>
        struct ConstantFunction {
            SORA_NO_UNIQUE_ADDRESS V value;

            template<typename Self, typename... Arguments>
                requires std::constructible_from<V, decltype((std::forward<Self>(std::declval<Self>()).value))>
            [[nodiscard]] constexpr V operator()(this Self&& self, Arguments&&...) noexcept(
                std::is_nothrow_constructible_v<V, decltype((std::forward<Self>(self).value))>) {
                return std::forward<Self>(self).value;
            }
        };

        template<typename Outer, typename Inner>
        struct Composition {
            SORA_NO_UNIQUE_ADDRESS Outer outer;
            SORA_NO_UNIQUE_ADDRESS Inner inner;

            template<typename Self, typename... Arguments>
                requires requires(Self&& self, Arguments&&... arguments) {
                    std::invoke(std::forward<Self>(self).outer,
                                std::invoke(std::forward<Self>(self).inner, std::forward<Arguments>(arguments)...));
                }
            constexpr decltype(auto) operator()(this Self&& self, Arguments&&... arguments) noexcept(noexcept(
                std::invoke(std::forward<Self>(self).outer,
                            std::invoke(std::forward<Self>(self).inner, std::forward<Arguments>(arguments)...)))) {
                return std::invoke(std::forward<Self>(self).outer,
                                   std::invoke(std::forward<Self>(self).inner,
                                               std::forward<Arguments>(arguments)...));
            }
        };

        template<typename Outer, typename Inner>
        Composition(Outer, Inner) -> Composition<Outer, Inner>;

    } // namespace Detail

    /**
     * @brief Identity closure preserving the operand's cv-ref category.
     * @details The closure is stateless and returns the forwarded operand unchanged. It is useful as a default
     * projection or as a neutral callable in a composition.
     * @code{.cpp}
     * int value = 4;
     * int& reference = Sora::Identity(value);
     * reference = 5; // value == 5
     * @endcode
     */
    inline constexpr auto Identity = Fn(Detail::IdentityFunction{});

    /**
     * @brief Return a closure that ignores its arguments and returns a stored value by value.
     * @details @p value must be constructible as its decayed storage type and is owned by the returned closure.
     * Invocation copies it for lvalue closures and moves it for rvalue closures when the value type permits that
     * operation.
     * @tparam V Value type before decay.
     * @param[in] value Value to store in the closure.
     * @return A pipeable constant-value closure.
     * @code{.cpp}
     * auto answer = Sora::Constant(42);
     * auto input = std::string{"ignored"};
     * auto result = answer(input); // 42
     * @endcode
     */
    template<typename V>
        requires std::constructible_from<std::decay_t<V>, V>
    [[nodiscard]] constexpr auto Constant(V&& value) {
        return Fn(Detail::ConstantFunction<std::decay_t<V>>{std::forward<V>(value)});
    }

    /**
     * @brief Compose one callable from right to left.
     * @details The single callable is wrapped as a pipeable closure. With multiple callables, the rightmost callable
     * runs first and its result is passed to the next callable. Invocation is constrained by the composed callables'
     * invocability.
     * @tparam F Callable type before decay.
     * @param[in] function Callable placed at the innermost position.
     * @return An owning pipeable closure.
     * @code{.cpp}
     * auto increment = Sora::Compose([](int value) { return value + 1; });
     * auto input = 3;
     * auto result = increment(input); // 4
     * @endcode
     */
    template<typename F>
    [[nodiscard]] constexpr auto Compose(F&& function) {
        return Fn(std::forward<F>(function));
    }

    /**
     * @brief Compose two or more callables from right to left.
     * @details Callables are stored by decayed value in the resulting nested closure. The rightmost callable receives
     * the original arguments; each outer callable receives the preceding result. Every composition boundary must be
     * invocable with the preceding result.
     * @tparam F Outermost callable type.
     * @tparam G Inner callable type.
     * @tparam Rest Remaining callable types.
     * @param[in] outer Outermost callable.
     * @param[in] inner Callable immediately inside @p outer.
     * @param[in] rest Remaining callables, ordered from outer to inner.
     * @return An owning composed closure.
     * @code{.cpp}
     * auto pipeline = Sora::Compose([](int value) { return value - 3; },
     *                               [](int value) { return value * 2; },
     *                               [](int value) { return value + 1; });
     * auto input = 4;
     * auto result = pipeline(input); // 7
     * @endcode
     */
    template<typename F, typename G, typename... Rest>
    [[nodiscard]] constexpr auto Compose(F&& outer, G&& inner, Rest&&... rest) {
        return Fn(Detail::Composition{std::decay_t<F>(std::forward<F>(outer)),
                                      Compose(std::forward<G>(inner), std::forward<Rest>(rest)...)});
    }

    /**
     * @brief Compose one callable from left to right.
     * @details The single callable is wrapped as a pipeable closure. With multiple callables, the first callable runs
     * on the original arguments and each following callable consumes the preceding result. The callable must satisfy
     * the invocation requirements of the resulting closure.
     * @tparam F Callable type before decay.
     * @param[in] function First callable in the pipeline.
     * @return An owning pipeable closure.
     * @code{.cpp}
     * auto normalize = Sora::Then([](int value) { return value + 1; });
     * auto input = 4;
     * auto result = normalize(input); // 5
     * @endcode
     */
    template<typename F>
    [[nodiscard]] constexpr auto Then(F&& function) {
        return Fn(std::forward<F>(function));
    }

    /**
     * @brief Compose two or more callables from left to right.
     * @details Callables are stored by decayed value. The first callable receives the original arguments; subsequent
     * callables receive one result at a time, so each composition boundary must be invocable.
     * @tparam F First callable type.
     * @tparam G Second callable type.
     * @tparam Rest Remaining callable types.
     * @param[in] first First callable in the pipeline.
     * @param[in] second Callable applied after @p first.
     * @param[in] rest Remaining callables in application order.
     * @return An owning composed closure.
     * @code{.cpp}
     * auto pipeline = Sora::Then([](int value) { return value + 1; },
     *                            [](int value) { return value * 2; });
     * auto input = 3;
     * auto result = pipeline(input); // 8
     * @endcode
     */
    template<typename F, typename G, typename... Rest>
    [[nodiscard]] constexpr auto Then(F&& first, G&& second, Rest&&... rest) {
        return Compose(Then(std::forward<G>(second), std::forward<Rest>(rest)...), std::forward<F>(first));
    }

    /**
     * @brief Compose two Sora adaptor closures from left to right.
     * @details The resulting closure owns both operands and invokes the left closure before the right closure when it
     * is eventually called. This overload is selected only when both operands are adaptor closures.
     * @tparam Left Left closure type.
     * @tparam Right Right closure type.
     * @param[in] left First adaptor closure.
     * @param[in] right Adaptor closure applied after @p left.
     * @return An owning composed adaptor closure.
     * @code{.cpp}
     * auto addOne = Sora::Fn([](int value) { return value + 1; });
     * auto doubleValue = Sora::Fn([](int value) { return value * 2; });
     * auto pipeline = addOne | doubleValue;
     * auto input = 3;
     * auto result = input | pipeline; // 8
     * @endcode
     */
    template<Concept::AdaptorClosure Left, Concept::AdaptorClosure Right>
    [[nodiscard]] constexpr auto operator|(Left&& left, Right&& right) {
        return Then(std::forward<Left>(left), std::forward<Right>(right));
    }

    /**
     * @brief Bind leading arguments using the standard perfect-forwarding binder.
     * @details The function and bound arguments must satisfy @c std::bind_front's construction and invocation
     * requirements. They are stored according to its semantics; the returned closure owns those stored objects and
     * forwards later call arguments when invoked.
     * @tparam F Callable type.
     * @tparam Arguments Bound argument types.
     * @param[in] function Callable to invoke.
     * @param[in] arguments Arguments inserted before invocation-time arguments.
     * @return An owning pipeable bound closure.
     * @code{.cpp}
     * auto addFive = Sora::BindFront(std::plus{}, 5);
     * auto input = 7;
     * auto result = addFive(input); // 12
     * @endcode
     */
    template<typename F, typename... Arguments>
    [[nodiscard]] constexpr auto BindFront(F&& function, Arguments&&... arguments) {
        return Fn(std::bind_front(std::forward<F>(function), std::forward<Arguments>(arguments)...));
    }

    /**
     * @brief Bind trailing arguments using the standard perfect-forwarding binder.
     * @details The function and bound arguments must satisfy @c std::bind_back's construction and invocation
     * requirements. They are stored according to its semantics; the returned closure owns those stored objects and
     * forwards invocation-time arguments before them.
     * @tparam F Callable type.
     * @tparam Arguments Bound argument types.
     * @param[in] function Callable to invoke.
     * @param[in] arguments Arguments inserted after invocation-time arguments.
     * @return An owning pipeable bound closure.
     * @code{.cpp}
     * auto appendSuffix = Sora::BindBack([](std::string text, std::string suffix) {
     *     return text + suffix;
     * }, "!");
     * auto input = std::string{"hello"};
     * auto result = appendSuffix(input); // "hello!"
     * @endcode
     */
    template<typename F, typename... Arguments>
    [[nodiscard]] constexpr auto BindBack(F&& function, Arguments&&... arguments) {
        return Fn(std::bind_back(std::forward<F>(function), std::forward<Arguments>(arguments)...));
    }

    /**
     * @brief Bind leading arguments using the compatibility spelling for @ref BindFront.
     * @details The returned closure has the same ownership and forwarding semantics as @ref BindFront.
     * @tparam F Callable type.
     * @tparam Arguments Bound argument types.
     * @param[in] function Callable to invoke.
     * @param[in] arguments Arguments inserted before invocation-time arguments.
     * @return An owning pipeable bound closure.
     * @code{.cpp}
     * auto addTwo = Sora::Bind(std::plus{}, 2);
     * auto input = 3;
     * auto result = addTwo(input); // 5
     * @endcode
     */
    template<typename F, typename... Arguments>
    [[nodiscard]] constexpr auto Bind(F&& function, Arguments&&... arguments) {
        return BindFront(std::forward<F>(function), std::forward<Arguments>(arguments)...);
    }

} // namespace Sora

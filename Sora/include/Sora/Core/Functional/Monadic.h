/**
 * @file Monadic.h
 * @brief Protocol-based pipeable monadic operations.
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/Functional/Callable.h>

#include <concepts>
#include <utility>

namespace Sora {

    namespace Concept {

        /** @brief Value exposing an engagement query, such as optional or expected. */
        template<typename T>
        concept Fallible = requires(T&& value) {
            { std::forward<T>(value).has_value() } -> std::convertible_to<bool>;
        };

    } // namespace Concept

    template<typename T>
    concept Fallible = Concept::Fallible<T>;

    namespace Detail {

        template<typename F>
        struct AndThenFunction {
            SORA_NO_UNIQUE_ADDRESS F function;

            template<typename Self, typename M>
                requires requires(Self&& self, M&& monad) {
                    std::forward<M>(monad).and_then(std::forward<Self>(self).function);
                }
            constexpr decltype(auto) operator()(this Self&& self, M&& monad) noexcept(noexcept(
                std::forward<M>(monad).and_then(std::forward<Self>(self).function))) {
                return std::forward<M>(monad).and_then(std::forward<Self>(self).function);
            }
        };

        template<typename F>
        struct TransformFunction {
            SORA_NO_UNIQUE_ADDRESS F function;

            template<typename Self, typename M>
                requires requires(Self&& self, M&& monad) {
                    std::forward<M>(monad).transform(std::forward<Self>(self).function);
                }
            constexpr decltype(auto) operator()(this Self&& self, M&& monad) noexcept(noexcept(
                std::forward<M>(monad).transform(std::forward<Self>(self).function))) {
                return std::forward<M>(monad).transform(std::forward<Self>(self).function);
            }
        };

        template<typename F>
        struct OrElseFunction {
            SORA_NO_UNIQUE_ADDRESS F function;

            template<typename Self, typename M>
                requires requires(Self&& self, M&& monad) {
                    std::forward<M>(monad).or_else(std::forward<Self>(self).function);
                }
            constexpr decltype(auto) operator()(this Self&& self, M&& monad) noexcept(noexcept(
                std::forward<M>(monad).or_else(std::forward<Self>(self).function))) {
                return std::forward<M>(monad).or_else(std::forward<Self>(self).function);
            }
        };

        template<typename V>
        struct ValueOrFunction {
            SORA_NO_UNIQUE_ADDRESS V fallback;

            template<typename Self, typename M>
                requires requires(Self&& self, M&& monad) {
                    std::forward<M>(monad).value_or(std::forward<Self>(self).fallback);
                }
            constexpr decltype(auto) operator()(this Self&& self, M&& monad) noexcept(noexcept(
                std::forward<M>(monad).value_or(std::forward<Self>(self).fallback))) {
                return std::forward<M>(monad).value_or(std::forward<Self>(self).fallback);
            }
        };

    } // namespace Detail

    /**
     * @brief Pipe a monadic value into its @c and_then protocol operation.
     * @details The closure owns a decayed callable and forwards the piped monad to its @c and_then member. The
     * operation is lazy until the closure is invoked. The piped monad must expose an @c and_then member accepting
     * the stored callable; no inheritance from @ref Fallible is required.
     * @tparam F Callable type before decay.
     * @param[in] function Callable receiving the engaged value and returning a compatible monadic result.
     * @return An owning pipeable closure.
     * @code{.cpp}
     * auto input = std::expected<int, std::string>{4};
     * auto result = input |
     *               Sora::AndThen([](int value) {
     *                   return std::expected<int, std::string>{value * 2};
     *               });
     * // result.value() == 8
     * @endcode
     */
    template<typename F>
    [[nodiscard]] constexpr auto AndThen(F&& function) {
        return Fn(Detail::AndThenFunction<std::decay_t<F>>{std::forward<F>(function)});
    }

    /**
     * @brief Pipe a monadic value into its @c transform protocol operation.
     * @details The closure owns a decayed callable and forwards it to the monad's @c transform member. An engaged
     * value is mapped while an empty value and its error state are propagated according to the monad's protocol. The
     * piped monad must expose a compatible @c transform member.
     * @tparam F Callable type before decay.
     * @param[in] function Callable mapping the engaged value to a new value.
     * @return An owning pipeable closure.
     * @code{.cpp}
     * auto input = std::optional<int>{4};
     * auto result = input |
     *               Sora::Transform([](int value) { return value + 1; });
     * // result.value() == 5
     * @endcode
     */
    template<typename F>
    [[nodiscard]] constexpr auto Transform(F&& function) {
        return Fn(Detail::TransformFunction<std::decay_t<F>>{std::forward<F>(function)});
    }

    /**
     * @brief Pipe a monadic value into its @c or_else protocol operation.
     * @details The closure owns a decayed recovery callable and forwards it to the monad's @c or_else member. The
     * recovery callable is considered only when the monad is empty or contains an error, as defined by that protocol.
     * The piped monad must expose a compatible @c or_else member.
     * @tparam F Callable type before decay.
     * @param[in] function Callable producing or transforming the failure state.
     * @return An owning pipeable closure.
     * @code{.cpp}
     * auto input = std::optional<int>{};
     * auto recovered = input |
     *                  Sora::OrElse([] { return std::optional<int>{7}; });
     * // recovered.value() == 7
     * @endcode
     */
    template<typename F>
    [[nodiscard]] constexpr auto OrElse(F&& function) {
        return Fn(Detail::OrElseFunction<std::decay_t<F>>{std::forward<F>(function)});
    }

    /**
     * @brief Extract a monadic value or return a fallback through @c value_or.
     * @details The fallback is owned by the returned closure and is forwarded to the monad's @c value_or member at
     * invocation. The piped monad must expose a compatible @c value_or member. Whether the fallback is copied or
     * moved follows the value category of the closure and the monad's protocol.
     * @tparam V Fallback value type before decay.
     * @param[in] fallback Value returned when the monad is disengaged or otherwise has no value.
     * @return An owning pipeable closure whose result is the monad's @c value_or result.
     * @code{.cpp}
     * auto input = std::optional<int>{};
     * auto value = input | Sora::ValueOr(42); // value == 42
     * @endcode
     */
    template<typename V>
    [[nodiscard]] constexpr auto ValueOr(V&& fallback) {
        return Fn(Detail::ValueOrFunction<std::decay_t<V>>{std::forward<V>(fallback)});
    }

} // namespace Sora

#pragma once

#include <Sora/Core/Concurrency/CoroutineTraits.h>

#include <utility>

namespace Sora {

    namespace Concurrency {

        template<Sora::Concept::AwaiterOrAwaitable T>
        auto GetAwaiter(T&& awaitable, decltype(std::ignore) = {}) noexcept {
            if constexpr (Sora::Concept::HasCoAwaitOperatorMember<T>) {
                return std::forward<T>(awaitable).operator co_await();
            } else if constexpr (Sora::Concept::HasADLCoAwaitOperator<T>) {
                return operator co_await(std::forward<T>(awaitable));
            } else {
                return std::forward<T>(awaitable);
            }
        }
        static_assert(std::is_same_v<decltype(GetAwaiter(std::suspend_always{})), std::suspend_always>);

        template<typename T, typename Promise>
        auto GetAwaiter(T&& awaitable, Promise* promise) noexcept {
            SORA_PRE(promise != nullptr, "Promise pointer must not be null.");
            if constexpr (requires { promise->await_transform(std::forward<T>(awaitable)).operator co_await(); }) {
                return promise->await_transform(std::forward<T>(awaitable)).operator co_await();
            } else if constexpr (requires {
                                     operator co_await(promise->await_transform(std::forward<T>(awaitable)));
                                 }) {
                return operator co_await(promise->await_transform(std::forward<T>(awaitable)));
            } else {
                return promise->await_transform(std::forward<T>(awaitable));
            }
        }

    } // namespace Concurrency

} // namespace Sora
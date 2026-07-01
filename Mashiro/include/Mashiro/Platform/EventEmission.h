/**
 * @file EventEmission.h
 * @brief Platform-thread event emission primitives.
 * @ingroup Platform
 */
#pragma once

#include "Mashiro/Platform/SystemEvent.h"

#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>

namespace Mashiro::Platform {

    /**
     * @brief Callable contract for synchronous platform event consumers.
     *
     * A consumer receives an owning @ref SystemEvent rvalue from an ingress path and must not throw. The Platform
     * thread performs translation, bookkeeping, and broadcast in one loop, so backend-local recovery is impossible.
     */
    template<class F>
    concept SystemEventConsumer = std::invocable<F&, SystemEvent&&> && std::is_nothrow_invocable_v<F&, SystemEvent&&>;

    /**
     * @brief Non-owning view over a @ref SystemEventConsumer.
     *
     * This is the platform-domain spelling of a C++26 function_ref: two machine words, no allocation, no ownership,
     * and no template body exposure from OS-specific translation units. The constructor takes an lvalue callable so
     * temporary lambdas cannot dangle.
     */
    class SystemEventConsumerRef final {
    public:
        SystemEventConsumerRef() = delete;

        template<class F>
            requires SystemEventConsumer<F> && (!std::same_as<std::remove_cvref_t<F>, SystemEventConsumerRef>) &&
                     (!std::is_const_v<F>)
        constexpr SystemEventConsumerRef(F& consumer) noexcept
            : object_(std::addressof(consumer)), consume_([](void* object, SystemEvent&& event) noexcept {
                  using Consumer = std::remove_reference_t<F>;
                  (*static_cast<Consumer*>(object))(std::move(event));
              }) {}

        /** @brief Forward @p event to the referenced consumer. */
        void operator()(SystemEvent&& event) const noexcept {
            consume_(object_, std::move(event));
        }

    private:
        using ConsumeFn = void (*)(void*, SystemEvent&&) noexcept;

        void* object_;
        ConsumeFn consume_;
    };

    /** @brief Construct a window-scoped payload for @p id. */
    template<Traits::Event::WindowScoped Payload>
        requires std::default_initializable<Payload>
    [[nodiscard]] constexpr Payload MakeWindowPayload(WindowId id) noexcept {
        Payload payload{};
        payload.windowId = id;
        return payload;
    }

    /** @brief Wrap @p payload as @ref SystemEvent and emit it to @p consume. */
    template<Traits::Event::SystemEventPayload Payload>
    void EmitSystemEvent(SystemEventConsumerRef consume, Payload payload) noexcept {
        consume(SystemEvent{std::move(payload)});
    }

} // namespace Mashiro::Platform
/**
 * @file EventPort.h
 * @brief Experimental event port decoupled from the BaseUnknown object model.
 * @ingroup Experimental
 *
 * @details This header models event participation as four orthogonal capabilities: an event port, static event schema,
 * object identity, and an optional lifetime token. @ref EventPort itself is a plain final class and never inherits from
 * @c BaseUnknown. COM participation is provided by a separate adaptor header, so the event model can be tested without
 * making COM/data-extension storage part of its core semantics.
 */
#pragma once

#include "Sora/Core/Hash.h"
#include "Sora/Core/Traits/InheritanceTraits.h"
#include "Sora/Core/Traits/ScopeTraits.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <type_traits>
#include <tuple>
#include <utility>
#include <vector>

#include <exec/start_detached.hpp>
#include <stdexec/execution.hpp>

namespace Sora::Experimental {

    class EventPort;

    /** @brief Marker accepted in an event declaration list to mean every event payload. */
    struct AllEvents {};

    /** @brief Placeholder event type used by schema predicates when no concrete event is supplied. */
    struct UnspecifiedEvent {};

    /** @brief Event selector used by runtime subscription APIs to request type-erased callbacks. */
    struct AnyEventTag {};

    /** @brief Event selector constant for type-erased subscriptions. */
    inline constexpr AnyEventTag AnyEvent{};

    /** @brief Default callback tag used when a relation does not need a specific callback resource. */
    struct DefaultCallbackTag {};

    /** @brief Compile-time event declaration list. */
    template<typename... Ts>
    using EventList = Sora::Traits::TypeList<Ts...>;

    /** @brief Stable runtime identifier for an event payload or callback tag type. */
    struct EventId {
        uint64_t value{}; /**< Reflected type-name hash. */

        constexpr bool operator==(const EventId&) const noexcept = default;
        constexpr auto operator<=>(const EventId&) const noexcept = default;
    };

    /** @brief Callback flow-control decision. */
    enum class EventFlow : uint8_t {
        Continue, /**< Continue delivering the current emission. */
        Stop,     /**< Stop after the current callback. */
    };

    /** @brief Dispatch budget attached to an event relation. */
    struct WoreBudget {
        static constexpr uint32_t kPersistent = 0xffffffffu; /**< Non-wearing relation sentinel. */
        uint32_t value{kPersistent};                         /**< Remaining successful dispatches. */
    };

    /** @brief Relation budget that never wears down. */
    inline constexpr WoreBudget Persistent{WoreBudget::kPersistent};

    /** @brief Relation budget that wears out after one successful dispatch. */
    inline constexpr WoreBudget OneShot{1};

    /** @brief Event relation options. */
    struct EventOptions {
        WoreBudget budget{Persistent}; /**< Successful-dispatch budget. */
        EventId callbackId{};          /**< Optional callback tag identifier. */
        int32_t priority{};            /**< Larger priority dispatches first. */
    };

    namespace Traits {

        /** @brief Reflected type-name hash for @p T. */
        template<typename T>
        inline constexpr EventId EventIdOf = [] consteval {
            Sora::Hashing::Fnv1a64State state{};
            constexpr std::string_view name = Sora::Meta::ScopeChainIdentifierOf(^^std::remove_cvref_t<T>, "::");
            auto bytes = Sora::Meta::BytesOf(name);
            state.Feed(std::span<const std::byte>{bytes.data(), bytes.size()});
            return EventId{state.Finalize()};
        }();

    } // namespace Traits
    /** @brief Trace phase for observing delivery without becoming an event callback. */
    enum class EventTracePhase : uint8_t {
        ReceiveBegin,
        CallbackScheduled,
        CallbackBegin,
        CallbackEnd,
        CallbackCanceled,
        ReceiveEnd,
    };

    /** @brief Type-erased object reference used by event contexts and traces. */
    struct EventObjectRef {
        void* object{};              /**< Borrowed object pointer. */
        EventId type{};              /**< Stable runtime type identifier associated with @p object. */
        std::string_view typeName{}; /**< Stable display name for diagnostics and trace consumers. */

        /** @brief Return whether this reference is non-empty. */
        [[nodiscard]] explicit constexpr operator bool() const noexcept { return object != nullptr; }

        /** @brief Cast this reference to @p T. The caller must know the static relation selected the right type. */
        template<typename T>
        [[nodiscard]] T& As() const noexcept {
            assert(object != nullptr);
            return *static_cast<T*>(object);
        }

        /** @brief Build a reference from a concrete object. */
        template<typename T>
        [[nodiscard]] static EventObjectRef Of(T& object) noexcept {
            using U = std::remove_cvref_t<T>;
            return EventObjectRef{.object = std::addressof(object),
                                  .type = Traits::EventIdOf<U>,
                                  .typeName = Sora::Meta::ScopeChainIdentifierOf(^^U, "::")};
        }
    };

    /** @brief Type-erased receive context shared by typed callbacks, any-event callbacks, and trace hooks. */
    struct EventContextBase {
        EventObjectRef receiver{}; /**< Object selected as receiver. */
        EventObjectRef emitter{};  /**< Object that emitted the event. */
        EventId eventId{};         /**< Reflected event payload identifier. */
        EventId callbackId{};      /**< Reflected callback tag identifier. */
        uint64_t subscription{};   /**< Subscription identifier currently being evaluated. */
        uint64_t sequence{};       /**< Monotone emission sequence on the emitter port. */
    };

    /** @brief Strongly typed event receive context. */
    template<typename Event>
    struct EventContext : EventContextBase {
        const Event& event; /**< Event payload being delivered. */
    };

    /** @brief Trace hook payload produced by an event port. */
    struct EventTrace {
        EventTracePhase phase{};   /**< Current receive phase. */
        EventObjectRef receiver{}; /**< Receiver object, empty when a deferred act observes cancellation. */
        EventObjectRef emitter{};  /**< Emitter object, empty when a deferred act observes cancellation. */
        EventId eventId{};         /**< Reflected event payload identifier. */
        EventId callbackId{};      /**< Reflected callback tag identifier. */
        uint64_t subscription{};   /**< Subscription identifier, or zero for port-wide phases. */
        uint64_t sequence{};       /**< Monotone emission sequence on the emitter port. */
    };

    namespace Concept {

        /** @brief Copyable class payload that can be retained by deferred event delivery. */
        template<typename T>
        concept EventPayload =
            std::is_class_v<std::remove_cvref_t<T>> && !std::same_as<std::remove_cvref_t<T>, AllEvents> &&
            !std::same_as<std::remove_cvref_t<T>, UnspecifiedEvent> && std::copy_constructible<std::remove_cvref_t<T>>;

        /** @brief Class tag usable as a callback-resource selector. */
        template<typename T>
        concept CallbackTagClass = std::is_class_v<std::remove_cvref_t<T>>;

    } // namespace Concept

    namespace Meta {

        /** @brief Return whether @p list is a TypeList declaration for events. */
        consteval bool IsEventTypeList(std::meta::info list) {
            return Sora::Meta::IsTypeList(list);
        }

        /** @brief Return whether @p list is a TypeList declaration for callback tags. */
        consteval bool IsCallbackTagList(std::meta::info list) {
            return Sora::Meta::IsTypeList(list);
        }

    } // namespace Meta

    namespace Detail {

        template<typename>
        inline constexpr bool kDependentFalse = false;

        /** @brief Return a merged TypeList named @p name along @p type's single-inheritance chain. */
        template<std::meta::info Validator>
        consteval std::meta::info DeclaredTypeListInfoOf(std::meta::info type, std::string_view name) {
            type = std::meta::dealias(std::meta::remove_cvref(type));
            if (!std::meta::is_class_type(type)) {
                throw std::define_static_string("Experimental::DeclaredTypeListInfoOf: target is not a class type.");
            }

            std::vector<std::meta::info> result;
            auto chain = Sora::Meta::InheritanceChainOf(type);
            for (auto scope : chain | std::views::reverse) {
                auto list = Sora::Meta::FindDirectTypeMemberOf(scope, name);
                if (list == std::meta::info{}) {
                    continue;
                }
                if (!Sora::Meta::IsTypeList(list) || !Sora::Meta::Invoke<Validator>(list)) {
                    throw std::define_static_string(
                        std::format("Experimental::DeclaredTypeListInfoOf: '{}' is not a valid '{}'.",
                                    Sora::Meta::DisplayStringOf(list), name));
                }
                Sora::Meta::AppendTypeListUnique(result, list);
            }
            return std::meta::substitute(^^Sora::Traits::TypeList, result);
        }

    } // namespace Detail

    namespace Traits {

        /** @brief Type-list of event payloads emitted by @p T after inherited declarations are merged. */
        template<typename T>
            requires std::is_class_v<std::remove_cvref_t<T>>
        using EmittedEventsOf =
            typename [:Detail::DeclaredTypeListInfoOf<^^Meta::IsEventTypeList>(^^std::remove_cvref_t<T>, "Emits"):];

        /** @brief Type-list of event payloads accepted by @p T after inherited declarations are merged. */
        template<typename T>
            requires std::is_class_v<std::remove_cvref_t<T>>
        using AcceptedEventsOf =
            typename [:Detail::DeclaredTypeListInfoOf<^^Meta::IsEventTypeList>(^^std::remove_cvref_t<T>, "Accepts"):];

        /** @brief Type-list of callback tags allowed by @p T after inherited declarations are merged. */
        template<typename T>
            requires std::is_class_v<std::remove_cvref_t<T>>
        using CallbackTagsOf = typename
            [:Detail::DeclaredTypeListInfoOf<^^Meta::IsCallbackTagList>(^^std::remove_cvref_t<T>, "Callbacks"):];

        /** @brief Return whether @p List permits event payload @p Event, honoring @ref AllEvents. */
        template<typename List, typename Event = UnspecifiedEvent>
        inline constexpr bool EventListContains =
            (std::same_as<std::remove_cvref_t<Event>, UnspecifiedEvent> && List::size != 0) ||
            Sora::Traits::Contains<List, std::remove_cvref_t<Event>> || Sora::Traits::Contains<List, AllEvents>;

        /** @brief Return whether @p T can emit @p Event. */
        template<typename T, typename Event = UnspecifiedEvent>
        inline constexpr bool CanEmitEvent = std::is_class_v<std::remove_cvref_t<T>> &&
                                             (std::same_as<std::remove_cvref_t<Event>, UnspecifiedEvent> ||
                                              Concept::EventPayload<std::remove_cvref_t<Event>>) &&
                                             EventListContains<Traits::EmittedEventsOf<std::remove_cvref_t<T>>, Event>;

        /** @brief Return whether @p T can receive @p Event. */
        template<typename T, typename Event = UnspecifiedEvent>
        inline constexpr bool CanAcceptEvent =
            std::is_class_v<std::remove_cvref_t<T>> &&
            (std::same_as<std::remove_cvref_t<Event>, UnspecifiedEvent> ||
             Concept::EventPayload<std::remove_cvref_t<Event>>) &&
            EventListContains<Traits::AcceptedEventsOf<std::remove_cvref_t<T>>, Event>;

        /** @brief Return whether @p T can attach a callback tagged by @p CallbackTag. */
        template<typename T, typename CallbackTag>
        inline constexpr bool CanAttachCallback =
            std::is_class_v<std::remove_cvref_t<T>> && Concept::CallbackTagClass<std::remove_cvref_t<CallbackTag>> &&
            Sora::Traits::Contains<Traits::CallbackTagsOf<std::remove_cvref_t<T>>, std::remove_cvref_t<CallbackTag>>;

    } // namespace Traits

    /** @brief Copyable lifetime token retained by deferred event delivery. */
    class EventLifetimeToken {
    public:
        /** @brief Construct an empty lifetime token. */
        EventLifetimeToken() noexcept = default;

        /** @brief Construct a borrowed token. This does not extend object lifetime. */
        [[nodiscard]] static EventLifetimeToken Borrowed(void* object) noexcept {
            return EventLifetimeToken{object, nullptr};
        }

        /** @brief Construct a token that retains @p object through an arbitrary shared holder. */
        [[nodiscard]] static EventLifetimeToken Retained(void* object, std::shared_ptr<void> holder) noexcept {
            return EventLifetimeToken{object, std::move(holder)};
        }

        /** @brief Return the retained or borrowed object pointer. */
        [[nodiscard]] void* Get() const noexcept { return object_; }

        /** @brief Return whether this token carries a non-null object pointer. */
        [[nodiscard]] explicit operator bool() const noexcept { return object_ != nullptr; }

        /** @brief Return whether this token owns a lifetime holder. */
        [[nodiscard]] bool Retaining() const noexcept { return holder_ != nullptr; }

    private:
        EventLifetimeToken(void* object, std::shared_ptr<void> holder) noexcept
            : object_(object), holder_(std::move(holder)) {}

        void* object_{};
        std::shared_ptr<void> holder_{};
    };

    namespace Detail {

        template<typename T>
        concept HasEventPort = requires(T& object) {
            { EventPortOf(object) } -> std::same_as<EventPort&>;
        } || requires(T& object) {
            { object.EventPort() } -> std::same_as<EventPort&>;
        } || requires(T& object) {
            { object.Events() } -> std::same_as<EventPort&>;
        } || requires(T& object) { requires std::same_as<std::remove_cvref_t<decltype(object.events)>, EventPort>; };

    } // namespace Detail

    namespace Detail {

        /** @brief CPO functor that implements @c Events(object) -> @ref EventPort. */
        struct EventsFn {
            template<typename T>
            [[nodiscard]] EventPort& operator()(T& object) const noexcept(noexcept(Dispatch(object))) {
                return Dispatch(object);
            }

        private:
            template<typename T>
            [[nodiscard]] static EventPort& Dispatch(T& object) {
                if constexpr (requires { EventPortOf(object); }) {
                    return EventPortOf(object);
                } else if constexpr (requires { object.EventPort(); }) {
                    return object.EventPort();
                } else if constexpr (requires { object.Events(); }) {
                    return object.Events();
                } else if constexpr (requires { object.events; }) {
                    static_assert(std::same_as<std::remove_cvref_t<decltype(object.events)>, EventPort>,
                                  "A member named 'events' must be Sora::Experimental::EventPort.");
                    return object.events;
                } else {
                    static_assert(Detail::kDependentFalse<T>, "Type does not expose an EventPort.");
                }
            }
        };

        /** @brief CPO functor that implements @c EventLifetime(object) -> @ref EventLifetimeToken. */
        struct EventLifetimeFn {
            template<typename T>
            [[nodiscard]] EventLifetimeToken operator()(T& object) const noexcept(noexcept(Dispatch(object))) {
                return Dispatch(object);
            }

        private:
            template<typename T>
            [[nodiscard]] static EventLifetimeToken Dispatch(T& object) {
                if constexpr (requires { EventLifetimeOf(object); }) {
                    return EventLifetimeOf(object);
                } else if constexpr (requires { object.EventLifetime(); }) {
                    return object.EventLifetime();
                } else if constexpr (requires { object.shared_from_this(); }) {
                    auto holder = object.shared_from_this();
                    return EventLifetimeToken::Retained(std::addressof(object), std::static_pointer_cast<void>(holder));
                } else {
                    return EventLifetimeToken::Borrowed(std::addressof(object));
                }
            }
        };

        /** @brief CPO functor that implements @c EventRef(object) -> @ref EventObjectRef. */
        struct EventRefFn {
            template<typename T>
            [[nodiscard]] EventObjectRef operator()(T& object) const noexcept(noexcept(Dispatch(object))) {
                return Dispatch(object);
            }

        private:
            template<typename T>
            [[nodiscard]] static EventObjectRef Dispatch(T& object) {
                if constexpr (requires { EventRefOf(object); }) {
                    return EventRefOf(object);
                } else {
                    return EventObjectRef::Of(object);
                }
            }
        };

    } // namespace Detail

    /** @brief Customisation-point object returning an object's event port. */
    inline constexpr Detail::EventsFn Events{};

    /** @brief Customisation-point object returning an object's event lifetime token. */
    inline constexpr Detail::EventLifetimeFn EventLifetime{};

    /** @brief Customisation-point object returning an object's event reference. */
    inline constexpr Detail::EventRefFn EventRef{};

    namespace Concept {

        /** @brief Object type that exposes an event port through the @ref Events CPO. */
        template<typename T>
        concept EventParticipant =
            std::is_class_v<std::remove_cvref_t<T>> && Detail::HasEventPort<T> && requires(T& object) {
                { Sora::Experimental::Events(object) } -> std::same_as<EventPort&>;
                { Sora::Experimental::EventLifetime(object) } -> std::same_as<EventLifetimeToken>;
                { Sora::Experimental::EventRef(object) } -> std::same_as<EventObjectRef>;
            };

        /** @brief Class type that is either a concrete payload or the unspecified schema sentinel. */
        template<typename T>
        concept EventClass = EventPayload<T> || std::same_as<std::remove_cvref_t<T>, UnspecifiedEvent>;

        /** @brief Event participant whose emitted-event list permits @p Event. */
        template<typename T, typename Event = UnspecifiedEvent>
        concept EventEmitter = EventParticipant<T> && EventClass<Event> && Traits::CanEmitEvent<T, Event>;

        /** @brief Event participant whose accepted-event list permits @p Event. */
        template<typename T, typename Event = UnspecifiedEvent>
        concept EventReceiver = EventParticipant<T> && EventClass<Event> && Traits::CanAcceptEvent<T, Event>;

        /** @brief Event participant whose callback-tag list permits @p CallbackTag. */
        template<typename T, typename CallbackTag>
        concept CallbackAttachable =
            EventParticipant<T> && CallbackTagClass<CallbackTag> && Traits::CanAttachCallback<T, CallbackTag>;

    } // namespace Concept

    /** @brief Stable handle for an explicit event relation. */
    class EventLink {
    public:
        /** @brief Opaque relation state shared by handles and the owning port. */
        struct State;

        EventLink() noexcept = default;

        /** @brief Return whether this handle still refers to an active relation. */
        [[nodiscard]] bool Active() const noexcept;

    private:
        explicit EventLink(std::weak_ptr<State> state) noexcept : state_(std::move(state)) {}

        friend class EventPort;
        friend void Drop(const EventLink&) noexcept;
        friend void Suspend(const EventLink&) noexcept;
        friend void Resume(const EventLink&) noexcept;
        friend void SetBudget(const EventLink&, WoreBudget) noexcept;

        std::weak_ptr<State> state_{};
    };

    /** @brief Remove @p link from future dispatch without mutating relation storage immediately. */
    void Drop(const EventLink& link) noexcept;

    /** @brief Pause @p link without consuming its dispatch budget. */
    void Suspend(const EventLink& link) noexcept;

    /** @brief Resume @p link after a previous @ref Suspend. */
    void Resume(const EventLink& link) noexcept;

    /** @brief Replace @p link's successful-dispatch budget. */
    void SetBudget(const EventLink& link, WoreBudget budget) noexcept;

    /** @brief RAII-capable handle for a trace hook. */
    class EventTraceHook {
    public:
        /** @brief Opaque trace state shared by handles and the owning port. */
        struct State;

        EventTraceHook() noexcept = default;

        /** @brief Detach this trace hook if its port still exists. */
        void Cancel() noexcept;

        /** @brief Return whether this hook is still active. */
        [[nodiscard]] bool Active() const noexcept;

    private:
        explicit EventTraceHook(std::weak_ptr<State> state) noexcept : state_(std::move(state)) {}

        friend class EventPort;

        std::weak_ptr<State> state_{};
    };

    namespace Detail {

        using ErasedCallback = std::move_only_function<EventFlow(EventContextBase&, const void*)>;
        using ErasedTrace = std::move_only_function<void(const EventTrace&)>;

        struct SubscriptionState {
            std::atomic<uint32_t> remaining{WoreBudget::kPersistent};
            std::atomic_bool active{true};
            std::atomic_bool suspended{false};
            std::atomic_bool canceled{false};
        };

        struct TraceState {
            std::atomic_bool active{true};
        };

    } // namespace Detail

    struct EventLink::State : Detail::SubscriptionState {};
    struct EventTraceHook::State : Detail::TraceState {};

    /** @brief Return whether @p state is active and dispatchable before budget consumption. */
    [[nodiscard]] inline bool Dispatchable(const Detail::SubscriptionState& state) noexcept {
        return state.active.load(std::memory_order_acquire) && !state.suspended.load(std::memory_order_acquire) &&
               !state.canceled.load(std::memory_order_acquire) && state.remaining.load(std::memory_order_acquire) != 0;
    }

    /** @brief Consume one dispatch budget slot when @p state is active, returning whether callback may run. */
    [[nodiscard]] inline bool TryBeginDispatch(Detail::SubscriptionState& state) noexcept {
        if (!Dispatchable(state)) {
            return false;
        }

        uint32_t remaining = state.remaining.load(std::memory_order_acquire);
        if (remaining == WoreBudget::kPersistent) {
            return true;
        }
        while (remaining != 0) {
            if (state.remaining.compare_exchange_weak(remaining, remaining - 1, std::memory_order_acq_rel,
                                                      std::memory_order_acquire)) {
                if (remaining == 1) {
                    state.active.store(false, std::memory_order_release);
                }
                return true;
            }
        }
        return false;
    }

    inline bool EventLink::Active() const noexcept {
        auto state = state_.lock();
        return state && Dispatchable(*state);
    }

    inline void Drop(const EventLink& link) noexcept {
        if (auto state = link.state_.lock()) {
            state->active.store(false, std::memory_order_release);
            state->canceled.store(true, std::memory_order_release);
        }
    }

    inline void Suspend(const EventLink& link) noexcept {
        if (auto state = link.state_.lock()) {
            state->suspended.store(true, std::memory_order_release);
        }
    }

    inline void Resume(const EventLink& link) noexcept {
        if (auto state = link.state_.lock()) {
            state->suspended.store(false, std::memory_order_release);
        }
    }

    inline void SetBudget(const EventLink& link, WoreBudget budget) noexcept {
        if (auto state = link.state_.lock()) {
            state->remaining.store(budget.value, std::memory_order_release);
            state->active.store(budget.value != 0, std::memory_order_release);
            state->canceled.store(budget.value == 0, std::memory_order_release);
        }
    }

    inline void EventTraceHook::Cancel() noexcept {
        if (auto state = state_.lock()) {
            state->active.store(false, std::memory_order_release);
        }
    }

    inline bool EventTraceHook::Active() const noexcept {
        auto state = state_.lock();
        return state && state->active.load(std::memory_order_acquire);
    }

    namespace Detail {

        template<typename Result>
        [[nodiscard]] constexpr EventFlow FlowOf(Result&& result) noexcept {
            if constexpr (std::same_as<std::remove_cvref_t<Result>, EventFlow>) {
                return result;
            } else {
                return EventFlow::Continue;
            }
        }

        template<typename Callable, typename... Args>
        [[nodiscard]] EventFlow InvokeAndFlow(Callable& callable, Args&&... args) {
            if constexpr (std::same_as<void, std::invoke_result_t<Callable&, Args...>>) {
                std::invoke(callable, std::forward<Args>(args)...);
                return EventFlow::Continue;
            } else {
                return FlowOf(std::invoke(callable, std::forward<Args>(args)...));
            }
        }

        template<typename Event, typename Receiver, typename Emitter>
        [[nodiscard]] EventFlow InvokeDefaultReceiver(EventContext<Event>& context) {
            Receiver& receiver = context.receiver.template As<Receiver>();
            Emitter& emitter = context.emitter.template As<Emitter>();
            if constexpr (requires { receiver.On(context.event, emitter); }) {
                if constexpr (std::same_as<void, decltype(receiver.On(context.event, emitter))>) {
                    receiver.On(context.event, emitter);
                    return EventFlow::Continue;
                } else {
                    return FlowOf(receiver.On(context.event, emitter));
                }
            } else if constexpr (requires { receiver.On(context.event); }) {
                if constexpr (std::same_as<void, decltype(receiver.On(context.event))>) {
                    receiver.On(context.event);
                    return EventFlow::Continue;
                } else {
                    return FlowOf(receiver.On(context.event));
                }
            } else if constexpr (requires { receiver.On(context); }) {
                if constexpr (std::same_as<void, decltype(receiver.On(context))>) {
                    receiver.On(context);
                    return EventFlow::Continue;
                } else {
                    return FlowOf(receiver.On(context));
                }
            } else if constexpr (requires { receiver.On(); }) {
                if constexpr (std::same_as<void, decltype(receiver.On())>) {
                    receiver.On();
                    return EventFlow::Continue;
                } else {
                    return FlowOf(receiver.On());
                }
            } else {
                static_assert(kDependentFalse<Receiver>, "Receiver has no compatible On handler.");
            }
        }

        template<typename Event, typename Receiver, typename Emitter, typename Handler>
        [[nodiscard]] EventFlow InvokeTypedHandler(Handler& handler, EventContext<Event>& context) {
            Receiver& receiver = context.receiver.template As<Receiver>();
            Emitter& emitter = context.emitter.template As<Emitter>();
            if constexpr (std::invocable<Handler&, Receiver&, const Event&, Emitter&, EventContext<Event>&>) {
                return InvokeAndFlow(handler, receiver, context.event, emitter, context);
            } else if constexpr (std::invocable<Handler&, Receiver&, const Event&, Emitter&>) {
                return InvokeAndFlow(handler, receiver, context.event, emitter);
            } else if constexpr (std::invocable<Handler&, Receiver&, const Event&>) {
                return InvokeAndFlow(handler, receiver, context.event);
            } else if constexpr (std::invocable<Handler&, const Event&, Emitter&>) {
                return InvokeAndFlow(handler, context.event, emitter);
            } else if constexpr (std::invocable<Handler&, const Event&, EventContext<Event>&>) {
                return InvokeAndFlow(handler, context.event, context);
            } else if constexpr (std::invocable<Handler&, const Event&>) {
                return InvokeAndFlow(handler, context.event);
            } else if constexpr (std::invocable<Handler&, EventContext<Event>&>) {
                return InvokeAndFlow(handler, context);
            } else if constexpr (std::invocable<Handler&>) {
                return InvokeAndFlow(handler);
            } else {
                static_assert(kDependentFalse<Handler>, "Typed event handler has no compatible signature.");
            }
        }

        template<typename Receiver, typename Emitter, typename Handler>
        [[nodiscard]] EventFlow InvokeAnyHandler(Handler& handler, EventContextBase& context) {
            Receiver& receiver = context.receiver.template As<Receiver>();
            Emitter& emitter = context.emitter.template As<Emitter>();
            if constexpr (std::invocable<Handler&, Receiver&, Emitter&, EventContextBase&>) {
                return InvokeAndFlow(handler, receiver, emitter, context);
            } else if constexpr (std::invocable<Handler&, Receiver&, Emitter&>) {
                return InvokeAndFlow(handler, receiver, emitter);
            } else if constexpr (std::invocable<Handler&, Receiver&, EventContextBase&>) {
                return InvokeAndFlow(handler, receiver, context);
            } else if constexpr (std::invocable<Handler&, EventContextBase&>) {
                return InvokeAndFlow(handler, context);
            } else if constexpr (std::invocable<Handler&>) {
                return InvokeAndFlow(handler);
            } else {
                static_assert(kDependentFalse<Handler>, "Any-event handler has no compatible signature.");
            }
        }

        struct SubscriptionRecord {
            std::shared_ptr<EventLink::State> state{};
            ErasedCallback callback{};
            EventObjectRef receiver{};
            EventLifetimeToken receiverLifetime{};
            EventId eventId{};
            EventId callbackId{};
            uint64_t id{};
            uint64_t order{};
            int32_t priority{};
            bool anyEvent{};
        };

        struct TraceRecord {
            std::shared_ptr<EventTraceHook::State> state{};
            ErasedTrace callback{};
            uint64_t id{};
            uint64_t order{};
        };

        struct SubscriptionSnapshot {
            std::vector<std::shared_ptr<SubscriptionRecord>> records{};
        };

        struct TraceSnapshot {
            std::vector<std::shared_ptr<TraceRecord>> records{};
        };

    } // namespace Detail

    /** @brief Immutable data shared by every deferred delivery act in one emission. */
    template<Concept::EventPayload Event>
    struct EmissionFrame {
        Event event;                                           /**< Retained event payload. */
        EventObjectRef emitter{};                              /**< Retained emitter reference. */
        EventLifetimeToken emitterLifetime{};                  /**< Emitter lifetime token. */
        std::shared_ptr<const Detail::TraceSnapshot> traces{}; /**< Trace snapshot for this emission. */
        uint64_t sequence{};                                   /**< Emission sequence number. */
    };

    /** @brief One move-only deferred callback act selected from an emission. */
    template<Concept::EventPayload Event>
    class EventDeliveryAct {
    public:
        EventDeliveryAct() noexcept = default;
        EventDeliveryAct(const EventDeliveryAct&) = delete;
        EventDeliveryAct& operator=(const EventDeliveryAct&) = delete;
        EventDeliveryAct(EventDeliveryAct&&) noexcept = default;
        EventDeliveryAct& operator=(EventDeliveryAct&&) noexcept = default;

        /** @brief Return whether this act carries a callback to invoke. */
        [[nodiscard]] explicit operator bool() const noexcept { return frame_ && receiver_ && record_; }

        /** @brief Invoke the retained callback if the relation has not been canceled or suspended. */
        void operator()();

    private:
        friend class EventPort;

        EventDeliveryAct(std::shared_ptr<const EmissionFrame<Event>> frame, EventLifetimeToken receiver,
                         std::shared_ptr<Detail::SubscriptionRecord> record) noexcept
            : frame_(std::move(frame)), receiver_(std::move(receiver)), record_(std::move(record)) {}

        void Trace(EventTracePhase phase, EventObjectRef receiver, EventObjectRef emitter) const;

        std::shared_ptr<const EmissionFrame<Event>> frame_{};
        EventLifetimeToken receiver_{};
        std::shared_ptr<Detail::SubscriptionRecord> record_{};
    };

    /** @brief Build a lazy sender that schedules @p act on @p scheduler and then invokes it. */
    template<typename Scheduler, typename Act>
        requires stdexec::scheduler<std::remove_cvref_t<Scheduler>> && std::move_constructible<Act> &&
                 std::invocable<Act&>
    [[nodiscard]] auto EventSender(Scheduler&& scheduler, Act act) {
        return stdexec::schedule(std::forward<Scheduler>(scheduler)) | stdexec::then(std::move(act));
    }

    /** @brief Start @p act detached on @p scheduler. */
    template<typename Scheduler, typename Act>
        requires stdexec::scheduler<std::remove_cvref_t<Scheduler>> && std::move_constructible<Act> &&
                 std::invocable<Act&>
    void StartEvent(Scheduler&& scheduler, Act act) {
        exec::start_detached(EventSender(std::forward<Scheduler>(scheduler), std::move(act)));
    }

    /** @brief Plain event relation container. */
    class EventPort final {
    public:
        /** @brief Construct an empty event port. */
        EventPort()
            : subscriptions_(std::make_shared<Detail::SubscriptionSnapshot>()),
              traces_(std::make_shared<Detail::TraceSnapshot>()) {}

        EventPort(const EventPort&) = delete;
        EventPort& operator=(const EventPort&) = delete;

        /** @brief Cancel outgoing and incoming relations attached to this port. */
        ~EventPort() noexcept { CancelAll(); }

        /** @brief Attach a typed relation owned by this port's emitter. */
        template<Concept::EventPayload Event, typename Receiver, typename Emitter, typename Callback>
        [[nodiscard]] EventLink ListenTyped(Receiver& receiver, Emitter&, Callback&& callback,
                                            EventOptions options = {}) {
            using R = std::remove_cvref_t<Receiver>;
            using E = std::remove_cvref_t<Emitter>;
            using F = std::remove_cvref_t<Callback>;
            static_assert(std::move_constructible<F>);

            auto typedCallback = F(std::forward<Callback>(callback));
            auto state = std::make_shared<EventLink::State>();
            state->remaining.store(options.budget.value, std::memory_order_relaxed);
            state->active.store(options.budget.value != 0, std::memory_order_relaxed);

            Detail::SubscriptionRecord record{
                .state = state,
                .callback =
                    [callback = std::move(typedCallback)](EventContextBase& base, const void* payload) mutable {
                        auto& event = *static_cast<const Event*>(payload);
                        EventContext<Event> context{base, event};
                        return Detail::InvokeTypedHandler<Event, R, E>(callback, context);
                    },
                .receiver = EventRef(receiver),
                .receiverLifetime = EventLifetime(receiver),
                .eventId = Traits::EventIdOf<Event>,
                .callbackId = options.callbackId,
                .priority = options.priority,
                .anyEvent = false,
            };
            Events(receiver).AttachInbound(state);
            return AddSubscription(std::move(record));
        }

        /** @brief Attach an event-type-erased relation owned by this port's emitter. */
        template<typename Receiver, typename Emitter, typename Callback>
        [[nodiscard]] EventLink ListenAny(Receiver& receiver, Emitter&, Callback&& callback,
                                          EventOptions options = {}) {
            using R = std::remove_cvref_t<Receiver>;
            using E = std::remove_cvref_t<Emitter>;
            using F = std::remove_cvref_t<Callback>;
            static_assert(std::move_constructible<F>);

            auto anyCallback = F(std::forward<Callback>(callback));
            auto state = std::make_shared<EventLink::State>();
            state->remaining.store(options.budget.value, std::memory_order_relaxed);
            state->active.store(options.budget.value != 0, std::memory_order_relaxed);

            Detail::SubscriptionRecord record{
                .state = state,
                .callback =
                    [callback = std::move(anyCallback)](EventContextBase& context, const void*) mutable {
                        return Detail::InvokeAnyHandler<R, E>(callback, context);
                    },
                .receiver = EventRef(receiver),
                .receiverLifetime = EventLifetime(receiver),
                .callbackId = options.callbackId,
                .priority = options.priority,
                .anyEvent = true,
            };
            Events(receiver).AttachInbound(state);
            return AddSubscription(std::move(record));
        }

        /** @brief Deliver @p event synchronously from @p emitter. */
        template<Concept::EventPayload Event, typename Emitter>
        void Emit(Emitter& emitter, const Event& event) {
            const uint64_t sequence = nextSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
            auto emitterRef = EventRef(emitter);
            auto subscriptions = Subscriptions();
            Trace(EventTrace{.phase = EventTracePhase::ReceiveBegin,
                             .emitter = emitterRef,
                             .eventId = Traits::EventIdOf<Event>,
                             .sequence = sequence});

            for (const auto& record : subscriptions->records) {
                if (!record || (!record->anyEvent && record->eventId != Traits::EventIdOf<Event>)) {
                    continue;
                }
                EventObjectRef receiverRef = record->receiverLifetime ? record->receiver : EventObjectRef{};
                if (!receiverRef || !record->state) {
                    if (record->state) {
                        record->state->active.store(false, std::memory_order_release);
                    }
                    continue;
                }

                EventContext<Event> context{{.receiver = receiverRef,
                                             .emitter = emitterRef,
                                             .eventId = Traits::EventIdOf<Event>,
                                             .callbackId = record->callbackId,
                                             .subscription = record->id,
                                             .sequence = sequence},
                                            event};
                const EventFlow flow = DispatchRecord(record, context, event);
                if (flow == EventFlow::Stop) {
                    break;
                }
            }

            Trace(EventTrace{.phase = EventTracePhase::ReceiveEnd,
                             .emitter = emitterRef,
                             .eventId = Traits::EventIdOf<Event>,
                             .sequence = sequence});
        }

        /** @brief Schedule every callback selected by @p event on @p scheduler without storing the scheduler. */
        template<stdexec::scheduler Scheduler, Concept::EventPayload Event, typename Emitter>
        void EmitOn(Emitter& emitter, Scheduler scheduler, const Event& event) {
            const uint64_t sequence = nextSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
            const EventId eventId = Traits::EventIdOf<Event>;
            auto emitterRef = EventRef(emitter);
            auto emitterLifetime = EventLifetime(emitter);
            auto subscriptions = Subscriptions();
            auto traces = Traces();
            Trace(traces, EventTrace{.phase = EventTracePhase::ReceiveBegin,
                                     .emitter = emitterRef,
                                     .eventId = eventId,
                                     .sequence = sequence});

            std::shared_ptr<const EmissionFrame<Event>> frame;
            auto matchingRecords = subscriptions->records | std::views::filter([eventId](const auto& record) {
                                       return record && (record->anyEvent || record->eventId == eventId);
                                   });
            for (const auto& record : matchingRecords) {
                if (!record->receiverLifetime || !record->state) {
                    if (record->state) {
                        record->state->active.store(false, std::memory_order_release);
                    }
                    continue;
                }
                if (!frame) {
                    frame = std::make_shared<const EmissionFrame<Event>>(
                        EmissionFrame<Event>{.event = event,
                                             .emitter = emitterRef,
                                             .emitterLifetime = emitterLifetime,
                                             .traces = traces,
                                             .sequence = sequence});
                }
                auto act = EventDeliveryAct<Event>{frame, record->receiverLifetime, record};
                if (act) {
                    StartEvent(scheduler, std::move(act));
                }
            }

            Trace(traces, EventTrace{.phase = EventTracePhase::ReceiveEnd,
                                     .emitter = emitterRef,
                                     .eventId = eventId,
                                     .sequence = sequence});
        }

        /** @brief Attach a trace hook. */
        template<typename Callback>
            requires std::move_constructible<std::remove_cvref_t<Callback>> &&
                     std::invocable<std::remove_cvref_t<Callback>&, const EventTrace&>
        [[nodiscard]] EventTraceHook AttachTrace(Callback&& callback) {
            using F = std::remove_cvref_t<Callback>;
            auto state = std::make_shared<EventTraceHook::State>();
            Detail::TraceRecord record{
                .state = state,
                .callback = F(std::forward<Callback>(callback)),
            };
            return AddTrace(std::move(record));
        }

        /** @brief Cancel every relation owned by this port and every inbound relation known by this port. */
        void CancelAll() noexcept {
            std::scoped_lock lock(mutex_);
            for (const auto& subscription : subscriptions_->records) {
                if (subscription && subscription->state) {
                    subscription->state->active.store(false, std::memory_order_release);
                    subscription->state->canceled.store(true, std::memory_order_release);
                }
            }
            std::erase_if(inbound_, [](const std::weak_ptr<EventLink::State>& relation) {
                auto state = relation.lock();
                if (!state) {
                    return true;
                }
                state->active.store(false, std::memory_order_release);
                state->canceled.store(true, std::memory_order_release);
                return false;
            });
            for (const auto& trace : traces_->records) {
                if (trace && trace->state) {
                    trace->state->active.store(false, std::memory_order_release);
                }
            }
        }

    private:
        template<Concept::EventPayload>
        friend class EventDeliveryAct;

        EventLink AddSubscription(Detail::SubscriptionRecord record) {
            record.id = nextSubscription_.fetch_add(1, std::memory_order_relaxed) + 1;
            record.order = record.id;
            auto state = record.state;
            auto stored =
                std::shared_ptr<Detail::SubscriptionRecord>{new Detail::SubscriptionRecord(std::move(record))};
            {
                std::scoped_lock lock(mutex_);
                auto next = std::make_shared<Detail::SubscriptionSnapshot>(*subscriptions_);
                next->records.push_back(std::move(stored));
                std::ranges::stable_sort(next->records, [](const auto& lhs, const auto& rhs) noexcept {
                    if (lhs->priority != rhs->priority) {
                        return lhs->priority > rhs->priority;
                    }
                    return lhs->order < rhs->order;
                });
                subscriptions_ = std::move(next);
            }
            return EventLink{state};
        }

        EventTraceHook AddTrace(Detail::TraceRecord record) {
            record.id = nextTrace_.fetch_add(1, std::memory_order_relaxed) + 1;
            record.order = record.id;
            auto state = record.state;
            auto stored = std::shared_ptr<Detail::TraceRecord>{new Detail::TraceRecord(std::move(record))};
            {
                std::scoped_lock lock(mutex_);
                auto next = std::make_shared<Detail::TraceSnapshot>(*traces_);
                next->records.push_back(std::move(stored));
                traces_ = std::move(next);
            }
            return EventTraceHook{state};
        }

        void AttachInbound(std::weak_ptr<EventLink::State> state) {
            std::scoped_lock lock(mutex_);
            std::erase_if(inbound_, [](const std::weak_ptr<EventLink::State>& relation) { return relation.expired(); });
            inbound_.push_back(std::move(state));
        }

        [[nodiscard]] std::shared_ptr<const Detail::SubscriptionSnapshot> Subscriptions() const {
            std::scoped_lock lock(mutex_);
            return subscriptions_;
        }

        [[nodiscard]] std::shared_ptr<const Detail::TraceSnapshot> Traces() const {
            std::scoped_lock lock(mutex_);
            return traces_;
        }

        void Trace(const EventTrace& trace) const { Trace(Traces(), trace); }

        static void Trace(std::shared_ptr<const Detail::TraceSnapshot> traces, const EventTrace& trace) {
            if (!traces) {
                return;
            }
            for (const auto& hook : traces->records) {
                if (hook && hook->state && hook->state->active.load(std::memory_order_acquire)) {
                    hook->callback(trace);
                }
            }
        }

        template<Concept::EventPayload Event>
        EventFlow DispatchRecord(const std::shared_ptr<Detail::SubscriptionRecord>& record,
                                 EventContext<Event>& context, const Event& event) {
            if (!record || !record->state || !TryBeginDispatch(*record->state)) {
                return EventFlow::Continue;
            }
            return record->callback(context, std::addressof(event));
        }

        mutable std::mutex mutex_{};
        std::shared_ptr<Detail::SubscriptionSnapshot> subscriptions_{};
        std::shared_ptr<Detail::TraceSnapshot> traces_{};
        std::vector<std::weak_ptr<EventLink::State>> inbound_{};
        std::atomic<uint64_t> nextSubscription_{};
        std::atomic<uint64_t> nextTrace_{};
        std::atomic<uint64_t> nextSequence_{};
    };

    template<Concept::EventPayload Event>
    void EventDeliveryAct<Event>::Trace(EventTracePhase phase, EventObjectRef receiver, EventObjectRef emitter) const {
        EventPort::Trace(frame_ ? frame_->traces : nullptr,
                         EventTrace{.phase = phase,
                                    .receiver = receiver,
                                    .emitter = emitter,
                                    .eventId = Traits::EventIdOf<Event>,
                                    .callbackId = record_ ? record_->callbackId : EventId{},
                                    .subscription = record_ ? record_->id : 0,
                                    .sequence = frame_ ? frame_->sequence : 0});
    }

    template<Concept::EventPayload Event>
    void EventDeliveryAct<Event>::operator()() {
        if (!frame_ || !record_ || !record_->state || !Dispatchable(*record_->state)) {
            return;
        }
        EventObjectRef receiverRef = receiver_ ? record_->receiver : EventObjectRef{};
        EventObjectRef emitterRef = frame_->emitterLifetime ? frame_->emitter : EventObjectRef{};
        if (!receiverRef || !emitterRef) {
            if (record_->state) {
                record_->state->active.store(false, std::memory_order_release);
            }
            Trace(EventTracePhase::CallbackCanceled, receiverRef, emitterRef);
            return;
        }
        Trace(EventTracePhase::CallbackBegin, receiverRef, emitterRef);
        EventContext<Event> context{{.receiver = receiverRef,
                                     .emitter = emitterRef,
                                     .eventId = Traits::EventIdOf<Event>,
                                     .callbackId = record_->callbackId,
                                     .subscription = record_->id,
                                     .sequence = frame_->sequence},
                                    frame_->event};
        if (TryBeginDispatch(*record_->state)) {
            std::ignore = record_->callback(context, std::addressof(frame_->event));
        }
        Trace(EventTracePhase::CallbackEnd, receiverRef, emitterRef);
    }

    /** @brief Subscribe @p receiver to @p emitter's @p Event with the receiver's default On handler. */
    template<typename Receiver, typename Emitter, Concept::EventPayload Event>
        requires Concept::EventReceiver<Receiver, Event> && Concept::EventEmitter<Emitter, Event>
    [[nodiscard]] EventLink Listen(Receiver& receiver, Emitter& emitter, const Event&, EventOptions options = {}) {
        auto defaultHandler = [](Receiver& r, const Event& event, Emitter& e, EventContext<Event>& context) {
            std::ignore = r;
            std::ignore = event;
            std::ignore = e;
            return Detail::InvokeDefaultReceiver<Event, Receiver, Emitter>(context);
        };
        return Events(emitter).template ListenTyped<Event>(receiver, emitter, std::move(defaultHandler), options);
    }

    /** @brief Subscribe @p receiver to @p emitter's @p Event with an explicit handler. */
    template<typename Receiver, typename Emitter, Concept::EventPayload Event, typename Handler>
        requires Concept::EventReceiver<Receiver, Event> && Concept::EventEmitter<Emitter, Event>
    [[nodiscard]] EventLink Listen(Receiver& receiver, Emitter& emitter, const Event&, EventOptions options,
                                   Handler&& handler) {
        return Events(emitter).template ListenTyped<Event>(receiver, emitter, std::forward<Handler>(handler), options);
    }

    /** @brief Subscribe @p receiver to @p emitter's @p Event with an explicit handler and budget shorthand. */
    template<typename Receiver, typename Emitter, Concept::EventPayload Event, typename Handler>
        requires Concept::EventReceiver<Receiver, Event> && Concept::EventEmitter<Emitter, Event>
    [[nodiscard]] EventLink Listen(Receiver& receiver, Emitter& emitter, const Event& event, WoreBudget budget,
                                   Handler&& handler) {
        return Listen(receiver, emitter, event, EventOptions{.budget = budget}, std::forward<Handler>(handler));
    }

    /** @brief Subscribe @p receiver to every event emitted by @p emitter. */
    template<Concept::EventParticipant Receiver, Concept::EventParticipant Emitter, typename Handler>
        requires Traits::CanAcceptEvent<Receiver> && Traits::CanEmitEvent<Emitter>
    [[nodiscard]] EventLink Listen(Receiver& receiver, Emitter& emitter, AnyEventTag, EventOptions options,
                                   Handler&& handler) {
        return Events(emitter).ListenAny(receiver, emitter, std::forward<Handler>(handler), options);
    }

    /** @brief Emit @p event from @p emitter synchronously. */
    template<typename Emitter, Concept::EventPayload Event>
        requires Concept::EventEmitter<Emitter, Event>
    void Emit(Emitter& emitter, const Event& event) {
        Events(emitter).Emit(emitter, event);
    }

    /** @brief Emit a default-constructed @p Event from @p emitter synchronously. */
    template<Concept::EventPayload Event, typename Emitter>
        requires Concept::EventEmitter<Emitter, Event>
    void Emit(Emitter& emitter) {
        Events(emitter).template Emit<Event>(emitter, Event{});
    }

    /** @brief Emit @p event from @p emitter on @p scheduler. */
    template<typename Emitter, stdexec::scheduler Scheduler, Concept::EventPayload Event>
        requires Concept::EventEmitter<Emitter, Event>
    void EmitOn(Emitter& emitter, Scheduler scheduler, const Event& event) {
        Events(emitter).EmitOn(emitter, std::move(scheduler), event);
    }

} // namespace Sora::Experimental

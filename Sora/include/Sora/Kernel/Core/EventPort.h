/**
 * @file EventPort.h
 * @brief Per-nucleus event/callback hub, event type-list traits, subscriptions, delivery acts, and trace hooks.
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/Traits/InheritanceTraits.h>
#include <Sora/Core/Traits/ScopeTraits.h>
#include <Sora/Kernel/Core/Traits.h>
#include <Sora/Kernel/Core/BaseObject.h>
#include <Sora/Kernel/Core/ComPtr.h>
#include <Sora/Kernel/Core/IID.h>

#include <Sora/Core/Hash.h>
#include <Sora/Core/Traits/TypeTraits.h>

#include <atomic>
#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <meta>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <exec/start_detached.hpp>
#include <stdexec/execution.hpp>

namespace Sora::Kernel {

    namespace Event {

        /** @brief Base class for all event payloads. */
        struct BaseEvent {};

        /** @brief Type-list sentinel declaring that a class emits or accepts every concrete event payload. */
        struct AllEvents : BaseEvent {};

        /** @brief Type-list sentinel declaring that a class emits or accepts an unspecified event payload. */
        struct UnspecifiedEvent : BaseEvent {};

    } // namespace Event

    namespace $ {

        struct Eventivity {
            bool value = true;
        };

        inline constexpr auto Eventive = Eventivity{.value = true};

    } // namespace $

    /** @brief Strong identifier for event payload types and callback policy tags. */
    struct EventId : Uuid {
        constexpr EventId() noexcept = default;
        explicit constexpr EventId(uint128_t value) noexcept : Uuid(value) {}

        constexpr bool operator==(const EventId&) const noexcept = default;
        constexpr auto operator<=>(const EventId&) const noexcept = default;
    };

    namespace Meta {

        consteval bool IsEventType(std::meta::info info) {
            info = std::meta::dealias(info);
            if (!std::meta::is_class_type(info)) {
                throw std::define_static_string("Meta::IsEvent: '" + std::string{std::meta::display_string_of(info)} +
                                                "' is not a class reflection -- only classes can be events.");
            }

            // Case 1: Event types are either explicitly marked with the @ref Eventive tag or derive from @ref
            // Event::BaseEvent.
            if (Sora::$::Has<Sora::Kernel::$::Eventivity>(info) || Sora::Meta::IsInScope(info, ^^Sora::Kernel::Event) ||
                Sora::Meta::DerivedFrom(info, ^^Sora::Kernel::Event::BaseEvent)) {
                return true;
            }

            // Case 2: Structural event types are aggregates that are standard-layout and trivially copyable. This
            // allows event payloads to be passed by value without requiring a vtable or heap allocation
            return std::meta::is_standard_layout_type(info) && std::meta::is_trivially_copyable_type(info) &&
                   std::meta::is_aggregate_type(info);
        }

    } // namespace Meta

    namespace Concept {

        template<typename T>
        concept EventClass = std::is_class_v<T> && Sora::Kernel::Meta::IsEventType(^^T);

        /** @brief Concrete event payload class accepted by the event hub. */
        template<typename T>
        concept EventPayload =
            std::is_class_v<std::remove_cvref_t<T>> && std::copy_constructible<std::remove_cvref_t<T>> &&
            !std::same_as<std::remove_cvref_t<T>, Event::AllEvents> &&
            !std::same_as<std::remove_cvref_t<T>, Event::UnspecifiedEvent> &&
            Sora::Kernel::Meta::IsEventType(std::meta::remove_cvref(^^T));

        /** @brief Callback policy tag; tags are ordinary class types, not event payloads. */
        template<typename T>
        concept CallbackTagClass = std::is_class_v<std::remove_cvref_t<T>>;

        static_assert(EventClass<Event::BaseEvent>, "Event::BaseEvent must satisfy EventClass concept.");
        static_assert(EventClass<Event::AllEvents>, "Event::AllEvents must satisfy EventClass concept.");
        static_assert(EventClass<Event::UnspecifiedEvent>, "Event::UnspecifiedEvent must satisfy EventClass concept.");

    } // namespace Concept

    /** @brief Declaration-side event-list sentinel accepting every concrete event payload. */
    using AllEvents = Event::AllEvents;

    /** @brief Declaration-side event-list sentinel used by unconstrained event concepts. */
    using UnspecifiedEvent = Event::UnspecifiedEvent;

    /** @brief Default callback policy tag used when no more specific tag is requested. */
    struct DefaultCallbackTag {};

    /** @brief TypeList of event payloads, or the single @ref AllEvents declaration pattern. */
    template<typename... Ts>
    using EventList = Sora::Traits::TypeList<Ts...>;

    /** @brief Event wildcard value used by Listen overloads that do not care about the concrete event type. */
    struct AnyEventTag {};

    /** @brief Subscribe to every event explicitly emitted through the selected emitter relation. */
    inline constexpr AnyEventTag AnyEvent{};

    namespace Meta {

        consteval bool IsCallbackTagList(std::meta::info info) {
            info = std::meta::dealias(info);
            if (!Sora::Meta::IsTypeList(info)) {
                return false;
            }

            return std::ranges::all_of(Sora::Meta::TypeListTypesOf(info),
                                       std::meta::is_class_type); // TODO: not sufficient
        }

        consteval bool IsEventTypeList(std::meta::info info) {
            info = std::meta::dealias(info);
            if (!Sora::Meta::IsTypeList(info)) {
                return false;
            }

            auto args = Sora::Meta::TypeListTypesOf(info);
            if (!std::ranges::all_of(args, Meta::IsEventType)) {
                return false;
            }

            return true;
        }

        consteval std::meta::info CanonicalizeEventList(std::meta::info info) {
            info = std::meta::dealias(info);
            if (!IsEventTypeList(info)) {
                throw std::define_static_string("Meta::CanonicalEventListOf: '" +
                                                std::string{std::meta::display_string_of(info)} +
                                                "' is not a Sora::Traits::TypeList specialization of event types.");
            }

            auto types = Sora::Meta::TypeListTypesOf(info) | std::views::filter([](std::meta::info type) {
                             return type != ^^Sora::Kernel::Event::UnspecifiedEvent;
                         }) |
                         std::ranges::to<std::vector>();
            if (std::ranges::contains(types, ^^Sora::Kernel::Event::AllEvents)) {
                types = {^^Sora::Kernel::Event::AllEvents};
            }
            return std::meta::substitute(^^Sora::Kernel::EventList, types);
        }

        template<std::meta::info Validator>
        consteval bool HasDeclaredTypeListInfo(std::meta::info type, std::string_view name) {
            auto info = std::meta::dealias(std::meta::remove_cvref(type));
            if (!Sora::Kernel::Meta::IsComClass(info)) {
                return false;
            }

            auto chain = Sora::Meta::InheritanceChainUntil(info, ^^Sora::Kernel::BaseUnknown);
            for (auto scope : chain) {
                auto list = Sora::Meta::FindDirectTypeMemberOf(scope, name);
                if (list == std::meta::info{}) {
                    continue;
                }

                if (!Sora::Meta::IsTypeList(list) || !Sora::Meta::Invoke<Validator>(list)) {
                    throw std::define_static_string(
                        std::format("Meta::HasDeclaredTypeListInfo: '{}' is not a valid type-list for '{}'.",
                                    Sora::Meta::DisplayStringOf(list), name));
                }
                return true;
            }
            return false;
        }

    } // namespace Meta

    namespace Concept {

        /** @brief Nested event type-list declaration whose entries are event payloads. */
        template<typename T>
        concept EventTypeList = Meta::IsEventTypeList(^^T);

        /** @brief Nested callback-tag type-list declaration whose entries are callback tags. */
        template<typename T>
        concept CallbackTagTypeList = Meta::IsCallbackTagList(^^T);

        /** @brief Class that declares emitted events through a nested or inherited @c Emits alias. */
        template<typename T>
        concept EmittableClass = Meta::HasDeclaredTypeListInfo<^^Meta::IsEventTypeList>(^^T, "Emits");

        /** @brief Class that declares accepted events through a nested or inherited @c Accepts alias. */
        template<typename T>
        concept AcceptableClass = Meta::HasDeclaredTypeListInfo<^^Meta::IsEventTypeList>(^^T, "Accepts");

        /** @brief Class that declares callback tags through a nested or inherited @c Callbacks alias. */
        template<typename T>
        concept CallbackAttachableClass = Meta::HasDeclaredTypeListInfo<^^Meta::IsCallbackTagList>(^^T, "Callbacks");

    } // namespace Concept

    namespace Traits {

        /** @brief Stable event/callback tag identifier derived from the reflected ABI digest of @p T. */
        template<typename T>
            requires std::is_class_v<std::remove_cvref_t<T>>
        inline constexpr EventId EventIdOf{Sora::Traits::AbiDigestOf<std::remove_cvref_t<T>, true>};

        namespace Detail {

            template<std::meta::info Validator>
            consteval std::meta::info DeclaredTypeListInfoOf(std::meta::info type, std::string_view name) {
                auto info = std::meta::dealias(std::meta::remove_cvref(type));
                if (!Sora::Kernel::Meta::IsComClass(info)) {
                    throw std::define_static_string(
                        std::format("Traits::DeclaredTypeListOf: '{}' is not a COM class reflection.",
                                    Sora::Meta::DisplayStringOf(info)));
                }

                // 1. collect all direct TypeList members of the given name in the inheritance chain, starting from the
                // most-derived type
                std::vector<std::meta::info> result;
                auto chain = Sora::Meta::InheritanceChainUntil(info, ^^Sora::Kernel::BaseUnknown);
                for (auto scope : chain | std::views::reverse) {
                    auto list = Sora::Meta::FindDirectTypeMemberOf(scope, name);
                    if (list == std::meta::info{}) {
                        continue;
                    }

                    if (!Sora::Meta::IsTypeList(list) || !Sora::Meta::Invoke<Validator>(list)) {
                        throw std::define_static_string(
                            std::format("Traits::DeclaredTypeListOf: '{}' is not a valid type-list for '{}'.",
                                        Sora::Meta::DisplayStringOf(list), name));
                    }
                    Sora::Meta::AppendTypeListUnique(result, list);
                }

                // 2. merge the collected type-lists into a single TypeList specialization
                return Sora::Kernel::Meta::CanonicalizeEventList(
                    std::meta::substitute(^^Sora::Kernel::EventList, result));
            }

        } // namespace Detail

        /** @brief Type-list of event payloads emitted by @p T after inherited declarations are merged. */
        template<Concept::ComClass T>
        using EmittedEventsOf = typename [:Detail::DeclaredTypeListInfoOf<^^Meta::IsEventTypeList>(^^T, "Emits"):];

        /** @brief Type-list of event payloads accepted by @p T after inherited declarations are merged. */
        template<Concept::ComClass T>
        using AcceptedEventsOf = typename [:Detail::DeclaredTypeListInfoOf<^^Meta::IsEventTypeList>(^^T, "Accepts"):];

        /** @brief Type-list of callback tags allowed by @p T after inherited declarations are merged. */
        template<Concept::ComClass T>
        using CallbackTagsOf = typename [:Detail::DeclaredTypeListInfoOf<^^Meta::IsCallbackTagList>(^^T, "Callbacks"):];

    } // namespace Traits

    namespace Traits {

        /** @brief Return whether @p List permits event payload @p Event, honoring @ref AllEvents. */
        template<Concept::EventTypeList List, Concept::EventClass E = Event::UnspecifiedEvent>
        inline constexpr bool EventListContains = (std::same_as<E, Event::UnspecifiedEvent> && List::size) ||
                                                  Sora::Traits::Contains<List, std::remove_cvref_t<E>> ||
                                                  Sora::Traits::Contains<List, Sora::Kernel::Event::AllEvents>;

        /**
         * @brief Return whether @p T can emit @p Event.
         * @details Classes emit only payload types declared by their own type-list alias or inherited from direct
         * bases.
         */
        template<Concept::ComClass T, Concept::EventClass E = Event::UnspecifiedEvent>
        inline constexpr bool CanEmitEvent = (std::same_as<std::remove_cvref_t<E>, Event::UnspecifiedEvent> ||
                                              Concept::EventPayload<std::remove_cvref_t<E>>) &&
                                             EventListContains<Traits::EmittedEventsOf<std::remove_cvref_t<T>>, E>;

        /**
         * @brief Return whether @p T can receive @p Event.
         * @details Classes accept only payload types declared by their own type-list alias or inherited from direct
         * bases.
         */
        template<Concept::ComClass T, Concept::EventClass E = Event::UnspecifiedEvent>
        inline constexpr bool CanAcceptEvent = (std::same_as<std::remove_cvref_t<E>, Event::UnspecifiedEvent> ||
                                                Concept::EventPayload<std::remove_cvref_t<E>>) &&
                                               EventListContains<Traits::AcceptedEventsOf<std::remove_cvref_t<T>>, E>;

        /**
         * @brief Return whether @p T can attach a callback tagged by @p CallbackTag.
         * @details Classes allow only callback tags declared by their own type-list alias or inherited from direct
         * bases.
         */
        template<Concept::ComClass T, Concept::CallbackTagClass CallbackTag>
        inline constexpr bool CanAttachCallback =
            Concept::CallbackTagClass<std::remove_cvref_t<CallbackTag>> &&
            Sora::Traits::Contains<Traits::CallbackTagsOf<std::remove_cvref_t<T>>, std::remove_cvref_t<CallbackTag>>;

    } // namespace Traits

    namespace Concept {

        /** @brief Object-model class whose emitted-event list permits @p Event. */
        template<typename T, typename E = Event::UnspecifiedEvent>
        concept EventEmitter = Concept::ComClass<T> && Concept::EventClass<E> && Traits::CanEmitEvent<T, E>;

        /** @brief Object-model class whose accepted-event list permits @p Event. */
        template<typename T, typename E = Event::UnspecifiedEvent>
        concept EventReceiver = Concept::ComClass<T> && Concept::EventClass<E> && Traits::CanAcceptEvent<T, E>;

        /** @brief Object-model class whose callback-tag list permits @p CallbackTag. */
        template<typename T, typename CallbackTag>
        concept CallbackAttachable =
            Concept::ComClass<T> && Concept::CallbackTagClass<CallbackTag> && Traits::CanAttachCallback<T, CallbackTag>;

    } // namespace Concept

    /** @brief Completion decision returned by an event callback. */
    enum class EventFlow : uint8_t {
        Continue = 0, /**< Continue dispatching later callbacks in the same receive pass. */
        Stop = 1,     /**< Stop dispatching later callbacks in the same receive pass. */
    };

    /** @brief Trace phase for observing an object's event receive process. */
    enum class EventTracePhase : uint8_t {
        ReceiveBegin,
        CallbackScheduled,
        CallbackBegin,
        CallbackEnd,
        CallbackCanceled,
        ReceiveEnd,
    };

    /** @brief Type-erased receive context shared by callbacks and trace hooks. */
    struct EventContextBase {
        BaseUnknown& receiver;   /**< Object whose hub is accepting the event. */
        BaseUnknown& emitter;    /**< Object that emitted the event. */
        EventId eventId{};       /**< Reflected event payload identifier. */
        EventId callbackId{};    /**< Reflected callback tag identifier. */
        uint64_t subscription{}; /**< Subscription identifier currently being evaluated. */
        uint64_t sequence{};     /**< Monotone receive sequence on the receiver hub. */
    };

    /** @brief Strongly typed event receive context. */
    template<Concept::EventPayload E>
    struct EventContext : EventContextBase {
        const E& event; /**< Event payload being delivered. */
    };

    /** @brief Trace hook payload produced by an event hub. */
    struct EventTrace {
        EventTracePhase phase{}; /**< Current receive phase. */
        BaseUnknown* receiver{}; /**< Receiver object, null when an async task observes destruction. */
        BaseUnknown* emitter{};  /**< Emitter object, null when an async task observes destruction. */
        EventId eventId{};       /**< Reflected event payload identifier. */
        EventId callbackId{};    /**< Reflected callback tag identifier. */
        uint64_t subscription{}; /**< Subscription identifier, or zero for hub-wide phases. */
        uint64_t sequence{};     /**< Monotone receive sequence on the receiver hub. */
    };

    /** @brief Dispatch budget attached to an event relation. */
    struct WoreBudget {
        static constexpr uint32_t kPersistent = 0xffffffffu; /**< Sentinel for a non-wearing relation. */
        uint32_t value{kPersistent};                         /**< Remaining successful dispatches. */
    };

    /** @brief Relation budget that never wears down. */
    inline constexpr WoreBudget Persistent{WoreBudget::kPersistent};

    /** @brief Relation budget consumed by one successful dispatch. */
    inline constexpr WoreBudget OneShot{1};

    /** @brief Event relation behavior knobs shared by typed and wildcard listeners. */
    struct EventOptions {
        WoreBudget budget{Persistent}; /**< Successful dispatch budget for this relation. */
        int32_t priority{};            /**< Higher priority callbacks run first for immediate dispatch. */
        EventId callbackId{Traits::EventIdOf<DefaultCallbackTag>}; /**< Callback policy tag. */
    };

    namespace Detail {

        struct SubscriptionState {
            std::atomic<bool> active{true}; /**< False when no future dispatch may enter. */
            std::atomic<bool> canceled{};   /**< True after Drop, CancelAll, or zero budget. */
            std::atomic<bool> suspended{};  /**< True while dispatch is paused. */
            std::atomic<uint32_t> remaining{WoreBudget::kPersistent}; /**< Successful dispatch budget. */
        };

        struct TraceState {
            std::atomic<bool> active{true};
        };

        /** @brief Move-only pointer-like retainer that keeps a closure nucleus alive across deferred callbacks. */
        class ClosureRetainerPtr {
        public:
            ClosureRetainerPtr() noexcept = default;

            /** @brief Retain the closure nucleus of @p object, if any. */
            explicit ClosureRetainerPtr(BaseUnknown* object) noexcept
                : nucleus_(object ? object->Nucleus() : nullptr) {}

            ClosureRetainerPtr(const ClosureRetainerPtr&) = delete;
            ClosureRetainerPtr& operator=(const ClosureRetainerPtr&) = delete;
            ClosureRetainerPtr(ClosureRetainerPtr&&) noexcept = default;
            ClosureRetainerPtr& operator=(ClosureRetainerPtr&&) noexcept = default;

            /** @brief Return the retained nucleus pointer, or null when this retainer is empty. */
            [[nodiscard]] BaseUnknown* Get() const noexcept { return nucleus_.Get(); }

        private:
            ComPtr<BaseUnknown> nucleus_{};
        };

    } // namespace Detail

    /** @brief Stable handle for an explicit event relation. */
    class EventLink {
    public:
        EventLink() noexcept = default;
        explicit EventLink(std::weak_ptr<Detail::SubscriptionState> state) noexcept : state_(std::move(state)) {}

        /** @brief Return whether this handle still refers to an active relation. */
        [[nodiscard]] bool Active() const noexcept {
            auto state = state_.lock();
            return state && state->active.load(std::memory_order_acquire) &&
                   !state->canceled.load(std::memory_order_acquire);
        }

    private:
        friend void Drop(const EventLink&) noexcept;
        friend void Suspend(const EventLink&) noexcept;
        friend void Resume(const EventLink&) noexcept;
        friend void SetBudget(const EventLink&, WoreBudget) noexcept;

        std::weak_ptr<Detail::SubscriptionState> state_{};
    };

    /** @brief Remove @p link from future dispatch without mutating relation storage immediately. */
    inline void Drop(const EventLink& link) noexcept {
        if (auto state = link.state_.lock()) {
            state->active.store(false, std::memory_order_release);
            state->canceled.store(true, std::memory_order_release);
        }
    }

    /** @brief Pause @p link without consuming its dispatch budget. */
    inline void Suspend(const EventLink& link) noexcept {
        if (auto state = link.state_.lock()) {
            state->suspended.store(true, std::memory_order_release);
        }
    }

    /** @brief Resume @p link after a previous @ref Suspend. */
    inline void Resume(const EventLink& link) noexcept {
        if (auto state = link.state_.lock()) {
            state->suspended.store(false, std::memory_order_release);
        }
    }

    /** @brief Replace @p link's successful-dispatch budget. */
    inline void SetBudget(const EventLink& link, WoreBudget budget) noexcept {
        if (auto state = link.state_.lock()) {
            state->remaining.store(budget.value, std::memory_order_release);
            state->active.store(budget.value != 0, std::memory_order_release);
            state->canceled.store(budget.value == 0, std::memory_order_release);
        }
    }

    /** @brief RAII-capable handle for a trace hook. */
    class EventTraceHook {
    public:
        EventTraceHook() noexcept = default;
        explicit EventTraceHook(std::weak_ptr<Detail::TraceState> state) noexcept : state_(std::move(state)) {}

        /** @brief Detach the trace hook if its hub still exists. */
        void Cancel() noexcept {
            if (auto state = state_.lock()) {
                state->active.store(false, std::memory_order_release);
            }
        }

        /** @brief Return whether this handle still refers to an active trace hook. */
        [[nodiscard]] bool Active() const noexcept {
            auto state = state_.lock();
            return state && state->active.load(std::memory_order_acquire);
        }

    private:
        std::weak_ptr<Detail::TraceState> state_{};
    };

    namespace Detail {

        using ErasedCallback = std::move_only_function<EventFlow(EventContextBase&, const void*)>;
        using ErasedTrace = std::move_only_function<void(const EventTrace&)>;

        struct SubscriptionRecord {
            std::shared_ptr<SubscriptionState> state{};
            ErasedCallback callback{};
            BaseUnknown* receiver{};
            WeakRef receiverWeak{};
            EventId eventId{};
            EventId callbackId{};
            uint64_t id{};
            uint64_t order{};
            int32_t priority{};
            bool anyEvent{};
        };

        struct TraceRecord {
            std::shared_ptr<TraceState> state{};
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

        inline void Trace(const std::shared_ptr<const TraceSnapshot>& traces, const EventTrace& trace) {
            if (!traces) {
                return;
            }
            for (const auto& hook : traces->records) {
                if (hook && hook->state->active.load(std::memory_order_acquire)) {
                    hook->callback(trace);
                }
            }
        }

        [[nodiscard]] inline bool TryConsume(SubscriptionState& state) noexcept {
            if (!state.active.load(std::memory_order_acquire) || state.canceled.load(std::memory_order_acquire) ||
                state.suspended.load(std::memory_order_acquire)) {
                return false;
            }
            for (uint32_t current = state.remaining.load(std::memory_order_acquire);;) {
                if (current == 0) {
                    state.active.store(false, std::memory_order_release);
                    return false;
                }
                if (current == WoreBudget::kPersistent) {
                    return true;
                }
                const uint32_t next = current - 1;
                if (state.remaining.compare_exchange_weak(current, next, std::memory_order_acq_rel,
                                                          std::memory_order_acquire)) {
                    if (next == 0) {
                        state.active.store(false, std::memory_order_release);
                    }
                    return true;
                }
            }
        }
        template<typename Result>
        [[nodiscard]] constexpr EventFlow ToEventFlow(Result&& result) {
            using R = std::remove_cvref_t<Result>;
            if constexpr (std::same_as<R, EventFlow>) {
                return result;
            } else if constexpr (std::same_as<R, void>) {
                return EventFlow::Continue;
            } else {
                static_assert(false, "Event callback must return void or Sora::Kernel::EventFlow.");
            }
        }

        template<Concept::EventPayload Event, typename Callback>
        [[nodiscard]] EventFlow InvokeTypedCallback(Callback& callback, EventContextBase& base, const void* payload) {
            auto& context = static_cast<EventContext<Event>&>(base);
            const Event& event = *static_cast<const Event*>(payload);
            if constexpr (std::invocable<Callback&, EventContext<Event>&>) {
                if constexpr (std::same_as<void, std::invoke_result_t<Callback&, EventContext<Event>&>>) {
                    std::invoke(callback, context);
                    return EventFlow::Continue;
                } else {
                    return ToEventFlow(std::invoke(callback, context));
                }
            } else if constexpr (std::invocable<Callback&, const Event&, BaseUnknown&>) {
                if constexpr (std::same_as<void, std::invoke_result_t<Callback&, const Event&, BaseUnknown&>>) {
                    std::invoke(callback, event, context.emitter);
                    return EventFlow::Continue;
                } else {
                    return ToEventFlow(std::invoke(callback, event, context.emitter));
                }
            } else if constexpr (std::invocable<Callback&, const Event&, EventContext<Event>&>) {
                if constexpr (std::same_as<void, std::invoke_result_t<Callback&, const Event&, EventContext<Event>&>>) {
                    std::invoke(callback, event, context);
                    return EventFlow::Continue;
                } else {
                    return ToEventFlow(std::invoke(callback, event, context));
                }
            } else if constexpr (std::invocable<Callback&, const Event&>) {
                if constexpr (std::same_as<void, std::invoke_result_t<Callback&, const Event&>>) {
                    std::invoke(callback, event);
                    return EventFlow::Continue;
                } else {
                    return ToEventFlow(std::invoke(callback, event));
                }
            } else if constexpr (std::invocable<Callback&>) {
                if constexpr (std::same_as<void, std::invoke_result_t<Callback&>>) {
                    std::invoke(callback);
                    return EventFlow::Continue;
                } else {
                    return ToEventFlow(std::invoke(callback));
                }
            } else {
                static_assert(false, "Event callback must accept EventContext<Event>&, "
                                     "(const Event&, EventContext<Event>&), const Event&, or no "
                                     "arguments.");
            }
        }

        template<typename Callback>
        [[nodiscard]] EventFlow InvokeAnyCallback(Callback& callback, EventContextBase& context, const void*) {
            if constexpr (std::invocable<Callback&, EventContextBase&>) {
                if constexpr (std::same_as<void, std::invoke_result_t<Callback&, EventContextBase&>>) {
                    std::invoke(callback, context);
                    return EventFlow::Continue;
                } else {
                    return ToEventFlow(std::invoke(callback, context));
                }
            } else if constexpr (std::invocable<Callback&, BaseUnknown&>) {
                if constexpr (std::same_as<void, std::invoke_result_t<Callback&, BaseUnknown&>>) {
                    std::invoke(callback, context.emitter);
                    return EventFlow::Continue;
                } else {
                    return ToEventFlow(std::invoke(callback, context.emitter));
                }
            } else if constexpr (std::invocable<Callback&>) {
                if constexpr (std::same_as<void, std::invoke_result_t<Callback&>>) {
                    std::invoke(callback);
                    return EventFlow::Continue;
                } else {
                    return ToEventFlow(std::invoke(callback));
                }
            } else {
                static_assert(false, "AnyEvent callback must accept EventContextBase&, BaseUnknown&, or no arguments.");
            }
        }

    } // namespace Detail

    /** @brief Stable asynchronous context shared by every delivery act created by one event emission. */
    template<Concept::EventPayload Event>
    struct EmissionFrame {
        /** @brief Construct a stable emission frame from the source event and emitter. */
        EmissionFrame(const Event& event, Detail::ClosureRetainerPtr emitter,
                      std::shared_ptr<const Detail::TraceSnapshot> traces,
                      uint64_t sequence) noexcept(std::is_nothrow_copy_constructible_v<Event>)
            : payload(event), emitter(std::move(emitter)), traces(std::move(traces)), sequence(sequence) {}

        Event payload;                                         /**< Stable payload copy shared by all deliveries. */
        Detail::ClosureRetainerPtr emitter{};                  /**< Retains the emitter closure nucleus. */
        std::shared_ptr<const Detail::TraceSnapshot> traces{}; /**< Trace hooks snapshotted at emission time. */
        uint64_t sequence{};                                   /**< Monotone receive sequence. */
    };

    /** @brief Move-only delivery act that performs one lifetime-safe event callback invocation. */
    template<Concept::EventPayload Event>
    class EventDeliveryAct {
    public:
        EventDeliveryAct() noexcept = default;
        EventDeliveryAct(const EventDeliveryAct&) = delete;
        EventDeliveryAct& operator=(const EventDeliveryAct&) = delete;
        EventDeliveryAct(EventDeliveryAct&&) noexcept = default;
        EventDeliveryAct& operator=(EventDeliveryAct&&) noexcept = default;

        /** @brief Return whether this delivery act has a callback relation to execute. */
        [[nodiscard]] explicit operator bool() const noexcept { return frame_ != nullptr && record_ != nullptr; }

        /** @brief Invoke the delivery act once on the current execution agent. */
        EventFlow operator()() {
            if (!frame_ || !record_ || !record_->state) {
                return EventFlow::Continue;
            }

            BaseUnknown* receiverObject = receiver_.Get();
            BaseUnknown* emitterObject = frame_->emitter.Get();
            if (!receiverObject || !emitterObject || record_->state->canceled.load(std::memory_order_acquire) ||
                record_->state->suspended.load(std::memory_order_acquire)) {
                Trace(EventTracePhase::CallbackCanceled, receiverObject, emitterObject);
                return EventFlow::Continue;
            }

            EventContext<Event> context{{.receiver = *receiverObject,
                                         .emitter = *emitterObject,
                                         .eventId = Traits::EventIdOf<Event>,
                                         .callbackId = record_->callbackId,
                                         .subscription = record_->id,
                                         .sequence = frame_->sequence},
                                        frame_->payload};
            Trace(EventTracePhase::CallbackBegin, receiverObject, emitterObject);
            const EventFlow flow = record_->callback(context, std::addressof(frame_->payload));
            Trace(EventTracePhase::CallbackEnd, receiverObject, emitterObject);
            return flow;
        }

    private:
        friend class EventPort;

        EventDeliveryAct(std::shared_ptr<const EmissionFrame<Event>> frame, Detail::ClosureRetainerPtr receiver,
                         std::shared_ptr<Detail::SubscriptionRecord> record) noexcept
            : frame_(std::move(frame)), receiver_(std::move(receiver)), record_(std::move(record)) {}

        void Trace(EventTracePhase phase, BaseUnknown* receiver, BaseUnknown* emitter) const {
            Detail::Trace(frame_->traces, EventTrace{.phase = phase,
                                                     .receiver = receiver,
                                                     .emitter = emitter,
                                                     .eventId = Traits::EventIdOf<Event>,
                                                     .callbackId = record_->callbackId,
                                                     .subscription = record_->id,
                                                     .sequence = frame_->sequence});
        }

        std::shared_ptr<const EmissionFrame<Event>> frame_{};
        Detail::ClosureRetainerPtr receiver_{};
        std::shared_ptr<Detail::SubscriptionRecord> record_{};
    };

    /** @brief Build a sender that invokes @p act after @p scheduler completes one schedule operation. */
    template<typename Scheduler, typename Act>
        requires stdexec::scheduler<std::remove_cvref_t<Scheduler>> && std::move_constructible<Act> &&
                 std::invocable<Act&>
    [[nodiscard]] auto EventSender(Scheduler&& scheduler, Act act) {
        return stdexec::schedule(std::forward<Scheduler>(scheduler)) | stdexec::then(std::move(act));
    }

    /** @brief Start @p act as fire-and-forget work on @p scheduler. */
    template<typename Scheduler, typename Act>
        requires stdexec::scheduler<std::remove_cvref_t<Scheduler>> && std::move_constructible<Act> &&
                 std::invocable<Act&>
    void StartEvent(Scheduler&& scheduler, Act act) {
        exec::start_detached(EventSender(std::forward<Scheduler>(scheduler), std::move(act)));
    }

    class EventPort;

    /** @brief Return the event hub associated with @p object. */
    [[nodiscard]] EventPort& Events(BaseUnknown& object);

    /** @brief Cold callback manager stored as a closure-owned data extension on one closure nucleus. */
    class [[= Sora::Kernel::$::DataExtension, = Sora::Kernel::$::Extends<BaseUnknown>{}]] EventPort
        : public BaseUnknown {
        S_OBJECT

    public:
        /** @brief Construct an unbound hub extension; @ref Events binds it to the closure nucleus on demand. */
        EventPort() noexcept;
        EventPort(const EventPort&) = delete;
        EventPort& operator=(const EventPort&) = delete;
        ~EventPort() noexcept;

        /** @brief Return the closure nucleus that owns this hub. */
        [[nodiscard]] BaseUnknown& Owner() noexcept { return *Nucleus(); }

        /** @brief Return the closure nucleus that owns this hub. */
        [[nodiscard]] const BaseUnknown& Owner() const noexcept { return *Nucleus(); }

        /** @brief Return whether the owning nucleus is still alive. */
        [[nodiscard]] bool OwnerAlive() const noexcept { return Extendee() != nullptr; }

        /** @brief Attach a typed relation owned by this hub's emitter. */
        template<Concept::EventPayload Event, typename Callback>
        [[nodiscard]] EventLink ListenTyped(BaseUnknown* receiver, Callback&& callback, EventOptions options = {}) {
            assert(OwnerAlive() && "EventPort used after its owner nucleus was destroyed");
            using F = std::remove_cvref_t<Callback>;
            static_assert(std::move_constructible<F>);

            BaseUnknown* actualReceiver = receiver ? receiver->Nucleus() : nullptr;
            auto typedCallback = F(std::forward<Callback>(callback));
            auto state = std::make_shared<Detail::SubscriptionState>();
            state->remaining.store(options.budget.value, std::memory_order_relaxed);
            state->active.store(options.budget.value != 0, std::memory_order_relaxed);
            if (actualReceiver) {
                Events(*actualReceiver).AttachInbound(state);
            }
            Detail::SubscriptionRecord record{
                .state = std::move(state),
                .callback =
                    [callback = std::move(typedCallback)](EventContextBase& context, const void* payload) mutable {
                        return Detail::InvokeTypedCallback<Event>(callback, context, payload);
                    },
                .receiver = actualReceiver,
                .receiverWeak = actualReceiver ? actualReceiver->GetComponentWeakRef() : WeakRef{},
                .eventId = Traits::EventIdOf<Event>,
                .callbackId = options.callbackId,
                .priority = options.priority,
                .anyEvent = false,
            };
            return AddSubscription(std::move(record));
        }

        /** @brief Attach an event-type-erased relation owned by this hub's emitter. */
        template<typename Callback>
        [[nodiscard]] EventLink ListenAny(BaseUnknown* receiver, Callback&& callback, EventOptions options = {}) {
            assert(OwnerAlive() && "EventPort used after its owner nucleus was destroyed");
            using F = std::remove_cvref_t<Callback>;
            static_assert(std::move_constructible<F>);

            BaseUnknown* actualReceiver = receiver ? receiver->Nucleus() : nullptr;
            auto anyCallback = F(std::forward<Callback>(callback));
            auto state = std::make_shared<Detail::SubscriptionState>();
            state->remaining.store(options.budget.value, std::memory_order_relaxed);
            state->active.store(options.budget.value != 0, std::memory_order_relaxed);
            if (actualReceiver) {
                Events(*actualReceiver).AttachInbound(state);
            }
            Detail::SubscriptionRecord record{
                .state = std::move(state),
                .callback =
                    [callback = std::move(anyCallback)](EventContextBase& context, const void* payload) mutable {
                        return Detail::InvokeAnyCallback(callback, context, payload);
                    },
                .receiver = actualReceiver,
                .receiverWeak = actualReceiver ? actualReceiver->GetComponentWeakRef() : WeakRef{},
                .eventId = {},
                .callbackId = options.callbackId,
                .priority = options.priority,
                .anyEvent = true,
            };
            return AddSubscription(std::move(record));
        }

        /** @brief Deliver @p event to this hub as an accepted event. */
        template<Concept::EventPayload Event>
        void Emit(const Event& event, BaseUnknown* emitter = nullptr) {
            assert(OwnerAlive() && "EventPort used after its owner nucleus was destroyed");
            BaseUnknown* owner = Nucleus();
            BaseUnknown* actualEmitter = emitter ? emitter->Nucleus() : owner;
            if (!actualEmitter) {
                actualEmitter = owner;
            }

            const uint64_t sequence = nextSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
            auto subscriptions = Subscriptions();
            Trace(EventTrace{.phase = EventTracePhase::ReceiveBegin,
                             .receiver = owner,
                             .emitter = actualEmitter,
                             .eventId = Traits::EventIdOf<Event>,
                             .sequence = sequence});

            for (const auto& record : subscriptions->records) {
                if (!record || (!record->anyEvent && record->eventId != Traits::EventIdOf<Event>)) {
                    continue;
                }
                BaseUnknown* actualReceiver = record->receiver ? record->receiverWeak.Get() : actualEmitter;
                if (!actualReceiver || !record->state) {
                    if (record->state) {
                        record->state->active.store(false, std::memory_order_release);
                    }
                    continue;
                }
                EventContext<Event> context{{.receiver = *actualReceiver,
                                             .emitter = *actualEmitter,
                                             .eventId = Traits::EventIdOf<Event>,
                                             .sequence = sequence},
                                            event};
                context.callbackId = record->callbackId;
                context.subscription = record->id;
                const EventFlow flow = DispatchRecord(record, context, event);
                if (flow == EventFlow::Stop) {
                    break;
                }
            }

            Trace(EventTrace{.phase = EventTracePhase::ReceiveEnd,
                             .receiver = owner,
                             .emitter = actualEmitter,
                             .eventId = Traits::EventIdOf<Event>,
                             .sequence = sequence});
        }

        /** @brief Schedule all callbacks selected by @p event on @p scheduler without storing the scheduler. */
        template<stdexec::scheduler Scheduler, Concept::EventPayload Event>
        void EmitOn(Scheduler scheduler, const Event& event, BaseUnknown* emitter = nullptr) {
            assert(OwnerAlive() && "EventPort used after its owner nucleus was destroyed");
            BaseUnknown* owner = Nucleus();
            BaseUnknown* actualEmitter = emitter ? emitter->Nucleus() : owner;
            if (!actualEmitter) {
                actualEmitter = owner;
            }

            const uint64_t sequence = nextSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
            const EventId eventId = Traits::EventIdOf<Event>;
            auto subscriptions = Subscriptions();
            Trace(EventTrace{.phase = EventTracePhase::ReceiveBegin,
                             .receiver = owner,
                             .emitter = actualEmitter,
                             .eventId = eventId,
                             .sequence = sequence});

            auto matchingRecords = subscriptions->records | std::views::filter([eventId](const auto& record) {
                                       return record && (record->anyEvent || record->eventId == eventId);
                                   });
            std::shared_ptr<const EmissionFrame<Event>> frame;
            for (const auto& record : matchingRecords) {
                BaseUnknown* actualReceiver = record->receiver ? record->receiverWeak.Get() : actualEmitter;
                if (!actualReceiver || !record->state) {
                    if (record->state) {
                        record->state->active.store(false, std::memory_order_release);
                    }
                    continue;
                }
                auto deliveryAct = MakeDeliveryAct(record, *actualReceiver, *actualEmitter, event, sequence, frame);
                if (deliveryAct) {
                    StartEvent(scheduler, std::move(deliveryAct));
                }
            }

            Trace(EventTrace{.phase = EventTracePhase::ReceiveEnd,
                             .receiver = owner,
                             .emitter = actualEmitter,
                             .eventId = eventId,
                             .sequence = sequence});
        }

        /** @brief Deliver a default-constructed payload event. */
        template<Concept::EventPayload Event>
            requires std::default_initializable<Event>
        void Emit(BaseUnknown* emitter = nullptr) {
            Emit(Event{}, emitter);
        }

        /** @brief Attach a trace hook observing this hub's receive process. */
        template<typename TraceCallback>
            requires std::invocable<TraceCallback&, const EventTrace&>
        [[nodiscard]] EventTraceHook AttachTrace(TraceCallback&& callback) {
            assert(OwnerAlive() && "EventPort used after its owner nucleus was destroyed");
            using F = std::remove_cvref_t<TraceCallback>;
            static_assert(std::move_constructible<F>);
            Detail::TraceRecord record{
                .state = std::make_shared<Detail::TraceState>(),
                .callback = F(std::forward<TraceCallback>(callback)),
            };
            return AddTrace(std::move(record));
        }

        /** @brief Cancel every subscription and trace hook currently owned by this hub. */
        void CancelAll() noexcept;

    private:
        [[nodiscard]] EventLink AddSubscription(Detail::SubscriptionRecord record);
        [[nodiscard]] EventTraceHook AddTrace(Detail::TraceRecord record);
        [[nodiscard]] std::shared_ptr<const Detail::SubscriptionSnapshot> Subscriptions() const;
        [[nodiscard]] std::shared_ptr<const Detail::TraceSnapshot> Traces() const;
        void AttachInbound(std::weak_ptr<Detail::SubscriptionState> state);
        void Trace(const EventTrace& trace) const;
        static void Trace(std::shared_ptr<const Detail::TraceSnapshot> traces, const EventTrace& trace);
        EventFlow DispatchRecord(const std::shared_ptr<Detail::SubscriptionRecord>& record, EventContextBase& context,
                                 const void* event) = delete;

        template<Concept::EventPayload Event>
        EventFlow DispatchRecord(const std::shared_ptr<Detail::SubscriptionRecord>& record,
                                 EventContext<Event>& context, const Event& event) {
            if (!record->state->active.load(std::memory_order_acquire)) {
                return EventFlow::Continue;
            }

            if (!Detail::TryConsume(*record->state)) {
                return EventFlow::Continue;
            }

            Trace(EventTrace{.phase = EventTracePhase::CallbackBegin,
                             .receiver = std::addressof(context.receiver),
                             .emitter = std::addressof(context.emitter),
                             .eventId = context.eventId,
                             .callbackId = record->callbackId,
                             .subscription = record->id,
                             .sequence = context.sequence});
            const EventFlow flow = record->callback(context, std::addressof(event));
            Trace(EventTrace{.phase = EventTracePhase::CallbackEnd,
                             .receiver = std::addressof(context.receiver),
                             .emitter = std::addressof(context.emitter),
                             .eventId = context.eventId,
                             .callbackId = record->callbackId,
                             .subscription = record->id,
                             .sequence = context.sequence});
            return flow;
        }

        template<Concept::EventPayload Event>
        EventDeliveryAct<Event> MakeDeliveryAct(const std::shared_ptr<Detail::SubscriptionRecord>& record,
                                                BaseUnknown& receiver, BaseUnknown& emitter, const Event& event,
                                                uint64_t sequence, std::shared_ptr<const EmissionFrame<Event>>& frame) {
            if (!record || !record->state || !record->state->active.load(std::memory_order_acquire)) {
                return {};
            }

            if (!Detail::TryConsume(*record->state)) {
                return {};
            }
            if (!frame) {
                frame = std::make_shared<const EmissionFrame<Event>>(
                    event, Detail::ClosureRetainerPtr{std::addressof(emitter)}, Traces(), sequence);
            }

            Detail::Trace(frame->traces, EventTrace{.phase = EventTracePhase::CallbackScheduled,
                                                    .receiver = std::addressof(receiver),
                                                    .emitter = std::addressof(emitter),
                                                    .eventId = Traits::EventIdOf<Event>,
                                                    .callbackId = record->callbackId,
                                                    .subscription = record->id,
                                                    .sequence = frame->sequence});

            return EventDeliveryAct<Event>{frame, Detail::ClosureRetainerPtr{std::addressof(receiver)}, record};
        }

        mutable std::mutex mutex_{};
        std::shared_ptr<const Detail::SubscriptionSnapshot> subscriptions_{};
        std::shared_ptr<const Detail::TraceSnapshot> traces_{};
        std::vector<std::weak_ptr<Detail::SubscriptionState>> inbound_{};
        std::atomic<uint64_t> nextSubscription_{};
        std::atomic<uint64_t> nextTrace_{};
        std::atomic<uint64_t> nextSequence_{};
    };

    /** @brief A group of links produced by wildcard-emitter materialization. */
    struct EventLinks {
        std::vector<EventLink> links{}; /**< Materialized links, one per explicit emitter relation. */
    };

    /** @brief Remove every relation in @p links from future dispatch. */
    inline void Drop(const EventLinks& links) noexcept {
        for (const auto& link : links.links) {
            Drop(link);
        }
    }

    /** @brief Pause every relation in @p links. */
    inline void Suspend(const EventLinks& links) noexcept {
        for (const auto& link : links.links) {
            Suspend(link);
        }
    }

    /** @brief Resume every relation in @p links. */
    inline void Resume(const EventLinks& links) noexcept {
        for (const auto& link : links.links) {
            Resume(link);
        }
    }

    /** @brief Replace every relation budget in @p links. */
    inline void SetBudget(const EventLinks& links, WoreBudget budget) noexcept {
        for (const auto& link : links.links) {
            SetBudget(link, budget);
        }
    }

    /** @brief Explicit scope of emitters used to materialize @ref AnyEmitter relations. */
    class EventSpace {
    public:
        /** @brief Add @p emitter to this explicit event space. */
        EventSpace& Add(BaseUnknown& emitter) {
            std::scoped_lock lock(mutex_);
            BaseUnknown* nucleus = emitter.Nucleus();
            if (nucleus &&
                std::ranges::none_of(emitters_, [nucleus](const WeakRef& ref) { return ref.Get() == nucleus; })) {
                emitters_.push_back(nucleus->GetComponentWeakRef());
            }
            return *this;
        }

        /** @brief Return currently alive emitters explicitly present in this space. */
        [[nodiscard]] std::vector<BaseUnknown*> Emitters() const {
            std::scoped_lock lock(mutex_);
            return emitters_ | std::views::transform([](const WeakRef& ref) { return ref.Get(); }) |
                   std::views::filter([](BaseUnknown* object) { return object != nullptr; }) |
                   std::ranges::to<std::vector>();
        }

    private:
        mutable std::mutex mutex_{};
        std::vector<WeakRef> emitters_{};
    };

    /** @brief Wildcard emitter source bound to an explicit @ref EventSpace. */
    struct AnyEmitter {
        EventSpace& space; /**< Explicit emitter scope. */
    };

    /** @brief Return the event hub associated with @p object. */
    [[nodiscard]] EventPort& Events(BaseUnknown* object);

    namespace Detail {

        template<typename R>
        [[nodiscard]] EventFlow FlowOf(R&& result) {
            if constexpr (std::same_as<std::remove_cvref_t<R>, EventFlow>) {
                return result;
            } else {
                return EventFlow::Continue;
            }
        }

        template<Concept::EventPayload Event, typename Receiver, typename Emitter>
        [[nodiscard]] EventFlow InvokeDefaultOn(EventContext<Event>& context) {
            auto& receiver = static_cast<std::remove_cvref_t<Receiver>&>(context.receiver);
            auto& emitter = static_cast<std::remove_cvref_t<Emitter>&>(context.emitter);
            if constexpr (requires { receiver.On(context.event, emitter); }) {
                if constexpr (std::same_as<void, decltype(receiver.On(context.event, emitter))>) {
                    receiver.On(context.event, emitter);
                    return EventFlow::Continue;
                } else {
                    return FlowOf(receiver.On(context.event, emitter));
                }
            } else if constexpr (requires { receiver.On(context.event, static_cast<BaseUnknown&>(emitter)); }) {
                if constexpr (std::same_as<void,
                                           decltype(receiver.On(context.event, static_cast<BaseUnknown&>(emitter)))>) {
                    receiver.On(context.event, static_cast<BaseUnknown&>(emitter));
                    return EventFlow::Continue;
                } else {
                    return FlowOf(receiver.On(context.event, static_cast<BaseUnknown&>(emitter)));
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
            } else {
                static_assert(false, "Receiver must provide On(event, emitter), On(event), or On(context).");
            }
        }

        template<Concept::EventPayload Event, typename Receiver, typename Emitter, typename Handler>
        [[nodiscard]] EventFlow InvokeReceiverHandler(Handler& handler, EventContext<Event>& context) {
            auto& receiver = static_cast<std::remove_cvref_t<Receiver>&>(context.receiver);
            auto& emitter = static_cast<std::remove_cvref_t<Emitter>&>(context.emitter);
            if constexpr (std::invocable<Handler&, Receiver&, const Event&, Emitter&>) {
                if constexpr (std::same_as<void, std::invoke_result_t<Handler&, Receiver&, const Event&, Emitter&>>) {
                    std::invoke(handler, receiver, context.event, emitter);
                    return EventFlow::Continue;
                } else {
                    return FlowOf(std::invoke(handler, receiver, context.event, emitter));
                }
            } else if constexpr (std::invocable<Handler&, Receiver&, const Event&, BaseUnknown&>) {
                if constexpr (std::same_as<void,
                                           std::invoke_result_t<Handler&, Receiver&, const Event&, BaseUnknown&>>) {
                    std::invoke(handler, receiver, context.event, context.emitter);
                    return EventFlow::Continue;
                } else {
                    return FlowOf(std::invoke(handler, receiver, context.event, context.emitter));
                }
            } else if constexpr (std::invocable<Handler&, Receiver&, const Event&>) {
                if constexpr (std::same_as<void, std::invoke_result_t<Handler&, Receiver&, const Event&>>) {
                    std::invoke(handler, receiver, context.event);
                    return EventFlow::Continue;
                } else {
                    return FlowOf(std::invoke(handler, receiver, context.event));
                }
            } else if constexpr (std::invocable<Handler&, const Event&, EventContext<Event>&>) {
                if constexpr (std::same_as<void, std::invoke_result_t<Handler&, const Event&, EventContext<Event>&>>) {
                    std::invoke(handler, context.event, context);
                    return EventFlow::Continue;
                } else {
                    return FlowOf(std::invoke(handler, context.event, context));
                }
            } else if constexpr (std::invocable<Handler&, const Event&, BaseUnknown&>) {
                if constexpr (std::same_as<void, std::invoke_result_t<Handler&, const Event&, BaseUnknown&>>) {
                    std::invoke(handler, context.event, context.emitter);
                    return EventFlow::Continue;
                } else {
                    return FlowOf(std::invoke(handler, context.event, context.emitter));
                }
            } else if constexpr (std::invocable<Handler&, const Event&>) {
                if constexpr (std::same_as<void, std::invoke_result_t<Handler&, const Event&>>) {
                    std::invoke(handler, context.event);
                    return EventFlow::Continue;
                } else {
                    return FlowOf(std::invoke(handler, context.event));
                }
            } else if constexpr (std::invocable<Handler&>) {
                if constexpr (std::same_as<void, std::invoke_result_t<Handler&>>) {
                    std::invoke(handler);
                    return EventFlow::Continue;
                } else {
                    return FlowOf(std::invoke(handler));
                }
            } else if constexpr (std::invocable<Handler&, EventContext<Event>&>) {
                if constexpr (std::same_as<void, std::invoke_result_t<Handler&, EventContext<Event>&>>) {
                    std::invoke(handler, context);
                    return EventFlow::Continue;
                } else {
                    return FlowOf(std::invoke(handler, context));
                }
            } else {
                static_assert(false, "Handler is not compatible with this receiver/event relation.");
            }
        }

        template<typename Receiver, typename Emitter, typename Handler>
        [[nodiscard]] EventFlow InvokeAnyReceiverHandler(Handler& handler, EventContextBase& context) {
            auto& receiver = static_cast<std::remove_cvref_t<Receiver>&>(context.receiver);
            auto& emitter = static_cast<std::remove_cvref_t<Emitter>&>(context.emitter);
            if constexpr (std::invocable<Handler&, Receiver&, Emitter&, EventContextBase&>) {
                if constexpr (std::same_as<void,
                                           std::invoke_result_t<Handler&, Receiver&, Emitter&, EventContextBase&>>) {
                    std::invoke(handler, receiver, emitter, context);
                    return EventFlow::Continue;
                } else {
                    return FlowOf(std::invoke(handler, receiver, emitter, context));
                }
            } else if constexpr (std::invocable<Handler&, Receiver&, EventContextBase&>) {
                if constexpr (std::same_as<void, std::invoke_result_t<Handler&, Receiver&, EventContextBase&>>) {
                    std::invoke(handler, receiver, context);
                    return EventFlow::Continue;
                } else {
                    return FlowOf(std::invoke(handler, receiver, context));
                }
            } else if constexpr (std::invocable<Handler&, Receiver&, Emitter&>) {
                if constexpr (std::same_as<void, std::invoke_result_t<Handler&, Receiver&, Emitter&>>) {
                    std::invoke(handler, receiver, emitter);
                    return EventFlow::Continue;
                } else {
                    return FlowOf(std::invoke(handler, receiver, emitter));
                }
            } else if constexpr (std::invocable<Handler&, Receiver&, BaseUnknown&>) {
                if constexpr (std::same_as<void, std::invoke_result_t<Handler&, Receiver&, BaseUnknown&>>) {
                    std::invoke(handler, receiver, context.emitter);
                    return EventFlow::Continue;
                } else {
                    return FlowOf(std::invoke(handler, receiver, context.emitter));
                }
            } else if constexpr (std::invocable<Handler&, EventContextBase&>) {
                if constexpr (std::same_as<void, std::invoke_result_t<Handler&, EventContextBase&>>) {
                    std::invoke(handler, context);
                    return EventFlow::Continue;
                } else {
                    return FlowOf(std::invoke(handler, context));
                }
            } else if constexpr (std::invocable<Handler&, BaseUnknown&>) {
                if constexpr (std::same_as<void, std::invoke_result_t<Handler&, BaseUnknown&>>) {
                    std::invoke(handler, context.emitter);
                    return EventFlow::Continue;
                } else {
                    return FlowOf(std::invoke(handler, context.emitter));
                }
            } else if constexpr (std::invocable<Handler&>) {
                if constexpr (std::same_as<void, std::invoke_result_t<Handler&>>) {
                    std::invoke(handler);
                    return EventFlow::Continue;
                } else {
                    return FlowOf(std::invoke(handler));
                }
            } else {
                static_assert(false, "Handler is not compatible with this wildcard event relation.");
            }
        }

    } // namespace Detail

    /**
     * @brief Listen to one concrete event emitted by one concrete emitter using the receiver's default On overload.
     */

    template<Concept::ComClass Receiver, Concept::ComClass Emitter, Concept::EventPayload Event>
    [[nodiscard]] EventLink Listen(Receiver& receiver, Emitter& emitter, const Event&, WoreBudget budget = Persistent) {
        return Listen(receiver, emitter, Event{}, EventOptions{.budget = budget});
    }

    /**
     * @brief Listen to one concrete event emitted by one concrete emitter using the receiver's default On overload.
     */
    template<Concept::ComClass Receiver, Concept::ComClass Emitter, Concept::EventPayload Event>
    [[nodiscard]] EventLink Listen(Receiver& receiver, Emitter& emitter, const Event&, EventOptions options) {
        static_assert(Traits::CanEmitEvent<Emitter, Event>, "Emitter class does not emit this event payload.");
        static_assert(Traits::CanAcceptEvent<Receiver, Event>, "Receiver class does not accept this event payload.");
        return Events(emitter).template ListenTyped<Event>(
            receiver.Nucleus(),
            [](EventContext<Event>& context) { return Detail::InvokeDefaultOn<Event, Receiver, Emitter>(context); },
            std::move(options));
    }

    /** @brief Listen to one concrete event emitted by one concrete emitter using @p handler. */
    template<Concept::ComClass Receiver, Concept::ComClass Emitter, Concept::EventPayload Event, typename Handler>
    [[nodiscard]] EventLink Listen(Receiver& receiver, Emitter& emitter, const Event&, WoreBudget budget,
                                   Handler&& handler) {
        return Listen(receiver, emitter, Event{}, EventOptions{.budget = budget}, std::forward<Handler>(handler));
    }

    /** @brief Listen to one concrete event emitted by one concrete emitter using @p handler. */
    template<Concept::ComClass Receiver, Concept::ComClass Emitter, Concept::EventPayload Event, typename Handler>
    [[nodiscard]] EventLink Listen(Receiver& receiver, Emitter& emitter, const Event&, EventOptions options,
                                   Handler&& handler) {
        static_assert(Traits::CanEmitEvent<Emitter, Event>, "Emitter class does not emit this event payload.");
        static_assert(Traits::CanAcceptEvent<Receiver, Event>, "Receiver class does not accept this event payload.");
        using F = std::remove_cvref_t<Handler>;
        return Events(emitter).template ListenTyped<Event>(
            receiver.Nucleus(),
            [handler = F(std::forward<Handler>(handler))](EventContext<Event>& context) mutable {
                return Detail::InvokeReceiverHandler<Event, Receiver, Emitter>(handler, context);
            },
            std::move(options));
    }

    /** @brief Listen to one concrete event with a lambda receiver owned by @p emitter. */
    template<Concept::ComClass Emitter, Concept::EventPayload Event, typename Handler>
    [[nodiscard]] EventLink Listen(Emitter& emitter, const Event&, WoreBudget budget, Handler&& handler) {
        return Listen(emitter, Event{}, EventOptions{.budget = budget}, std::forward<Handler>(handler));
    }

    /** @brief Listen to one concrete event with a lambda receiver owned by @p emitter. */
    template<Concept::ComClass Emitter, Concept::EventPayload Event, typename Handler>
    [[nodiscard]] EventLink Listen(Emitter& emitter, const Event&, EventOptions options, Handler&& handler) {
        static_assert(Traits::CanEmitEvent<Emitter, Event>, "Emitter class does not emit this event payload.");
        return Events(emitter).template ListenTyped<Event>(nullptr, std::forward<Handler>(handler), std::move(options));
    }

    /** @brief Listen to every event emitted by one concrete emitter. */
    template<Concept::ComClass Receiver, Concept::ComClass Emitter, typename Handler>
    [[nodiscard]] EventLink Listen(Receiver& receiver, Emitter& emitter, AnyEventTag, WoreBudget budget,
                                   Handler&& handler) {
        return Listen(receiver, emitter, AnyEvent, EventOptions{.budget = budget}, std::forward<Handler>(handler));
    }

    /** @brief Listen to every event emitted by one concrete emitter. */
    template<Concept::ComClass Receiver, Concept::ComClass Emitter, typename Handler>
    [[nodiscard]] EventLink Listen(Receiver& receiver, Emitter& emitter, AnyEventTag, EventOptions options,
                                   Handler&& handler) {
        using F = std::remove_cvref_t<Handler>;
        return Events(emitter).ListenAny(
            receiver.Nucleus(),
            [handler = F(std::forward<Handler>(handler))](EventContextBase& context) mutable {
                return Detail::InvokeAnyReceiverHandler<Receiver, Emitter>(handler, context);
            },
            std::move(options));
    }

    /** @brief Listen to one concrete event from every emitter explicitly present in @p source. */
    template<Concept::ComClass Receiver, Concept::EventPayload Event, typename Handler>
    [[nodiscard]] EventLinks Listen(Receiver& receiver, AnyEmitter source, const Event&, WoreBudget budget,
                                    Handler&& handler) {
        return Listen(receiver, source, Event{}, EventOptions{.budget = budget}, std::forward<Handler>(handler));
    }

    /** @brief Listen to one concrete event from every emitter explicitly present in @p source. */
    template<Concept::ComClass Receiver, Concept::EventPayload Event, typename Handler>
    [[nodiscard]] EventLinks Listen(Receiver& receiver, AnyEmitter source, const Event&, EventOptions options,
                                    Handler&& handler) {
        static_assert(Traits::CanAcceptEvent<Receiver, Event>, "Receiver class does not accept this event payload.");
        using F = std::remove_cvref_t<Handler>;
        static_assert(std::copy_constructible<F>, "AnyEmitter materialization requires a copy-constructible handler.");
        EventLinks result;
        auto emitters = source.space.Emitters();
        result.links.reserve(emitters.size());
        F stored(std::forward<Handler>(handler));
        for (BaseUnknown* emitter : emitters) {
            F copy(stored);
            result.links.push_back(Events(*emitter).template ListenTyped<Event>(
                receiver.Nucleus(),
                [handler = std::move(copy)](EventContext<Event>& context) mutable {
                    return Detail::InvokeReceiverHandler<Event, Receiver, BaseUnknown>(handler, context);
                },
                options));
        }
        return result;
    }

    /** @brief Listen to every event from every emitter explicitly present in @p source. */
    template<Concept::ComClass Receiver, typename Handler>
    [[nodiscard]] EventLinks Listen(Receiver& receiver, AnyEmitter source, AnyEventTag, WoreBudget budget,
                                    Handler&& handler) {
        return Listen(receiver, source, AnyEvent, EventOptions{.budget = budget}, std::forward<Handler>(handler));
    }

    /** @brief Listen to every event from every emitter explicitly present in @p source. */
    template<Concept::ComClass Receiver, typename Handler>
    [[nodiscard]] EventLinks Listen(Receiver& receiver, AnyEmitter source, AnyEventTag, EventOptions options,
                                    Handler&& handler) {
        using F = std::remove_cvref_t<Handler>;
        static_assert(std::copy_constructible<F>, "AnyEmitter materialization requires a copy-constructible handler.");
        EventLinks result;
        auto emitters = source.space.Emitters();
        result.links.reserve(emitters.size());
        F stored(std::forward<Handler>(handler));
        for (BaseUnknown* emitter : emitters) {
            F copy(stored);
            result.links.push_back(Events(*emitter).ListenAny(
                receiver.Nucleus(),
                [handler = std::move(copy)](EventContextBase& context) mutable {
                    return Detail::InvokeAnyReceiverHandler<Receiver, BaseUnknown>(handler, context);
                },
                options));
        }
        return result;
    }

    /** @brief Typed event emission with compile-time emitter policy check. */
    template<Concept::ComClass Emitter, Concept::EventPayload Event>
    void Emit(Emitter& emitter, const Event& event) {
        static_assert(Traits::CanEmitEvent<Emitter, Event>, "Emitter class does not emit this event payload.");
        Events(emitter).Emit(event, std::addressof(emitter));
    }

    /** @brief Typed event emission with a default-constructed payload. */
    template<Concept::EventPayload Event, Concept::ComClass Emitter>
        requires std::default_initializable<Event>
    void Emit(Emitter& emitter) {
        static_assert(Traits::CanEmitEvent<Emitter, Event>, "Emitter class does not emit this event payload.");
        Events(emitter).template Emit<Event>(std::addressof(emitter));
    }

    /** @brief Typed event emission whose selected callbacks are scheduled on @p scheduler. */
    template<Concept::ComClass Emitter, stdexec::scheduler Scheduler, Concept::EventPayload Event>
    void EmitOn(Emitter& emitter, Scheduler scheduler, const Event& event) {
        static_assert(Traits::CanEmitEvent<Emitter, Event>, "Emitter class does not emit this event payload.");
        Events(emitter).EmitOn(std::move(scheduler), event, std::addressof(emitter));
    }

    /** @brief Typed scheduled event emission with a default-constructed payload. */
    template<Concept::EventPayload Event, Concept::ComClass Emitter, stdexec::scheduler Scheduler>
        requires std::default_initializable<Event>
    void EmitOn(Emitter& emitter, Scheduler scheduler) {
        static_assert(Traits::CanEmitEvent<Emitter, Event>, "Emitter class does not emit this event payload.");
        Events(emitter).EmitOn(std::move(scheduler), Event{}, std::addressof(emitter));
    }

} // namespace Sora::Kernel

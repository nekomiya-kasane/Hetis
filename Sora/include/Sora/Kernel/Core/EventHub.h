/**
 * @file EventHub.h
 * @brief Per-nucleus event/callback hub, event type-list traits, subscriptions, schedulers, and trace hooks.
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/Traits/InheritanceTraits.h>
#include <Sora/Core/Traits/ScopeTraits.h>
#include <Sora/Kernel/Core/Traits.h>
#include <Sora/Kernel/Core/BaseObject.h>
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

namespace Sora::Kernel {

    namespace Event {

        struct BaseEvent {};

        struct AnyEvent : BaseEvent {};

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
                                                "' is not a class reflection — only classes can be events.");
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

        /** @brief Event payload class accepted by the event hub. */
        template<typename T>
        concept EventPayload =
            std::is_class_v<std::remove_cvref_t<T>> && std::copy_constructible<std::remove_cvref_t<T>>;

        /** @brief Callback policy tag; tags are ordinary class types, not event payloads. */
        template<typename T>
        concept CallbackTagClass = std::is_class_v<std::remove_cvref_t<T>>;

        static_assert(EventClass<Event::AnyEvent>, "Event::AnyEvent must satisfy EventClass concept.");
        static_assert(EventClass<Event::BaseEvent>, "Event::BaseEvent must satisfy EventClass concept.");

    } // namespace Concept

    /** @brief Default callback policy tag used when no more specific tag is requested. */
    struct DefaultCallbackTag {};

    namespace event {

        /** @brief Begin a chainable event or callback TypeList declaration. */
        [[nodiscard]] consteval auto types() noexcept {
            return Sora::Traits::TypeList<>{};
        }

    } // namespace event

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

            if (args.size() != 1 && std::ranges::contains(args, ^^Sora::Kernel::Event::AnyEvent)) {
                throw std::define_static_string(
                    "Meta::IsEventTypeList: Event::AnyEvent cannot be combined with other event types in a list.");
            }
            return true;
        }

    } // namespace Meta

    namespace Concept {

        /** @brief Nested event type-list declaration whose entries are event payloads. */
        template<typename T>
        concept EventTypeList = Meta::IsEventTypeList(^^T);

        /** @brief Nested callback-tag type-list declaration whose entries are callback tags. */
        template<typename T>
        concept CallbackTagTypeList = Meta::IsCallbackTagList(^^T);

        /** @brief Class that declares emitted events through a nested @c Emits alias. */
        template<typename T>
        concept EmittableClass = requires { typename std::remove_cvref_t<T>::Emits; } &&
                                 EventTypeList<typename std::remove_cvref_t<T>::Emits>;

        /** @brief Class that declares accepted events through a nested @c Accepts alias. */
        template<typename T>
        concept AcceptableClass = requires { typename std::remove_cvref_t<T>::Accepts; } &&
                                  EventTypeList<typename std::remove_cvref_t<T>::Accepts>;

        /** @brief Class that declares callback tags through a nested @c Callbacks alias. */
        template<typename T>
        concept CallbackAttachableClass = requires { typename std::remove_cvref_t<T>::Callbacks; } &&
                                          CallbackTagTypeList<typename std::remove_cvref_t<T>::Callbacks>;

    } // namespace Concept

    namespace Traits {

        /** @brief Stable event/callback tag identifier derived from the reflected ABI digest of @p T. */
        template<typename T>
            requires std::is_class_v<std::remove_cvref_t<T>>
        inline constexpr EventId EventIdOf{Sora::Traits::AbiDigestOf<std::remove_cvref_t<T>, true>};

        namespace Detail {

            template<Concept::ComClass T>
            consteval std::meta::info DeclaredTypeListInfoOf(std::string_view name, std::invocable auto predicate) {
                auto info = std::meta::dealias(^^T);
                if (!Sora::Kernel::Meta::IsComClass(info)) {
                    throw std::define_static_string(
                        std::format("Traits::DeclaredTypeListOf: '{}' is not a COM class reflection.",
                                    Sora::Meta::DisplayStringOf(info)));
                }

                std::vector<std::meta::info> result;
                auto chain = Sora::Meta::InheritanceChainUntil(info, ^^Sora::Kernel::BaseUnknown);
                for (auto scope : chain | std::views::reverse) {
                    auto list = Sora::Meta::FindDirectTypeMemberOf(scope, "Emits");
                    if (list == std::meta::info{}) {
                        continue;
                    }
                    if (!predicate(list)) {
                        throw std::define_static_string(
                            std::format("Traits::DeclaredTypeListOf: '{}' is not a valid event type list.",
                                        Sora::Meta::DisplayStringOf(list)));
                    }
                    Sora::Meta::AppendTypeList(result, list);
                }
                return std::meta::substitute(^^Sora::Traits::TypeList, result);
            }

        } // namespace Detail

        /** @brief Type-list of event payloads emitted by @p T after inherited declarations are merged. */
        template<Concept::ComClass T>
        using EmittedEventsOf = typename [:Detail::DeclaredTypeListInfoOf<T>("Emits", Meta::IsEventTypeList):];

        /** @brief Type-list of event payloads accepted by @p T after inherited declarations are merged. */
        template<Concept::ComClass T>
        using AcceptedEventsOf = typename [:Detail::DeclaredTypeListInfoOf<T>("Accepts", Meta::IsEventTypeList):];

        /** @brief Type-list of callback tags allowed by @p T after inherited declarations are merged. */
        template<Concept::ComClass T>
        using CallbackTagsOf = typename [:Detail::DeclaredTypeListInfoOf<T>("Callbacks", Meta::IsCallbackTagList):];

    } // namespace Traits

    namespace Traits {

        /** @brief Return whether @p List permits event payload @p Event, honoring @ref Event::AnyEvent. */
        template<typename List, typename Event>
        consteval bool EventListContains() {
            return Sora::Traits::Contains<List, std::remove_cvref_t<Event>> ||
                   Sora::Traits::Contains<List, Sora::Kernel::Event::AnyEvent>;
        }

        /**
         * @brief Return whether @p T can emit @p Event.
         * @details Classes emit only payload types declared by their own type-list alias or inherited from direct
         * bases.
         */
        template<typename T, typename Event>
        inline constexpr bool CanEmitEvent = [] consteval {
            using Emits = Traits::EmittedEventsOf<std::remove_cvref_t<T>>;
            return Concept::ComClass<std::remove_cvref_t<T>> && Concept::EventPayload<std::remove_cvref_t<Event>> &&
                   (Sora::Traits::Contains<Emits, std::remove_cvref_t<Event>> ||
                    Sora::Traits::Contains<Emits, Sora::Kernel::Event::AnyEvent>);
        }();

        /**
         * @brief Return whether @p T can receive @p Event.
         * @details Classes accept only payload types declared by their own type-list alias or inherited from direct
         * bases.
         */
        template<typename T, typename Event>
        inline constexpr bool CanAcceptEvent = [] consteval {
            using Accepts = Traits::AcceptedEventsOf<std::remove_cvref_t<T>>;
            return Concept::ComClass<std::remove_cvref_t<T>> && Concept::EventPayload<std::remove_cvref_t<Event>> &&
                   (Sora::Traits::Contains<Accepts, std::remove_cvref_t<Event>> ||
                    Sora::Traits::Contains<Accepts, Sora::Kernel::Event::AnyEvent>);
        }();

        /**
         * @brief Return whether @p T can attach a callback tagged by @p CallbackTag.
         * @details Classes allow only callback tags declared by their own type-list alias or inherited from direct
         * bases.
         */
        template<typename T, typename CallbackTag>
        inline constexpr bool CanAttachCallback = [] consteval {
            using Callbacks = Traits::CallbackTagsOf<std::remove_cvref_t<T>>;
            return Concept::ComClass<std::remove_cvref_t<T>> &&
                   Concept::CallbackTagClass<std::remove_cvref_t<CallbackTag>> &&
                   Sora::Traits::Contains<Callbacks, std::remove_cvref_t<CallbackTag>>;
        }();

    } // namespace Traits

    namespace Concept {

        /** @brief Object-model class whose emitted-event list permits @p Event. */
        template<typename T, typename Event>
        concept EventEmitter = Traits::CanEmitEvent<T, Event>;

        /** @brief Object-model class whose accepted-event list permits @p Event. */
        template<typename T, typename Event>
        concept EventReceiver = Traits::CanAcceptEvent<T, Event>;

        /** @brief Object-model class whose callback-tag list permits @p CallbackTag. */
        template<typename T, typename CallbackTag>
        concept CallbackAttachable = Traits::CanAttachCallback<T, CallbackTag>;

    } // namespace Concept

    /** @brief Completion decision returned by an event callback. */
    enum class EventFlow : uint8_t {
        Continue = 0, /**< Continue dispatching later callbacks in the same receive pass. */
        Stop = 1,     /**< Stop dispatching later callbacks in the same receive pass. */
    };

    /** @brief Trace phase for observing an object's event receive process. */
    enum class EventTracePhase : uint8_t {
        ReceiveBegin,
        ConditionRejected,
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
    template<Concept::EventPayload Event>
    struct EventContext : EventContextBase {
        const Event& event; /**< Event payload being delivered. */
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

    namespace Detail {

        template<typename>
        class UniqueFunction;

        /** @brief Minimal move-only callable erasure used until the active libc++ ships std::move_only_function. */
        template<typename R, typename... Args>
        class UniqueFunction<R(Args...)> {
        public:
            UniqueFunction() noexcept = default;
            UniqueFunction(std::nullptr_t) noexcept {}

            template<typename F>
                requires(!std::same_as<std::remove_cvref_t<F>, UniqueFunction> &&
                         std::invocable<std::remove_cvref_t<F>&, Args...>)
            UniqueFunction(F&& fn) {
                using Fn = std::remove_cvref_t<F>;
                object_ = new Fn(std::forward<F>(fn));
                invoke_ = [](void* object, Args... args) -> R {
                    if constexpr (std::same_as<R, void>) {
                        std::invoke(*static_cast<Fn*>(object), std::forward<Args>(args)...);
                    } else {
                        return std::invoke(*static_cast<Fn*>(object), std::forward<Args>(args)...);
                    }
                };
                destroy_ = [](void* object) noexcept { delete static_cast<Fn*>(object); };
            }

            UniqueFunction(const UniqueFunction&) = delete;
            UniqueFunction& operator=(const UniqueFunction&) = delete;

            UniqueFunction(UniqueFunction&& other) noexcept
                : object_(std::exchange(other.object_, nullptr)),
                  invoke_(std::exchange(other.invoke_, nullptr)),
                  destroy_(std::exchange(other.destroy_, nullptr)) {}

            UniqueFunction& operator=(UniqueFunction&& other) noexcept {
                if (this != std::addressof(other)) {
                    Reset();
                    object_ = std::exchange(other.object_, nullptr);
                    invoke_ = std::exchange(other.invoke_, nullptr);
                    destroy_ = std::exchange(other.destroy_, nullptr);
                }
                return *this;
            }

            ~UniqueFunction() noexcept { Reset(); }

            explicit operator bool() const noexcept { return invoke_ != nullptr; }

            R operator()(Args... args) {
                if constexpr (std::same_as<R, void>) {
                    invoke_(object_, std::forward<Args>(args)...);
                } else {
                    return invoke_(object_, std::forward<Args>(args)...);
                }
            }

            void Reset() noexcept {
                if (destroy_) {
                    destroy_(object_);
                }
                object_ = nullptr;
                invoke_ = nullptr;
                destroy_ = nullptr;
            }

        private:
            void* object_{};
            R (*invoke_)(void*, Args...){};
            void (*destroy_)(void*) noexcept {};
        };

    } // namespace Detail

    /** @brief Move-only scheduled work item used by event schedulers. */
    using EventTask = Detail::UniqueFunction<void()>;

    /** @brief Owning type-erased scheduler for deferred event callback invocation. */
    class EventScheduler {
    public:
        EventScheduler() noexcept = default;
        EventScheduler(const EventScheduler&) = delete;
        EventScheduler& operator=(const EventScheduler&) = delete;
        virtual ~EventScheduler() noexcept = default;

        /** @brief Enqueue @p task for eventual execution. */
        virtual void Schedule(EventTask task) const = 0;
    };

    /** @brief Shared scheduler handle. Null means immediate in-thread invocation. */
    using EventSchedulerPtr = std::shared_ptr<const EventScheduler>;

    namespace Detail {

        template<typename S>
        concept SchedulerObject = requires(S& scheduler, EventTask task) {
            { scheduler.Schedule(std::move(task)) };
        } || requires(S& scheduler, EventTask task) {
            { std::invoke(scheduler, std::move(task)) };
        };

        template<typename S>
        class SchedulerModel final : public EventScheduler {
        public:
            explicit SchedulerModel(S scheduler) : scheduler_(std::move(scheduler)) {}

            void Schedule(EventTask task) const override {
                if constexpr (requires(S& scheduler, EventTask work) { scheduler.Schedule(std::move(work)); }) {
                    scheduler_.Schedule(std::move(task));
                } else {
                    std::invoke(scheduler_, std::move(task));
                }
            }

        private:
            mutable S scheduler_;
        };

        template<typename>
        inline constexpr bool kAlwaysFalse = false;

    } // namespace Detail

    /** @brief Build an event scheduler from either a @c Schedule(EventTask) object or a callable. */
    template<typename Scheduler>
        requires Detail::SchedulerObject<std::remove_cvref_t<Scheduler>>
    [[nodiscard]] EventSchedulerPtr MakeEventScheduler(Scheduler&& scheduler) {
        using S = std::remove_cvref_t<Scheduler>;
        return std::make_shared<Detail::SchedulerModel<S>>(std::forward<Scheduler>(scheduler));
    }

    /** @brief Subscription behavior knobs shared by all event payload types. */
    struct EventSubscriptionOptions {
        bool once{};                   /**< Disable the subscription after the first accepted event. */
        int32_t priority{};            /**< Higher priority callbacks run first for immediate dispatch. */
        EventSchedulerPtr scheduler{}; /**< Null for immediate invocation; non-null for deferred invocation. */
        EventId callbackId{Traits::EventIdOf<DefaultCallbackTag>}; /**< Callback policy tag. */
    };

    namespace Detail {

        struct SubscriptionState {
            std::atomic<bool> active{true};
        };

        struct TraceState {
            std::atomic<bool> active{true};
        };

        /** @brief Move-only intrusive strong anchor used by scheduler-owned callback tasks. */
        class StrongObjectRef {
        public:
            StrongObjectRef() noexcept = default;

            explicit StrongObjectRef(BaseUnknown* object) noexcept : object_(object ? object->Nucleus() : nullptr) {
                Retain(object_);
            }

            StrongObjectRef(const StrongObjectRef&) = delete;
            StrongObjectRef& operator=(const StrongObjectRef&) = delete;

            StrongObjectRef(StrongObjectRef&& other) noexcept : object_(std::exchange(other.object_, nullptr)) {}

            StrongObjectRef& operator=(StrongObjectRef&& other) noexcept {
                if (this != std::addressof(other)) {
                    Release(object_);
                    object_ = std::exchange(other.object_, nullptr);
                }
                return *this;
            }

            ~StrongObjectRef() noexcept { Release(object_); }

            [[nodiscard]] BaseUnknown* Get() const noexcept { return object_; }

        private:
            BaseUnknown* object_{};
        };

    } // namespace Detail

    /** @brief RAII-capable handle for a callback subscription. */
    class EventSubscription {
    public:
        EventSubscription() noexcept = default;
        explicit EventSubscription(std::weak_ptr<Detail::SubscriptionState> state) noexcept
            : state_(std::move(state)) {}

        /** @brief Cancel the subscription if its hub still exists. */
        void Cancel() noexcept {
            if (auto state = state_.lock()) {
                state->active.store(false, std::memory_order_release);
            }
        }

        /** @brief Return whether this handle still refers to an active subscription. */
        [[nodiscard]] bool Active() const noexcept {
            auto state = state_.lock();
            return state && state->active.load(std::memory_order_acquire);
        }

    private:
        std::weak_ptr<Detail::SubscriptionState> state_{};
    };

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

        using ErasedPredicate = UniqueFunction<bool(const EventContextBase&, const void*)>;
        using ErasedCallback = UniqueFunction<EventFlow(EventContextBase&, const void*)>;
        using ErasedTrace = UniqueFunction<void(const EventTrace&)>;

        struct SubscriptionRecord {
            std::shared_ptr<SubscriptionState> state{};
            ErasedPredicate condition{};
            ErasedCallback callback{};
            EventSchedulerPtr scheduler{};
            EventId eventId{};
            EventId callbackId{};
            uint64_t id{};
            uint64_t order{};
            int32_t priority{};
            bool once{};
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

        template<typename Result>
        [[nodiscard]] constexpr EventFlow ToEventFlow(Result&& result) {
            using R = std::remove_cvref_t<Result>;
            if constexpr (std::same_as<R, EventFlow>) {
                return result;
            } else if constexpr (std::same_as<R, void>) {
                return EventFlow::Continue;
            } else {
                static_assert(kAlwaysFalse<R>, "Event callback must return void or Sora::Kernel::EventFlow.");
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
                static_assert(kAlwaysFalse<Callback>, "Event callback must accept EventContext<Event>&, "
                                                      "(const Event&, EventContext<Event>&), const Event&, or no "
                                                      "arguments.");
            }
        }

        template<Concept::EventPayload Event, typename Condition>
        [[nodiscard]] bool InvokeTypedCondition(Condition& condition, const EventContextBase& base,
                                                const void* payload) {
            const auto& context = static_cast<const EventContext<Event>&>(base);
            const Event& event = *static_cast<const Event*>(payload);
            if constexpr (std::predicate<Condition&, const Event&, const EventContext<Event>&>) {
                return std::invoke(condition, event, context);
            } else if constexpr (std::predicate<Condition&, const Event&>) {
                return std::invoke(condition, event);
            } else if constexpr (std::predicate<Condition&, const EventContext<Event>&>) {
                return std::invoke(condition, context);
            } else if constexpr (std::predicate<Condition&>) {
                return std::invoke(condition);
            } else {
                static_assert(kAlwaysFalse<Condition>, "Event condition must be a bool predicate over "
                                                       "(const Event&, const EventContext<Event>&), const Event&, "
                                                       "const EventContext<Event>&, or no arguments.");
            }
        }

    } // namespace Detail

    /** @brief Cold callback manager attached to one closure nucleus. */
    class EventHub {
    public:
        /** @brief Construct for @p owner, which must be the closure nucleus. */
        explicit EventHub(BaseUnknown& owner) noexcept;
        EventHub(const EventHub&) = delete;
        EventHub& operator=(const EventHub&) = delete;
        ~EventHub() noexcept;

        /** @brief Return the closure nucleus that owns this hub. */
        [[nodiscard]] BaseUnknown& Owner() noexcept { return *owner_; }

        /** @brief Return the closure nucleus that owns this hub. */
        [[nodiscard]] const BaseUnknown& Owner() const noexcept { return *owner_; }

        /** @brief Return whether the owning nucleus is still alive. */
        [[nodiscard]] bool OwnerAlive() const noexcept { return ownerWeak_.Get() == owner_; }

        /** @brief Attach @p callback for payload type @p Event. */
        template<Concept::EventPayload Event, typename Callback>
        [[nodiscard]] EventSubscription Subscribe(Callback&& callback, EventSubscriptionOptions options = {}) {
            assert(OwnerAlive() && "EventHub used after its owner nucleus was destroyed");
            return SubscribeIf<Event>([] { return true; }, std::forward<Callback>(callback), std::move(options));
        }

        /** @brief Attach @p callback for payload type @p Event, guarded by @p condition. */
        template<Concept::EventPayload Event, typename Condition, typename Callback>
        [[nodiscard]] EventSubscription SubscribeIf(Condition&& condition, Callback&& callback,
                                                    EventSubscriptionOptions options = {}) {
            assert(OwnerAlive() && "EventHub used after its owner nucleus was destroyed");
            using C = std::remove_cvref_t<Condition>;
            using F = std::remove_cvref_t<Callback>;
            static_assert(std::move_constructible<C>);
            static_assert(std::move_constructible<F>);

            auto typedCondition = C(std::forward<Condition>(condition));
            auto typedCallback = F(std::forward<Callback>(callback));
            Detail::SubscriptionRecord record{
                .state = std::make_shared<Detail::SubscriptionState>(),
                .condition =
                    [condition = std::move(typedCondition)](const EventContextBase& context,
                                                            const void* payload) mutable {
                        return Detail::InvokeTypedCondition<Event>(condition, context, payload);
                    },
                .callback =
                    [callback = std::move(typedCallback)](EventContextBase& context, const void* payload) mutable {
                        return Detail::InvokeTypedCallback<Event>(callback, context, payload);
                    },
                .scheduler = std::move(options.scheduler),
                .eventId = Traits::EventIdOf<Event>,
                .callbackId = options.callbackId,
                .priority = options.priority,
                .once = options.once,
            };
            return AddSubscription(std::move(record));
        }

        /** @brief Deliver @p event to this hub as an accepted event. */
        template<Concept::EventPayload Event>
        void Emit(const Event& event, BaseUnknown* emitter = nullptr) {
            assert(OwnerAlive() && "EventHub used after its owner nucleus was destroyed");
            BaseUnknown* actualEmitter = emitter ? emitter->Nucleus() : owner_;
            if (!actualEmitter) {
                actualEmitter = owner_;
            }

            const uint64_t sequence = nextSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
            auto subscriptions = Subscriptions();
            Trace(EventTrace{.phase = EventTracePhase::ReceiveBegin,
                             .receiver = owner_,
                             .emitter = actualEmitter,
                             .eventId = Traits::EventIdOf<Event>,
                             .sequence = sequence});

            EventContext<Event> context{{.receiver = *owner_,
                                         .emitter = *actualEmitter,
                                         .eventId = Traits::EventIdOf<Event>,
                                         .sequence = sequence},
                                        event};
            std::shared_ptr<const Event> deferredPayload;
            for (const auto& record : subscriptions->records) {
                if (!record || record->eventId != Traits::EventIdOf<Event>) {
                    continue;
                }
                context.callbackId = record->callbackId;
                context.subscription = record->id;
                const EventFlow flow = DispatchRecord(record, context, event, deferredPayload);
                if (flow == EventFlow::Stop) {
                    break;
                }
            }

            Trace(EventTrace{.phase = EventTracePhase::ReceiveEnd,
                             .receiver = owner_,
                             .emitter = actualEmitter,
                             .eventId = Traits::EventIdOf<Event>,
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
            assert(OwnerAlive() && "EventHub used after its owner nucleus was destroyed");
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
        [[nodiscard]] EventSubscription AddSubscription(Detail::SubscriptionRecord record);
        [[nodiscard]] EventTraceHook AddTrace(Detail::TraceRecord record);
        [[nodiscard]] std::shared_ptr<const Detail::SubscriptionSnapshot> Subscriptions() const;
        [[nodiscard]] std::shared_ptr<const Detail::TraceSnapshot> Traces() const;
        void Trace(const EventTrace& trace) const;
        static void Trace(std::shared_ptr<const Detail::TraceSnapshot> traces, const EventTrace& trace);
        EventFlow DispatchRecord(const std::shared_ptr<Detail::SubscriptionRecord>& record, EventContextBase& context,
                                 const void* event, std::shared_ptr<const void>& deferredPayload) = delete;

        template<Concept::EventPayload Event>
        EventFlow DispatchRecord(const std::shared_ptr<Detail::SubscriptionRecord>& record,
                                 EventContext<Event>& context, const Event& event,
                                 std::shared_ptr<const Event>& deferredPayload) {
            if (!record->state->active.load(std::memory_order_acquire)) {
                return EventFlow::Continue;
            }
            if (!record->condition(context, std::addressof(event))) {
                Trace(EventTrace{.phase = EventTracePhase::ConditionRejected,
                                 .receiver = owner_,
                                 .emitter = std::addressof(context.emitter),
                                 .eventId = context.eventId,
                                 .callbackId = record->callbackId,
                                 .subscription = record->id,
                                 .sequence = context.sequence});
                return EventFlow::Continue;
            }
            if (record->once && !record->state->active.exchange(false, std::memory_order_acq_rel)) {
                return EventFlow::Continue;
            }

            if (!record->scheduler) {
                Trace(EventTrace{.phase = EventTracePhase::CallbackBegin,
                                 .receiver = owner_,
                                 .emitter = std::addressof(context.emitter),
                                 .eventId = context.eventId,
                                 .callbackId = record->callbackId,
                                 .subscription = record->id,
                                 .sequence = context.sequence});
                const EventFlow flow = record->callback(context, std::addressof(event));
                Trace(EventTrace{.phase = EventTracePhase::CallbackEnd,
                                 .receiver = owner_,
                                 .emitter = std::addressof(context.emitter),
                                 .eventId = context.eventId,
                                 .callbackId = record->callbackId,
                                 .subscription = record->id,
                                 .sequence = context.sequence});
                return flow;
            }

            if (!deferredPayload) {
                deferredPayload = std::make_shared<const Event>(event);
            }

            auto state = record->state;
            auto scheduler = record->scheduler;
            Detail::StrongObjectRef receiver{owner_};
            Detail::StrongObjectRef emitter{std::addressof(context.emitter)};
            auto payload = deferredPayload;
            EventId eventId = context.eventId;
            EventId callbackId = record->callbackId;
            uint64_t subscription = record->id;
            uint64_t sequence = context.sequence;
            auto traces = Traces();

            Trace(EventTrace{.phase = EventTracePhase::CallbackScheduled,
                             .receiver = owner_,
                             .emitter = std::addressof(context.emitter),
                             .eventId = eventId,
                             .callbackId = callbackId,
                             .subscription = subscription,
                             .sequence = sequence});

            scheduler->Schedule([state = std::move(state), payload = std::move(payload), receiver = std::move(receiver),
                                 emitter = std::move(emitter), eventId, callbackId, subscription, sequence, record,
                                 traces = std::move(traces)]() mutable {
                BaseUnknown* receiverObject = receiver.Get();
                BaseUnknown* emitterObject = emitter.Get();
                if (!state->active.load(std::memory_order_acquire)) {
                    Trace(traces, EventTrace{.phase = EventTracePhase::CallbackCanceled,
                                             .receiver = receiverObject,
                                             .emitter = emitterObject,
                                             .eventId = eventId,
                                             .callbackId = callbackId,
                                             .subscription = subscription,
                                             .sequence = sequence});
                    return;
                }
                EventContext<Event> asyncContext{{.receiver = *receiverObject,
                                                  .emitter = *emitterObject,
                                                  .eventId = eventId,
                                                  .callbackId = callbackId,
                                                  .subscription = subscription,
                                                  .sequence = sequence},
                                                 *payload};
                Trace(traces, EventTrace{.phase = EventTracePhase::CallbackBegin,
                                         .receiver = receiverObject,
                                         .emitter = emitterObject,
                                         .eventId = eventId,
                                         .callbackId = callbackId,
                                         .subscription = subscription,
                                         .sequence = sequence});
                record->callback(asyncContext, payload.get());
                Trace(traces, EventTrace{.phase = EventTracePhase::CallbackEnd,
                                         .receiver = receiverObject,
                                         .emitter = emitterObject,
                                         .eventId = eventId,
                                         .callbackId = callbackId,
                                         .subscription = subscription,
                                         .sequence = sequence});
            });
            return EventFlow::Continue;
        }

        BaseUnknown* owner_{};
        WeakRef ownerWeak_{};
        mutable std::mutex mutex_{};
        std::shared_ptr<const Detail::SubscriptionSnapshot> subscriptions_{};
        std::shared_ptr<const Detail::TraceSnapshot> traces_{};
        std::atomic<uint64_t> nextSubscription_{};
        std::atomic<uint64_t> nextTrace_{};
        std::atomic<uint64_t> nextSequence_{};
    };

    /** @brief Return the event hub associated with @p object. */
    [[nodiscard]] EventHub& Events(BaseUnknown& object);

    /** @brief Return the event hub associated with @p object. */
    [[nodiscard]] EventHub& Events(BaseUnknown* object);

    /** @brief Typed event subscription with compile-time receiver and callback-tag policy checks. */
    template<Concept::EventPayload Event, typename CallbackTag = DefaultCallbackTag, Concept::ComClass Receiver,
             typename Callback>
    [[nodiscard]] EventSubscription SubscribeEvent(Receiver& receiver, Callback&& callback,
                                                   EventSubscriptionOptions options = {}) {
        static_assert(Meta::CanAcceptEvent<Receiver, Event>(), "Receiver class does not accept this event payload.");
        static_assert(Meta::CanAttachCallback<Receiver, CallbackTag>(), "Receiver class does not allow this callback.");
        options.callbackId = Traits::EventIdOf<CallbackTag>;
        return Events(receiver).template Subscribe<Event>(std::forward<Callback>(callback), std::move(options));
    }

    /** @brief Typed conditional event subscription with compile-time receiver and callback-tag policy checks. */
    template<Concept::EventPayload Event, typename CallbackTag = DefaultCallbackTag, Concept::ComClass Receiver,
             typename Condition, typename Callback>
    [[nodiscard]] EventSubscription SubscribeEventIf(Receiver& receiver, Condition&& condition, Callback&& callback,
                                                     EventSubscriptionOptions options = {}) {
        static_assert(Meta::CanAcceptEvent<Receiver, Event>(), "Receiver class does not accept this event payload.");
        static_assert(Meta::CanAttachCallback<Receiver, CallbackTag>(), "Receiver class does not allow this callback.");
        options.callbackId = Traits::EventIdOf<CallbackTag>;
        return Events(receiver).template SubscribeIf<Event>(std::forward<Condition>(condition),
                                                            std::forward<Callback>(callback), std::move(options));
    }

    /** @brief Typed event emission with compile-time emitter policy check. */
    template<Concept::EventPayload Event, Concept::ComClass Emitter>
    void EmitEvent(Emitter& emitter, const Event& event) {
        static_assert(Meta::CanEmitEvent<Emitter, Event>(), "Emitter class does not emit this event payload.");
        Events(emitter).Emit(event, std::addressof(emitter));
    }

    /** @brief Typed event emission with a default-constructed payload. */
    template<Concept::EventPayload Event, Concept::ComClass Emitter>
        requires std::default_initializable<Event>
    void EmitEvent(Emitter& emitter) {
        static_assert(Meta::CanEmitEvent<Emitter, Event>(), "Emitter class does not emit this event payload.");
        Events(emitter).template Emit<Event>(std::addressof(emitter));
    }

} // namespace Sora::Kernel

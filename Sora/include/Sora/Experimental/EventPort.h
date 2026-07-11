/**
 * @file EventPort.h
 * @brief Experimental event port decoupled from the BaseUnknown object model.
 * @ingroup Experimental
 *
 * @details This header models event participation as four orthogonal capabilities: an event port, static event schema,
 * object identity, and weak lifetime observation. @ref EventPort itself is a plain final class and never inherits from
 * @c BaseUnknown. COM participation is provided by a separate adaptor header, so the event model can be tested without
 * making COM/data-extension storage part of its core semantics.
 *
 * @verbatim
 *                         +--------------------------+
 *                         |  Event schema of T       |
 *                         |  Emits/Accepts/Callbacks |
 *                         +------------+-------------+
 *                                      |
 *                                      v
 * +-------------+  EventPortOf(T) +-----------+  selects  +--------------------+
 * | user object |  -------------> | EventPort | --------> | SubscriptionRecord |
 * | + liveness  |  EventRefOf(T)  | relations |           | observer+callback  |
 * +------+------+                 +-----+-----+           +---------+----------+
 *        |                              |                           |
 *        | EventLifetimeOf(T)           | Emit / EmitOn             | Acquire()
 *        v                              v                           v
 * +-------------------+        +---------------+          +--------------------+
 * | weak observer     |        | EmissionFrame | -------> | EventDeliveryAct   |
 * +-------------------+        +---------------+          | temporary retain   |
 *                                                        +--------------------+
 *
 * Instrumentation is not stored in EventPort. Delivery sites call the no-op-by-default EventProbe CPO; external code
 * can observe through ADL without adding trace records, trace hooks, or a second callback plane to the port.
 * @endverbatim
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

    /** @brief Direction selector for enumerating connections stored by an event port. */
    enum class EventConnectionDirection : uint8_t {
        Outgoing, /**< Connections emitted by this port. */
        Incoming, /**< Connections received by this port from other ports. */
        All,      /**< Both outgoing and incoming connections, with duplicates collapsed. */
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

    /** @brief Probe phase emitted by delivery code through the no-op-by-default @ref EventProbe CPO. */
    enum class EventProbePhase : uint8_t {
        ReceiveBegin,
        CallbackScheduled,
        CallbackBegin,
        CallbackEnd,
        CallbackCanceled,
        ReceiveEnd,
    };

    /** @brief Type-erased object reference used by event contexts and probes. */
    struct EventObjectRef {
        void* object{};              /**< Borrowed object pointer. */
        EventId type{};              /**< Stable runtime type identifier associated with @p object. */
        std::string_view typeName{}; /**< Stable display name for diagnostics and probe consumers. */

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

    /** @brief Probe payload produced at delivery points and passed to @ref EventProbe. */
    struct EventProbeContext {
        EventProbePhase phase{};   /**< Current delivery phase. */
        EventObjectRef receiver{}; /**< Receiver object, empty when a deferred act observes cancellation. */
        EventObjectRef emitter{};  /**< Emitter object, empty when a deferred act observes cancellation. */
        EventId eventId{};         /**< Reflected event payload identifier. */
        EventId callbackId{};      /**< Reflected callback tag identifier. */
        uint64_t subscription{};   /**< Subscription identifier, or zero for port-wide phases. */
        uint64_t sequence{};       /**< Monotone emission sequence on the emitter port. */
    };

    namespace Detail::EventProbeCPOFn {

        /** @brief Customisation-point object for non-invasive event instrumentation. */
        struct EventProbeCPO {
            /** @brief Invoke an ADL probe hook in @p anchor's namespace when one exists; otherwise compile to no-op. */
            template<typename Context, typename Anchor>
                requires std::same_as<std::remove_cvref_t<Context>, EventProbeContext>
            constexpr void operator()(const Context& context, const Anchor& anchor) const noexcept {
                if constexpr (requires(const Context& c, const Anchor& a) { ProbeEvent(c, a); }) {
                    ProbeEvent(context, anchor);
                }
            }

            /** @brief No-op fallback for instrumentation sites without a semantic anchor. */
            template<typename Context>
                requires std::same_as<std::remove_cvref_t<Context>, EventProbeContext>
            constexpr void operator()(const Context&) const noexcept {}
        };

    } // namespace Detail::EventProbeCPOFn

    inline constexpr Detail::EventProbeCPOFn::EventProbeCPO EventProbe;

    /** @brief Return whether event payload @p Event has an ADL probe hook. */
    template<typename Event>
    inline constexpr bool EventProbeEnabled =
        requires(const EventProbeContext& context, const Event& event) { ProbeEvent(context, event); };

    /** @brief Emit an event probe only when @p Event provides an ADL hook. */
    template<typename Event>
    constexpr void ProbeEventIfEnabled(EventProbePhase phase, const Event& event, EventObjectRef receiver = {},
                                       EventObjectRef emitter = {}, EventId eventId = {}, EventId callbackId = {},
                                       uint64_t subscription = 0, uint64_t sequence = 0) noexcept {
        if constexpr (EventProbeEnabled<Event>) {
            EventProbe(EventProbeContext{.phase = phase,
                                         .receiver = receiver,
                                         .emitter = emitter,
                                         .eventId = eventId,
                                         .callbackId = callbackId,
                                         .subscription = subscription,
                                         .sequence = sequence},
                       event);
        }
    }

    /** @brief Type-erased receive context shared by typed callbacks and any-event callbacks. */
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

    namespace Detail {

        /** @brief Shared liveness bit owned by an EventPort and invalidated by its destructor. */
        struct PortLiveness {
            std::atomic_bool alive{true}; /**< False after the owning port starts destruction. */
        };

    } // namespace Detail

    /** @brief Lifetime strength required by one event delivery path. */
    enum class EventLeaseMode : uint8_t {
        Immediate, /**< Callback runs before the current emit call returns; borrowed storage may be acceptable. */
        Deferred,  /**< Callback may run later on a scheduler; the lease must own enough lifetime to stay valid. */
    };

    /** @brief Object access token acquired for one concrete event delivery. */
    class EventObjectLease {
    public:
        /** @brief Construct an empty lease. */
        EventObjectLease() noexcept = default;

        /** @brief Construct a non-owning lease for immediate delivery under external lifetime control. */
        [[nodiscard]] static EventObjectLease Borrowed(void* object) noexcept {
            return EventObjectLease{object, nullptr};
        }

        /** @brief Construct an owning lease for deferred delivery through an arbitrary holder. */
        [[nodiscard]] static EventObjectLease Owned(void* object, std::shared_ptr<void> holder) noexcept {
            return holder ? EventObjectLease{object, std::move(holder)} : EventObjectLease{};
        }

        /** @brief Return the leased object pointer. */
        [[nodiscard]] void* Get() const noexcept { return object_; }

        /** @brief Return whether this lease carries a non-null object pointer. */
        [[nodiscard]] explicit operator bool() const noexcept { return object_ != nullptr; }

        /** @brief Return whether this lease extends lifetime instead of merely borrowing. */
        [[nodiscard]] bool Owning() const noexcept { return holder_ != nullptr; }

    private:
        EventObjectLease(void* object, std::shared_ptr<void> holder) noexcept
            : object_(object), holder_(std::move(holder)) {}

        void* object_{};
        std::shared_ptr<void> holder_{};
    };

    /** @brief Weak lifetime model stored by event relations without keeping participants alive. */
    class EventObjectLifetime {
    public:
        /** @brief Function used by custom lifetime models to acquire one delivery lease. */
        using AcquireFn = EventObjectLease (*)(const EventObjectLifetime&, EventLeaseMode) noexcept;

        /** @brief Construct an empty lifetime model. */
        EventObjectLifetime() noexcept = default;

        /** @brief Observe an object whose liveness is guarded only by its EventPort. */
        [[nodiscard]] static EventObjectLifetime PortBound(void* object,
                                                           std::weak_ptr<Detail::PortLiveness> port) noexcept {
            return EventObjectLifetime{object, {}, std::move(port), {}, &AcquirePortBound};
        }

        /** @brief Observe an object owned by a weak shared control block. */
        [[nodiscard]] static EventObjectLifetime Shared(void* object, std::weak_ptr<void> holder) noexcept {
            return EventObjectLifetime{object, std::move(holder), {}, {}, &AcquireShared};
        }

        /** @brief Build a custom lifetime model backed by @p state and @p acquire. */
        [[nodiscard]] static EventObjectLifetime Custom(void* object, std::shared_ptr<void> state,
                                                        AcquireFn acquire) noexcept {
            return acquire ? EventObjectLifetime{object, {}, {}, std::move(state), acquire} : EventObjectLifetime{};
        }

        /** @brief Return whether the observed object can be used for immediate synchronous delivery. */
        [[nodiscard]] bool Alive() const noexcept { return static_cast<bool>(Acquire(EventLeaseMode::Immediate)); }

        /** @brief Try to acquire an object lease for @p mode. */
        [[nodiscard]] EventObjectLease Acquire(EventLeaseMode mode) const noexcept {
            return acquire_ ? acquire_(*this, mode) : EventObjectLease{};
        }

        /**
         * @brief Return the originally observed pointer.
         * @details Custom lifetime models may use this pointer as their payload pointer.
         */
        [[nodiscard]] void* Object() const noexcept { return object_; }

        /** @brief Return custom observer state supplied by @ref Custom. */
        [[nodiscard]] const std::shared_ptr<void>& CustomState() const noexcept { return customState_; }

    private:
        EventObjectLifetime(void* object, std::weak_ptr<void> weakHolder,
                            std::weak_ptr<Detail::PortLiveness> portLiveness, std::shared_ptr<void> customState,
                            AcquireFn acquire) noexcept
            : object_(object),
              weakHolder_(std::move(weakHolder)),
              portLiveness_(std::move(portLiveness)),
              customState_(std::move(customState)),
              acquire_(acquire) {}

        [[nodiscard]] static EventObjectLease AcquireShared(const EventObjectLifetime& lifetime,
                                                            EventLeaseMode) noexcept {
            auto holder = lifetime.weakHolder_.lock();
            return holder ? EventObjectLease::Owned(lifetime.object_, std::move(holder)) : EventObjectLease{};
        }

        [[nodiscard]] static EventObjectLease AcquirePortBound(const EventObjectLifetime& lifetime,
                                                               EventLeaseMode mode) noexcept {
            if (mode == EventLeaseMode::Deferred) {
                return {};
            }
            auto port = lifetime.portLiveness_.lock();
            if (!port || !port->alive.load(std::memory_order_acquire)) {
                return {};
            }
            return EventObjectLease::Borrowed(lifetime.object_);
        }

        void* object_{};
        std::weak_ptr<void> weakHolder_{};
        std::weak_ptr<Detail::PortLiveness> portLiveness_{};
        std::shared_ptr<void> customState_{};
        AcquireFn acquire_{};
    };

    namespace Concept {

        template<typename T>
        concept HasEventPort = requires(T& object) {
            { EventPortOf(object) } -> std::same_as<EventPort&>;
        } || requires(T& object) {
            { object.EventPort() } -> std::same_as<EventPort&>;
        } || requires(T& object) {
            { object.Events() } -> std::same_as<EventPort&>;
        } || requires(T& object) { requires std::same_as<std::remove_cvref_t<decltype(object.events)>, EventPort>; };

    } // namespace Concept

    namespace Detail {

        /** @brief CPO functor that implements @c Events(object) -> @ref EventPort. */
        struct EventPortOfFn {
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
                    static_assert(Sora::kDependentFalse<T>, "Type does not expose an EventPort.");
                }
            }
        };

        /** @brief CPO functor that implements @c EventLifetimeOf(object). */
        struct EventLifetimeOfFn {
            template<typename T>
            [[nodiscard]] EventObjectLifetime operator()(T& object) const noexcept(noexcept(Dispatch(object))) {
                return Dispatch(object);
            }

        private:
            template<typename T>
            [[nodiscard]] static EventObjectLifetime Dispatch(T& object) {
                if constexpr (requires { MakeEventLifetime(object); }) {
                    return MakeEventLifetime(object);
                } else if constexpr (requires { object.EventLifetime(); }) {
                    return object.EventLifetime();
                } else if constexpr (requires { object.weak_from_this(); }) {
                    std::weak_ptr<void> holder = object.weak_from_this();
                    return EventObjectLifetime::Shared(std::addressof(object), std::move(holder));
                } else if constexpr (requires { object.shared_from_this(); }) {
                    std::weak_ptr<void> holder = object.shared_from_this();
                    return EventObjectLifetime::Shared(std::addressof(object), std::move(holder));
                } else {
                    return EventObjectLifetime::PortBound(std::addressof(object), EventPortOf(object).LivenessState());
                }
            }
        };

        /** @brief CPO functor that implements @c EventRef(object) -> @ref EventObjectRef. */
        struct EventRefOfFn {
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
    inline constexpr Detail::EventPortOfFn EventPortOf{};

    /** @brief Customisation-point object returning an object's weak event lifetime model. */
    inline constexpr Detail::EventLifetimeOfFn EventLifetimeOf{};

    /** @brief Customisation-point object returning an object's event reference. */
    inline constexpr Detail::EventRefOfFn EventRefOf{};

    namespace Concept {

        /** @brief Object type that exposes an event port through the @ref Events CPO. */
        template<typename T>
        concept EventParticipant =
            std::is_class_v<std::remove_cvref_t<T>> && Concept::HasEventPort<T> && requires(T& object) {
                { Sora::Experimental::EventPortOf(object) } -> std::same_as<EventPort&>;
                { Sora::Experimental::EventLifetimeOf(object) } -> std::same_as<EventObjectLifetime>;
                { Sora::Experimental::EventRefOf(object) } -> std::same_as<EventObjectRef>;
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

    namespace Detail {

        struct SubscriptionRecord;

    } // namespace Detail

    /** @brief Stable handle for an explicit event relation. */
    class EventConnection {
    public:
        /** @brief Construct an empty connection handle. */
        EventConnection() noexcept = default;

        /** @brief Return whether this handle still refers to an active relation. */
        [[nodiscard]] bool IsActive() const noexcept;

        /** @brief Return whether this handle still names a relation record. */
        [[nodiscard]] bool HasRecord() const noexcept { return !record_.expired(); }

        /** @brief Disconnect this relation from future dispatch. */
        void Disconnect() const noexcept;

        /** @brief Pause this relation without consuming its dispatch budget. */
        void Suspend() const noexcept;

        /** @brief Resume this relation after a previous @ref Suspend. */
        void Resume() const noexcept;

        /** @brief Replace this relation's successful-dispatch budget. */
        void SetBudget(WoreBudget budget) const noexcept;

    private:
        explicit EventConnection(std::weak_ptr<Detail::SubscriptionRecord> record) noexcept
            : record_(std::move(record)) {}

        friend class EventPort;
        friend void Disconnect(const EventConnection&) noexcept;
        friend void Suspend(const EventConnection&) noexcept;
        friend void Resume(const EventConnection&) noexcept;
        friend void SetBudget(const EventConnection&, WoreBudget) noexcept;

        std::weak_ptr<Detail::SubscriptionRecord> record_{};
    };

    /** @brief Disconnect @p link from future dispatch without mutating relation storage immediately. */
    void Disconnect(const EventConnection& link) noexcept;

    /** @brief Pause @p link without consuming its dispatch budget. */
    void Suspend(const EventConnection& link) noexcept;

    /** @brief Resume @p link after a previous @ref Suspend. */
    void Resume(const EventConnection& link) noexcept;

    /** @brief Replace @p link's successful-dispatch budget. */
    void SetBudget(const EventConnection& link, WoreBudget budget) noexcept;

    namespace Detail {

        using ErasedCallback = std::move_only_function<EventFlow(EventContextBase&, const void*)>;

        static constexpr uint32_t kCanceledFlag = 0x80000000u;
        static constexpr uint32_t kSuspendedFlag = 0x40000000u;
        static constexpr uint32_t kBudgetMask = 0x3fffffffu;
        static constexpr uint32_t kPersistentState = kBudgetMask;

        struct SubscriptionState {
            std::atomic<uint32_t> word{kPersistentState}; /**< Packed canceled/suspended/budget state. */
        };

        /** @brief Encode @p budget as a packed subscription state word. */
        [[nodiscard]] inline uint32_t PackDispatchState(WoreBudget budget) noexcept {
            if (budget.value == 0) {
                return kCanceledFlag;
            }
            const uint32_t value =
                budget.value == WoreBudget::kPersistent ? kPersistentState : budget.value & kBudgetMask;
            return value == 0 ? kCanceledFlag : value;
        }

        /** @brief Attempt to enter dispatch, consuming one finite budget slot on success. */
        [[nodiscard]] inline bool TryBeginDispatch(SubscriptionState& state) noexcept {
            uint32_t word = state.word.load(std::memory_order_acquire);
            if ((word & (kCanceledFlag | kSuspendedFlag)) != 0) {
                return false;
            }
            uint32_t remaining = word & kBudgetMask;
            if (remaining == kPersistentState) {
                return true;
            }
            while (remaining != 0) {
                const uint32_t next = remaining == 1 ? kCanceledFlag : remaining - 1;
                if (state.word.compare_exchange_weak(word, next, std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
                    return true;
                }
                if ((word & (kCanceledFlag | kSuspendedFlag)) != 0) {
                    return false;
                }
                remaining = word & kBudgetMask;
            }
            return false;
        }

        /** @brief Replace @p state's dispatch budget while preserving suspension state. */
        inline void SetDispatchBudget(SubscriptionState& state, WoreBudget budget) noexcept {
            const uint32_t packed = PackDispatchState(budget);
            uint32_t word = state.word.load(std::memory_order_acquire);
            for (;;) {
                const uint32_t next = (word & kSuspendedFlag) | packed;
                if (state.word.compare_exchange_weak(word, next, std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
                    return;
                }
            }
        }

        template<typename Result>
        [[nodiscard]] constexpr EventFlow FlowOf(Result&& result) noexcept {
            return std::same_as<std::remove_cvref_t<Result>, EventFlow> ? std::forward<Result>(result)
                                                                        : EventFlow::Continue;
        }

        /**
         * @brief Invoke a callable and normalize the result to @ref EventFlow.
         *
         * @protocol
         * - Callables returning `void` map to `EventFlow::Continue`.
         * - Callables returning `EventFlow` propagate that value directly.
         * - Any other return type is treated as `EventFlow::Continue`.
         * - The callable is invoked exactly once.
         *
         * @tparam Callable Callable type.
         * @tparam Args Argument types.
         * @param callable Callable to invoke.
         * @param args Arguments to forward to the callable.
         * @return Normalized event flow result.
         */
        template<typename Callable, typename... Args>
        [[nodiscard]] EventFlow InvokeAndFlow(Callable& callable, Args&&... args) {
            if constexpr (std::same_as<void, std::invoke_result_t<Callable&, Args...>>) {
                std::invoke(callable, std::forward<Args>(args)...);
                return EventFlow::Continue;
            } else {
                return FlowOf(std::invoke(callable, std::forward<Args>(args)...));
            }
        }

        /**
         * @brief Invoke a default receiver handler using the best supported signature.
         *
         * @protocol
         * - The receiver is resolved from @p context using the expected receiver type.
         * - The emitter is resolved from @p context using the expected emitter type.
         * - The first compatible overload in this order is called exactly once:
         *   1. `receiver.On(context.event, emitter)`
         *   2. `receiver.On(context.event)`
         *   3. `receiver.On(context)`
         *   4. `receiver.On()`
         * - `void` results map to `EventFlow::Continue`; `EventFlow` results are propagated.
         * - If no compatible overload exists, compilation fails.
         *
         * @tparam Event Event type.
         * @tparam Receiver Receiver type.
         * @tparam Emitter Emitter type.
         * @param context Event invocation context.
         * @return Normalized event flow result.
         */
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

        /**
         * @brief Invoke a typed handler using the most specific compatible signature.
         *
         * @protocol
         * - The receiver and emitter are extracted from @p context before invocation.
         * - The handler is matched against a fixed priority list, from most specific to least:
         *   1. `(Receiver&, const Event&, Emitter&, EventContext<Event>&)`
         *   2. `(Receiver&, const Event&, Emitter&)
         *   3. `(Receiver&, const Event&)`
         *   4. `(const Event&, Emitter&)`
         *   5. `(const Event&, EventContext<Event>&)`
         *   6. `(const Event&)`
         *   7. `(EventContext<Event>&)`
         *   8. `()`
         * - The first compatible signature is invoked exactly once.
         * - Return normalization follows @ref InvokeAndFlow.
         * - If no compatible signature exists, compilation fails.
         *
         * @tparam Event Event type.
         * @tparam Receiver Receiver type.
         * @tparam Emitter Emitter type.
         * @tparam Handler Handler type.
         * @param handler Handler to invoke.
         * @param context Event invocation context.
         * @return Normalized event flow result.
         */
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

        /**
         * @brief Invoke an any-event handler using the most specific compatible signature.
         *
         * @protocol
         * - The receiver and emitter are extracted from @p context before invocation.
         * - The handler is matched against a fixed priority list, from most specific to least:
         *   1. `(Receiver&, Emitter&, EventContextBase&)`
         *   2. `(Receiver&, Emitter&)`
         *   3. `(Receiver&, EventContextBase&)`
         *   4. `(EventContextBase&)`
         *   5. `()`
         * - The first compatible signature is invoked exactly once.
         * - Return normalization follows @ref InvokeAndFlow.
         * - If no compatible signature exists, compilation fails.
         *
         * @tparam Receiver Receiver type.
         * @tparam Emitter Emitter type.
         * @tparam Handler Handler type.
         * @param handler Handler to invoke.
         * @param context Event invocation context.
         * @return Normalized event flow result.
         */
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
            SubscriptionState state{};
            ErasedCallback callback{};
            EventObjectRef receiver{};
            EventObjectLifetime receiverLifetime{};
            EventPort* owner{};
            EventId eventId{};
            EventId callbackId{};
            uint64_t id{};
            uint64_t order{};
            int32_t priority{};
            bool anyEvent{};

            [[nodiscard]] auto operator==(const SubscriptionRecord& rhs) const noexcept { return this == &rhs; }

            [[nodiscard]] auto operator<=>(const SubscriptionRecord& rhs) const noexcept {
                if (priority != rhs.priority) {
                    return priority > rhs.priority ? std::strong_ordering::less : std::strong_ordering::greater;
                }
                return std::tie(order, id, eventId, callbackId, anyEvent) <=>
                       std::tie(rhs.order, rhs.id, rhs.eventId, rhs.callbackId, rhs.anyEvent);
            }
        };

        /** @brief Apply cancellation flags to a subscription relation. */
        inline void Cancel(SubscriptionRecord& record) noexcept {
            record.state.word.fetch_or(kCanceledFlag, std::memory_order_release);
        }

        struct SubscriptionSnapshot {
            std::vector<std::shared_ptr<SubscriptionRecord>> records{};
        };

    } // namespace Detail

    inline bool EventConnection::IsActive() const noexcept {
        auto record = record_.lock();
        if (!record) {
            return false;
        }
        const uint32_t word = record->state.word.load(std::memory_order_acquire);
        return (word & (Detail::kCanceledFlag | Detail::kSuspendedFlag)) == 0 && (word & Detail::kBudgetMask) != 0;
    }

    inline void EventConnection::Disconnect() const noexcept {
        Sora::Experimental::Disconnect(*this);
    }

    inline void EventConnection::Suspend() const noexcept {
        Sora::Experimental::Suspend(*this);
    }

    inline void EventConnection::Resume() const noexcept {
        Sora::Experimental::Resume(*this);
    }

    inline void EventConnection::SetBudget(WoreBudget budget) const noexcept {
        Sora::Experimental::SetBudget(*this, budget);
    }

    inline void Suspend(const EventConnection& link) noexcept {
        if (auto record = link.record_.lock()) {
            record->state.word.fetch_or(Detail::kSuspendedFlag, std::memory_order_release);
        }
    }

    inline void Resume(const EventConnection& link) noexcept {
        if (auto record = link.record_.lock()) {
            record->state.word.fetch_and(~Detail::kSuspendedFlag, std::memory_order_release);
        }
    }

    inline void SetBudget(const EventConnection& link, WoreBudget budget) noexcept {
        if (auto record = link.record_.lock()) {
            Detail::SetDispatchBudget(record->state, budget);
        }
    }

    /** @brief Immutable data shared by every deferred delivery act in one emission. */
    template<Concept::EventPayload Event>
    struct EmissionFrame {
        Event event;                     /**< Retained event payload. */
        EventObjectRef emitter{};        /**< Emitter reference leased by @ref emitterLease. */
        EventObjectLease emitterLease{}; /**< Emitter lease held by every deferred callback. */
        uint64_t sequence{};             /**< Emission sequence number. */
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

        EventDeliveryAct(std::shared_ptr<const EmissionFrame<Event>> frame, EventObjectLease receiver,
                         std::shared_ptr<Detail::SubscriptionRecord> record) noexcept
            : frame_(std::move(frame)), receiver_(std::move(receiver)), record_(std::move(record)) {}

        void Probe(EventProbePhase phase, EventObjectRef receiver, EventObjectRef emitter) const;

        std::shared_ptr<const EmissionFrame<Event>> frame_{};
        EventObjectLease receiver_{};
        std::shared_ptr<Detail::SubscriptionRecord> record_{};
    };

    /** @brief Move-only batch of delivery acts produced by one asynchronous emission. */
    template<Concept::EventPayload Event>
    class EventDeliveryBatch {
    public:
        /** @brief Construct an empty batch. */
        EventDeliveryBatch() noexcept = default;

        /** @brief Construct a batch from selected delivery acts. */
        explicit EventDeliveryBatch(std::vector<EventDeliveryAct<Event>> acts) noexcept : acts_(std::move(acts)) {}

        EventDeliveryBatch(const EventDeliveryBatch&) = delete;
        EventDeliveryBatch& operator=(const EventDeliveryBatch&) = delete;
        EventDeliveryBatch(EventDeliveryBatch&&) noexcept = default;
        EventDeliveryBatch& operator=(EventDeliveryBatch&&) noexcept = default;

        /** @brief Run every selected delivery act on the current execution agent. */
        void operator()() {
            for (auto& act : acts_) {
                act();
            }
        }

    private:
        std::vector<EventDeliveryAct<Event>> acts_{};
    };

    /** @brief Build a lazy sender that schedules @p act on @p scheduler and then invokes it. */
    template<typename Scheduler, typename Act>
        requires stdexec::scheduler<std::remove_cvref_t<Scheduler>> && std::move_constructible<Act> &&
                 std::invocable<Act&>
    [[nodiscard]] auto EventSender(Scheduler&& scheduler, Act act) {
        return stdexec::schedule(std::forward<Scheduler>(scheduler)) | stdexec::then(std::move(act));
    }

    /** @brief Plain event relation container. */
    class EventPort final {
    public:
        /** @brief Construct an empty event port. */
        EventPort()
            : subscriptions_(std::make_shared<Detail::SubscriptionSnapshot>()),
              liveness_(std::make_shared<Detail::PortLiveness>()) {}

        EventPort(const EventPort&) = delete;
        EventPort& operator=(const EventPort&) = delete;

        /** @brief Cancel outgoing and incoming relations attached to this port. */
        ~EventPort() noexcept {
            liveness_->alive.store(false, std::memory_order_release);
            CancelAll();
        }

        /** @brief Return this port's weak liveness state for ordinary non-owning event participants. */
        [[nodiscard]] std::weak_ptr<Detail::PortLiveness> LivenessState() const noexcept { return liveness_; }

        /** @brief Return stable handles to relations known by this port. */
        [[nodiscard]] std::vector<EventConnection>
        Connections(EventConnectionDirection direction = EventConnectionDirection::Outgoing) const {
            std::vector<EventConnection> result;
            std::scoped_lock lock(mutex_);
            if (direction == EventConnectionDirection::Outgoing || direction == EventConnectionDirection::All) {
                for (const auto& record : subscriptions_->records) {
                    AppendConnection(result, record);
                }
            }
            if (direction == EventConnectionDirection::Incoming || direction == EventConnectionDirection::All) {
                for (const auto& relation : inbound_) {
                    AppendConnection(result, relation.lock());
                }
            }
            return result;
        }

        /** @brief Attach a typed relation owned by this port's emitter. */
        template<Concept::EventPayload Event, typename Receiver, typename Emitter, typename Callback>
        EventConnection ConnectTyped(Receiver& receiver, Emitter&, Callback&& callback, EventOptions options = {}) {
            using R = std::remove_cvref_t<Receiver>;
            using E = std::remove_cvref_t<Emitter>;
            using F = std::remove_cvref_t<Callback>;
            static_assert(std::move_constructible<F>);

            auto typedCallback = F(std::forward<Callback>(callback));
            auto record = std::make_shared<Detail::SubscriptionRecord>();
            record->callback = [callback = std::move(typedCallback)](EventContextBase& base,
                                                                     const void* payload) mutable {
                auto& event = *static_cast<const Event*>(payload);
                EventContext<Event> context{base, event};
                return Detail::InvokeTypedHandler<Event, R, E>(callback, context);
            };
            record->receiver = EventRefOf(receiver);
            record->receiverLifetime = EventLifetimeOf(receiver);
            record->owner = this;
            record->eventId = Traits::EventIdOf<Event>;
            record->callbackId = options.callbackId;
            record->priority = options.priority;
            record->anyEvent = false;
            record->state.word.store(Detail::PackDispatchState(options.budget), std::memory_order_relaxed);

            auto stored = AddSubscription(std::move(record));

            EventPortOf(receiver).AttachInbound(stored);
            return EventConnection{stored};
        }

        /** @brief Attach an event-type-erased relation owned by this port's emitter. */
        template<typename Receiver, typename Emitter, typename Callback>
        EventConnection ConnectAny(Receiver& receiver, Emitter&, Callback&& callback, EventOptions options = {}) {
            using R = std::remove_cvref_t<Receiver>;
            using E = std::remove_cvref_t<Emitter>;
            using F = std::remove_cvref_t<Callback>;
            static_assert(std::move_constructible<F>);

            auto anyCallback = F(std::forward<Callback>(callback));
            auto record = std::make_shared<Detail::SubscriptionRecord>();
            record->callback = [callback = std::move(anyCallback)](EventContextBase& context, const void*) mutable {
                return Detail::InvokeAnyHandler<R, E>(callback, context);
            };
            record->receiver = EventRefOf(receiver);
            record->receiverLifetime = EventLifetimeOf(receiver);
            record->owner = this;
            record->callbackId = options.callbackId;
            record->priority = options.priority;
            record->anyEvent = true;
            record->state.word.store(Detail::PackDispatchState(options.budget), std::memory_order_relaxed);

            auto stored = AddSubscription(std::move(record));

            EventPortOf(receiver).AttachInbound(stored);
            return EventConnection{stored};
        }

        /** @brief Deliver @p event synchronously from @p emitter. */
        template<Concept::EventPayload Event, typename Emitter>
        void Emit(Emitter& emitter, const Event& event) {
            const uint64_t sequence = nextSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
            auto emitterRef = EventRefOf(emitter);
            auto subscriptions = Subscriptions();
            ProbeEventIfEnabled(EventProbePhase::ReceiveBegin, event, {}, emitterRef, Traits::EventIdOf<Event>, {}, 0,
                                sequence);

            auto eventnessFilter = std::views::filter([](const auto& record) {
                return record && (record->anyEvent || record->eventId == Traits::EventIdOf<Event>);
            });
            for (const auto& record : subscriptions->records | eventnessFilter) {
                EventObjectLease receiverLease = record->receiverLifetime.Acquire(EventLeaseMode::Immediate);
                EventObjectRef receiverRef = receiverLease ? record->receiver : EventObjectRef{};
                if (receiverRef) {
                    receiverRef.object = receiverLease.Get();
                }
                if (!receiverRef) {
                    Detail::Cancel(*record);
                    continue;
                }

                EventContext<Event> context{{.receiver = receiverRef,
                                             .emitter = emitterRef,
                                             .eventId = Traits::EventIdOf<Event>,
                                             .callbackId = record->callbackId,
                                             .subscription = record->id,
                                             .sequence = sequence},
                                            event};
                if (DispatchRecord(record, context, event) == EventFlow::Stop) {
                    break;
                }
            }

            ProbeEventIfEnabled(EventProbePhase::ReceiveEnd, event, {}, emitterRef, Traits::EventIdOf<Event>, {}, 0,
                                sequence);
        }

        /** @brief Build a lazy sender that delivers every callback selected by @p event on @p scheduler. */
        template<stdexec::scheduler Scheduler, Concept::EventPayload Event, typename Emitter>
        [[nodiscard]] auto EmitOn(Emitter& emitter, Scheduler scheduler, const Event& event) {
            const uint64_t sequence = nextSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
            const EventId eventId = Traits::EventIdOf<Event>;
            auto emitterRef = EventRefOf(emitter);
            auto emitterLease = EventLifetimeOf(emitter).Acquire(EventLeaseMode::Deferred);
            auto subscriptions = Subscriptions();
            ProbeEventIfEnabled(EventProbePhase::ReceiveBegin, event, {}, emitterRef, eventId, {}, 0, sequence);

            std::vector<EventDeliveryAct<Event>> acts;
            std::shared_ptr<const EmissionFrame<Event>> frame;
            if (emitterLease) {
                emitterRef.object = emitterLease.Get();

                auto matchingRecords = subscriptions->records | std::views::filter([eventId](const auto& record) {
                                           return record && (record->anyEvent || record->eventId == eventId);
                                       });
                for (const auto& record : matchingRecords) {
                    const uint32_t word = record->state.word.load(std::memory_order_acquire);
                    if ((word & (Detail::kCanceledFlag | Detail::kSuspendedFlag)) != 0 ||
                        (word & Detail::kBudgetMask) == 0) {
                        continue;
                    }
                    EventObjectLease receiverLease = record->receiverLifetime.Acquire(EventLeaseMode::Deferred);
                    EventObjectRef receiverRef = receiverLease ? record->receiver : EventObjectRef{};
                    if (receiverRef) {
                        receiverRef.object = receiverLease.Get();
                    }
                    if (!receiverRef) {
                        Detail::Cancel(*record);
                        continue;
                    }
                    if (!frame) {
                        frame = std::make_shared<const EmissionFrame<Event>>(
                            EmissionFrame<Event>{.event = event,
                                                 .emitter = emitterRef,
                                                 .emitterLease = std::move(emitterLease),
                                                 .sequence = sequence});
                    }
                    ProbeEventIfEnabled(EventProbePhase::CallbackScheduled, event, receiverRef, emitterRef, eventId,
                                        record->callbackId, record->id, sequence);
                    auto act = EventDeliveryAct<Event>{frame, std::move(receiverLease), record};
                    if (act) {
                        acts.push_back(std::move(act));
                    }
                }
            } else {
                emitterRef = {};
            }

            ProbeEventIfEnabled(EventProbePhase::ReceiveEnd, event, {}, emitterRef, eventId, {}, 0, sequence);
            return EventSender(std::move(scheduler), EventDeliveryBatch<Event>{std::move(acts)});
        }

        /** @brief Cancel every relation owned by this port and every inbound relation known by this port. */
        void CancelAll() noexcept {
            std::scoped_lock lock(mutex_);
            for (const auto& subscription : subscriptions_->records) {
                if (subscription) {
                    Detail::Cancel(*subscription);
                }
            }
            std::erase_if(inbound_, [](const std::weak_ptr<Detail::SubscriptionRecord>& relation) {
                auto record = relation.lock();
                if (!record) {
                    return true;
                }
                Detail::Cancel(*record);
                return false;
            });
        }

    private:
        friend void Disconnect(const EventConnection&) noexcept;

        template<Concept::EventPayload>
        friend class EventDeliveryAct;

        static void AppendConnection(std::vector<EventConnection>& result,
                                     const std::shared_ptr<Detail::SubscriptionRecord>& record) {
            if (!record) {
                return;
            }
            const bool alreadyAdded = std::ranges::any_of(result, [&](const EventConnection& connection) {
                return connection.record_.lock().get() == record.get();
            });
            if (!alreadyAdded) {
                result.push_back(EventConnection{record});
            }
        }

        [[nodiscard]] std::shared_ptr<Detail::SubscriptionRecord>
        AddSubscription(std::shared_ptr<Detail::SubscriptionRecord> stored) {
            stored->id = nextSubscription_.fetch_add(1, std::memory_order_relaxed) + 1;
            stored->order = stored->id;
            {
                std::scoped_lock lock(mutex_);
                auto next = std::make_shared<Detail::SubscriptionSnapshot>(*subscriptions_);
                next->records.push_back(stored);
                std::ranges::stable_sort(next->records, [](const auto& lhs, const auto& rhs) noexcept {
                    if (lhs->priority != rhs->priority) {
                        return lhs->priority > rhs->priority;
                    }
                    return lhs->order < rhs->order;
                });
                subscriptions_ = std::move(next);
            }
            return stored;
        }

        void RemoveSubscription(const std::shared_ptr<Detail::SubscriptionRecord>& target) noexcept {
            if (!target) {
                return;
            }
            Detail::Cancel(*target);
            std::scoped_lock lock(mutex_);
            auto next = std::make_shared<Detail::SubscriptionSnapshot>(*subscriptions_);
            std::erase_if(next->records, [&](const auto& record) { return !record || record.get() == target.get(); });
            subscriptions_ = std::move(next);
            std::erase_if(inbound_, [](const auto& relation) { return relation.expired(); });
        }

        void AttachInbound(std::weak_ptr<Detail::SubscriptionRecord> record) {
            std::scoped_lock lock(mutex_);
            std::erase_if(inbound_, [](const auto& relation) { return relation.expired(); });
            inbound_.push_back(std::move(record));
        }

        [[nodiscard]] std::shared_ptr<const Detail::SubscriptionSnapshot> Subscriptions() const {
            std::scoped_lock lock(mutex_);
            return subscriptions_;
        }

        template<Concept::EventPayload Event>
        EventFlow DispatchRecord(const std::shared_ptr<Detail::SubscriptionRecord>& record,
                                 EventContext<Event>& context, const Event& event) {
            if (!record || !Detail::TryBeginDispatch(record->state)) {
                return EventFlow::Continue;
            }
            ProbeEventIfEnabled(EventProbePhase::CallbackBegin, event, context.receiver, context.emitter,
                                context.eventId, context.callbackId, context.subscription, context.sequence);
            EventFlow flow = record->callback(context, std::addressof(event));
            ProbeEventIfEnabled(EventProbePhase::CallbackEnd, event, context.receiver, context.emitter, context.eventId,
                                context.callbackId, context.subscription, context.sequence);
            return flow;
        }

        mutable std::mutex mutex_{};
        std::shared_ptr<Detail::SubscriptionSnapshot> subscriptions_{};
        std::vector<std::weak_ptr<Detail::SubscriptionRecord>> inbound_{};
        std::shared_ptr<Detail::PortLiveness> liveness_{};
        std::atomic<uint64_t> nextSubscription_{};
        std::atomic<uint64_t> nextSequence_{};
    };

    inline void Disconnect(const EventConnection& link) noexcept {
        if (auto record = link.record_.lock()) {
            if (record->owner) {
                record->owner->RemoveSubscription(record);
            } else {
                Detail::Cancel(*record);
            }
        }
    }

    template<Concept::EventPayload Event>
    void EventDeliveryAct<Event>::Probe(EventProbePhase phase, EventObjectRef receiver, EventObjectRef emitter) const {
        ProbeEventIfEnabled(phase, frame_->event, receiver, emitter, Traits::EventIdOf<Event>,
                            record_ ? record_->callbackId : EventId{}, record_ ? record_->id : 0,
                            frame_ ? frame_->sequence : 0);
    }

    template<Concept::EventPayload Event>
    void EventDeliveryAct<Event>::operator()() {
        if (!frame_ || !record_) {
            return;
        }
        EventObjectRef receiverRef = receiver_ ? record_->receiver : EventObjectRef{};
        EventObjectRef emitterRef = frame_->emitterLease ? frame_->emitter : EventObjectRef{};
        if (receiverRef) {
            receiverRef.object = receiver_.Get();
        }
        if (emitterRef) {
            emitterRef.object = frame_->emitterLease.Get();
        }
        if (!receiverRef || !emitterRef) {
            Detail::Cancel(*record_);
            Probe(EventProbePhase::CallbackCanceled, receiverRef, emitterRef);
            return;
        }
        if (!Detail::TryBeginDispatch(record_->state)) {
            return;
        }
        Probe(EventProbePhase::CallbackBegin, receiverRef, emitterRef);
        EventContext<Event> context{{.receiver = receiverRef,
                                     .emitter = emitterRef,
                                     .eventId = Traits::EventIdOf<Event>,
                                     .callbackId = record_->callbackId,
                                     .subscription = record_->id,
                                     .sequence = frame_->sequence},
                                    frame_->event};
        std::ignore = record_->callback(context, std::addressof(frame_->event));
        Probe(EventProbePhase::CallbackEnd, receiverRef, emitterRef);
    }

    /** @brief Subscribe @p receiver to @p emitter's @p Event with the receiver's default On handler. */
    template<typename Receiver, typename Emitter, Concept::EventPayload Event>
        requires Concept::EventReceiver<Receiver, Event> && Concept::EventEmitter<Emitter, Event>
    EventConnection Connect(Receiver& receiver, Emitter& emitter, const Event&, EventOptions options = {}) {
        auto defaultHandler = [](Receiver& r, const Event& event, Emitter& e, EventContext<Event>& context) {
            std::ignore = r;
            std::ignore = event;
            std::ignore = e;
            return Detail::InvokeDefaultReceiver<Event, Receiver, Emitter>(context);
        };
        return EventPortOf(emitter).template ConnectTyped<Event>(receiver, emitter, std::move(defaultHandler), options);
    }

    /** @brief Subscribe @p receiver to @p emitter's @p Event with an explicit handler. */
    template<typename Receiver, typename Emitter, Concept::EventPayload Event, typename Handler>
        requires Concept::EventReceiver<Receiver, Event> && Concept::EventEmitter<Emitter, Event>
    EventConnection Connect(Receiver& receiver, Emitter& emitter, const Event&, EventOptions options,
                            Handler&& handler) {
        return EventPortOf(emitter).template ConnectTyped<Event>(receiver, emitter, std::forward<Handler>(handler),
                                                                 options);
    }

    /** @brief Subscribe @p receiver to @p emitter's @p Event with an explicit handler and budget shorthand. */
    template<typename Receiver, typename Emitter, Concept::EventPayload Event, typename Handler>
        requires Concept::EventReceiver<Receiver, Event> && Concept::EventEmitter<Emitter, Event>
    EventConnection Connect(Receiver& receiver, Emitter& emitter, const Event& event, WoreBudget budget,
                            Handler&& handler) {
        return Connect(receiver, emitter, event, EventOptions{.budget = budget}, std::forward<Handler>(handler));
    }

    /** @brief Subscribe @p receiver to every event emitted by @p emitter. */
    template<Concept::EventParticipant Receiver, Concept::EventParticipant Emitter, typename Handler>
        requires Traits::CanAcceptEvent<Receiver> && Traits::CanEmitEvent<Emitter>
    EventConnection Connect(Receiver& receiver, Emitter& emitter, AnyEventTag, EventOptions options,
                            Handler&& handler) {
        return EventPortOf(emitter).ConnectAny(receiver, emitter, std::forward<Handler>(handler), options);
    }

    /** @brief Emit @p event from @p emitter synchronously. */
    template<typename Emitter, Concept::EventPayload Event>
        requires Concept::EventEmitter<Emitter, Event>
    void Emit(Emitter& emitter, const Event& event) {
        EventPortOf(emitter).Emit(emitter, event);
    }

    /** @brief Emit a default-constructed @p Event from @p emitter synchronously. */
    template<Concept::EventPayload Event, typename Emitter>
        requires Concept::EventEmitter<Emitter, Event>
    void Emit(Emitter& emitter) {
        EventPortOf(emitter).template Emit<Event>(emitter, Event{});
    }

    /** @brief Build a lazy sender that emits @p event from @p emitter on @p scheduler. */
    template<typename Emitter, stdexec::scheduler Scheduler, Concept::EventPayload Event>
        requires Concept::EventEmitter<Emitter, Event>
    [[nodiscard]] auto EmitOn(Emitter& emitter, Scheduler scheduler, const Event& event) {
        return EventPortOf(emitter).EmitOn(emitter, std::move(scheduler), event);
    }

} // namespace Sora::Experimental

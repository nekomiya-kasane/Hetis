/**
 * @file EventPump.h
 * @brief Platform-thread main-loop dispatch core.
 *
 * EventPump is the rendezvous of six generic async primitives at one physical
 * site (see @c docs/cpp/general-async-primitives.md §15.4):
 *
 * | edge in EventPump                          | primitive                       |
 * |--------------------------------------------|---------------------------------|
 * | Dedicated Manager → Pump (inbox)           | @b Channel<SystemEvent> (MPSC)  |
 * | Bookkeep stage → Broadcast stage           | @b Pipe<Bookkeep, Broadcast>    |
 * | Pump → Subscribers (1→N fan-out)           | @b Topic<SystemEvent>           |
 * | Worker thread → Pump (coroutine resume)    | @b Tether (OwnerExecutor)       |
 * | Manager classification (Plat/Ded/Free)     | @b Apartment via @c ScheduleMode |
 * | (Manager, Payload) bookkeep participation  | @b Edge via @c Traits::HandlesBookkeep |
 *
 * EventPump owns no algorithm of its own — it stitches the six primitives.
 *
 * @par Compile-time guarantees (P2996 reflection / P3385 annotations / P1306-style folds)
 *   - @c Apartment partition of the Manager pack runs in @c consteval (annotation
 *     read via @c std::meta::annotations_of). The three apartment tuples are
 *     materialised at instantiation; nothing is decided at runtime.
 *   - The bookkeep table is *not* a runtime data structure: per-payload-per-manager
 *     dispatch is an @c if @c constexpr branch inside a fold-expanded lambda over
 *     the platform-apartment tuple, so each (M, P) pair compiles to either a
 *     direct call or to nothing.
 *   - No virtual functions, no @c std::function, no runtime registration.
 *
 * @par Two-phase bind (semantically distinct, kept separate)
 *   - @ref AttachContext sets the structured-concurrency context (scope and stop
 *     token). No I/O, no thread spawn, cannot fail.
 *   - @ref AttachManagers takes the Manager pack and starts dedicated threads.
 *     This is the only step that may fail (a dedicated Manager refuses to
 *     start), so it returns @c Result<void> and propagates errors verbatim.
 *
 * @ingroup Platform
 */
#pragma once

#include "Mashiro/Core/MpscQueue.h"
#include "Mashiro/Core/Result.h"
#include "Mashiro/Platform/EventChannel.h"
#include "Mashiro/Platform/PlatformBackend.h"
#include "Mashiro/Platform/SystemEvent.h"
#include "Mashiro/Platform/ThreadContract.h"

#include <stdexec/execution.hpp>
#include <exec/repeat_until.hpp>

#include <array>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <meta>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace Mashiro::Platform {

    /*
     * Edge: (Manager, Payload) participation in bookkeeping.
     *
     * The convention is canonicalised once in @c SystemEvent.h:
     *
     *     concept Mashiro::Traits::Event::HandlesBookkeep<M, P>
     *         := SystemEventPayload<P> && requires(M& m, const P& p) {
     *             { m.On(p) } noexcept -> std::same_as<void>;
     *         };
     *
     * EventPump consumes that concept verbatim. There is no parallel @c Platform-scoped concept and no second method
     * name. The dispatch site in @ref EventPump::DispatchEvent is the same fold-expanded @c if @c constexpr that
     * @c Mashiro::DispatchBookkeep uses, lifted across the Manager pack so the (Manager, Payload) table is
     * materialised at compile time.
     */

    /* Apartment partition: split a Manager pack by ScheduleDomain. */
    /** @cond INTERNAL */
    namespace Detail {

        /**
         * @brief Compile-time indices in @c <Ms...> whose ScheduleMode is @p D.
         *
         * Returns a fixed-size array plus the count of valid leading entries. Keeping the array as a local
         * @c constexpr avoids exposing @c std::array / @c std::pair as structural NTTPs under libc++.
         */
        template<ScheduleDomain D, class... Ms>
        consteval auto IndicesByDomain() {
            constexpr size_t N = sizeof...(Ms);
            std::array<size_t, N> picks{};
            size_t k = 0;
            [&]<size_t... I>(std::index_sequence<I...>) {
                ((Traits::GetScheduleMode<Ms>() == D ? (picks[k++] = I, void()) : void()), ...);
            }(std::make_index_sequence<N>{});
            return std::pair{picks, k};
        }

        /**
         * @brief @c tuple<Mgr*...> for the apartment selected by @p D.
         *
         * The tuple stores pointers into caller-owned Manager objects. Slicing stays inside this consteval function so
         * the index array remains a local @c constexpr instead of crossing a template-parameter boundary as an NTTP.
         */
        template<ScheduleDomain D, class... Ms>
        consteval auto MakeApartmentPtrTuple() {
            constexpr auto p = IndicesByDomain<D, Ms...>();
            return [&]<size_t... I>(std::index_sequence<I...>) {
                return std::tuple<std::tuple_element_t<p.first[I], std::tuple<Ms...>>*...>{};
            }(std::make_index_sequence<p.second>{});
        }

        template<ScheduleDomain D, class... Ms>
        using ApartmentPtrTuple = decltype(MakeApartmentPtrTuple<D, Ms...>());

    } // namespace Detail
    /** @endcond */

    /**
     * @brief Platform-thread dispatch core, parametrised by the full Manager set.
     *
     * @tparam Managers Every Manager class that participates in platform input.
     *                  The set is declared exactly once in @c ManagerSet.h and
     *                  fed into @c EventPump<...> from there.
     *
     * @par Lifecycle
     *   1. @ref AttachContext is called with the scope and stop-token.
     *   2. @ref AttachManagers is called with the live Manager objects.
     *   3. @ref AddChannel may be called any number of times *before* @ref Run
     *      (or from inside the Platform thread once it is running).
     *   4. @ref Run is started. It returns a sender; the caller drives it via
     *      @c stdexec::sync_wait or composes it into a larger graph.
     *
     * @par Thread safety
     *   - @ref TryPostExternal and @ref SubmitResume: any thread.
     *   - @ref AddChannel: Platform thread, or any thread before @ref Run starts.
     *   - All other members: Platform thread only.
     */
    template<class... Managers>
    class EventPump final {
    public:
        using PlatformManagers = Detail::ApartmentPtrTuple<ScheduleDomain::PlatformThread, Managers...>;

        EventPump();
        ~EventPump();

        EventPump(const EventPump&) = delete;
        EventPump& operator=(const EventPump&) = delete;
        EventPump(EventPump&&) = delete;
        EventPump& operator=(EventPump&&) = delete;

        /**
         * @brief Phase 1 of bind: structured-concurrency context only. Cannot fail.
         */
        void AttachContext(stdexec::counting_scope& scope, stdexec::inplace_stop_token stop) noexcept;

        /**
         * @brief Phase 2 of bind: take the Manager pack and start dedicated threads.
         *
         * Stores Platform-apartment Managers in @ref PlatformManagers, starts each Dedicated Manager with the external
         * inbox post callback, and leaves Free-threaded Managers untouched. Returns the first failure verbatim.
         */
        Result<void> AttachManagers(Managers&... managers);

        /**
         * @brief Allocate a new position-stable subscriber endpoint.
         */
        [[nodiscard]] EventChannel& AddChannel();

        /**
         * @brief Submit a SystemEvent from any thread and wake the Platform thread.
         */
        [[nodiscard]] bool TryPostExternal(SystemEvent ev) noexcept;

        /**
         * @brief Submit a coroutine handle for resumption on the Platform thread.
         *
         * A full mailbox is a design-invariant violation: dropping the handle would leak the coroutine, so the pump
         * terminates instead. Awaiters must check stop before submission; after stop, delivery is best-effort.
         */
        void SubmitResume(std::coroutine_handle<> h) noexcept;

        /**
         * @brief The main-loop sender. Caller drives it via @c stdexec.
         *
         * Each iteration blocks in @ref PlatformBackend::WaitForAnySource, drains native events, drains the external
         * inbox, drains the owner mailbox, then returns @c stop_.stop_requested() to @c exec::repeat_until. The drains
         * still run on the stop-observing iteration so already-enqueued events and coroutine handles are not stranded.
         */
        [[nodiscard]] auto Run() -> stdexec::sender auto;

    private:
        void DispatchEvent(const SystemEvent& ev);

        void DrainNativeOnce();
        void DrainExternalOnce();
        void DrainOwnerMailboxOnce();

        /* Platform-apartment Managers: pointers into caller-owned objects. */
        PlatformManagers platformManagers_{};

        /* Inbox: Dedicated Managers and other threads post here. */
        MpscQueue<SystemEvent> inbox_{};

        /* OwnerExecutor mailbox: cross-thread coroutine resumption. */
        MpscQueue<std::coroutine_handle<>> ownerMailbox_{};

        /* Topic fan-out: subscribers, written by Platform thread only. */
        std::vector<std::unique_ptr<EventChannel>> channels_{};

        /* Native bridge: blocking wait, wake, native-queue drain. */
        PlatformBackend backend_{};

        /* Context wired by AttachContext. */
        stdexec::counting_scope* scope_{nullptr};
        stdexec::inplace_stop_token stop_{};
    };

    template<class... Ms>
    EventPump<Ms...>::EventPump() = default;

    template<class... Ms>
    EventPump<Ms...>::~EventPump() = default;

    template<class... Ms>
    void EventPump<Ms...>::AttachContext(stdexec::counting_scope& scope, stdexec::inplace_stop_token stop) noexcept {
        scope_ = &scope;
        stop_ = stop;
    }

    template<class... Ms>
    Result<void> EventPump<Ms...>::AttachManagers(Ms&... managers) {
        if (scope_ == nullptr) {
            std::terminate();
        }

        /* Stash Platform-apartment manager pointers in declared order. */
        constexpr auto plat = Detail::IndicesByDomain<ScheduleDomain::PlatformThread, Ms...>();
        auto bag = std::tuple<Ms*...>{&managers...};

        [&]<size_t... I>(std::index_sequence<I...>) {
            ((std::get<I>(platformManagers_) = std::get<plat.first[I]>(bag)), ...);
        }(std::make_index_sequence<plat.second>{});

        auto bindWindowRegistry = [this](auto& manager) noexcept {
            using M = std::remove_cvref_t<decltype(manager)>;
            if constexpr (std::same_as<M, WindowManager>) {
                backend_.AttachWindowRegistry(manager);
            }
        };
        (bindWindowRegistry(managers), ...);

        /* Start each Dedicated-apartment manager; the first failure aborts bring-up verbatim. */
        constexpr auto ded = Detail::IndicesByDomain<ScheduleDomain::DedicatedThread, Ms...>();
        auto post = [this](SystemEvent ev) noexcept { return TryPostExternal(std::move(ev)); };

        Result<void> startResult{};
        auto startOne = [&](auto* manager) {
            startResult = manager->Start(stop_, post, *scope_);
            return startResult.has_value();
        };

        [&]<size_t... I>(std::index_sequence<I...>) {
            (void)(startOne(std::get<ded.first[I]>(bag)) && ...);
        }(std::make_index_sequence<ded.second>{});

        if (!startResult) {
            return startResult;
        }
        /* FreeThreaded managers have nothing to wire: they are stateless or atomically protected. */
        return {};
    }

    template<class... Ms>
    EventChannel& EventPump<Ms...>::AddChannel() {
        channels_.emplace_back(std::make_unique<EventChannel>());
        return *channels_.back();
    }

    template<class... Ms>
    bool EventPump<Ms...>::TryPostExternal(SystemEvent ev) noexcept {
        const bool ok = inbox_.TryPush(std::move(ev));
        backend_.Wake();
        return ok;
    }

    template<class... Ms>
    void EventPump<Ms...>::SubmitResume(std::coroutine_handle<> h) noexcept {
        if (!ownerMailbox_.TryPush(h)) {
            std::terminate();
        }
        backend_.Wake();
    }

    template<class... Ms>
    void EventPump<Ms...>::DispatchEvent(const SystemEvent& ev) {
        std::visit(
            [this]<class P>(const P& payload) {
                /*
                 * Bookkeeper stage: compile-time-expanded over PlatformManagers. A Manager opts in with
                 * void On(const P&) noexcept; the fold-expanded if constexpr materialises the
                 * (Manager, Payload) table at compile time.
                 */
                constexpr size_t N = std::tuple_size_v<PlatformManagers>;
                [&]<size_t... I>(std::index_sequence<I...>) {
                    (([&] {
                         using M = std::remove_pointer_t<std::tuple_element_t<I, PlatformManagers>>;
                         if constexpr (Traits::Event::HandlesBookkeep<M, P>) {
                             std::get<I>(platformManagers_)->On(payload);
                         }
                     }()),
                     ...);
                }(std::make_index_sequence<N>{});

                /* Broadcast stage: 1→N fan-out (Topic). */
                for (auto& ch : channels_) {
                    (void)ch->TryPush(SystemEvent{payload});
                }
            },
            ev);
    }

    template<class... Ms>
    void EventPump<Ms...>::DrainNativeOnce() {
        auto consume = [this](SystemEvent&& event) noexcept { DispatchEvent(event); };
        backend_.DrainNative(consume);
    }

    template<class... Ms>
    void EventPump<Ms...>::DrainExternalOnce() {
        inbox_.Drain([this](SystemEvent ev) { DispatchEvent(ev); });
    }

    template<class... Ms>
    void EventPump<Ms...>::DrainOwnerMailboxOnce() {
        ownerMailbox_.Drain([](std::coroutine_handle<> h) { h.resume(); });
    }

    template<class... Ms>
    auto EventPump<Ms...>::Run() -> stdexec::sender auto {
        return stdexec::just() | stdexec::then([this] {
                   backend_.WaitForAnySource(stop_);
                   DrainNativeOnce();
                   DrainExternalOnce();
                   DrainOwnerMailboxOnce();
                   return stop_.stop_requested();
               }) |
               exec::repeat_until();
    }

} // namespace Mashiro::Platform

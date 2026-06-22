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
 * | Manager classification (Plat/Ded/Free)     | @b Apartment via @c ScheduleMode|
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

#include <array>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <meta>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace Mashiro::Platform {

    // -----------------------------------------------------------------------
    // Edge — (Manager, Payload) participation in bookkeeping.
    // -----------------------------------------------------------------------
    //
    // The convention is canonicalised once in @c SystemEvent.h:
    //
    //     concept Mashiro::Traits::Event::HandlesBookkeep<M, P>
    //         := SystemEventPayload<P> && requires(M& m, const P& p) { m.On(p); };
    //
    // EventPump consumes that concept verbatim — there is no parallel
    // @c Platform-scoped concept and no second method name. The dispatch site
    // in @ref EventPump::DispatchEvent is the same fold-expanded @c if
    // @c constexpr that @c Mashiro::DispatchBookkeep uses, lifted across the
    // Manager pack so the (Manager, Payload) table is materialised at compile
    // time. Adding a Manager or a payload changes only the type list; the
    // table re-derives without touching this header.

    // -----------------------------------------------------------------------
    // Apartment partition — split a Manager pack by ScheduleDomain.
    // -----------------------------------------------------------------------
    /** @cond INTERNAL */
    namespace Detail {

        /// @brief Compile-time list of indices in @c <Ms...> whose ScheduleMode is @p D.
        ///
        /// Returns a fixed-size @c std::array sized to the full pack (so the type is
        /// stable across domains) and the count of valid leading entries. The result
        /// is consumed inside @ref ApartmentPtrTuple as a single @c constexpr local —
        /// it is not exposed as a non-type template argument, sidestepping the
        /// structural-type requirement on @c std::array / @c std::pair under libc++.
        template<ScheduleDomain D, class... Ms>
        consteval auto IndicesByDomain() {
            constexpr std::size_t N = sizeof...(Ms);
            std::array<std::size_t, N> picks{};
            std::size_t k = 0;
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                ((Traits::GetScheduleMode<Ms>() == D ? (picks[k++] = I, void()) : void()), ...);
            }(std::make_index_sequence<N>{});
            return std::pair{picks, k};
        }

        /// @brief @c tuple<Mgr*...> for the apartment selected by @p D.
        ///
        /// Pointers, not values: AttachManagers receives the Manager pack by
        /// reference and stores their addresses; the Manager objects themselves
        /// are owned by the caller (typically a stack frame in PlatformThread::Run).
        ///
        /// The slicing is done inside a single consteval function so the index array
        /// stays a local @c constexpr value rather than crossing a template-parameter
        /// boundary as an NTTP. This works on any C++20 implementation regardless of
        /// whether @c std::array satisfies the structural-type requirement of P1907.
        template<ScheduleDomain D, class... Ms>
        consteval auto MakeApartmentPtrTuple() {
            constexpr auto p = IndicesByDomain<D, Ms...>();
            return [&]<std::size_t... I>(std::index_sequence<I...>) {
                return std::tuple<std::tuple_element_t<p.first[I], std::tuple<Ms...>>*...>{};
            }(std::make_index_sequence<p.second>{});
        }

        template<ScheduleDomain D, class... Ms>
        using ApartmentPtrTuple = decltype(MakeApartmentPtrTuple<D, Ms...>());

    } // namespace Detail
    /** @endcond */

    // -----------------------------------------------------------------------
    // EventPump<Managers...>
    // -----------------------------------------------------------------------

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
        using PlatformMgrs = Detail::ApartmentPtrTuple<ScheduleDomain::PlatformThread, Managers...>;
        using DedicatedMgrs = Detail::ApartmentPtrTuple<ScheduleDomain::DedicatedThread, Managers...>;
        using FreeMgrs = Detail::ApartmentPtrTuple<ScheduleDomain::FreeThreaded, Managers...>;

        EventPump();
        ~EventPump();

        EventPump(const EventPump&) = delete;
        EventPump& operator=(const EventPump&) = delete;
        EventPump(EventPump&&) = delete;
        EventPump& operator=(EventPump&&) = delete;

        /// @brief Phase 1 of bind: structured-concurrency context only. Cannot fail.
        void AttachContext(stdexec::counting_scope& scope, stdexec::inplace_stop_token stop) noexcept;

        /// @brief Phase 2 of bind: take the Manager pack and start dedicated threads.
        ///
        /// Stores Platform-apartment Managers as pointers in @ref PlatformMgrs,
        /// asks each Dedicated Manager to start its thread (handing it the post
        /// callback into the inbox), and leaves Free-threaded Managers untouched.
        /// Returns the first failure verbatim; on success the pump is fully
        /// armed and ready for @ref Run.
        Result<void> AttachManagers(Managers&... mgrs);

        /// @brief Allocate a new subscriber endpoint. The returned reference is
        ///        position-stable — @ref EventChannel must not move.
        EventChannel& AddChannel();

        /// @brief Submit a SystemEvent from any thread. Wakes the Platform thread.
        ///        Returns false on inbox-full back-pressure.
        [[nodiscard]] bool TryPostExternal(SystemEvent ev) noexcept;

        /// @brief Submit a coroutine handle for resumption on the Platform thread.
        ///
        /// The caller has a hard contract: the mailbox is sized for the project's
        /// maximum in-flight cross-thread coroutines, so a full mailbox is a
        /// design-invariant violation, not a runtime condition. We terminate
        /// rather than silently drop a handle (which would leak the coroutine).
        ///
        /// @par Stop interaction (caller contract)
        /// Cross-thread awaiters must check @c stop.stop_requested() in their
        /// @c await_suspend *before* calling SubmitResume; calling after stop
        /// is best-effort — the handle may be drained on the loop iteration
        /// that observes stop (see @ref Run drain-after-stop guarantee), or it
        /// may not, depending on race timing. Only the awaiter itself can
        /// guarantee its coroutine progresses correctly under stop, by reading
        /// the stop token on its next suspension point.
        void SubmitResume(std::coroutine_handle<> h) noexcept;

        /// @brief The main-loop sender. Caller drives it via @c stdexec.
        ///
        /// @par Loop body (one iteration)
        ///   1. @c backend_.WaitForAnySource(stop_) — block until at least one
        ///      source is ready, @ref Wake has been called, or stop fires.
        ///   2. Drain native messages, then the external inbox, then the owner
        ///      mailbox. Order matters: native first so platform-originated
        ///      lifecycle events are seen before the dedicated managers'
        ///      derived signals; owner mailbox last so any coroutines resumed
        ///      this iteration observe the latest state.
        ///   3. Return @c stop_.stop_requested(); the surrounding
        ///      @c exec::repeat_until adapter re-runs the body until @c true.
        ///
        /// @par Drain-after-stop guarantee
        /// The three drain calls run *unconditionally* every iteration — even
        /// the iteration that observes stop. This is intentional: events
        /// already enqueued before @ref RequestStop fired must be delivered to
        /// platform managers (so window-destroy bookkeeping happens) and to
        /// any subscribers still reading. Coroutines whose handles are already
        /// in the owner mailbox are likewise resumed; they observe stop on
        /// their own next suspension point. The contract for cross-thread
        /// awaiters is therefore: do not call @ref SubmitResume after stop —
        /// see @ref SubmitResume's caller-side notes.
        [[nodiscard]] auto Run() -> stdexec::sender auto;

    private:
        void DispatchEvent(const SystemEvent& ev);
        void DrainNativeOnce();
        void DrainExternalOnce();
        void DrainOwnerMailboxOnce();

        // Static trampolines for NativeEventSink (function pointer + opaque state).
        static void DispatchTrampoline(void* state, SystemEvent ev) noexcept;

        // ---- Platform-apartment Managers: pointers into caller-owned objects ----
        PlatformMgrs platformMgrs_{};

        // ---- Inbox: Dedicated Managers (and any other thread) post here ----
        MpscQueue<SystemEvent, 256> inbox_{};

        // ---- OwnerExecutor mailbox: cross-thread coroutine resumption ----
        MpscQueue<std::coroutine_handle<>, 256> ownerMailbox_{};

        // ---- Topic fan-out: subscribers, written by Platform thread only ----
        std::vector<std::unique_ptr<EventChannel>> channels_{};

        // ---- Native bridge: blocking wait, wake, native-queue drain ----
        PlatformBackend backend_{};

        // ---- Context wired by AttachContext ----
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
    Result<void> EventPump<Ms...>::AttachManagers(Ms&... mgrs) {
        // (1) Stash Platform-apartment manager pointers in declared order.
        constexpr auto plat = Detail::IndicesByDomain<ScheduleDomain::PlatformThread, Ms...>();
        auto bag = std::tuple<Ms*...>{&mgrs...};

        [&]<std::size_t... I>(std::index_sequence<I...>) {
            ((std::get<I>(platformMgrs_) = std::get<plat.first[I]>(bag)), ...);
        }(std::make_index_sequence<plat.second>{});

        // (2) Start each Dedicated-apartment manager. First failure aborts the
        // bring-up and is returned verbatim; later managers are not contacted.
        constexpr auto ded = Detail::IndicesByDomain<ScheduleDomain::DedicatedThread, Ms...>();
        auto post = [this](SystemEvent ev) noexcept { return TryPostExternal(std::move(ev)); };

        Result<void> startResult{};
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            // Short-circuit on first error: the unary &-fold of a bool sequence
            // stops evaluating once any operand is false, so subsequent managers
            // never see Start().
            (void)((startResult = std::get<ded.first[I]>(bag)->Start(stop_, post, *scope_), startResult.has_value()) &&
                   ...);
        }(std::make_index_sequence<ded.second>{});

        if (!startResult) {
            return startResult;
        }
        // (3) FreeThreaded managers: nothing to wire — they're stateless or atomically protected.
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
        // The mailbox is sized for the project's maximum in-flight cross-thread
        // coroutines (spec §6.5). Overflow is a design-invariant violation, not
        // a runtime condition; silently dropping a handle would leak a coroutine.
        if (!ownerMailbox_.TryPush(h)) {
            std::terminate();
        }
        backend_.Wake();
    }

    template<class... Ms>
    void EventPump<Ms...>::DispatchEvent(const SystemEvent& ev) {
        std::visit(
            [this]<class P>(const P& payload) {
                // ----- Bookkeep stage: compile-time-expanded over PlatformMgrs -----
                // Reuses the canonical convention from SystemEvent.h: a Manager
                // opts in by declaring `void On(const P&) noexcept`. The fold-
                // expanded `if constexpr` is the same shape as
                // `Mashiro::DispatchBookkeep`, just lifted across the pack so the
                // (Manager, Payload) table is materialised here at compile time.
                constexpr std::size_t N = std::tuple_size_v<PlatformMgrs>;
                [&]<std::size_t... I>(std::index_sequence<I...>) {
                    (([&] {
                         using M = std::remove_pointer_t<std::tuple_element_t<I, PlatformMgrs>>;
                         if constexpr (Traits::Event::HandlesBookkeep<M, P>) {
                             std::get<I>(platformMgrs_)->On(payload);
                         }
                     }()),
                     ...);
                }(std::make_index_sequence<N>{});

                // ----- Broadcast stage: 1→N fan-out (Topic) -----
                for (auto& ch : channels_) {
                    (void)ch->TryPush(SystemEvent{payload});
                }
            },
            ev);
    }

    template<class... Ms>
    void EventPump<Ms...>::DispatchTrampoline(void* state, SystemEvent ev) noexcept {
        static_cast<EventPump*>(state)->DispatchEvent(ev);
    }

    template<class... Ms>
    void EventPump<Ms...>::DrainNativeOnce() {
        backend_.EnumerateNative(NativeEventSink{&DispatchTrampoline, this});
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
        // exec::repeat_until consumes a sender whose value completion is a bool;
        // it re-runs the sender until the value is true. Our body returns
        // `stop_.stop_requested()` after one iteration of the wait+drain cycle,
        // which gives the loop the documented drain-after-stop guarantee: the
        // last iteration runs the three drains exactly the same way as every
        // earlier one, then signals stop on its return value.
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


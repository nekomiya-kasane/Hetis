/**
 * @file EventChannel.h
 * @brief Subscriber endpoint into the @ref Mashiro::Platform::EventPump broadcast.
 *
 * Each EventChannel is a single-producer / single-consumer ring: the Platform
 * thread is the sole producer (via @c EventPump::DispatchEvent), and the
 * subscribing thread is the sole consumer. The channel must be position-stable
 * because the SPSC ring's correctness rests on its address not changing - so
 * channels are always allocated through @c EventPump::AddChannel, which holds
 * them indirected by @c std::unique_ptr.
 *
 * @par Capacity policy (intentional uniformity)
 * Every channel uses the same compile-time capacity. A slow subscriber should
 * surface as drops, not be smoothed over with a deeper queue - per-subscriber
 * tuning would invite "make my queue bigger" instead of "fix my consumer".
 *
 * @ingroup Platform
 */
#pragma once

#include "Mashiro/Core/SpscRingBuffer.h"
#include "Mashiro/Platform/SystemEvent.h"

#include <stdexec/execution.hpp>

#include <atomic>
#include <cstddef>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace Mashiro::Platform {

    namespace Detail {

        template<class Receiver>
        struct NextBatchOpState;

    } // namespace Detail

    /// @brief Compile-time capacity of every EventChannel's SPSC ring.
    inline constexpr std::size_t kEventChannelCapacity = 256;

    /**
     * @brief Owning, fixed-capacity batch returned by @ref EventChannel::NextBatch.
     *
     * The batch is the consumer-visible drain boundary: it owns moved-out @ref SystemEvent values,
     * keeps iteration independent from the channel, and never allocates on the heap.
     */
    class EventBatch {
    public:
        using value_type = SystemEvent;

        class Iterator {
        public:
            using difference_type = std::ptrdiff_t;
            using value_type = SystemEvent;
            using reference = value_type&;
            using pointer = value_type*;
            using iterator_category = std::forward_iterator_tag;

            Iterator() noexcept = default;
            Iterator(EventBatch* batch, std::size_t index) noexcept : batch_(batch), index_(index) {}

            [[nodiscard]] reference operator*() const noexcept { return (*batch_)[index_]; }
            [[nodiscard]] pointer operator->() const noexcept { return &(*batch_)[index_]; }

            Iterator& operator++() noexcept {
                ++index_;
                return *this;
            }

            Iterator operator++(int) noexcept {
                Iterator old = *this;
                ++(*this);
                return old;
            }

            [[nodiscard]] friend bool operator==(Iterator a, Iterator b) noexcept {
                return a.batch_ == b.batch_ && a.index_ == b.index_;
            }

        private:
            EventBatch*  batch_{nullptr};
            std::size_t index_{0};
        };

        class ConstIterator {
        public:
            using difference_type = std::ptrdiff_t;
            using value_type = SystemEvent;
            using reference = const value_type&;
            using pointer = const value_type*;
            using iterator_category = std::forward_iterator_tag;

            ConstIterator() noexcept = default;
            ConstIterator(const EventBatch* batch, std::size_t index) noexcept : batch_(batch), index_(index) {}

            [[nodiscard]] reference operator*() const noexcept { return (*batch_)[index_]; }
            [[nodiscard]] pointer operator->() const noexcept { return &(*batch_)[index_]; }

            ConstIterator& operator++() noexcept {
                ++index_;
                return *this;
            }

            ConstIterator operator++(int) noexcept {
                ConstIterator old = *this;
                ++(*this);
                return old;
            }

            [[nodiscard]] friend bool operator==(ConstIterator a, ConstIterator b) noexcept {
                return a.batch_ == b.batch_ && a.index_ == b.index_;
            }

        private:
            const EventBatch* batch_{nullptr};
            std::size_t index_{0};
        };

        EventBatch() noexcept = default;
        EventBatch(const EventBatch&) = delete;
        EventBatch& operator=(const EventBatch&) = delete;

        EventBatch(EventBatch&& other) noexcept { MoveFrom(std::move(other)); }

        EventBatch& operator=(EventBatch&& other) noexcept {
            if (this != &other) {
                Clear();
                MoveFrom(std::move(other));
            }
            return *this;
        }

        ~EventBatch() noexcept { Clear(); }

        [[nodiscard]] std::size_t size() const noexcept { return size_; }
        [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

        [[nodiscard]] value_type& operator[](std::size_t index) noexcept { return *Ptr(index); }
        [[nodiscard]] const value_type& operator[](std::size_t index) const noexcept { return *Ptr(index); }

        [[nodiscard]] Iterator begin() noexcept { return Iterator{this, 0}; }
        [[nodiscard]] Iterator end() noexcept { return Iterator{this, size_}; }
        [[nodiscard]] ConstIterator begin() const noexcept { return ConstIterator{this, 0}; }
        [[nodiscard]] ConstIterator end() const noexcept { return ConstIterator{this, size_}; }

    private:
        friend class EventChannel;

        [[nodiscard]] value_type* Ptr(std::size_t index) noexcept {
            return std::launder(reinterpret_cast<value_type*>(storage_[index]));
        }

        [[nodiscard]] const value_type* Ptr(std::size_t index) const noexcept {
            return std::launder(reinterpret_cast<const value_type*>(storage_[index]));
        }

        void Push(SystemEvent&& event) noexcept {
            std::construct_at(Ptr(size_), std::move(event));
            ++size_;
        }

        void Clear() noexcept {
            for (std::size_t i = 0; i != size_; ++i) {
                std::destroy_at(Ptr(i));
            }
            size_ = 0;
        }

        void MoveFrom(EventBatch&& other) noexcept {
            for (SystemEvent& event : other) {
                Push(std::move(event));
            }
            other.Clear();
        }

        alignas(value_type) std::byte storage_[kEventChannelCapacity][sizeof(value_type)]{};
        std::size_t size_{0};
    };

    /**
     * @brief One subscriber endpoint into the EventPump broadcast.
     *
     * @par Concurrency
     * One producer (Platform thread) and one consumer (the subscribing thread).
     * Concurrent consumers are undefined behaviour.
     *
     * @par Reactive consumption (@ref NextBatch)
     * Beyond the polling @ref TryPop, the channel exposes a sender form
     * @ref NextBatch that completes with @c set_value(EventBatch) when at least
     * one event has been atomically drained, or @c set_stopped if the receiver's
     * stop_token fires first. The mechanism is one extra atomic (@c pushEpoch_)
     * bumped per push and one futex @c notify_one - zero cost on the no-waiter
     * happy path, one syscall on the wake edge.
     */
    class EventChannel {
    public:
        EventChannel() noexcept = default;

        EventChannel(const EventChannel&) = delete;
        EventChannel& operator=(const EventChannel&) = delete;
        EventChannel(EventChannel&&) = delete;
        EventChannel& operator=(EventChannel&&) = delete;

        /// @brief Producer side (Platform thread). Returns false on full ring.
        ///
        /// On a successful push, bumps @ref pushEpoch_ (release) and notifies
        /// one parked @ref NextBatch waiter. @c notify_one is a cheap no-op
        /// when no waiter is parked, so the cost is one fetch_add per push.
        /// Order: ring publish (release inside @c SpscRingBuffer) -> epoch bump
        /// (release) -> notify_one. The consumer's acquire-load of the epoch
        /// therefore synchronises with the ring publication too.
        [[nodiscard]] bool TryPush(SystemEvent ev) noexcept {
            const bool ok = ring_.TryPush(std::move(ev));
            if (ok) {
                pushEpoch_.fetch_add(1, std::memory_order_release);
                pushEpoch_.notify_one();
            }
            return ok;
        }

        /// @brief Consumer side (subscribing thread). Returns false when empty.
        [[nodiscard]] bool TryPop(SystemEvent& out) noexcept { return ring_.TryPop(out); }

        /// @brief Consumer-side observation: ring empty? Always exact when called from the single consumer thread.
        [[nodiscard]] bool Empty() const noexcept { return ring_.Empty(); }

        /// @brief Sender form: completes with a drained @ref EventBatch, or @c set_stopped on receiver stop.
        ///
        /// Lost-wake-free by construction: the awaiter checks the ring (early complete on non-empty), snapshots
        /// @c pushEpoch_, re-checks the ring, then parks on @c pushEpoch_.wait(snapshot). A producer that pushes
        /// between snapshot and wait advances the epoch so @c wait returns immediately.
        [[nodiscard]] auto NextBatch() noexcept;

    private:
        template<class R>
        friend struct Detail::NextBatchOpState;

        std::uint64_t LoadEpoch() const noexcept { return pushEpoch_.load(std::memory_order_acquire); }
        void WaitEpoch(std::uint64_t snapshot) const noexcept { pushEpoch_.wait(snapshot, std::memory_order_acquire); }
        void NotifyEpochForStop() noexcept { pushEpoch_.notify_all(); }

        [[nodiscard]] EventBatch DrainBatch() noexcept {
            EventBatch batch;
            ring_.Drain([&batch](SystemEvent&& event) noexcept { batch.Push(std::move(event)); });
            return batch;
        }

        SpscRingBuffer<SystemEvent, kEventChannelCapacity> ring_{};
        std::atomic<std::uint64_t> pushEpoch_{0};
    };

    namespace Detail {

        template<class Receiver>
        struct NextBatchOpState {
            EventChannel* owner;
            Receiver receiver;

            struct OnStop {
                EventChannel* owner;
                void operator()() const noexcept { owner->NotifyEpochForStop(); }
            };

            using StopToken = stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;
            using StopCallback = typename StopToken::template callback_type<OnStop>;
            std::optional<StopCallback> stopCb;

            void start() noexcept {
                // 1. normal case: new events enter
                if (!owner->Empty()) {
                    stdexec::set_value(std::move(receiver), owner->DrainBatch());
                    return;
                }

                // 2. going to pass stop token: ask receiver what kind of stop token it likes
                auto token = stdexec::get_stop_token(stdexec::get_env(receiver));
                stopCb.emplace(token, OnStop{owner});
                auto snapshot = owner->LoadEpoch();

                for (;;) {
                    if (!owner->Empty()) {
                        stopCb.reset();
                        stdexec::set_value(std::move(receiver), owner->DrainBatch());
                        return;
                    }
                    if (token.stop_requested()) {
                        stopCb.reset();
                        stdexec::set_stopped(std::move(receiver));
                        return;
                    }
                    owner->WaitEpoch(snapshot);
                    snapshot = owner->LoadEpoch();
                }
            }
        };

        struct NextBatchSender {
            EventChannel* owner;

            using sender_concept = stdexec::sender_t;
            using completion_signatures =
                stdexec::completion_signatures<stdexec::set_value_t(EventBatch), stdexec::set_stopped_t()>;

            template<class Receiver>
            auto connect(Receiver&& r) const noexcept -> NextBatchOpState<std::remove_cvref_t<Receiver>> {
                return NextBatchOpState<std::remove_cvref_t<Receiver>>{
                    .owner = owner,
                    .receiver = std::forward<Receiver>(r),
                };
            }
        };

    } // namespace Detail

    inline auto EventChannel::NextBatch() noexcept {
        return Detail::NextBatchSender{this};
    }

} // namespace Mashiro::Platform

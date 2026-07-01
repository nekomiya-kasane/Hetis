/**
 * @file AsyncQueue.h
 * @brief Experimental P2300-native asynchronous queue facade with non-blocking operation states.
 * @ingroup Concurrency
 */
#pragma once

#include "Mashiro/Core/FalseSharing.h"
#include "Mashiro/Experimental/Concurrency/IntrusiveWaitQueue.h"
#include "Mashiro/Experimental/Concurrency/QueueAdapter.h"
#include "Mashiro/Experimental/Concurrency/QueueError.h"

#include <stdexec/execution.hpp>

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

namespace Mashiro::Experimental::Concurrency {

    template<class Storage, class Adapter = QueueAdapter<Storage>>
        requires AsyncQueueAdapter<Adapter>
    class AsyncQueue;

    template<class Queue>
    class ProducerPort;

    template<class Queue>
    class ConsumerPort;

    namespace Detail {

        struct QueueControlDomain : Mashiro::Concurrency::ContentionDomain {};
        struct QueueStorageDomain : Mashiro::Concurrency::ContentionDomain {};
        struct QueueLifecycleDomain : Mashiro::Concurrency::ContentionDomain {};

        enum class QueueLifecycle : std::uint8_t { Open, Closing, Aborted };
        enum class OperationKind : std::uint8_t { Push, Pop, PushBatch, PopBatch };
        enum class CompletionKind : std::uint8_t { Value, Stopped, Error };
        enum class OperationPhase : std::uint8_t {
            Constructed,
            Starting,
            StopRequested,
            Pending,
            Completing,
            Completed,
        };

        [[nodiscard]] constexpr bool IsProducer(OperationKind kind) noexcept {
            return kind == OperationKind::Push || kind == OperationKind::PushBatch;
        }

        [[nodiscard]] constexpr QueueError ErrorFromStatus(QueueTransferStatus status) noexcept {
            switch (status) {
            case QueueTransferStatus::BatchTooLarge:
                return kQueueBatchTooLarge;
            case QueueTransferStatus::ContractViolation:
                return kQueueContractViolation;
            case QueueTransferStatus::Committed:
            case QueueTransferStatus::BackPressure:
                return {};
            }
            return kQueueContractViolation;
        }

        struct OperationBase;
        inline thread_local OperationBase* activeStopCallback = nullptr;

        template<class Queue, class Receiver>
        struct PushOp;
        template<class Queue, class Receiver>
        struct PopOp;
        template<class Queue, class Receiver>
        struct PushBatchOp;
        template<class Queue, class Receiver>
        struct PopBatchOp;

        struct OperationBase : WaitNode {
            using TryFn = bool (*)(OperationBase&) noexcept;
            using CompleteFn = void (*)(OperationBase&) noexcept;
            using ErrorFn = void (*)(OperationBase&, QueueError) noexcept;

            OperationKind kind;
            TryFn tryImmediateLocked;
            TryFn trySatisfyLocked;
            CompleteFn completeValue;
            CompleteFn completeStopped;
            ErrorFn completeError;
            OperationBase* completeNext{nullptr};
            QueueError completionError{};
            QueueError readyError{};
            CompletionKind completionKind{CompletionKind::Value};
            std::atomic<OperationPhase> phase{OperationPhase::Constructed};

            OperationBase(OperationKind opKind,
                          TryFn immediate,
                          TryFn satisfy,
                          CompleteFn value,
                          CompleteFn stopped,
                          ErrorFn error) noexcept
                : kind(opKind),
                  tryImmediateLocked(immediate),
                  trySatisfyLocked(satisfy),
                  completeValue(value),
                  completeStopped(stopped),
                  completeError(error) {}
        };

        class StopCallbackScope {
        public:
            explicit StopCallbackScope(OperationBase& op) noexcept : previous_(activeStopCallback) {
                activeStopCallback = &op;
            }

            ~StopCallbackScope() { activeStopCallback = previous_; }

            StopCallbackScope(const StopCallbackScope&) = delete;
            StopCallbackScope& operator=(const StopCallbackScope&) = delete;

        private:
            OperationBase* previous_;
        };

        template<class Op>
        void ReleaseStopCallback(Op& op) noexcept {
            if (activeStopCallback != static_cast<OperationBase*>(&op)) {
                op.stopCallback.reset();
            }
        }

        class CompletionList {
        public:
            void PushValue(OperationBase& op) noexcept { Push(op, CompletionKind::Value, {}); }
            void PushStopped(OperationBase& op) noexcept { Push(op, CompletionKind::Stopped, {}); }
            void PushError(OperationBase& op, QueueError error) noexcept { Push(op, CompletionKind::Error, error); }

            void CompleteAll() noexcept {
                while (head_ != nullptr) {
                    OperationBase* op = head_;
                    head_ = head_->completeNext;
                    op->completeNext = nullptr;
                    const CompletionKind kind = op->completionKind;
                    const QueueError error = op->completionError;
                    op->phase.store(OperationPhase::Completed, std::memory_order_release);
                    if (kind == CompletionKind::Value) {
                        op->completeValue(*op);
                    } else if (kind == CompletionKind::Stopped) {
                        op->completeStopped(*op);
                    } else {
                        op->completeError(*op, error);
                    }
                }
                tail_ = nullptr;
            }

        private:
            void Push(OperationBase& op, CompletionKind kind, QueueError error) noexcept {
                op.completionKind = kind;
                op.completionError = error;
                op.completeNext = nullptr;
                if (tail_ != nullptr) {
                    tail_->completeNext = &op;
                } else {
                    head_ = &op;
                }
                tail_ = &op;
            }

            OperationBase* head_{nullptr};
            OperationBase* tail_{nullptr};
        };

    } // namespace Detail
    template<class Storage, class Adapter>
        requires AsyncQueueAdapter<Adapter>
    class AsyncQueue {
    public:
        using storage_type = Storage;
        using adapter_type = Adapter;
        using value_type = typename Adapter::value_type;
        using batch_type = typename Adapter::batch_type;

        static constexpr QueueRoleCardinality producer_cardinality = Adapter::producer_cardinality;
        static constexpr QueueRoleCardinality consumer_cardinality = Adapter::consumer_cardinality;
        static constexpr std::size_t capacity = Adapter::capacity;

        AsyncQueue() requires std::default_initializable<storage_type> = default;

        explicit AsyncQueue(storage_type storage) noexcept(std::is_nothrow_move_constructible_v<storage_type>)
            : storage_(std::move(storage)) {}

        AsyncQueue(const AsyncQueue&) = delete;
        AsyncQueue& operator=(const AsyncQueue&) = delete;
        AsyncQueue(AsyncQueue&&) = delete;
        AsyncQueue& operator=(AsyncQueue&&) = delete;

        /** @brief Teardown is abortive: no operation state may remain parked on a destroyed facade. */
        ~AsyncQueue() { Abort(kQueueAborted); }

        [[nodiscard]] std::optional<ProducerPort<AsyncQueue>> TryProducer() noexcept {
            if constexpr (producer_cardinality == QueueRoleCardinality::Single) {
                bool expected = false;
                if (!producerIssued_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                    return std::nullopt;
                }
            }
            return ProducerPort<AsyncQueue>{this};
        }

        [[nodiscard]] ProducerPort<AsyncQueue> Producer() noexcept {
            auto port = TryProducer();
            if (!port.has_value()) [[unlikely]] {
                std::terminate();
            }
            return std::move(*port);
        }

        [[nodiscard]] std::optional<ConsumerPort<AsyncQueue>> TryConsumer() noexcept {
            if constexpr (consumer_cardinality == QueueRoleCardinality::Single) {
                bool expected = false;
                if (!consumerIssued_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                    return std::nullopt;
                }
            }
            return ConsumerPort<AsyncQueue>{this};
        }

        [[nodiscard]] ConsumerPort<AsyncQueue> Consumer() noexcept {
            auto port = TryConsumer();
            if (!port.has_value()) [[unlikely]] {
                std::terminate();
            }
            return std::move(*port);
        }

        void Close() noexcept {
            Detail::CompletionList completions;
            {
                std::scoped_lock lock{mutex_};
                if (lifecycle_ != Detail::QueueLifecycle::Open) {
                    return;
                }
                lifecycle_ = Detail::QueueLifecycle::Closing;
                FailPendingProducersLocked(completions, kQueueClosed);
                ProgressLocked(completions);
                StopConsumersIfClosedAndEmptyLocked(completions);
            }
            completions.CompleteAll();
        }

        void Abort(QueueError error = kQueueAborted) noexcept {
            Detail::CompletionList completions;
            {
                std::scoped_lock lock{mutex_};
                lifecycle_ = Detail::QueueLifecycle::Aborted;
                abortError_ = error ? error : kQueueAborted;
                DrainAllLocked(pendingProducers_, completions, abortError_);
                DrainAllLocked(pendingConsumers_, completions, abortError_);
            }
            completions.CompleteAll();
        }

        [[nodiscard]] bool TryPush(value_type value) noexcept {
            Detail::CompletionList completions;
            bool committed = false;
            {
                std::scoped_lock lock{mutex_};
                if (lifecycle_ != Detail::QueueLifecycle::Open || !pendingProducers_.Empty()) {
                    return false;
                }
                committed = Adapter::TryPush(storage_, value);
                if (committed) {
                    ProgressLocked(completions);
                    StopConsumersIfClosedAndEmptyLocked(completions);
                }
            }
            completions.CompleteAll();
            return committed;
        }

        [[nodiscard]] std::optional<value_type> TryPop() noexcept {
            Detail::CompletionList completions;
            std::optional<value_type> value;
            {
                std::scoped_lock lock{mutex_};
                if (lifecycle_ == Detail::QueueLifecycle::Aborted || !pendingConsumers_.Empty()) {
                    return std::nullopt;
                }
                value = Adapter::TryPop(storage_);
                if (value.has_value()) {
                    ProgressLocked(completions);
                    StopConsumersIfClosedAndEmptyLocked(completions);
                }
            }
            completions.CompleteAll();
            return value;
        }

        [[nodiscard]] bool IsEmpty() noexcept {
            std::scoped_lock lock{mutex_};
            return Adapter::IsEmpty(storage_);
        }

    private:
        template<class>
        friend class ProducerPort;
        template<class>
        friend class ConsumerPort;
        template<class, class>
        friend struct Detail::PushOp;
        template<class, class>
        friend struct Detail::PopOp;
        template<class, class>
        friend struct Detail::PushBatchOp;
        template<class, class>
        friend struct Detail::PopBatchOp;

        [[nodiscard]] bool StartOperation(Detail::OperationBase& op) noexcept {
            Detail::CompletionList completions;
            bool accepted = false;
            {
                std::scoped_lock lock{mutex_};
                if (op.phase.load(std::memory_order_acquire) != Detail::OperationPhase::Starting) {
                    return false;
                }
                if (lifecycle_ == Detail::QueueLifecycle::Aborted) {
                    MarkCompletingLocked(op);
                    completions.PushError(op, abortError_);
                    accepted = true;
                } else if (IsClosedProducerLocked(op.kind)) {
                    MarkCompletingLocked(op);
                    completions.PushError(op, kQueueClosed);
                    accepted = true;
                } else if (IsClosedEmptyConsumerLocked(op.kind)) {
                    MarkCompletingLocked(op);
                    completions.PushStopped(op);
                    accepted = true;
                } else if (!HasOlderSameSideWaiterLocked(op.kind) && op.tryImmediateLocked(op)) {
                    MarkCompletingLocked(op);
                    completions.PushValue(op);
                    ProgressLocked(completions);
                    StopConsumersIfClosedAndEmptyLocked(completions);
                    accepted = true;
                } else if (op.readyError) {
                    MarkCompletingLocked(op);
                    completions.PushError(op, op.readyError);
                    accepted = true;
                } else {
                    EnqueueLocked(op);
                    ProgressLocked(completions);
                    StopConsumersIfClosedAndEmptyLocked(completions);
                    accepted = true;
                }
            }
            completions.CompleteAll();
            return accepted;
        }

        void CancelOperation(Detail::OperationBase& op) noexcept {
            Detail::CompletionList completions;
            {
                std::scoped_lock lock{mutex_};
                const auto phase = op.phase.load(std::memory_order_acquire);
                if (phase == Detail::OperationPhase::Starting) {
                    op.phase.store(Detail::OperationPhase::StopRequested, std::memory_order_release);
                } else if (phase == Detail::OperationPhase::Pending) {
                    QueueFor(op.kind).Erase(op);
                    MarkCompletingLocked(op);
                    completions.PushStopped(op);
                }
            }
            completions.CompleteAll();
        }

        [[nodiscard]] bool TryPushLocked(value_type& value) noexcept { return Adapter::TryPush(storage_, value); }
        [[nodiscard]] std::optional<value_type> TryPopLocked() noexcept { return Adapter::TryPop(storage_); }
        [[nodiscard]] QueueTransferStatus TryPushBatchLocked(batch_type& batch) noexcept {
            return Adapter::TryPushBatch(storage_, batch);
        }
        [[nodiscard]] batch_type TryPopBatchLocked() noexcept { return Adapter::TryPopBatch(storage_); }

        [[nodiscard]] bool HasOlderSameSideWaiterLocked(Detail::OperationKind kind) const noexcept {
            return Detail::IsProducer(kind) ? !pendingProducers_.Empty() : !pendingConsumers_.Empty();
        }

        [[nodiscard]] bool IsClosedProducerLocked(Detail::OperationKind kind) const noexcept {
            return Detail::IsProducer(kind) && lifecycle_ == Detail::QueueLifecycle::Closing;
        }

        [[nodiscard]] bool IsClosedEmptyConsumerLocked(Detail::OperationKind kind) noexcept {
            return !Detail::IsProducer(kind) && lifecycle_ == Detail::QueueLifecycle::Closing &&
                   Adapter::IsEmpty(storage_);
        }

        void MarkCompletingLocked(Detail::OperationBase& op) noexcept {
            op.phase.store(Detail::OperationPhase::Completing, std::memory_order_release);
        }

        void EnqueueLocked(Detail::OperationBase& op) noexcept {
            op.phase.store(Detail::OperationPhase::Pending, std::memory_order_release);
            QueueFor(op.kind).PushBack(op);
        }

        [[nodiscard]] Detail::IntrusiveWaitQueue& QueueFor(Detail::OperationKind kind) noexcept {
            return Detail::IsProducer(kind) ? pendingProducers_ : pendingConsumers_;
        }

        void ProgressLocked(Detail::CompletionList& completions) noexcept {
            bool madeProgress = true;
            while (madeProgress) {
                madeProgress = false;
                madeProgress = TryCompleteFrontLocked(pendingProducers_, completions) || madeProgress;
                madeProgress = TryCompleteFrontLocked(pendingConsumers_, completions) || madeProgress;
            }
        }

        [[nodiscard]] bool TryCompleteFrontLocked(Detail::IntrusiveWaitQueue& queue,
                                                  Detail::CompletionList& completions) noexcept {
            Detail::WaitNode* node = queue.Front();
            if (node == nullptr) {
                return false;
            }
            auto& op = *static_cast<Detail::OperationBase*>(node);
            if (lifecycle_ == Detail::QueueLifecycle::Aborted) {
                queue.Erase(op);
                MarkCompletingLocked(op);
                completions.PushError(op, abortError_);
                return true;
            }
            if (IsClosedProducerLocked(op.kind)) {
                queue.Erase(op);
                MarkCompletingLocked(op);
                completions.PushError(op, kQueueClosed);
                return true;
            }
            if (IsClosedEmptyConsumerLocked(op.kind)) {
                queue.Erase(op);
                MarkCompletingLocked(op);
                completions.PushStopped(op);
                return true;
            }
            if (!op.trySatisfyLocked(op)) {
                if (op.readyError) {
                    queue.Erase(op);
                    MarkCompletingLocked(op);
                    completions.PushError(op, op.readyError);
                    return true;
                }
                return false;
            }
            queue.Erase(op);
            MarkCompletingLocked(op);
            completions.PushValue(op);
            return true;
        }

        void FailPendingProducersLocked(Detail::CompletionList& completions, QueueError error) noexcept {
            DrainAllLocked(pendingProducers_, completions, error);
        }

        void StopConsumersIfClosedAndEmptyLocked(Detail::CompletionList& completions) noexcept {
            if (lifecycle_ != Detail::QueueLifecycle::Closing || !Adapter::IsEmpty(storage_)) {
                return;
            }
            while (!pendingConsumers_.Empty()) {
                auto& op = *static_cast<Detail::OperationBase*>(pendingConsumers_.PopFront());
                MarkCompletingLocked(op);
                completions.PushStopped(op);
            }
        }

        void DrainAllLocked(Detail::IntrusiveWaitQueue& queue,
                            Detail::CompletionList& completions,
                            QueueError error) noexcept {
            while (!queue.Empty()) {
                auto& op = *static_cast<Detail::OperationBase*>(queue.PopFront());
                MarkCompletingLocked(op);
                completions.PushError(op, error);
            }
        }

        [[=Detail::QueueControlDomain{}]] alignas(Platform::kCacheLineSize) std::mutex mutex_{};
        [[=Detail::QueueStorageDomain{}]] alignas(Platform::kCacheLineSize) storage_type storage_{};
        [[=Detail::QueueLifecycleDomain{}]] alignas(Platform::kCacheLineSize)
        Detail::QueueLifecycle lifecycle_{Detail::QueueLifecycle::Open};
        [[=Detail::QueueLifecycleDomain{}]] QueueError abortError_{kQueueAborted};
        [[=Detail::QueueControlDomain{}]] Detail::IntrusiveWaitQueue pendingProducers_{};
        [[=Detail::QueueControlDomain{}]] Detail::IntrusiveWaitQueue pendingConsumers_{};
        [[=Detail::QueueLifecycleDomain{}]] std::atomic_bool producerIssued_{false};
        [[=Detail::QueueLifecycleDomain{}]] std::atomic_bool consumerIssued_{false};
    };
    namespace Detail {

        template<class Op>
        void CompleteStopRequestedAfterRegistration(Op& op) noexcept {
            auto expected = OperationPhase::StopRequested;
            if (!op.phase.compare_exchange_strong(expected,
                                                  OperationPhase::Completed,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
                return;
            }
            op.stopCallback.reset();
            stdexec::set_stopped(std::move(op.receiver));
        }

        template<class Op>
        void StartWithStopToken(Op& op) noexcept {
            auto expected = OperationPhase::Constructed;
            if (!op.phase.compare_exchange_strong(expected,
                                                  OperationPhase::Starting,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
                return;
            }

            auto token = stdexec::get_stop_token(stdexec::get_env(op.receiver));
            if (token.stop_requested()) {
                op.phase.store(OperationPhase::Completed, std::memory_order_release);
                stdexec::set_stopped(std::move(op.receiver));
                return;
            }

            op.stopCallback.emplace(token, typename Op::OnStop{&op});
            if (!op.owner->StartOperation(op)) {
                CompleteStopRequestedAfterRegistration(op);
            }
        }

        template<class Queue, class Receiver>
        struct PushOp : OperationBase {
            using value_type = typename Queue::value_type;
            using StopToken = stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;

            struct OnStop {
                PushOp* self;
                void operator()() const noexcept {
                    StopCallbackScope scope{*self};
                    self->owner->CancelOperation(*self);
                }
            };

            using StopCallback = typename StopToken::template callback_type<OnStop>;

            Queue* owner;
            Receiver receiver;
            value_type value;
            std::optional<StopCallback> stopCallback;

            PushOp(Queue* q, Receiver&& r, value_type&& v) noexcept(std::is_nothrow_move_constructible_v<Receiver>)
                : OperationBase(OperationKind::Push,
                                &PushOp::TryImmediate,
                                &PushOp::TryImmediate,
                                &PushOp::CompleteValue,
                                &PushOp::CompleteStopped,
                                &PushOp::CompleteError),
                  owner(q),
                  receiver(std::move(r)),
                  value(std::move(v)) {}

            void start() & noexcept { StartWithStopToken(*this); }

            [[nodiscard]] static bool TryImmediate(OperationBase& base) noexcept {
                auto& self = static_cast<PushOp&>(base);
                return self.owner->TryPushLocked(self.value);
            }

            static void CompleteValue(OperationBase& base) noexcept {
                auto& self = static_cast<PushOp&>(base);
                ReleaseStopCallback(self);
                stdexec::set_value(std::move(self.receiver));
            }

            static void CompleteStopped(OperationBase& base) noexcept {
                auto& self = static_cast<PushOp&>(base);
                ReleaseStopCallback(self);
                stdexec::set_stopped(std::move(self.receiver));
            }

            static void CompleteError(OperationBase& base, QueueError error) noexcept {
                auto& self = static_cast<PushOp&>(base);
                ReleaseStopCallback(self);
                stdexec::set_error(std::move(self.receiver), error);
            }
        };

        template<class Queue, class Receiver>
        struct PopOp : OperationBase {
            using value_type = typename Queue::value_type;
            using StopToken = stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;

            struct OnStop {
                PopOp* self;
                void operator()() const noexcept {
                    StopCallbackScope scope{*self};
                    self->owner->CancelOperation(*self);
                }
            };

            using StopCallback = typename StopToken::template callback_type<OnStop>;

            Queue* owner;
            Receiver receiver;
            std::optional<value_type> value;
            std::optional<StopCallback> stopCallback;

            PopOp(Queue* q, Receiver&& r) noexcept(std::is_nothrow_move_constructible_v<Receiver>)
                : OperationBase(OperationKind::Pop,
                                &PopOp::TryImmediate,
                                &PopOp::TryImmediate,
                                &PopOp::CompleteValue,
                                &PopOp::CompleteStopped,
                                &PopOp::CompleteError),
                  owner(q),
                  receiver(std::move(r)) {}

            void start() & noexcept { StartWithStopToken(*this); }

            [[nodiscard]] static bool TryImmediate(OperationBase& base) noexcept {
                auto& self = static_cast<PopOp&>(base);
                self.value = self.owner->TryPopLocked();
                return self.value.has_value();
            }

            static void CompleteValue(OperationBase& base) noexcept {
                auto& self = static_cast<PopOp&>(base);
                ReleaseStopCallback(self);
                stdexec::set_value(std::move(self.receiver), std::move(*self.value));
            }

            static void CompleteStopped(OperationBase& base) noexcept {
                auto& self = static_cast<PopOp&>(base);
                ReleaseStopCallback(self);
                stdexec::set_stopped(std::move(self.receiver));
            }

            static void CompleteError(OperationBase& base, QueueError error) noexcept {
                auto& self = static_cast<PopOp&>(base);
                ReleaseStopCallback(self);
                stdexec::set_error(std::move(self.receiver), error);
            }
        };

        template<class Queue, class Receiver>
        struct PushBatchOp : OperationBase {
            using batch_type = typename Queue::batch_type;
            using StopToken = stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;

            struct OnStop {
                PushBatchOp* self;
                void operator()() const noexcept {
                    StopCallbackScope scope{*self};
                    self->owner->CancelOperation(*self);
                }
            };

            using StopCallback = typename StopToken::template callback_type<OnStop>;

            Queue* owner;
            Receiver receiver;
            batch_type batch;
            std::optional<StopCallback> stopCallback;

            PushBatchOp(Queue* q, Receiver&& r, batch_type&& b)
                noexcept(std::is_nothrow_move_constructible_v<Receiver>)
                : OperationBase(OperationKind::PushBatch,
                                &PushBatchOp::TryImmediate,
                                &PushBatchOp::TryImmediate,
                                &PushBatchOp::CompleteValue,
                                &PushBatchOp::CompleteStopped,
                                &PushBatchOp::CompleteError),
                  owner(q),
                  receiver(std::move(r)),
                  batch(std::move(b)) {}

            void start() & noexcept { StartWithStopToken(*this); }

            [[nodiscard]] static bool TryImmediate(OperationBase& base) noexcept {
                auto& self = static_cast<PushBatchOp&>(base);
                const QueueTransferStatus status = self.owner->TryPushBatchLocked(self.batch);
                self.readyError = ErrorFromStatus(status);
                return status == QueueTransferStatus::Committed;
            }

            static void CompleteValue(OperationBase& base) noexcept {
                auto& self = static_cast<PushBatchOp&>(base);
                ReleaseStopCallback(self);
                stdexec::set_value(std::move(self.receiver));
            }

            static void CompleteStopped(OperationBase& base) noexcept {
                auto& self = static_cast<PushBatchOp&>(base);
                ReleaseStopCallback(self);
                stdexec::set_stopped(std::move(self.receiver));
            }

            static void CompleteError(OperationBase& base, QueueError error) noexcept {
                auto& self = static_cast<PushBatchOp&>(base);
                ReleaseStopCallback(self);
                stdexec::set_error(std::move(self.receiver), error);
            }
        };

        template<class Queue, class Receiver>
        struct PopBatchOp : OperationBase {
            using batch_type = typename Queue::batch_type;
            using StopToken = stdexec::stop_token_of_t<stdexec::env_of_t<Receiver>>;

            struct OnStop {
                PopBatchOp* self;
                void operator()() const noexcept {
                    StopCallbackScope scope{*self};
                    self->owner->CancelOperation(*self);
                }
            };

            using StopCallback = typename StopToken::template callback_type<OnStop>;

            Queue* owner;
            Receiver receiver;
            batch_type batch;
            std::optional<StopCallback> stopCallback;

            PopBatchOp(Queue* q, Receiver&& r) noexcept(std::is_nothrow_move_constructible_v<Receiver>)
                : OperationBase(OperationKind::PopBatch,
                                &PopBatchOp::TryImmediate,
                                &PopBatchOp::TryImmediate,
                                &PopBatchOp::CompleteValue,
                                &PopBatchOp::CompleteStopped,
                                &PopBatchOp::CompleteError),
                  owner(q),
                  receiver(std::move(r)) {}

            void start() & noexcept { StartWithStopToken(*this); }

            [[nodiscard]] static bool TryImmediate(OperationBase& base) noexcept {
                auto& self = static_cast<PopBatchOp&>(base);
                self.batch = self.owner->TryPopBatchLocked();
                return !self.batch.empty();
            }

            static void CompleteValue(OperationBase& base) noexcept {
                auto& self = static_cast<PopBatchOp&>(base);
                ReleaseStopCallback(self);
                stdexec::set_value(std::move(self.receiver), std::move(self.batch));
            }

            static void CompleteStopped(OperationBase& base) noexcept {
                auto& self = static_cast<PopBatchOp&>(base);
                ReleaseStopCallback(self);
                stdexec::set_stopped(std::move(self.receiver));
            }

            static void CompleteError(OperationBase& base, QueueError error) noexcept {
                auto& self = static_cast<PopBatchOp&>(base);
                ReleaseStopCallback(self);
                stdexec::set_error(std::move(self.receiver), error);
            }
        };
        template<class Queue>
        struct PopSender {
            using sender_concept = stdexec::sender_t;
            using value_type = typename Queue::value_type;
            using completion_signatures = stdexec::completion_signatures<
                stdexec::set_value_t(value_type),
                stdexec::set_stopped_t(),
                stdexec::set_error_t(QueueError)>;

            Queue* owner;

            template<class Receiver>
            [[nodiscard]] auto connect(Receiver&& receiver) const
                noexcept(std::is_nothrow_constructible_v<std::remove_cvref_t<Receiver>, Receiver>)
                    -> PopOp<Queue, std::remove_cvref_t<Receiver>> {
                return PopOp<Queue, std::remove_cvref_t<Receiver>>{owner, std::forward<Receiver>(receiver)};
            }
        };

        template<class Queue>
        struct PopBatchSender {
            using sender_concept = stdexec::sender_t;
            using batch_type = typename Queue::batch_type;
            using completion_signatures = stdexec::completion_signatures<
                stdexec::set_value_t(batch_type),
                stdexec::set_stopped_t(),
                stdexec::set_error_t(QueueError)>;

            Queue* owner;

            template<class Receiver>
            [[nodiscard]] auto connect(Receiver&& receiver) const
                noexcept(std::is_nothrow_constructible_v<std::remove_cvref_t<Receiver>, Receiver>)
                    -> PopBatchOp<Queue, std::remove_cvref_t<Receiver>> {
                return PopBatchOp<Queue, std::remove_cvref_t<Receiver>>{owner, std::forward<Receiver>(receiver)};
            }
        };

        template<class Queue>
        struct PushSender {
            using sender_concept = stdexec::sender_t;
            using value_type = typename Queue::value_type;
            using completion_signatures = stdexec::completion_signatures<
                stdexec::set_value_t(),
                stdexec::set_stopped_t(),
                stdexec::set_error_t(QueueError)>;

            Queue* owner;
            value_type value;

            template<class Receiver>
                requires std::copy_constructible<value_type>
            [[nodiscard]] auto connect(Receiver&& receiver) const&
                noexcept(std::is_nothrow_constructible_v<std::remove_cvref_t<Receiver>, Receiver> &&
                         std::is_nothrow_copy_constructible_v<value_type>)
                    -> PushOp<Queue, std::remove_cvref_t<Receiver>> {
                return PushOp<Queue, std::remove_cvref_t<Receiver>>{
                    owner,
                    std::forward<Receiver>(receiver),
                    value_type(value),
                };
            }

            template<class Receiver>
            [[nodiscard]] auto connect(Receiver&& receiver) &&
                noexcept(std::is_nothrow_constructible_v<std::remove_cvref_t<Receiver>, Receiver> &&
                         std::is_nothrow_move_constructible_v<value_type>)
                    -> PushOp<Queue, std::remove_cvref_t<Receiver>> {
                return PushOp<Queue, std::remove_cvref_t<Receiver>>{
                    owner,
                    std::forward<Receiver>(receiver),
                    std::move(value),
                };
            }
        };

        template<class Queue>
        struct PushBatchSender {
            using sender_concept = stdexec::sender_t;
            using batch_type = typename Queue::batch_type;
            using completion_signatures = stdexec::completion_signatures<
                stdexec::set_value_t(),
                stdexec::set_stopped_t(),
                stdexec::set_error_t(QueueError)>;

            Queue* owner;
            batch_type batch;

            template<class Receiver>
                requires std::copy_constructible<batch_type>
            [[nodiscard]] auto connect(Receiver&& receiver) const&
                noexcept(std::is_nothrow_constructible_v<std::remove_cvref_t<Receiver>, Receiver> &&
                         std::is_nothrow_copy_constructible_v<batch_type>)
                    -> PushBatchOp<Queue, std::remove_cvref_t<Receiver>> {
                return PushBatchOp<Queue, std::remove_cvref_t<Receiver>>{
                    owner,
                    std::forward<Receiver>(receiver),
                    batch_type(batch),
                };
            }

            template<class Receiver>
            [[nodiscard]] auto connect(Receiver&& receiver) &&
                noexcept(std::is_nothrow_constructible_v<std::remove_cvref_t<Receiver>, Receiver> &&
                         std::is_nothrow_move_constructible_v<batch_type>)
                    -> PushBatchOp<Queue, std::remove_cvref_t<Receiver>> {
                return PushBatchOp<Queue, std::remove_cvref_t<Receiver>>{
                    owner,
                    std::forward<Receiver>(receiver),
                    std::move(batch),
                };
            }
        };

    } // namespace Detail

    /** @brief Producer endpoint for an @ref AsyncQueue. */
    template<class Queue>
    class ProducerPort {
    public:
        explicit ProducerPort(Queue* owner) noexcept : owner_(owner) {}

        ProducerPort(const ProducerPort&)
            requires(Queue::producer_cardinality == QueueRoleCardinality::Multi) = default;
        ProducerPort(const ProducerPort&)
            requires(Queue::producer_cardinality == QueueRoleCardinality::Single) = delete;

        ProducerPort& operator=(const ProducerPort&)
            requires(Queue::producer_cardinality == QueueRoleCardinality::Multi) = default;
        ProducerPort& operator=(const ProducerPort&)
            requires(Queue::producer_cardinality == QueueRoleCardinality::Single) = delete;

        ProducerPort(ProducerPort&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}

        ProducerPort& operator=(ProducerPort&& other) noexcept {
            owner_ = std::exchange(other.owner_, nullptr);
            return *this;
        }

        template<class Value>
            requires std::constructible_from<typename Queue::value_type, Value&&>
        [[nodiscard]] auto Push(Value&& value)
            noexcept(std::is_nothrow_constructible_v<typename Queue::value_type, Value&&>) {
            return Detail::PushSender<Queue>{
                .owner = owner_,
                .value = typename Queue::value_type(std::forward<Value>(value)),
            };
        }

        template<class Batch>
            requires std::constructible_from<typename Queue::batch_type, Batch&&>
        [[nodiscard]] auto PushBatch(Batch&& batch)
            noexcept(std::is_nothrow_constructible_v<typename Queue::batch_type, Batch&&>) {
            return Detail::PushBatchSender<Queue>{
                .owner = owner_,
                .batch = typename Queue::batch_type(std::forward<Batch>(batch)),
            };
        }

        [[nodiscard]] bool TryPush(typename Queue::value_type value) noexcept {
            return owner_->TryPush(std::move(value));
        }

    private:
        Queue* owner_;
    };

    /** @brief Consumer endpoint for an @ref AsyncQueue. */
    template<class Queue>
    class ConsumerPort {
    public:
        explicit ConsumerPort(Queue* owner) noexcept : owner_(owner) {}

        ConsumerPort(const ConsumerPort&) = delete;
        ConsumerPort& operator=(const ConsumerPort&) = delete;
        ConsumerPort(ConsumerPort&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}

        ConsumerPort& operator=(ConsumerPort&& other) noexcept {
            owner_ = std::exchange(other.owner_, nullptr);
            return *this;
        }

        [[nodiscard]] auto Pop() noexcept {
            return Detail::PopSender<Queue>{.owner = owner_};
        }

        [[nodiscard]] auto PopBatch() noexcept {
            return Detail::PopBatchSender<Queue>{.owner = owner_};
        }

        [[nodiscard]] std::optional<typename Queue::value_type> TryPop() noexcept {
            return owner_->TryPop();
        }

    private:
        Queue* owner_;
    };

    template<class Storage>
    AsyncQueue(Storage) -> AsyncQueue<Storage, QueueAdapter<Storage>>;

} // namespace Mashiro::Experimental::Concurrency
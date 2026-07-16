/**
 * @file AsyncQueue.h
 * @brief P2300-native asynchronous facade over specialized bounded queue storage.
 * @ingroup Concurrency
 */
#pragma once

#include "Mashiro/Concurrency/Backpressure.h"
#include "Mashiro/Core/FalseSharing.h"
#include "Mashiro/Concurrency/IntrusiveWaitQueue.h"
#include "Mashiro/Concurrency/QueueAdapter.h"
#include "Mashiro/Concurrency/QueueError.h"

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

namespace Mashiro::Concurrency {

    template<class Storage, class Adapter = QueueAdapter<Storage>>
        requires AsyncQueueAdapterFor<Adapter, Storage>
    class AsyncQueue;

    template<class Queue, BackpressurePolicy Policy = Backpressure::Suspend>
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

        struct OperationBase;
        struct QueueAccess;
        inline thread_local OperationBase* activeStopCallback = nullptr;

        template<class Queue, class Receiver, BackpressurePolicy Policy>
        struct PushOp;
        template<class Queue, class Receiver>
        struct PopOp;
        template<class Queue, class Receiver, BackpressurePolicy Policy>
        struct PushBatchOp;
        template<class Queue, class Receiver>
        struct PopBatchOp;

        struct OperationBase : WaitNode {
            using TryFn = bool (*)(OperationBase&) noexcept;
            using CompleteFn = void (*)(OperationBase&) noexcept;
            using ErrorFn = void (*)(OperationBase&, QueueError) noexcept;

            OperationKind kind;
            bool maySuspend;
            TryFn tryTransfer;
            TryFn resolveBackpressure;
            CompleteFn completeValue;
            CompleteFn completeStopped;
            ErrorFn completeError;
            OperationBase* completeNext{nullptr};
            QueueError completionError{};
            QueueError readyError{};
            CompletionKind completionKind{CompletionKind::Value};
            std::atomic<OperationPhase> phase{OperationPhase::Constructed};

            OperationBase(OperationKind opKind, bool canSuspend, TryFn transfer, TryFn blocked, CompleteFn value,
                          CompleteFn stopped, ErrorFn error) noexcept
                : kind(opKind),
                  maySuspend(canSuspend),
                  tryTransfer(transfer),
                  resolveBackpressure(blocked),
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

        template<BackpressurePolicy Policy, class Attempt>
        [[nodiscard]] bool ResolvePush(Policy& policy, PushOutcome& outcome, QueueError& error,
                                       Attempt&& attempt) noexcept {
            for (;;) {
                const QueueTransferStatus status = attempt();
                if (status == QueueTransferStatus::Committed) {
                    policy.OnCommitted();
                    outcome = PushOutcome{PushDisposition::Enqueued};
                    return true;
                }
                if (status == QueueTransferStatus::BatchTooLarge) {
                    error = kQueueBatchTooLarge;
                    return false;
                }

                switch (policy.OnBackpressure()) {
                    case BackpressureAction::Suspend:
                        if constexpr (!Policy::maySuspend) {
                            std::terminate();
                        }
                        return false;
                    case BackpressureAction::Retry:
                        policy.BeforeRetry();
                        continue;
                    case BackpressureAction::Reject:
                        outcome = PushOutcome{PushDisposition::Rejected};
                        return true;
                    case BackpressureAction::Drop:
                        outcome = PushOutcome{PushDisposition::Dropped};
                        return true;
                }
                std::terminate();
            }
        }

    } // namespace Detail
    /**
     * @brief P2300 sender facade sharing lifecycle and waiter control over specialized bounded storage.
     *
     * @tparam Storage Specialized SPSC, MPSC, or MPMC data-plane storage.
     * @tparam Adapter Compile-time capability description and storage bridge.
     *
     * @details The queue owns no operation state. It must outlive all producer and consumer ports, senders, connected
     * operation states, and receiver completion execution. Single-cardinality ports are affine capabilities issued at
     * most once for the queue lifetime; moving a port transfers that capability. Concurrent operations through one
     * single-cardinality port violate the underlying storage contract.
     */
    template<class Storage, class Adapter>
        requires AsyncQueueAdapterFor<Adapter, Storage>
    class AsyncQueue {
    public:
        using storage_type = Storage;
        using adapter_type = Adapter;
        using value_type = typename Adapter::value_type;
        using batch_type = typename Adapter::batch_type;

        static constexpr QueueCardinality producerCardinality = Adapter::producerCardinality;
        static constexpr QueueCardinality consumerCardinality = Adapter::consumerCardinality;
        static constexpr std::size_t capacity = Adapter::capacity;

        AsyncQueue()
            requires std::default_initializable<storage_type>
        = default;

        template<class... Args>
            requires std::constructible_from<storage_type, Args&&...>
        explicit AsyncQueue(std::in_place_type_t<storage_type>,
                            Args&&... args) noexcept(std::is_nothrow_constructible_v<storage_type, Args&&...>)
            : storage_(std::forward<Args>(args)...) {}

        AsyncQueue(const AsyncQueue&) = delete;
        AsyncQueue& operator=(const AsyncQueue&) = delete;
        AsyncQueue(AsyncQueue&&) = delete;
        AsyncQueue& operator=(AsyncQueue&&) = delete;

        /** @brief Teardown is abortive: no operation state may remain parked on a destroyed facade. */
        ~AsyncQueue() { Abort(kQueueAborted); }

        [[nodiscard]] std::optional<ProducerPort<AsyncQueue>> TryProducer() noexcept {
            if constexpr (producerCardinality == QueueCardinality::Single) {
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
            if constexpr (consumerCardinality == QueueCardinality::Single) {
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
                if (lifecycle_.load(std::memory_order_relaxed) != Detail::QueueLifecycle::Open) {
                    return;
                }
                lifecycle_.store(Detail::QueueLifecycle::Closing, std::memory_order_release);
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
                if (lifecycle_.load(std::memory_order_relaxed) == Detail::QueueLifecycle::Aborted) {
                    return;
                }
                lifecycle_.store(Detail::QueueLifecycle::Aborted, std::memory_order_release);
                abortError_ = error ? error : kQueueAborted;
                DrainAllLocked(pendingProducers_, completions, abortError_);
                DrainAllLocked(pendingConsumers_, completions, abortError_);
            }
            completions.CompleteAll();
        }

        /** @brief Try once; backpressure or a non-open lifecycle leaves @p value unchanged for retry or recovery. */
        [[nodiscard]] bool TryPush(value_type& value) noexcept {
            bool shouldService = false;
            if (!EnterProducer(shouldService)) {
                if (shouldService) {
                    ServiceWaiters();
                }
                return false;
            }
            const bool committed =
                waitingProducers_.load(std::memory_order_acquire) == 0 && Adapter::TryPush(storage_, value);
            shouldService = LeaveProducer() || (committed && waitingConsumers_.load(std::memory_order_acquire) != 0);
            if (shouldService) {
                ServiceWaiters();
            }
            return committed;
        }

        [[nodiscard]] std::optional<value_type> TryPop() noexcept {
            if (lifecycle_.load(std::memory_order_acquire) == Detail::QueueLifecycle::Aborted ||
                waitingConsumers_.load(std::memory_order_acquire) != 0) {
                return std::nullopt;
            }
            std::optional<value_type> value = Adapter::TryPop(storage_);
            if (value.has_value() && waitingProducers_.load(std::memory_order_acquire) != 0) {
                ServiceWaiters();
            } else if (!value.has_value() &&
                       lifecycle_.load(std::memory_order_acquire) == Detail::QueueLifecycle::Closing) {
                ServiceWaiters();
            }
            return value;
        }

        /** @brief Return a transient occupancy hint; concurrent operations may invalidate it immediately. */
        [[nodiscard]] bool EmptyApprox() const noexcept { return Adapter::SizeApprox(storage_) == 0; }

        /** @brief Return a transient approximate occupancy count. */
        [[nodiscard]] std::size_t SizeApprox() const noexcept { return Adapter::SizeApprox(storage_); }

    private:
        template<class, BackpressurePolicy>
        friend class ProducerPort;
        template<class>
        friend class ConsumerPort;
        template<class, class, BackpressurePolicy>
        friend struct Detail::PushOp;
        template<class, class>
        friend struct Detail::PopOp;
        template<class, class, BackpressurePolicy>
        friend struct Detail::PushBatchOp;
        template<class, class>
        friend struct Detail::PopBatchOp;
        friend struct Detail::QueueAccess;

        [[nodiscard]] bool StartOperation(Detail::OperationBase& op) noexcept {
            if (op.phase.load(std::memory_order_acquire) != Detail::OperationPhase::Starting) {
                return false;
            }

            Detail::CompletionList completions;
            const bool producer = Detail::IsProducer(op.kind);
            const auto lifecycle = lifecycle_.load(std::memory_order_acquire);
            bool enteredProducer = false;
            bool shouldService = false;
            if (producer && lifecycle == Detail::QueueLifecycle::Open) {
                enteredProducer = EnterProducer(shouldService);
            }
            const bool mayTry = producer ? enteredProducer : lifecycle != Detail::QueueLifecycle::Aborted;
            const bool sameSideWaiting = WaitingFor(op.kind).load(std::memory_order_acquire) != 0;
            const bool completedWithoutParking =
                mayTry && ((!sameSideWaiting && op.tryTransfer(op)) ||
                           (sameSideWaiting && !op.maySuspend && op.resolveBackpressure(op)));
            if (completedWithoutParking) {
                if (enteredProducer) {
                    shouldService = LeaveProducer() || shouldService;
                }
                MarkCompleting(op);
                completions.PushValue(op);
                if (shouldService || OppositeWaitingFor(op.kind).load(std::memory_order_acquire) != 0) {
                    std::scoped_lock lock{mutex_};
                    ProgressLocked(completions);
                    StopConsumersIfClosedAndEmptyLocked(completions);
                }
                completions.CompleteAll();
                return true;
            }
            if (enteredProducer) {
                shouldService = LeaveProducer() || shouldService;
            }

            bool started = true;
            {
                std::scoped_lock lock{mutex_};
                if (op.phase.load(std::memory_order_acquire) != Detail::OperationPhase::Starting) {
                    started = false;
                } else if (lifecycle_.load(std::memory_order_relaxed) == Detail::QueueLifecycle::Aborted) {
                    MarkCompleting(op);
                    completions.PushError(op, abortError_);
                } else if (IsClosedProducerLocked(op.kind)) {
                    MarkCompleting(op);
                    completions.PushError(op, kQueueClosed);
                } else if (IsClosedEmptyConsumerLocked(op.kind)) {
                    MarkCompleting(op);
                    completions.PushStopped(op);
                } else if (op.readyError) {
                    MarkCompleting(op);
                    completions.PushError(op, op.readyError);
                } else if (!op.maySuspend) {
                    const bool resolved =
                        HasOlderSameSideWaiterLocked(op.kind) ? op.resolveBackpressure(op) : op.tryTransfer(op);
                    if (!resolved && !op.readyError) [[unlikely]] {
                        std::terminate();
                    }
                    MarkCompleting(op);
                    if (op.readyError) {
                        completions.PushError(op, op.readyError);
                    } else {
                        completions.PushValue(op);
                    }
                } else {
                    ReserveWaiterLocked(op.kind);
                    if (!HasOlderSameSideWaiterLocked(op.kind) && op.tryTransfer(op)) {
                        ReleaseWaiterLocked(op.kind);
                        MarkCompleting(op);
                        completions.PushValue(op);
                    } else if (op.readyError) {
                        ReleaseWaiterLocked(op.kind);
                        MarkCompleting(op);
                        completions.PushError(op, op.readyError);
                    } else {
                        EnqueueReservedLocked(op);
                    }
                }
                ProgressLocked(completions);
                StopConsumersIfClosedAndEmptyLocked(completions);
            }
            completions.CompleteAll();
            return started;
        }

        void CancelOperation(Detail::OperationBase& op) noexcept {
            Detail::CompletionList completions;
            {
                std::scoped_lock lock{mutex_};
                const auto phase = op.phase.load(std::memory_order_acquire);
                if (phase == Detail::OperationPhase::Starting) {
                    op.phase.store(Detail::OperationPhase::StopRequested, std::memory_order_release);
                } else if (phase == Detail::OperationPhase::Pending) {
                    RemoveWaiterLocked(op);
                    MarkCompleting(op);
                    completions.PushStopped(op);
                }
            }
            completions.CompleteAll();
        }

        [[nodiscard]] bool TryPushStorage(value_type& value) noexcept { return Adapter::TryPush(storage_, value); }
        [[nodiscard]] std::optional<value_type> TryPopStorage() noexcept { return Adapter::TryPop(storage_); }
        [[nodiscard]] QueueTransferStatus TryPushBatchStorage(batch_type& batch) noexcept
            requires TransactionalBatchQueueAdapter<Adapter>
        {
            return Adapter::TryPushBatch(storage_, batch);
        }
        [[nodiscard]] bool TryPopBatchStorage(batch_type& batch) noexcept {
            return Adapter::TryPopBatch(storage_, batch);
        }

        [[nodiscard]] bool HasOlderSameSideWaiterLocked(Detail::OperationKind kind) const noexcept {
            return Detail::IsProducer(kind) ? !pendingProducers_.Empty() : !pendingConsumers_.Empty();
        }

        [[nodiscard]] bool IsClosedProducerLocked(Detail::OperationKind kind) const noexcept {
            return Detail::IsProducer(kind) &&
                   lifecycle_.load(std::memory_order_relaxed) == Detail::QueueLifecycle::Closing;
        }

        [[nodiscard]] bool IsClosedEmptyConsumerLocked(Detail::OperationKind kind) noexcept {
            return !Detail::IsProducer(kind) &&
                   lifecycle_.load(std::memory_order_relaxed) == Detail::QueueLifecycle::Closing &&
                   activeProducers_.load(std::memory_order_acquire) == 0 && Adapter::IsDrained(storage_);
        }

        void MarkCompleting(Detail::OperationBase& op) noexcept {
            op.phase.store(Detail::OperationPhase::Completing, std::memory_order_release);
        }

        void EnqueueReservedLocked(Detail::OperationBase& op) noexcept {
            op.phase.store(Detail::OperationPhase::Pending, std::memory_order_release);
            QueueFor(op.kind).PushBack(op);
        }

        [[nodiscard]] std::atomic_uint32_t& WaitingFor(Detail::OperationKind kind) noexcept {
            return Detail::IsProducer(kind) ? waitingProducers_ : waitingConsumers_;
        }

        [[nodiscard]] std::atomic_uint32_t& OppositeWaitingFor(Detail::OperationKind kind) noexcept {
            return Detail::IsProducer(kind) ? waitingConsumers_ : waitingProducers_;
        }

        void ReserveWaiterLocked(Detail::OperationKind kind) noexcept {
            WaitingFor(kind).fetch_add(1, std::memory_order_release);
        }

        void ReleaseWaiterLocked(Detail::OperationKind kind) noexcept {
            WaitingFor(kind).fetch_sub(1, std::memory_order_release);
        }

        void RemoveWaiterLocked(Detail::OperationBase& op) noexcept {
            QueueFor(op.kind).Erase(op);
            ReleaseWaiterLocked(op.kind);
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
            if (lifecycle_.load(std::memory_order_relaxed) == Detail::QueueLifecycle::Aborted) {
                RemoveWaiterLocked(op);
                MarkCompleting(op);
                completions.PushError(op, abortError_);
                return true;
            }
            if (IsClosedProducerLocked(op.kind)) {
                RemoveWaiterLocked(op);
                MarkCompleting(op);
                completions.PushError(op, kQueueClosed);
                return true;
            }
            if (IsClosedEmptyConsumerLocked(op.kind)) {
                RemoveWaiterLocked(op);
                MarkCompleting(op);
                completions.PushStopped(op);
                return true;
            }
            if (!op.tryTransfer(op)) {
                if (op.readyError) {
                    RemoveWaiterLocked(op);
                    MarkCompleting(op);
                    completions.PushError(op, op.readyError);
                    return true;
                }
                return false;
            }
            RemoveWaiterLocked(op);
            MarkCompleting(op);
            completions.PushValue(op);
            return true;
        }

        void FailPendingProducersLocked(Detail::CompletionList& completions, QueueError error) noexcept {
            DrainAllLocked(pendingProducers_, completions, error);
        }

        void StopConsumersIfClosedAndEmptyLocked(Detail::CompletionList& completions) noexcept {
            if (lifecycle_.load(std::memory_order_relaxed) != Detail::QueueLifecycle::Closing ||
                activeProducers_.load(std::memory_order_acquire) != 0 || !Adapter::IsDrained(storage_)) {
                return;
            }
            while (!pendingConsumers_.Empty()) {
                auto& op = *static_cast<Detail::OperationBase*>(pendingConsumers_.Front());
                RemoveWaiterLocked(op);
                MarkCompleting(op);
                completions.PushStopped(op);
            }
        }

        void DrainAllLocked(Detail::IntrusiveWaitQueue& queue, Detail::CompletionList& completions,
                            QueueError error) noexcept {
            while (!queue.Empty()) {
                auto& op = *static_cast<Detail::OperationBase*>(queue.Front());
                RemoveWaiterLocked(op);
                MarkCompleting(op);
                completions.PushError(op, error);
            }
        }

        [[nodiscard]] bool EnterProducer(bool& shouldService) noexcept {
            activeProducers_.fetch_add(1, std::memory_order_acq_rel);
            if (lifecycle_.load(std::memory_order_acquire) == Detail::QueueLifecycle::Open) {
                return true;
            }
            shouldService = LeaveProducer();
            return false;
        }

        [[nodiscard]] bool LeaveProducer() noexcept {
            const bool last = activeProducers_.fetch_sub(1, std::memory_order_acq_rel) == 1;
            return last && lifecycle_.load(std::memory_order_acquire) == Detail::QueueLifecycle::Closing;
        }

        void ServiceWaiters() noexcept {
            Detail::CompletionList completions;
            {
                std::scoped_lock lock{mutex_};
                ProgressLocked(completions);
                StopConsumersIfClosedAndEmptyLocked(completions);
            }
            completions.CompleteAll();
        }

        [[= Detail::QueueControlDomain{}]] alignas(Platform::kCacheLineSize) std::mutex mutex_{};
        [[= Detail::QueueStorageDomain{}]] alignas(Platform::kCacheLineSize) storage_type storage_{};
        [[= Detail::QueueLifecycleDomain{}]] alignas(
            Platform::kCacheLineSize) std::atomic<Detail::QueueLifecycle> lifecycle_{Detail::QueueLifecycle::Open};
        [[= Detail::QueueLifecycleDomain{}]] QueueError abortError_{kQueueAborted};
        [[= Detail::QueueControlDomain{}]] Detail::IntrusiveWaitQueue pendingProducers_{};
        [[= Detail::QueueControlDomain{}]] Detail::IntrusiveWaitQueue pendingConsumers_{};
        [[= Detail::QueueControlDomain{}]] std::atomic_uint32_t waitingProducers_{0};
        [[= Detail::QueueControlDomain{}]] std::atomic_uint32_t waitingConsumers_{0};
        [[= Detail::QueueLifecycleDomain{}]] std::atomic_uint32_t activeProducers_{0};
        [[= Detail::QueueLifecycleDomain{}]] std::atomic_bool producerIssued_{false};
        [[= Detail::QueueLifecycleDomain{}]] std::atomic_bool consumerIssued_{false};
    };
    namespace Detail {

        struct QueueAccess {
            template<class Queue>
            [[nodiscard]] static bool Start(Queue& queue, OperationBase& operation) noexcept {
                return queue.StartOperation(operation);
            }
        };

        template<class Op>
        void CompleteStopRequestedAfterRegistration(Op& op) noexcept {
            auto expected = OperationPhase::StopRequested;
            if (!op.phase.compare_exchange_strong(expected, OperationPhase::Completed, std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
                return;
            }
            op.stopCallback.reset();
            stdexec::set_stopped(std::move(op.receiver));
        }

        template<class Op>
        void StartWithStopToken(Op& op) noexcept {
            auto expected = OperationPhase::Constructed;
            if (!op.phase.compare_exchange_strong(expected, OperationPhase::Starting, std::memory_order_acq_rel,
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
            if (!QueueAccess::Start(*op.owner, op)) {
                CompleteStopRequestedAfterRegistration(op);
            }
        }

        template<class Queue, class Receiver, BackpressurePolicy Policy>
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
            [[msvc::no_unique_address]] Policy policy;
            PushOutcome outcome{};
            std::optional<StopCallback> stopCallback;

            PushOp(Queue* q, Receiver&& r, value_type&& v,
                   Policy p) noexcept(std::is_nothrow_move_constructible_v<Receiver> &&
                                      std::is_nothrow_move_constructible_v<value_type> &&
                                      std::is_nothrow_move_constructible_v<Policy>)
                : OperationBase(OperationKind::Push, Policy::maySuspend, &PushOp::TryImmediate,
                                &PushOp::TryBackpressured, &PushOp::CompleteValue, &PushOp::CompleteStopped,
                                &PushOp::CompleteError),
                  owner(q),
                  receiver(std::move(r)),
                  value(std::move(v)),
                  policy(std::move(p)) {}

            void start() & noexcept { StartWithStopToken(*this); }

            [[nodiscard]] static bool TryImmediate(OperationBase& base) noexcept {
                auto& self = static_cast<PushOp&>(base);
                return ResolvePush(self.policy, self.outcome, self.readyError, [&self] noexcept {
                    return self.owner->TryPushStorage(self.value) ? QueueTransferStatus::Committed
                                                                  : QueueTransferStatus::BackPressure;
                });
            }

            [[nodiscard]] static bool TryBackpressured(OperationBase& base) noexcept {
                auto& self = static_cast<PushOp&>(base);
                return ResolvePush(self.policy, self.outcome, self.readyError,
                                   [] noexcept { return QueueTransferStatus::BackPressure; });
            }

            static void CompleteValue(OperationBase& base) noexcept {
                auto& self = static_cast<PushOp&>(base);
                ReleaseStopCallback(self);
                stdexec::set_value(std::move(self.receiver), self.outcome);
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
                : OperationBase(OperationKind::Pop, true, &PopOp::TryImmediate, nullptr, &PopOp::CompleteValue,
                                &PopOp::CompleteStopped, &PopOp::CompleteError),
                  owner(q),
                  receiver(std::move(r)) {}

            void start() & noexcept { StartWithStopToken(*this); }

            [[nodiscard]] static bool TryImmediate(OperationBase& base) noexcept {
                auto& self = static_cast<PopOp&>(base);
                auto value = self.owner->TryPopStorage();
                if (!value.has_value()) {
                    return false;
                }
                self.value.emplace(std::move(*value));
                return true;
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

        template<class Queue, class Receiver, BackpressurePolicy Policy>
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
            [[msvc::no_unique_address]] Policy policy;
            PushOutcome outcome{};
            std::optional<StopCallback> stopCallback;

            PushBatchOp(Queue* q, Receiver&& r, batch_type&& b,
                        Policy p) noexcept(std::is_nothrow_move_constructible_v<Receiver> &&
                                           std::is_nothrow_move_constructible_v<batch_type> &&
                                           std::is_nothrow_move_constructible_v<Policy>)
                : OperationBase(OperationKind::PushBatch, Policy::maySuspend, &PushBatchOp::TryImmediate,
                                &PushBatchOp::TryBackpressured, &PushBatchOp::CompleteValue,
                                &PushBatchOp::CompleteStopped, &PushBatchOp::CompleteError),
                  owner(q),
                  receiver(std::move(r)),
                  batch(std::move(b)),
                  policy(std::move(p)) {}

            void start() & noexcept { StartWithStopToken(*this); }

            [[nodiscard]] static bool TryImmediate(OperationBase& base) noexcept {
                auto& self = static_cast<PushBatchOp&>(base);
                return ResolvePush(self.policy, self.outcome, self.readyError,
                                   [&self] noexcept { return self.owner->TryPushBatchStorage(self.batch); });
            }

            [[nodiscard]] static bool TryBackpressured(OperationBase& base) noexcept {
                auto& self = static_cast<PushBatchOp&>(base);
                return ResolvePush(self.policy, self.outcome, self.readyError,
                                   [] noexcept { return QueueTransferStatus::BackPressure; });
            }

            static void CompleteValue(OperationBase& base) noexcept {
                auto& self = static_cast<PushBatchOp&>(base);
                ReleaseStopCallback(self);
                stdexec::set_value(std::move(self.receiver), self.outcome);
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
                : OperationBase(OperationKind::PopBatch, true, &PopBatchOp::TryImmediate, nullptr,
                                &PopBatchOp::CompleteValue, &PopBatchOp::CompleteStopped, &PopBatchOp::CompleteError),
                  owner(q),
                  receiver(std::move(r)) {}

            void start() & noexcept { StartWithStopToken(*this); }

            [[nodiscard]] static bool TryImmediate(OperationBase& base) noexcept {
                auto& self = static_cast<PopBatchOp&>(base);
                return self.owner->TryPopBatchStorage(self.batch);
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
            using completion_signatures =
                stdexec::completion_signatures<stdexec::set_value_t(value_type), stdexec::set_stopped_t(),
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
            using completion_signatures =
                stdexec::completion_signatures<stdexec::set_value_t(batch_type), stdexec::set_stopped_t(),
                                               stdexec::set_error_t(QueueError)>;

            Queue* owner;

            template<class Receiver>
            [[nodiscard]] auto connect(Receiver&& receiver) const
                noexcept(std::is_nothrow_constructible_v<std::remove_cvref_t<Receiver>, Receiver>)
                    -> PopBatchOp<Queue, std::remove_cvref_t<Receiver>> {
                return PopBatchOp<Queue, std::remove_cvref_t<Receiver>>{owner, std::forward<Receiver>(receiver)};
            }
        };

        template<class Queue, BackpressurePolicy Policy>
        struct PushSender {
            using sender_concept = stdexec::sender_t;
            using value_type = typename Queue::value_type;
            using completion_signatures =
                stdexec::completion_signatures<stdexec::set_value_t(PushOutcome), stdexec::set_stopped_t(),
                                               stdexec::set_error_t(QueueError)>;

            Queue* owner;
            value_type value;
            [[msvc::no_unique_address]] Policy policy;

            template<class Receiver>
                requires std::copy_constructible<value_type>
            [[nodiscard]] auto connect(Receiver&& receiver) const& noexcept(
                std::is_nothrow_constructible_v<std::remove_cvref_t<Receiver>, Receiver> &&
                std::is_nothrow_copy_constructible_v<value_type> && std::is_nothrow_copy_constructible_v<Policy>)
                -> PushOp<Queue, std::remove_cvref_t<Receiver>, Policy> {
                return PushOp<Queue, std::remove_cvref_t<Receiver>, Policy>{
                    owner,
                    std::forward<Receiver>(receiver),
                    value_type(value),
                    policy,
                };
            }

            template<class Receiver>
            [[nodiscard]] auto connect(Receiver&& receiver) && noexcept(
                std::is_nothrow_constructible_v<std::remove_cvref_t<Receiver>, Receiver> &&
                std::is_nothrow_move_constructible_v<value_type> && std::is_nothrow_move_constructible_v<Policy>)
                -> PushOp<Queue, std::remove_cvref_t<Receiver>, Policy> {
                return PushOp<Queue, std::remove_cvref_t<Receiver>, Policy>{
                    owner,
                    std::forward<Receiver>(receiver),
                    std::move(value),
                    std::move(policy),
                };
            }
        };

        template<class Queue, BackpressurePolicy Policy>
        struct PushBatchSender {
            using sender_concept = stdexec::sender_t;
            using batch_type = typename Queue::batch_type;
            using completion_signatures =
                stdexec::completion_signatures<stdexec::set_value_t(PushOutcome), stdexec::set_stopped_t(),
                                               stdexec::set_error_t(QueueError)>;

            Queue* owner;
            batch_type batch;
            [[msvc::no_unique_address]] Policy policy;

            template<class Receiver>
                requires std::copy_constructible<batch_type>
            [[nodiscard]] auto connect(Receiver&& receiver) const& noexcept(
                std::is_nothrow_constructible_v<std::remove_cvref_t<Receiver>, Receiver> &&
                std::is_nothrow_copy_constructible_v<batch_type> && std::is_nothrow_copy_constructible_v<Policy>)
                -> PushBatchOp<Queue, std::remove_cvref_t<Receiver>, Policy> {
                return PushBatchOp<Queue, std::remove_cvref_t<Receiver>, Policy>{
                    owner,
                    std::forward<Receiver>(receiver),
                    batch_type(batch),
                    policy,
                };
            }

            template<class Receiver>
            [[nodiscard]] auto connect(Receiver&& receiver) && noexcept(
                std::is_nothrow_constructible_v<std::remove_cvref_t<Receiver>, Receiver> &&
                std::is_nothrow_move_constructible_v<batch_type> && std::is_nothrow_move_constructible_v<Policy>)
                -> PushBatchOp<Queue, std::remove_cvref_t<Receiver>, Policy> {
                return PushBatchOp<Queue, std::remove_cvref_t<Receiver>, Policy>{
                    owner,
                    std::forward<Receiver>(receiver),
                    std::move(batch),
                    std::move(policy),
                };
            }
        };

    } // namespace Detail

    /** @brief Producer capability carrying one compile-time backpressure policy prototype. */
    template<class Queue, BackpressurePolicy Policy>
    class ProducerPort {
    public:
        explicit ProducerPort(Queue* owner) noexcept
            requires std::default_initializable<Policy>
            : owner_(owner) {}

        ProducerPort(Queue* owner, Policy policy) noexcept : owner_(owner), policy_(std::move(policy)) {}

        ProducerPort(const ProducerPort&)
            requires(Queue::producerCardinality == QueueCardinality::Multiple)
        = default;
        ProducerPort(const ProducerPort&)
            requires(Queue::producerCardinality == QueueCardinality::Single)
        = delete;

        ProducerPort& operator=(const ProducerPort&)
            requires(Queue::producerCardinality == QueueCardinality::Multiple)
        = default;
        ProducerPort& operator=(const ProducerPort&)
            requires(Queue::producerCardinality == QueueCardinality::Single)
        = delete;

        ProducerPort(ProducerPort&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), policy_(std::move(other.policy_)) {}

        ProducerPort& operator=(ProducerPort&& other) noexcept {
            if (this != &other) {
                owner_ = std::exchange(other.owner_, nullptr);
                policy_ = std::move(other.policy_);
            }
            return *this;
        }

        /** @brief Consume this capability and attach @p policy to subsequently created producer senders. */
        template<BackpressurePolicy NewPolicy>
        [[nodiscard]] auto WithBackpressure(NewPolicy policy) && noexcept
            -> ProducerPort<Queue, std::remove_cvref_t<NewPolicy>> {
            return ProducerPort<Queue, std::remove_cvref_t<NewPolicy>>{
                std::exchange(owner_, nullptr),
                std::move(policy),
            };
        }

        /** @brief Copy a multi-producer capability and attach @p policy to the copy. */
        template<BackpressurePolicy NewPolicy>
            requires(Queue::producerCardinality == QueueCardinality::Multiple)
        [[nodiscard]] auto WithBackpressure(NewPolicy policy) const& noexcept
            -> ProducerPort<Queue, std::remove_cvref_t<NewPolicy>> {
            return ProducerPort<Queue, std::remove_cvref_t<NewPolicy>>{owner_, std::move(policy)};
        }

        template<class Value>
            requires std::constructible_from<typename Queue::value_type, Value&&>
        [[nodiscard]] auto
        Push(Value&& value) noexcept(std::is_nothrow_constructible_v<typename Queue::value_type, Value&&> &&
                                     std::is_nothrow_copy_constructible_v<Policy>) {
            return Detail::PushSender<Queue, Policy>{
                .owner = owner_,
                .value = typename Queue::value_type(std::forward<Value>(value)),
                .policy = policy_,
            };
        }

        template<class Batch>
            requires TransactionalBatchQueueAdapter<typename Queue::adapter_type> &&
                     std::constructible_from<typename Queue::batch_type, Batch&&>
        [[nodiscard]] auto
        PushBatch(Batch&& batch) noexcept(std::is_nothrow_constructible_v<typename Queue::batch_type, Batch&&> &&
                                          std::is_nothrow_copy_constructible_v<Policy>) {
            return Detail::PushBatchSender<Queue, Policy>{
                .owner = owner_,
                .batch = typename Queue::batch_type(std::forward<Batch>(batch)),
                .policy = policy_,
            };
        }

        /** @brief Try once; backpressure or a non-open lifecycle leaves @p value unchanged for retry or recovery. */
        [[nodiscard]] bool TryPush(typename Queue::value_type& value) noexcept { return owner_->TryPush(value); }

    private:
        template<class, BackpressurePolicy>
        friend class ProducerPort;

        Queue* owner_;
        [[msvc::no_unique_address]] Policy policy_;
    };

    /** @brief Consumer endpoint for an @ref AsyncQueue. */
    template<class Queue>
    class ConsumerPort {
    public:
        explicit ConsumerPort(Queue* owner) noexcept : owner_(owner) {}

        ConsumerPort(const ConsumerPort&)
            requires(Queue::consumerCardinality == QueueCardinality::Multiple)
        = default;
        ConsumerPort(const ConsumerPort&)
            requires(Queue::consumerCardinality == QueueCardinality::Single)
        = delete;

        ConsumerPort& operator=(const ConsumerPort&)
            requires(Queue::consumerCardinality == QueueCardinality::Multiple)
        = default;
        ConsumerPort& operator=(const ConsumerPort&)
            requires(Queue::consumerCardinality == QueueCardinality::Single)
        = delete;
        ConsumerPort(ConsumerPort&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}

        ConsumerPort& operator=(ConsumerPort&& other) noexcept {
            if (this != &other) {
                owner_ = std::exchange(other.owner_, nullptr);
            }
            return *this;
        }

        [[nodiscard]] auto Pop() noexcept { return Detail::PopSender<Queue>{.owner = owner_}; }

        [[nodiscard]] auto PopBatch() noexcept { return Detail::PopBatchSender<Queue>{.owner = owner_}; }

        [[nodiscard]] std::optional<typename Queue::value_type> TryPop() noexcept { return owner_->TryPop(); }

    private:
        Queue* owner_;
    };

    template<class Storage, class... Args>
    AsyncQueue(std::in_place_type_t<Storage>, Args&&...) -> AsyncQueue<Storage, QueueAdapter<Storage>>;

} // namespace Mashiro::Concurrency

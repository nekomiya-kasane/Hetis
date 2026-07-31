/**
 * @file ExecutableBatch.h
 * @brief Move-only physical command ownership and completed-submission leases.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Backend/Queue.h>

#include <cstdint>
#include <expected>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace Sora::Compute {

    /** @brief Owned inputs from which one immutable physical command batch is validated. */
    struct ExecutableBatchStorage {
        Queue queue{};
        std::vector<BackendCommand> commands{};
        std::vector<BackendBinding> bindings{};
        std::vector<std::byte> scalarData{};
        std::vector<BackendScalarBinding> scalarBindings{};
        std::vector<Artifact> artifacts{};
        std::vector<Allocation> allocations{};
        std::vector<Event> waits{};
        bool signalOnly = false;
    };

    /** @brief Accepted asynchronous work retaining every backend resource until completion. */
    class Submission {
    public:
        Submission(const Submission&) = delete;
        Submission& operator=(const Submission&) = delete;
        Submission(Submission&&) noexcept = default;
        Submission& operator=(Submission&& other) noexcept {
            if (this != &other) {
                Drain();
                queue_ = std::move(other.queue_);
                artifacts_ = std::move(other.artifacts_);
                allocations_ = std::move(other.allocations_);
                waits_ = std::move(other.waits_);
                completion_ = std::move(other.completion_);
            }
            return *this;
        }

        /** @brief Drain accepted work before releasing any backend resource lease. */
        ~Submission() noexcept { Drain(); }

        /** @brief Observe the completion event without blocking. */
        [[nodiscard]] EventObservation Query() const noexcept { return completion_.Query(); }

        /** @brief Wait without a deadline until the completion event reaches a terminal state. */
        [[nodiscard]] EventObservation WaitUntilTerminal() const noexcept { return completion_.WaitUntilTerminal(); }

        /** @brief Wait for completion or timeout while retaining every resource lease. */
        [[nodiscard]] EventObservation Wait(uint64_t timeoutNanoseconds) const noexcept {
            return completion_.Wait(timeoutNanoseconds);
        }

        /** @brief Return a shared handle to the completion event. */
        [[nodiscard]] Event Completion() const noexcept { return completion_; }

    private:
        friend class ExecutableBatch;

        Submission(Queue queue, std::vector<Artifact> artifacts, std::vector<Allocation> allocations,
                   std::vector<Event> waits, Event completion) noexcept
            : queue_(std::move(queue)),
              artifacts_(std::move(artifacts)),
              allocations_(std::move(allocations)),
              waits_(std::move(waits)),
              completion_(std::move(completion)) {}

        void Drain() const noexcept {
            if (!completion_) {
                return;
            }
            if (completion_.Query().status == EventStatus::Pending) {
                (void)completion_.WaitUntilTerminal();
            }
        }

        Queue queue_{};
        std::vector<Artifact> artifacts_{};
        std::vector<Allocation> allocations_{};
        std::vector<Event> waits_{};
        Event completion_{};
    };

    /** @brief One validated move-only physical batch ready for exactly one submission. */
    class ExecutableBatch {
    public:
        ExecutableBatch(const ExecutableBatch&) = delete;
        ExecutableBatch& operator=(const ExecutableBatch&) = delete;
        ExecutableBatch(ExecutableBatch&&) noexcept = default;
        ExecutableBatch& operator=(ExecutableBatch&&) noexcept = default;

        /** @brief Validate ownership, ABI command semantics, and wait-event compatibility. */
        [[nodiscard]] static std::expected<ExecutableBatch, SubmissionError>
        Create(ExecutableBatchStorage storage) noexcept {
            const bool invalidSignal =
                storage.signalOnly &&
                (!storage.commands.empty() || !storage.bindings.empty() || !storage.scalarData.empty() ||
                 !storage.scalarBindings.empty() || !storage.artifacts.empty());
            if (!storage.queue || (!storage.signalOnly && storage.commands.empty()) || invalidSignal ||
                storage.commands.size() > UINT32_MAX || storage.bindings.size() > UINT32_MAX ||
                storage.scalarBindings.size() > UINT32_MAX || storage.waits.size() > UINT32_MAX) {
                return std::unexpected(SubmissionError{SubmissionErrorCode::InvalidRequest, 0});
            }

            try {
                std::vector<EventView> waitViews;
                waitViews.reserve(storage.waits.size());
                for (const Event& wait : storage.waits) {
                    if (!wait) {
                        return std::unexpected(SubmissionError{SubmissionErrorCode::InvalidRequest, 0});
                    }
                    waitViews.push_back(wait.View());
                }

                ExecutableBatch batch{std::move(storage), std::move(waitViews)};
                const QueueView queue = batch.queue_.View();
                if (!batch.OwnsEveryReferencedResource() ||
                    !Detail::ValidSubmitRequest(batch.Request(), queue.backend, queue.device, queue.service,
                                                queue.capabilities, queue.dispatchArtifactKind)) {
                    return std::unexpected(SubmissionError{SubmissionErrorCode::InvalidRequest, 0});
                }
                return std::move(batch);
            } catch (const std::bad_alloc&) {
                return std::unexpected(SubmissionError{SubmissionErrorCode::OutOfMemory, 0});
            }
        }

        /** @brief Return the immutable queue identity used for physical dependency validation. */
        [[nodiscard]] QueueView QueueIdentity() const noexcept { return queue_.View(); }

        /** @brief Return the validated request while this batch retains all referenced storage. */
        [[nodiscard]] BackendSubmitRequest Request() const noexcept {
            return {.commands = commands_.data(),
                    .commandCount = static_cast<uint32_t>(commands_.size()),
                    .bindings = bindings_.data(),
                    .bindingCount = static_cast<uint32_t>(bindings_.size()),
                    .scalarData = scalarData_.data(),
                    .scalarDataSize = scalarData_.size(),
                    .scalarBindings = scalarBindings_.data(),
                    .scalarBindingCount = static_cast<uint32_t>(scalarBindings_.size()),
                    .waits = waitViews_.data(),
                    .waitCount = static_cast<uint32_t>(waitViews_.size())};
        }

        /**
         * @brief Inject runtime predecessor events, submit exactly once, and transfer every lease.
         * @details Dynamic waits exist only after predecessor graph segments have been accepted by their queues.
         */
        [[nodiscard]] std::expected<Submission, SubmissionError>
        Submit(std::vector<Event> dynamicWaits = {}) && noexcept {
            try {
                if (dynamicWaits.size() > UINT32_MAX - waits_.size()) {
                    return std::unexpected(SubmissionError{SubmissionErrorCode::InvalidRequest, 0});
                }
                waits_.reserve(waits_.size() + dynamicWaits.size());
                waitViews_.reserve(waitViews_.size() + dynamicWaits.size());
                for (Event& wait : dynamicWaits) {
                    if (!wait) {
                        return std::unexpected(SubmissionError{SubmissionErrorCode::InvalidRequest, 0});
                    }
                    waitViews_.push_back(wait.View());
                    waits_.push_back(std::move(wait));
                }
            } catch (const std::bad_alloc&) {
                return std::unexpected(SubmissionError{SubmissionErrorCode::OutOfMemory, 0});
            }
            auto completion = queue_.Submit(Request());
            if (!completion) {
                return std::unexpected(completion.error());
            }
            return Submission{std::move(queue_), std::move(artifacts_), std::move(allocations_), std::move(waits_),
                              std::move(*completion)};
        }

    private:
        ExecutableBatch(ExecutableBatchStorage storage, std::vector<EventView> waitViews) noexcept
            : queue_(std::move(storage.queue)),
              commands_(std::move(storage.commands)),
              bindings_(std::move(storage.bindings)),
              scalarData_(std::move(storage.scalarData)),
              scalarBindings_(std::move(storage.scalarBindings)),
              artifacts_(std::move(storage.artifacts)),
              allocations_(std::move(storage.allocations)),
              waits_(std::move(storage.waits)),
              waitViews_(std::move(waitViews)) {}

        [[nodiscard]] bool OwnsArtifact(ArtifactView view) const noexcept {
            for (const Artifact& artifact : artifacts_) {
                if (artifact.View() == view) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool OwnsAllocation(AllocationView view) const noexcept {
            for (const Allocation& allocation : allocations_) {
                if (allocation.View() == view) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool OwnsEveryReferencedResource() const noexcept {
            for (const BackendBinding& binding : bindings_) {
                if (!OwnsAllocation(binding.allocation)) {
                    return false;
                }
            }
            for (const BackendCommand& command : commands_) {
                if (command.kind == BackendCommandKind::Dispatch && !OwnsArtifact(command.artifact)) {
                    return false;
                }
                if (command.source && !OwnsAllocation(command.source)) {
                    return false;
                }
                if (command.target && !OwnsAllocation(command.target)) {
                    return false;
                }
            }
            return true;
        }

        Queue queue_{};
        std::vector<BackendCommand> commands_{};
        std::vector<BackendBinding> bindings_{};
        std::vector<std::byte> scalarData_{};
        std::vector<BackendScalarBinding> scalarBindings_{};
        std::vector<Artifact> artifacts_{};
        std::vector<Allocation> allocations_{};
        std::vector<Event> waits_{};
        std::vector<EventView> waitViews_{};
    };

} // namespace Sora::Compute

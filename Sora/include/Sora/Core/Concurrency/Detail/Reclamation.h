/**
 * @file Reclamation.h
 * @brief Internal ownership and participant-record mechanics for safe reclamation.
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/Concurrency/ConcurrentBatchQueue.h>
#include <Sora/Core/InlineFunction.h>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Concurrency::Detail {

    struct RetiredObject {
        void* pointer = nullptr;
        InlineFunction<void() noexcept, 32> reclaim;
        std::uint64_t generation = 0;
    };

    inline constexpr std::uint64_t kGenerationHalfRange = std::uint64_t{1} << 63;

    [[nodiscard]] constexpr bool GenerationReached(std::uint64_t observed, std::uint64_t target) noexcept {
        return observed == target || observed - target < kGenerationHalfRange;
    }

    /** @brief Retirement-specific safety facade over a generic concurrent batch queue. */
    class RetirementQueue {
    public:
        using Queue = ConcurrentBatchQueue<RetiredObject>;
        using Batch = Queue::Batch;

        RetirementQueue() noexcept = default;
        RetirementQueue(const RetirementQueue&) = delete;
        RetirementQueue& operator=(const RetirementQueue&) = delete;
        RetirementQueue(RetirementQueue&&) = delete;
        RetirementQueue& operator=(RetirementQueue&&) = delete;

        ~RetirementQueue() { Drain(); }

        template<typename T, typename Deleter>
            requires std::is_nothrow_constructible_v<std::remove_cvref_t<Deleter>, Deleter&&> &&
                     std::is_nothrow_move_constructible_v<std::remove_cvref_t<Deleter>> &&
                     std::is_nothrow_invocable_v<std::remove_cvref_t<Deleter>&, T*>
        void Retire(T* pointer, Deleter&& deleter, std::uint64_t generation = 0) {
            if (pointer == nullptr) {
                return;
            }
            using StoredDeleter = std::remove_cvref_t<Deleter>;
            auto reclaim = [pointer, deleter = StoredDeleter(std::forward<Deleter>(deleter))]() mutable noexcept {
                deleter(pointer);
            };
            RetiredObject object{pointer, std::move(reclaim), generation};
            try {
                queue_.Emplace(std::move(object));
            } catch (...) {
                object.reclaim();
                throw;
            }
        }

        template<typename T>
        void Retire(T* pointer, std::uint64_t generation = 0) {
            Retire(pointer, std::default_delete<T>{}, generation);
        }

        template<typename Pointer, std::size_t Extent, typename Deleter>
            requires std::is_pointer_v<std::remove_cv_t<Pointer>> &&
                     std::is_convertible_v<std::remove_cv_t<Pointer>, void*> &&
                     std::is_nothrow_copy_constructible_v<Deleter> && std::is_nothrow_move_constructible_v<Deleter> &&
                     std::is_nothrow_invocable_v<Deleter&, std::remove_cv_t<Pointer>>
        void Retire(std::span<Pointer, Extent> pointers, Deleter deleter, std::uint64_t generation = 0) {
            using RawPointer = std::remove_cv_t<Pointer>;
            std::size_t next = 0;
            try {
                queue_.EmplaceBatch(pointers, [&](RawPointer pointer) noexcept -> std::optional<RetiredObject> {
                    ++next;
                    if (pointer == nullptr) {
                        return std::nullopt;
                    }
                    auto reclaim = [pointer, deleter]() mutable noexcept { deleter(pointer); };
                    return RetiredObject{pointer, std::move(reclaim), generation};
                });
            } catch (...) {
                for (; next < pointers.size(); ++next) {
                    RawPointer pointer = pointers[next];
                    if (pointer != nullptr) {
                        deleter(pointer);
                    }
                }
                throw;
            }
        }

        template<typename Pointer, std::size_t Extent>
            requires std::is_pointer_v<std::remove_cv_t<Pointer>> &&
                     std::is_convertible_v<std::remove_cv_t<Pointer>, void*>
        void Retire(std::span<Pointer, Extent> pointers, std::uint64_t generation = 0) {
            using Pointee = std::remove_pointer_t<std::remove_cv_t<Pointer>>;
            Retire(pointers, std::default_delete<Pointee>{}, generation);
        }

        [[nodiscard]] Batch DetachAll() noexcept {
            return queue_.TakeAll();
        }

        void Requeue(Batch objects) noexcept {
            queue_.Requeue(std::move(objects));
        }

        [[nodiscard]] std::size_t Size() const noexcept { return queue_.Size(); }

        void Drain() noexcept {
            for (;;) {
                Batch objects = DetachAll();
                if (objects.Empty()) {
                    return;
                }
                Destroy(std::move(objects));
            }
        }

        void Destroy(Batch objects) noexcept {
            Batch reusable;
            while (auto* entry = objects.PopFront()) {
                Batch::Value(*entry).reclaim();
                reusable.PushBack(entry);
            }
            queue_.Release(std::move(reusable));
        }

    private:
        Queue queue_;
    };

    template<typename Safe>
    [[nodiscard]] std::size_t ReclaimDetached(RetirementQueue& retired, RetirementQueue::Batch candidates,
                                               Safe&& safe) {
        static_assert(std::is_nothrow_invocable_r_v<bool, Safe&, const RetiredObject&>);
        RetirementQueue::Batch deferred;
        RetirementQueue::Batch reclaimable;
        while (auto* entry = candidates.PopFront()) {
            if (safe(RetirementQueue::Batch::Value(*entry))) {
                reclaimable.PushBack(entry);
            } else {
                deferred.PushBack(entry);
            }
        }
        retired.Requeue(std::move(deferred));
        const std::size_t count = reclaimable.Size();
        retired.Destroy(std::move(reclaimable));
        return count;
    }

    /** @brief Append-only stable record registry with reusable records and allocation-free snapshots. */
    template<typename Record>
    class StableRecordRegistry {
        struct Node {
            Record record;
            Node* next = nullptr;
        };

    public:
        [[nodiscard]] Record* Register() {
            std::scoped_lock lock(mutex_);
            for (Node* node = head_.load(std::memory_order_relaxed); node != nullptr; node = node->next) {
                if (node->record.TryRegister()) {
                    return std::addressof(node->record);
                }
            }
            auto node = std::make_unique<Node>();
            const bool registered = node->record.TryRegister();
            assert(registered);
            Record* result = std::addressof(node->record);
            node->next = head_.load(std::memory_order_relaxed);
            Node* published = node.get();
            storage_.push_back(std::move(node));
            head_.store(published, std::memory_order_seq_cst);
            return result;
        }

        template<typename Visitor>
        void Visit(Visitor&& visitor) const {
            for (Node* node = head_.load(std::memory_order_seq_cst); node != nullptr; node = node->next) {
                visitor(node->record);
            }
        }

    private:
        mutable std::mutex mutex_;
        std::atomic<Node*> head_{nullptr};
        std::vector<std::unique_ptr<Node>> storage_;
    };

} // namespace Sora::Concurrency::Detail

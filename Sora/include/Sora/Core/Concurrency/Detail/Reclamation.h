/**
 * @file Reclamation.h
 * @brief Internal ownership and participant-record mechanics for safe reclamation.
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/InlineFunction.h>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
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

    /** @brief Thread-safe ownership queue with allocation-free batch transfer and requeue. */
    class RetiredList {
    public:
        using Batch = std::list<RetiredObject>;

        template<typename T, typename Deleter>
            requires std::is_nothrow_constructible_v<std::remove_cvref_t<Deleter>, Deleter&&> &&
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
                std::scoped_lock lock(mutex_);
                objects_.push_back(std::move(object));
            } catch (...) {
                object.reclaim();
                throw;
            }
        }

        template<typename T>
        void Retire(T* pointer, std::uint64_t generation = 0) {
            Retire(pointer, std::default_delete<T>{}, generation);
        }

        [[nodiscard]] Batch DetachAll() noexcept {
            Batch detached;
            std::scoped_lock lock(mutex_);
            detached.splice(detached.end(), objects_);
            return detached;
        }

        void Requeue(Batch objects) noexcept {
            if (objects.empty()) {
                return;
            }
            std::scoped_lock lock(mutex_);
            objects_.splice(objects_.end(), objects);
        }

        [[nodiscard]] std::size_t Size() const noexcept {
            std::scoped_lock lock(mutex_);
            return objects_.size();
        }

        void Drain() noexcept {
            for (;;) {
                Batch objects = DetachAll();
                if (objects.empty()) {
                    return;
                }
                Destroy(std::move(objects));
            }
        }

        static void Destroy(Batch objects) noexcept {
            for (RetiredObject& object : objects) {
                object.reclaim();
            }
        }

    private:
        mutable std::mutex mutex_;
        Batch objects_;
    };

    template<typename Safe>
    [[nodiscard]] std::size_t ReclaimDetached(RetiredList& retired, RetiredList::Batch candidates, Safe&& safe) {
        static_assert(std::is_nothrow_invocable_r_v<bool, Safe&, const RetiredObject&>);
        RetiredList::Batch deferred;
        RetiredList::Batch reclaimable;
        for (auto current = candidates.begin(); current != candidates.end();) {
            auto object = current++;
            if (safe(*object)) {
                reclaimable.splice(reclaimable.end(), candidates, object);
            } else {
                deferred.splice(deferred.end(), candidates, object);
            }
        }
        retired.Requeue(std::move(deferred));
        const std::size_t count = reclaimable.size();
        RetiredList::Destroy(std::move(reclaimable));
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

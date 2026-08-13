/**
 * @file Reclamation.h
 * @brief Internal ownership and participant-record mechanics for safe reclamation.
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/InlineFunction.h>

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
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

    struct RetiredNode {
        RetiredObject object;
        RetiredNode* next = nullptr;
    };

    inline constexpr std::uint64_t kGenerationHalfRange = std::uint64_t{1} << 63;

    [[nodiscard]] constexpr bool GenerationReached(std::uint64_t observed, std::uint64_t target) noexcept {
        return observed == target || observed - target < kGenerationHalfRange;
    }

    /** @brief Thread-safe ownership queue with pooled nodes and allocation-free batch transfer. */
    class RetiredList {
    public:
        class Batch {
        public:
            Batch() noexcept = default;
            Batch(const Batch&) = delete;
            Batch& operator=(const Batch&) = delete;
            Batch(Batch&& other) noexcept
                : head_(std::exchange(other.head_, nullptr)),
                  tail_(std::exchange(other.tail_, nullptr)),
                  size_(std::exchange(other.size_, 0)) {}
            Batch& operator=(Batch&&) = delete;

            [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }
            [[nodiscard]] std::size_t size() const noexcept { return size_; }

            void PushBack(RetiredNode* node) noexcept {
                node->next = nullptr;
                if (tail_ == nullptr) {
                    head_ = node;
                } else {
                    tail_->next = node;
                }
                tail_ = node;
                ++size_;
            }

            [[nodiscard]] RetiredNode* PopFront() noexcept {
                RetiredNode* node = head_;
                if (node == nullptr) {
                    return nullptr;
                }
                head_ = node->next;
                if (head_ == nullptr) {
                    tail_ = nullptr;
                }
                node->next = nullptr;
                --size_;
                return node;
            }

        private:
            friend class RetiredList;

            RetiredNode* head_ = nullptr;
            RetiredNode* tail_ = nullptr;
            std::size_t size_ = 0;
        };

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
                std::scoped_lock lock(mutex_);
                RetiredNode* node = AcquireNode();
                node->object = std::move(object);
                objects_.PushBack(node);
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
            Batch staged;
            std::size_t next = 0;
            try {
                std::scoped_lock lock(mutex_);
                for (; next < pointers.size(); ++next) {
                    RawPointer pointer = pointers[next];
                    if (pointer == nullptr) {
                        continue;
                    }
                    RetiredNode* node = AcquireNode();
                    auto reclaim = [pointer, deleter]() mutable noexcept { deleter(pointer); };
                    node->object = RetiredObject{pointer, std::move(reclaim), generation};
                    staged.PushBack(node);
                }
                Append(objects_, std::move(staged));
                return;
            } catch (...) {
                Destroy(std::move(staged));
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
            std::scoped_lock lock(mutex_);
            return Batch(std::move(objects_));
        }

        void Requeue(Batch objects) noexcept {
            if (objects.empty()) {
                return;
            }
            std::scoped_lock lock(mutex_);
            Append(objects_, std::move(objects));
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

        void Destroy(Batch objects) noexcept {
            Batch reusable;
            while (RetiredNode* node = objects.PopFront()) {
                node->object.reclaim();
                node->object = {};
                reusable.PushBack(node);
            }
            std::scoped_lock lock(mutex_);
            ReleaseNodes(std::move(reusable));
        }

    private:
        static constexpr std::size_t kNodesPerBlock = 64;

        struct Block {
            std::array<RetiredNode, kNodesPerBlock> nodes;
            std::unique_ptr<Block> next;
        };

        [[nodiscard]] RetiredNode* AcquireNode() {
            if (free_ == nullptr) {
                auto block = std::make_unique<Block>();
                for (RetiredNode& node : block->nodes) {
                    node.next = free_;
                    free_ = std::addressof(node);
                }
                block->next = std::move(blocks_);
                blocks_ = std::move(block);
            }
            RetiredNode* node = free_;
            free_ = node->next;
            node->next = nullptr;
            return node;
        }

        static void Append(Batch& destination, Batch source) noexcept {
            if (source.empty()) {
                return;
            }
            if (destination.tail_ == nullptr) {
                destination.head_ = std::exchange(source.head_, nullptr);
                destination.tail_ = std::exchange(source.tail_, nullptr);
                destination.size_ = std::exchange(source.size_, 0);
                return;
            }
            destination.tail_->next = source.head_;
            destination.tail_ = source.tail_;
            destination.size_ += source.size_;
            source.head_ = nullptr;
            source.tail_ = nullptr;
            source.size_ = 0;
        }

        void ReleaseNodes(Batch nodes) noexcept {
            if (nodes.empty()) {
                return;
            }
            nodes.tail_->next = free_;
            free_ = nodes.head_;
        }

        mutable std::mutex mutex_;
        Batch objects_;
        RetiredNode* free_ = nullptr;
        std::unique_ptr<Block> blocks_;
    };

    template<typename Safe>
    [[nodiscard]] std::size_t ReclaimDetached(RetiredList& retired, RetiredList::Batch candidates, Safe&& safe) {
        static_assert(std::is_nothrow_invocable_r_v<bool, Safe&, const RetiredObject&>);
        RetiredList::Batch deferred;
        RetiredList::Batch reclaimable;
        while (RetiredNode* node = candidates.PopFront()) {
            if (safe(node->object)) {
                reclaimable.PushBack(node);
            } else {
                deferred.PushBack(node);
            }
        }
        retired.Requeue(std::move(deferred));
        const std::size_t count = reclaimable.size();
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

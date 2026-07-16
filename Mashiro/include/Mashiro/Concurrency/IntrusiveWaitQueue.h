/**
 * @file IntrusiveWaitQueue.h
 * @brief Allocation-free FIFO wait queues for asynchronous queue operation states.
 * @ingroup Concurrency
 */
#pragma once

#include <utility>

namespace Mashiro::Concurrency::Detail {

    /** @brief Intrusive node embedded into every queue operation state. */
    struct WaitNode {
        WaitNode* prev{nullptr};
        WaitNode* next{nullptr};
        bool queued{false};
    };

    /**
     * @brief Minimal FIFO intrusive list.
     *
     * @details The container owns no nodes and performs no allocation. Synchronisation belongs to the enclosing
     * queue state; every member function expects the caller to hold the control-plane lock.
     */
    class IntrusiveWaitQueue {
    public:
        [[nodiscard]] bool Empty() const noexcept { return head_ == nullptr; }
        [[nodiscard]] WaitNode* Front() const noexcept { return head_; }

        void PushBack(WaitNode& node) noexcept {
            node.prev = tail_;
            node.next = nullptr;
            node.queued = true;
            if (tail_ != nullptr) {
                tail_->next = &node;
            } else {
                head_ = &node;
            }
            tail_ = &node;
        }

        [[nodiscard]] WaitNode* PopFront() noexcept {
            WaitNode* node = head_;
            if (node != nullptr) {
                Erase(*node);
            }
            return node;
        }

        void Erase(WaitNode& node) noexcept {
            if (!node.queued) {
                return;
            }
            if (node.prev != nullptr) {
                node.prev->next = node.next;
            } else {
                head_ = node.next;
            }
            if (node.next != nullptr) {
                node.next->prev = node.prev;
            } else {
                tail_ = node.prev;
            }
            node.prev = nullptr;
            node.next = nullptr;
            node.queued = false;
        }

    private:
        WaitNode* head_{nullptr};
        WaitNode* tail_{nullptr};
    };

} // namespace Mashiro::Concurrency::Detail

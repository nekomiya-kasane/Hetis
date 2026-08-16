/**
 * @file IntrusiveWaitQueue.h
 * @brief Allocation-free compile-time-ordered wait lists for asynchronous operation states.
 * @ingroup Concurrency
 */
#pragma once

#include "Sora/Core/Concurrency/QueueTopology.h"

#include <utility>

namespace Sora::Concurrency::Detail {

    /** @brief Intrusive node embedded into every queue operation state. */
    struct WaitNode {
        WaitNode* prev{nullptr};
        WaitNode* next{nullptr};
        bool queued{false};
    };

    /**
     * @brief Minimal intrusive list selecting its next operation at compile time.
     *
     * @details The container owns no nodes and performs no allocation. Synchronisation belongs to the enclosing
     * queue state; every member function expects the caller to hold the control-plane lock.
     */
    template<WaiterOrder Order>
    class IntrusiveWaitList {
    public:
        [[nodiscard]] bool Empty() const noexcept { return head_ == nullptr; }
        [[nodiscard]] WaitNode* Next() const noexcept {
            if constexpr (Order == WaiterOrder::Fifo) {
                return head_;
            } else {
                return tail_;
            }
        }

        void Push(WaitNode& node) noexcept {
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

        [[nodiscard]] WaitNode* PopNext() noexcept {
            WaitNode* node = Next();
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

} // namespace Sora::Concurrency::Detail

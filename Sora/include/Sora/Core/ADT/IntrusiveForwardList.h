/**
 * @file IntrusiveForwardList.h
 * @brief Non-owning, allocation-free singly linked topology.
 * @ingroup Core
 */
#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace Sora {

    /** @brief True when @p NextMember is a direct @c Node* link member. */
    template<typename Node, auto NextMember>
    concept IntrusiveForwardLink = std::same_as<decltype(NextMember), Node* Node::*>;

    /**
     * @brief Non-owning singly linked list with O(1) append, pop-front, and detached-segment transfer.
     *
     * @details The list owns only the topology anchor. Nodes must outlive every list or detached segment that contains
     * them. @p NextMember is a compile-time member projection, so multiple independent links per node are supported
     * without type erasure, callbacks, or runtime hook lookup.
     *
     * @tparam Node Node type containing the intrusive link.
     * @tparam NextMember Pointer-to-member selecting the @c Node* forward link.
     */
    template<typename Node, auto NextMember>
        requires IntrusiveForwardLink<Node, NextMember>
    class IntrusiveForwardList {
    public:
        /** @brief Detached intrusive segment whose nodes remain non-owning. */
        class Segment {
        public:
            Segment() noexcept = default;
            Segment(const Segment&) = delete;
            Segment& operator=(const Segment&) = delete;

            Segment(Segment&& other) noexcept
                : head_(std::exchange(other.head_, nullptr)),
                  tail_(std::exchange(other.tail_, nullptr)),
                  size_(std::exchange(other.size_, 0)) {}

            Segment& operator=(Segment&&) = delete;

            [[nodiscard]] bool Empty() const noexcept { return head_ == nullptr; }
            [[nodiscard]] std::size_t Size() const noexcept { return size_; }
            [[nodiscard]] Node* Head() const noexcept { return head_; }
            [[nodiscard]] Node* Tail() const noexcept { return tail_; }

            /** @brief Append an unlinked node without allocating or taking ownership of its lifetime. */
            void PushBack(Node* node) noexcept {
                assert(node != nullptr);
                node->*NextMember = nullptr;
                if (tail_ == nullptr) {
                    head_ = node;
                } else {
                    tail_->*NextMember = node;
                }
                tail_ = node;
                ++size_;
            }

            /** @brief Remove and return the first node, clearing its forward link. */
            [[nodiscard]] Node* PopFront() noexcept {
                Node* node = head_;
                if (node == nullptr) {
                    return nullptr;
                }
                head_ = node->*NextMember;
                if (head_ == nullptr) {
                    tail_ = nullptr;
                }
                node->*NextMember = nullptr;
                --size_;
                return node;
            }

            /** @brief Append another detached segment in O(1) and consume its anchor. */
            void Append(Segment&& source) noexcept {
                if (source.Empty()) {
                    return;
                }
                if (tail_ == nullptr) {
                    head_ = std::exchange(source.head_, nullptr);
                    tail_ = std::exchange(source.tail_, nullptr);
                    size_ = std::exchange(source.size_, 0);
                    return;
                }
                tail_->*NextMember = source.head_;
                tail_ = source.tail_;
                size_ += source.size_;
                source.ForgetLinks();
            }

            /**
             * @brief Forget this anchor after another owner has adopted the complete segment.
             * @pre The caller has already transferred @ref Head and @ref Tail to another topology owner.
             */
            void ForgetLinks() noexcept {
                head_ = nullptr;
                tail_ = nullptr;
                size_ = 0;
            }

        private:
            Node* head_ = nullptr;
            Node* tail_ = nullptr;
            std::size_t size_ = 0;
        };

        IntrusiveForwardList() noexcept = default;
        IntrusiveForwardList(const IntrusiveForwardList&) = delete;
        IntrusiveForwardList& operator=(const IntrusiveForwardList&) = delete;

        IntrusiveForwardList(IntrusiveForwardList&& other) noexcept : segment_(other.DetachAll()) {}
        IntrusiveForwardList& operator=(IntrusiveForwardList&&) = delete;

        ~IntrusiveForwardList() { assert(Empty()); }

        [[nodiscard]] bool Empty() const noexcept { return segment_.Empty(); }
        [[nodiscard]] std::size_t Size() const noexcept { return segment_.Size(); }
        [[nodiscard]] Node* Head() const noexcept { return segment_.Head(); }
        [[nodiscard]] Node* Tail() const noexcept { return segment_.Tail(); }

        /** @brief Append an unlinked node without allocating or taking ownership of its lifetime. */
        void PushBack(Node* node) noexcept { segment_.PushBack(node); }

        /** @brief Remove and return the first node, clearing its forward link. */
        [[nodiscard]] Node* PopFront() noexcept { return segment_.PopFront(); }

        /** @brief Detach the complete sequence in O(1) while preserving node links. */
        [[nodiscard]] Segment DetachAll() noexcept { return std::move(segment_); }

        /** @brief Append a detached segment in O(1). */
        void Append(Segment&& source) noexcept { segment_.Append(std::move(source)); }

        /** @brief Append another list in O(1) and leave the source empty. */
        void Append(IntrusiveForwardList&& source) noexcept { Append(source.DetachAll()); }

    private:
        Segment segment_;
    };

} // namespace Sora

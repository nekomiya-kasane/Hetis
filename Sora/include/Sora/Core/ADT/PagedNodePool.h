/**
 * @file PagedNodePool.h
 * @brief Stable-address reusable storage for permanently constructed intrusive nodes.
 * @ingroup Core
 */
#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <memory>
#include <type_traits>

#include <Sora/Core/ADT/IntrusiveForwardList.h>

namespace Sora {

    /**
     * @brief Stable-address node storage grown in fixed-capacity pages.
     *
     * @details The pool constructs every node in a page once and reuses nodes through @p FreeNextMember. It does not
     * synchronize callers, validate foreign nodes, or assign stable identities. A released node may be returned at the
     * same address, so callers that publish long-lived handles must add their own generation or reclamation protocol.
     *
     * @tparam Node Node type, default-constructible for page construction.
     * @tparam FreeNextMember Pointer-to-member selecting the reusable @c Node* link.
     * @tparam NodesPerPage Number of nodes constructed in each page.
     */
    template<typename Node, auto FreeNextMember, std::size_t NodesPerPage = 64>
        requires IntrusiveForwardLink<Node, FreeNextMember> && std::default_initializable<Node> && (NodesPerPage > 0)
    class PagedNodePool {
        using List = IntrusiveForwardList<Node, FreeNextMember>;
        using Segment = typename List::Segment;

        struct Page {
            std::array<Node, NodesPerPage> nodes;
            std::unique_ptr<Page> next;
        };

    public:
        PagedNodePool() noexcept = default;
        PagedNodePool(const PagedNodePool&) = delete;
        PagedNodePool& operator=(const PagedNodePool&) = delete;
        PagedNodePool(PagedNodePool&&) = delete;
        PagedNodePool& operator=(PagedNodePool&&) = delete;
        ~PagedNodePool() = default;

        /** @brief Acquire one detached node, allocating and initializing a page when necessary. */
        [[nodiscard]] Node* Acquire() {
            if (free_ == nullptr) {
                auto page = std::make_unique<Page>();
                for (Node& node : page->nodes) {
                    node.*FreeNextMember = free_;
                    free_ = std::addressof(node);
                }
                page->next = std::move(pages_);
                pages_ = std::move(page);
                ++pageCount_;
            }
            Node* node = free_;
            free_ = node->*FreeNextMember;
            node->*FreeNextMember = nullptr;
            return node;
        }

        /** @brief Return one detached node to the free chain. @pre @p node is non-null and belongs to this pool. */
        void Release(Node* node) noexcept {
            assert(node != nullptr);
            node->*FreeNextMember = free_;
            free_ = node;
        }

        /** @brief Return an entire detached segment to the free chain in O(1). */
        void Release(Segment&& nodes) noexcept {
            if (nodes.Empty()) {
                return;
            }
            nodes.Tail()->*FreeNextMember = free_;
            free_ = nodes.Head();
            nodes.ForgetLinks();
        }

        /** @brief Number of allocated pages retained by this pool. */
        [[nodiscard]] std::size_t PageCount() const noexcept { return pageCount_; }

        /** @brief Total number of permanently constructed node slots. */
        [[nodiscard]] std::size_t Capacity() const noexcept { return pageCount_ * NodesPerPage; }

    private:
        std::unique_ptr<Page> pages_;
        Node* free_ = nullptr;
        std::size_t pageCount_ = 0;
    };

} // namespace Sora

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

#include <Sora/Platform.h>

#include <Sora/Core/ADT/IntrusiveForwardList.h>
#include <Sora/Core/Memory/AllocatorTraits.h>

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
     * @tparam Allocator Allocator used only when a new page is grown.
     */
    template<typename Node, auto FreeNextMember, std::size_t NodesPerPage = 64,
             typename Allocator = std::allocator<Node>>
        requires IntrusiveForwardLink<Node, FreeNextMember> && std::default_initializable<Node> && (NodesPerPage > 0) &&
                 Concept::RebindableAllocator<Allocator, Node>
    class PagedNodePool {
        using List = IntrusiveForwardList<Node, FreeNextMember>;
        using Segment = typename List::Segment;

        struct Page {
            std::array<Node, NodesPerPage> nodes;
            Page* next = nullptr;
        };

        using PageAllocator = typename std::allocator_traits<Allocator>::template rebind_alloc<Page>;
        using PageTraits = std::allocator_traits<PageAllocator>;

        static_assert(Concept::RebindableAllocator<Allocator, Page>,
                      "PagedNodePool allocator must rebind to its page type.");

    public:
        PagedNodePool() noexcept(std::is_nothrow_default_constructible_v<PageAllocator>) = default;
        explicit PagedNodePool(const Allocator& allocator) noexcept(
            std::is_nothrow_constructible_v<PageAllocator, const Allocator&>)
            : allocator_(allocator) {}
        PagedNodePool(const PagedNodePool&) = delete;
        PagedNodePool& operator=(const PagedNodePool&) = delete;
        PagedNodePool(PagedNodePool&&) = delete;
        PagedNodePool& operator=(PagedNodePool&&) = delete;
        ~PagedNodePool() { ClearPages(); }

        /** @brief Acquire one detached node, allocating and initializing a page when necessary. */
        [[nodiscard]] Node* Acquire() {
            Reserve(1);
            return AcquireReserved();
        }

        /** @brief Ensure @p count future acquisitions do not allocate. */
        void Reserve(std::size_t count) {
            while (freeCount_ < count) {
                AddPage();
            }
        }

        /** @brief Acquire a node after a sufficient @ref Reserve call. */
        [[nodiscard]] Node* AcquireReserved() noexcept {
            assert(free_ != nullptr && freeCount_ != 0);
            Node* node = free_;
            free_ = node->*FreeNextMember;
            node->*FreeNextMember = nullptr;
            --freeCount_;
            return node;
        }

        /** @brief Return one detached node to the free chain. @pre @p node is non-null and belongs to this pool. */
        void Release(Node* node) noexcept {
            assert(node != nullptr);
            node->*FreeNextMember = free_;
            free_ = node;
            ++freeCount_;
        }

        /** @brief Return an entire detached segment to the free chain in O(1). */
        void Release(Segment&& nodes) noexcept {
            if (nodes.Empty()) {
                return;
            }
            nodes.Tail()->*FreeNextMember = free_;
            free_ = nodes.Head();
            freeCount_ += nodes.Size();
            nodes.ForgetLinks();
        }

        /** @brief Number of allocated pages retained by this pool. */
        [[nodiscard]] std::size_t PageCount() const noexcept { return pageCount_; }

        /** @brief Total number of permanently constructed node slots. */
        [[nodiscard]] std::size_t Capacity() const noexcept { return pageCount_ * NodesPerPage; }

    private:
        void AddPage() {
            Page* page = PageTraits::allocate(allocator_, 1);
            try {
                PageTraits::construct(allocator_, page);
            } catch (...) {
                PageTraits::deallocate(allocator_, page, 1);
                throw;
            }
            for (Node& node : page->nodes) {
                node.*FreeNextMember = free_;
                free_ = std::addressof(node);
            }
            page->next = pages_;
            pages_ = page;
            ++pageCount_;
            freeCount_ += NodesPerPage;
        }

        void ClearPages() noexcept {
            while (pages_ != nullptr) {
                Page* page = pages_;
                pages_ = page->next;
                PageTraits::destroy(allocator_, page);
                PageTraits::deallocate(allocator_, page, 1);
            }
        }

        SORA_NO_UNIQUE_ADDRESS PageAllocator allocator_{};
        Page* pages_ = nullptr;
        Node* free_ = nullptr;
        std::size_t pageCount_ = 0;
        std::size_t freeCount_ = 0;
    };

} // namespace Sora

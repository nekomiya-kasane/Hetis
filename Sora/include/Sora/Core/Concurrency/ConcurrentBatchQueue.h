/**
 * @file ConcurrentBatchQueue.h
 * @brief Mutex-protected queue with allocation-free detached batch processing.
 * @ingroup Concurrency
 */
#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>

#include <Sora/Core/ADT/IntrusiveForwardList.h>
#include <Sora/Core/ADT/PagedNodePool.h>
#include <Sora/Core/Guard.h>

namespace Sora::Concurrency {

    namespace Detail {

        template<typename T>
        struct BatchQueueEntry {
        private:
            alignas(T) std::byte storage_[sizeof(T)];

        public:
            BatchQueueEntry* next = nullptr;

            [[nodiscard]] T* StorageAddress() noexcept { return reinterpret_cast<T*>(storage_); }
            [[nodiscard]] T* Value() noexcept { return std::launder(reinterpret_cast<T*>(storage_)); }

            [[nodiscard]] const T* Value() const noexcept { return std::launder(reinterpret_cast<const T*>(storage_)); }

            template<typename... Args>
            T& Construct(Args&&... args) {
                return *std::construct_at(StorageAddress(), std::forward<Args>(args)...);
            }

            void Destroy() noexcept { std::destroy_at(Value()); }
        };

    } // namespace Detail

    /**
     * @brief Concurrent ownership queue whose consumers process detached batches outside the mutex.
     *
     * @details Producers construct values in permanently addressed page nodes. @ref TakeAll detaches the complete
     * pending sequence in O(1); consumers may partition and process that batch without holding the queue mutex,
     * then return it through @ref Requeue or @ref Release. The queue owns values, while @ref Batch owns only detached
     * node topology and must not outlive its source queue.
     *
     * @tparam T Value type. Its destructor must be non-throwing.
     * @tparam NodesPerPage Number of reusable queue nodes allocated per page.
     * @tparam Allocator Allocator used only when a new page is grown.
     */
    template<typename T, std::size_t NodesPerPage = 64, typename Allocator = std::allocator<T>>
        requires(std::is_object_v<T> && NodesPerPage > 0 && std::is_nothrow_destructible_v<T> &&
                 requires { typename std::allocator_traits<Allocator>::template rebind_alloc<T>; })
    class ConcurrentBatchQueue {
    public:
        using ValueType = T;

        /** @brief Public batch node used to partition detached batches without type erasure. */
        using Entry = Detail::BatchQueueEntry<T>;

    private:
        using List = Sora::IntrusiveForwardList<Entry, &Entry::next>;
        using Segment = typename List::Segment;
        using Pool = Sora::PagedNodePool<Entry, &Entry::next, NodesPerPage, Allocator>;

        static_assert(std::is_default_constructible_v<Entry>);

    public:
        /** @brief Detached, move-only sequence of queue entries. */
        class Batch {
        public:
            Batch(const Batch&) = delete;
            Batch& operator=(const Batch&) = delete;
            Batch(Batch&& other) noexcept
                : list_(std::move(other.list_)), owner_(std::exchange(other.owner_, nullptr)) {}
            Batch& operator=(Batch&&) = delete;

            ~Batch() { assert(list_.Empty()); }

            [[nodiscard]] bool Empty() const noexcept { return list_.Empty(); }
            [[nodiscard]] std::size_t Size() const noexcept { return list_.Size(); }

            /** @brief Remove and return the first entry for lock-free batch processing. */
            [[nodiscard]] Entry* PopFront() noexcept { return list_.PopFront(); }

            /** @brief Append an entry to a detached batch without allocating. */
            void PushBack(Entry* entry) noexcept { list_.PushBack(entry); }

            /** @brief Append another detached batch in O(1). */
            void Append(Batch&& source) noexcept {
                assert(owner_ == source.owner_ && "detached batches must originate from the same queue");
                list_.Append(std::move(source.list_));
            }

            /** @brief Create an empty partition that retains this batch's source-queue identity. */
            [[nodiscard]] Batch MakeEmptySibling() const noexcept { return Batch(owner_); }

            [[nodiscard]] static T& Value(Entry& entry) noexcept { return *entry.Value(); }
            [[nodiscard]] static const T& Value(const Entry& entry) noexcept { return *entry.Value(); }

        private:
            friend class ConcurrentBatchQueue;

            explicit Batch(const ConcurrentBatchQueue* owner) noexcept : owner_(owner) {}
            Batch(List&& list, const ConcurrentBatchQueue* owner) noexcept : list_(std::move(list)), owner_(owner) {}

            [[nodiscard]] Segment Detach() noexcept { return list_.DetachAll(); }

            List list_;
            const ConcurrentBatchQueue* owner_ = nullptr;
        };

        ConcurrentBatchQueue() noexcept(std::is_nothrow_default_constructible_v<Pool>) = default;
        explicit ConcurrentBatchQueue(const Allocator& allocator) noexcept(
            std::is_nothrow_constructible_v<Pool, const Allocator&>)
            : pool_(allocator) {}
        ConcurrentBatchQueue(const ConcurrentBatchQueue&) = delete;
        ConcurrentBatchQueue& operator=(const ConcurrentBatchQueue&) = delete;
        ConcurrentBatchQueue(ConcurrentBatchQueue&&) = delete;
        ConcurrentBatchQueue& operator=(ConcurrentBatchQueue&&) = delete;

        ~ConcurrentBatchQueue() { Clear(); }

        /** @brief Construct and enqueue one value; page allocation is the only expected allocation point. */
        template<typename... Args>
            requires std::constructible_from<T, Args...>
        void Emplace(Args&&... args) {
            std::scoped_lock lock(mutex_);
            Entry* entry = pool_.Acquire();
            Sora::ScopeExit recycleEntry{[&]() noexcept { pool_.Release(entry); }};
            entry->Construct(std::forward<Args>(args)...);
            pending_.PushBack(entry);
            recycleEntry.Release();
        }

        /**
         * @brief Acquire storage before invoking a factory that transfers one value into the queue.
         *
         * @details If storage acquisition throws, @p factory is not invoked. A throwing factory only recycles the
         * acquired node, so it cannot observe a partially constructed queue value.
         */
        template<typename Factory>
            requires std::invocable<Factory&> && std::constructible_from<T, std::invoke_result_t<Factory&>>
        void EmplaceGenerated(Factory&& factory) {
            std::scoped_lock lock(mutex_);
            Entry* entry = pool_.Acquire();
            Sora::ScopeExit recycleEntry{[&]() noexcept { pool_.Release(entry); }};
            entry->Construct(std::invoke(std::forward<Factory>(factory)));
            pending_.PushBack(entry);
            recycleEntry.Release();
        }

        /**
         * @brief Construct a range of values while taking one queue lock.
         *
         * @details The factory may return either @c T or @c std::optional<T>. A disengaged optional skips that input
         * after recycling its reserved node. Factory and value construction execute under the queue mutex and should
         * therefore be short, non-blocking, and non-reentrant. If construction throws, the successfully constructed
         * prefix remains pending and the exception propagates; no constructed value is destroyed during propagation.
         */
        template<std::ranges::input_range Range, typename Factory>
            requires std::invocable<Factory&, std::ranges::range_reference_t<Range>> &&
                     (std::same_as<
                          std::remove_cvref_t<std::invoke_result_t<Factory&, std::ranges::range_reference_t<Range>>>,
                          T> ||
                      std::same_as<
                          std::remove_cvref_t<std::invoke_result_t<Factory&, std::ranges::range_reference_t<Range>>>,
                          std::optional<T>>)
        void EmplaceBatch(Range&& range, Factory&& factory) {
            std::unique_lock lock(mutex_);
            Batch staged{this};
            Sora::ScopeExit commitStaged{[&]() noexcept { pending_.Append(std::move(staged.list_)); }};
            for (auto&& input : range) {
                Entry* entry = pool_.Acquire();
                Sora::ScopeExit recycleEntry{[&]() noexcept { pool_.Release(entry); }};
                auto result = std::invoke(factory, std::forward<decltype(input)>(input));
                if constexpr (std::same_as<std::remove_cvref_t<decltype(result)>, std::optional<T>>) {
                    if (!result) {
                        continue;
                    }
                    entry->Construct(std::move(*result));
                } else {
                    entry->Construct(std::move(result));
                }
                staged.PushBack(entry);
                recycleEntry.Release();
            }
        }

        /**
         * @brief Reserve a sized batch before invoking its non-throwing factory.
         *
         * @details The reservation is an upper bound; disengaged optional results leave unused nodes available for
         * later calls. Allocation failure occurs before @p factory is invoked and leaves the input ownership untouched.
         */
        template<std::ranges::sized_range Range, typename Factory>
            requires std::invocable<Factory&, std::ranges::range_reference_t<Range>> &&
                     (std::same_as<
                          std::remove_cvref_t<std::invoke_result_t<Factory&, std::ranges::range_reference_t<Range>>>,
                          T> ||
                      std::same_as<
                          std::remove_cvref_t<std::invoke_result_t<Factory&, std::ranges::range_reference_t<Range>>>,
                          std::optional<T>>) &&
                     std::is_nothrow_invocable_v<Factory&, std::ranges::range_reference_t<Range>> &&
                     std::is_nothrow_move_constructible_v<T>
        void EmplaceReservedBatch(Range&& range, Factory&& factory) {
            std::unique_lock lock(mutex_);
            pool_.Reserve(static_cast<std::size_t>(std::ranges::size(range)));
            Batch staged{this};
            for (auto&& input : range) {
                auto result = std::invoke(factory, std::forward<decltype(input)>(input));
                if constexpr (std::same_as<std::remove_cvref_t<decltype(result)>, std::optional<T>>) {
                    if (!result) {
                        continue;
                    }
                    Entry* entry = pool_.AcquireReserved();
                    entry->Construct(std::move(*result));
                    staged.PushBack(entry);
                } else {
                    Entry* entry = pool_.AcquireReserved();
                    entry->Construct(std::move(result));
                    staged.PushBack(entry);
                }
            }
            pending_.Append(std::move(staged.list_));
        }

        /** @brief Detach all pending values in O(1), leaving the queue empty. */
        [[nodiscard]] Batch TakeAll() noexcept {
            std::scoped_lock lock(mutex_);
            return Batch(std::move(pending_), this);
        }

        /** @brief Requeue a batch in O(1); the batch must originate from this queue. */
        void Requeue(Batch batch) noexcept {
            assert(batch.owner_ == this && "detached batch must originate from this queue");
            if (batch.Empty()) {
                return;
            }
            std::scoped_lock lock(mutex_);
            pending_.Append(std::move(batch.list_));
        }

        /** @brief Destroy values and return all batch nodes to the pool. */
        void Release(Batch batch) noexcept {
            assert(batch.owner_ == this && "detached batch must originate from this queue");
            if (batch.Empty()) {
                return;
            }
            Segment reusable = DestroyValues(batch.Detach());
            std::scoped_lock lock(mutex_);
            pool_.Release(std::move(reusable));
        }

        /** @brief Remove and release every pending value. */
        void Clear() noexcept { Release(TakeAll()); }

        /** @brief Number of pending values, protected by the queue mutex. */
        [[nodiscard]] std::size_t Size() const noexcept {
            std::scoped_lock lock(mutex_);
            return pending_.Size();
        }

        /** @brief Number of allocated node pages retained by this queue. */
        [[nodiscard]] std::size_t PageCount() const noexcept {
            std::scoped_lock lock(mutex_);
            return pool_.PageCount();
        }

    private:
        [[nodiscard]] static Segment DestroyValues(Segment nodes) noexcept {
            Segment reusable;
            while (Entry* entry = nodes.PopFront()) {
                entry->Destroy();
                reusable.PushBack(entry);
            }
            return reusable;
        }

        mutable std::mutex mutex_;
        Pool pool_;
        List pending_;
    };

} // namespace Sora::Concurrency

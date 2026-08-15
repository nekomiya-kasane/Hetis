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

namespace Sora::Concurrency {

    namespace Detail {

        template<typename T>
        struct BatchQueueEntry {
        private:
            alignas(T) std::byte storage_[sizeof(T)];

        public:
            BatchQueueEntry* next = nullptr;

            [[nodiscard]] T* Value() noexcept {
                return std::launder(reinterpret_cast<T*>(storage_));
            }

            [[nodiscard]] const T* Value() const noexcept {
                return std::launder(reinterpret_cast<const T*>(storage_));
            }

            template<typename... Args>
            T& Construct(Args&&... args) {
                return *std::construct_at(Value(), std::forward<Args>(args)...);
            }

            void Destroy() noexcept { std::destroy_at(Value()); }
        };

    } // namespace Detail

    /**
     * @brief Concurrent ownership queue whose consumers process detached batches outside the mutex.
     *
     * @details Producers construct values in permanently addressed page nodes. @ref TakeAll detaches the complete
     * pending sequence in O(1); consumers may partition and process that batch without holding the queue mutex, then
     * return it through @ref Requeue or @ref Release. The queue owns values, while @ref Batch owns only detached node
     * topology and must not outlive its source queue.
     *
     * @tparam T Value type. Its destructor must be non-throwing.
     * @tparam NodesPerPage Number of reusable queue nodes allocated per page.
     */
    template<typename T, std::size_t NodesPerPage = 64>
        requires(std::is_object_v<T> && NodesPerPage > 0 && std::is_nothrow_destructible_v<T>)
    class ConcurrentBatchQueue {
    public:
        /** @brief Public batch node used to partition detached batches without type erasure. */
        using Entry = Detail::BatchQueueEntry<T>;

    private:
        using List = Sora::IntrusiveForwardList<Entry, &Entry::next>;
        using Segment = typename List::Segment;
        using Pool = Sora::PagedNodePool<Entry, &Entry::next, NodesPerPage>;

        static_assert(std::is_default_constructible_v<Entry>);

    public:
        /** @brief Detached, move-only sequence of queue entries. */
        class Batch {
        public:
            Batch() noexcept = default;
            Batch(const Batch&) = delete;
            Batch& operator=(const Batch&) = delete;
            Batch(Batch&& other) noexcept : list_(std::move(other.list_)) {}
            Batch& operator=(Batch&&) = delete;

            ~Batch() { assert(list_.Empty()); }

            [[nodiscard]] bool Empty() const noexcept { return list_.Empty(); }
            [[nodiscard]] std::size_t Size() const noexcept { return list_.Size(); }

            /** @brief Remove and return the first entry for lock-free batch processing. */
            [[nodiscard]] Entry* PopFront() noexcept { return list_.PopFront(); }

            /** @brief Append an entry to a detached batch without allocating. */
            void PushBack(Entry* entry) noexcept { list_.PushBack(entry); }

            /** @brief Append another detached batch in O(1). */
            void Append(Batch&& source) noexcept { list_.Append(std::move(source.list_)); }

            [[nodiscard]] static T& Value(Entry& entry) noexcept { return *entry.Value(); }
            [[nodiscard]] static const T& Value(const Entry& entry) noexcept { return *entry.Value(); }

        private:
            friend class ConcurrentBatchQueue;

            explicit Batch(List&& list) noexcept : list_(std::move(list)) {}

            [[nodiscard]] Segment Detach() noexcept { return list_.DetachAll(); }

            List list_;
        };

        ConcurrentBatchQueue() noexcept = default;
        ConcurrentBatchQueue(const ConcurrentBatchQueue&) = delete;
        ConcurrentBatchQueue& operator=(const ConcurrentBatchQueue&) = delete;
        ConcurrentBatchQueue(ConcurrentBatchQueue&&) = delete;
        ConcurrentBatchQueue& operator=(ConcurrentBatchQueue&&) = delete;

        ~ConcurrentBatchQueue() { Clear(); }

        /** @brief Construct and enqueue one value; page allocation is the only expected allocation point. */
        template<typename... Args>
            requires std::constructible_from<T, Args...>
        T& Emplace(Args&&... args) {
            std::scoped_lock lock(mutex_);
            Entry* entry = pool_.Acquire();
            try {
                T& value = entry->Construct(std::forward<Args>(args)...);
                pending_.PushBack(entry);
                return value;
            } catch (...) {
                pool_.Release(entry);
                throw;
            }
        }

        /**
         * @brief Construct a range of values while taking one queue lock.
         *
         * @details The factory may return either @c T or @c std::optional<T>. A disengaged optional skips that input
         * after recycling its reserved node. Factory and value construction execute under the queue mutex and should
         * therefore be short, non-blocking, and non-reentrant.
         */
        template<std::ranges::input_range Range, typename Factory>
            requires std::invocable<Factory&, std::ranges::range_reference_t<Range>> &&
                     (std::same_as<
                          std::remove_cvref_t<
                              std::invoke_result_t<Factory&, std::ranges::range_reference_t<Range>>>,
                          T> ||
                      std::same_as<
                          std::remove_cvref_t<std::invoke_result_t<Factory&, std::ranges::range_reference_t<Range>>>,
                                   std::optional<T>>)
        void EmplaceBatch(Range&& range, Factory&& factory) {
            std::scoped_lock lock(mutex_);
            Batch staged;
            for (auto&& input : range) {
                Entry* entry = nullptr;
                try {
                    entry = pool_.Acquire();
                    auto result = std::invoke(factory, std::forward<decltype(input)>(input));
                    if constexpr (std::same_as<std::remove_cvref_t<decltype(result)>, std::optional<T>>) {
                        if (!result) {
                            pool_.Release(entry);
                            continue;
                        }
                        entry->Construct(std::move(*result));
                    } else {
                        entry->Construct(std::move(result));
                    }
                    staged.PushBack(entry);
                    entry = nullptr;
                } catch (...) {
                    if (entry != nullptr) {
                        pool_.Release(entry);
                    }
                    ReleaseUnlocked(std::move(staged));
                    throw;
                }
            }
            pending_.Append(std::move(staged.list_));
        }

        /** @brief Detach all pending values in O(1), leaving the queue empty. */
        [[nodiscard]] Batch TakeAll() noexcept {
            std::scoped_lock lock(mutex_);
            return Batch(std::move(pending_));
        }

        /** @brief Requeue a batch in O(1); the batch must originate from this queue. */
        void Requeue(Batch batch) noexcept {
            if (batch.Empty()) {
                return;
            }
            std::scoped_lock lock(mutex_);
            pending_.Append(std::move(batch.list_));
        }

        /** @brief Destroy values and return all batch nodes to the pool. */
        void Release(Batch batch) noexcept {
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

        void ReleaseUnlocked(Batch batch) noexcept {
            Segment reusable = DestroyValues(batch.Detach());
            pool_.Release(std::move(reusable));
        }

        mutable std::mutex mutex_;
        Pool pool_;
        List pending_;
    };

} // namespace Sora::Concurrency

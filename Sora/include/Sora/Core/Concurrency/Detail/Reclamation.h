/**
 * @file Reclamation.h
 * @brief Internal ownership and participant-record mechanics for safe reclamation.
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/Concurrency/ConcurrentBatchQueue.h>
#include <Sora/Core/Guard.h>
#include <Sora/Core/InlineFunction.h>

#include <array>
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

    /** @brief Move-only ownership of one deferred callback; destruction is the final no-loss rollback path. */
    struct RetiredObject {
        RetiredObject(void* pointer, InlineFunction<void() noexcept, 32> reclaim, std::uint64_t generation) noexcept
            : pointer(pointer), reclaim(std::move(reclaim)), generation(generation) {}
        RetiredObject(const RetiredObject&) = delete;
        RetiredObject& operator=(const RetiredObject&) = delete;
        RetiredObject(RetiredObject&& other) noexcept
            : pointer(std::exchange(other.pointer, nullptr)),
              reclaim(std::move(other.reclaim)),
              generation(other.generation) {}
        RetiredObject& operator=(RetiredObject&&) = delete;
        ~RetiredObject() { Reclaim(); }

        void Reclaim() noexcept {
            pointer = nullptr;
            auto callback = std::move(reclaim);
            if (callback) {
                callback();
            }
        }

        void* pointer = nullptr;
        InlineFunction<void() noexcept, 32> reclaim;
        std::uint64_t generation = 0;
    };

    inline constexpr std::uint64_t kGenerationHalfRange = std::uint64_t{1} << 63;

    [[nodiscard]] constexpr bool GenerationReached(std::uint64_t observed, std::uint64_t target) noexcept {
        return observed == target || observed - target < kGenerationHalfRange;
    }

    /** @brief Retirement-specific safety facade over a generic concurrent batch queue. */
    template<typename Allocator = std::allocator<RetiredObject>>
    class BasicRetirementQueue {
    public:
        using Queue = ConcurrentBatchQueue<RetiredObject, 64, Allocator>;
        using Batch = Queue::Batch;

        BasicRetirementQueue() noexcept(std::is_nothrow_default_constructible_v<Queue>) = default;
        explicit BasicRetirementQueue(const Allocator& allocator) noexcept(
            std::is_nothrow_constructible_v<Queue, const Allocator&>)
            : queue_(allocator) {}
        BasicRetirementQueue(const BasicRetirementQueue&) = delete;
        BasicRetirementQueue& operator=(const BasicRetirementQueue&) = delete;
        BasicRetirementQueue(BasicRetirementQueue&&) = delete;
        BasicRetirementQueue& operator=(BasicRetirementQueue&&) = delete;

        ~BasicRetirementQueue() { Drain(); }

        template<typename T, typename Deleter>
            requires std::is_nothrow_constructible_v<std::remove_cvref_t<Deleter>, Deleter&&> &&
                     std::is_nothrow_move_constructible_v<std::remove_cvref_t<Deleter>> &&
                     std::is_nothrow_invocable_v<std::remove_cvref_t<Deleter>&, T*>
        void Retire(T* pointer, Deleter&& deleter, std::uint64_t generation = 0) {
            if (pointer == nullptr) {
                return;
            }
            using StoredDeleter = std::remove_cvref_t<Deleter>;
            queue_.EmplaceGenerated([&]() noexcept {
                auto reclaim = [pointer, deleter = StoredDeleter(std::forward<Deleter>(deleter))]() mutable noexcept {
                    deleter(pointer);
                };
                return RetiredObject{pointer, std::move(reclaim), generation};
            });
        }

        template<typename T>
            requires std::is_nothrow_destructible_v<T>
        void Retire(T* pointer, std::uint64_t generation = 0) {
            Retire(pointer, std::default_delete<T>{}, generation);
        }

        template<typename Pointer, std::size_t Extent, typename Deleter>
            requires std::is_pointer_v<std::remove_cv_t<Pointer>> &&
                     std::is_convertible_v<std::remove_cv_t<Pointer>, void*> &&
                     std::is_nothrow_copy_constructible_v<std::remove_cvref_t<Deleter>> &&
                     std::is_nothrow_move_constructible_v<std::remove_cvref_t<Deleter>> &&
                     std::is_nothrow_invocable_v<std::remove_cvref_t<Deleter>&, std::remove_cv_t<Pointer>>
        void Retire(std::span<Pointer, Extent> pointers, Deleter&& deleter, std::uint64_t generation = 0) {
            using RawPointer = std::remove_cv_t<Pointer>;
            using StoredDeleter = std::remove_cvref_t<Deleter>;
            queue_.EmplaceReservedBatch(pointers, [&](RawPointer pointer) noexcept -> std::optional<RetiredObject> {
                if (pointer == nullptr) {
                    return std::nullopt;
                }
                auto reclaim = [pointer, deleter = StoredDeleter(deleter)]() mutable noexcept { deleter(pointer); };
                return RetiredObject{pointer, std::move(reclaim), generation};
            });
        }

        template<typename Pointer, std::size_t Extent>
            requires std::is_pointer_v<std::remove_cv_t<Pointer>> &&
                     std::is_convertible_v<std::remove_cv_t<Pointer>, void*> &&
                     std::is_nothrow_destructible_v<std::remove_pointer_t<std::remove_cv_t<Pointer>>>
        void Retire(std::span<Pointer, Extent> pointers, std::uint64_t generation = 0) {
            using Pointee = std::remove_pointer_t<std::remove_cv_t<Pointer>>;
            Retire(pointers, std::default_delete<Pointee>{}, generation);
        }

        [[nodiscard]] Batch DetachAll() noexcept { return queue_.TakeAll(); }

        void Requeue(Batch objects) noexcept { queue_.Requeue(std::move(objects)); }

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

        void Destroy(Batch objects) noexcept { queue_.Release(std::move(objects)); }

    private:
        Queue queue_;
    };

    using RetirementQueue = BasicRetirementQueue<>;

    template<typename Safe>
    [[nodiscard]] std::size_t ReclaimDetached(RetirementQueue& retired, RetirementQueue::Batch candidates,
                                              Safe&& safe) {
        static_assert(std::is_nothrow_invocable_r_v<bool, Safe&, const RetiredObject&>);
        RetirementQueue::Batch deferred = candidates.MakeEmptySibling();
        RetirementQueue::Batch reclaimable = candidates.MakeEmptySibling();
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

    /** @brief Append-only paged registry with stable, reusable records and allocation-free contiguous scans. */
    template<typename Record, std::size_t RecordsPerPage = 16>
        requires(RecordsPerPage > 0)
    class StableRecordRegistry {
        struct Page {
            Page* next = nullptr;
            std::atomic<std::size_t> publishedCount{0};
            std::array<Record, RecordsPerPage> records{};
        };

    public:
        [[nodiscard]] Record* Register() {
            std::scoped_lock lock(mutex_);
            for (Page* page = head_.load(std::memory_order_relaxed); page != nullptr; page = page->next) {
                const std::size_t publishedCount = page->publishedCount.load(std::memory_order_relaxed);
                for (std::size_t index = 0; index < publishedCount; ++index) {
                    if (page->records[index].TryRegister()) {
                        return std::addressof(page->records[index]);
                    }
                }
                if (publishedCount != RecordsPerPage) {
                    Record* result = std::addressof(page->records[publishedCount]);
                    const bool registered = result->TryRegister();
                    assert(registered);
                    page->publishedCount.store(publishedCount + 1, std::memory_order_seq_cst);
                    return result;
                }
            }

            auto page = std::make_unique<Page>();
            Record* result = std::addressof(page->records.front());
            const bool registered = result->TryRegister();
            assert(registered);
            page->publishedCount.store(1, std::memory_order_relaxed);
            page->next = head_.load(std::memory_order_relaxed);
            Page* published = page.get();
            storage_.push_back(std::move(page));
            head_.store(published, std::memory_order_seq_cst);
            return result;
        }

        template<typename Visitor>
        void Visit(Visitor&& visitor) const {
            for (Page* page = head_.load(std::memory_order_seq_cst); page != nullptr; page = page->next) {
                const std::size_t publishedCount = page->publishedCount.load(std::memory_order_seq_cst);
                for (std::size_t index = 0; index < publishedCount; ++index) {
                    visitor(page->records[index]);
                }
            }
        }

    private:
        mutable std::mutex mutex_;
        std::atomic<Page*> head_{nullptr};
        std::vector<std::unique_ptr<Page>> storage_;
    };

} // namespace Sora::Concurrency::Detail

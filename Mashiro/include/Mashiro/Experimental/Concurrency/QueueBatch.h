/**
 * @file QueueBatch.h
 * @brief Fixed-capacity batch value used by experimental asynchronous queue operations.
 * @ingroup Concurrency
 */
#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <exception>
#include <iterator>
#include <new>
#include <ranges>
#include <type_traits>
#include <utility>

namespace Mashiro::Experimental::Concurrency {

    /**
     * @brief Fixed-capacity, allocation-free batch container for queue transfer.
     *
     * @details Batch is a value-level transport object. It is deliberately not a queue: it has no waiting semantics,
     * no cancellation semantics, and no hidden heap allocation. Elements are lifetime-managed in raw inline storage,
     * so payloads do not need to be default-constructible. If a range constructor observes more than @p Capacity
     * elements, it records @ref oversized without consuming a prefix of the input range; the async facade can then
     * report a permanent @ref QueueErrorCode::BatchTooLarge instead of parking an operation that can never complete.
     */
    template<class T, std::size_t Capacity>
        requires(Capacity > 0)
    class QueueBatch {
        struct Slot {
            alignas(T) std::byte bytes[sizeof(T)];
        };

        template<bool Const>
        class BasicIterator {
            using Owner = std::conditional_t<Const, const QueueBatch, QueueBatch>;

        public:
            using iterator_concept = std::forward_iterator_tag;
            using iterator_category = std::forward_iterator_tag;
            using value_type = T;
            using difference_type = std::ptrdiff_t;
            using reference = std::conditional_t<Const, const T&, T&>;
            using pointer = std::conditional_t<Const, const T*, T*>;

            BasicIterator() = default;
            BasicIterator(Owner* owner, std::size_t index) noexcept : owner_(owner), index_(index) {}

            [[nodiscard]] reference operator*() const noexcept { return (*owner_)[index_]; }
            [[nodiscard]] pointer operator->() const noexcept { return &(*owner_)[index_]; }

            BasicIterator& operator++() noexcept {
                ++index_;
                return *this;
            }

            BasicIterator operator++(int) noexcept {
                BasicIterator copy = *this;
                ++*this;
                return copy;
            }

            [[nodiscard]] friend bool operator==(BasicIterator, BasicIterator) noexcept = default;

        private:
            Owner* owner_{nullptr};
            std::size_t index_{0};
        };

    public:
        using value_type = T;
        using iterator = BasicIterator<false>;
        using const_iterator = BasicIterator<true>;
        static constexpr std::size_t kCapacity = Capacity;

        QueueBatch() = default;

        template<class Range>
            requires(!std::same_as<std::remove_cvref_t<Range>, QueueBatch>) && std::ranges::sized_range<Range> &&
                    std::constructible_from<T, std::ranges::range_reference_t<Range>>
        explicit QueueBatch(Range&& range)
            noexcept(std::is_nothrow_constructible_v<T, std::ranges::range_reference_t<Range>>) {
            if (std::ranges::size(range) > Capacity) {
                oversized_ = true;
                return;
            }
            try {
                for (auto&& value : range) {
                    Push(std::forward<decltype(value)>(value));
                }
            } catch (...) {
                Clear();
                throw;
            }
        }

        QueueBatch(const QueueBatch& other) requires std::copy_constructible<T> {
            try {
                CopyFrom(other);
            } catch (...) {
                Clear();
                throw;
            }
        }
        QueueBatch(const QueueBatch&) requires(!std::copy_constructible<T>) = delete;

        QueueBatch(QueueBatch&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
            requires std::move_constructible<T> {
            try {
                MoveFrom(other);
            } catch (...) {
                Clear();
                throw;
            }
        }

        QueueBatch& operator=(const QueueBatch& other) requires std::copy_constructible<T> {
            if (this != &other) {
                Clear();
                CopyFrom(other);
            }
            return *this;
        }

        QueueBatch& operator=(const QueueBatch& other) requires(!std::copy_constructible<T>) = delete;

        QueueBatch& operator=(QueueBatch&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
            requires std::move_constructible<T> {
            if (this != &other) {
                Clear();
                MoveFrom(other);
            }
            return *this;
        }

        ~QueueBatch() { Clear(); }

        [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
        [[nodiscard]] std::size_t size() const noexcept { return size_; }
        [[nodiscard]] bool oversized() const noexcept { return oversized_; }
        [[nodiscard]] static consteval std::size_t capacity() noexcept { return Capacity; }

        [[nodiscard]] T& operator[](std::size_t index) noexcept { return *Ptr(index); }
        [[nodiscard]] const T& operator[](std::size_t index) const noexcept { return *Ptr(index); }

        [[nodiscard]] iterator begin() noexcept { return iterator{this, 0}; }
        [[nodiscard]] iterator end() noexcept { return iterator{this, size_}; }
        [[nodiscard]] const_iterator begin() const noexcept { return const_iterator{this, 0}; }
        [[nodiscard]] const_iterator end() const noexcept { return const_iterator{this, size_}; }
        [[nodiscard]] const_iterator cbegin() const noexcept { return begin(); }
        [[nodiscard]] const_iterator cend() const noexcept { return end(); }

        template<class U>
            requires std::constructible_from<T, U&&>
        [[nodiscard]] bool TryPush(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
            if (size_ == Capacity) {
                return false;
            }
            ::new (Raw(size_)) T(std::forward<U>(value));
            ++size_;
            return true;
        }

        template<class U>
            requires std::constructible_from<T, U&&>
        void Push(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
            if (!TryPush(std::forward<U>(value))) [[unlikely]] {
                std::terminate();
            }
        }

        void Clear() noexcept {
            while (size_ != 0) {
                --size_;
                std::destroy_at(Ptr(size_));
            }
            oversized_ = false;
        }

    private:
        [[nodiscard]] void* Raw(std::size_t index) noexcept { return static_cast<void*>(&storage_[index].bytes[0]); }

        [[nodiscard]] T* Ptr(std::size_t index) noexcept {
            return std::launder(reinterpret_cast<T*>(Raw(index)));
        }

        [[nodiscard]] const T* Ptr(std::size_t index) const noexcept {
            return std::launder(reinterpret_cast<const T*>(&storage_[index].bytes[0]));
        }

        void CopyFrom(const QueueBatch& other) requires std::copy_constructible<T> {
            oversized_ = other.oversized_;
            for (const T& value : other) {
                Push(value);
            }
        }

        void MoveFrom(QueueBatch& other) noexcept(std::is_nothrow_move_constructible_v<T>)
            requires std::move_constructible<T> {
            oversized_ = other.oversized_;
            for (T& value : other) {
                Push(std::move(value));
            }
            other.Clear();
        }

        std::array<Slot, Capacity> storage_{};
        std::size_t size_{0};
        bool oversized_{false};
    };

} // namespace Mashiro::Experimental::Concurrency
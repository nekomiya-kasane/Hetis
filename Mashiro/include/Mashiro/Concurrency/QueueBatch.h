/**
 * @file QueueBatch.h
 * @brief Fixed-capacity batch value used by asynchronous queue operations.
 * @ingroup Concurrency
 */
#pragma once

#include <concepts>
#include <cstddef>
#include <exception>
#include <initializer_list>
#include <inplace_vector>
#include <ranges>
#include <type_traits>
#include <utility>

namespace Mashiro::Concurrency {

    /**
     * @brief Allocation-free transport batch with explicit oversized-input state.
     *
     * @details The container delegates inline lifetime management to C++26 @c std::inplace_vector. A sized range larger
     * than @p Capacity is rejected before any input element is consumed and records @ref oversized, allowing an async
     * operation to report a permanent size error rather than waiting forever for impossible capacity.
     */
    template<class T, std::size_t Capacity>
        requires(Capacity > 0)
    class QueueBatch {
    public:
        using value_type = T;
        using iterator = typename std::inplace_vector<T, Capacity>::iterator;
        using const_iterator = typename std::inplace_vector<T, Capacity>::const_iterator;
        static constexpr std::size_t kCapacity = Capacity;

        QueueBatch() = default;

        QueueBatch(std::initializer_list<T> values)
            requires std::constructible_from<T, const T&>
            : QueueBatch(std::views::all(values)) {}

        template<std::ranges::sized_range Range>
            requires(!std::same_as<std::remove_cvref_t<Range>, QueueBatch>) &&
                    std::constructible_from<T, std::ranges::range_reference_t<Range>>
        explicit QueueBatch(Range&& range) {
            if (std::ranges::size(range) > Capacity) {
                oversized_ = true;
                return;
            }
            for (auto&& value : range) {
                values_.emplace_back(std::forward<decltype(value)>(value));
            }
        }

        [[nodiscard]] bool empty() const noexcept { return values_.empty(); }
        [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }
        [[nodiscard]] bool oversized() const noexcept { return oversized_; }
        [[nodiscard]] static consteval std::size_t capacity() noexcept { return Capacity; }

        [[nodiscard]] T& operator[](std::size_t index) noexcept { return values_[index]; }
        [[nodiscard]] const T& operator[](std::size_t index) const noexcept { return values_[index]; }

        [[nodiscard]] iterator begin() noexcept { return values_.begin(); }
        [[nodiscard]] iterator end() noexcept { return values_.end(); }
        [[nodiscard]] const_iterator begin() const noexcept { return values_.begin(); }
        [[nodiscard]] const_iterator end() const noexcept { return values_.end(); }
        [[nodiscard]] const_iterator cbegin() const noexcept { return values_.cbegin(); }
        [[nodiscard]] const_iterator cend() const noexcept { return values_.cend(); }

        template<class U>
            requires std::constructible_from<T, U&&>
        [[nodiscard]] bool TryPush(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
            if (oversized_ || values_.size() == Capacity) {
                return false;
            }
            values_.emplace_back(std::forward<U>(value));
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
            values_.clear();
            oversized_ = false;
        }

    private:
        std::inplace_vector<T, Capacity> values_{};
        bool oversized_{false};
    };

} // namespace Mashiro::Concurrency

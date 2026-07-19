/**
 * @file FixedCapacityVector.h
 * @brief Fixed-capacity vector-like storage for constexpr-friendly descriptor tables.
 * @ingroup Core
 */

#pragma once

#include <array>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace Sora {

    /**
     * @brief Fixed-capacity vector-like container backed by std::array.
     *
     * Provides a small vector interface with compile-time capacity and runtime size. The element type must be default
     * constructible and trivially destructible.
     *
     * @tparam T Element type.
     * @tparam N Maximum number of elements the container can hold.
     */
    template<typename T, size_t N = 1024>
        requires std::is_default_constructible_v<T> && std::is_trivially_destructible_v<T>
    class FixedCapacityVector {
        /** @brief Internal contiguous storage. */
        std::array<T, N> data{};

        /** @brief Number of currently stored elements. */
        size_t count = 0;

    public:
        /** @brief Compile-time capacity constant. */
        inline static constexpr size_t kCapacity = N;

        /**
         * @brief Returns the maximum number of elements.
         * @return Container capacity.
         */
        [[nodiscard]] static constexpr auto capacity() noexcept -> size_t { return N; }

        /**
         * @brief Returns the current number of elements.
         * @return Current size.
         */
        [[nodiscard]] constexpr auto size() const noexcept -> size_t { return count; }

        /**
         * @brief Checks whether the container has no elements.
         * @return true if empty; otherwise false.
         */
        [[nodiscard]] constexpr auto empty() const noexcept -> bool { return count == 0; }

        /**
         * @brief Checks whether the container reached its capacity.
         * @return true if full; otherwise false.
         */
        [[nodiscard]] constexpr auto full() const noexcept -> bool { return count >= N; }

        /**
         * @brief Removes all elements by resetting the size to zero.
         */
        constexpr void clear() noexcept { count = 0; }

        /**
         * @brief Appends an element by copy.
         * @param value Element to append.
         * @throws compile-time static string if capacity is exceeded.
         */
        constexpr void push_back(const T& value) {
            if (count >= N) {
                throw "FixedCapacityVector: exceeded capacity.";
            }
            data[count++] = value;
        }

        /**
         * @brief Appends an element by move.
         * @param value Element to append.
         * @throws compile-time static string if capacity is exceeded.
         */
        constexpr void push_back(T&& value) {
            if (count >= N) {
                throw "FixedCapacityVector: exceeded capacity.";
            }
            data[count++] = std::move(value);
        }

        /**
         * @brief Constructs and appends an element in-place from arguments.
         * @tparam Args Constructor argument types.
         * @param args Arguments forwarded to T construction.
         * @return Reference to the inserted element.
         * @throws compile-time static string if capacity is exceeded.
         */
        template<typename... Args>
        constexpr auto emplace_back(Args&&... args) -> T& {
            if (count >= N) {
                throw "FixedCapacityVector: exceeded capacity.";
            }
            auto& slot = data[count++];
            slot = T(std::forward<Args>(args)...);
            return slot;
        }

        /**
         * @brief Removes the last element.
         * @throws compile-time static string if the container is empty.
         */
        constexpr void pop_back() {
            if (count == 0) {
                throw "FixedCapacityVector: pop_back() on empty vector.";
            }
            --count;
        }

        /**
         * @brief Returns pointer to the underlying storage.
         * @return Pointer to contiguous data.
         */
        [[nodiscard]] constexpr auto data_ptr() noexcept -> T* { return data.data(); }

        /** @brief Returns pointer to the underlying storage. */
        [[nodiscard]] constexpr auto data_ptr() const noexcept -> T const* { return data.data(); }

        /**
         * @brief Returns reference to the first element.
         * @return Reference to first element.
         * @throws compile-time static string if the container is empty.
         */
        [[nodiscard]] constexpr auto front() -> T& {
            if (count == 0) {
                throw "FixedCapacityVector: front() on empty vector.";
            }
            return data[0];
        }

        /** @brief Returns reference to the first element. */
        [[nodiscard]] constexpr auto front() const -> T const& {
            if (count == 0) {
                throw "FixedCapacityVector: front() on empty vector.";
            }
            return data[0];
        }

        /**
         * @brief Returns reference to the last element.
         * @return Reference to last element.
         * @throws compile-time static string if the container is empty.
         */
        [[nodiscard]] constexpr auto back() -> T& {
            if (count == 0) {
                throw "FixedCapacityVector: back() on empty vector.";
            }
            return data[count - 1];
        }

        /** @brief Returns reference to the last element. */
        [[nodiscard]] constexpr auto back() const -> T const& {
            if (count == 0) {
                throw "FixedCapacityVector: back() on empty vector.";
            }
            return data[count - 1];
        }

        /**
         * @brief Returns reference to the element at a given index with bounds checking.
         * @param index Element index.
         * @return Reference to element at index.
         * @throws compile-time static string if index is out of range.
         */
        [[nodiscard]] constexpr auto at(size_t index) -> T& {
            if (index >= count) {
                throw "FixedCapacityVector: index out of range.";
            }
            return data[index];
        }

        /** @brief Returns reference to the element at a given index with bounds checking. */
        [[nodiscard]] constexpr auto at(size_t index) const -> T const& {
            if (index >= count) {
                throw "FixedCapacityVector: index out of range.";
            }
            return data[index];
        }

        /**
         * @brief Returns const iterator to the first element.
         * @return Const begin iterator.
         */
        [[nodiscard]] constexpr auto cbegin() const noexcept { return data.cbegin(); }

        /**
         * @brief Returns const iterator one past the last valid element.
         * @return Const end iterator.
         */
        [[nodiscard]] constexpr auto cend() const noexcept { return data.cbegin() + count; }

        /**
         * @brief Returns iterator to the first element.
         * @return Begin iterator.
         */
        [[nodiscard]] constexpr auto begin() noexcept { return data.begin(); }

        /** @brief Returns const iterator to the first element. */
        [[nodiscard]] constexpr auto begin() const noexcept { return data.begin(); }

        /**
         * @brief Returns iterator one past the last valid element.
         * @return End iterator.
         */
        [[nodiscard]] constexpr auto end() noexcept { return data.begin() + count; }

        /** @brief Returns const iterator one past the last valid element. */
        [[nodiscard]] constexpr auto end() const noexcept { return data.begin() + count; }

        /**
         * @brief Returns reference to the element at index without bounds checking.
         * @param index Element index.
         * @return Reference to element at index.
         */
        [[nodiscard]] constexpr auto operator[](size_t index) noexcept -> T& { return data[index]; }

        /** @brief Returns reference to the element at index without bounds checking. */
        [[nodiscard]] constexpr auto operator[](size_t index) const noexcept -> T const& { return data[index]; }
    };

} // namespace Sora

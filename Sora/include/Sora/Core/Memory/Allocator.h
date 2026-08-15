/**
 * @file Allocator.h
 * @brief General-purpose Sora allocation primitives and standard allocator adapter.
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/Memory/MemoryLayout.h>

#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>

namespace Sora {

    /**
     * @brief Allocate uninitialized storage from Sora's general-purpose allocator.
     * @param[in] bytes Number of bytes requested. Zero is permitted.
     * @param[in] alignment Minimum byte alignment of the returned storage.
     * @return Allocated storage, or @c nullptr when the allocation cannot be satisfied.
     * @note The allocation must be released by a matching @ref DeallocateMemory overload.
     */
    [[nodiscard]] void* AllocateMemory(size_t bytes, Align alignment = Align::Of<std::max_align_t>()) noexcept;

    /** @brief Release storage returned by @ref AllocateMemory without supplying its layout. */
    void DeallocateMemory(void* allocation) noexcept;

    /**
     * @brief Release storage returned by @ref AllocateMemory using its original layout.
     * @param[in] allocation Storage to release; @c nullptr is permitted.
     * @param[in] bytes Original requested byte count.
     * @param[in] alignment Original requested alignment.
     */
    void DeallocateMemory(void* allocation, size_t bytes, Align alignment) noexcept;

    /**
     * @brief Stateless standard allocator backed by Sora's general-purpose allocator.
     * @tparam T Complete cv-unqualified object type allocated by this adapter.
     * @details All instances share one allocation domain and compare equal. The adapter does not replace global
     * @c new or @c delete; containers use it only when selected explicitly.
    */
    template<typename T>
        requires(std::is_object_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T>)
    class Allocator {
    public:
        using value_type = T;
        using size_type = size_t;
        using difference_type = std::ptrdiff_t;
        using is_always_equal = std::true_type;

        constexpr Allocator() noexcept = default;

        template<typename U>
        constexpr Allocator(const Allocator<U>&) noexcept {}

        /** @brief Allocate uninitialized storage for @p count objects. */
        [[nodiscard]] T* allocate(size_t count) {
            if (count > MaxSize()) [[unlikely]] {
                throw std::bad_array_new_length{};
            }

            void* allocation = AllocateMemory(count * sizeof(T), Align::Of<T>());
            if (!allocation) [[unlikely]] {
                throw std::bad_alloc{};
            }
            return static_cast<T*>(allocation);
        }

        /** @brief Release storage previously returned by @ref allocate. */
        void deallocate(T* allocation, size_t count) noexcept {
            DeallocateMemory(allocation, count * sizeof(T), Align::Of<T>());
        }

        [[nodiscard]] static constexpr size_t MaxSize() noexcept {
            return std::numeric_limits<size_t>::max() / sizeof(T);
        }
    };

    template<typename T, typename U>
    [[nodiscard]] constexpr bool operator==(const Allocator<T>&, const Allocator<U>&) noexcept {
        return true;
    }

} // namespace Sora

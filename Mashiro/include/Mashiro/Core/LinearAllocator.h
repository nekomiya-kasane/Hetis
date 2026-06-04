/**
 * @file LinearAllocator.h
 * @brief Bump/linear allocator for arena-style memory management.
 *
 * `LinearAllocator` owns a contiguous memory block and provides O(1)
 * allocation by advancing a pointer. Individual deallocations are not
 * supported — the entire arena is freed at once via `Reset()`, or partially
 * via `Restore(savepoint)`.
 *
 * Features:
 * - **O(1) allocation** — single pointer bump + alignment adjustment.
 * - **Savepoint / ScopeGuard** — `Save()` captures current offset;
 *   `Restore()` rewinds. `MakeScope()` returns an RAII guard that restores
 *   on destruction.
 * - **Page-aligned backing** — runtime uses `_aligned_malloc` / `aligned_alloc`
 *   for TLB-friendly large arenas.
 * - **constexpr capable** — C++26 `constexpr new` allows compile-time use.
 * - **Debug statistics** — peak usage + allocation count tracked in debug builds.
 *
 * Primary use case: per-frame / per-graph temporary storage where many
 * small, short-lived allocations are made and then discarded together.
 *
 * @ingroup Core
 */
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace Mashiro {

    // =========================================================================
    // Savepoint
    // =========================================================================

    /// @brief Opaque position marker within a LinearAllocator.
    struct ArenaSavepoint {
        size_t offset = 0; ///< Byte offset at the time of Save().
    };

    // =========================================================================
    // LinearAllocator
    // =========================================================================

    /**
     * @brief Bump allocator with a single contiguous backing buffer.
     *
     * Thread-safety: None. Intended for single-threaded graph construction
     * or per-thread arenas.
     */
    class LinearAllocator {
    public:
        /// @name Construction / destruction
        /// @{

        /**
         * @brief Construct with a given capacity in bytes.
         *
         * At runtime, the backing buffer is page-aligned for optimal TLB usage.
         * In constexpr context, uses regular `new`.
         *
         * @param capacityBytes Size of the arena in bytes.
         */
        constexpr explicit LinearAllocator(size_t capacityBytes) : capacity_(capacityBytes) {
            if consteval {
                buffer_ = new std::byte[capacityBytes];
            } else {
                buffer_ = static_cast<std::byte*>(
                    ::operator new(capacityBytes, std::align_val_t{kPageSize}));
            }
        }

        constexpr ~LinearAllocator() {
            if consteval {
                delete[] buffer_;
            } else {
                ::operator delete(buffer_, std::align_val_t{kPageSize});
            }
        }

        LinearAllocator(const LinearAllocator&) = delete;
        LinearAllocator& operator=(const LinearAllocator&) = delete;

        constexpr LinearAllocator(LinearAllocator&& other) noexcept
            : buffer_(other.buffer_), capacity_(other.capacity_), offset_(other.offset_) {
#ifndef NDEBUG
            stats_ = other.stats_;
#endif
            other.buffer_ = nullptr;
            other.capacity_ = 0;
            other.offset_ = 0;
        }

        constexpr LinearAllocator& operator=(LinearAllocator&& other) noexcept {
            if (this != &other) {
                if consteval {
                    delete[] buffer_;
                } else {
                    ::operator delete(buffer_, std::align_val_t{kPageSize});
                }
                buffer_ = other.buffer_;
                capacity_ = other.capacity_;
                offset_ = other.offset_;
#ifndef NDEBUG
                stats_ = other.stats_;
#endif
                other.buffer_ = nullptr;
                other.capacity_ = 0;
                other.offset_ = 0;
            }
            return *this;
        }

        /// @}

        /// @name Allocation
        /// @{

        /**
         * @brief Allocate raw bytes with the given alignment.
         * @param bytes Number of bytes to allocate.
         * @param alignment Required alignment (must be power of 2).
         * @return Pointer to allocated memory, or nullptr if exhausted.
         */
        [[nodiscard]] constexpr void*
        Allocate(size_t bytes, size_t alignment = alignof(std::max_align_t)) noexcept {
            size_t aligned = AlignUp(offset_, alignment);
            if (aligned + bytes > capacity_) [[unlikely]] {
                return nullptr;
            }
            void* ptr = buffer_ + aligned;
            offset_ = aligned + bytes;
#ifndef NDEBUG
            ++stats_.allocationCount;
            if (offset_ > stats_.peakUsage) {
                stats_.peakUsage = offset_;
            }
#endif
            return ptr;
        }

        /**
         * @brief Allocate storage for N objects of type T (uninitialized).
         * @tparam T Element type.
         * @param count Number of elements.
         * @return Span over the allocated region, or empty span on failure.
         */
        template<typename T>
        [[nodiscard]] constexpr std::span<T> AllocateArray(size_t count) noexcept {
            if (count == 0) {
                return {};
            }
            void* ptr = Allocate(count * sizeof(T), alignof(T));
            if (!ptr) {
                return {};
            }
            return {static_cast<T*>(ptr), count};
        }

        /**
         * @brief Construct a single object of type T in arena memory.
         * @tparam T Object type.
         * @tparam Args Constructor argument types.
         * @return Pointer to the constructed object, or nullptr on OOM.
         */
        template<typename T, typename... Args>
        [[nodiscard]] constexpr T* Emplace(Args&&... args) {
            void* ptr = Allocate(sizeof(T), alignof(T));
            if (!ptr) return nullptr;
            return ::new (ptr) T(std::forward<Args>(args)...);
        }

        /**
         * @brief Copy a range of trivially-copyable elements into the arena.
         * @return Span over the arena copy, or empty span on failure.
         */
        template<typename T>
        [[nodiscard]] constexpr std::span<T> CopyToArena(std::span<const T> src) noexcept
            requires std::is_trivially_copyable_v<T>
        {
            auto dst = AllocateArray<T>(src.size());
            if (!dst.empty()) {
                if consteval {
                    for (size_t i = 0; i < src.size(); ++i)
                        dst[i] = src[i];
                } else {
                    std::memcpy(dst.data(), src.data(), src.size_bytes());
                }
            }
            return dst;
        }

        /// @}

        /// @name Savepoint / scope
        /// @{

        /// @brief Capture the current allocation offset as a savepoint.
        [[nodiscard]] constexpr ArenaSavepoint Save() const noexcept {
            return ArenaSavepoint{offset_};
        }

        /// @brief Restore the allocator to a previous savepoint (rewind).
        /// @pre `savepoint.offset <= offset_` (cannot roll forward).
        constexpr void Restore(ArenaSavepoint savepoint) noexcept {
            assert(savepoint.offset <= offset_);
            offset_ = savepoint.offset;
        }

        /**
         * @brief RAII scope guard that restores to the savepoint on destruction.
         *
         * @code
         * {
         *     auto scope = allocator.MakeScope();
         *     auto* tmp = allocator.Emplace<Foo>(...);
         *     // ... use tmp ...
         * } // automatically restored here
         * @endcode
         */
        class [[nodiscard]] ScopeGuard {
        public:
            constexpr explicit ScopeGuard(LinearAllocator& alloc) noexcept
                : allocator_(&alloc), mark_(alloc.Save()) {}

            constexpr ~ScopeGuard() {
                if (allocator_) allocator_->Restore(mark_);
            }

            ScopeGuard(const ScopeGuard&) = delete;
            ScopeGuard& operator=(const ScopeGuard&) = delete;

            constexpr ScopeGuard(ScopeGuard&& other) noexcept
                : allocator_(other.allocator_), mark_(other.mark_) {
                other.allocator_ = nullptr;
            }

            ScopeGuard& operator=(ScopeGuard&&) = delete;

            /// @brief Release the guard without restoring (commit allocations).
            constexpr void Release() noexcept { allocator_ = nullptr; }

        private:
            LinearAllocator* allocator_;
            ArenaSavepoint mark_;
        };

        /// @brief Create an RAII scope that auto-restores on destruction.
        [[nodiscard]] constexpr ScopeGuard MakeScope() noexcept { return ScopeGuard{*this}; }

        /// @}

        /// @name Reset
        /// @{

        /// @brief Reset the allocator, reclaiming all memory. Does NOT call destructors.
        constexpr void Reset() noexcept { offset_ = 0; }

        /// @}

        /// @name Queries
        /// @{

        /// @brief Number of bytes currently allocated.
        [[nodiscard]] constexpr size_t GetUsedBytes() const noexcept { return offset_; }

        /// @brief Total arena capacity in bytes.
        [[nodiscard]] constexpr size_t GetCapacity() const noexcept { return capacity_; }

        /// @brief Remaining unallocated bytes.
        [[nodiscard]] constexpr size_t GetRemainingBytes() const noexcept {
            return capacity_ - offset_;
        }

        /// @brief Pointer to the start of the backing buffer.
        [[nodiscard]] constexpr std::byte* GetBuffer() noexcept { return buffer_; }
        [[nodiscard]] constexpr const std::byte* GetBuffer() const noexcept { return buffer_; }

        /// @}

        /// @name Debug statistics (debug builds only)
        /// @{
#ifndef NDEBUG
        /// @brief Debug statistics (only available in debug builds).
        struct Stats {
            size_t peakUsage = 0;    ///< High-water mark of bytes used.
            uint32_t allocationCount = 0; ///< Total number of Allocate() calls.
        };

        /// @brief Get current debug statistics.
        [[nodiscard]] constexpr const Stats& GetStats() const noexcept { return stats_; }

        /// @brief Reset debug statistics.
        constexpr void ResetStats() noexcept { stats_ = {}; }
#endif
        /// @}

    private:
        static constexpr size_t kPageSize = 4096;

        [[nodiscard]] static constexpr size_t AlignUp(size_t value,
                                                           size_t alignment) noexcept {
            return (value + alignment - 1) & ~(alignment - 1);
        }

        std::byte* buffer_ = nullptr;
        size_t capacity_ = 0;
        size_t offset_ = 0;

#ifndef NDEBUG
        Stats stats_{};
#endif
    };

} // namespace Mashiro

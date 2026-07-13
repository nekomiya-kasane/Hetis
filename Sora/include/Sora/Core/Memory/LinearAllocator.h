/**
 * @file LinearAllocator.h
 * @brief Provide a constexpr-capable linear allocator for arena-style memory management.
 * @details @ref Sora::LinearAllocator owns contiguous storage and performs constant-time allocation by alignment and
 * advancing an offset. It does not support individual deallocation; @ref Sora::LinearAllocator::Reset reclaims the
 * entire arena, while @ref Sora::LinearAllocator::Restore rewinds it to a captured savepoint. Runtime storage is
 * page-aligned, and debug builds track peak usage and allocation count. This allocator is intended for per-frame,
 * per-graph, or per-thread temporary storage whose allocations share a common lifetime.
 * @ingroup Core
 */
#pragma once

#include <Sora/Core/Memory/MemoryLayout.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace Sora {

    /** @brief Represent an opaque allocation position captured from a @ref LinearAllocator. */
    struct ArenaSavepoint {
        size_t offset = 0; /**< Byte offset captured by @ref LinearAllocator::Save. */
    };

    /**
     * @brief Bump allocator with a single contiguous backing buffer.
     * @details Allocation advances a byte offset and has constant complexity. Restoring or resetting the arena does not
     * invoke destructors for stored objects. Instances are not thread-safe and should be confined to one thread or
     * externally synchronized.
     */
    class LinearAllocator {
    public:
        /** @name Construction and Destruction @{ ------------------------------------------------------------------- */

        /**
         * @brief Construct an allocator with the requested byte capacity.
         * @details Runtime allocation uses page alignment, while constant evaluation uses a regular array allocation.
         * @param[in] capacityBytes Arena capacity in bytes.
         */
        constexpr explicit LinearAllocator(size_t capacityBytes) : capacity_(capacityBytes) {
            if consteval {
                buffer_ = new std::byte[capacityBytes];
            } else {
                buffer_ = static_cast<std::byte*>(::operator new(capacityBytes, std::align_val_t{kPageSize}));
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
            if (this == &other) {
                return *this;
            }

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
            return *this;
        }

        /** @} ------------------------------------------------------------------------------------------------------ */

        /** @name Allocation @{ ------------------------------------------------------------------------------------- */

        /**
         * @brief Allocate raw bytes with the requested alignment.
         * @param[in] bytes Number of bytes to allocate.
         * @param[in] alignment Required power-of-two alignment.
         * @return Pointer to the allocated storage, or @c nullptr when the remaining capacity is insufficient.
         */
        [[nodiscard]] constexpr void* Allocate(size_t bytes, size_t alignment = alignof(std::max_align_t)) noexcept {
            size_t aligned = Sora::AlignUpModulo(offset_, Align{alignment});
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
         * @brief Allocate uninitialized storage for @p count objects of type @p T.
         * @tparam T Element type stored in the allocated region.
         * @param[in] count Number of elements to allocate.
         * @return Span over the allocated region, or an empty span when allocation fails or @p count is zero.
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
         * @brief Construct one object of type @p T in arena storage.
         * @tparam T Object type to construct.
         * @tparam Args Constructor argument types.
         * @param[in] args Arguments forwarded to the constructor of @p T.
         * @return Pointer to the constructed object, or @c nullptr when allocation fails.
         */
        template<typename T, typename... Args>
        [[nodiscard]] constexpr T* Emplace(Args&&... args) {
            void* ptr = Allocate(sizeof(T), alignof(T));
            if (!ptr) {
                return nullptr;
            }
            return ::new (ptr) T(std::forward<Args>(args)...);
        }

        /**
         * @brief Copy a span of trivially copyable elements into arena storage.
         * @tparam T Trivially copyable element type.
         * @param[in] src Elements to copy.
         * @return Span over the copied elements, or an empty span when allocation fails or @p src is empty.
         */
        template<typename T>
        [[nodiscard]] constexpr std::span<T> CopyToArena(std::span<const T> src) noexcept
            requires std::is_trivially_copyable_v<T>
        {
            auto dst = AllocateArray<T>(src.size());
            if (!dst.empty()) {
                if consteval {
                    for (size_t i = 0; i < src.size(); ++i) {
                        dst[i] = src[i];
                    }
                } else {
                    std::memcpy(dst.data(), src.data(), src.size_bytes());
                }
            }
            return dst;
        }

        /** @} ------------------------------------------------------------------------------------------------------ */

        /** @name Savepoints and Scopes @{ -------------------------------------------------------------------------- */

        /** @brief Capture the current allocation offset as a savepoint. */
        [[nodiscard]] constexpr ArenaSavepoint Save() const noexcept { return ArenaSavepoint{offset_}; }

        /**
         * @brief Rewind the allocator to a previously captured savepoint.
         * @param[in] savepoint Position to restore.
         * @pre @p savepoint belongs to this allocator and its offset does not exceed the current allocation offset.
         */
        constexpr void Restore(ArenaSavepoint savepoint) noexcept {
            assert(savepoint.offset <= offset_);
            offset_ = savepoint.offset;
        }

        /**
         * @brief Restore an allocator to a captured savepoint when the guard is destroyed.
         * @details Call @ref Release to retain allocations made while the guard is active.
         * @code{.cpp}
         * {
         *     auto scope = allocator.MakeScope();
         *     auto* tmp = allocator.Emplace<Foo>(...);
         *     Use(tmp);
         * }
         * @endcode
         */
        class [[nodiscard]] ScopeGuard {
        public:
            constexpr explicit ScopeGuard(LinearAllocator& alloc) noexcept : allocator_(&alloc), mark_(alloc.Save()) {}

            constexpr ~ScopeGuard() {
                if (allocator_) {
                    allocator_->Restore(mark_);
                }
            }

            ScopeGuard(const ScopeGuard&) = delete;
            ScopeGuard& operator=(const ScopeGuard&) = delete;

            constexpr ScopeGuard(ScopeGuard&& other) noexcept : allocator_(other.allocator_), mark_(other.mark_) {
                other.allocator_ = nullptr;
            }

            ScopeGuard& operator=(ScopeGuard&&) = delete;

            /** @brief Release the guard without restoring its allocator. */
            constexpr void Release() noexcept { allocator_ = nullptr; }

        private:
            LinearAllocator* allocator_;
            ArenaSavepoint mark_;
        };

        /** @brief Create an RAII scope that restores the current allocation position upon destruction. */
        [[nodiscard]] constexpr ScopeGuard MakeScope() noexcept { return ScopeGuard{*this}; }

        /** @} ------------------------------------------------------------------------------------------------------ */

        /** @name Reset @{ ------------------------------------------------------------------------------------------ */

        /** @brief Reclaim all arena storage without invoking destructors for objects residing in it. */
        constexpr void Reset() noexcept { offset_ = 0; }

        /** @} ------------------------------------------------------------------------------------------------------ */

        /** @name Queries @{ ---------------------------------------------------------------------------------------- */

        /** @brief Return the number of bytes currently allocated. */
        [[nodiscard]] constexpr size_t GetUsedBytes() const noexcept { return offset_; }

        /** @brief Return the total arena capacity in bytes. */
        [[nodiscard]] constexpr size_t GetCapacity() const noexcept { return capacity_; }

        /** @brief Return the number of unallocated bytes remaining in the arena. */
        [[nodiscard]] constexpr size_t GetRemainingBytes() const noexcept { return capacity_ - offset_; }

        /** @brief Return a mutable pointer to the beginning of the backing buffer. */
        [[nodiscard]] constexpr std::byte* GetBuffer() noexcept { return buffer_; }

        /** @brief Return a read-only pointer to the beginning of the backing buffer. */
        [[nodiscard]] constexpr const std::byte* GetBuffer() const noexcept { return buffer_; }

        /** @} ------------------------------------------------------------------------------------------------------ */

        /** @name Debug Statistics @{ ------------------------------------------------------------------------------- */
#ifndef NDEBUG
        /** @brief Collect allocator usage statistics in debug builds. */
        struct Stats {
            size_t peakUsage = 0;         /**< Maximum simultaneously allocated byte count. */
            uint32_t allocationCount = 0; /**< Total number of calls to @ref Allocate. */
        };

        /** @brief Return the current debug statistics. */
        [[nodiscard]] constexpr const Stats& GetStats() const noexcept { return stats_; }

        /** @brief Reset all debug statistics to zero. */
        constexpr void ResetStats() noexcept { stats_ = {}; }
#endif
        /** @} ------------------------------------------------------------------------------------------------------ */

    private:
        static constexpr size_t kPageSize = 4096;

        std::byte* buffer_ = nullptr;
        size_t capacity_ = 0;
        size_t offset_ = 0;

#ifndef NDEBUG
        Stats stats_{};
#endif
    };

} // namespace Sora

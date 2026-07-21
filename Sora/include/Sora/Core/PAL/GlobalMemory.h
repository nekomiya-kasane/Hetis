/**
 * @file GlobalMemory.h
 * @brief Own, borrow, and lock platform movable global-memory blocks.
 * @ingroup PAL
 *
 * @details This facility models the Win32 movable global-memory protocol used by clipboard and OLE-style native
 * interfaces. It remains available to portable callers, but operations return @ref ErrorCode::NotSupported where the
 * target platform has no corresponding service.
 */
#pragma once

#include <Sora/ErrorCode.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace Sora::PAL {

    /** @brief Opaque native movable global-memory handle. */
    struct NativeGlobalMemory;

    /** @brief Type-safe handle exchanged with native APIs that transfer or borrow global memory. */
    using GlobalMemoryHandle = NativeGlobalMemory*;

    /** @brief Initialization policy applied while allocating a movable global-memory block. */
    enum class GlobalMemoryInitialization : uint8_t {
        Uninitialized, /**< Leave the allocated bytes uninitialized. */
        Zeroed,        /**< Initialize every allocated byte to zero. */
    };

    /** @brief Scoped mutable byte view obtained by locking a movable global-memory block. */
    class GlobalMemoryLock {
    public:
        /** @brief Construct an empty lock. */
        constexpr GlobalMemoryLock() noexcept = default;

        /** @brief Unlock the block when this object still owns a lock operation. */
        ~GlobalMemoryLock();

        GlobalMemoryLock(const GlobalMemoryLock&) = delete;
        GlobalMemoryLock& operator=(const GlobalMemoryLock&) = delete;

        /** @brief Transfer the lock operation from @p other. */
        GlobalMemoryLock(GlobalMemoryLock&& other) noexcept;

        /** @brief Unlock the current block and transfer the lock operation from @p other. */
        GlobalMemoryLock& operator=(GlobalMemoryLock&& other) noexcept;

        /** @brief Return the first mutable byte, or @c nullptr when empty. */
        [[nodiscard]] constexpr std::byte* Data() noexcept { return data_; }

        /** @brief Return the first immutable byte, or @c nullptr when empty. */
        [[nodiscard]] constexpr const std::byte* Data() const noexcept { return data_; }

        /** @brief Return all mutable locked bytes. */
        [[nodiscard]] constexpr std::span<std::byte> Bytes() noexcept { return {data_, size_}; }

        /** @brief Return all immutable locked bytes. */
        [[nodiscard]] constexpr std::span<const std::byte> Bytes() const noexcept { return {data_, size_}; }

        /** @brief Return the locked block size in bytes. */
        [[nodiscard]] constexpr size_t Size() const noexcept { return size_; }

        /** @brief Return whether this object owns no lock operation. */
        [[nodiscard]] constexpr bool Empty() const noexcept { return data_ == nullptr; }

        /** @brief Return whether this object owns a lock operation. */
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return !Empty(); }

        /** @brief Unlock explicitly so a native failure can be observed. */
        [[nodiscard]] VoidResult Unlock() noexcept;

        /** @brief Exchange lock ownership and byte views with @p other. */
        void Swap(GlobalMemoryLock& other) noexcept;

        /** @brief Exchange @p left and @p right. */
        friend void swap(GlobalMemoryLock& left, GlobalMemoryLock& right) noexcept { left.Swap(right); }

    private:
        friend class GlobalMemoryView;

        constexpr GlobalMemoryLock(GlobalMemoryHandle handle, std::byte* data, size_t size) noexcept
            : handle_{handle}, data_{data}, size_{size} {}

        void UnlockIgnoringError() noexcept;

        GlobalMemoryHandle handle_ = nullptr;
        std::byte* data_ = nullptr;
        size_t size_ = 0;
    };

    /** @brief Copyable non-owning reference to a native movable global-memory block. */
    class GlobalMemoryView {
    public:
        /** @brief Construct an empty view. */
        constexpr GlobalMemoryView() noexcept = default;

        /** @brief Borrow @p handle without assuming cleanup ownership. */
        [[nodiscard]] static constexpr GlobalMemoryView Borrow(GlobalMemoryHandle handle) noexcept {
            return GlobalMemoryView{handle};
        }

        /** @brief Return the borrowed native handle. */
        [[nodiscard]] constexpr GlobalMemoryHandle NativeHandle() const noexcept { return handle_; }

        /** @brief Return whether this view refers to no block. */
        [[nodiscard]] constexpr bool Empty() const noexcept { return handle_ == nullptr; }

        /** @brief Return whether this view refers to a block. */
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return !Empty(); }

        /** @brief Query the native allocation size in bytes. */
        [[nodiscard]] Result<size_t> Size() const noexcept;

        /**
         * @brief Lock the referenced block and expose its bytes.
         * @pre The referenced block remains valid until the returned lock is destroyed or explicitly unlocked.
         */
        [[nodiscard]] Result<GlobalMemoryLock> Lock() const noexcept;

    private:
        constexpr explicit GlobalMemoryView(GlobalMemoryHandle handle) noexcept : handle_{handle} {}

        GlobalMemoryHandle handle_ = nullptr;
    };

    /** @brief Move-only owner of a native movable global-memory block. */
    class OwnedGlobalMemory {
    public:
        /** @brief Construct an empty owner. */
        constexpr OwnedGlobalMemory() noexcept = default;

        /** @brief Free the block when this object still owns it. */
        ~OwnedGlobalMemory();

        OwnedGlobalMemory(const OwnedGlobalMemory&) = delete;
        OwnedGlobalMemory& operator=(const OwnedGlobalMemory&) = delete;

        /** @brief Transfer ownership from @p other. */
        OwnedGlobalMemory(OwnedGlobalMemory&& other) noexcept;

        /** @brief Free the current block and transfer ownership from @p other. */
        OwnedGlobalMemory& operator=(OwnedGlobalMemory&& other) noexcept;

        /**
         * @brief Allocate a movable global-memory block containing @p size bytes.
         * @param size Non-zero allocation size in bytes.
         * @param initialization Initial byte-value policy.
         */
        [[nodiscard]] static Result<OwnedGlobalMemory>
        Allocate(size_t size,
                 GlobalMemoryInitialization initialization = GlobalMemoryInitialization::Uninitialized) noexcept;

        /**
         * @brief Adopt exclusive cleanup ownership of @p handle.
         * @pre @p handle is null or denotes a movable global-memory block whose ownership has been transferred.
         */
        [[nodiscard]] static constexpr OwnedGlobalMemory Adopt(GlobalMemoryHandle handle) noexcept {
            return OwnedGlobalMemory{handle};
        }

        /** @brief Return the owned native handle. */
        [[nodiscard]] constexpr GlobalMemoryHandle NativeHandle() const noexcept { return handle_; }

        /** @brief Borrow the owned block without changing ownership. */
        [[nodiscard]] constexpr GlobalMemoryView View() const noexcept { return GlobalMemoryView::Borrow(handle_); }

        /** @brief Return whether this object owns no block. */
        [[nodiscard]] constexpr bool Empty() const noexcept { return handle_ == nullptr; }

        /** @brief Return whether this object owns a block. */
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return !Empty(); }

        /** @brief Query the owned allocation size in bytes. */
        [[nodiscard]] Result<size_t> Size() const noexcept { return View().Size(); }

        /**
         * @brief Lock the owned block and expose its bytes.
         * @pre This owner is not moved, reset, released, or destroyed before the returned lock is released.
         */
        [[nodiscard]] Result<GlobalMemoryLock> Lock() const noexcept { return View().Lock(); }

        /** @brief Relinquish cleanup ownership and return the native handle. */
        [[nodiscard]] GlobalMemoryHandle Release() noexcept;

        /**
         * @brief Free the owned block and restore the empty state.
         * @return Success, or a native failure while retaining ownership for a later retry.
         */
        [[nodiscard]] VoidResult Reset() noexcept;

        /** @brief Exchange ownership with @p other. */
        void Swap(OwnedGlobalMemory& other) noexcept;

        /** @brief Exchange @p left and @p right. */
        friend void swap(OwnedGlobalMemory& left, OwnedGlobalMemory& right) noexcept { left.Swap(right); }

    private:
        constexpr explicit OwnedGlobalMemory(GlobalMemoryHandle handle) noexcept : handle_{handle} {}

        GlobalMemoryHandle handle_ = nullptr;
    };

} // namespace Sora::PAL

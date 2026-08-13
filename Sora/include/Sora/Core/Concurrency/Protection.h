/**
 * @file Protection.h
 * @brief Move-only protected observations returned by safe-reclamation protocols.
 * @ingroup Core
 */
#pragma once

#include <Sora/Platform.h>

#include <cassert>
#include <type_traits>
#include <utility>

namespace Sora::Concurrency {

    /** @brief A pointer whose dereference lifetime is coupled to a move-only protection object. */
    template<typename T, typename Protection>
    class ProtectedPointer {
        static_assert(std::is_nothrow_move_constructible_v<Protection> &&
                      std::is_nothrow_move_assignable_v<Protection>,
                      "a protection capability must transfer without failure");

    public:
        ProtectedPointer() noexcept = default;
        ProtectedPointer(T* pointer, Protection protection) noexcept
            : pointer_(pointer), protection_(std::move(protection)) {}

        ProtectedPointer(const ProtectedPointer&) = delete;
        ProtectedPointer& operator=(const ProtectedPointer&) = delete;
        ProtectedPointer(ProtectedPointer&& other) noexcept
            : pointer_(std::exchange(other.pointer_, nullptr)), protection_(std::move(other.protection_)) {}
        ProtectedPointer& operator=(ProtectedPointer&& other) noexcept {
            if (this != &other) {
                pointer_ = std::exchange(other.pointer_, nullptr);
                protection_ = std::move(other.protection_);
            }
            return *this;
        }

        [[nodiscard]] T* Get() const noexcept { return pointer_; }
        [[nodiscard]] T& operator*() const noexcept {
            assert(pointer_ != nullptr);
            return *pointer_;
        }
        [[nodiscard]] T* operator->() const noexcept {
            assert(pointer_ != nullptr);
            return pointer_;
        }
        [[nodiscard]] explicit operator bool() const noexcept { return pointer_ != nullptr; }

    private:
        T* pointer_ = nullptr;
        SORA_NO_UNIQUE_ADDRESS Protection protection_;
    };

    /** @brief A stamped pointer snapshot whose pointee remains protected for the observation lifetime. */
    template<typename Stamped, typename Protection>
        requires std::is_pointer_v<typename Stamped::ValueType>
    class ProtectedStampedValue {
        static_assert(std::is_nothrow_move_constructible_v<Protection> &&
                      std::is_nothrow_move_assignable_v<Protection>,
                      "a protection capability must transfer without failure");

    public:
        using ElementType = std::remove_pointer_t<typename Stamped::ValueType>;

        ProtectedStampedValue() noexcept = default;
        ProtectedStampedValue(Stamped snapshot, Protection protection) noexcept
            : snapshot_(snapshot), protection_(std::move(protection)) {}

        ProtectedStampedValue(const ProtectedStampedValue&) = delete;
        ProtectedStampedValue& operator=(const ProtectedStampedValue&) = delete;
        ProtectedStampedValue(ProtectedStampedValue&& other) noexcept
            : snapshot_(std::exchange(other.snapshot_, {})), protection_(std::move(other.protection_)) {}
        ProtectedStampedValue& operator=(ProtectedStampedValue&& other) noexcept {
            if (this != &other) {
                snapshot_ = std::exchange(other.snapshot_, {});
                protection_ = std::move(other.protection_);
            }
            return *this;
        }

        [[nodiscard]] Stamped Snapshot() const noexcept { return snapshot_; }
        [[nodiscard]] ElementType* Get() const noexcept { return snapshot_.Value(); }
        [[nodiscard]] ElementType& operator*() const noexcept {
            assert(Get() != nullptr);
            return *Get();
        }
        [[nodiscard]] ElementType* operator->() const noexcept {
            assert(Get() != nullptr);
            return Get();
        }
        [[nodiscard]] explicit operator bool() const noexcept { return Get() != nullptr; }

    private:
        Stamped snapshot_{};
        SORA_NO_UNIQUE_ADDRESS Protection protection_;
    };

} // namespace Sora::Concurrency

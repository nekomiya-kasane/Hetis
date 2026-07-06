/**
 * @file RefPtr.h
 * @brief Non-owning nullable pointer wrapper.
 * @ingroup Core
 */
#pragma once

#include <compare>
#include <concepts>
#include <cstddef>
#include <memory>
#include <type_traits>

namespace Sora {

    /**
     * @brief Non-owning nullable pointer wrapper.
     *
     * @details @ref RefPtr stores a raw pointer and never participates in object lifetime. It is intended for APIs
     * that want to make borrowed pointer semantics explicit without changing representation, ownership, or runtime
     * cost. Destroying, copying, moving, or resetting a @ref RefPtr never deletes, retains, releases, or otherwise
     * touches the pointed-to object.
     *
     * @tparam T Referenced object type.
     */
    template<typename T>
    class RefPtr {
        static_assert(!std::is_reference_v<T>,
                      "RefPtr<T> requires T to be an object or function type, not a reference.");

    public:
        /** @brief Referenced element type. */
        using ElementType = T;

        /** @brief Stored pointer type. */
        using Pointer = T*;

        /** @name Constructors */
        /** @{ */

        /** @brief Construct an empty pointer. */
        constexpr RefPtr() noexcept = default;

        /** @brief Construct an empty pointer from null. */
        constexpr RefPtr(std::nullptr_t) noexcept {}

        /** @brief Construct from a borrowed raw pointer. */
        constexpr explicit RefPtr(Pointer pointer) noexcept : pointer_(pointer) {}

        /** @brief Construct from a statically compatible borrowed raw pointer. */
        template<typename U>
            requires(!std::same_as<U, T> && std::convertible_to<U*, T*>)
        constexpr explicit RefPtr(U* pointer) noexcept : pointer_(pointer) {}

        /** @brief Convert from a statically compatible borrowed pointer wrapper. */
        template<typename U>
            requires(!std::same_as<U, T> && std::convertible_to<U*, T*>)
        constexpr RefPtr(RefPtr<U> other) noexcept : pointer_(other.Get()) {}

        /** @} */

        /** @name Assignment */
        /** @{ */

        /** @brief Reset to null. */
        constexpr RefPtr& operator=(std::nullptr_t) noexcept {
            pointer_ = nullptr;
            return *this;
        }

        /** @brief Rebind to a borrowed raw pointer. */
        constexpr RefPtr& operator=(Pointer pointer) noexcept {
            pointer_ = pointer;
            return *this;
        }

        /** @brief Rebind to a statically compatible borrowed raw pointer. */
        template<typename U>
            requires(!std::same_as<U, T> && std::convertible_to<U*, T*>)
        constexpr RefPtr& operator=(U* pointer) noexcept {
            pointer_ = pointer;
            return *this;
        }

        /** @brief Rebind from a statically compatible borrowed pointer wrapper. */
        template<typename U>
            requires(!std::same_as<U, T> && std::convertible_to<U*, T*>)
        constexpr RefPtr& operator=(RefPtr<U> other) noexcept {
            pointer_ = other.Get();
            return *this;
        }

        /** @} */

        /** @name Observers */
        /** @{ */

        /** @brief Return the borrowed raw pointer. */
        [[nodiscard]] constexpr Pointer Get() const noexcept { return pointer_; }

        /** @brief Return whether this pointer is non-null. */
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return pointer_ != nullptr; }

        /** @brief Dereference the borrowed pointer. The pointer must be non-null. */
        [[nodiscard]] constexpr decltype(auto) operator*() const noexcept
            requires(!std::is_void_v<T>)
        {
            return *pointer_;
        }

        /** @brief Access the borrowed pointer. */
        [[nodiscard]] constexpr Pointer operator->() const noexcept { return pointer_; }

        /** @} */

        /** @name Mutation */
        /** @{ */

        /** @brief Rebind to @p pointer. */
        constexpr void Reset(Pointer pointer = nullptr) noexcept { pointer_ = pointer; }

        /** @brief Swap two borrowed pointer wrappers. */
        constexpr void Swap(RefPtr& other) noexcept {
            Pointer tmp = pointer_;
            pointer_ = other.pointer_;
            other.pointer_ = tmp;
        }

        /** @} */

        /** @name Comparison */
        /** @{ */

        /** @brief Compare two borrowed pointer wrappers by address. */
        [[nodiscard]] constexpr auto operator<=>(const RefPtr&) const noexcept = default;

        /** @brief Compare this borrowed pointer with null. */
        [[nodiscard]] constexpr bool operator==(std::nullptr_t) const noexcept { return pointer_ == nullptr; }

        /** @} */

    private:
        Pointer pointer_{};
    };

    /** @brief Deduction guide for raw pointers. */
    template<typename T>
    RefPtr(T*) -> RefPtr<T>;

    /** @brief Construct a borrowed pointer wrapper from an lvalue reference. */
    template<typename T>
    [[nodiscard]] constexpr auto Ref(T& object) noexcept {
        if constexpr (std::is_pointer_v<T>) {
            return RefPtr<std::remove_pointer_t<T>>{object};
        } else {
            return RefPtr<T>{std::addressof(object)};
        }
    }

    /** @brief Swap two borrowed pointer wrappers. */
    template<typename T>
    constexpr void Swap(RefPtr<T>& lhs, RefPtr<T>& rhs) noexcept {
        lhs.Swap(rhs);
    }

} // namespace Sora

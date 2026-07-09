/**
 * @file RefPtr.h
 * @brief Non-owning observer pointer with identity semantics.
 * @ingroup Core
 *
 * @details @ref Sora::RefPtr is a value-semantic wrapper over a raw pointer. It makes borrowed object references
 * explicit in APIs without changing representation, ownership, or runtime cost. Destroying, copying, moving,
 * assigning, or resetting a @ref RefPtr never deletes, retains, releases, or otherwise touches the observed object.
 *
 * The abstraction deliberately models pointer identity, not pointee value. Comparison, ordering, string rendering,
 * JSON rendering, and hashing operate on the address stored in the wrapper and never dereference the pointee. This
 * keeps diagnostics safe for null or dangling observers and avoids accidental recursive traversal of object graphs.
 *
 * Constness is pointer-like and does not propagate to the pointee: a @c const @c RefPtr<T> behaves like @c T* const.
 * To observe a const object, spell @c RefPtr<const T>.
 */
#pragma once

#include <array>
#include <bit>
#include <compare>
#include <concepts>
#include <cstddef>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <type_traits>

#include "Sora/Core/Traits/TypeTraits.h"

namespace Sora::Hook {

    template<typename T>
    struct ToStringHook;

} // namespace Sora::Hook

namespace Sora {

    template<typename T>
    class RefPtr;

    namespace Detail {

        /** @brief Convert an object or void pointer to a display-only address token. */
        template<typename T>
        [[nodiscard]] constexpr const void* AddressForDisplay(T* pointer) noexcept {
            return const_cast<const void*>(static_cast<const volatile void*>(pointer));
        }

    } // namespace Detail

    /**
     * @brief Non-owning nullable observer over a raw @c T*.
     *
     * @details The stored pointer is the complete state. @ref RefPtr is therefore trivially copyable,
     * standard-layout, and ABI-equivalent to @c T*. It is appropriate for back-references, parent links, cached
     * borrowed handles, and API parameters/returns that must communicate "borrowed, nullable, no lifetime transfer".
     *
     * @tparam T Observed object type. @c void is supported for opaque borrowed handles. Function types are rejected
     * because their addresses cannot be rendered through @c const @c void* portably.
     */
    template<typename T>
    class RefPtr {
        static_assert(std::is_object_v<T> || std::is_void_v<T>,
                      "RefPtr<T> requires T to be an object type or void.");

    public:
        /** @brief Observed element type. */
        using ElementType = T;

        /** @brief Stored raw pointer type, @c T*. */
        using Pointer = T*;

        /** @name Constructors @{ ------------------------------------------------------------ */

        /** @brief Construct an empty observer. */
        constexpr RefPtr() noexcept = default;

        /** @brief Construct an empty observer from @c nullptr. */
        constexpr RefPtr(std::nullptr_t) noexcept {}

        /** @brief Observe the object at @p pointer. No ownership is acquired. */
        constexpr explicit RefPtr(Pointer pointer) noexcept : pointer_(pointer) {}

        /**
         * @brief Construct from a statically compatible borrowed raw pointer.
         * @tparam U Source element type whose @c U* implicitly converts to @c T*.
         * @param[in] pointer Raw pointer to observe.
         */
        template<typename U>
            requires(!std::same_as<U, T> && std::convertible_to<U*, T*>)
        constexpr explicit RefPtr(U* pointer) noexcept : pointer_(pointer) {}

        /**
         * @brief Convert from a statically compatible borrowed pointer wrapper.
         * @tparam U Source element type whose @c U* implicitly converts to @c T*.
         * @param[in] other Source observer.
         */
        template<typename U>
            requires(!std::same_as<U, T> && std::convertible_to<U*, T*>)
        constexpr RefPtr(RefPtr<U> other) noexcept : pointer_(other.Get()) {}

        /** @} ------------------------------------------------------------------------------- */

        /** @name Assignment @{ -------------------------------------------------------------- */

        /** @brief Reset to null. */
        constexpr RefPtr& operator=(std::nullptr_t) noexcept {
            pointer_ = nullptr;
            return *this;
        }

        /** @brief Rebind to a borrowed raw pointer. No ownership is acquired. */
        constexpr RefPtr& operator=(Pointer pointer) noexcept {
            pointer_ = pointer;
            return *this;
        }

        /**
         * @brief Rebind to a statically compatible borrowed raw pointer.
         * @tparam U Source element type whose @c U* implicitly converts to @c T*.
         */
        template<typename U>
            requires(!std::same_as<U, T> && std::convertible_to<U*, T*>)
        constexpr RefPtr& operator=(U* pointer) noexcept {
            pointer_ = pointer;
            return *this;
        }

        /**
         * @brief Rebind from a statically compatible borrowed pointer wrapper.
         * @tparam U Source element type whose @c U* implicitly converts to @c T*.
         */
        template<typename U>
            requires(!std::same_as<U, T> && std::convertible_to<U*, T*>)
        constexpr RefPtr& operator=(RefPtr<U> other) noexcept {
            pointer_ = other.Get();
            return *this;
        }

        /** @} ------------------------------------------------------------------------------- */

        /** @name Observers @{ --------------------------------------------------------------- */

        /** @brief Return the observed raw pointer. The result may be null. */
        [[nodiscard]] constexpr Pointer Get() const noexcept { return pointer_; }

        /** @brief Return @c true iff this observer is non-null. */
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return pointer_ != nullptr; }

        /** @brief Explicit conversion to the observed raw pointer. */
        [[nodiscard]] constexpr explicit operator Pointer() const noexcept { return pointer_; }

        /** @brief Dereference the observed object. @pre The observer is non-null. */
        [[nodiscard]] constexpr std::add_lvalue_reference_t<T> operator*() const noexcept
            requires(!std::is_void_v<T>)
        {
            return *pointer_;
        }

        /** @brief Access the observed object. @pre The observer is non-null. */
        [[nodiscard]] constexpr Pointer operator->() const noexcept
            requires(!std::is_void_v<T>)
        {
            return pointer_;
        }

        /** @} ------------------------------------------------------------------------------- */

        /** @name Mutation @{ ---------------------------------------------------------------- */

        /** @brief Rebind to @p pointer. Does not free the previously observed object. */
        constexpr void Reset(Pointer pointer = nullptr) noexcept { pointer_ = pointer; }

        /** @brief Swap observed addresses with @p other. */
        constexpr void Swap(RefPtr& other) noexcept {
            Pointer tmp = pointer_;
            pointer_ = other.pointer_;
            other.pointer_ = tmp;
        }

        /** @} ------------------------------------------------------------------------------- */

        /** @name Comparison @{ -------------------------------------------------------------- */

        /** @brief Compare two observers by address equality. */
        [[nodiscard]] friend constexpr bool operator==(RefPtr lhs, RefPtr rhs) noexcept {
            return lhs.pointer_ == rhs.pointer_;
        }

        /** @brief Total order over observed addresses. */
        [[nodiscard]] friend constexpr std::strong_ordering operator<=>(RefPtr lhs, RefPtr rhs) noexcept {
            return std::compare_three_way{}(lhs.pointer_, rhs.pointer_);
        }

        /** @brief Compare this observer with null. */
        [[nodiscard]] friend constexpr bool operator==(RefPtr pointer, std::nullptr_t) noexcept {
            return pointer.pointer_ == nullptr;
        }

        /** @brief Compare this observer with a raw pointer of the same pointer type. */
        [[nodiscard]] friend constexpr bool operator==(RefPtr lhs, Pointer rhs) noexcept {
            return lhs.pointer_ == rhs;
        }

        /** @brief Order this observer against a raw pointer of the same pointer type. */
        [[nodiscard]] friend constexpr std::strong_ordering operator<=>(RefPtr lhs, Pointer rhs) noexcept {
            return std::compare_three_way{}(lhs.pointer_, rhs);
        }

        /**
         * @brief Compare compatible observers by address equality.
         * @tparam U Other observed element type.
         */
        template<typename U>
            requires requires(RefPtr lhs, RefPtr<U> rhs) { lhs.Get() == rhs.Get(); }
        [[nodiscard]] friend constexpr bool operator==(RefPtr lhs, RefPtr<U> rhs) noexcept {
            return lhs.Get() == rhs.Get();
        }

        /**
         * @brief Order compatible observers by address.
         * @tparam U Other observed element type accepted by @c std::compare_three_way.
         */
        template<typename U>
            requires requires(RefPtr lhs, RefPtr<U> rhs) { std::compare_three_way{}(lhs.Get(), rhs.Get()); }
        [[nodiscard]] friend constexpr auto operator<=>(RefPtr lhs, RefPtr<U> rhs) noexcept {
            return std::compare_three_way{}(lhs.Get(), rhs.Get());
        }

        /** @} ------------------------------------------------------------------------------- */

    private:
        Pointer pointer_{};
    };

    /** @brief Deduction guide: @c RefPtr(pointer) deduces @c RefPtr<T> from @c T*. */
    template<typename T>
    RefPtr(T*) -> RefPtr<T>;

    /** @brief Construct a borrowed pointer wrapper from a raw pointer. */
    template<typename T>
    [[nodiscard]] constexpr RefPtr<T> MakeRef(T* pointer) noexcept {
        return RefPtr<T>{pointer};
    }

    /**
     * @brief Construct a borrowed pointer wrapper from an lvalue reference or raw pointer lvalue.
     * @details Passing an object lvalue observes that object. Passing a raw pointer lvalue observes its pointee, not
     * the pointer variable itself.
     */
    template<typename T>
    [[nodiscard]] constexpr auto Ref(T& object) noexcept {
        if constexpr (std::is_pointer_v<std::remove_reference_t<T>>) {
            return RefPtr<std::remove_pointer_t<std::remove_cv_t<T>>>{object};
        } else {
            return RefPtr<T>{std::addressof(object)};
        }
    }

    /** @brief ADL swap for @ref RefPtr. */
    template<typename T>
    constexpr void Swap(RefPtr<T>& lhs, RefPtr<T>& rhs) noexcept {
        lhs.Swap(rhs);
    }

    /** @brief Standard lower-case ADL swap for @ref RefPtr. */
    template<typename T>
    constexpr void swap(RefPtr<T>& lhs, RefPtr<T>& rhs) noexcept {
        lhs.Swap(rhs);
    }

    static_assert(sizeof(RefPtr<int>) == sizeof(int*));
    static_assert(alignof(RefPtr<int>) == alignof(int*));
    static_assert(std::is_trivially_copyable_v<RefPtr<int>>);
    static_assert(std::is_standard_layout_v<RefPtr<int>>);
    static_assert(sizeof(RefPtr<void>) == sizeof(void*));

    /**
     * @brief Hash hook for @ref RefPtr: hashes the observed address, never the pointee.
     * @tparam Algo Sora hash algorithm exposing @c ResultType and either stateless-call or stateful seed protocol.
     * @tparam T Observed element type.
     * @param[in] algo Hash algorithm object.
     * @param[in] pointer Observer whose address identity is hashed.
     * @return Hash of the pointer object's representation.
     */
    template<typename Algo, typename T>
        requires requires { typename Algo::ResultType; }
    [[nodiscard]] constexpr Algo::ResultType HashValue(const Algo& algo, RefPtr<T> pointer) noexcept {
        const auto raw = pointer.Get();
        const auto bytes = std::bit_cast<std::array<std::byte, sizeof(raw)>>(raw);
        const std::span<const std::byte> data{bytes};
        if constexpr (requires(const Algo& a, std::span<const std::byte> s) { a(s); }) {
            return algo(data);
        } else {
            auto state = Algo::Seed();
            state.Feed(data);
            return state.Finalize();
        }
    }

    namespace Traits {

        /** @brief Concept: @p T is a specialization of @ref Sora::RefPtr. */
        template<typename T>
        concept RefPtrType =
            Meta::IsSpecializationOf<RefPtr>(std::meta::dealias(std::meta::remove_cvref(^^T)));

        /** @brief The observed element type @c T of a @ref RefPtrType. */
        template<RefPtrType T>
        using RefPtrElement = typename std::remove_cvref_t<T>::ElementType;

    } // namespace Traits

} // namespace Sora

namespace Sora::Hook {

    /**
     * @brief String conversion hook for @ref Sora::RefPtr.
     * @tparam T Observed element type.
     */
    template<typename T>
    struct ToStringHook<RefPtr<T>> {
        /** @brief Render @p pointer as @c null or @c RefPtr<T>(0xADDR). */
        [[nodiscard]] static std::string ToString(RefPtr<T> pointer) {
            if (!pointer) {
                return "null";
            }
            return std::format("RefPtr<{}>({})", Traits::TypeName<T>, Detail::AddressForDisplay(pointer.Get()));
        }
    };

} // namespace Sora::Hook

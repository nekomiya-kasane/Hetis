/**
 * @file Box.h
 * @brief Always-engaged inline value storage for polymorphic representation roots.
 * @ingroup Core
 *
 * @details
 * `Box<T>` owns one `T` directly. It is deliberately not an optional value, an owning pointer, or a type-erased
 * wrapper. The class exists to give representation roots a named storage boundary while preserving empty-type
 * compression through `SORA_NO_UNIQUE_ADDRESS` and propagating `T`'s special-member properties unchanged.
 */
#pragma once

#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

#include <Sora/Platform.h>

namespace Sora::Polymorphism {

    /** @brief Construction tag for building a `Box<T>` from an invocation result without an intermediate value. */
    struct BoxFromFactoryTag {
        explicit constexpr BoxFromFactoryTag() noexcept = default;
    };

    inline constexpr BoxFromFactoryTag BoxFromFactory{};

    /**
     * @brief Always-engaged, directly owned value storage.
     * @tparam T Complete non-array object type stored in the box.
     *
     * @details
     * `Box<T>` has exactly the storage and special-member semantics of its `T` member, subject only to the
     * implementation's empty-type attribute. Use `std::in_place` or @ref BoxFromFactory when constructing a type that
     * is not movable or copyable. The factory form requires an exact result of `T`, preventing accidental narrowing or
     * conversion at a type-erasure boundary.
     */
    template<typename T>
        requires(std::is_object_v<T> && !std::is_array_v<T>)
    class Box {
    public:
        using ValueType = T;

        /** @name Constructors @{ ------------------------------------------------------------------------------ */

        /** @brief Default-construct the stored value when `T` is default-initializable. */
        constexpr Box()
            requires std::default_initializable<T>
        = default;

        /** @brief Construct `T` from an exact `T` value or reference. */
        template<typename Value>
            requires(std::same_as<std::remove_cvref_t<Value>, T> &&
                     !std::same_as<std::remove_cvref_t<Value>, Box>)
        constexpr explicit Box(Value&& value)
            noexcept(std::is_nothrow_constructible_v<T, Value&&>)
            : value_(std::forward<Value>(value)) {}

        /** @brief Construct `T` directly from @p args. */
        template<typename... Args>
            requires std::constructible_from<T, Args...>
        constexpr explicit Box(std::in_place_t, Args&&... args)
            noexcept(std::is_nothrow_constructible_v<T, Args...>)
            : value_(std::forward<Args>(args)...) {}

        /** @brief Invoke @p factory and construct `T` directly from its exact result. */
        template<typename Factory, typename... Args>
            requires(std::invocable<Factory&&, Args&&...> &&
                     std::same_as<std::invoke_result_t<Factory&&, Args&&...>, T>)
        constexpr explicit Box(BoxFromFactoryTag, Factory&& factory, Args&&... args)
            noexcept(std::is_nothrow_invocable_v<Factory&&, Args&&...>)
            : value_(std::invoke(std::forward<Factory>(factory), std::forward<Args>(args)...)) {}

        /** @} ----------------------------------------------------------------------------------------------- */

        /** @name Special Members @{ -------------------------------------------------------------------------- */

        Box(const Box&) = default;
        Box(Box&&) = default;
        Box& operator=(const Box&) = default;
        Box& operator=(Box&&) = default;
        ~Box() = default;

        /** @} ----------------------------------------------------------------------------------------------- */

        /** @brief Access the stored value while preserving the box's cv/ref qualification. */
        [[nodiscard]] constexpr decltype(auto) Value(this auto&& self) noexcept {
            return std::forward_like<decltype(self)>(self.value_);
        }

    private:
        SORA_NO_UNIQUE_ADDRESS T value_{};
    };

} // namespace Sora::Polymorphism

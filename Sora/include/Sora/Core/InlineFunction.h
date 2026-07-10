/**
 * @file InlineFunction.h
 * @brief Fixed-capacity, SBO-only, move-only callable wrapper with no heap allocation.
 *
 * @c InlineFunction<Sig, Cap> owns a callable entirely inside inline storage. Construction fails at compile time when
 * the callable exceeds @p Cap or requires stronger alignment than @c std::max_align_t. This is intended for render
 * graph pass lambdas, task queues, callbacks in hot paths, and other places where allocation is a semantic error.
 *
 * This type is deliberately not a replacement for @c std::function_ref: @c function_ref is non-owning, while
 * @c InlineFunction owns the callable. It is also not equivalent to @c std::move_only_function, because the fixed
 * inline capacity and no-allocation guarantee are part of the contract.
 *
 * @ingroup Core
 */
#pragma once

#include <concepts>
#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace Sora {

    template<typename Sig, std::size_t Cap = 64>
    class InlineFunction;

    namespace Detail {

        /** @brief Operations needed only for callables that cannot use the trivial relocation fast path. */
        struct Ops {
            void (*destroy)(void* storage) noexcept;
            void (*move)(void* dst, void* src) noexcept;
        };

        /** @brief Static operation table for a concrete callable type. */
        template<typename F>
        inline constexpr Ops kOps{
            .destroy = [](void* storage) noexcept { static_cast<F*>(storage)->~F(); },
            .move =
                [](void* dst, void* src) noexcept {
                    ::new (dst) F(std::move(*static_cast<F*>(src)));
                    static_cast<F*>(src)->~F();
                },
        };

        /**
         * @brief True when @p T can be relocated by copying its object representation.
         *
         * @details The compiler builtin is used when available. The fallback is intentionally conservative enough for
         * the fast path because it is additionally gated by trivial destruction before @c ops_ is elided.
         */
        template<typename T>
        inline constexpr bool kTriviallyRelocatable =
#if __has_builtin(__builtin_is_cpp_trivially_relocatable)
            __builtin_is_cpp_trivially_relocatable(T);
#elif __has_builtin(__is_trivially_relocatable)
            __is_trivially_relocatable(T);
#else
            std::is_trivially_copyable_v<T>;
#endif

        template<bool Noexcept, typename R, typename... Args>
        struct InvokePointer;

        template<typename R, typename... Args>
        struct InvokePointer<false, R, Args...> {
            using Type = R (*)(void*, Args...);
        };

        template<typename R, typename... Args>
        struct InvokePointer<true, R, Args...> {
            using Type = R (*)(void*, Args...) noexcept;
        };

        template<bool Noexcept, typename R, typename F, typename... Args>
        inline constexpr bool kCallableMatches =
            Noexcept ? std::is_nothrow_invocable_r_v<R, F&, Args...> : std::is_invocable_r_v<R, F&, Args...>;

        /**
         * @brief Shared implementation for throwing and @c noexcept function signatures.
         *
         * @tparam Noexcept Whether @c operator() and the stored invoker are @c noexcept.
         * @tparam R Return type of the callable signature.
         * @tparam Cap Inline storage capacity in bytes.
         * @tparam Args Callable argument types.
         */
        template<bool Noexcept, typename R, std::size_t Cap, typename... Args>
        class InlineFunctionImpl {
            static_assert(Cap >= sizeof(void*), "InlineFunction capacity must be at least pointer-sized.");
            static_assert(Cap % alignof(std::max_align_t) == 0,
                          "InlineFunction capacity should be a multiple of max_align_t alignment.");

            using InvokeFn = typename InvokePointer<Noexcept, R, Args...>::Type;

            InvokeFn invoke_ = nullptr;
            const Ops* ops_ = nullptr;
            alignas(std::max_align_t) std::byte storage_[Cap]{};

            template<typename F>
            static constexpr InvokeFn MakeInvoker() noexcept {
                if constexpr (Noexcept) {
                    return [](void* storage, Args... args) noexcept -> R {
                        return (*static_cast<F*>(storage))(std::forward<Args>(args)...);
                    };
                } else {
                    return [](void* storage, Args... args) -> R {
                        return (*static_cast<F*>(storage))(std::forward<Args>(args)...);
                    };
                }
            }

            void DestroyStorage() noexcept {
                if (invoke_ != nullptr && ops_ != nullptr) {
                    ops_->destroy(storage_);
                }
                invoke_ = nullptr;
                ops_ = nullptr;
            }

            void MoveFrom(InlineFunctionImpl& other) noexcept {
                invoke_ = other.invoke_;
                ops_ = other.ops_;
                if (other.invoke_ == nullptr) {
                    return;
                }
                if (other.ops_ != nullptr) {
                    other.ops_->move(storage_, other.storage_);
                } else {
                    __builtin_memcpy(storage_, other.storage_, Cap);
                }
                other.invoke_ = nullptr;
                other.ops_ = nullptr;
            }

        public:
            /** @brief Construct an empty callable wrapper. */
            constexpr InlineFunctionImpl() noexcept = default;

            /** @brief Construct an empty callable wrapper from @c nullptr. */
            constexpr InlineFunctionImpl(std::nullptr_t) noexcept {}

            /** @brief Construct from a callable that fits in @p Cap bytes and satisfies the signature. */
            template<typename F>
                requires(!std::derived_from<std::decay_t<F>, InlineFunctionImpl> &&
                         kCallableMatches<Noexcept, R, std::decay_t<F>, Args...>)
            constexpr InlineFunctionImpl(F&& f) noexcept(std::is_nothrow_move_constructible_v<std::decay_t<F>>) {
                using Decayed = std::decay_t<F>;
                static_assert(sizeof(Decayed) <= Cap, "Callable exceeds InlineFunction capacity.");
                static_assert(alignof(Decayed) <= alignof(std::max_align_t), "Callable alignment exceeds max_align_t.");

                invoke_ = MakeInvoker<Decayed>();
                if constexpr (kTriviallyRelocatable<Decayed> && std::is_trivially_destructible_v<Decayed>) {
                    ops_ = nullptr;
                } else {
                    ops_ = &kOps<Decayed>;
                }
                ::new (static_cast<void*>(storage_)) Decayed(std::forward<F>(f));
            }

            ~InlineFunctionImpl() { DestroyStorage(); }

            InlineFunctionImpl(const InlineFunctionImpl&) = delete ("InlineFunction is move-only.");
            InlineFunctionImpl& operator=(const InlineFunctionImpl&) = delete ("InlineFunction is move-only.");

            InlineFunctionImpl(InlineFunctionImpl&& other) noexcept { MoveFrom(other); }

            InlineFunctionImpl& operator=(InlineFunctionImpl&& other) noexcept {
                if (this != &other) {
                    DestroyStorage();
                    MoveFrom(other);
                }
                return *this;
            }

            InlineFunctionImpl& operator=(std::nullptr_t) noexcept {
                DestroyStorage();
                return *this;
            }

            /**
             * @brief Invoke the stored callable.
             * @pre A callable is stored; @c static_cast<bool>(*this) is true.
             */
            R operator()(Args... args) noexcept(Noexcept) { return invoke_(storage_, std::forward<Args>(args)...); }

            /** @brief True when a callable is currently stored. */
            [[nodiscard]] explicit constexpr operator bool() const noexcept { return invoke_ != nullptr; }

            /** @brief True when no callable is currently stored. */
            [[nodiscard]] friend constexpr bool operator==(const InlineFunctionImpl& f, std::nullptr_t) noexcept {
                return !static_cast<bool>(f);
            }

            /** @brief True when a callable is currently stored. */
            [[nodiscard]] friend constexpr bool operator!=(const InlineFunctionImpl& f, std::nullptr_t) noexcept {
                return static_cast<bool>(f);
            }

            /** @brief Swap two callable wrappers with identical signature and capacity. */
            void Swap(InlineFunctionImpl& other) noexcept {
                if (!ops_ && !other.ops_) {
                    alignas(std::max_align_t) std::byte tmp[Cap];
                    __builtin_memcpy(tmp, storage_, Cap);
                    __builtin_memcpy(storage_, other.storage_, Cap);
                    __builtin_memcpy(other.storage_, tmp, Cap);
                    std::swap(invoke_, other.invoke_);
                    return;
                }

                InlineFunctionImpl tmp{std::move(other)};
                other = std::move(*this);
                *this = std::move(tmp);
            }
        };

    } // namespace Detail
    /** @brief Fixed-capacity owning callable wrapper for a potentially throwing function signature. */
    template<typename R, typename... Args, std::size_t Cap>
    class InlineFunction<R(Args...), Cap> : private Detail::InlineFunctionImpl<false, R, Cap, Args...> {
        using Base = Detail::InlineFunctionImpl<false, R, Cap, Args...>;

    public:
        using Base::Base;
        using Base::operator();
        using Base::operator bool;

        constexpr InlineFunction() noexcept = default;
        InlineFunction(const InlineFunction&) = delete ("InlineFunction is move-only.");
        InlineFunction& operator=(const InlineFunction&) = delete ("InlineFunction is move-only.");
        InlineFunction(InlineFunction&&) noexcept = default;
        InlineFunction& operator=(InlineFunction&&) noexcept = default;

        /** @brief Clear the stored callable. */
        InlineFunction& operator=(std::nullptr_t) noexcept {
            Base::operator=(nullptr);
            return *this;
        }

        /** @brief True when no callable is currently stored. */
        [[nodiscard]] friend constexpr bool operator==(const InlineFunction& f, std::nullptr_t) noexcept {
            return !static_cast<bool>(f);
        }

        /** @brief True when a callable is currently stored. */
        [[nodiscard]] friend constexpr bool operator!=(const InlineFunction& f, std::nullptr_t) noexcept {
            return static_cast<bool>(f);
        }

        /** @brief Swap two callable wrappers with identical signature and capacity. */
        void Swap(InlineFunction& other) noexcept { Base::Swap(other); }

        /** @brief Swap two callable wrappers with identical signature and capacity. */
        friend void swap(InlineFunction& a, InlineFunction& b) noexcept { a.Swap(b); }
    };

    /** @brief Fixed-capacity owning callable wrapper for a non-throwing function signature. */
    template<typename R, typename... Args, std::size_t Cap>
    class InlineFunction<R(Args...) noexcept, Cap> : private Detail::InlineFunctionImpl<true, R, Cap, Args...> {
        using Base = Detail::InlineFunctionImpl<true, R, Cap, Args...>;

    public:
        using Base::Base;
        using Base::operator();
        using Base::operator bool;

        constexpr InlineFunction() noexcept = default;
        InlineFunction(const InlineFunction&) = delete ("InlineFunction is move-only.");
        InlineFunction& operator=(const InlineFunction&) = delete ("InlineFunction is move-only.");
        InlineFunction(InlineFunction&&) noexcept = default;
        InlineFunction& operator=(InlineFunction&&) noexcept = default;

        /** @brief Clear the stored callable. */
        InlineFunction& operator=(std::nullptr_t) noexcept {
            Base::operator=(nullptr);
            return *this;
        }

        /** @brief True when no callable is currently stored. */
        [[nodiscard]] friend constexpr bool operator==(const InlineFunction& f, std::nullptr_t) noexcept {
            return !static_cast<bool>(f);
        }

        /** @brief True when a callable is currently stored. */
        [[nodiscard]] friend constexpr bool operator!=(const InlineFunction& f, std::nullptr_t) noexcept {
            return static_cast<bool>(f);
        }

        /** @brief Swap two callable wrappers with identical signature and capacity. */
        void Swap(InlineFunction& other) noexcept { Base::Swap(other); }

        /** @brief Swap two callable wrappers with identical signature and capacity. */
        friend void swap(InlineFunction& a, InlineFunction& b) noexcept { a.Swap(b); }
    };

} // namespace Sora
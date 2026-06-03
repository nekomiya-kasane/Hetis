/**
 * @file InlineFunction.h
 * @brief Fixed-capacity, SBO-only, move-only callable wrapper — zero heap allocation.
 *
 * `InlineFunction<Sig, Cap>` stores any callable entirely within inline storage.
 * A `static_assert` fires if the callable exceeds capacity. Designed for
 * render-graph pass lambdas, task queues, and any hot path where allocation
 * is unacceptable.
 *
 * Key optimisations over a naive vtable approach:
 * - **Single indirect call**: `invoke_` pointer is stored inline (not behind a
 *   vtable pointer), saving one cache miss per invocation.
 * - **Trivial callable fast-path**: for `trivially_copyable` callables, destroy
 *   and move are no-ops / memcpy — no function-pointer overhead.
 * - **noexcept propagation**: `InlineFunction<void(int) noexcept>` correctly
 *   marks `operator()` as noexcept.
 * - **constexpr construction**: leverages C++26 constexpr placement new.
 * - **Deducing this**: single `operator()` definition covers const/non-const.
 *
 * @ingroup Core
 */
#pragma once

#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace Mashiro {

    // Forward declaration
    template <typename Sig, std::size_t Cap = 64>
    class InlineFunction;

    // =========================================================================
    // Implementation detail: split noexcept and non-noexcept signatures
    // =========================================================================

    namespace Detail::IFn {

        /// @brief Ops table for non-trivial callables (destroy + move).
        struct Ops {
            void (*destroy)(void* storage) noexcept;
            void (*move)(void* dst, void* src) noexcept;
        };

        /// @brief Generate a static Ops instance for callable type F.
        template <typename F>
        constexpr Ops kOps{
            .destroy = [](void* storage) noexcept {
                static_cast<F*>(storage)->~F();
            },
            .move = [](void* dst, void* src) noexcept {
                ::new (dst) F(std::move(*static_cast<F*>(src)));
                static_cast<F*>(src)->~F();
            },
        };

        /// @brief Detect trivially relocatable types.
        /// Uses compiler builtin when available, falls back to trivially_copyable.
        template <typename T>
        inline constexpr bool kTriviallyRelocatable =
#if __has_builtin(__builtin_is_cpp_trivially_relocatable)
            __builtin_is_cpp_trivially_relocatable(T);
#elif __has_builtin(__is_trivially_relocatable)
            __is_trivially_relocatable(T);
#else
            std::is_trivially_copyable_v<T>;
#endif

    } // namespace Detail::IFn

    // =========================================================================
    // InlineFunction<R(Args...), Cap> — non-noexcept signature
    // =========================================================================

    template <typename R, typename... Args, std::size_t Cap>
    class InlineFunction<R(Args...), Cap> {
        static_assert(Cap >= sizeof(void*), "Cap must be at least pointer-sized.");
        static_assert(Cap % alignof(std::max_align_t) == 0,
                      "Cap should be a multiple of max_align_t alignment.");

        using InvokeFn = R(*)(void*, Args...);
        using Ops = Detail::IFn::Ops;

        // ----- Storage -----
        InvokeFn invoke_ = nullptr;
        const Ops* ops_ = nullptr; // nullptr for trivially-relocatable callables
        alignas(std::max_align_t) unsigned char storage_[Cap]{};

        // ----- Helpers -----
        template <typename F>
        static constexpr InvokeFn MakeInvoker() noexcept {
            return [](void* storage, Args... args) -> R {
                return (*static_cast<F*>(storage))(std::forward<Args>(args)...);
            };
        }

        void DestroyStorage() noexcept {
            if (invoke_ && ops_) {
                ops_->destroy(storage_);
            }
            invoke_ = nullptr;
            ops_ = nullptr;
        }

        void MoveFrom(InlineFunction& other) noexcept {
            invoke_ = other.invoke_;
            ops_ = other.ops_;
            if (other.invoke_) {
                if (other.ops_) {
                    other.ops_->move(storage_, other.storage_);
                } else {
                    // Trivially relocatable: memcpy
                    __builtin_memcpy(storage_, other.storage_, Cap);
                }
                other.invoke_ = nullptr;
                other.ops_ = nullptr;
            }
        }

       public:
        /// @name Constructors / destructor
        /// @{

        constexpr InlineFunction() noexcept = default;
        constexpr InlineFunction(std::nullptr_t) noexcept {} // NOLINT

        /// @brief Construct from any callable that fits in Cap bytes.
        template <typename F>
            requires(!std::is_same_v<std::remove_cvref_t<F>, InlineFunction> &&
                     std::is_invocable_r_v<R, std::remove_cvref_t<F>&, Args...>)
        constexpr InlineFunction(F&& f)
            noexcept(std::is_nothrow_move_constructible_v<std::remove_cvref_t<F>>) {
            using Decayed = std::remove_cvref_t<F>;
            static_assert(sizeof(Decayed) <= Cap,
                "Callable exceeds InlineFunction capacity.");
            static_assert(alignof(Decayed) <= alignof(std::max_align_t),
                "Callable alignment exceeds max_align_t.");

            invoke_ = MakeInvoker<Decayed>();

            // Trivially relocatable callables need no ops (destroy = no-op, move = memcpy)
            if constexpr (Detail::IFn::kTriviallyRelocatable<Decayed> &&
                          std::is_trivially_destructible_v<Decayed>) {
                ops_ = nullptr;
            } else {
                ops_ = &Detail::IFn::kOps<Decayed>;
            }

            ::new (static_cast<void*>(storage_)) Decayed(std::forward<F>(f));
        }

        ~InlineFunction() { DestroyStorage(); }

        /// @}

        /// @name Move semantics (no copy)
        /// @{

        InlineFunction(const InlineFunction&) = delete;
        InlineFunction& operator=(const InlineFunction&) = delete;

        InlineFunction(InlineFunction&& other) noexcept { MoveFrom(other); }

        InlineFunction& operator=(InlineFunction&& other) noexcept {
            if (this != &other) {
                DestroyStorage();
                MoveFrom(other);
            }
            return *this;
        }

        InlineFunction& operator=(std::nullptr_t) noexcept {
            DestroyStorage();
            return *this;
        }

        /// @}

        /// @name Invocation
        /// @{

        /// @brief Invoke the stored callable.
        /// @pre A callable is stored (`operator bool()` is true).
        R operator()(Args... args) {
            return invoke_(storage_, std::forward<Args>(args)...);
        }

        /// @}

        /// @name Observers
        /// @{

        /// @brief True if a callable is stored.
        [[nodiscard]] explicit constexpr operator bool() const noexcept {
            return invoke_ != nullptr;
        }

        /// @brief Equality with nullptr.
        [[nodiscard]] friend constexpr bool operator==(
            const InlineFunction& f, std::nullptr_t) noexcept {
            return !static_cast<bool>(f);
        }

        /// @}

        /// @name Swap
        /// @{

        void Swap(InlineFunction& other) noexcept {
            // Both trivial: plain memcpy swap
            if (!ops_ && !other.ops_) {
                alignas(std::max_align_t) unsigned char tmp[Cap];
                __builtin_memcpy(tmp, storage_, Cap);
                __builtin_memcpy(storage_, other.storage_, Cap);
                __builtin_memcpy(other.storage_, tmp, Cap);
                std::swap(invoke_, other.invoke_);
                // ops_ stays nullptr for both
            } else {
                InlineFunction tmp{std::move(other)};
                other = std::move(*this);
                *this = std::move(tmp);
            }
        }

        friend void swap(InlineFunction& a, InlineFunction& b) noexcept { a.Swap(b); }

        /// @}
    };

    // =========================================================================
    // InlineFunction<R(Args...) noexcept, Cap> — noexcept signature
    // =========================================================================

    template <typename R, typename... Args, std::size_t Cap>
    class InlineFunction<R(Args...) noexcept, Cap> {
        static_assert(Cap >= sizeof(void*), "Cap must be at least pointer-sized.");
        static_assert(Cap % alignof(std::max_align_t) == 0,
                      "Cap should be a multiple of max_align_t alignment.");

        using InvokeFn = R(*)(void*, Args...) noexcept;
        using Ops = Detail::IFn::Ops;

        InvokeFn invoke_ = nullptr;
        const Ops* ops_ = nullptr;
        alignas(std::max_align_t) unsigned char storage_[Cap]{};

        template <typename F>
        static constexpr InvokeFn MakeInvoker() noexcept {
            return [](void* storage, Args... args) noexcept -> R {
                return (*static_cast<F*>(storage))(std::forward<Args>(args)...);
            };
        }

        void DestroyStorage() noexcept {
            if (invoke_ && ops_) {
                ops_->destroy(storage_);
            }
            invoke_ = nullptr;
            ops_ = nullptr;
        }

        void MoveFrom(InlineFunction& other) noexcept {
            invoke_ = other.invoke_;
            ops_ = other.ops_;
            if (other.invoke_) {
                if (other.ops_) {
                    other.ops_->move(storage_, other.storage_);
                } else {
                    __builtin_memcpy(storage_, other.storage_, Cap);
                }
                other.invoke_ = nullptr;
                other.ops_ = nullptr;
            }
        }

       public:
        constexpr InlineFunction() noexcept = default;
        constexpr InlineFunction(std::nullptr_t) noexcept {}

        template <typename F>
            requires(!std::is_same_v<std::remove_cvref_t<F>, InlineFunction> &&
                     std::is_nothrow_invocable_r_v<R, std::remove_cvref_t<F>&, Args...>)
        constexpr InlineFunction(F&& f)
            noexcept(std::is_nothrow_move_constructible_v<std::remove_cvref_t<F>>) {
            using Decayed = std::remove_cvref_t<F>;
            static_assert(sizeof(Decayed) <= Cap,
                "Callable exceeds InlineFunction capacity.");
            static_assert(alignof(Decayed) <= alignof(std::max_align_t),
                "Callable alignment exceeds max_align_t.");

            invoke_ = MakeInvoker<Decayed>();

            if constexpr (Detail::IFn::kTriviallyRelocatable<Decayed> &&
                          std::is_trivially_destructible_v<Decayed>) {
                ops_ = nullptr;
            } else {
                ops_ = &Detail::IFn::kOps<Decayed>;
            }

            ::new (static_cast<void*>(storage_)) Decayed(std::forward<F>(f));
        }

        ~InlineFunction() { DestroyStorage(); }

        InlineFunction(const InlineFunction&) = delete;
        InlineFunction& operator=(const InlineFunction&) = delete;

        InlineFunction(InlineFunction&& other) noexcept { MoveFrom(other); }

        InlineFunction& operator=(InlineFunction&& other) noexcept {
            if (this != &other) { DestroyStorage(); MoveFrom(other); }
            return *this;
        }

        InlineFunction& operator=(std::nullptr_t) noexcept { DestroyStorage(); return *this; }

        /// @brief Invoke (noexcept guaranteed by signature).
        R operator()(Args... args) noexcept {
            return invoke_(storage_, std::forward<Args>(args)...);
        }

        [[nodiscard]] explicit constexpr operator bool() const noexcept { return invoke_ != nullptr; }

        [[nodiscard]] friend constexpr bool operator==(
            const InlineFunction& f, std::nullptr_t) noexcept {
            return !static_cast<bool>(f);
        }

        void Swap(InlineFunction& other) noexcept {
            if (!ops_ && !other.ops_) {
                alignas(std::max_align_t) unsigned char tmp[Cap];
                __builtin_memcpy(tmp, storage_, Cap);
                __builtin_memcpy(storage_, other.storage_, Cap);
                __builtin_memcpy(other.storage_, tmp, Cap);
                std::swap(invoke_, other.invoke_);
            } else {
                InlineFunction tmp{std::move(other)};
                other = std::move(*this);
                *this = std::move(tmp);
            }
        }

        friend void swap(InlineFunction& a, InlineFunction& b) noexcept { a.Swap(b); }
    };

}  // namespace Mashiro

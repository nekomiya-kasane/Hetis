/**
 * @file Generator.h
 * @brief Synchronous coroutine generator for ranges — a feature-complete, zero-overhead
 *        replacement for std::generator (P2502) built on C++26.
 *
 * Capabilities:
 * - **Lazy evaluation**: initial_suspend = suspend_always.
 * - **Recursive delegation**: co_yield ElementsOf(gen) with O(1) per-element symmetric
 *   transfer via intrusive root/leaf promise chain (no std::stack heap allocation).
 * - **Allocator support**: template parameter, allocator_arg_t function parameter, or both.
 * - **Reference/value type control**: Generator<Ref, V, Alloc> — Ref controls dereference,
 *   V controls range_value_t (avoids dangling views like string_view→string).
 * - **Move-only**: coroutine state is a unique resource.
 * - **Exception propagation**: unhandled_exception stores exception_ptr, rethrown on resume.
 * - **Ranges integration**: models input_range.
 * - **C++26 consteval validation**: compile-time constraint checks on template parameters.
 * - **HALO-friendly**: minimal promise size (4 pointers + exception_ptr).
 *
 * Design notes (recursive delegation):
 *
 *   std::generator maintains a stack<coroutine_handle<>> externally. We use an intrusive
 *   approach: every promise stores root_ (outermost promise) and parent_ (enclosing promise).
 *   The root promise stores active_ — the coroutine_handle of the innermost leaf that the
 *   iterator should resume next. When a generator delegates via co_yield ElementsOf(child),
 *   the DelegationAwaiter wires the child's root_/parent_ and updates root_->active_.
 *   When the child finishes, FinalAwaiter restores root_->active_ to the parent and does
 *   symmetric transfer back to the parent coroutine body. The iterator always resumes
 *   root_->active_ and reads root_->value_.
 *
 * @ingroup Schedular
 */
#pragma once

#include <concepts>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <iterator>
#include <memory>
#include <new>
#include <ranges>
#include <type_traits>
#include <utility>

namespace Mashiro {

    // =========================================================================
    // ElementsOf — tag for recursive generator delegation
    // =========================================================================

    /**
     * @brief Tag wrapper indicating that the elements of a range should be yielded
     *        individually rather than the range itself.
     *
     * Analogous to std::ranges::elements_of (which may be absent in this libc++).
     *
     * @tparam Rng   The range type.
     * @tparam Alloc Allocator to use for the nested coroutine frame (default = void).
     */
    template <std::ranges::range Rng, typename Alloc = void>
    struct ElementsOf {
        Rng range;

        constexpr explicit ElementsOf(Rng&& rng) noexcept
            : range(std::forward<Rng>(rng)) {}
    };

    template <std::ranges::range Rng, typename Alloc>
        requires (!std::is_void_v<Alloc>)
    struct ElementsOf<Rng, Alloc> {
        Rng range;
        Alloc allocator;

        constexpr ElementsOf(Rng&& rng, Alloc alloc) noexcept
            : range(std::forward<Rng>(rng)), allocator(std::move(alloc)) {}
    };

    template <std::ranges::range Rng>
    ElementsOf(Rng&&) -> ElementsOf<Rng>;

    template <std::ranges::range Rng, typename Alloc>
    ElementsOf(Rng&&, Alloc) -> ElementsOf<Rng, Alloc>;

    // =========================================================================
    // Forward declarations
    // =========================================================================

    template <typename Ref, typename V = void, typename Allocator = void>
    class Generator;

    /// @cond INTERNAL
    namespace Detail::Gen {

        // ---------------------------------------------------------------------
        // Type computation (matches std::generator semantics exactly)
        // ---------------------------------------------------------------------

        template <typename Ref, typename V>
        using ValueType = std::conditional_t<std::is_void_v<V>,
            std::remove_cvref_t<Ref>, V>;

        template <typename Ref, typename V>
        using ReferenceType = std::conditional_t<std::is_void_v<V>,
            Ref&&, Ref>;

        template <typename Reference>
        using YieldedType = std::conditional_t<std::is_reference_v<Reference>,
            Reference, const Reference&>;

        template <typename Reference>
        using RRefType = std::conditional_t<std::is_reference_v<Reference>,
            std::remove_reference_t<Reference>&&, Reference>;

        // ---------------------------------------------------------------------
        // Concept: valid Generator parameter combination
        // (mirrors [coro.generator.class]/2 ill-formed clauses)
        // ---------------------------------------------------------------------

        template <typename Ref, typename V>
        concept ValidGeneratorParams = requires {
            typename ValueType<Ref, V>;
            typename ReferenceType<Ref, V>;
        } && std::common_reference_with<ReferenceType<Ref, V>&&, ValueType<Ref, V>&>
          && std::common_reference_with<ReferenceType<Ref, V>&&, RRefType<ReferenceType<Ref, V>>&&>
          && std::common_reference_with<RRefType<ReferenceType<Ref, V>>&&, const ValueType<Ref, V>&>;

        // =====================================================================
        // Allocator-aware operator new/delete for promise_type
        //
        // The allocator (rebound to std::byte) is stored immediately before
        // the compiler-allocated coroutine frame at max_align_t alignment.
        // =====================================================================

        template <typename Allocator>
        struct AllocatorSupport {};  // void / non-allocator: default new/delete

        template <typename Allocator>
            requires (!std::is_void_v<Allocator>)
        struct AllocatorSupport<Allocator> {
        private:
            using ByteAlloc  = typename std::allocator_traits<Allocator>::template rebind_alloc<std::byte>;
            using ByteTraits = std::allocator_traits<ByteAlloc>;

            static constexpr size_t kAllocOffset =
                (sizeof(ByteAlloc) + alignof(std::max_align_t) - 1)
                    & ~(alignof(std::max_align_t) - 1);

            static void* AllocFrame(ByteAlloc alloc, size_t frameSize) {
                size_t total = kAllocOffset + frameSize;
                auto*  mem   = ByteTraits::allocate(alloc, total);
                ::new (static_cast<void*>(mem)) ByteAlloc(std::move(alloc));
                return mem + kAllocOffset;
            }

        public:
            static void* operator new(size_t frameSize)
                requires std::default_initializable<ByteAlloc>
            {
                return AllocFrame(ByteAlloc{}, frameSize);
            }

            template <typename... Args>
            static void* operator new(size_t frameSize,
                                       std::allocator_arg_t, const Allocator& a, Args&&...) {
                return AllocFrame(ByteAlloc{a}, frameSize);
            }

            template <typename This, typename... Args>
            static void* operator new(size_t frameSize,
                                       This&, std::allocator_arg_t,
                                       const Allocator& a, Args&&...) {
                return AllocFrame(ByteAlloc{a}, frameSize);
            }

            static void operator delete(void* ptr, size_t frameSize) noexcept {
                auto*  mem  = static_cast<std::byte*>(ptr) - kAllocOffset;
                auto&  a    = *reinterpret_cast<ByteAlloc*>(mem);
                ByteAlloc tmp(std::move(a));
                a.~ByteAlloc();
                ByteTraits::deallocate(tmp, mem, kAllocOffset + frameSize);
            }
        };

        // void allocator: no custom operator new/delete — use default.
        // The compiler will use global ::operator new for the coroutine frame.
        template <>
        struct AllocatorSupport<void> {};

        // =====================================================================
        // Promise type
        // =====================================================================

        template <typename Ref, typename V, typename Allocator>
        class PromiseType : public AllocatorSupport<Allocator> {
        public:
            using generator_type = Generator<Ref, V, Allocator>;
            using reference      = ReferenceType<Ref, V>;
            using yielded        = YieldedType<reference>;
            using value_type     = ValueType<Ref, V>;

        private:
            // Currently-yielded value. All yields (including nested) write to
            // root_->value_ so the iterator always reads from the root promise.
            std::add_pointer_t<yielded> value_ = nullptr;

            // Exception propagation — nested generators store here via root_.
            std::exception_ptr except_{};

            // Intrusive recursive delegation chain:
            //   root_   → outermost promise (iterator reads root_->value_)
            //   parent_ → enclosing promise (for unwinding on finish)
            //   active_ → (root only) handle of the innermost leaf coroutine
            //              that the iterator should resume next.
            PromiseType*                       root_   = this;
            PromiseType*                       parent_ = nullptr;
            std::coroutine_handle<PromiseType> active_{};

            friend generator_type;

            template <typename, typename, typename>
            friend class PromiseType;

        public:
            PromiseType() = default;
            PromiseType(PromiseType&&)            = delete;
            PromiseType(const PromiseType&)       = delete;
            PromiseType& operator=(PromiseType&&) = delete;
            PromiseType& operator=(const PromiseType&) = delete;

            // -----------------------------------------------------------------
            // Coroutine interface
            // -----------------------------------------------------------------

            generator_type get_return_object() noexcept {
                auto h = std::coroutine_handle<PromiseType>::from_promise(*this);
                active_ = h;  // root's initial active is itself
                return generator_type{h};
            }

            std::suspend_always initial_suspend() noexcept { return {}; }

            void return_void() noexcept {}

            void unhandled_exception() {
                if (root_ != this)
                    root_->except_ = std::current_exception();
                else
                    except_ = std::current_exception();
            }

            // -----------------------------------------------------------------
            // yield_value — single element
            //
            // Three overloads mirror [coro.generator.promise]/5-7:
            // (1) yielded is a reference type → accept yielded directly.
            // (2) yielded is const T& (non-reference reference type) →
            //     accept by value, store pointer to the parameter
            //     (the parameter lives across the suspension).
            // (3) Universal-ref fallback for convertible types.
            //
            // For Generator<int>:  reference = int&&, yielded = int&&.
            //   co_yield lvalue → overload (3) with U=int&, converts via
            //   Element{} to yielded. But int& is not convertible to int&&.
            //   The standard solves this: "An expression e is yielded as
            //   if by yield_value(e)" where yield_value(yielded) is the
            //   primary overload.  For prvalues, yielded=int&& binds.
            //   For lvalues, the standard's wording uses an additional
            //   overload accepting `reference` (see [coro.generator.promise]).
            //
            // We unify with a single template overload that materializes
            // the value on the coroutine frame side.
            // -----------------------------------------------------------------

            // Primary: yielded is a reference → binds to lvalue or rvalue
            // that already matches the reference type.
            std::suspend_always yield_value(yielded val) noexcept
                requires std::is_reference_v<yielded>
            {
                root_->value_ = std::addressof(val);
                return {};
            }

            // When reference is an rvalue reference (e.g. Generator<int> →
            // reference = int&&, yielded = int&&), lvalues can't bind to
            // yielded. This overload accepts any convertible type by
            // forwarding reference and stores the result in a temporary
            // that persists across the suspension point.
            //
            // The standard achieves this via an internal wrapper type.
            // We use a nested awaitable that stores the converted value.
            struct ValueAwaiter {
                std::remove_cvref_t<yielded> stored;
                PromiseType* root;

                bool await_ready() noexcept { return false; }
                void await_suspend(std::coroutine_handle<>) noexcept {
                    root->value_ = std::addressof(stored);
                }
                void await_resume() noexcept {}
            };

            template <typename U>
                requires (!std::same_as<std::remove_cvref_t<U>, PromiseType>)
                      && (!std::is_reference_v<yielded> || !std::same_as<U, yielded>)
                      && std::constructible_from<std::remove_cvref_t<yielded>, U>
            ValueAwaiter yield_value(U&& val) noexcept(
                std::is_nothrow_constructible_v<std::remove_cvref_t<yielded>, U>)
            {
                return {std::remove_cvref_t<yielded>(std::forward<U>(val)), root_};
            }

            // -----------------------------------------------------------------
            // yield_value — recursive delegation (ElementsOf)
            //
            // The awaiter does symmetric transfer to the nested coroutine.
            // The nested coroutine's promise is wired into the chain so that:
            //   - Its root_ points to our root_.
            //   - Its parent_ points to us.
            //   - root_->active_ is updated to the nested handle.
            // When the nested finishes, FinalAwaiter restores root_->active_
            // to the parent handle and symmetric-transfers back to us.
            // -----------------------------------------------------------------

            template <typename Gen>
            struct DelegationAwaiter {
                Gen nested;

                constexpr bool await_ready() noexcept { return false; }

                std::coroutine_handle<>
                await_suspend(std::coroutine_handle<PromiseType> parent) noexcept {
                    auto  childHandle  = nested.coroutine_;
                    auto& childPromise = childHandle.promise();
                    auto& parentPromise = parent.promise();

                    // Wire the chain.
                    childPromise.root_   = parentPromise.root_;
                    childPromise.parent_ = &parentPromise;

                    // Update root's active to the new leaf.
                    parentPromise.root_->active_ = childHandle;

                    // Transfer ownership from the Generator object to us:
                    // the nested generator's destructor won't destroy the handle.
                    nested.coroutine_ = nullptr;

                    // Symmetric transfer: resume the child coroutine.
                    return childHandle;
                }

                void await_resume() {
                    // After the nested generator is fully consumed, we resume here.
                    // Exceptions from nested were stored in root_->except_ by
                    // unhandled_exception() and will be rethrown by the iterator.
                }
            };

            template <typename R2, typename V2, typename A2>
            DelegationAwaiter<Generator<R2, V2, A2>>
            yield_value(ElementsOf<Generator<R2, V2, A2>> eo) noexcept {
                return {std::move(eo.range)};
            }

            template <std::ranges::range Rng, typename Alloc>
                requires (!std::same_as<std::remove_cvref_t<Rng>, generator_type>)
            DelegationAwaiter<generator_type>
            yield_value(ElementsOf<Rng, Alloc> eo) {
                auto adapt = [](auto rng) -> generator_type {
                    for (auto&& elem : rng) {
                        co_yield static_cast<decltype(elem)>(elem);
                    }
                };
                return {adapt(std::move(eo.range))};
            }

            void await_transform() = delete;

            // -----------------------------------------------------------------
            // final_suspend — symmetric transfer back to parent
            //
            // For nested coroutines: destroy our frame, restore root_->active_
            // to parent, and symmetric-transfer to parent (which resumes at
            // DelegationAwaiter::await_resume).
            //
            // For root coroutine: return noop_coroutine so the iterator's
            // .resume() returns and the iterator can detect .done().
            // -----------------------------------------------------------------

            struct FinalAwaiter {
                constexpr bool await_ready() noexcept { return false; }

                std::coroutine_handle<>
                await_suspend(std::coroutine_handle<PromiseType> h) noexcept {
                    auto& promise = h.promise();
                    auto* parent  = promise.parent_;

                    if (parent) {
                        // Restore root's active to the parent.
                        auto parentHandle =
                            std::coroutine_handle<PromiseType>::from_promise(*parent);
                        promise.root_->active_ = parentHandle;

                        // Destroy the nested coroutine frame.
                        h.destroy();

                        // Symmetric transfer back to parent (resumes at await_resume
                        // of DelegationAwaiter, then parent body continues).
                        return parentHandle;
                    }

                    // Top-level: return to caller (iterator::operator++ / begin).
                    return std::noop_coroutine();
                }

                void await_resume() noexcept {}
            };

            FinalAwaiter final_suspend() noexcept { return {}; }
        };

    } // namespace Detail::Gen
    /// @endcond

    // =========================================================================
    // Generator
    // =========================================================================

    /**
     * @brief Synchronous coroutine generator for ranges.
     *
     * A feature-complete, zero-overhead replacement for `std::generator` (P2502)
     * targeting C++26 with COCA clang-p2996.
     *
     * @tparam Ref       The reference type yielded by the generator. Controls what
     *                   `*it` returns. If `V` is `void`, `reference = Ref&&`.
     * @tparam V         The value type. If `void`, deduced as `remove_cvref_t<Ref>`.
     *                   Use to avoid dangling (e.g. `Generator<string_view, string>`).
     * @tparam Allocator Custom allocator for the coroutine frame. `void` = default.
     *
     * @code
     * Generator<int> fibonacci() {
     *     int a = 0, b = 1;
     *     while (true) {
     *         co_yield a;
     *         auto next = a + b;
     *         a = b;
     *         b = next;
     *     }
     * }
     *
     * // Recursive delegation:
     * Generator<const int&> tree_inorder(const Tree& t) {
     *     if (t.left)  co_yield ElementsOf(tree_inorder(*t.left));
     *     co_yield t.value;
     *     if (t.right) co_yield ElementsOf(tree_inorder(*t.right));
     * }
     * @endcode
     */
    template <typename Ref, typename V, typename Allocator>
    class Generator {
        static_assert(Detail::Gen::ValidGeneratorParams<Ref, V>,
            "Generator<Ref, V>: invalid parameter combination. "
            "Ref/V must satisfy the common_reference_with constraints.");
    public:
        // -----------------------------------------------------------------
        // Public type aliases (match std::generator interface)
        // -----------------------------------------------------------------
        using promise_type = Detail::Gen::PromiseType<Ref, V, Allocator>;
        using value_type   = Detail::Gen::ValueType<Ref, V>;
        using reference    = Detail::Gen::ReferenceType<Ref, V>;
        using yielded      = Detail::Gen::YieldedType<reference>;

    private:
        std::coroutine_handle<promise_type> coroutine_ = nullptr;

        friend promise_type;

        template <typename, typename, typename>
        friend class Detail::Gen::PromiseType;

        explicit Generator(std::coroutine_handle<promise_type> h) noexcept
            : coroutine_(h) {}

    public:
        Generator() noexcept = default;

        Generator(Generator&& other) noexcept
            : coroutine_(std::exchange(other.coroutine_, nullptr)) {}

        Generator& operator=(Generator&& other) noexcept {
            if (this != &other) {
                if (coroutine_) coroutine_.destroy();
                coroutine_ = std::exchange(other.coroutine_, nullptr);
            }
            return *this;
        }

        Generator(const Generator&) = delete;
        Generator& operator=(const Generator&) = delete;

        ~Generator() {
            if (coroutine_) coroutine_.destroy();
        }

        // -----------------------------------------------------------------
        // Iterator
        //
        // The iterator holds the root coroutine handle. For resume it uses
        // root.promise().active_ (the innermost leaf). For dereference it
        // reads root.promise().value_ (all yields write there).
        // -----------------------------------------------------------------

        struct Sentinel {};

        class Iterator {
        public:
            using iterator_concept = std::input_iterator_tag;
            using difference_type  = std::ptrdiff_t;
            using value_type       = Generator::value_type;

        private:
            std::coroutine_handle<promise_type> coroutine_ = nullptr;

            friend Generator;

            explicit Iterator(std::coroutine_handle<promise_type> h) noexcept
                : coroutine_(h) {}

            void ThrowIfException() {
                if (coroutine_) {
                    auto& p = coroutine_.promise();
                    if (p.except_)
                        std::rethrow_exception(std::move(p.except_));
                }
            }

        public:
            Iterator() noexcept = default;

            Iterator(Iterator&& other) noexcept
                : coroutine_(std::exchange(other.coroutine_, nullptr)) {}
            Iterator& operator=(Iterator&& other) noexcept {
                coroutine_ = std::exchange(other.coroutine_, nullptr);
                return *this;
            }

            [[nodiscard]] reference operator*() const noexcept {
                return static_cast<reference>(*coroutine_.promise().value_);
            }

            Iterator& operator++() {
                // Resume the active (innermost) coroutine.
                auto& active = coroutine_.promise().active_;
                active.resume();

                // After resume, check for exceptions propagated from nested generators.
                ThrowIfException();

                return *this;
            }

            void operator++(int) { ++*this; }

            [[nodiscard]] friend bool operator==(const Iterator& it, Sentinel) noexcept {
                return !it.coroutine_ || it.coroutine_.done();
            }
        };

        // -----------------------------------------------------------------
        // Range interface
        // -----------------------------------------------------------------

        [[nodiscard]] Iterator begin() {
            if (coroutine_) {
                // Resume the root coroutine to produce the first value.
                // active_ starts as the root handle itself (set in get_return_object).
                coroutine_.promise().active_.resume();

                // Check for immediate completion or exception.
                Iterator it{coroutine_};
                it.ThrowIfException();
                return it;
            }
            return Iterator{nullptr};
        }

        [[nodiscard]] static constexpr Sentinel end() noexcept { return {}; }

        // -----------------------------------------------------------------
        // Utility
        // -----------------------------------------------------------------

        [[nodiscard]] explicit operator bool() const noexcept {
            return coroutine_ != nullptr;
        }
    };

} // namespace Mashiro

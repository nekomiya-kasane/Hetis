/**
 * @file Generator.h
 * @brief Synchronous coroutine generator for ranges.
 * @details Provides a move-only, lazy coroutine range modelled after
 * @c std::generator (P2502), with recursive delegation and optional coroutine
 * frame allocation support.
 *
 * @par Capabilities
 * @li Lazy evaluation through @c initial_suspend returning @c std::suspend_always.
 * @li Recursive delegation through @c co_yield @c ElementsOf(gen).
 * @li Allocator support through the template parameter and
 *     @c std::allocator_arg_t coroutine function parameters.
 * @li Separate reference and value type control via @c Generator<Ref,V,Allocator>.
 * @li Move-only ownership of the coroutine state.
 * @li Exception capture in @c unhandled_exception and rethrow on iterator resume.
 * @li Ranges integration as an input range.
 *
 * @par Concurrency and progress
 * Iteration is synchronous: @c begin and increment resume coroutine work on the calling thread. A generator and every
 * iterator referring to it form one affine traversal and must not be resumed or destroyed concurrently. Coroutine-frame
 * allocation and user code may block, so this range makes no lock-free or wait-free progress claim.
 *
 * @par Recursive Delegation Model
 * @c std::generator maintains an external stack of coroutine handles. This
 * implementation uses an intrusive chain: each promise stores @c root_ for the
 * outermost promise and @c parent_ for the enclosing promise. The root promise
 * stores @c active_, the handle of the innermost coroutine resumed by the
 * iterator. Delegation wires the child promise into this chain and updates
 * @c root_->active_. Completion restores the active handle to the parent.
 *
 * @ingroup Concurrency
 */
#pragma once

#include <bit>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <ranges>
#include <type_traits>
#include <utility>

#include <Sora/Core/Memory/AllocatorTraits.h>

namespace Sora {

    /** @name Recursive Delegation Tags @{ ————————————————————————————————————— */

    /**
     * @brief Tag wrapper indicating that the elements of a range should be yielded
     *        individually rather than the range itself.
     *
     * @details Analogous to @c std::ranges::elements_of, which may be absent in
     * the active standard library.
     *
     * @tparam Rng The range type.
     * @tparam Alloc Allocator type to use for a nested coroutine frame, or
     * @c void for the default path.
     */
    template<std::ranges::range Rng, typename Alloc = void>
    struct ElementsOf {
        /** @brief Range whose elements are delegated. */
        Rng range;

        /**
         * @brief Construct a delegation wrapper from a range.
         * @param[in] rng Range object or reference to delegate.
         */
        constexpr explicit ElementsOf(Rng&& rng) noexcept : range(std::forward<Rng>(rng)) {}
    };

    /**
     * @brief Delegation wrapper carrying an explicit allocator object.
     * @tparam Rng The range type.
     * @tparam Alloc Allocator type for nested coroutine allocation.
     */
    template<std::ranges::range Rng, typename Alloc>
        requires(!std::is_void_v<Alloc>)
    struct ElementsOf<Rng, Alloc> {
        /** @brief Range whose elements are delegated. */
        Rng range;

        /** @brief Allocator object associated with the delegated range. */
        Alloc allocator;

        /**
         * @brief Construct a delegation wrapper from a range and allocator.
         * @param[in] rng Range object or reference to delegate.
         * @param[in] alloc Allocator object for nested coroutine state.
         */
        constexpr ElementsOf(Rng&& rng, Alloc alloc) noexcept
            : range(std::forward<Rng>(rng)), allocator(std::move(alloc)) {}
    };

    /**
     * @brief Deduce @c ElementsOf without an explicit allocator.
     * @tparam Rng The range type.
     */
    template<std::ranges::range Rng>
    ElementsOf(Rng&&) -> ElementsOf<Rng>;

    /**
     * @brief Deduce @c ElementsOf with an explicit allocator.
     * @tparam Rng The range type.
     * @tparam Alloc Allocator type.
     */
    template<std::ranges::range Rng, typename Alloc>
    ElementsOf(Rng&&, Alloc) -> ElementsOf<Rng, Alloc>;

    /** @} ————————————————————————————————————————————————————————————————————— */

    /** @name Forward Declarations @{ —————————————————————————————————————————— */

    /**
     * @brief Synchronous coroutine generator for ranges.
     * @tparam Ref Reference type exposed by iterator dereference.
     * @tparam V Value type exposed through range traits, or @c void to derive it.
     * @tparam Allocator Allocator type for the coroutine frame, or @c void.
     * @tparam FrameAlignment Maximum alignment required by the promise and coroutine-frame locals.
     */
    template<typename Ref, typename V = void, typename Allocator = void,
             std::size_t FrameAlignment = alignof(std::max_align_t)>
    class Generator;

    /** @} ————————————————————————————————————————————————————————————————————— */

    /** @cond INTERNAL */
    namespace Detail::Gen {

        /** @name Type Computation @{ —————————————————————————————————————————— */

        /**
         * @brief Range value type for a generator parameter set.
         * @tparam Ref Reference parameter.
         * @tparam V Explicit value parameter, or @c void.
         */
        template<typename Ref, typename V>
        using ValueType = std::conditional_t<std::is_void_v<V>, std::remove_cvref_t<Ref>, V>;

        /**
         * @brief Iterator reference type for a generator parameter set.
         * @tparam Ref Reference parameter.
         * @tparam V Explicit value parameter, or @c void.
         */
        template<typename Ref, typename V>
        using ReferenceType = std::conditional_t<std::is_void_v<V>, Ref&&, Ref>;

        /**
         * @brief Type accepted by @c yield_value for directly yielded objects.
         * @tparam Reference Generator reference type.
         */
        template<typename Reference>
        using YieldedType = std::conditional_t<std::is_reference_v<Reference>, Reference, const Reference&>;

        /**
         * @brief Rvalue-reference form used by generator common-reference checks.
         * @tparam Reference Generator reference type.
         */
        template<typename Reference>
        using RRefType =
            std::conditional_t<std::is_reference_v<Reference>, std::remove_reference_t<Reference>&&, Reference>;

        /**
         * @brief True when @c Ref and @c V satisfy the generator parameter rules.
         * @details Mirrors the common-reference constraints from
         * [coro.generator.class].
         * @tparam Ref Reference parameter.
         * @tparam V Explicit value parameter, or @c void.
         */
        template<typename Ref, typename V>
        concept ValidGeneratorParams =
            requires {
                typename ValueType<Ref, V>;
                typename ReferenceType<Ref, V>;
            } && std::common_reference_with<ReferenceType<Ref, V>&&, ValueType<Ref, V>&> &&
            std::common_reference_with<ReferenceType<Ref, V>&&, RRefType<ReferenceType<Ref, V>>&&> &&
            std::common_reference_with<RRefType<ReferenceType<Ref, V>>&&, const ValueType<Ref, V>&>;

        template<typename Allocator>
        concept ValidGeneratorAllocator = std::is_void_v<Allocator> ||
                                          Sora::Concept::RawPointerRebindableAllocator<Allocator, std::byte>;

        template<std::size_t Alignment>
        concept ValidGeneratorFrameAlignment = Alignment >= alignof(std::max_align_t) && std::has_single_bit(Alignment);

        /** @} ————————————————————————————————————————————————————————————————— */

        /** @name Allocator Support @{ ————————————————————————————————————————— */

        /**
         * @brief Empty allocation hook for unsupported allocator specialisations.
         * @tparam Allocator Allocator parameter.
         * @tparam FrameAlignment Required coroutine-frame alignment.
         */
        template<typename Allocator, std::size_t FrameAlignment>
        struct AllocatorSupport {};

        /**
         * @brief Coroutine-frame allocation hook for non-void allocators.
         * @details The allocator is rebound to @c std::byte and stored
         * immediately before the compiler-allocated coroutine frame.
         * @tparam Allocator Allocator parameter.
         * @tparam FrameAlignment Required coroutine-frame alignment.
         */
        template<typename Allocator, std::size_t FrameAlignment>
            requires(!std::is_void_v<Allocator>)
        struct AllocatorSupport<Allocator, FrameAlignment> {
        private:
            /** @brief Byte allocator used to reserve raw coroutine-frame storage. */
            using ByteAlloc = typename std::allocator_traits<Allocator>::template rebind_alloc<std::byte>;

            /** @brief Traits type for @c ByteAlloc. */
            using ByteTraits = std::allocator_traits<ByteAlloc>;

            static_assert(std::is_pointer_v<typename ByteTraits::pointer>,
                          "Generator coroutine-frame allocators must use true pointers");

            struct FrameHeader {
                FrameHeader(ByteAlloc&& allocator, std::byte* allocation, size_t allocationSize) noexcept
                    : allocator(std::move(allocator)), allocation(allocation), allocationSize(allocationSize) {}

                ByteAlloc allocator;
                std::byte* allocation;
                size_t allocationSize;
            };

            static_assert(std::is_nothrow_move_constructible_v<ByteAlloc> && std::is_nothrow_destructible_v<ByteAlloc>,
                          "Generator coroutine-frame allocators must move and destruct without throwing");

            static constexpr size_t kFrameAlignment = FrameAlignment > alignof(FrameHeader) ? FrameAlignment
                                                                                            : alignof(FrameHeader);

            [[nodiscard]] static consteval size_t AlignUp(size_t value, size_t alignment) noexcept {
                return (value + alignment - 1) & ~(alignment - 1);
            }

            static constexpr size_t kHeaderSize = AlignUp(sizeof(FrameHeader), kFrameAlignment);

            /**
             * @brief Allocate and initialise raw storage for a coroutine frame.
             * @param[in] alloc Byte allocator object.
             * @param[in] frameSize Compiler-requested coroutine frame size.
             * @return Pointer to the coroutine frame region.
             */
            static void* AllocFrame(ByteAlloc alloc, size_t frameSize) {
                constexpr size_t alignmentPadding = kFrameAlignment - 1;
                constexpr size_t fixedOverhead = kHeaderSize + alignmentPadding;
                if (frameSize > std::numeric_limits<size_t>::max() - fixedOverhead) {
                    throw std::bad_array_new_length{};
                }

                const size_t allocationSize = fixedOverhead + frameSize;
                std::byte* allocation = ByteTraits::allocate(alloc, allocationSize);
                void* frame = allocation + kHeaderSize;
                size_t available = allocationSize - kHeaderSize;
                frame = std::align(kFrameAlignment, frameSize, frame, available);
                auto* header = reinterpret_cast<FrameHeader*>(static_cast<std::byte*>(frame) - kHeaderSize);
                std::construct_at(header, std::move(alloc), allocation, allocationSize);
                return frame;
            }

            static void FreeFrame(void* ptr) noexcept {
                auto* header = std::launder(reinterpret_cast<FrameHeader*>(static_cast<std::byte*>(ptr) - kHeaderSize));
                ByteAlloc allocator(std::move(header->allocator));
                std::byte* allocation = header->allocation;
                const size_t allocationSize = header->allocationSize;
                std::destroy_at(header);
                ByteTraits::deallocate(allocator, allocation, allocationSize);
            }

        public:
            /**
             * @brief Allocate a coroutine frame using a default-constructed allocator.
             * @param[in] frameSize Compiler-requested coroutine frame size.
             * @return Pointer to the coroutine frame region.
             */
            static void* operator new(size_t frameSize)
                requires std::default_initializable<ByteAlloc>
            {
                return AllocFrame(ByteAlloc{}, frameSize);
            }

            /**
             * @brief Allocate a coroutine frame using an allocator argument.
             * @param[in] frameSize Compiler-requested coroutine frame size.
             * @param[in] a Allocator object supplied by the coroutine function.
             * @return Pointer to the coroutine frame region.
             */
            template<typename... Args>
            static void* operator new(size_t frameSize, std::allocator_arg_t, const Allocator& a, Args&&...) {
                return AllocFrame(ByteAlloc{a}, frameSize);
            }

            /**
             * @brief Allocate a member-coroutine frame using an allocator argument.
             * @param[in] frameSize Compiler-requested coroutine frame size.
             * @param[in] a Allocator object supplied by the coroutine function.
             * @return Pointer to the coroutine frame region.
             */
            template<typename This, typename... Args>
            static void* operator new(size_t frameSize, This&, std::allocator_arg_t, const Allocator& a, Args&&...) {
                return AllocFrame(ByteAlloc{a}, frameSize);
            }

            /**
             * @brief Destroy the stored allocator and release the coroutine frame.
             * @param[in] ptr Pointer previously returned by @c operator new.
             * @param[in] frameSize Compiler-requested coroutine frame size.
             */
            static void operator delete(void* ptr, size_t) noexcept { FreeFrame(ptr); }

            /** @brief Release a coroutine frame when the compiler selects unsized deallocation. */
            static void operator delete(void* ptr) noexcept { FreeFrame(ptr); }
        };

        /**
         * @brief Alignment-aware global allocation hook for a @c void allocator.
         */
        template<std::size_t FrameAlignment>
        struct AllocatorSupport<void, FrameAlignment> {
            static void* operator new(size_t frameSize) {
                if constexpr (FrameAlignment > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
                    return ::operator new(frameSize, std::align_val_t{FrameAlignment});
                } else {
                    return ::operator new(frameSize);
                }
            }

            static void operator delete(void* ptr, size_t) noexcept {
                if constexpr (FrameAlignment > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
                    ::operator delete(ptr, std::align_val_t{FrameAlignment});
                } else {
                    ::operator delete(ptr);
                }
            }

            static void operator delete(void* ptr) noexcept {
                if constexpr (FrameAlignment > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
                    ::operator delete(ptr, std::align_val_t{FrameAlignment});
                } else {
                    ::operator delete(ptr);
                }
            }
        };

        /** @} ————————————————————————————————————————————————————————————————— */

        /** @name Promise Type @{ —————————————————————————————————————————————— */

        /**
         * @brief Coroutine promise backing @c Generator.
         * @tparam Ref Reference parameter.
         * @tparam V Explicit value parameter, or @c void.
         * @tparam Allocator Allocator parameter.
         * @tparam FrameAlignment Required coroutine-frame alignment.
         */
        template<typename Ref, typename V, typename Allocator, std::size_t FrameAlignment>
        class alignas(FrameAlignment) PromiseType : public AllocatorSupport<Allocator, FrameAlignment> {
        public:
            /** @brief Public generator type associated with this promise. */
            using generator_type = Generator<Ref, V, Allocator, FrameAlignment>;

            /** @brief Iterator reference type produced by this promise. */
            using reference = ReferenceType<Ref, V>;

            /** @brief Type accepted by direct @c co_yield expressions. */
            using yielded = YieldedType<reference>;

            /** @brief Range value type associated with this promise. */
            using value_type = ValueType<Ref, V>;

        private:
            /**
             * @brief Pointer to the currently yielded value.
             * @details All yields, including nested yields, write to
             * @c root_->value_ so the iterator always reads from the root promise.
             */
            std::add_pointer_t<yielded> value_ = nullptr;

            /** @brief Exception captured while this promise is delegated by a parent generator. */
            std::exception_ptr except_{};

            /** @brief Outermost promise in the recursive delegation chain. */
            PromiseType* root_ = this;

            /** @brief Enclosing promise in the recursive delegation chain. */
            PromiseType* parent_ = nullptr;

            /** @brief Root-only handle of the innermost coroutine resumed next. */
            std::coroutine_handle<PromiseType> active_{};

            friend generator_type;

            template<typename, typename, typename, std::size_t>
            friend class PromiseType;

        public:
            /** @brief Construct a promise with an empty delegation chain. */
            PromiseType() = default;

            /** @brief Promise objects are not movable. */
            PromiseType(PromiseType&&) = delete;

            /** @brief Promise objects are not copyable. */
            PromiseType(const PromiseType&) = delete;

            /** @brief Promise objects are not move-assignable. */
            PromiseType& operator=(PromiseType&&) = delete;

            /** @brief Promise objects are not copy-assignable. */
            PromiseType& operator=(const PromiseType&) = delete;

            /** @name Coroutine Interface @{ —————————————————————————————————— */

            /**
             * @brief Create the public generator object for this coroutine.
             * @return Generator owning this coroutine handle.
             */
            generator_type get_return_object() noexcept {
                auto h = std::coroutine_handle<PromiseType>::from_promise(*this);
                /** @brief The root coroutine starts as the active coroutine. */
                active_ = h;
                return generator_type{h};
            }

            /**
             * @brief Lazily suspend before executing the coroutine body.
             * @return Always-suspend awaiter.
             */
            std::suspend_always initial_suspend() noexcept { return {}; }

            /** @brief Complete a generator that reaches @c co_return. */
            void return_void() noexcept {}

            /** @brief Propagate a root exception directly or defer a nested exception to the parent awaiter. */
            void unhandled_exception() {
                if (root_ == this) {
                    throw;
                }
                except_ = std::current_exception();
            }

            /** @} ————————————————————————————————————————————————————————————— */

            /** @name Element Yielding @{ ————————————————————————————————————— */

            /**
             * @brief Yield a value whose type already matches @c yielded.
             * @details This overload stores the address of the yielded object in
             * the root promise. The yielded object is required to remain alive
             * across the suspension point according to coroutine yield semantics.
             * @param[in] val Yielded object.
             * @return Always-suspend awaiter.
             */
            std::suspend_always yield_value(yielded val) noexcept
                requires std::is_reference_v<yielded>
            {
                root_->value_ = std::addressof(val);
                return {};
            }

            /**
             * @brief Awaiter that owns a materialised yielded value.
             * @details Used when the source expression cannot bind directly to
             * @c yielded and must be converted into coroutine-frame storage.
             */
            struct ValueAwaiter {
                /** @brief Materialised value kept alive across suspension. */
                std::remove_cvref_t<yielded> stored;

                /** @brief Root promise receiving @c stored as the current value. */
                PromiseType* root;

                /** @brief The awaiter always suspends at a yield point. */
                bool await_ready() noexcept { return false; }

                /**
                 * @brief Publish the materialised value to the root promise.
                 * @param[in] Awaiting coroutine handle, unused.
                 */
                void await_suspend(std::coroutine_handle<>) noexcept { root->value_ = std::addressof(stored); }

                /** @brief Resume without producing an additional result. */
                void await_resume() noexcept {}
            };

            /**
             * @brief Yield a value after materialising a converted temporary.
             * @tparam U Source expression type.
             * @param[in] val Source value.
             * @return Awaiter that owns the converted value across suspension.
             */
            template<typename U>
                requires(!std::same_as<std::remove_cvref_t<U>, PromiseType>) &&
                        (!std::is_reference_v<yielded> || !std::same_as<U, yielded>) &&
                        std::constructible_from<std::remove_cvref_t<yielded>, U>
            ValueAwaiter
            yield_value(U&& val) noexcept(std::is_nothrow_constructible_v<std::remove_cvref_t<yielded>, U>) {
                return {std::remove_cvref_t<yielded>(std::forward<U>(val)), root_};
            }

            /** @} ————————————————————————————————————————————————————————————— */

            /** @name Recursive Delegation @{ ————————————————————————————————— */

            /**
             * @brief Awaiter that transfers execution to a nested generator.
             * @tparam Gen Nested generator type.
             */
            template<typename Gen>
            struct DelegationAwaiter {
                /** @brief Nested generator being delegated. */
                Gen nested;

                /** @brief Empty and exhausted generators require no delegation transfer. */
                bool await_ready() const noexcept { return !nested.coroutine_ || nested.coroutine_.done(); }

                /**
                 * @brief Wire the nested promise and transfer execution to it.
                 * @param[in] parent Parent coroutine handle.
                 * @return Coroutine handle to resume via symmetric transfer.
                 */
                std::coroutine_handle<> await_suspend(std::coroutine_handle<PromiseType> parent) noexcept {
                    auto childHandle = nested.coroutine_;
                    auto& childPromise = childHandle.promise();
                    auto& parentPromiseRef = parent.promise();

                    /** @brief Attach the child promise to the parent's root chain. */
                    childPromise.root_ = parentPromiseRef.root_;
                    childPromise.parent_ = &parentPromiseRef;

                    /** @brief Make the child the active leaf coroutine. */
                    parentPromiseRef.root_->active_ = childHandle;

                    /** @brief Resume the child coroutine via symmetric transfer. */
                    return childHandle;
                }

                /** @brief Continue the parent, rethrowing delegated exceptions first. */
                void await_resume() {
                    if (nested.coroutine_) {
                        auto& childPromise = nested.coroutine_.promise();
                        if (auto ex = std::exchange(childPromise.except_, {})) {
                            std::rethrow_exception(ex);
                        }
                    }
                }
            };

            /**
             * @brief Delegate directly to another generator of the same type.
             * @param[in] eo Delegation wrapper.
             * @return Awaiter that transfers execution to the nested generator.
             */
            DelegationAwaiter<generator_type> yield_value(ElementsOf<generator_type> eo) noexcept {
                return {std::move(eo.range)};
            }

            /**
             * @brief Delegate directly to a same-typed generator with an ignored allocator tag.
             * @tparam Alloc Allocator tag carried by @c ElementsOf.
             * @param[in] eo Delegation wrapper.
             * @return Awaiter that transfers execution to the nested generator.
             */
            template<typename Alloc>
            DelegationAwaiter<generator_type> yield_value(ElementsOf<generator_type, Alloc> eo) noexcept {
                return {std::move(eo.range)};
            }

            /**
             * @brief Adapt an arbitrary range into a same-typed generator and delegate it.
             * @tparam Rng Delegated range type.
             * @tparam Alloc Optional allocator type carried by @c ElementsOf.
             * @param[in] eo Delegation wrapper.
             * @return Awaiter that transfers execution to the adapted generator.
             */
            template<std::ranges::range Rng, typename Alloc>
                requires(!std::same_as<std::remove_cvref_t<Rng>, generator_type>)
            DelegationAwaiter<generator_type> yield_value(ElementsOf<Rng, Alloc> eo) {
                auto adapt = [](Rng rng) -> generator_type {
                    for (auto&& elem : rng) {
                        co_yield static_cast<decltype(elem)>(elem);
                    }
                };

                if constexpr (std::is_void_v<Alloc>) {
                    return {adapt(std::forward<Rng>(eo.range))};
                } else if constexpr (!std::is_void_v<Allocator> && std::constructible_from<Allocator, const Alloc&>) {
                    auto adaptWithAllocator = [](std::allocator_arg_t, Allocator, Rng rng) -> generator_type {
                        for (auto&& elem : rng) {
                            co_yield static_cast<decltype(elem)>(elem);
                        }
                    };
                    return {
                        adaptWithAllocator(std::allocator_arg, Allocator{eo.allocator}, std::forward<Rng>(eo.range))};
                } else {
                    static_assert(!std::is_void_v<Allocator> && std::constructible_from<Allocator, const Alloc&>,
                                  "ElementsOf(range, alloc) requires the parent Generator "
                                  "Allocator to be constructible from alloc.");
                }
            }

            /** @brief Disable arbitrary @c co_await inside generator coroutines. */
            void await_transform() = delete;

            /** @} ————————————————————————————————————————————————————————————— */

            /** @name Final Suspension @{ ————————————————————————————————————— */

            /**
             * @brief Awaiter used at final suspension.
             * @details Nested coroutines restore the root active handle to their
             * parent and symmetric-transfer back to that parent. The root
             * coroutine returns @c std::noop_coroutine so iterator resumption
             * returns to the caller.
             */
            struct FinalAwaiter {
                /** @brief Final suspension always suspends. */
                constexpr bool await_ready() noexcept { return false; }

                /**
                 * @brief Restore delegation state and choose the continuation.
                 * @param[in] h Coroutine finishing final suspension.
                 * @return Continuation handle for symmetric transfer.
                 */
                std::coroutine_handle<> await_suspend(std::coroutine_handle<PromiseType> h) noexcept {
                    auto& promise = h.promise();
                    auto* parent = promise.parent_;

                    if (parent) {
                        /** @brief Restore the parent as the active coroutine. */
                        auto parentHandle = std::coroutine_handle<PromiseType>::from_promise(*parent);
                        promise.root_->active_ = parentHandle;

                        /** @brief Resume the parent at the delegation awaiter. */
                        return parentHandle;
                    }

                    /** @brief Return control to the iterator resume caller. */
                    return std::noop_coroutine();
                }

                /** @brief Final suspension produces no result. */
                void await_resume() noexcept {}
            };

            /**
             * @brief Suspend at coroutine completion.
             * @return Final-suspend awaiter.
             */
            FinalAwaiter final_suspend() noexcept { return {}; }
        };

        /** @} ———————————————————————————————————————————————————————————————— */

    } // namespace Detail::Gen
    /** @endcond */

    /** @name Generator Range @{ ——————————————————————————————————————————————— */

    /**
     * @brief Synchronous coroutine generator for ranges.
     *
     * @details A move-only coroutine range modelled after @c std::generator
     * (P2502), targeting the project's C++26 toolchain.
     *
     * @tparam Ref The reference type yielded by the generator. This controls
     * what @c *it returns. If @p V is @c void, @c reference is @c Ref&&.
     * @tparam V The value type. If @c void, it is deduced as
     * @c std::remove_cvref_t<Ref>. Use this to avoid dangling views, such as
     * @c Generator<std::string_view,std::string>.
     * @tparam Allocator Custom allocator for the coroutine frame, or @c void
     * for global allocation.
     * @tparam FrameAlignment Maximum alignment required by the promise and coroutine-frame locals. Coroutine allocation
     * receives only the frame size, so a body containing over-aligned state must state that alignment explicitly.
     *
     * @code{.cpp}
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
     * Generator<const int&> tree_inorder(const Tree& t) {
     *     if (t.left)  co_yield ElementsOf(tree_inorder(*t.left));
     *     co_yield t.value;
     *     if (t.right) co_yield ElementsOf(tree_inorder(*t.right));
     * }
     * @endcode
     */
    template<typename Ref, typename V, typename Allocator, std::size_t FrameAlignment>
    class Generator {
        static_assert(Detail::Gen::ValidGeneratorParams<Ref, V>,
                      "Generator<Ref, V>: invalid parameter combination. "
                      "Ref/V must satisfy the common_reference_with constraints.");
        static_assert(Detail::Gen::ValidGeneratorAllocator<Allocator>,
                      "Generator coroutine-frame allocators must rebind to std::byte with a true pointer type");
        static_assert(Detail::Gen::ValidGeneratorFrameAlignment<FrameAlignment>,
                      "Generator frame alignment must be a power of two no smaller than alignof(max_align_t)");

    public:
        /** @name Public Types @{ ————————————————————————————————————————————— */

        /** @brief Coroutine promise type associated with this generator. */
        using promise_type = Detail::Gen::PromiseType<Ref, V, Allocator, FrameAlignment>;

        /** @brief Range value type associated with this generator. */
        using value_type = Detail::Gen::ValueType<Ref, V>;

        /** @brief Iterator dereference type associated with this generator. */
        using reference = Detail::Gen::ReferenceType<Ref, V>;

        /** @brief Type accepted by direct @c co_yield expressions. */
        using yielded = Detail::Gen::YieldedType<reference>;

        /** @brief Declared maximum alignment for the compiler-owned coroutine frame. */
        static constexpr std::size_t kFrameAlignment = FrameAlignment;

        /** @} ——————————————————————————————————————————————————————————————— */

    private:
        /** @brief Owned root coroutine handle. */
        std::coroutine_handle<promise_type> coroutine_ = nullptr;

        friend promise_type;

        template<typename, typename, typename, std::size_t>
        friend class Detail::Gen::PromiseType;

        /**
         * @brief Construct from an already-created coroutine handle.
         * @param[in] h Coroutine handle to own.
         */
        explicit Generator(std::coroutine_handle<promise_type> h) noexcept : coroutine_(h) {}

    public:
        /** @brief Construct an empty generator. */
        Generator() noexcept = default;

        /**
         * @brief Move-construct by transferring coroutine ownership.
         * @param[in,out] other Source generator.
         */
        Generator(Generator&& other) noexcept : coroutine_(std::exchange(other.coroutine_, nullptr)) {}

        /**
         * @brief Move-assign by destroying the current coroutine and taking ownership.
         * @param[in,out] other Source generator.
         * @return This generator.
         */
        Generator& operator=(Generator&& other) noexcept {
            if (this != &other) {
                if (coroutine_) {
                    coroutine_.destroy();
                }
                coroutine_ = std::exchange(other.coroutine_, nullptr);
            }
            return *this;
        }

        /** @brief Generators are move-only. */
        Generator(const Generator&) = delete;

        /** @brief Generators are move-only. */
        Generator& operator=(const Generator&) = delete;

        /** @brief Destroy the owned coroutine frame, if any. */
        ~Generator() {
            if (coroutine_) {
                coroutine_.destroy();
            }
        }

        /** @name Iterator Types @{ ——————————————————————————————————————————— */

        /** @brief Sentinel for the end of the generator range. */
        struct Sentinel {};

        /**
         * @brief Move-only input iterator over the generator.
         * @details The iterator stores the root coroutine handle. Resumption uses
         * @c root.promise().active_, while dereference reads @c root.promise().value_.
         */
        class Iterator {
        public:
            /** @brief Iterator category used by C++20 ranges. */
            using iterator_concept = std::input_iterator_tag;

            /** @brief Difference type for iterator traits. */
            using difference_type = std::ptrdiff_t;

            /** @brief Value type for iterator traits. */
            using value_type = Generator::value_type;

        private:
            /** @brief Root coroutine handle observed by this iterator. */
            std::coroutine_handle<promise_type> coroutine_ = nullptr;

            friend Generator;

            /**
             * @brief Construct an iterator from a root coroutine handle.
             * @param[in] h Root coroutine handle.
             */
            explicit Iterator(std::coroutine_handle<promise_type> h) noexcept : coroutine_(h) {}

            /** @brief Resume the active coroutine and propagate any exception. */
            void ResumeActive() {
                if (!coroutine_) {
                    return;
                }
                coroutine_.promise().active_.resume();
            }

        public:
            /** @brief Construct a default end-like iterator. */
            Iterator() noexcept = default;

            /**
             * @brief Move-construct by transferring the observed handle.
             * @param[in,out] other Source iterator.
             */
            Iterator(Iterator&& other) noexcept : coroutine_(std::exchange(other.coroutine_, nullptr)) {}

            /**
             * @brief Move-assign by transferring the observed handle.
             * @param[in,out] other Source iterator.
             * @return This iterator.
             */
            Iterator& operator=(Iterator&& other) noexcept {
                coroutine_ = std::exchange(other.coroutine_, nullptr);
                return *this;
            }

            /**
             * @brief Access the currently yielded value.
             * @return Current yielded reference.
             */
            [[nodiscard]] reference operator*() const noexcept {
                return static_cast<reference>(*coroutine_.promise().value_);
            }

            /**
             * @brief Resume the active coroutine to produce the next value.
             * @return This iterator.
             */
            Iterator& operator++() {
                ResumeActive();
                return *this;
            }

            /** @brief Post-increment for input-iterator syntax. */
            void operator++(int) { ++*this; }

            /**
             * @brief Test whether an iterator has reached the sentinel.
             * @param[in] it Iterator to test.
             * @return True when @p it is empty or the root coroutine is done.
             */
            [[nodiscard]] friend bool operator==(const Iterator& it, Sentinel) noexcept {
                return !it.coroutine_ || it.coroutine_.done();
            }
        };

        /** @} ——————————————————————————————————————————————————————————————— */

        /** @name Range Interface @{ —————————————————————————————————————————— */

        /**
         * @brief Start generator iteration and produce the first value.
         * @return Iterator positioned at the first value, or at end if complete.
         */
        [[nodiscard]] Iterator begin() {
            if (coroutine_) {
                Iterator it{coroutine_};
                it.ResumeActive();
                return it;
            }
            return Iterator{nullptr};
        }

        /**
         * @brief Return the generator sentinel.
         * @return End sentinel.
         */
        [[nodiscard]] static constexpr Sentinel end() noexcept { return {}; }

        /** @} ——————————————————————————————————————————————————————————————— */

        /** @name Utility @{ —————————————————————————————————————————————————— */

        /**
         * @brief Test whether this generator owns a coroutine handle.
         * @return True if the generator is non-empty.
         */
        [[nodiscard]] explicit operator bool() const noexcept { return coroutine_ != nullptr; }

        /** @} ——————————————————————————————————————————————————————————————— */
    };

    /** @} ————————————————————————————————————————————————————————————————————— */

} // namespace Sora

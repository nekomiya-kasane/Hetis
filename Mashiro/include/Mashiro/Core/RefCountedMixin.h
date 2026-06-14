/**
 * @file RefCountedMixin.h
 * @brief CRTP intrusive reference counting — single-threaded and atomic flavours.
 *
 * Two CRTP mixins that drop a self-managed reference counter into a derived class:
 *   - @ref RefCounted        — single-threaded counter (no atomics, no synchronisation cost).
 *   - @ref RefCountedAtomic  — thread-safe counter using the canonical `relaxed`-add /
 *                              `acq_rel`-sub / `acquire`-fence pattern.
 *
 * Both flavours follow the **intrusive** model: the count lives on the object, not on a separate
 * control block. That keeps allocations down to one per object (vs. `std::shared_ptr`'s two) and
 * shrinks the per-instance overhead to one integer of width `Bits` — typically @c uint16_t for
 * UI / scene-graph nodes, @c uint32_t for engine-wide handles.
 *
 * @par Usage contract (read this once)
 * - Derive via CRTP: `struct Node : RefCounted<Node> { ... }`. The mixin must be a **direct,
 *   public** base of @c T; no diamond, no protected derivation.
 * - **Always allocate with `new`.** @ref RefCounted::Release / @ref RefCountedAtomic::Release
 *   call `delete this` when the count hits zero, so the object must be a heap-managed
 *   `T*`. Stack-allocating, embedding by value, or owning through `unique_ptr` is **undefined
 *   behaviour**.
 * - The initial reference count is **1** — the conventional "the constructor returns one
 *   reference, the caller owns it". Pair every `new T(...)` with exactly one `Release()` (or
 *   wrap in an `IntrusivePtr<T>`-style RAII handle).
 * - `T` should not have a `virtual` destructor: the mixin static-casts to @c T* before
 *   `delete`, so the most-derived type is what gets destroyed. If you need polymorphic
 *   derivation under @c T, give @c T itself a `virtual` destructor; the mixin does not.
 *
 * @par Cost model
 * Storage:    one @c uint{8,16,32,64}_t per instance, depending on @c Bits.
 * AddRef:     one increment (or `fetch_add(relaxed)` for the atomic flavour).
 * Release:    one decrement; on transition to zero, one `acquire` fence (atomic flavour) and
 *             one `delete`. The fence-on-zero pattern is the canonical Boost-style intrusive
 *             release: the `acq_rel` decrement publishes prior writes, the trailing `acquire`
 *             fence ensures the destructor sees every other thread's prior modifications before
 *             tearing down the object.
 *
 * @par Why two types instead of a `Storage` policy parameter
 * The two flavours differ only in the underlying counter type and the memory-ordering ceremony.
 * A single class template parameterised on a `Storage` policy would unify them, but at the cost
 * of obscuring the user-visible distinction (which is a *thread-safety* contract, not a tuning
 * knob). Keeping them as two named types makes the choice explicit at every derivation site.
 *
 * @ingroup Core
 */
#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Mashiro {

    namespace Traits {

        /**
         * @brief Smallest unsigned integer type capable of holding a @p Bits-wide value.
         *
         * Selects @c uint8_t / @c uint16_t / @c uint32_t / @c uint64_t. The mixin uses this to
         * size the per-instance counter according to the user's expected reference-graph
         * depth: a UI tree rarely needs more than 65k references, so a 16-bit counter halves
         * the storage cost compared to a default @c uint32_t.
         *
         * @tparam Bits Counter width in bits. Must be one of @c {8, 16, 32, 64}; an unsupported
         *              value selects @c uint64_t to fail safe.
         */
        template<std::size_t Bits = 16>
        using BestSizeType = std::conditional_t<
            (Bits <= 8),  std::uint8_t,
            std::conditional_t<(Bits <= 16), std::uint16_t,
            std::conditional_t<(Bits <= 32), std::uint32_t,
                                              std::uint64_t>>>;

    } // namespace Traits

    /**
     * @brief CRTP mixin adding a single-threaded intrusive reference count to @p T.
     *
     * Use this flavour when ownership transitions are confined to one thread (UI nodes manipulated
     * exclusively on the UI thread, scene-graph edits during a single frame, etc.). It avoids
     * every synchronisation cost a thread-safe counter would otherwise impose.
     *
     * @tparam T    The CRTP-derived class. Must be the most-derived type and must be allocated
     *              with @c new before the first @ref AddRef / @ref Release pair.
     * @tparam Bits Counter width in bits (default 16). See @ref Traits::BestSizeType.
     *
     * @code
     * struct UiNode : RefCounted<UiNode> {
     *     std::string label;
     * };
     * UiNode* node = new UiNode{};   // refCount == 1
     * node->AddRef();                // 2
     * node->Release();               // 1
     * node->Release();               // 0 -> delete
     * @endcode
     */
    template<typename T, std::size_t Bits = 16>
    struct RefCounted {
        using SizeType = Traits::BestSizeType<Bits>;

        /// @brief Increment the reference count.
        constexpr void AddRef() const noexcept { ++refCount; }

        /// @brief Decrement the reference count; on transition to zero, @c delete the @c T object.
        ///
        /// @warning The object **must** have been allocated with @c new (and never embedded by
        ///          value, on the stack, or in another container that owns its storage). Calling
        ///          @c Release on a non-heap @c T is undefined behaviour.
        constexpr void Release() const noexcept(std::is_nothrow_destructible_v<T>) {
            if (--refCount == 0) [[unlikely]] {
                delete static_cast<const T*>(this);
            }
        }

        /// @brief Snapshot of the current reference count.
        [[nodiscard]] constexpr SizeType RefCount() const noexcept { return refCount; }

    private:
        mutable SizeType refCount = 1;
    };

    /**
     * @brief CRTP mixin adding a thread-safe intrusive reference count to @p T.
     *
     * Use this flavour when an object's ownership crosses thread boundaries (asset registries,
     * worker-pool tasks, anything published into a concurrent container). The atomic protocol is
     * the canonical Boost-style intrusive release:
     *
     *   - @ref AddRef : `fetch_add(relaxed)` — incrementing a non-zero count never publishes
     *                   ordering (the existing reference's writes are already observable).
     *   - @ref Release: `fetch_sub(acq_rel)` — release pairs with the matching acquire fence
     *                   on the destroying thread, so the destructor observes all prior writes
     *                   from every thread that ever held a reference.
     *   - On reaching zero, an explicit `atomic_thread_fence(acquire)` is issued before @c delete
     *     to gather those writes before destruction begins.
     *
     * @tparam T    The CRTP-derived class. Must be the most-derived type and must be allocated
     *              with @c new before the first @ref AddRef / @ref Release pair.
     * @tparam Bits Counter width in bits (default 16). See @ref Traits::BestSizeType.
     */
    template<typename T, std::size_t Bits = 16>
    struct RefCountedAtomic {
        using SizeType = Traits::BestSizeType<Bits>;

        /// @brief Atomically increment the reference count (relaxed — no ordering needed for add).
        constexpr void AddRef() const noexcept {
            refCount.fetch_add(1, std::memory_order_relaxed);
        }

        /// @brief Atomically decrement; on transition to zero, fence and @c delete the @c T object.
        ///
        /// @warning The object **must** have been allocated with @c new. See @ref RefCounted for
        ///          the full ownership contract.
        constexpr void Release() const noexcept(std::is_nothrow_destructible_v<T>) {
            if (refCount.fetch_sub(1, std::memory_order_acq_rel) == 1) [[unlikely]] {
                // Ensure every other thread's writes through this object are visible to ~T.
                std::atomic_thread_fence(std::memory_order_acquire);
                delete static_cast<const T*>(this);
            }
        }

        /// @brief Snapshot of the current reference count (relaxed; for diagnostics, not synchronisation).
        [[nodiscard]] constexpr SizeType RefCount() const noexcept {
            return refCount.load(std::memory_order_relaxed);
        }

    private:
        mutable std::atomic<SizeType> refCount{1};
    };

    // -----------------------------------------------------------------------------
    // Compile-time invariants
    // -----------------------------------------------------------------------------

    /** @cond INTERNAL */

    // Width must be one of the four supported sizes (otherwise BestSizeType silently picks u64,
    // which masks intent and slack-checks at the call site).
    template<std::size_t Bits>
    concept SupportedRefCountWidth = (Bits == 8 || Bits == 16 || Bits == 32 || Bits == 64);

    // The mixins must be cheap and standard-layout-friendly. We pick a representative
    // instantiation rather than constraining the templates themselves, because the contract
    // depends on the *derived* T (which is incomplete at the mixin's template-parameter scope).
    namespace Detail {
        struct ProbeT {};
    }

    static_assert(sizeof(RefCounted<Detail::ProbeT, 16>)       == sizeof(std::uint16_t));
    static_assert(sizeof(RefCounted<Detail::ProbeT, 32>)       == sizeof(std::uint32_t));
    static_assert(std::is_trivially_destructible_v<RefCounted<Detail::ProbeT>>);
    static_assert(std::is_standard_layout_v<RefCounted<Detail::ProbeT>>);
    static_assert(!std::is_polymorphic_v<RefCounted<Detail::ProbeT>>);

    static_assert(std::is_trivially_destructible_v<RefCountedAtomic<Detail::ProbeT>>);
    static_assert(!std::is_polymorphic_v<RefCountedAtomic<Detail::ProbeT>>);

    /** @endcond */

} // namespace Mashiro

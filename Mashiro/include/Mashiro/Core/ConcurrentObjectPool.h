/**
 * @file ConcurrentObjectPool.h
 * @brief Lock-free, fixed-capacity object pool with versioned handles for safe
 *        multi-producer / multi-consumer slot recycling.
 *
 * The pool owns a fixed number of slots, each holding one manually-lifetime
 * `T`. Free slots are threaded onto a lock-free Treiber free-list keyed by slot
 * *index* (never raw pointers), so the backing storage can live inline or on the
 * heap without changing the algorithm. Three design pillars:
 *
 * - **Versioned handles** — every @ref PoolHandle carries a 16-bit generation
 *   that is bumped each time its slot is released. @ref ConcurrentObjectPool::Deref
 *   compares the handle's generation against the slot's live generation, so a
 *   stale handle to a recycled slot is rejected (probabilistic use-after-free
 *   detection, the EnTT / Bevy versioned-handle pattern).
 * - **ABA-safe free-list** — the free-list head is a single 64-bit atomic packing
 *   `[aba-tag | slot-index]`. The tag is bumped on every push so a popped-then-
 *   repushed head fails the CAS instead of silently succeeding (the classic
 *   tagged-pointer ABA mitigation). The bit split is computed at compile time by
 *   @ref Detail::ComputeAbaLayout from the configured capacity.
 * - **SoA storage** — slot payloads, free-list links and generations live in
 *   three parallel arrays so free-list traversal and generation checks never pull
 *   `sizeof(T)`-strided cache lines, and the contended head sits alone on its line.
 *
 * ### Concurrency contract
 * `Emplace` / `Acquire`, `Release`, `Deref` and the bulk variants are all safe to
 * call concurrently from any number of threads (MPMC). Reads through a `T*`
 * obtained from @ref ConcurrentObjectPool::Deref are the caller's responsibility to
 * synchronise; the pool only guarantees the *slot* is not recycled out from under
 * a live handle without a detectable generation change.
 *
 * ### Compile-time guarantees (C++26 reflection)
 * The contended free-list head is tagged with the project-wide
 * @ref Concurrency::Contended domain and the cold SoA storage with
 * @ref Concurrency::SharedStorage (see @ref FalseSharing.h).
 * @ref Concurrency::AuditFalseSharing proves — via P2996 static reflection — that
 * these two writer domains never share a cache line, and
 * @ref Concurrency::DomainStartsLine that the head begins its own line. The audit
 * runs in the `consteval` block at the end of this header for representative
 * instantiations.
 *
 * @ingroup Core
 */
#pragma once

#include "Mashiro/Core/FalseSharing.h"
#include "Mashiro/Core/TypeTraits.h"
#include "Mashiro/Core/Result.h"

#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <array>
#include <memory>
#include <meta>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace Mashiro {

    namespace Traits {

        /**
         * @brief Element requirements for @ref ConcurrentObjectPool.
         *
         * A poolable type must be movable (slots are filled and drained by move),
         * and both its move-construction and destruction must be `noexcept` — the
         * pool runs them on its lock-free fast paths where an exception could not
         * be unwound without leaking the slot.
         */
        template<typename T>
        concept Poolable = std::movable<T> &&                  //
                           std::is_nothrow_move_constructible_v<T> && //
                           std::is_nothrow_destructible_v<T>;

    } // namespace Traits

    namespace Detail {

        /// @brief Where a pool's backing storage lives.
        enum class BackingKind : uint8_t {
            Inline, ///< Storage embedded directly in the pool object (stack/static friendly).
            Heap,   ///< Storage allocated on the heap (large capacities, small pool object).
        };

        /**
         * @brief Compile-time configuration for @ref ConcurrentObjectPool.
         *
         * Passed as a non-type template parameter so every field participates in
         * overload resolution and `if constexpr` specialisation with zero runtime
         * cost. Validated by @ref ValidateConfig.
         */
        struct PoolConfig {
            size_t capacity = 1024;                      ///< Number of slots (1 .. 32767).
            BackingKind backing = BackingKind::Inline;   ///< Storage location.
            size_t alignment = 0;                        ///< Slot alignment; 0 ⇒ `alignof(T)`.
            bool enableStats = false;                    ///< Track acquire/release counters.
        };

        /// @brief Largest slot index representable by a 15-bit @ref Mashiro::PoolHandle field.
        inline constexpr size_t kMaxPoolCapacity = (size_t{1} << 15) - 1;

        /**
         * @brief Validate a @ref PoolConfig entirely at compile time.
         * @return true iff the capacity fits a handle's index field and the
         *         alignment (when non-zero) is a power of two.
         */
        consteval bool ValidateConfig(const PoolConfig& c) {
            const bool capacityOk = c.capacity >= 1 && c.capacity <= kMaxPoolCapacity;
            const bool alignOk = c.alignment == 0 || std::has_single_bit(c.alignment);
            return capacityOk && alignOk;
        }

        /**
         * @brief Compile-time bit layout of the packed free-list head word.
         *
         * The head is a 64-bit atomic split into a high *ABA tag* and a low *index*
         * field wide enough to encode every valid slot plus a `sentinel` value used
         * for the empty list. The tag occupies the remaining high bits and is bumped
         * on every push to defeat ABA.
         */
        struct AbaLayout {
            uint32_t indexBits = 0; ///< Width of the index field in bits.
            uint32_t tagBits = 0;   ///< Width of the ABA tag field in bits.
            uint64_t indexMask = 0; ///< Mask selecting the index field.
            uint64_t sentinel = 0;  ///< Reserved index value meaning "empty list".
        };

        /**
         * @brief Derive the @ref AbaLayout for a given slot capacity.
         *
         * `indexBits = bit_width(capacity)` so the field can hold every index in
         * `0 .. capacity-1` *and* the sentinel value `capacity` itself. The tag gets
         * all remaining bits; we require at least 32 tag bits so wrap-around is
         * astronomically unlikely on the contended path.
         */
        consteval AbaLayout ComputeAbaLayout(size_t capacity) {
            AbaLayout l{};
            l.indexBits = static_cast<uint32_t>(std::bit_width(capacity));
            l.tagBits = 64u - l.indexBits;
            l.indexMask = (uint64_t{1} << l.indexBits) - 1u;
            l.sentinel = static_cast<uint64_t>(capacity);
            return l;
        }

    } // namespace Detail

    // =========================================================================
    // =========================================================================
    // PoolHandle — versioned, type-erased reference into a pool slot
    // =========================================================================

    /**
     * @brief A 32-bit versioned handle into a @ref ConcurrentObjectPool slot.
     *
     * Bit layout (LSB-first): `[valid:1][index:15][generation:16]`. The
     * generation lets the owning pool detect a stale handle whose slot has since
     * been recycled. A default-constructed handle is **invalid** (`bits == 0`),
     * which is the canonical "null" value returned by a failed acquire.
     *
     * Handles are deliberately POD and pool-agnostic: they carry no back-pointer,
     * so they are 4 bytes, trivially copyable, and cheap to store en masse.
     *
     * @todo The index/generation split is fixed at 15/16 bits regardless of the
     *       pool's capacity. Since the capacity is known at compile time, the
     *       index field only needs `bit_width(capacity-1)` bits and every bit
     *       saved there could widen the generation field — directly enlarging the
     *       use-after-free / ABA detection window (a capacity-1024 pool wastes 5
     *       index bits that could grow the generation window 32×). Making the
     *       split capacity-adaptive (template the field widths, or demote
     *       `PoolHandle` to an opaque 32-bit token packed/unpacked by the pool)
     *       is deferred to keep handles pool-agnostic for now.
     */
    struct PoolHandle {
        uint32_t bits = 0; ///< Packed `[valid:1][index:15][generation:16]`.

        static constexpr uint32_t kValidMask = 0x0000'0001u;
        static constexpr uint32_t kIndexMask = 0x0000'FFFEu;
        static constexpr uint32_t kGenMask = 0xFFFF'0000u;
        static constexpr uint32_t kIndexShift = 1;
        static constexpr uint32_t kGenShift = 16;

        /// @brief True if this handle was produced by a successful acquire.
        [[nodiscard]] constexpr bool IsValid() const noexcept { return bits & kValidMask; }

        /// @brief Slot index in `0 .. capacity-1` (only meaningful when valid).
        [[nodiscard]] constexpr uint32_t Index() const noexcept {
            return (bits & kIndexMask) >> kIndexShift;
        }

        /// @brief Generation stamp recorded when the slot was acquired.
        [[nodiscard]] constexpr uint32_t Generation() const noexcept {
            return (bits & kGenMask) >> kGenShift;
        }

        /// @brief Build a valid handle for @p index with generation @p gen.
        [[nodiscard]] static constexpr PoolHandle Make(uint32_t index, uint32_t gen) noexcept {
            return PoolHandle{kValidMask | ((index << kIndexShift) & kIndexMask) |
                              ((gen << kGenShift) & kGenMask)};
        }

        [[nodiscard]] friend constexpr bool operator==(PoolHandle, PoolHandle) = default;
    };

    static_assert(std::is_trivially_copyable_v<PoolHandle> && sizeof(PoolHandle) == 4);

    /**
     * @brief Optional runtime counters, compiled in only when `enableStats`.
     *
     * Kept in a tiny struct so the storage can hold a `[[msvc::no_unique_address]]`
     * member that is empty (zero cost) when stats are disabled.
     */
    struct PoolStats {
        std::atomic<uint64_t> acquires{0}; ///< Successful slot acquisitions.
        std::atomic<uint64_t> releases{0}; ///< Slot releases.
        std::atomic<uint64_t> failures{0}; ///< Acquisitions that hit an empty pool.
    };

    /// @brief Empty stand-in used when stats are disabled (occupies no space).
    struct NoPoolStats {};

    namespace Detail {

        /**
         * @brief Inline (object-embedded) SoA storage for a pool.
         *
         * Three parallel arrays sit on a single cold cache region, well away from
         * the contended free-list head: raw payload bytes, free-list links, and
         * per-slot generations. Each array starts on its own cache line so a write
         * to one array (e.g. a `links` update on the alloc path) never invalidates
         * a neighbouring array's line at the array boundary — only the head's line
         * is shared by design (proven by @ref Concurrency::AuditFalseSharing).
         * Payload bytes are deliberately uninitialised — every slot is placement-new
         * constructed before it is read. Optional @p Stats live here too (empty and
         * free when disabled) so the pool object itself keeps exactly two members:
         * the hot head and this cold block.
         *
         * @note Adjacent slots within a single array still share cache lines; that
         *       is inherent to dense SoA and the price of compactness. Per-slot
         *       padding would bloat the pool with no benefit for the common case.
         *
         * @tparam T        Element type.
         * @tparam Capacity Slot count.
         * @tparam Align    Payload alignment (already resolved to a power of two).
         * @tparam Stats    Statistics struct (empty type when stats are disabled).
         */
        template<typename T, size_t Capacity, size_t Align, typename Stats>
        struct InlinePoolStore {
            alignas(Align) std::byte values[Capacity * sizeof(T)];
            alignas(Platform::kCacheLineSize) std::atomic<uint32_t> links[Capacity]{};
            alignas(Platform::kCacheLineSize) std::atomic<uint32_t> gens[Capacity]{};
            [[msvc::no_unique_address]] Stats stats{};

            [[nodiscard]] T* ValuePtr(uint32_t i) noexcept {
                return std::launder(reinterpret_cast<T*>(&values[size_t{i} * sizeof(T)]));
            }
            [[nodiscard]] std::atomic<uint32_t>& Link(uint32_t i) noexcept { return links[i]; }
            [[nodiscard]] const std::atomic<uint32_t>& Link(uint32_t i) const noexcept {
                return links[i];
            }
            [[nodiscard]] std::atomic<uint32_t>& Gen(uint32_t i) noexcept { return gens[i]; }
            [[nodiscard]] const std::atomic<uint32_t>& Gen(uint32_t i) const noexcept {
                return gens[i];
            }
        };

        /**
         * @brief Heap-allocated SoA storage for a pool (small pool object, large capacity).
         *
         * Same logical layout as @ref InlinePoolStore, but each array is a single
         * heap block. Arrays are value-initialised, so links and generations start
         * at zero; payload bytes are raw and placement-new constructed on demand.
         * Each array is an independent allocation, so the three arrays never share a
         * cache line at their boundaries — no explicit inter-array padding needed.
         */
        template<typename T, size_t Capacity, size_t Align, typename Stats>
        struct HeapPoolStore {
            struct alignas(Align) Cell {
                std::byte bytes[sizeof(T)];
            };
            std::unique_ptr<Cell[]> values{std::make_unique<Cell[]>(Capacity)};
            std::unique_ptr<std::atomic<uint32_t>[]> links{
                std::make_unique<std::atomic<uint32_t>[]>(Capacity)};
            std::unique_ptr<std::atomic<uint32_t>[]> gens{
                std::make_unique<std::atomic<uint32_t>[]>(Capacity)};
            [[msvc::no_unique_address]] Stats stats{};

            [[nodiscard]] T* ValuePtr(uint32_t i) noexcept {
                return std::launder(reinterpret_cast<T*>(values[i].bytes));
            }
            [[nodiscard]] std::atomic<uint32_t>& Link(uint32_t i) noexcept { return links[i]; }
            [[nodiscard]] const std::atomic<uint32_t>& Link(uint32_t i) const noexcept {
                return links[i];
            }
            [[nodiscard]] std::atomic<uint32_t>& Gen(uint32_t i) noexcept { return gens[i]; }
            [[nodiscard]] const std::atomic<uint32_t>& Gen(uint32_t i) const noexcept {
                return gens[i];
            }
        };

        /// @brief Select the storage backend for @p Kind.
        template<typename T, size_t Capacity, size_t Align, typename Stats, BackingKind Kind>
        using PoolStoreFor = std::conditional_t<Kind == BackingKind::Inline,
                                                InlinePoolStore<T, Capacity, Align, Stats>,
                                                HeapPoolStore<T, Capacity, Align, Stats>>;

    } // namespace Detail

    template<typename T, size_t Capacity, size_t Align = 16, typename Stats = NoPoolStats, bool Inline = true>
    using PoolStore = Detail::PoolStoreFor<T, Capacity, Align, Stats, 
                                           Inline ? Detail::BackingKind::Inline : Detail::BackingKind::Heap>;

    // =========================================================================
    // ConcurrentObjectPool<T, Config>
    // =========================================================================

    /**
     * @brief Lock-free, fixed-capacity pool of `T` with versioned handles.
     *
     * @tparam T Element type satisfying @ref Traits::Poolable.
     * @tparam C Compile-time @ref Detail::PoolConfig (capacity, backing, …).
     *
     * @code
     * ConcurrentObjectPool<Frame> pool;             // 1024 inline slots
     * PoolHandle h = pool.Emplace(args...);         // construct a Frame in a slot
     * if (Frame* f = pool.Deref(h)) { ... }          // generation-checked access
     * pool.Release(h);                               // run ~Frame, recycle the slot
     * @endcode
     */
    template<typename T, Detail::PoolConfig C = Detail::PoolConfig{}>
        requires(Traits::Poolable<T> && Detail::ValidateConfig(C))
    class ConcurrentObjectPool {
    public:
        using value_type = T;                                 ///< Pooled element type.
        using Handle = PoolHandle;                            ///< Versioned slot handle type.
        using Config = Detail::PoolConfig;                    ///< Configuration struct type.
        using BackingKind = Detail::BackingKind;              ///< Backing-storage selector.

        static constexpr size_t kCapacity = C.capacity;       ///< Compile-time slot count.
        static constexpr size_t kAlignment = C.alignment ? C.alignment : alignof(T);

    private:
        static constexpr Detail::AbaLayout kAba = Detail::ComputeAbaLayout(C.capacity);
        static constexpr uint64_t kSentinel = kAba.sentinel;  ///< Packed "empty list" index.
        static constexpr uint32_t kSentinelU = static_cast<uint32_t>(kAba.sentinel);
        static constexpr uint64_t kTagStep = uint64_t{1} << kAba.indexBits;

        using StatsT = std::conditional_t<C.enableStats, PoolStats, NoPoolStats>;
        using Store = Detail::PoolStoreFor<T, kCapacity, kAlignment, StatsT, C.backing>;

        static_assert(std::atomic<uint64_t>::is_always_lock_free,
                      "ConcurrentObjectPool requires lock-free 64-bit atomics");
        static_assert(C.capacity <= Detail::kMaxPoolCapacity,
                      "capacity exceeds the 15-bit handle index field");
        static_assert(kAba.tagBits >= 32,
                      "ABA tag must keep >= 32 bits so the free-list head cannot wrap in practice");

        /// @brief Decode the slot index from a packed head word.
        [[nodiscard]] static constexpr uint32_t IndexOf(uint64_t word) noexcept {
            return static_cast<uint32_t>(word & kAba.indexMask);
        }
        /// @brief Re-pack @p index with an incremented ABA tag taken from @p prev.
        [[nodiscard]] static constexpr uint64_t Pack(uint32_t index, uint64_t prev) noexcept {
            return (prev + kTagStep & ~kAba.indexMask) | (uint64_t{index} & kAba.indexMask);
        }

    public:
        /// @name Construction
        /// @{

        /// @brief Build an empty pool with every slot threaded onto the free-list.
        ConcurrentObjectPool() noexcept(C.backing == BackingKind::Inline) {
            // Thread slots so index 0 is popped first: head -> 0 -> 1 -> ... -> n-1 -> sentinel.
            for (uint32_t i = 0; i < kCapacity; ++i) {
                store_.Link(i).store((i + 1 < kCapacity) ? (i + 1)
                                                         : static_cast<uint32_t>(kSentinel),
                                     std::memory_order_relaxed);
            }
            head_.store(Pack(0, 0), std::memory_order_relaxed);
        }

        ConcurrentObjectPool(const ConcurrentObjectPool&) = delete;
        ConcurrentObjectPool& operator=(const ConcurrentObjectPool&) = delete;
        ConcurrentObjectPool(ConcurrentObjectPool&&) = delete;
        ConcurrentObjectPool& operator=(ConcurrentObjectPool&&) = delete;

        /// @brief Destroy any still-live elements (single-threaded teardown).
        ~ConcurrentObjectPool() {
            if constexpr (!std::is_trivially_destructible_v<T>) {
                // A slot is live iff its generation is odd (see Acquire/Release).
                for (uint32_t i = 0; i < kCapacity; ++i) {
                    if (store_.Gen(i).load(std::memory_order_relaxed) & 1u) {
                        std::destroy_at(store_.ValuePtr(i));
                    }
                }
            }
        }
        /// @}

        /// @name Acquire / release (multi-producer, multi-consumer)
        /// @{

        /**
         * @brief Construct a `T` in a free slot and return a versioned handle.
         *
         * Lock-free and safe to call from any thread. The slot's generation is
         * bumped from even (free) to odd (live) so the returned handle can later
         * be validated by @ref Deref.
         *
         * @tparam Args Constructor argument types (must satisfy `constructible_from`).
         * @param args  Forwarded to `T`'s constructor.
         * @return A valid handle, or an invalid handle if the pool is exhausted.
         */
        template<typename... Args>
            requires std::constructible_from<T, Args&&...>
        [[nodiscard]] Handle Emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
            const uint32_t index = PopFreeIndex();
            if (index == kSentinel) [[unlikely]] {
                if constexpr (C.enableStats) {
                    store_.stats.failures.fetch_add(1, std::memory_order_relaxed);
                }
                return Handle{}; // invalid: pool exhausted
            }
            if constexpr (std::is_nothrow_constructible_v<T, Args&&...>) {
                ::new (static_cast<void*>(store_.ValuePtr(index))) T(std::forward<Args>(args)...);
            } else {
                // Strong guarantee: a throwing constructor must not leak the slot.
                try {
                    ::new (static_cast<void*>(store_.ValuePtr(index))) T(std::forward<Args>(args)...);
                } catch (...) {
                    PushFreeIndex(index);
                    throw;
                }
            }
            // even -> odd marks the slot live; the handle records this generation.
            const uint32_t gen = store_.Gen(index).fetch_add(1, std::memory_order_release) + 1u;
            if constexpr (C.enableStats) {
                store_.stats.acquires.fetch_add(1, std::memory_order_relaxed);
            }
            return Handle::Make(index, gen);
        }

        /**
         * @brief Acquire a default-constructed slot.
         * @return A valid handle, or an invalid handle if the pool is exhausted.
         */
        [[nodiscard]] Handle Acquire() noexcept(std::is_nothrow_default_constructible_v<T>)
            requires std::default_initializable<T>
        {
            return Emplace();
        }

        /**
         * @brief `Result`-returning variant of @ref Emplace for `std::expected` flows.
         *
         * Identical semantics to @ref Emplace, but reports exhaustion as
         * `ErrorCode::ResourceExhausted` instead of an invalid handle, so it
         * composes with the engine's monadic `Result<T>` chaining
         * (`.and_then` / `.transform` / `.or_else`).
         *
         * @return The fresh handle on success, or `ErrorCode::ResourceExhausted`.
         */
        template<typename... Args>
            requires std::constructible_from<T, Args&&...>
        [[nodiscard]] Result<Handle> TryEmplace(Args&&... args) noexcept(
            std::is_nothrow_constructible_v<T, Args&&...>) {
            Handle h = Emplace(std::forward<Args>(args)...);
            if (!h.IsValid()) [[unlikely]] {
                return std::unexpected(ErrorCode::ResourceExhausted);
            }
            return h;
        }

        /**
         * @brief Destroy the element behind @p handle and recycle its slot.
         *
         * No-op (returns false) if the handle is invalid or stale — i.e. its slot
         * was already released or recycled. Bumps the slot generation from odd
         * (live) back to even (free) so every outstanding copy of @p handle is
         * permanently invalidated.
         *
         * @return true if a live slot was released by this call.
         */
        bool Release(Handle handle) noexcept {
            if (!ReclaimSlot(handle)) {
                return false;
            }
            PushFreeIndex(handle.Index());
            if constexpr (C.enableStats) {
                store_.stats.releases.fetch_add(1, std::memory_order_relaxed);
            }
            return true;
        }
        /// @}

        /// @name Access
        /// @{

        /**
         * @brief Resolve @p handle to a live element pointer, or `nullptr`.
         *
         * Returns `nullptr` if the handle is invalid or its slot has since been
         * released/recycled (generation mismatch). The pointer is valid until the
         * slot is released; concurrent reads/writes through it are the caller's
         * responsibility to synchronise.
         */
        [[nodiscard]] T* Deref(Handle handle) noexcept {
            if (!handle.IsValid()) [[unlikely]] {
                return nullptr;
            }
            const uint32_t index = handle.Index();
            const uint32_t live = store_.Gen(index).load(std::memory_order_acquire);
            // A live slot has an odd generation (see Emplace); the handle stores the
            // low 16 bits of that generation. Both must match: the parity check also
            // rejects a forged/default handle (even generation) that happens to alias
            // a currently-free slot's counter.
            if (!(live & 1u) ||
                static_cast<uint16_t>(live) != static_cast<uint16_t>(handle.Generation())) {
                return nullptr; // stale, freed, or forged handle
            }
            return store_.ValuePtr(index);
        }

        /// @brief `const` overload of @ref Deref.
        [[nodiscard]] const T* Deref(Handle handle) const noexcept {
            return const_cast<ConcurrentObjectPool*>(this)->Deref(handle);
        }

        /// @brief True if @p handle currently refers to a live slot.
        [[nodiscard]] bool IsLive(Handle handle) const noexcept {
            if (!handle.IsValid()) {
                return false;
            }
            const uint32_t live = store_.Gen(handle.Index()).load(std::memory_order_acquire);
            return (live & 1u) && static_cast<uint16_t>(live) == static_cast<uint16_t>(handle.Generation());
        }
        /// @}

        /// @name Bulk operations
        /// @{

        /**
         * @brief Acquire up to `out.size()` default-constructed slots in one pass.
         *
         * Detaches a chain of free slots from the list with a *single* CAS (instead
         * of one CAS per slot), then default-constructs `T` into each and stamps a
         * handle. Under contention this collapses N head-CAS retries into one,
         * which is the whole point of a batch API. Returns the count actually
         * acquired (fewer than requested if the pool drains mid-batch).
         *
         * @return Number of handles written to the front of @p out.
         */
        [[nodiscard]] size_t AcquireBulk(std::span<Handle> out) noexcept(std::is_nothrow_default_constructible_v<T>)
            requires std::default_initializable<T>
        {
            size_t n = 0;
            while (n < out.size()) {
                uint32_t first = kSentinelU;
                // Grab as many slots as remain to be filled, in one detaching CAS.
                const size_t got = PopFreeBatch(out.size() - n, first);
                if (got == 0) {
                    break; // pool drained
                }
                uint32_t index = first;
                for (size_t k = 0; k < got; ++k) {
                    const uint32_t next = store_.Link(index).load(std::memory_order_relaxed);
                    ::new (static_cast<void*>(store_.ValuePtr(index))) T();
                    const uint32_t gen = store_.Gen(index).fetch_add(1, std::memory_order_release) + 1u;
                    out[n++] = Handle::Make(index, gen);
                    index = next;
                }
                if constexpr (C.enableStats) {
                    store_.stats.acquires.fetch_add(got, std::memory_order_relaxed);
                }
            }
            return n;
        }

        /**
         * @brief Release a batch of handles, returning their slots in one CAS.
         *
         * Reclaims each live handle (destroying its element and bumping its
         * generation), strings the reclaimed slots into a local chain, and splices
         * the whole chain back onto the free-list with a *single* CAS. Stale or
         * already-freed handles in @p handles are skipped.
         *
         * @return Number of handles that referred to a live slot and were released.
         */
        size_t ReleaseBulk(std::span<const Handle> handles) noexcept {
            uint32_t chainHead = kSentinelU;
            uint32_t chainTail = kSentinelU;
            size_t n = 0;
            for (Handle h : handles) {
                if (!ReclaimSlot(h)) {
                    continue; // stale / already freed
                }
                const uint32_t index = h.Index();
                if (chainHead == kSentinelU) {
                    chainHead = index;
                    chainTail = index;
                } else {
                    store_.Link(index).store(chainHead, std::memory_order_relaxed);
                    chainHead = index;
                }
                ++n;
            }
            if (n != 0) {
                PushFreeChain(chainHead, chainTail);
                if constexpr (C.enableStats) {
                    store_.stats.releases.fetch_add(n, std::memory_order_relaxed);
                }
            }
            return n;
        }
        /// @}

        /// @name Bulk lifetime management (NOT thread-safe — quiescent pool only)
        /// @{

        /**
         * @brief Invoke @p fn on every currently-live element.
         *
         * Walks the generation array (SoA makes this a dense, branch-predictable
         * scan) and calls `fn(T&)` for each slot whose generation is odd (live).
         *
         * @warning **Not thread-safe.** Intended for single-threaded phases (setup,
         *          teardown, debug dumps) when no other thread is acquiring or
         *          releasing. Concurrent mutation races the scan.
         *
         * @tparam Fn Callable as `void(T&)`.
         */
        template<typename Fn>
            requires std::invocable<Fn&, T&>
        void ForEach(Fn&& fn) noexcept(std::is_nothrow_invocable_v<Fn&, T&>) {
            for (uint32_t i = 0; i < kCapacity; ++i) {
                if (store_.Gen(i).load(std::memory_order_relaxed) & 1u) {
                    fn(*store_.ValuePtr(i));
                }
            }
        }

        /**
         * @brief Destroy every live element and return all slots to the free-list.
         *
         * Each reclaimed slot's generation is bumped (odd → even) so any handle
         * outstanding across the `Clear` is permanently invalidated, then the
         * free-list is rebuilt in ascending index order.
         *
         * @warning **Not thread-safe.** Call only when the pool is quiescent.
         */
        void Clear() noexcept {
            for (uint32_t i = 0; i < kCapacity; ++i) {
                if (store_.Gen(i).load(std::memory_order_relaxed) & 1u) {
                    std::destroy_at(store_.ValuePtr(i));
                    store_.Gen(i).fetch_add(1, std::memory_order_relaxed); // odd -> even (free)
                }
                store_.Link(i).store((i + 1 < kCapacity) ? (i + 1)
                                                         : static_cast<uint32_t>(kSentinel),
                                     std::memory_order_relaxed);
            }
            head_.store(Pack(0, head_.load(std::memory_order_relaxed)),
                        std::memory_order_release);
        }
        /// @}

        /// @name Queries / diagnostics
        /// @{

        /// @brief Compile-time slot capacity.
        [[nodiscard]] static constexpr size_t Capacity() noexcept { return kCapacity; }

        /**
         * @brief Approximate number of free slots (racy but safe).
         *
         * Walks the free-list from a relaxed snapshot of the head, so it is only a
         * diagnostic estimate under concurrency. O(free-count); intended for tests
         * and assertions, not the hot path.
         */
        [[nodiscard]] size_t ApproxFree() const noexcept {
            uint64_t head = head_.load(std::memory_order_acquire);
            size_t count = 0;
            uint32_t index = IndexOf(head);
            while (index != kSentinel && count <= kCapacity) {
                ++count;
                index = store_.Link(index).load(std::memory_order_relaxed);
            }
            return count;
        }

        /// @brief Successful acquisitions so far (only when `enableStats`).
        [[nodiscard]] uint64_t AcquireCount() const noexcept
            requires(C.enableStats)
        {
            return store_.stats.acquires.load(std::memory_order_relaxed);
        }

        /// @brief Slot releases so far (only when `enableStats`).
        [[nodiscard]] uint64_t ReleaseCount() const noexcept
            requires(C.enableStats)
        {
            return store_.stats.releases.load(std::memory_order_relaxed);
        }

        /// @brief Acquisitions that found the pool empty (only when `enableStats`).
        [[nodiscard]] uint64_t FailureCount() const noexcept
            requires(C.enableStats)
        {
            return store_.stats.failures.load(std::memory_order_relaxed);
        }

        /// @brief Compile-time physical-layout report for this specialisation.
        [[nodiscard]] static consteval Concurrency::CacheLayoutReport Layout() {
            return Concurrency::AnalyzeCacheLayout<ConcurrentObjectPool>();
        }
        /// @}

    private:
        /// @brief Validate @p handle and, if it is live, destroy its element and mark
        ///        its slot free (odd→even generation). Does *not* touch the free-list.
        /// @return true iff this call won the live→free transition for the slot.
        [[nodiscard]] bool ReclaimSlot(Handle handle) noexcept {
            if (!handle.IsValid()) [[unlikely]] {
                return false;
            }
            const uint32_t index = handle.Index();
            std::atomic<uint32_t>& gen = store_.Gen(index);
            uint32_t current = gen.load(std::memory_order_relaxed);
            // The slot generation is a full 32-bit monotonic counter; the handle holds
            // only its low 16 bits, so we compare in 16-bit space but CAS the full word.
            // A live slot has an odd generation (see Emplace); a stale or already-freed
            // handle therefore mismatches on parity or on the low 16 bits.
            for (;;) {
                if (!(current & 1u) || static_cast<uint16_t>(current) != static_cast<uint16_t>(handle.Generation())) {
                    return false; // stale or already released
                }
                if (gen.compare_exchange_weak(current, current + 1u, std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
                    break; // odd(live) -> even(free): this thread won the release
                }
            }
            std::destroy_at(store_.ValuePtr(index));
            return true;
        }

        /// @brief Pop a free slot index, or `kSentinel` cast if the pool is empty.
        ///        ABA-safe: the CAS embeds an incrementing tag in the head word.
        [[nodiscard]] uint32_t PopFreeIndex() noexcept {
            uint64_t head = head_.load(std::memory_order_acquire);
            for (;;) {
                const uint32_t index = IndexOf(head);
                if (index == kSentinel) {
                    return static_cast<uint32_t>(kSentinel); // empty
                }
                const uint32_t next = store_.Link(index).load(std::memory_order_relaxed);
                const uint64_t desired = Pack(next, head);
                // Success: acquire pairs with the releaser's PushFreeIndex release store, so
                // the freed slot's destruction happens-before its reuse. Failure: relaxed —
                // we only reload `head` and retry, no data is published or consumed.
                if (head_.compare_exchange_weak(head, desired, std::memory_order_acquire,
                                                std::memory_order_relaxed)) {
                    return index;
                }
            }
        }

        /// @brief Push slot @p index back onto the free-list (ABA-safe).
        void PushFreeIndex(uint32_t index) noexcept {
            uint64_t head = head_.load(std::memory_order_relaxed);
            for (;;) {
                store_.Link(index).store(IndexOf(head), std::memory_order_relaxed);
                const uint64_t desired = Pack(index, head);
                if (head_.compare_exchange_weak(head, desired, std::memory_order_release,
                                                std::memory_order_relaxed)) {
                    return;
                }
            }
        }

        /**
         * @brief Detach up to @p maxCount slots from the free-list in a single CAS.
         *
         * Walks the existing chain to find the (maxCount)-th successor, then swings
         * the head to it with one CAS — so a whole batch costs one head update, not
         * one per slot. The detached chain stays intact (each node's `Link` still
         * points to the next), so the caller can iterate it via `store_.Link`.
         *
         * @warning **Correctness rests on a structural invariant**: while a node sits
         *          on the free-list, its `Link` is written only by the thread that
         *          wins the head CAS detaching/attaching it — never by a thread merely
         *          *holding* an acquired slot. That is why the forward walk reads
         *          `Link` with `relaxed` ordering. The classic Treiber-stack ABA
         *          hazard (a node popped, recycled, and re-pushed mid-walk, so a stale
         *          `afterBatch` swings the head onto a dangling chain) is contained by
         *          the head word's ABA tag (@ref Pack): any intervening head change
         *          fails our CAS and we retry. ASan/TSan stress (see the test file)
         *          exercises this path.
         * @todo If a future code path ever writes an in-list node's `Link` from a
         *       context other than the head-CAS winner (lazy/segmented free lists,
         *       hazard-pointer reclamation, per-thread caching that passes nodes around
         *       without touching the head, …), this `relaxed` walk must be revisited:
         *       promote the `Link` loads to `acquire` and re-audit, or move to a
         *       tagged-per-node scheme. The invariant is load-bearing, not cosmetic.
         *
         * @param maxCount Maximum slots to detach (must be >= 1).
         * @param[out] outFirst Index of the first slot in the detached chain
         *                      (untouched and meaningless when the return is 0).
         * @return Number of slots actually detached (0 iff the pool was empty).
         */
        [[nodiscard]] size_t PopFreeBatch(size_t maxCount, uint32_t& outFirst) noexcept {
            uint64_t head = head_.load(std::memory_order_acquire);
            for (;;) {
                const uint32_t first = IndexOf(head);
                if (first == kSentinelU) {
                    return 0; // empty
                }
                // Walk forward to find the new head (the slot just past the batch) and
                // count how many we can actually take (the chain may be shorter).
                uint32_t last = first;
                size_t taken = 1;
                uint32_t afterBatch = store_.Link(last).load(std::memory_order_relaxed);
                while (taken < maxCount && afterBatch != kSentinelU) {
                    last = afterBatch;
                    afterBatch = store_.Link(last).load(std::memory_order_relaxed);
                    ++taken;
                }
                const uint64_t desired = Pack(afterBatch, head);
                if (head_.compare_exchange_weak(head, desired, std::memory_order_acquire,
                                                std::memory_order_relaxed)) {
                    outFirst = first;
                    return taken;
                }
                // CAS lost: another thread mutated the head; retry from the new snapshot.
            }
        }

        /**
         * @brief Splice an already-linked chain `[first .. last]` onto the free-list
         *        in a single CAS. @p last's `Link` is set to the old head.
         * @pre `first` and `last` bound a chain whose interior `Link`s are set, and
         *      both are valid slot indices (not the sentinel).
         */
        void PushFreeChain(uint32_t first, uint32_t last) noexcept {
            uint64_t head = head_.load(std::memory_order_relaxed);
            for (;;) {
                store_.Link(last).store(IndexOf(head), std::memory_order_relaxed);
                const uint64_t desired = Pack(first, head);
                if (head_.compare_exchange_weak(head, desired, std::memory_order_release,
                                                std::memory_order_relaxed)) {
                    return;
                }
            }
        }

        // --- Contended free-list head: alone on its cache line ---
        [[=Concurrency::Contended{}]] alignas(Platform::kCacheLineSize)
        std::atomic<uint64_t> head_{};

        // --- Cold SoA storage (+ optional stats) on a separate cache region ---
        [[=Concurrency::SharedStorage{}]] alignas(Platform::kCacheLineSize)
        Store store_{};
    };

    // =========================================================================
    // Compile-time layout audit (representative instantiations)
    // =========================================================================

    /** @cond INTERNAL */
    consteval {

        using Concurrency::AuditFalseSharing;
        using Concurrency::DomainStartsLine;
        using Concurrency::Contended;

        using HeapPool = ConcurrentObjectPool<
            std::uint64_t, Detail::PoolConfig{.backing = Detail::BackingKind::Heap}>;

        // Internal false sharing: the contended head and the cold SoA storage are
        // distinct write domains and must never overlap a cache line.
        static_assert(AuditFalseSharing<ConcurrentObjectPool<std::uint64_t>>(),
                      "ConcurrentObjectPool<uint64_t> failed the internal false-sharing audit");
        static_assert(AuditFalseSharing<HeapPool>(),
                      "heap-backed ConcurrentObjectPool failed the internal false-sharing audit");
        // The contended head owns the start of its own cache line.
        static_assert(DomainStartsLine<ConcurrentObjectPool<std::uint64_t>, Contended>(),
                      "free-list head must start its own cache line");

    }
    /** @endcond */

} // namespace Mashiro

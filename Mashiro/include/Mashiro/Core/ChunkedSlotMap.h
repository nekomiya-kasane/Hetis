/**
 * @file ChunkedSlotMap.h
 * @brief Cache-friendly chunked slot map with generational handles and dense iteration.
 *
 * `ChunkedSlotMap<T, ChunkBits>` is a contiguous-chunk sparse set that provides:
 * - **O(1)** insert, remove, and lookup by generational handle.
 * - **Dense iteration** over live elements only (cache-linear, no branch on dead slots).
 * - **Intrusive free-list** (zero extra allocation for free tracking).
 * - **Generation odd/even encoding** (eliminates the `occupied` bool per slot).
 * - **Stable handles**: insertion/removal never invalidates other handles.
 * - **No capacity limit**: new chunks allocated on demand.
 *
 * Handle: 8 bytes = `{uint32_t index, uint32_t generation}`.
 * Supports up to 4 billion slots and 4 billion generations before wrap.
 *
 * @ingroup Core
 */
#pragma once

#include <cassert>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace Mashiro {

    // =========================================================================
    // SlotHandle
    // =========================================================================

    /**
     * @brief Generational handle for SlotMap lookups (8 bytes).
     *
     * `index = 0` is reserved as the null sentinel.
     * Generation is incremented on each free; a stale handle's generation won't
     * match the slot's current generation.
     */
    struct SlotHandle {
        uint32_t index = 0;      ///< Slot index (0 = invalid/null).
        uint32_t generation = 0; ///< Generation counter at allocation time.

        /// @brief True if this handle is non-null (index != 0).
        [[nodiscard]] constexpr bool IsValid() const noexcept { return index != 0; }

        /// @brief Null handle constant.
        [[nodiscard]] static constexpr SlotHandle Null() noexcept { return {}; }

        constexpr bool operator==(const SlotHandle&) const noexcept = default;
    };

    // =========================================================================
    // ChunkedSlotMap
    // =========================================================================

    /**
     * @brief Chunked slot map with dense iteration and generational handles.
     *
     * @tparam T         Value type stored in each slot.
     * @tparam ChunkBits Log2 of slots per chunk (default 8 → 256 slots/chunk).
     *
     * Memory layout:
     * - **Sparse array**: chunked, each chunk = 2^ChunkBits Slot entries.
     * - **Dense array**: contiguous vector of `{T value, uint32_t sparseIndex}`.
     * - Sparse slot stores the dense index (when occupied) or next-free link.
     *
     * Iteration via `Values()` / `Entries()` / `ForEach()` touches only the
     * dense array — perfectly cache-linear, zero branching on dead slots.
     */
    template<typename T, uint32_t ChunkBits = 8>
    class ChunkedSlotMap {
        static constexpr uint32_t kChunkSize = 1u << ChunkBits;
        static constexpr uint32_t kChunkMask = kChunkSize - 1;

        /** @cond INTERNAL */

        /// @brief Sparse slot: generation + payload (dense-index or next-free link).
        struct SparseSlot {
            uint32_t generation = 0; ///< Even = free, odd = occupied.
            uint32_t payload = 0;    ///< Occupied: dense index. Free: next-free sparse index.

            [[nodiscard]] bool IsOccupied() const noexcept { return (generation & 1u) != 0; }
        };

        /// @brief Dense element: stored value + back-pointer to sparse index.
        struct DenseEntry {
            T value;
            uint32_t sparseIndex; ///< Index into the sparse array.

            template<typename... Args>
            explicit DenseEntry(uint32_t si, Args&&... args)
                : value(std::forward<Args>(args)...), sparseIndex(si) {}
        };

        /// @brief Contiguous block of sparse slots.
        struct Chunk {
            SparseSlot slots[kChunkSize]{};
        };

        std::vector<std::unique_ptr<Chunk>> chunks_; ///< Sparse chunk array.
        std::vector<DenseEntry> dense_;              ///< Dense value array (live elements only).
        uint32_t freeHead_ = 0;                      ///< Intrusive free-list head (0 = empty).
        uint32_t nextSlot_ = 1;                      ///< Next never-allocated sparse index.

        /** @endcond */

    public:
        /// @name Construction / destruction
        /// @{

        ChunkedSlotMap() {
            // Allocate first chunk; slot 0 is the null sentinel (permanently even-gen = free).
            AllocateChunk();
        }

        ~ChunkedSlotMap() = default;

        ChunkedSlotMap(const ChunkedSlotMap&) = delete;
        ChunkedSlotMap& operator=(const ChunkedSlotMap&) = delete;
        ChunkedSlotMap(ChunkedSlotMap&&) noexcept = default;
        ChunkedSlotMap& operator=(ChunkedSlotMap&&) noexcept = default;

        /// @}

        /// @name Insertion
        /// @{

        /**
         * @brief Allocate a slot and construct T in-place.
         * @return Handle with unique index + current generation.
         */
        template<typename... Args>
        [[nodiscard]] SlotHandle Emplace(Args&&... args) {
            uint32_t si = AllocateSparseSlot();
            auto& slot = GetSparseSlot(si);

            // Bump generation from even (free) to odd (occupied)
            ++slot.generation;

            // Construct in dense array, record back-pointer
            uint32_t di = static_cast<uint32_t>(dense_.size());
            dense_.emplace_back(si, std::forward<Args>(args)...);
            slot.payload = di; // dense index

            return SlotHandle{si, slot.generation};
        }

        /// @}

        /// @name Removal
        /// @{

        /**
         * @brief Free the slot identified by handle. O(1) via swap-and-pop.
         * @return true if freed; false if stale/invalid.
         */
        bool Free(SlotHandle handle) {
            if (!handle.IsValid()) return false;

            auto* slot = TryGetSparseSlot(handle);
            if (!slot) return false;

            uint32_t di = slot->payload; // dense index of this element

            // Destroy by swap-and-pop in dense array
            uint32_t lastDi = static_cast<uint32_t>(dense_.size()) - 1;
            if (di != lastDi) {
                // Move last dense element into the vacated position
                uint32_t lastSi = dense_[lastDi].sparseIndex;
                dense_[di] = std::move(dense_[lastDi]);
                // Update the swapped element's sparse slot to point to new dense index
                GetSparseSlot(lastSi).payload = di;
            }
            dense_.pop_back();

            // Bump generation from odd (occupied) to even (free)
            ++slot->generation;
            if (slot->generation == 0)
                slot->generation = 2; // skip 0 (reserved for sentinel meaning)

            // Push onto intrusive free-list
            slot->payload = freeHead_;
            freeHead_ = handle.index;

            return true;
        }

        /// @}

        /// @name Lookup
        /// @{

        /// @brief Get a pointer to the stored value, or nullptr if stale/invalid.
        [[nodiscard]] T* Get(SlotHandle handle) {
            auto* slot = TryGetSparseSlot(handle);
            return slot ? &dense_[slot->payload].value : nullptr;
        }

        [[nodiscard]] const T* Get(SlotHandle handle) const {
            auto* slot = TryGetSparseSlot(handle);
            return slot ? &dense_[slot->payload].value : nullptr;
        }

        /// @brief Unchecked access (UB if handle is invalid). For hot paths.
        [[nodiscard]] T& operator[](SlotHandle handle) {
            return dense_[GetSparseSlot(handle.index).payload].value;
        }

        [[nodiscard]] const T& operator[](SlotHandle handle) const {
            return dense_[GetSparseSlot(handle.index).payload].value;
        }

        /// @brief Check if a handle refers to a live slot.
        [[nodiscard]] bool IsAlive(SlotHandle handle) const {
            return TryGetSparseSlot(handle) != nullptr;
        }

        /// @}

        /// @name Size / capacity
        /// @{

        /// @brief Number of live elements.
        [[nodiscard]] uint32_t Size() const noexcept {
            return static_cast<uint32_t>(dense_.size());
        }

        /// @brief True if no live elements.
        [[nodiscard]] bool Empty() const noexcept { return dense_.empty(); }

        /// @brief Total sparse slot capacity (excluding sentinel).
        [[nodiscard]] uint32_t Capacity() const noexcept {
            return static_cast<uint32_t>(chunks_.size()) * kChunkSize - 1;
        }

        /// @brief Reserve dense storage.
        void Reserve(uint32_t count) { dense_.reserve(count); }

        /// @}

        /// @name Dense iteration (cache-linear, live elements only)
        /// @{

        /// @brief Iterate all live values. `fn(T&)` or `fn(SlotHandle, T&)`.
        template<typename Fn>
        void ForEach(Fn&& fn) {
            for (auto& entry : dense_) {
                if constexpr (std::is_invocable_v<Fn, SlotHandle, T&>) {
                    auto& slot = GetSparseSlot(entry.sparseIndex);
                    fn(SlotHandle{entry.sparseIndex, slot.generation}, entry.value);
                } else {
                    fn(entry.value);
                }
            }
        }

        template<typename Fn>
        void ForEach(Fn&& fn) const {
            for (auto& entry : dense_) {
                if constexpr (std::is_invocable_v<Fn, SlotHandle, const T&>) {
                    auto& slot = GetSparseSlot(entry.sparseIndex);
                    fn(SlotHandle{entry.sparseIndex, slot.generation}, entry.value);
                } else {
                    fn(entry.value);
                }
            }
        }

        /// @brief Direct access to the dense value array (for SIMD / bulk processing).
        [[nodiscard]] T* Data() noexcept { return dense_.empty() ? nullptr : &dense_[0].value; }

        [[nodiscard]] const T* Data() const noexcept {
            return dense_.empty() ? nullptr : &dense_[0].value;
        }

        /// @brief Stride between consecutive values in the dense array (in bytes).
        /// Useful for interleaved access patterns.
        static constexpr size_t kDenseStride = sizeof(DenseEntry);

        /// @}

        /// @name Bulk operations
        /// @{

        /// @brief Remove all elements. Does not free chunk memory.
        void Clear() {
            dense_.clear();
            freeHead_ = 0;
            nextSlot_ = 1;
            // Reset all sparse slots to even generation (free)
            for (auto& chunk : chunks_) {
                for (auto& slot : chunk->slots) {
                    if (slot.IsOccupied()) {
                        ++slot.generation; // odd → even
                    }
                    slot.payload = 0;
                }
            }
        }

        /// @}

    private:
        void AllocateChunk() { chunks_.push_back(std::make_unique<Chunk>()); }

        [[nodiscard]] SparseSlot& GetSparseSlot(uint32_t index) {
            return chunks_[index >> ChunkBits]->slots[index & kChunkMask];
        }

        [[nodiscard]] const SparseSlot& GetSparseSlot(uint32_t index) const {
            return chunks_[index >> ChunkBits]->slots[index & kChunkMask];
        }

        uint32_t AllocateSparseSlot() {
            if (freeHead_ != 0) {
                uint32_t idx = freeHead_;
                freeHead_ = GetSparseSlot(idx).payload; // follow next-free link
                return idx;
            }
            // Allocate from never-used region
            uint32_t totalCap = static_cast<uint32_t>(chunks_.size()) * kChunkSize;
            if (nextSlot_ >= totalCap) {
                AllocateChunk();
            }
            return nextSlot_++;
        }

        [[nodiscard]] SparseSlot* TryGetSparseSlot(SlotHandle handle) {
            if (!handle.IsValid()) {
                return nullptr;
            }
            uint32_t totalCap = static_cast<uint32_t>(chunks_.size()) * kChunkSize;
            if (handle.index >= totalCap) {
                return nullptr;
            }
            auto& slot = GetSparseSlot(handle.index);
            if (!slot.IsOccupied() || slot.generation != handle.generation) {
                return nullptr;
            }
            return &slot;
        }

        [[nodiscard]] const SparseSlot* TryGetSparseSlot(SlotHandle handle) const {
            if (!handle.IsValid()) {
                return nullptr;
            }
            uint32_t totalCap = static_cast<uint32_t>(chunks_.size()) * kChunkSize;
            if (handle.index >= totalCap) {
                return nullptr;
            }
            auto& slot = GetSparseSlot(handle.index);
            if (!slot.IsOccupied() || slot.generation != handle.generation) {
                return nullptr;
            }
            return &slot;
        }
    };

} // namespace Mashiro

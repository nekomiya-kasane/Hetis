/**
 * @file ChunkedSlotMap.h
 * @brief Provide a cache-efficient slot map with generational handles and contiguous values.
 * @details @ref Sora::ChunkedSlotMap stores handle metadata in fixed-size sparse chunks and live values in a compact
 * contiguous array. Lookup, insertion after allocation, and unordered removal are constant-time. Removal uses
 * swap-and-pop, so it may relocate the last value but never invalidates handles for other live values.
 *
 * Use the map when external systems need small stable identifiers while hot loops need linear access to live values:
 * @code{.cpp}
 * struct Particle {
 *     float position;
 *     float velocity;
 * };
 *
 * Sora::ChunkedSlotMap<Particle> particles;
 * particles.Reserve(4096);
 * const Sora::SlotHandle particle = particles.Emplace(Particle{.position = 0.0F, .velocity = 2.0F});
 *
 * if (Particle* value = particles.Get(particle)) {
 *     value->position += value->velocity;
 * }
 * for (Particle& value : particles.Values()) {
 *     value.position += value.velocity;
 * }
 * particles.Free(particle);
 * @endcode
 *
 * Handles are local to the originating map; a handle from another map may coincidentally match and must not be used.
 * After roughly 2^31 reuse cycles of one sparse slot, its 32-bit generation can wrap and alias an ancient stale
 * handle. @ref Sora::ChunkedSlotMap::Emplace and @ref Sora::ChunkedSlotMap::Reserve may invalidate pointers,
 * references, spans, and iterators into the dense values. @ref Sora::ChunkedSlotMap::Free invalidates references to
 * the erased value and to the last value when it is moved into the erased position. Handles remain valid until their
 * own value is removed or the map is cleared.
 *
 * A handle remains an 8-byte pair of 32-bit sparse index and 32-bit generation. @p ChunkBits only partitions the
 * sparse index into a chunk selector and an in-chunk offset; it does not change the handle representation.
 *
 * The container is not thread-safe. Concurrent access requires external synchronization, including read-only lookup
 * while another thread mutates the map.
 * @ingroup Core
 */

#pragma once

#include <Sora/Core/FixedString.h>
#include <Sora/Core/Handle.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <ranges>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora {

    /** @brief Generational handle used by ChunkedSlotMap. */
    using SlotHandle = Handle;

    /** @brief Constrain values to no-throw relocation during unordered removal. */
    template<typename T>
    concept ChunkedSlotMapValue =
        std::movable<T> && std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>;

    /**
     * @brief Store live values densely while resolving stable generational handles through sparse chunks.
     * @tparam T Value type satisfying @ref ChunkedSlotMapValue.
     * @tparam ChunkBits Base-two logarithm of sparse slots per chunk; the default creates 256-slot chunks.
     * @tparam Name Compile-time diagnostic name for this map specialization.
     * @note Insertion order is not preserved after removal because @ref Free uses swap-and-pop.
     */
    template<ChunkedSlotMapValue T, std::uint32_t ChunkBits = 8, Sora::FixedString<16> Name = "<unnamed>">
        requires(ChunkBits > 0 && ChunkBits < 31)
    class ChunkedSlotMap {
    public:
        inline static constexpr auto kName = Name;                          /**< Compile-time diagnostic name. */
        inline static constexpr std::uint32_t kChunkBits = ChunkBits;       /**< Sparse chunk size exponent. */
        inline static constexpr std::uint32_t kChunkSize = 1U << ChunkBits; /**< Sparse slots per chunk. */
        inline static constexpr size_t kDenseStride = sizeof(T);       /**< Byte stride of dense values. */
        inline static constexpr size_t kMaximumSlotCount =             /**< Handle-representable count. */
            std::numeric_limits<std::uint32_t>::max() - size_t{1};
        inline static constexpr size_t kBatchCompactionNumerator = 3;   /**< Compaction threshold numerator. */
        inline static constexpr size_t kBatchCompactionDenominator = 4; /**< Compaction threshold denominator. */
        inline static constexpr size_t kPrefetchDistance = 8;           /**< Sparse batch lookup look-ahead. */

    private:
        inline static constexpr std::uint32_t kChunkMask = kChunkSize - 1;
        inline static constexpr std::uint32_t kInvalidDenseIndex = std::numeric_limits<std::uint32_t>::max();

        struct alignas(std::uint64_t) SparseSlot {
            inline static constexpr std::uint64_t kPayloadMask = std::numeric_limits<std::uint32_t>::max();

            std::uint64_t state = 0;

            [[nodiscard]] constexpr std::uint32_t Generation() const noexcept {
                return static_cast<std::uint32_t>(state >> 32);
            }

            [[nodiscard]] constexpr std::uint32_t Payload() const noexcept {
                return static_cast<std::uint32_t>(state & kPayloadMask);
            }

            [[nodiscard]] constexpr bool IsOccupied() const noexcept { return (Generation() & 1U) != 0; }

            constexpr void Set(std::uint32_t generation, std::uint32_t payload) noexcept {
                state = std::uint64_t{generation} << 32 | payload;
            }

            constexpr void SetPayload(std::uint32_t payload) noexcept { state = (state & ~kPayloadMask) | payload; }
        };

        static_assert(sizeof(SparseSlot) == sizeof(SlotHandle));
        static_assert(alignof(SparseSlot) == alignof(std::uint64_t));

        struct Chunk {
            std::array<SparseSlot, kChunkSize> slots{};
        };

        std::vector<Chunk> chunks_;
        std::vector<T> values_;
        std::vector<std::uint32_t> denseToSparse_;
        std::uint32_t freeHead_ = 0;
        std::uint32_t nextSlot_ = 1;

    public:
        /** @name Construction and Assignment @{ ------------------------------------------------------------ */

        /** @brief Construct an empty map without allocating storage. */
        ChunkedSlotMap() = default;

        ~ChunkedSlotMap() = default;

        ChunkedSlotMap(const ChunkedSlotMap&) = delete;
        ChunkedSlotMap& operator=(const ChunkedSlotMap&) = delete;

        /** @brief Move all storage and leave @p other as a reusable empty map. */
        ChunkedSlotMap(ChunkedSlotMap&& other) noexcept
            : chunks_(std::move(other.chunks_)),
              values_(std::move(other.values_)),
              denseToSparse_(std::move(other.denseToSparse_)),
              freeHead_(other.freeHead_),
              nextSlot_(other.nextSlot_) {
            other.ResetMovedFrom();
        }

        /** @brief Replace this map by moving @p other and leave @p other reusable and empty. */
        ChunkedSlotMap& operator=(ChunkedSlotMap&& other) noexcept {
            if (this == std::addressof(other)) {
                return *this;
            }
            chunks_ = std::move(other.chunks_);
            values_ = std::move(other.values_);
            denseToSparse_ = std::move(other.denseToSparse_);
            freeHead_ = other.freeHead_;
            nextSlot_ = other.nextSlot_;
            other.ResetMovedFrom();
            return *this;
        }

        /** @} ----------------------------------------------------------------------------------------------- */

        /** @name Capacity @{ ------------------------------------------------------------------------------- */

        /** @brief Return the number of live values. */
        [[nodiscard]] size_t Size() const noexcept { return values_.size(); }

        /** @brief Return whether the map contains no live values. */
        [[nodiscard]] bool Empty() const noexcept { return values_.empty(); }

        /** @brief Return the number of sparse slots currently backed by allocated chunks. */
        [[nodiscard]] size_t Capacity() const noexcept {
            if (chunks_.empty()) {
                return 0;
            }
            const size_t capacity = chunks_.size() * size_t{kChunkSize} - 1;
            return capacity < kMaximumSlotCount ? capacity : kMaximumSlotCount;
        }

        /** @brief Return the number of values insertable without growing either dense array. */
        [[nodiscard]] size_t DenseCapacity() const noexcept {
            return values_.capacity() < denseToSparse_.capacity() ? values_.capacity() : denseToSparse_.capacity();
        }

        /** @brief Return the maximum live count representable by handles and the underlying vectors. */
        [[nodiscard]] size_t MaxSize() const noexcept {
            size_t result = kMaximumSlotCount;
            if (values_.max_size() < result) {
                result = values_.max_size();
            }
            if (denseToSparse_.max_size() < result) {
                result = denseToSparse_.max_size();
            }
            const size_t maximumChunks = (kMaximumSlotCount + 1 + kChunkSize - 1) / kChunkSize;
            if (chunks_.max_size() < maximumChunks) {
                const size_t sparseLimit = chunks_.max_size() * size_t{kChunkSize} - 1;
                if (sparseLimit < result) {
                    result = sparseLimit;
                }
            }
            return result;
        }

        /** @brief Return the number of allocated sparse chunks. */
        [[nodiscard]] size_t ChunkCount() const noexcept { return chunks_.size(); }

        /**
         * @brief Preallocate sparse and dense storage for at least @p count live values.
         * @param[in] count Expected maximum number of simultaneously live values.
         */
        void Reserve(size_t count) {
            ReserveDense(count);
            ReserveSparse(count);
        }

        /**
         * @brief Preallocate both dense arrays without allocating sparse chunks.
         * @param[in] count Expected maximum number of simultaneously live values.
         */
        void ReserveDense(size_t count) {
            ValidateCapacity(count);
            values_.reserve(count);
            denseToSparse_.reserve(count);
        }

        /**
         * @brief Preallocate sparse metadata without reserving dense values.
         * @param[in] count Number of usable sparse slots to back with storage.
         */
        void ReserveSparse(size_t count) {
            ValidateCapacity(count);
            const size_t requiredChunks = RequiredChunkCount(count);
            if (chunks_.size() < requiredChunks) {
                chunks_.resize(requiredChunks);
            }
        }

        /** @} ----------------------------------------------------------------------------------------------- */

        /** @name Insertion and Removal @{ ------------------------------------------------------------------- */

        /**
         * @brief Construct one value and return its generational handle.
         * @tparam Args Constructor argument types for @p T.
         * @param[in] args Arguments forwarded to the constructor of @p T.
         * @return Handle resolving to the inserted value.
         * @throws std::length_error when the map reached @ref MaxSize.
         * @throws Any exception produced by allocation or construction of @p T. The live set and all existing handles
         * remain unchanged when insertion fails; allocated capacity may increase.
         */
        template<typename... Args>
            requires std::constructible_from<T, Args...>
        [[nodiscard]] SlotHandle Emplace(Args&&... args) {
            if (values_.size() >= MaxSize()) [[unlikely]] {
                throw std::length_error("ChunkedSlotMap exhausted all representable handles");
            }

            const bool reuseFreeSlot = freeHead_ != 0;
            const std::uint32_t sparseIndex = reuseFreeSlot ? freeHead_ : nextSlot_;
            EnsureSparseSlot(sparseIndex);
            SparseSlot& slot = SparseSlotAt(sparseIndex);
            const std::uint32_t nextFree = reuseFreeSlot ? slot.Payload() : 0;
            const std::uint32_t denseIndex = static_cast<std::uint32_t>(values_.size());

            denseToSparse_.push_back(sparseIndex);
            try {
                values_.emplace_back(std::forward<Args>(args)...);
            } catch (...) {
                denseToSparse_.pop_back();
                throw;
            }

            if (reuseFreeSlot) {
                freeHead_ = nextFree;
            } else {
                ++nextSlot_;
            }
            const std::uint32_t generation = OccupiedGenerationAfter(slot.Generation());
            slot.Set(generation, denseIndex);
            return SlotHandle{.index = sparseIndex, .generation = generation};
        }

        /**
         * @brief Insert every value from @p range and return handles in range order.
         * @details A sized range is reserved once before insertion. If construction fails, all values inserted by this
         * call are removed in reverse order, preserving the pre-call live set and handles. The range must not alias
         * this map's dense storage.
         * @tparam Range Input range whose references construct @p T.
         * @param[in] range Values to insert.
         * @return Handles corresponding one-to-one with the input range.
         */
        template<std::ranges::input_range Range>
            requires std::constructible_from<T, std::ranges::range_reference_t<Range>>
        [[nodiscard]] std::vector<SlotHandle> EmplaceRange(Range&& range) {
            std::vector<SlotHandle> handles;
            if constexpr (std::ranges::sized_range<Range>) {
                const size_t count = static_cast<size_t>(std::ranges::size(range));
                if (count > MaxSize() - values_.size()) [[unlikely]] {
                    throw std::length_error("ChunkedSlotMap range exceeds the remaining capacity");
                }
                Reserve(values_.size() + count);
                handles.reserve(count);
            }

            try {
                for (auto&& value : range) {
                    const SlotHandle handle = Emplace(std::forward<decltype(value)>(value));
                    try {
                        handles.push_back(handle);
                    } catch (...) {
                        static_cast<void>(Free(handle));
                        throw;
                    }
                }
            } catch (...) {
                while (!handles.empty()) {
                    static_cast<void>(Free(handles.back()));
                    handles.pop_back();
                }
                throw;
            }
            return handles;
        }

        /**
         * @brief Remove the value identified by @p handle using constant-time swap-and-pop.
         * @param[in] handle Handle returned by this map.
         * @return @c true when a live value was removed; @c false for a null or stale handle.
         * @warning @p handle must originate from this map. Container identity is intentionally not stored in the
         * compact 8-byte handle.
         */
        [[nodiscard]] bool Free(SlotHandle handle) noexcept {
            const std::uint32_t denseIndex = ResolveDenseIndex(handle);
            if (denseIndex == kInvalidDenseIndex) [[unlikely]] {
                return false;
            }

            SparseSlot& slot = SparseSlotAt(handle.index);
            const std::uint32_t lastDenseIndex = static_cast<std::uint32_t>(values_.size() - 1);
            if (denseIndex != lastDenseIndex) {
                values_[denseIndex] = std::move(values_[lastDenseIndex]);
                const std::uint32_t movedSparseIndex = denseToSparse_[lastDenseIndex];
                denseToSparse_[denseIndex] = movedSparseIndex;
                SparseSlotAt(movedSparseIndex).SetPayload(denseIndex);
            }
            values_.pop_back();
            denseToSparse_.pop_back();

            slot.Set(FreeGenerationAfter(slot.Generation()), freeHead_);
            freeHead_ = handle.index;
            return true;
        }

        /**
         * @brief Remove all live handles in @p handles and ignore null, stale, and duplicate entries.
         * @details Sparse batches use repeated O(1) swap-and-pop. Batches containing at least three quarters of the
         * live set switch to one O(n) compaction pass because the surviving tail then requires fewer moves than the
         * number of removals.
         * @param[in] handles Handles to remove.
         * @return Number of distinct live values removed.
         */
        [[nodiscard]] size_t FreeBatch(std::span<const SlotHandle> handles) {
            if (handles.empty() || values_.empty()) {
                return 0;
            }
            const size_t compactionThreshold =
                (values_.size() * kBatchCompactionNumerator + kBatchCompactionDenominator - 1) /
                kBatchCompactionDenominator;
            if (handles.size() < compactionThreshold) {
                size_t removed = 0;
                for (const SlotHandle handle : handles) {
                    removed += Free(handle) ? 1U : 0U;
                }
                return removed;
            }

            std::vector<std::uint8_t> eraseFlags(values_.size(), std::uint8_t{0});
            size_t removed = 0;
            for (const SlotHandle handle : handles) {
                const std::uint32_t denseIndex = ResolveDenseIndex(handle);
                if (denseIndex != kInvalidDenseIndex && eraseFlags[denseIndex] == 0) {
                    eraseFlags[denseIndex] = 1;
                    ++removed;
                }
            }
            if (removed == 0) {
                return 0;
            }

            size_t writeIndex = 0;
            for (size_t readIndex = 0; readIndex < values_.size(); ++readIndex) {
                const std::uint32_t sparseIndex = denseToSparse_[readIndex];
                if (eraseFlags[readIndex] != 0) {
                    SparseSlot& slot = SparseSlotAt(sparseIndex);
                    slot.Set(FreeGenerationAfter(slot.Generation()), freeHead_);
                    freeHead_ = sparseIndex;
                    continue;
                }
                if (writeIndex != readIndex) {
                    values_[writeIndex] = std::move(values_[readIndex]);
                    denseToSparse_[writeIndex] = sparseIndex;
                    SparseSlotAt(sparseIndex).SetPayload(static_cast<std::uint32_t>(writeIndex));
                }
                ++writeIndex;
            }

            values_.erase(values_.begin() + static_cast<std::ptrdiff_t>(writeIndex), values_.end());
            denseToSparse_.erase(denseToSparse_.begin() + static_cast<std::ptrdiff_t>(writeIndex),
                                 denseToSparse_.end());
            return removed;
        }

        /**
         * @brief Destroy all values, invalidate every handle, and retain allocated capacity.
         * @details Complexity is linear in the live count rather than sparse capacity.
         */
        void Clear() noexcept {
            for (const std::uint32_t sparseIndex : denseToSparse_) {
                SparseSlot& slot = SparseSlotAt(sparseIndex);
                slot.Set(FreeGenerationAfter(slot.Generation()), 0);
            }
            values_.clear();
            denseToSparse_.clear();
            freeHead_ = 0;
            nextSlot_ = 1;
        }

        /** @} ----------------------------------------------------------------------------------------------- */

        /** @name Lookup @{ ---------------------------------------------------------------------------------- */

        /** @brief Return the value for @p handle, or @c nullptr when the handle is not live in this map. */
        [[nodiscard]] auto Get(this auto& self, SlotHandle handle) noexcept {
            const std::uint32_t denseIndex = self.ResolveDenseIndex(handle);
            using Pointer = decltype(self.values_.data());
            return denseIndex == kInvalidDenseIndex ? static_cast<Pointer>(nullptr)
                                                    : std::addressof(self.values_[denseIndex]);
        }

        /**
         * @brief Return the value for @p handle without checking its generation or bounds.
         * @warning Passing any handle not currently live in this map is undefined behavior.
         */
        [[nodiscard]] decltype(auto) operator[](this auto& self, SlotHandle handle) noexcept {
            return (self.values_[self.SparseSlotAt(handle.index).Payload()]);
        }

        /** @brief Return whether @p handle currently resolves to a live value in this map. */
        [[nodiscard]] bool IsAlive(SlotHandle handle) const noexcept {
            return ResolveDenseIndex(handle) != kInvalidDenseIndex;
        }

        /** @brief Prefetch sparse metadata for a handle expected to be resolved shortly. */
        void Prefetch(SlotHandle handle) const noexcept {
            static_cast<void>(handle);
#if defined(__has_builtin)
#    if __has_builtin(__builtin_prefetch)
            if (handle.IsValid() && handle.index < nextSlot_) {
                __builtin_prefetch(std::addressof(SparseSlotAt(handle.index)), 0, 1);
            }
#    endif
#else
            static_cast<void>(handle);
#endif
        }

        /**
         * @brief Resolve a batch of handles with sparse metadata prefetching.
         * @param[in] handles Handles to resolve.
         * @param[out] output Pointer output with capacity for every input handle; stale handles produce @c nullptr.
         * @return Number of live handles resolved.
         * @throws std::invalid_argument when @p output is smaller than @p handles.
         */
        [[nodiscard]] size_t GetBatch(std::span<const SlotHandle> handles, std::span<T*> output) {
            ValidateBatchOutput(handles.size(), output.size());
            size_t resolved = 0;
            for (size_t index = 0; index < handles.size(); ++index) {
                if (index + kPrefetchDistance < handles.size()) {
                    Prefetch(handles[index + kPrefetchDistance]);
                }
                output[index] = Get(handles[index]);
                resolved += output[index] != nullptr ? 1U : 0U;
            }
            return resolved;
        }

        /** @brief Resolve a batch of handles through a const map. */
        [[nodiscard]] size_t GetBatch(std::span<const SlotHandle> handles, std::span<const T*> output) const {
            ValidateBatchOutput(handles.size(), output.size());
            size_t resolved = 0;
            for (size_t index = 0; index < handles.size(); ++index) {
                if (index + kPrefetchDistance < handles.size()) {
                    Prefetch(handles[index + kPrefetchDistance]);
                }
                output[index] = Get(handles[index]);
                resolved += output[index] != nullptr ? 1U : 0U;
            }
            return resolved;
        }

        /** @} ----------------------------------------------------------------------------------------------- */

        /** @name Dense Access @{ ---------------------------------------------------------------------------- */

        /** @brief Return a contiguous span over every live value in dense order. */
        [[nodiscard]] auto Values(this auto& self) noexcept { return std::span{self.values_}; }

        /** @brief Return the first dense value, or @c nullptr when the map is empty. */
        [[nodiscard]] auto Data(this auto& self) noexcept {
            using Pointer = decltype(self.values_.data());
            return self.values_.empty() ? static_cast<Pointer>(nullptr) : self.values_.data();
        }

        /** @brief Return an iterator to the first dense value. */
        [[nodiscard]] auto begin() noexcept { return values_.begin(); }

        /** @brief Return a constant iterator to the first dense value. */
        [[nodiscard]] auto begin() const noexcept { return values_.begin(); }

        /** @brief Return an iterator past the last dense value. */
        [[nodiscard]] auto end() noexcept { return values_.end(); }

        /** @brief Return a constant iterator past the last dense value. */
        [[nodiscard]] auto end() const noexcept { return values_.end(); }

        /** @brief Return a constant iterator to the first dense value. */
        [[nodiscard]] auto cbegin() const noexcept { return values_.cbegin(); }

        /** @brief Return a constant iterator past the last dense value. */
        [[nodiscard]] auto cend() const noexcept { return values_.cend(); }

        /**
         * @brief Invoke @p function for every live value in dense order.
         * @details A callable accepting `(SlotHandle, T&)` is preferred when both that form and `(T&)` are valid.
         * The callable must not structurally modify this map while iteration is in progress.
         */
        template<typename Function>
            requires(std::invocable<Function&, SlotHandle, T&> || std::invocable<Function&, T&>)
        void ForEach(Function&& function) {
            auto& callable = function;
            for (size_t denseIndex = 0; denseIndex < values_.size(); ++denseIndex) {
                if constexpr (std::invocable<Function&, SlotHandle, T&>) {
                    const std::uint32_t sparseIndex = denseToSparse_[denseIndex];
                    const SparseSlot& slot = SparseSlotAt(sparseIndex);
                    std::invoke(callable, SlotHandle{.index = sparseIndex, .generation = slot.Generation()},
                                values_[denseIndex]);
                } else {
                    std::invoke(callable, values_[denseIndex]);
                }
            }
        }

        /** @brief Invoke @p function for every live value through constant references. */
        template<typename Function>
            requires(std::invocable<Function&, SlotHandle, const T&> || std::invocable<Function&, const T&>)
        void ForEach(Function&& function) const {
            auto& callable = function;
            for (size_t denseIndex = 0; denseIndex < values_.size(); ++denseIndex) {
                if constexpr (std::invocable<Function&, SlotHandle, const T&>) {
                    const std::uint32_t sparseIndex = denseToSparse_[denseIndex];
                    const SparseSlot& slot = SparseSlotAt(sparseIndex);
                    std::invoke(callable, SlotHandle{.index = sparseIndex, .generation = slot.Generation()},
                                values_[denseIndex]);
                } else {
                    std::invoke(callable, values_[denseIndex]);
                }
            }
        }

        /** @} ----------------------------------------------------------------------------------------------- */

    private:
        [[nodiscard]] static constexpr size_t RequiredChunkCount(size_t count) noexcept {
            return count == 0 ? 0 : (count + 1 + kChunkSize - 1) / kChunkSize;
        }

        void ValidateCapacity(size_t count) const {
            if (count > MaxSize()) [[unlikely]] {
                throw std::length_error("ChunkedSlotMap capacity exceeds the handle representation");
            }
        }

        static void ValidateBatchOutput(size_t inputSize, size_t outputSize) {
            if (outputSize < inputSize) [[unlikely]] {
                throw std::invalid_argument("ChunkedSlotMap batch output is smaller than the input");
            }
        }

        void EnsureSparseSlot(std::uint32_t index) {
            const size_t requiredChunkCount = (index >> ChunkBits) + 1;
            if (chunks_.size() < requiredChunkCount) {
                chunks_.resize(requiredChunkCount);
            }
        }

        [[nodiscard]] decltype(auto) SparseSlotAt(this auto& self, std::uint32_t index) noexcept {
            return (self.chunks_[index >> ChunkBits].slots[index & kChunkMask]);
        }

        [[nodiscard]] std::uint32_t ResolveDenseIndex(SlotHandle handle) const noexcept {
            if (!handle.IsValid() || handle.index >= nextSlot_) [[unlikely]] {
                return kInvalidDenseIndex;
            }
            const SparseSlot& slot = SparseSlotAt(handle.index);
            if (!slot.IsOccupied() || slot.Generation() != handle.generation) [[unlikely]] {
                return kInvalidDenseIndex;
            }
            return slot.Payload();
        }

        [[nodiscard]] static constexpr std::uint32_t OccupiedGenerationAfter(std::uint32_t generation) noexcept {
            return generation + 1;
        }

        [[nodiscard]] static constexpr std::uint32_t FreeGenerationAfter(std::uint32_t generation) noexcept {
            const std::uint32_t next = generation + 1;
            return next == 0 ? 2 : next;
        }

        void ResetMovedFrom() noexcept {
            chunks_.clear();
            values_.clear();
            denseToSparse_.clear();
            freeHead_ = 0;
            nextSlot_ = 1;
        }
    };

} // namespace Sora

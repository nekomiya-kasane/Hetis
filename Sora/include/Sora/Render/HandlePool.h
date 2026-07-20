/**
 * @file HandlePool.h
 * @brief Provide stable-address storage, generational lookup, and deferred reclamation for Render resources.
 * @details @ref Sora::Render::HandlePool stores payloads in lazily allocated chunks that are never moved. Allocation,
 * immediate release, deferred-state transitions, reclamation, reservation, and debug-name mutation are serialized.
 * Lookup reads an atomically published packed handle and does not acquire the pool mutex.
 *
 * The following example creates a resource record, invalidates its public handle immediately, destroys the native
 * object after GPU completion, and finally returns the slot to the pool:
 * @code{.cpp}
 * struct BufferRecord {
 *     uint64_t native = 0;
 * };
 *
 * Sora::Render::HandlePool<BufferRecord, Sora::Render::HandleKind::Buffer> buffers;
 * auto allocation = buffers.Emplace(Sora::Render::Backend::Vulkan, BufferRecord{.native = 42});
 * if (!allocation) {
 *     return allocation.error();
 * }
 *
 * const Sora::Render::BufferHandle handle = allocation->handle;
 * if (BufferRecord* record = buffers.Lookup(handle)) {
 *     Submit(record->native);
 * }
 *
 * if (const auto reclaim = buffers.MarkDead(handle)) {
 *     WaitForGpu();
 *     if (BufferRecord* record = buffers.LookupDead(*reclaim)) {
 *         DestroyNativeBuffer(record->native);
 *     }
 *     static_cast<void>(buffers.Reclaim(*reclaim));
 * }
 * @endcode
 *
 * Handles and reclaim tokens are local to their originating pool. Lookup returns a non-owning pointer: callers must
 * prevent @ref Free or @ref Reclaim from running concurrently with pointer acquisition and use. Concurrent growth and
 * allocation do not move existing payloads. Prefer deferred destruction when readers can outlive API-level removal.
 * @ingroup Render
 */

#pragma once

#include <Sora/Core/Guard.h>
#include <Sora/ErrorCode.h>
#include <Sora/Render/Handle.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace Sora::Render {

    namespace Concept {

        /** @brief Constrain pool payloads to objects whose destruction cannot throw. */
        template<typename T>
        concept HandlePoolValue = std::destructible<T>;

    } // namespace Concept

    /**
     * @brief Store stable-address Render payloads behind typed, generation-checked handles.
     * @tparam T Payload type; its destructor must be non-throwing.
     * @tparam Tag Compile-time resource identity from @ref Handle.h.
     * @tparam Policy Pool capacity policy; defaults to the policy annotated on @p Tag.
     *
     * @details Capacity grows geometrically up to @p Policy.maximumCount. Each chunk is allocated once and retained
     * until
     * pool destruction, so growth does not move existing payloads. Free slots use a FIFO queue: this improves temporal
     * locality less than LIFO reuse, but distributes generation consumption across the pool and greatly delays 16-bit
     * generation exhaustion. A slot is permanently retired instead of wrapping its generation and reviving an ancient
     * stale handle.
     *
     * Mutating operations are mutually synchronized. @ref Lookup and @ref LookupDead use atomic metadata without the
     * mutex, but their returned raw pointers do not own lifetime. External scheduling, a frame/fence lifetime regime,
     * or deferred reclamation must prevent concurrent destruction while a returned pointer is in use.
     */
    template<Concept::HandlePoolValue T, HandleKind Tag, HandlePoolPolicy Policy = Traits::HandlePoolPolicyOf<Tag>>
        requires Concept::PooledHandleKind<Tag> && (Policy.IsValid())
    class HandlePool {
    public:
        using HandleType = Handle<Tag>; /**< Strongly typed public handle. */

        /** @brief Handle and stable payload address returned by a successful allocation. */
        struct Allocation {
            HandleType handle;  /**< Public resource identity. */
            T* value = nullptr; /**< Stable payload address, valid until release or reclamation. */
        };

        /** @brief Generation-checked authority to inspect and reclaim one deferred payload. */
        struct ReclaimToken {
            uint32_t index = std::numeric_limits<uint32_t>::max(); /**< Deferred slot index. */
            uint16_t generation = 0;                               /**< Post-invalidation slot generation. */

            /** @brief Return whether this token is non-null. */
            [[nodiscard]] constexpr bool IsValid() const noexcept {
                return index != std::numeric_limits<uint32_t>::max();
            }

            /** @brief Convert to @c true when this token is non-null. */
            [[nodiscard]] constexpr explicit operator bool() const noexcept { return IsValid(); }

            friend constexpr bool operator==(const ReclaimToken&, const ReclaimToken&) noexcept = default;
        };

    private:
        enum class SlotState : uint8_t { Free, Alive, Dead, DeadRetired, Retired };

        struct Slot {
            std::atomic<uint64_t> publishedHandle{0};
            std::atomic<uint16_t> generation{1};
            std::atomic<SlotState> state{SlotState::Free};
            uint32_t nextFree = HandleType::kInvlidIndex;
            alignas(T) std::byte storage[sizeof(T)];

            Slot() = default;
            Slot(const Slot&) = delete;
            Slot& operator=(const Slot&) = delete;

            /** @brief Return uninitialized storage suitable for @c std::construct_at. */
            [[nodiscard]] T* Storage() noexcept { return reinterpret_cast<T*>(storage); }

            /** @brief Return the live object occupying this slot. */
            [[nodiscard]] T* Object() noexcept { return std::launder(Storage()); }

            /** @brief Return the live object occupying this slot. */
            [[nodiscard]] const T* Object() const noexcept { return std::launder(reinterpret_cast<const T*>(storage)); }
        };

        struct Chunk {
            std::array<Slot, Policy.allocatedChunkSize> slots;
        };

    public:
        /** @name Construction and Assignment @{ ------------------------------------------------------------ */

        /** @brief Construct a pool and initialize @p Policy.initialCount slots. */
        HandlePool() {
            if constexpr (Policy.initialCount != 0) {
                GrowToUnlocked(Policy.initialCount);
            }
        }

        /** @brief Destroy every live or deferred payload and release all chunks. */
        ~HandlePool() {
            const uint32_t capacity = capacity_.load(std::memory_order_relaxed);
            for (uint32_t index = 0; index < capacity; ++index) {
                Slot& slot = SlotAt(index);
                const SlotState state = slot.state.load(std::memory_order_relaxed);
                if (state == SlotState::Alive || state == SlotState::Dead || state == SlotState::DeadRetired) {
                    std::destroy_at(slot.Object());
                }
            }
        }

        HandlePool(const HandlePool&) = delete;
        HandlePool& operator=(const HandlePool&) = delete;
        HandlePool(HandlePool&&) = delete;
        HandlePool& operator=(HandlePool&&) = delete;

        /** @} ----------------------------------------------------------------------------------------------- */

        /** @name Allocation and Capacity @{ ---------------------------------------------------------------- */

        /**
         * @brief Construct a payload in a free slot and publish its typed handle.
         * @tparam Args Constructor argument types for @p T.
         * @param[in] backend Backend encoded in the returned handle.
         * @param[in] args Arguments forwarded to @p T's constructor.
         * @return Allocation record, or @ref ErrorCode::ResourceExhausted when no reusable or growable slot remains.
         * @throws Any exception thrown by chunk allocation or @p T's constructor; constructor failure restores the
         * free slot before propagating the exception.
         */
        template<typename... Args>
            requires std::constructible_from<T, Args...>
        [[nodiscard]] Result<Allocation> Emplace(Backend backend, Args&&... args) {
            std::unique_lock lock{mutex_};
            if (freeHead_ == HandleType::kInvlidIndex && !GrowUnlocked()) {
                return std::unexpected{ErrorCode::ResourceExhausted};
            }

            const uint32_t index = PopFreeUnlocked();
            Slot& slot = SlotAt(index);
            Sora::ScopeExit rollback{[this, index] noexcept {
                std::scoped_lock rollbackLock{mutex_};
                PushFreeUnlocked(index);
            }};
            lock.unlock();
            T* object = std::construct_at(slot.Storage(), std::forward<Args>(args)...);

            lock.lock();
            const uint16_t generation = slot.generation.load(std::memory_order_relaxed);
            const HandleType handle = HandleType::Pack(generation, index, backend);
            slot.state.store(SlotState::Alive, std::memory_order_relaxed);
            slot.publishedHandle.store(handle.Raw(), std::memory_order_release);
            liveCount_.fetch_add(1, std::memory_order_relaxed);
            rollback.Release();
            return Allocation{.handle = handle, .value = object};
        }

        /**
         * @brief Default-construct a payload in a free slot.
         * @param[in] backend Backend encoded in the returned handle.
         * @return Allocation record, or @ref ErrorCode::ResourceExhausted when the pool cannot grow.
         */
        [[nodiscard]] Result<Allocation> Allocate(Backend backend = Backend::Unspecified)
            requires std::default_initializable<T>
        {
            return Emplace(backend);
        }

        /**
         * @brief Ensure at least @p requested slots have initialized metadata and stable storage.
         * @param[in] requested Required capacity.
         * @return Success, or @ref ErrorCode::ResourceExhausted when @p requested exceeds @p Policy.maximumCount.
         * @throws std::bad_alloc when host allocation fails.
         */
        [[nodiscard]] VoidResult Reserve(size_t requested) {
            if (requested > Policy.maximumCount) {
                return std::unexpected{ErrorCode::ResourceExhausted};
            }
            std::scoped_lock lock{mutex_};
            const size_t current = capacity_.load(std::memory_order_relaxed);
            if (requested > current) {
                GrowToUnlocked(requested);
            }
            return {};
        }

        /** @brief Return initialized slot capacity. */
        [[nodiscard]] size_t Capacity() const noexcept { return capacity_.load(std::memory_order_acquire); }

        /** @brief Return reusable initialized slot count. */
        [[nodiscard]] size_t FreeCount() const noexcept { return freeCount_.load(std::memory_order_relaxed); }

        /** @brief Return payloads reachable through public handles. */
        [[nodiscard]] size_t LiveCount() const noexcept { return liveCount_.load(std::memory_order_relaxed); }

        /** @brief Return invalidated payloads awaiting deferred reclamation. */
        [[nodiscard]] size_t PendingReclaimCount() const noexcept { return deadCount_.load(std::memory_order_relaxed); }

        /** @brief Return slots permanently removed to prevent generation wraparound. */
        [[nodiscard]] size_t RetiredCount() const noexcept { return retiredCount_.load(std::memory_order_relaxed); }

        /** @} ----------------------------------------------------------------------------------------------- */

        /** @name Lookup @{ ---------------------------------------------------------------------------------- */

        /**
         * @brief Return the live payload identified by @p handle, or @c nullptr for a null, foreign, or stale handle.
         * @warning The returned pointer is non-owning. @ref Free and @ref Reclaim must not run concurrently with this
         * lookup or with use of the returned pointer. Concurrent allocation and capacity growth preserve its address.
         */
        [[nodiscard]] auto Lookup(this auto& self, HandleType handle) noexcept {
            using Self = std::remove_reference_t<decltype(self)>;
            using Pointer = std::conditional_t<std::is_const_v<Self>, const T*, T*>;
            if (!handle.IsValid()) [[unlikely]] {
                return static_cast<Pointer>(nullptr);
            }
            const uint32_t capacity = self.capacity_.load(std::memory_order_acquire);
            if (handle.GetIndex() >= capacity) [[unlikely]] {
                return static_cast<Pointer>(nullptr);
            }
            auto& slot = self.SlotAt(handle.GetIndex());
            if (slot.publishedHandle.load(std::memory_order_acquire) != handle.Raw()) [[unlikely]] {
                return static_cast<Pointer>(nullptr);
            }
            return static_cast<Pointer>(slot.Object());
        }

        /**
         * @brief Return a deferred payload authorized by @p token, or @c nullptr when the token is stale.
         * @warning The returned pointer is non-owning. @ref Reclaim must not run concurrently with this lookup or with
         * use of the returned pointer.
         */
        [[nodiscard]] auto LookupDead(this auto& self, ReclaimToken token) noexcept {
            using Self = std::remove_reference_t<decltype(self)>;
            using Pointer = std::conditional_t<std::is_const_v<Self>, const T*, T*>;
            if (!token.IsValid()) [[unlikely]] {
                return static_cast<Pointer>(nullptr);
            }
            const uint32_t capacity = self.capacity_.load(std::memory_order_acquire);
            if (token.index >= capacity) [[unlikely]] {
                return static_cast<Pointer>(nullptr);
            }
            auto& slot = self.SlotAt(token.index);
            const SlotState state = slot.state.load(std::memory_order_acquire);
            if ((state != SlotState::Dead && state != SlotState::DeadRetired) ||
                slot.generation.load(std::memory_order_relaxed) != token.generation) [[unlikely]] {
                return static_cast<Pointer>(nullptr);
            }
            return static_cast<Pointer>(slot.Object());
        }

        /** @} ----------------------------------------------------------------------------------------------- */

        /** @name Release and Deferred Reclamation @{ ------------------------------------------------------- */

        /**
         * @brief Destroy a live payload immediately, invalidate @p handle, and recycle or retire its slot.
         * @param[in] handle Handle returned by this pool.
         * @return @c true when a live payload was destroyed; @c false for a null, foreign, or stale handle.
         * @warning Callers must ensure no thread or GPU-completion path can still access the payload.
         */
        [[nodiscard]] bool Free(HandleType handle) noexcept {
            if (!handle.IsValid()) {
                return false;
            }

            std::scoped_lock lock{mutex_};
            Slot* slot = FindLiveSlotUnlocked(handle);
            if (slot == nullptr) {
                return false;
            }

            slot->publishedHandle.store(0, std::memory_order_release);
            EraseDebugNameUnlocked(handle);
            std::destroy_at(slot->Object());
            liveCount_.fetch_sub(1, std::memory_order_relaxed);
            const bool retired = AdvanceGenerationUnlocked(*slot);
            if (retired) {
                slot->state.store(SlotState::Retired, std::memory_order_release);
                retiredCount_.fetch_add(1, std::memory_order_relaxed);
            } else {
                slot->state.store(SlotState::Free, std::memory_order_release);
                PushFreeUnlocked(handle.GetIndex());
            }
            return true;
        }

        /**
         * @brief Invalidate @p handle while retaining its payload for deferred native-resource destruction.
         * @param[in] handle Handle returned by this pool.
         * @return Generation-checked reclamation token, or @c std::nullopt for a null, foreign, or stale handle.
         */
        [[nodiscard]] std::optional<ReclaimToken> MarkDead(HandleType handle) noexcept {
            if (!handle.IsValid()) {
                return std::nullopt;
            }
            std::scoped_lock lock{mutex_};
            Slot* slot = FindLiveSlotUnlocked(handle);
            if (slot == nullptr) {
                return std::nullopt;
            }

            slot->publishedHandle.store(0, std::memory_order_release);
            EraseDebugNameUnlocked(handle);
            liveCount_.fetch_sub(1, std::memory_order_relaxed);
            deadCount_.fetch_add(1, std::memory_order_relaxed);
            const bool retired = AdvanceGenerationUnlocked(*slot);
            const SlotState state = retired ? SlotState::DeadRetired : SlotState::Dead;
            const uint16_t generation = slot->generation.load(std::memory_order_relaxed);
            slot->state.store(state, std::memory_order_release);
            return ReclaimToken{.index = handle.GetIndex(), .generation = generation};
        }

        /**
         * @brief Destroy a deferred payload and recycle or retire its slot.
         * @param[in] token Token returned by @ref MarkDead.
         * @return @c true when the matching deferred payload was reclaimed; @c false for a stale or duplicate token.
         */
        [[nodiscard]] bool Reclaim(ReclaimToken token) noexcept {
            if (!token.IsValid()) {
                return false;
            }
            std::scoped_lock lock{mutex_};
            const uint32_t capacity = capacity_.load(std::memory_order_relaxed);
            if (token.index >= capacity) {
                return false;
            }
            Slot& slot = SlotAt(token.index);
            const SlotState state = slot.state.load(std::memory_order_relaxed);
            if ((state != SlotState::Dead && state != SlotState::DeadRetired) ||
                slot.generation.load(std::memory_order_relaxed) != token.generation) {
                return false;
            }

            std::destroy_at(slot.Object());
            deadCount_.fetch_sub(1, std::memory_order_relaxed);
            if (state == SlotState::DeadRetired) {
                slot.state.store(SlotState::Retired, std::memory_order_release);
                retiredCount_.fetch_add(1, std::memory_order_relaxed);
            } else {
                slot.state.store(SlotState::Free, std::memory_order_release);
                PushFreeUnlocked(token.index);
            }
            return true;
        }

        /** @} ----------------------------------------------------------------------------------------------- */

        /** @name Debug Metadata @{ -------------------------------------------------------------------------- */

        /**
         * @brief Copy a semantic debug name for a live handle in debug builds; perform no work in release builds.
         * @param[in] handle Live handle returned by this pool.
         * @param[in] name Semantic resource name; an empty name removes the current entry.
         */
        void SetDebugName([[maybe_unused]] HandleType handle, [[maybe_unused]] std::string_view name) {
#ifndef NDEBUG
            std::scoped_lock lock{mutex_};
            if (FindLiveSlotUnlocked(handle) == nullptr) {
                return;
            }
            if (name.empty()) {
                debugNames_.erase(handle.Raw());
            } else {
                debugNames_.insert_or_assign(handle.Raw(), name);
            }
#endif
        }

        /** @brief Return a copied semantic debug name, or an empty string when absent or compiled for release. */
        [[nodiscard]] std::string GetDebugName([[maybe_unused]] HandleType handle) const {
#ifndef NDEBUG
            std::scoped_lock lock{mutex_};
            if (FindLiveSlotUnlocked(handle) != nullptr) {
                if (const auto iterator = debugNames_.find(handle.Raw()); iterator != debugNames_.end()) {
                    return iterator->second;
                }
            }
#endif
            return {};
        }

        /** @} ----------------------------------------------------------------------------------------------- */

    private:
        [[nodiscard]] Slot& SlotAt(uint32_t index) noexcept {
            Chunk* chunk = directory_[index / Policy.allocatedChunkSize].load(std::memory_order_relaxed);
            return chunk->slots[index % Policy.allocatedChunkSize];
        }

        [[nodiscard]] const Slot& SlotAt(uint32_t index) const noexcept {
            const Chunk* chunk = directory_[index / Policy.allocatedChunkSize].load(std::memory_order_relaxed);
            return chunk->slots[index % Policy.allocatedChunkSize];
        }

        [[nodiscard]] Slot* FindLiveSlotUnlocked(HandleType handle) noexcept {
            const uint32_t capacity = capacity_.load(std::memory_order_relaxed);
            if (handle.GetIndex() >= capacity) {
                return nullptr;
            }
            Slot& slot = SlotAt(handle.GetIndex());
            return slot.publishedHandle.load(std::memory_order_relaxed) == handle.Raw() ? std::addressof(slot)
                                                                                        : nullptr;
        }

        [[nodiscard]] const Slot* FindLiveSlotUnlocked(HandleType handle) const noexcept {
            const uint32_t capacity = capacity_.load(std::memory_order_relaxed);
            if (handle.GetIndex() >= capacity) {
                return nullptr;
            }
            const Slot& slot = SlotAt(handle.GetIndex());
            return slot.publishedHandle.load(std::memory_order_relaxed) == handle.Raw() ? std::addressof(slot)
                                                                                        : nullptr;
        }

        [[nodiscard]] bool GrowUnlocked() {
            const size_t current = capacity_.load(std::memory_order_relaxed);
            if (current == Policy.maximumCount) {
                return false;
            }
            size_t target = current == 0 ? std::min(Policy.maximumCount, Policy.allocatedChunkSize) : current * 2;
            if (target < current || target > Policy.maximumCount) {
                target = Policy.maximumCount;
            }
            GrowToUnlocked(target);
            return true;
        }

        void GrowToUnlocked(size_t target) {
            const size_t current = capacity_.load(std::memory_order_relaxed);
            if (target <= current) {
                return;
            }

            const size_t requiredChunks = (target + Policy.allocatedChunkSize - 1) / Policy.allocatedChunkSize;
            for (size_t chunkIndex = 0; chunkIndex < requiredChunks; ++chunkIndex) {
                if (ownedChunks_[chunkIndex] == nullptr) {
                    auto chunk = std::make_unique<Chunk>();
                    Chunk* published = chunk.get();
                    ownedChunks_[chunkIndex] = std::move(chunk);
                    directory_[chunkIndex].store(published, std::memory_order_relaxed);
                }
            }

            AppendFreeRangeUnlocked(static_cast<uint32_t>(current), static_cast<uint32_t>(target));
            capacity_.store(static_cast<uint32_t>(target), std::memory_order_release);
        }

        void AppendFreeRangeUnlocked(uint32_t begin, uint32_t end) noexcept {
            if (begin == end) {
                return;
            }
            for (uint32_t index = begin; index < end; ++index) {
                Slot& slot = SlotAt(index);
                slot.nextFree = index + 1 < end ? index + 1 : HandleType::kInvlidIndex;
            }
            if (freeTail_ == HandleType::kInvlidIndex) {
                freeHead_ = begin;
            } else {
                SlotAt(freeTail_).nextFree = begin;
            }
            freeTail_ = end - 1;
            freeCount_.fetch_add(end - begin, std::memory_order_relaxed);
        }

        [[nodiscard]] uint32_t PopFreeUnlocked() noexcept {
            const uint32_t index = freeHead_;
            Slot& slot = SlotAt(index);
            freeHead_ = slot.nextFree;
            slot.nextFree = HandleType::kInvlidIndex;
            if (freeHead_ == HandleType::kInvlidIndex) {
                freeTail_ = HandleType::kInvlidIndex;
            }
            freeCount_.fetch_sub(1, std::memory_order_relaxed);
            return index;
        }

        void PushFreeUnlocked(uint32_t index) noexcept {
            SlotAt(index).nextFree = HandleType::kInvlidIndex;
            if (freeTail_ == HandleType::kInvlidIndex) {
                freeHead_ = index;
            } else {
                SlotAt(freeTail_).nextFree = index;
            }
            freeTail_ = index;
            freeCount_.fetch_add(1, std::memory_order_relaxed);
        }

        [[nodiscard]] static bool AdvanceGenerationUnlocked(Slot& slot) noexcept {
            const uint16_t generation = slot.generation.load(std::memory_order_relaxed);
            if (generation == std::numeric_limits<uint16_t>::max()) {
                return true;
            }
            slot.generation.store(static_cast<uint16_t>(generation + 1), std::memory_order_relaxed);
            return false;
        }

        void EraseDebugNameUnlocked([[maybe_unused]] HandleType handle) noexcept {
#ifndef NDEBUG
            debugNames_.erase(handle.Raw());
#endif
        }

        mutable std::mutex mutex_;
        uint32_t freeHead_ = HandleType::kInvlidIndex;
        uint32_t freeTail_ = HandleType::kInvlidIndex;
        std::atomic<uint32_t> capacity_{0};
        std::atomic<uint32_t> freeCount_{0};
        std::atomic<uint32_t> liveCount_{0};
        std::atomic<uint32_t> deadCount_{0};
        std::atomic<uint32_t> retiredCount_{0};
        std::array<std::atomic<Chunk*>, Policy.MaximumChunkCount()> directory_{};
        std::array<std::unique_ptr<Chunk>, Policy.MaximumChunkCount()> ownedChunks_{};
#ifndef NDEBUG
        std::unordered_map<uint64_t, std::string> debugNames_;
#endif
    };

    /** @brief Namespace-level concept alias kept beside @ref HandlePool for unqualified Render API checks. */
    template<typename T>
    concept HandlePoolValue = Concept::HandlePoolValue<T>;

} // namespace Sora::Render

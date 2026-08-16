/**
 * @file MichaelScottQueue.h
 * @brief Bounded-allocation Michael-Scott MPMC queue with integrated hazard-pointer reclamation.
 * @ingroup Core
 */
#pragma once

#include "Sora/Platform.h"
#include "Sora/Core/Concurrency/FalseSharingChecker.h"
#include "Sora/Core/Concurrency/QueueTopology.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

namespace Sora {

    /** @brief Physical ownership of the queue's compile-time-sized node arena. */
    enum class MichaelScottBacking : std::uint8_t {
        Inline,
        Heap,
    };

    /**
     * @brief Structural compile-time configuration for @ref MichaelScottQueue.
     * @details @p capacity is the logical item bound. The implementation derives additional dummy and reclamation
     * nodes from @p maxConcurrentOperations, so reclamation lag never reduces the advertised logical capacity.
     */
    struct MichaelScottQueueConfig {
        std::size_t capacity = 1024;
        std::size_t maxConcurrentOperations = 16;
        std::size_t reclamationThreshold = 32;
        MichaelScottBacking backing = MichaelScottBacking::Heap;
        bool enableStats = false;
    };

    namespace Detail {

        [[nodiscard]] consteval bool ValidateMichaelScottConfig(MichaelScottQueueConfig config) noexcept {
            if (config.capacity == 0 || config.maxConcurrentOperations == 0 || config.reclamationThreshold == 0) {
                return false;
            }
            // The arena contains the logical items, one permanent dummy, and three
            // nodes per concurrently active operation. Validate the actual node
            // formula before any uint32_t index or sentinel is formed.
            constexpr std::size_t kMaxIndexedNodes = std::numeric_limits<std::uint32_t>::max() - 1uz;
            if (config.capacity > kMaxIndexedNodes - 1uz) {
                return false;
            }
            return config.maxConcurrentOperations <= (kMaxIndexedNodes - config.capacity - 1uz) / 3uz;
        }

        template<typename T>
        concept MichaelScottElement = std::is_nothrow_move_constructible_v<T> && std::is_nothrow_destructible_v<T>;

        template<typename Node, std::size_t Count>
        struct InlineMichaelScottStore {
            std::array<Node, Count> nodes{};

            [[nodiscard]] Node* Data() noexcept { return nodes.data(); }
            [[nodiscard]] const Node* Data() const noexcept { return nodes.data(); }
        };

        template<typename Node, std::size_t Count>
        struct HeapMichaelScottStore {
            std::unique_ptr<Node[]> nodes{std::make_unique<Node[]>(Count)};

            [[nodiscard]] Node* Data() noexcept { return nodes.get(); }
            [[nodiscard]] const Node* Data() const noexcept { return nodes.get(); }
        };

        struct NoMichaelScottStats {};

        struct MichaelScottStats {
            std::atomic<std::uint64_t> pushes{0};
            std::atomic<std::uint64_t> pops{0};
            std::atomic<std::uint64_t> collections{0};
            std::atomic<std::uint64_t> reclaimedNodes{0};
        };

    } // namespace Detail

    /**
     * @brief Fixed-arena Michael-Scott MPMC FIFO with address-granular safe reclamation.
     * @tparam T Nothrow-movable payload type.
     * @tparam Config Structural capacity, concurrency, reclamation, backing, and instrumentation configuration.
     *
     * @details Successful enqueue linearizes at the release CAS from the observed tail node's null @c next pointer.
     * Successful dequeue linearizes at the acquire-release CAS claiming the current head payload. Structural head
     * advancement may precede that claim, but a claimed payload cannot be skipped or reclaimed until consumption
     * publishes the @c consumed state. The fixed arena has @c capacity+1+3*maxConcurrentOperations nodes: logical
     * items, the current dummy, and the maximum number of distinct retired nodes protected by active operations.
     *
     * The configured operation bound is a concurrency precondition, not a thread-affinity requirement. Within that
     * bound all transfer and reclamation paths are allocation-free and lock-free; an individual operation may starve.
     */
    template<typename T, MichaelScottQueueConfig Config = MichaelScottQueueConfig{}>
        requires(Detail::MichaelScottElement<T> && Detail::ValidateMichaelScottConfig(Config))
    class MichaelScottQueue {
        static constexpr std::size_t kHazardsPerOperation = 3;
        static constexpr std::size_t kHazardCount = Config.maxConcurrentOperations * kHazardsPerOperation;
        static constexpr std::size_t kNodeCount = Config.capacity + 1uz + kHazardCount;

        struct Node {
            std::atomic<Node*> next{nullptr};
            std::atomic<std::uint32_t> freeNext{0};
            std::atomic<bool> retired{false};
            std::atomic<std::uint64_t> retireEpoch{0};
            std::atomic<std::uint64_t> generation{0};
            std::atomic<std::uint8_t> payloadState{0};
            bool hasValue = false;
            alignas(T) std::byte value[sizeof(T)];

            [[nodiscard]] T* StorageAddress() noexcept { return reinterpret_cast<T*>(value); }
            [[nodiscard]] T* Value() noexcept { return std::launder(reinterpret_cast<T*>(value)); }
            [[nodiscard]] const T* Value() const noexcept { return std::launder(reinterpret_cast<const T*>(value)); }
        };

        struct alignas(Platform::kCacheLineSize) HazardRecord {
            std::atomic<void*> protectedAddresses[kHazardsPerOperation]{};
            std::atomic<std::uint64_t> entryEpoch{0};
            std::atomic<std::uint8_t> state{0};
        };
        static_assert(sizeof(HazardRecord) % Platform::kCacheLineSize == 0);

        using Store = std::conditional_t<Config.backing == MichaelScottBacking::Inline,
                                         Detail::InlineMichaelScottStore<Node, kNodeCount>,
                                         Detail::HeapMichaelScottStore<Node, kNodeCount>>;
        using Stats = std::conditional_t<Config.enableStats, Detail::MichaelScottStats, Detail::NoMichaelScottStats>;

        class HazardLease {
        public:
            explicit HazardLease(MichaelScottQueue& queue) noexcept : queue_(&queue), record_(queue.AcquireHazards()) {}
            HazardLease(const HazardLease&) = delete;
            HazardLease& operator=(const HazardLease&) = delete;
            ~HazardLease() { queue_->ReleaseHazards(*record_); }

            [[nodiscard]] Node* Protect(const std::atomic<Node*>& source, std::size_t slot) noexcept {
                Node* observed = source.load(std::memory_order_relaxed);
                for (;;) {
                    const std::uint64_t generation =
                        observed == nullptr ? 0 : observed->generation.load(std::memory_order_relaxed);
                    record_->protectedAddresses[slot].store(observed, std::memory_order_seq_cst);
                    Node* validated = source.load(std::memory_order_seq_cst);
                    const bool validGeneration =
                        observed == nullptr || observed->generation.load(std::memory_order_relaxed) == generation;
                    if (validated == observed && validGeneration) {
                        return observed;
                    }
                    observed = validated;
                }
            }

            void Reset(std::size_t slot) noexcept {
                record_->protectedAddresses[slot].store(nullptr, std::memory_order_release);
            }

        private:
            MichaelScottQueue* queue_;
            HazardRecord* record_;
        };

        static constexpr std::uint32_t kFreeSentinel = static_cast<std::uint32_t>(kNodeCount);
        static constexpr std::uint32_t kFreeIndexBits = std::bit_width(kFreeSentinel);
        static constexpr std::uint64_t kFreeIndexMask = (std::uint64_t{1} << kFreeIndexBits) - 1uz;
        static constexpr std::uint64_t kFreeTagStep = std::uint64_t{1} << kFreeIndexBits;

        static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
        static_assert(std::atomic<Node*>::is_always_lock_free);
        static_assert(kNodeCount <= std::numeric_limits<std::uint32_t>::max() - 1uz,
                      "MichaelScottQueue node arena must fit its 32-bit free-list index and sentinel");

        [[nodiscard]] static constexpr std::uint32_t FreeIndex(std::uint64_t head) noexcept {
            return static_cast<std::uint32_t>(head & kFreeIndexMask);
        }

        [[nodiscard]] static constexpr std::uint64_t PackFree(std::uint32_t index, std::uint64_t previous) noexcept {
            return ((previous + kFreeTagStep) & ~kFreeIndexMask) | index;
        }

        [[nodiscard]] Node* NodeAt(std::uint32_t index) noexcept { return store_.Data() + index; }
        [[nodiscard]] const Node* NodeAt(std::uint32_t index) const noexcept { return store_.Data() + index; }

        [[nodiscard]] std::uint32_t IndexOf(const Node* node) const noexcept {
            return static_cast<std::uint32_t>(node - store_.Data());
        }

        [[nodiscard]] Node* PopFree() noexcept {
            std::uint64_t observed = freeHead_.load(std::memory_order_acquire);
            for (;;) {
                const std::uint32_t index = FreeIndex(observed);
                if (index == kFreeSentinel) {
                    return nullptr;
                }
                Node* node = NodeAt(index);
                const std::uint32_t next = node->freeNext.load(std::memory_order_relaxed);
                if (freeHead_.compare_exchange_weak(observed, PackFree(next, observed), std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
                    return node;
                }
            }
        }

        void PushFree(Node& node) noexcept {
            const std::uint32_t index = IndexOf(std::addressof(node));
            std::uint64_t observed = freeHead_.load(std::memory_order_relaxed);
            do {
                node.freeNext.store(FreeIndex(observed), std::memory_order_relaxed);
            } while (!freeHead_.compare_exchange_weak(observed, PackFree(index, observed), std::memory_order_release,
                                                      std::memory_order_relaxed));
        }

        [[nodiscard]] HazardRecord* AcquireHazards() noexcept {
            for (;;) {
                for (HazardRecord& record : hazards_) {
                    std::uint8_t expected = 0;
                    if (record.state.compare_exchange_weak(expected, 1, std::memory_order_acq_rel,
                                                           std::memory_order_relaxed)) {
                        record.entryEpoch.store(epoch_.load(std::memory_order_acquire), std::memory_order_relaxed);
                        record.state.store(2, std::memory_order_release);
                        return std::addressof(record);
                    }
                }
                std::this_thread::yield();
            }
        }

        static void ReleaseHazards(HazardRecord& record) noexcept {
            for (std::size_t slot = 0; slot != kHazardsPerOperation; ++slot) {
                record.protectedAddresses[slot].store(nullptr, std::memory_order_release);
            }
            record.state.store(0, std::memory_order_release);
        }

        void Retire(Node& node) noexcept {
            const std::uint8_t payloadState = node.payloadState.load(std::memory_order_acquire);
            assert(payloadState == 0 || payloadState == 3);
            node.retireEpoch.store(epoch_.fetch_add(1, std::memory_order_acq_rel), std::memory_order_relaxed);
            node.retired.store(true, std::memory_order_release);
            const std::size_t retirements = retirementsSinceCollection_.fetch_add(1, std::memory_order_relaxed) + 1uz;
            if (retirements >= Config.reclamationThreshold) {
                retirementsSinceCollection_.store(0, std::memory_order_relaxed);
                static_cast<void>(Collect());
            }
        }

        [[nodiscard]] Node* AcquireNode() noexcept {
            if (Node* node = PopFree(); node != nullptr) {
                node->generation.fetch_add(1, std::memory_order_relaxed);
                node->payloadState.store(0, std::memory_order_relaxed);
                return node;
            }
            for (;;) {
                static_cast<void>(Collect());
                if (Node* node = PopFree(); node != nullptr) {
                    node->generation.fetch_add(1, std::memory_order_relaxed);
                    node->payloadState.store(0, std::memory_order_relaxed);
                    return node;
                }
                std::this_thread::yield();
            }
        }

        [[nodiscard]] bool ReserveItem() noexcept {
            std::size_t observed = size_.load(std::memory_order_relaxed);
            while (observed != Config.capacity) {
                if (size_.compare_exchange_weak(observed, observed + 1uz, std::memory_order_acquire,
                                                std::memory_order_relaxed)) {
                    return true;
                }
            }
            return false;
        }

    public:
        using value_type = T;

        static constexpr std::size_t kCapacity = Config.capacity;
        static constexpr std::size_t kMaxConcurrentOperations = Config.maxConcurrentOperations;
        static constexpr std::size_t kReclamationThreshold = Config.reclamationThreshold;
        static constexpr std::size_t kPhysicalNodeCount = kNodeCount;
        static constexpr Concurrency::QueueTopology topology = Concurrency::QueueTopologies::Mpmc;

        /** @brief Build an empty queue and initialize its free arena plus permanent dummy node. */
        MichaelScottQueue() {
            for (std::uint32_t index = 0; index != kNodeCount; ++index) {
                NodeAt(index)->freeNext.store(index + 1uz == kNodeCount ? kFreeSentinel : index + 1uz,
                                              std::memory_order_relaxed);
            }
            freeHead_.store(PackFree(0, 0), std::memory_order_relaxed);
            Node* dummy = PopFree();
            assert(dummy != nullptr);
            head_.store(dummy, std::memory_order_relaxed);
            tail_.store(dummy, std::memory_order_relaxed);
        }

        MichaelScottQueue(const MichaelScottQueue&) = delete;
        MichaelScottQueue& operator=(const MichaelScottQueue&) = delete;
        MichaelScottQueue(MichaelScottQueue&&) = delete;
        MichaelScottQueue& operator=(MichaelScottQueue&&) = delete;

        /** @brief Destroy all remaining values after every concurrent operation has stopped. */
        ~MichaelScottQueue() {
            for (const HazardRecord& record : hazards_) {
                assert(record.state.load(std::memory_order_relaxed) == 0);
            }
            Node* node = head_.load(std::memory_order_relaxed);
            while (node != nullptr) {
                Node* next = node->next.load(std::memory_order_relaxed);
                if (node->hasValue) {
                    std::destroy_at(node->Value());
                    node->hasValue = false;
                }
                node = next;
            }
        }

        /** @brief Attempt to append @p value without allocating or blocking for capacity. */
        [[nodiscard]] bool TryPush(T&& value) noexcept {
            if (!ReserveItem()) {
                return false;
            }

            Node* node = AcquireNode();
            std::construct_at(node->StorageAddress(), std::move(value));
            node->hasValue = true;
            node->payloadState.store(1, std::memory_order_release);
            node->next.store(nullptr, std::memory_order_relaxed);

            HazardLease hazards{*this};
            for (;;) {
                Node* tail = hazards.Protect(tail_, 0);
                Node* next = hazards.Protect(tail->next, 1);
                if (tail != tail_.load(std::memory_order_acquire)) {
                    continue;
                }
                if (next != nullptr) {
                    static_cast<void>(
                        tail_.compare_exchange_weak(tail, next, std::memory_order_acq_rel, std::memory_order_relaxed));
                    continue;
                }
                if (tail->next.compare_exchange_weak(next, node, std::memory_order_release,
                                                     std::memory_order_relaxed)) {
                    static_cast<void>(tail_.compare_exchange_strong(tail, node, std::memory_order_release,
                                                                    std::memory_order_relaxed));
                    if constexpr (Config.enableStats) {
                        stats_.pushes.fetch_add(1, std::memory_order_relaxed);
                    }
                    return true;
                }
            }
        }

        /** @brief Forward a nothrow-constructible value into @ref TryPush. */
        template<typename U>
            requires std::constructible_from<T, U&&> && std::is_nothrow_constructible_v<T, U&&>
        [[nodiscard]] bool TryPush(U&& value) noexcept {
            T converted(std::forward<U>(value));
            return TryPush(std::move(converted));
        }

        /** @brief Remove and return the oldest published value, or an empty optional when the queue is empty. */
        [[nodiscard]] std::optional<T> TryPop() noexcept {
            HazardLease hazards{*this};
            for (;;) {
                Node* head = hazards.Protect(head_, 0);
                const std::uint8_t payloadState = head->payloadState.load(std::memory_order_acquire);
                if (payloadState == 2) {
                    continue;
                }
                if (payloadState == 1) {
                    std::uint8_t expected = 1;
                    if (!head->payloadState.compare_exchange_weak(expected, 2, std::memory_order_acq_rel,
                                                                  std::memory_order_acquire)) {
                        continue;
                    }
                    std::optional<T> result{std::in_place, std::move(*head->Value())};
                    std::destroy_at(head->Value());
                    head->hasValue = false;
                    head->payloadState.store(3, std::memory_order_release);
                    size_.fetch_sub(1, std::memory_order_release);
                    if constexpr (Config.enableStats) {
                        stats_.pops.fetch_add(1, std::memory_order_relaxed);
                    }
                    return result;
                }

                Node* next = hazards.Protect(head->next, 1);
                if (head != head_.load(std::memory_order_acquire)) {
                    continue;
                }
                Node* tail = hazards.Protect(tail_, 2);
                if (tail != tail_.load(std::memory_order_acquire)) {
                    continue;
                }
                if (next == nullptr) {
                    return std::nullopt;
                }
                if (head == tail) {
                    static_cast<void>(
                        tail_.compare_exchange_weak(tail, next, std::memory_order_acq_rel, std::memory_order_relaxed));
                    continue;
                }
                if (head_.compare_exchange_weak(head, next, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                    hazards.Reset(1);
                    Retire(*head);
                }
            }
        }

        /** @brief Move one value into @p output when available. */
        [[nodiscard]] bool TryPop(T& output) noexcept(std::is_nothrow_move_assignable_v<T>)
            requires std::is_nothrow_move_assignable_v<T>
        {
            auto value = TryPop();
            if (!value.has_value()) {
                return false;
            }
            output = std::move(*value);
            return true;
        }

        /** @brief Reclaim every currently retired node absent from a sequentially-consistent hazard snapshot. */
        [[nodiscard]] std::size_t Collect() noexcept {
            if (collectionInProgress_.test_and_set(std::memory_order_acquire)) {
                return 0;
            }
            struct CollectionGuard {
                std::atomic_flag& flag;
                ~CollectionGuard() { flag.clear(std::memory_order_release); }
            } guard{collectionInProgress_};

            std::array<std::uint64_t, kNodeCount> retiredEpochs{};
            for (std::uint32_t index = 0; index != kNodeCount; ++index) {
                Node& node = *NodeAt(index);
                if (node.retired.load(std::memory_order_acquire)) {
                    retiredEpochs[index] = node.retireEpoch.load(std::memory_order_acquire);
                }
            }

            std::array<bool, kNodeCount> protectedNodes{};
            std::atomic_thread_fence(std::memory_order_seq_cst);
            std::uint64_t oldestEntryEpoch = std::numeric_limits<std::uint64_t>::max();
            for (const HazardRecord& record : hazards_) {
                if (record.state.load(std::memory_order_acquire) == 2) {
                    oldestEntryEpoch = std::min(oldestEntryEpoch, record.entryEpoch.load(std::memory_order_acquire));
                }
                for (const auto& address : record.protectedAddresses) {
                    if (void* protectedAddress = address.load(std::memory_order_seq_cst); protectedAddress != nullptr) {
                        protectedNodes[IndexOf(static_cast<const Node*>(protectedAddress))] = true;
                    }
                }
            }

            std::size_t reclaimed = 0;
            for (std::uint32_t index = 0; index != kNodeCount; ++index) {
                Node& node = *NodeAt(index);
                const std::uint64_t retireEpoch = retiredEpochs[index];
                if (retireEpoch == 0 || !node.retired.load(std::memory_order_acquire) ||
                    node.retireEpoch.load(std::memory_order_acquire) != retireEpoch) {
                    continue;
                }
                if (protectedNodes[index]) {
                    continue;
                }
                if (oldestEntryEpoch <= retireEpoch) {
                    continue;
                }
                const std::uint8_t payloadState = node.payloadState.load(std::memory_order_acquire);
                if (payloadState == 1 || payloadState == 2) {
                    continue;
                }
                bool expected = true;
                if (node.retired.compare_exchange_strong(expected, false, std::memory_order_acq_rel,
                                                         std::memory_order_relaxed)) {
                    PushFree(node);
                    ++reclaimed;
                }
            }
            if constexpr (Config.enableStats) {
                stats_.collections.fetch_add(1, std::memory_order_relaxed);
                stats_.reclaimedNodes.fetch_add(reclaimed, std::memory_order_relaxed);
            }
            return reclaimed;
        }

        [[nodiscard]] static constexpr std::size_t GetCapacity() noexcept { return kCapacity; }
        [[nodiscard]] std::size_t SizeApprox() const noexcept { return size_.load(std::memory_order_acquire); }
        [[nodiscard]] bool Empty() const noexcept { return SizeApprox() == 0; }

        [[nodiscard]] std::uint64_t PushCount() const noexcept
            requires(Config.enableStats)
        {
            return stats_.pushes.load(std::memory_order_relaxed);
        }

        [[nodiscard]] std::uint64_t PopCount() const noexcept
            requires(Config.enableStats)
        {
            return stats_.pops.load(std::memory_order_relaxed);
        }

        [[nodiscard]] std::uint64_t CollectionCount() const noexcept
            requires(Config.enableStats)
        {
            return stats_.collections.load(std::memory_order_relaxed);
        }

        [[nodiscard]] std::uint64_t ReclaimedNodeCount() const noexcept
            requires(Config.enableStats)
        {
            return stats_.reclaimedNodes.load(std::memory_order_relaxed);
        }

    private:
        [[= Concurrency::$::Contended{}]] alignas(Platform::kCacheLineSize) std::atomic<Node*> head_{nullptr};
        [[= Concurrency::$::Contended{}]] alignas(Platform::kCacheLineSize) std::atomic<Node*> tail_{nullptr};
        [[= Concurrency::$::Contended{}]] alignas(Platform::kCacheLineSize) std::atomic<std::size_t> size_{0};
        [[= Concurrency::$::Contended{}]] alignas(Platform::kCacheLineSize) std::atomic<std::uint64_t> freeHead_{0};
        [[= Concurrency::$::Contended{}]] std::atomic<std::size_t> retirementsSinceCollection_{0};
        std::atomic<std::uint64_t> epoch_{1};
        std::atomic_flag collectionInProgress_ = ATOMIC_FLAG_INIT;
        alignas(Platform::kCacheLineSize) std::array<HazardRecord, Config.maxConcurrentOperations> hazards_{};
        SORA_NO_UNIQUE_ADDRESS Stats stats_{};
        Store store_{};
    };

} // namespace Sora

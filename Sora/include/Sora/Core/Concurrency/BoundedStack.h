/**
 * @file BoundedStack.h
 * @brief Allocation-free bounded LIFO storage specialized by compile-time endpoint topology.
 * @ingroup Core
 */
#pragma once

#include "Sora/Platform.h"
#include "Sora/Core/Concurrency/FalseSharingChecker.h"
#include "Sora/Core/Concurrency/QueueTopology.h"
#include "Sora/Core/Traits/TypeTraits.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace Sora {

    namespace Detail {

        /** @brief Empty compile-time-erased field keyed by its physical role. */
        enum class BoundedStackField : std::uint8_t {
            LocalSize,
            ReadyHead,
            FreeHead,
            SpscFreeHead,
            SpscReadyHead,
            SpscReadyRead,
            SpscReadyWrite,
            SpscReadyNodes,
            SpscReturnRead,
            SpscReturnWrite,
            SpscReturnedNodes,
            MpscReadyHead,
            SpmcFreeHead,
        };

        template<BoundedStackField Field>
        using EmptyBoundedStackField = std::integral_constant<BoundedStackField, Field>;

        struct BoundedStackSpscProducerDomain : Concurrency::$::CohesiveSingleWriterContentionDomain {};
        struct BoundedStackSpscConsumerDomain : Concurrency::$::CohesiveSingleWriterContentionDomain {};

        /** @brief Select the narrowest node index that retains one dedicated empty sentinel. */
        template<std::size_t Capacity>
        consteval auto BoundedStackIndexType() {
            static_assert(Capacity > 0, "a bounded stack requires positive capacity");
            static_assert(Capacity < std::numeric_limits<std::uint16_t>::max(),
                          "bounded stack capacity must fit a tagged 16-bit node index");
            if constexpr (Capacity < std::numeric_limits<std::uint8_t>::max()) {
                return std::type_identity<std::uint8_t>{};
            } else {
                return std::type_identity<std::uint16_t>{};
            }
        }

        template<class T, std::size_t Capacity, Concurrency::QueueTopology Topology>
        concept ValidBoundedStack = Capacity > 0 && Capacity < std::numeric_limits<std::uint16_t>::max() &&
                                    Topology.IsValid() && std::is_object_v<T>&& std::is_nothrow_destructible_v<T> &&
                                    (Topology.sharing == Concurrency::QueueSharing::SingleThread ||
                                     Concurrency::Concept::ConcurrentQueueElement<T>);

        /**
         * @brief Fixed-capacity stack with a direct local protocol or an ABA-resistant indexed Treiber protocol.
         *
         * @details Multi-endpoint specializations use preallocated tagged index stacks. The Treiber variants are
         * lock-free under the explicit assumption that no operation remains outstanding across the complete finite
         * head-tag period. SPSC publishes node indices through one ring, folds each acquired snapshot into a
         * consumer-local stack, and returns consumed indices through a second ring. A stalled operation owns only
         * detached nodes, so no protocol creates a publication or reclamation hole.
         */
        template<class T, std::size_t Capacity, Concurrency::QueueTopology Topology>
            requires ValidBoundedStack<T, Capacity, Topology>
        class BoundedStackImplementation {
            static constexpr bool kLocal = Topology.sharing == Concurrency::QueueSharing::SingleThread;
            static constexpr bool kSpsc = Topology == Concurrency::QueueTopologies::Spsc;
            static constexpr bool kMpsc = Topology == Concurrency::QueueTopologies::Mpsc;
            static constexpr bool kSpmc = Topology == Concurrency::QueueTopologies::Spmc;
            using Index = typename decltype(BoundedStackIndexType<Capacity>())::type;
            static constexpr unsigned kIndexBits = std::numeric_limits<Index>::digits;
            static constexpr unsigned kCountBits = std::bit_width(Capacity);
            static constexpr unsigned kTagShift = kIndexBits + kCountBits;
            static constexpr std::uint64_t kIndexMask = (std::uint64_t{1} << kIndexBits) - 1;
            static constexpr std::uint64_t kCountMask = (std::uint64_t{1} << kCountBits) - 1;
            static constexpr std::uint64_t kCountIncrement = std::uint64_t{1} << kIndexBits;
            static constexpr std::uint64_t kTagIncrement = std::uint64_t{1} << kTagShift;
            static constexpr std::uint64_t kTagMask = ~(kTagIncrement - 1);
            static constexpr Index kEmptyIndex = std::numeric_limits<Index>::max();
            static constexpr std::size_t kHeadAlignment = kLocal ? 1 : Platform::kCacheLineSize;

            using LocalSize =
                std::conditional_t<kLocal, std::size_t, EmptyBoundedStackField<BoundedStackField::LocalSize>>;
            using Head = std::conditional_t<kLocal || kSpsc, EmptyBoundedStackField<BoundedStackField::ReadyHead>,
                                            std::atomic<std::uint64_t>>;
            using FreeHead =
                std::conditional_t<kLocal, EmptyBoundedStackField<BoundedStackField::FreeHead>,
                                   std::conditional_t<kSpsc, EmptyBoundedStackField<BoundedStackField::FreeHead>,
                                                      std::atomic<std::uint64_t>>>;
            using SpscFreeHead =
                std::conditional_t<kSpsc, Index, EmptyBoundedStackField<BoundedStackField::SpscFreeHead>>;
            using SpscReadyHead =
                std::conditional_t<kSpsc, Index, EmptyBoundedStackField<BoundedStackField::SpscReadyHead>>;
            using SpscReadyRead =
                std::conditional_t<kSpsc, std::uint64_t, EmptyBoundedStackField<BoundedStackField::SpscReadyRead>>;
            using SpscReadyWrite = std::conditional_t<kSpsc, std::atomic<std::uint64_t>,
                                                      EmptyBoundedStackField<BoundedStackField::SpscReadyWrite>>;
            using SpscReadyNodes = std::conditional_t<kSpsc, std::array<Index, Capacity>,
                                                      EmptyBoundedStackField<BoundedStackField::SpscReadyNodes>>;
            using SpscReturnRead =
                std::conditional_t<kSpsc, std::uint64_t, EmptyBoundedStackField<BoundedStackField::SpscReturnRead>>;
            using SpscReturnWrite = std::conditional_t<kSpsc, std::atomic<std::uint64_t>,
                                                       EmptyBoundedStackField<BoundedStackField::SpscReturnWrite>>;
            using SpscReturnedNodes = std::conditional_t<kSpsc, std::array<Index, Capacity>,
                                                         EmptyBoundedStackField<BoundedStackField::SpscReturnedNodes>>;
            using MpscReadyHead =
                std::conditional_t<kMpsc, Index, EmptyBoundedStackField<BoundedStackField::MpscReadyHead>>;
            using SpmcFreeHead =
                std::conditional_t<kSpmc, Index, EmptyBoundedStackField<BoundedStackField::SpmcFreeHead>>;
            using LocalDomain = std::conditional_t<kLocal, Concurrency::$::SharedStorage, Concurrency::$::Inactive>;
            using SharedHeadDomain =
                std::conditional_t<!kLocal && !kSpsc, Concurrency::$::Contended, Concurrency::$::Inactive>;
            using SpscProducerDomain =
                std::conditional_t<kSpsc, BoundedStackSpscProducerDomain, Concurrency::$::Inactive>;
            using SpscConsumerDomain =
                std::conditional_t<kSpsc, BoundedStackSpscConsumerDomain, Concurrency::$::Inactive>;
            using SpscStorageDomain =
                std::conditional_t<kSpsc, Concurrency::$::SharedStorage, Concurrency::$::Inactive>;
            using MpscConsumerDomain =
                std::conditional_t<kMpsc, Concurrency::$::ConsumerOwned, Concurrency::$::Inactive>;
            using SpmcProducerDomain =
                std::conditional_t<kSpmc, Concurrency::$::ProducerOwned, Concurrency::$::Inactive>;

            struct NodeFields {
                std::atomic<Index> next{};
                alignas(T) std::byte storage[sizeof(T)];

                NodeFields() noexcept {}

                [[nodiscard]] void* Storage() noexcept { return storage; }
                [[nodiscard]] T* Value() noexcept { return std::launder(reinterpret_cast<T*>(storage)); }
            };

            struct alignas(Platform::kCacheLineSize) IsolatedNode : NodeFields {};

            static constexpr bool kIsolateNodes = !kLocal && sizeof(NodeFields) <= Platform::kCacheLineSize / 2;
            using Node = std::conditional_t<kIsolateNodes, IsolatedNode, NodeFields>;
            static constexpr std::size_t kNodePoolAlignment = kLocal ? alignof(Node) : Platform::kCacheLineSize;
            static constexpr std::size_t kSpscAlignment = kSpsc ? Platform::kCacheLineSize : 1;
            static constexpr std::size_t kMpscAlignment = kMpsc ? Platform::kCacheLineSize : 1;
            static constexpr std::size_t kSpmcAlignment = kSpmc ? Platform::kCacheLineSize : 1;

            struct IndexChain {
                Index first{kEmptyIndex};
                Index last{kEmptyIndex};
                std::size_t count{};

                [[nodiscard]] bool Empty() const noexcept { return count == 0; }
            };

            static_assert(kLocal || kSpsc || std::atomic<std::uint64_t>::is_always_lock_free,
                          "multi-endpoint BoundedStack requires lock-free 64-bit atomics");
            static_assert(kLocal || std::atomic<Index>::is_always_lock_free,
                          "concurrent BoundedStack requires lock-free compact indices");
            static_assert(kTagShift < std::numeric_limits<std::uint64_t>::digits,
                          "bounded stack head must retain version bits for ABA resistance");
            static_assert(kLocal || kSpsc || std::numeric_limits<std::uint64_t>::digits - kTagShift >= 32,
                          "a shared bounded-stack head must retain an ABA tag window of at least 32 bits");

        public:
            using value_type = T;
            static constexpr std::size_t kCapacity = Capacity;
            static constexpr Concurrency::QueueTopology topology = Topology;
            static constexpr Concurrency::QueueOrder order = Concurrency::QueueOrder::Lifo;
            static constexpr bool transactionalPushBatch = true;
            static constexpr unsigned kHeadTagBits = kLocal || kSpsc ? 0 : 64 - kTagShift; ///< Shared-head tag width.
            static constexpr std::uint64_t kHeadTagPeriod =
                kLocal || kSpsc ? 0 : std::uint64_t{1} << kHeadTagBits; ///< Shared-head identity period.

            BoundedStackImplementation() noexcept {
                if constexpr (!kLocal) {
                    for (std::size_t index = 0; index != Capacity; ++index) {
                        nodes_[index].next.store(index + 1 == Capacity ? kEmptyIndex : static_cast<Index>(index + 1),
                                                 std::memory_order_relaxed);
                    }
                    if constexpr (kSpsc) {
                        spscFreeHead_ = Index{0};
                        spscReadyHead_ = kEmptyIndex;
                        spscReadyRead_ = 0;
                        spscReadyWrite_.store(0, std::memory_order_relaxed);
                        spscReturnRead_ = 0;
                        spscReturnWrite_.store(0, std::memory_order_relaxed);
                    } else {
                        readyHead_.store(Pack(kEmptyIndex, 0, 0), std::memory_order_relaxed);
                        freeHead_.store(Pack(Index{0}, Capacity, 0), std::memory_order_relaxed);
                        if constexpr (kMpsc) {
                            mpscReadyHead_ = kEmptyIndex;
                        }
                        if constexpr (kSpmc) {
                            spmcFreeHead_ = kEmptyIndex;
                        }
                    }
                }
            }

            BoundedStackImplementation(const BoundedStackImplementation&) = delete;
            BoundedStackImplementation& operator=(const BoundedStackImplementation&) = delete;
            BoundedStackImplementation(BoundedStackImplementation&&) = delete;
            BoundedStackImplementation& operator=(BoundedStackImplementation&&) = delete;

            ~BoundedStackImplementation() {
                if constexpr (kLocal) {
                    while (localSize_ != 0) {
                        std::destroy_at(nodes_[--localSize_].Value());
                    }
                } else {
                    for (;;) {
                        const Index node = TryAcquireReadyNode();
                        if (node == kEmptyIndex) {
                            break;
                        }
                        std::destroy_at(NodeAt(node).Value());
                    }
                }
            }

            template<class... Args>
                requires std::constructible_from<T, Args&&...>
            [[nodiscard]] bool TryEmplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>) {
                if constexpr (kLocal) {
                    if (localSize_ == Capacity) {
                        return false;
                    }
                    ::new (nodes_[localSize_].Storage()) T(std::forward<Args>(args)...);
                    ++localSize_;
                    return true;
                } else {
                    const Index node = TryAcquireFreeNode();
                    if (node == kEmptyIndex) {
                        return false;
                    }
                    try {
                        ::new (NodeAt(node).Storage()) T(std::forward<Args>(args)...);
                    } catch (...) {
                        ReleaseUnpublishedNode(node);
                        throw;
                    }
                    if constexpr (kSpsc) {
                        PublishSpscReadyNode(node);
                    } else {
                        PushNode(readyHead_, node);
                    }
                    return true;
                }
            }

            template<class U>
                requires std::constructible_from<T, U&&>
            [[nodiscard]] bool TryPush(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
                return TryEmplace(std::forward<U>(value));
            }

            /**
             * @brief Publish a sized random-access batch atomically with sequential-push LIFO semantics.
             * @details On success, element @c size-1 becomes the next value popped. Backpressure leaves every input
             * intact; an element move that throws provides the normal moved-from source guarantee.
             */
            template<class Batch>
                requires requires(Batch& batch, std::size_t index) {
                    { batch.size() } noexcept -> std::convertible_to<std::size_t>;
                    { batch[index] } -> std::same_as<T&>;
                }
            [[nodiscard]] bool TryPushBatch(Batch& batch) noexcept(std::is_nothrow_move_constructible_v<T>) {
                const std::size_t count = batch.size();
                if (count == 0) {
                    return true;
                }
                if constexpr (kLocal) {
                    if (Capacity - localSize_ < count) {
                        return false;
                    }
                    std::size_t constructed = 0;
                    try {
                        for (; constructed != count; ++constructed) {
                            ::new (nodes_[localSize_ + constructed].Storage()) T(std::move(batch[constructed]));
                        }
                    } catch (...) {
                        while (constructed != 0) {
                            std::destroy_at(nodes_[localSize_ + --constructed].Value());
                        }
                        throw;
                    }
                    localSize_ += count;
                    return true;
                } else {
                    IndexChain chain = TryAcquireFreeChain(count);
                    if (chain.Empty()) {
                        return false;
                    }

                    Index node = chain.first;
                    std::size_t constructed = 0;
                    try {
                        for (; constructed != count; ++constructed) {
                            ::new (NodeAt(node).Storage()) T(std::move(batch[constructed]));
                            node = NodeAt(node).next.load(std::memory_order_relaxed);
                        }
                    } catch (...) {
                        DestroyPrefix(chain.first, constructed);
                        ReleaseUnpublishedChain(chain);
                        throw;
                    }

                    if constexpr (kSpsc) {
                        PublishSpscReadyChain(chain);
                    } else {
                        chain = Reverse(chain);
                        PushChain(readyHead_, chain);
                    }
                    return true;
                }
            }

            [[nodiscard]] std::optional<T> TryPop() noexcept(std::is_nothrow_move_constructible_v<T>)
                requires std::move_constructible<T>
            {
                if constexpr (kLocal) {
                    if (localSize_ == 0) {
                        return std::nullopt;
                    }
                    T* value = nodes_[localSize_ - 1].Value();
                    std::optional<T> result{std::move(*value)};
                    --localSize_;
                    std::destroy_at(value);
                    return result;
                } else {
                    const Index node = TryAcquireReadyNode();
                    if (node == kEmptyIndex) {
                        return std::nullopt;
                    }
                    return Extract(node);
                }
            }

            [[nodiscard]] bool TryPop(T& output) noexcept(std::is_nothrow_move_assignable_v<T>)
                requires std::is_move_assignable_v<T>
            {
                if constexpr (kLocal) {
                    if (localSize_ == 0) {
                        return false;
                    }
                    T* value = nodes_[localSize_ - 1].Value();
                    output = std::move(*value);
                    --localSize_;
                    std::destroy_at(value);
                    return true;
                } else {
                    const Index node = TryAcquireReadyNode();
                    if (node == kEmptyIndex) {
                        return false;
                    }
                    NodeRelease release{*this, node};
                    output = std::move(*NodeAt(node).Value());
                    return true;
                }
            }

            template<class Sink>
                requires std::invocable<Sink&, T&&>
            std::size_t Drain(Sink&& sink) noexcept(std::is_nothrow_invocable_v<Sink&, T&&>) {
                std::size_t drained = 0;
                while (auto value = TryPop()) {
                    std::forward<Sink>(sink)(std::move(*value));
                    ++drained;
                }
                return drained;
            }

            /** @brief Return one exact local size or a transient atomic size snapshot. */
            [[nodiscard]] std::size_t SizeApprox() const noexcept {
                if constexpr (kLocal) {
                    return localSize_;
                } else if constexpr (kSpsc) {
                    const std::uint64_t pushed = spscReadyWrite_.load(std::memory_order_acquire);
                    const std::uint64_t popped = spscReturnWrite_.load(std::memory_order_acquire);
                    return std::min<std::uint64_t>(pushed - popped, Capacity);
                } else if constexpr (kMpsc) {
                    return Capacity - HeadCount(freeHead_.load(std::memory_order_acquire));
                } else {
                    return HeadCount(readyHead_.load(std::memory_order_acquire));
                }
            }

            [[nodiscard]] bool Empty() const noexcept { return SizeApprox() == 0; }
            [[nodiscard]] static consteval std::size_t GetCapacity() noexcept { return Capacity; }

        private:
            struct NodeRelease {
                BoundedStackImplementation& stack;
                Index node;

                ~NodeRelease() {
                    std::destroy_at(stack.NodeAt(node).Value());
                    stack.RecycleConsumedNode(node);
                }
            };

            [[nodiscard]] static constexpr std::uint64_t Pack(Index index, std::size_t count,
                                                              std::uint64_t tag) noexcept {
                return (tag & kTagMask) | ((count & kCountMask) << kIndexBits) | static_cast<std::uint64_t>(index);
            }

            [[nodiscard]] static constexpr Index HeadIndex(std::uint64_t head) noexcept {
                return static_cast<Index>(head & kIndexMask);
            }

            [[nodiscard]] static constexpr std::size_t HeadCount(std::uint64_t head) noexcept {
                return static_cast<std::size_t>((head >> kIndexBits) & kCountMask);
            }

            [[nodiscard]] static constexpr std::uint64_t HeadAfterPop(std::uint64_t head, Index index,
                                                                      std::size_t count) noexcept {
                return ((head + kTagIncrement - count * kCountIncrement) & ~kIndexMask) |
                       static_cast<std::uint64_t>(index);
            }

            [[nodiscard]] static constexpr std::uint64_t HeadAfterPush(std::uint64_t head, Index index,
                                                                       std::size_t count) noexcept {
                return ((head + kTagIncrement + count * kCountIncrement) & ~kIndexMask) |
                       static_cast<std::uint64_t>(index);
            }

            [[nodiscard]] Node& NodeAt(Index index) noexcept { return nodes_.data()[static_cast<std::size_t>(index)]; }

            void PublishSpscReadyNode(Index node) noexcept
                requires(kSpsc)
            {
                // Owning a detached node proves that the unread ready ring has at least one free slot.
                const std::uint64_t write = spscReadyWrite_.load(std::memory_order_relaxed);
                spscReadyNodes_[write % Capacity] = node;
                spscReadyWrite_.store(write + 1, std::memory_order_release);
            }

            void PublishSpscReadyChain(const IndexChain& chain) noexcept
                requires(kSpsc)
            {
                const std::uint64_t write = spscReadyWrite_.load(std::memory_order_relaxed);
                Index node = chain.first;
                for (std::size_t offset = 0; offset != chain.count; ++offset) {
                    spscReadyNodes_[(write + offset) % Capacity] = node;
                    node = NodeAt(node).next.load(std::memory_order_relaxed);
                }
                spscReadyWrite_.store(write + chain.count, std::memory_order_release);
            }

            [[nodiscard]] Index TryAcquireSpscReadyNode() noexcept
                requires(kSpsc)
            {
                const std::uint64_t write = spscReadyWrite_.load(std::memory_order_acquire);
                while (spscReadyRead_ != write) {
                    const Index node = spscReadyNodes_[spscReadyRead_++ % Capacity];
                    NodeAt(node).next.store(spscReadyHead_, std::memory_order_relaxed);
                    spscReadyHead_ = node;
                }
                if (spscReadyHead_ == kEmptyIndex) {
                    return kEmptyIndex;
                }
                const Index node = spscReadyHead_;
                spscReadyHead_ = NodeAt(node).next.load(std::memory_order_relaxed);
                return node;
            }

            [[nodiscard]] Index TryAcquireMpscReadyNode() noexcept
                requires(kMpsc)
            {
                IndexChain published = TryDetachAll<true>(readyHead_);
                if (!published.Empty()) {
                    NodeAt(published.last).next.store(mpscReadyHead_, std::memory_order_relaxed);
                    mpscReadyHead_ = published.first;
                }
                if (mpscReadyHead_ == kEmptyIndex) {
                    return kEmptyIndex;
                }
                const Index node = mpscReadyHead_;
                mpscReadyHead_ = NodeAt(node).next.load(std::memory_order_relaxed);
                return node;
            }

            [[nodiscard]] Index TryAcquireReadyNode() noexcept
                requires(!kLocal)
            {
                if constexpr (kSpsc) {
                    return TryAcquireSpscReadyNode();
                } else if constexpr (kMpsc) {
                    return TryAcquireMpscReadyNode();
                } else {
                    return TryPopNode(readyHead_);
                }
            }

            [[nodiscard]] Index TryAcquireFreeNode() noexcept
                requires(!kLocal)
            {
                if constexpr (!kSpsc && !kSpmc) {
                    return TryPopNode(freeHead_);
                } else if constexpr (kSpsc) {
                    if (spscFreeHead_ != kEmptyIndex) {
                        const Index node = spscFreeHead_;
                        spscFreeHead_ = NodeAt(node).next.load(std::memory_order_relaxed);
                        return node;
                    }
                    const std::uint64_t write = spscReturnWrite_.load(std::memory_order_acquire);
                    if (spscReturnRead_ == write) {
                        return kEmptyIndex;
                    }

                    Index first = kEmptyIndex;
                    do {
                        const Index node = spscReturnedNodes_[spscReturnRead_++ % Capacity];
                        NodeAt(node).next.store(first, std::memory_order_relaxed);
                        first = node;
                    } while (spscReturnRead_ != write);

                    const Index node = first;
                    spscFreeHead_ = NodeAt(node).next.load(std::memory_order_relaxed);
                    return node;
                } else {
                    if (spmcFreeHead_ == kEmptyIndex) {
                        const IndexChain returned = TryDetachAll<false>(freeHead_);
                        if (returned.Empty()) {
                            return kEmptyIndex;
                        }
                        spmcFreeHead_ = returned.first;
                    }
                    const Index node = spmcFreeHead_;
                    spmcFreeHead_ = NodeAt(node).next.load(std::memory_order_relaxed);
                    return node;
                }
            }

            [[nodiscard]] IndexChain TryAcquireFreeChain(std::size_t count) noexcept
                requires(!kLocal)
            {
                if constexpr (!kSpsc && !kSpmc) {
                    return TryPopChain(freeHead_, count);
                } else {
                    IndexChain chain;
                    for (std::size_t index = 0; index != count; ++index) {
                        const Index node = TryAcquireFreeNode();
                        if (node == kEmptyIndex) {
                            ReleaseUnpublishedChain(chain);
                            return {};
                        }
                        NodeAt(node).next.store(kEmptyIndex, std::memory_order_relaxed);
                        if (chain.Empty()) {
                            chain.first = node;
                        } else {
                            NodeAt(chain.last).next.store(node, std::memory_order_relaxed);
                        }
                        chain.last = node;
                        ++chain.count;
                    }
                    return chain;
                }
            }

            void ReleaseUnpublishedNode(Index node) noexcept
                requires(!kLocal)
            {
                if constexpr (kSpsc) {
                    NodeAt(node).next.store(spscFreeHead_, std::memory_order_relaxed);
                    spscFreeHead_ = node;
                } else if constexpr (kSpmc) {
                    NodeAt(node).next.store(spmcFreeHead_, std::memory_order_relaxed);
                    spmcFreeHead_ = node;
                } else {
                    PushNode(freeHead_, node);
                }
            }

            void ReleaseUnpublishedChain(const IndexChain& chain) noexcept
                requires(!kLocal)
            {
                if (chain.Empty()) {
                    return;
                }
                if constexpr (kSpsc) {
                    NodeAt(chain.last).next.store(spscFreeHead_, std::memory_order_relaxed);
                    spscFreeHead_ = chain.first;
                } else if constexpr (kSpmc) {
                    NodeAt(chain.last).next.store(spmcFreeHead_, std::memory_order_relaxed);
                    spmcFreeHead_ = chain.first;
                } else {
                    PushChain(freeHead_, chain);
                }
            }

            void RecycleConsumedNode(Index node) noexcept
                requires(!kLocal)
            {
                if constexpr (!kSpsc) {
                    PushNode(freeHead_, node);
                } else {
                    const std::uint64_t write = spscReturnWrite_.load(std::memory_order_relaxed);
                    // Owning a consumed node proves that the unread return ring has at least one free slot.
                    spscReturnedNodes_[write % Capacity] = node;
                    spscReturnWrite_.store(write + 1, std::memory_order_release);
                }
            }

            [[nodiscard]] Index TryPopNode(std::atomic<std::uint64_t>& head) noexcept
                requires(!kLocal)
            {
                std::uint64_t observed = head.load(std::memory_order_relaxed);
                for (;;) {
                    const Index node = HeadIndex(observed);
                    if (node == kEmptyIndex) {
                        return kEmptyIndex;
                    }
                    const Index remainder = NodeAt(node).next.load(std::memory_order_relaxed);
                    const std::uint64_t desired = HeadAfterPop(observed, remainder, 1);
                    if (head.compare_exchange_weak(observed, desired, std::memory_order_acquire,
                                                   std::memory_order_relaxed)) {
                        return node;
                    }
                }
            }

            void PushNode(std::atomic<std::uint64_t>& head, Index node) noexcept
                requires(!kLocal)
            {
                std::uint64_t observed = head.load(std::memory_order_relaxed);
                for (;;) {
                    NodeAt(node).next.store(HeadIndex(observed), std::memory_order_relaxed);
                    const std::uint64_t desired = HeadAfterPush(observed, node, 1);
                    if (head.compare_exchange_weak(observed, desired, std::memory_order_release,
                                                   std::memory_order_relaxed)) {
                        return;
                    }
                }
            }

            [[nodiscard]] IndexChain TryPopChain(std::atomic<std::uint64_t>& head, std::size_t count) noexcept
                requires(!kLocal)
            {
                std::uint64_t observed = head.load(std::memory_order_relaxed);
                for (;;) {
                    if (HeadCount(observed) < count) {
                        return {};
                    }
                    const Index first = HeadIndex(observed);
                    Index last = first;
                    bool chainAvailable = true;
                    for (std::size_t index = 1; index != count; ++index) {
                        last = NodeAt(last).next.load(std::memory_order_relaxed);
                        if (last == kEmptyIndex) {
                            chainAvailable = false;
                            break;
                        }
                    }
                    if (!chainAvailable) {
                        observed = head.load(std::memory_order_relaxed);
                        continue;
                    }
                    const Index remainder = NodeAt(last).next.load(std::memory_order_relaxed);
                    const std::uint64_t desired = HeadAfterPop(observed, remainder, count);
                    if (head.compare_exchange_weak(observed, desired, std::memory_order_acquire,
                                                   std::memory_order_relaxed)) {
                        NodeAt(last).next.store(kEmptyIndex, std::memory_order_relaxed);
                        return {.first = first, .last = last, .count = count};
                    }
                }
            }

            template<bool FindLast>
            [[nodiscard]] IndexChain TryDetachAll(std::atomic<std::uint64_t>& head) noexcept
                requires(!kLocal && !kSpsc)
            {
                std::uint64_t observed = head.load(std::memory_order_relaxed);
                for (;;) {
                    const std::size_t count = HeadCount(observed);
                    if (count == 0) {
                        return {};
                    }
                    const Index first = HeadIndex(observed);
                    const std::uint64_t desired = HeadAfterPop(observed, kEmptyIndex, count);
                    if (!head.compare_exchange_weak(observed, desired, std::memory_order_acquire,
                                                    std::memory_order_relaxed)) {
                        continue;
                    }

                    Index last = kEmptyIndex;
                    if constexpr (FindLast) {
                        last = first;
                        for (std::size_t index = 1; index != count; ++index) {
                            last = NodeAt(last).next.load(std::memory_order_relaxed);
                        }
                    }
                    return {.first = first, .last = last, .count = count};
                }
            }

            void PushChain(std::atomic<std::uint64_t>& head, const IndexChain& chain) noexcept
                requires(!kLocal)
            {
                std::uint64_t observed = head.load(std::memory_order_relaxed);
                for (;;) {
                    NodeAt(chain.last).next.store(HeadIndex(observed), std::memory_order_relaxed);
                    const std::uint64_t desired = HeadAfterPush(observed, chain.first, chain.count);
                    if (head.compare_exchange_weak(observed, desired, std::memory_order_release,
                                                   std::memory_order_relaxed)) {
                        return;
                    }
                }
            }

            [[nodiscard]] IndexChain Reverse(IndexChain chain) noexcept {
                Index previous = kEmptyIndex;
                Index current = chain.first;
                while (current != kEmptyIndex) {
                    const Index next = NodeAt(current).next.load(std::memory_order_relaxed);
                    NodeAt(current).next.store(previous, std::memory_order_relaxed);
                    previous = current;
                    current = next;
                }
                std::swap(chain.first, chain.last);
                return chain;
            }

            void DestroyPrefix(Index first, std::size_t count) noexcept {
                Index current = first;
                for (std::size_t index = 0; index != count; ++index) {
                    std::destroy_at(NodeAt(current).Value());
                    current = NodeAt(current).next.load(std::memory_order_relaxed);
                }
            }

            [[nodiscard]] std::optional<T> Extract(Index node) noexcept(std::is_nothrow_move_constructible_v<T>) {
                NodeRelease release{*this, node};
                return std::optional<T>{std::move(*NodeAt(node).Value())};
            }

            [[= LocalDomain{}]] SORA_NO_UNIQUE_ADDRESS LocalSize localSize_{};
            [[= SharedHeadDomain{}]] SORA_NO_UNIQUE_ADDRESS alignas(kHeadAlignment) Head readyHead_{};
            [[= SharedHeadDomain{}]] SORA_NO_UNIQUE_ADDRESS alignas(kHeadAlignment) FreeHead freeHead_{};
            [[= SpscProducerDomain{}]] SORA_NO_UNIQUE_ADDRESS alignas(kSpscAlignment) SpscFreeHead spscFreeHead_{};
            [[= SpscProducerDomain{}]] SORA_NO_UNIQUE_ADDRESS SpscReturnRead spscReturnRead_{};
            [[= SpscConsumerDomain{}]] SORA_NO_UNIQUE_ADDRESS alignas(kSpscAlignment) SpscReadyHead spscReadyHead_{};
            [[= SpscConsumerDomain{}]] SORA_NO_UNIQUE_ADDRESS SpscReadyRead spscReadyRead_{};
            [[= SpscProducerDomain{}]] SORA_NO_UNIQUE_ADDRESS alignas(kSpscAlignment) SpscReadyWrite spscReadyWrite_{};
            [[= SpscStorageDomain{}]] SORA_NO_UNIQUE_ADDRESS alignas(kSpscAlignment) SpscReadyNodes spscReadyNodes_{};
            [[= SpscConsumerDomain{}]] SORA_NO_UNIQUE_ADDRESS alignas(kSpscAlignment)
                SpscReturnWrite spscReturnWrite_{};
            [[= SpscStorageDomain{}]] SORA_NO_UNIQUE_ADDRESS alignas(kSpscAlignment)
                SpscReturnedNodes spscReturnedNodes_{};
            [[= MpscConsumerDomain{}]] SORA_NO_UNIQUE_ADDRESS alignas(kMpscAlignment) MpscReadyHead mpscReadyHead_{};
            [[= SpmcProducerDomain{}]] SORA_NO_UNIQUE_ADDRESS alignas(kSpmcAlignment) SpmcFreeHead spmcFreeHead_{};
            [[= Concurrency::$::SharedStorage{}]] alignas(kNodePoolAlignment) std::array<Node, Capacity> nodes_;
        };

        template<class Storage>
        concept BoundedStackStorage = requires {
            typename Storage::value_type;
            { Storage::kCapacity } -> std::convertible_to<std::size_t>;
            { Storage::topology } -> std::same_as<const Concurrency::QueueTopology&>;
            { Storage::order } -> std::same_as<const Concurrency::QueueOrder&>;
            requires Storage::order == Concurrency::QueueOrder::Lifo;
            { Storage::transactionalPushBatch } -> std::same_as<const bool&>;
        };

    } // namespace Detail

    /**
     * @brief Bounded LIFO storage whose synchronization protocol is selected entirely at compile time.
     * @tparam T Stored payload type.
     * @tparam Capacity Number of preallocated value slots, limited to 65,534.
     * @tparam Topology Producer/consumer cardinality and sharing domain.
     */
    template<class T, std::size_t Capacity, Concurrency::QueueTopology Topology>
    using BoundedStack = Detail::BoundedStackImplementation<T, Capacity, Topology>;

} // namespace Sora

/**
 * @file RingBuffer.h
 * @brief Compile-time-specialized bounded ring buffers for local, SPSC, MPSC, SPMC, and MPMC topologies.
 * @ingroup Core
 */

#pragma once

#include "Sora/Core/Concurrency/ContentionDomain.h"
#include "Sora/Core/Concurrency/QueueTopology.h"
#include "Sora/Core/Concurrency/FalseSharingChecker.h"
#include "Sora/Core/Traits/TypeTraits.h"

#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <meta>
#include <memory>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

namespace Sora {

    namespace Concurrency {

        enum class RingBufferProtocol : uint8_t {
            Local,
            PublishedCursor,
            SequenceCell,
        };

        namespace Meta {

            /** @brief Match a queue topology to the protocol that determines generated code and physical layout. */
            [[nodiscard]] consteval RingBufferProtocol GetRingBufferProtocol(QueueTopology topology) {
                if (topology == QueueTopologies::Local) {
                    return RingBufferProtocol::Local;
                } else if (topology == QueueTopologies::Spsc) {
                    return RingBufferProtocol::PublishedCursor;
                } else {
                    /**
                     * @note We have to maintain status in every cell in order not to encounter
                     * applied-but-not-constructed objects when there are multiple producers or multiple consumers.
                     */
                    return RingBufferProtocol::SequenceCell;
                }
            }

        } // namespace Meta

        namespace Concept {

            template<size_t Capacity>
            concept ValidRingBufferCapacity =
                Capacity >= 2 && std::has_single_bit(Capacity) &&
                Capacity <= std::bit_floor(std::numeric_limits<size_t>::max()) && Capacity <= (size_t{1} << 31);

            template<QueueTopology Topo>
            concept ValidRingBufferTopology =
                Topo.sharing == QueueSharing::Concurrent ||
                (Topo.producers == QueueCardinality::Single && Topo.consumers == QueueCardinality::Single);

            template<typename T, QueueTopology Topo>
            concept RingBufferElement = std::is_object_v<T> && std::is_nothrow_destructible_v<T> && [] consteval {
                if constexpr (Meta::GetRingBufferProtocol(Topo) == RingBufferProtocol::PublishedCursor) {
                    return std::move_constructible<T> && std::is_nothrow_move_constructible_v<T> &&
                           std::is_nothrow_destructible_v<T>;
                } else {
                    return true;
                }
            }();

        } // namespace Concept

        /**
         * @brief Storage for one sequence-cell ring position, optionally aligned to a cache line.
         * @details The sequence number is used to determine whether the cell is ready for reading or
         * @{ -------------------------------------------------------------------------------------------------
         */

        /** @brief Raw storage for one sequence-cell ring position. */
        template<class T, bool CacheLineAligned>
        struct SequenceCell;

        /** @brief Raw storage for one sequence-cell ring position without cache line alignment. */
        template<class T>
        struct SequenceCell<T, false> {
            std::atomic<size_t> sequence{0};
            alignas(T) std::byte storage[sizeof(T)]{};

            [[nodiscard]] void* Storage() noexcept { return storage; }
            [[nodiscard]] T* Value() noexcept { return std::launder(reinterpret_cast<T*>(storage)); }
        };

        /** @brief Raw storage for one sequence-cell ring position with cache line alignment. */
        template<class T>
        struct alignas(Platform::kCacheLineSize) SequenceCell<T, true> {
            std::atomic<size_t> sequence{0};
            alignas(T) std::byte storage[sizeof(T)]{};

            [[nodiscard]] void* Storage() noexcept { return storage; }
            [[nodiscard]] T* Value() noexcept { return std::launder(reinterpret_cast<T*>(storage)); }
        };

        /** @brief Raw storage for a contiguous block of bytes, aligned to the element type. */
        template<typename T, size_t Capacity>
        struct BytesArena {
            alignas(T) std::byte slots[Capacity * sizeof(T)];
        };

        /** @brief Cells for a sequence-cell ring buffer. */
        template<typename T, size_t Capacity, bool CacheLineAligned>
        struct CellsArena {
            SequenceCell<T, CacheLineAligned> cells[Capacity]{};
        };

        /** @} -------------------------------------------------------------------------------------------------　*/

        namespace Detail {

            /**
             * @brief One bounded ring-buffer class whose protocol, state, and synchronization are specialized at
             * compile time.
             * @details Local, published-cursor SPSC, and sequence-cell protocols remain distinct mathematical state
             * machines. `if constexpr` erases unselected algorithms, while conditional field types and the MS-ABI
             * no-unique-address attribute erase unselected state. No protocol value or branch survives at runtime.
             */
            template<class T, size_t Capacity, QueueTopology Topo,
                     bool CacheLineAligned =
                         Topo.producers == QueueCardinality::Multiple || Topo.consumers == QueueCardinality::Multiple>
                requires Concept::ValidRingBufferCapacity<Capacity> && Concept::ValidRingBufferTopology<Topo> &&
                         Concept::RingBufferElement<T, Topo>
            class RingBufferImpl {
                // clang-format off
                static constexpr RingBufferProtocol kProtocol              = Meta::GetRingBufferProtocol(Topo);
                static constexpr size_t             kIndexMask             = Capacity - 1;
                static constexpr size_t             kSlotCount             = Capacity + 1;
                static constexpr size_t             kPadding               = (Platform::kCacheLineSize - 1) / sizeof(T) + 1;
                static constexpr size_t             kPositionAlignment     = kProtocol == RingBufferProtocol::Local ? alignof(size_t) : Platform::kCacheLineSize;
                static constexpr size_t             kCacheAlignment        = kProtocol == RingBufferProtocol::PublishedCursor ? Platform::kCacheLineSize : 1;
                static constexpr size_t             kArenaAlignment        = [] consteval {
                                                                                 if constexpr (kProtocol == RingBufferProtocol::SequenceCell) {
                                                                                     return Platform::kCacheLineSize;
                                                                                 } else if constexpr (kProtocol == RingBufferProtocol::Local) {
                                                                                     return alignof(T);
                                                                                 } else {
                                                                                     return 1;
                                                                                 }
                                                                             }();

                using PublishedSlotCount  = std::conditional_t<kProtocol == RingBufferProtocol::PublishedCursor, const size_t, void>;
                using AllocatedSlots      = std::conditional_t<kProtocol == RingBufferProtocol::PublishedCursor, T*, void>;
                using Allocator           = std::allocator<T>;
                using Position            = std::conditional_t<kProtocol == RingBufferProtocol::Local,           size_t, std::atomic<size_t>>;
                using CachedReadPosition  = std::conditional_t<kProtocol == RingBufferProtocol::PublishedCursor, size_t, void>;
                using CachedWritePosition = std::conditional_t<kProtocol == RingBufferProtocol::PublishedCursor, size_t, void>;
                using Arena               = std::conditional_t<kProtocol == RingBufferProtocol::Local,           BytesArena<T, Capacity>,
                                            std::conditional_t<kProtocol == RingBufferProtocol::SequenceCell,    CellsArena<T, Capacity, CacheLineAligned>, 
                                                                                                                 void>>;
                using WriteDomain         = std::conditional_t<Topo.producers == QueueCardinality::Multiple,     $::Contended,     $::ProducerOwned>;
                using ReadDomain          = std::conditional_t<Topo.consumers == QueueCardinality::Multiple,     $::Contended,     $::ConsumerOwned>;
                using CachedReadDomain    = std::conditional_t<kProtocol == RingBufferProtocol::PublishedCursor, $::ProducerOwned, $::Immutable>;
                using CachedWriteDomain   = std::conditional_t<kProtocol == RingBufferProtocol::PublishedCursor, $::ConsumerOwned, $::Immutable>;
                using ArenaDomain         = std::conditional_t<kProtocol == RingBufferProtocol::PublishedCursor, $::Immutable,     $::SharedStorage>;

                using Cell                = SequenceCell<T, CacheLineAligned>;
                // clang-format on

                static_assert(kProtocol == RingBufferProtocol::Local || std::atomic<std::size_t>::is_always_lock_free,
                              "concurrent RingBuffer requires lock-free size_t atomics");

            public:
                using ValueType = T;

                static constexpr std::size_t kCapacity = Capacity;
                static constexpr QueueTopology topology = Topo;
                static constexpr bool transactionalPushBatch = kProtocol != RingBufferProtocol::SequenceCell;
                static constexpr bool isolatesCells = kProtocol == RingBufferProtocol::SequenceCell && CacheLineAligned;

                RingBufferImpl() noexcept(kProtocol != RingBufferProtocol::PublishedCursor) {
                    if constexpr (kProtocol == RingBufferProtocol::PublishedCursor) {
                        allocatedSlots_ = allocator_.allocate(slotCount_ + 2 * kPadding);
                    } else if constexpr (kProtocol == RingBufferProtocol::SequenceCell) {
                        for (std::size_t index = 0; index != Capacity; ++index) {
                            arena_.cells[index].sequence.store(index, std::memory_order_relaxed);
                        }
                    }
                }

                RingBufferImpl(const RingBufferImpl&) = delete;
                RingBufferImpl& operator=(const RingBufferImpl&) = delete;
                RingBufferImpl(RingBufferImpl&&) = delete;
                RingBufferImpl& operator=(RingBufferImpl&&) = delete;

                

                // clang-format off
                [[= $::Immutable{}]]      
                SORA_NO_UNIQUE_ADDRESS                          PublishedSlotCount  slotCount_ = InitialSlotCount();
                [[= $::Immutable{}]]      
                SORA_NO_UNIQUE_ADDRESS                          AllocatedSlots      allocatedSlots_{};
                [[= $::Immutable{}]]      
                SORA_NO_UNIQUE_ADDRESS                          Allocator           allocator_{};
                [[= WriteDomain{}]]       
                alignas(kPositionAlignment)                     Position            writePosition_{0};
                [[= CachedReadDomain{}]]  
                SORA_NO_UNIQUE_ADDRESS alignas(kCacheAlignment) CachedReadPosition  cachedReadPosition_{};
                [[= ReadDomain{}]]        
                alignas(kPositionAlignment)                     Position            readPosition_{0};
                [[= CachedWriteDomain{}]] 
                SORA_NO_UNIQUE_ADDRESS alignas(kCacheAlignment) CachedWritePosition cachedWritePosition_{};
                [[= ArenaDomain{}]]       
                SORA_NO_UNIQUE_ADDRESS alignas(kArenaAlignment) Arena               arena_{};
                // clang-format on
            };

        } // namespace Detail

    } // namespace Concurrency

} // namespace Sora
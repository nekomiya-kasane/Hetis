/**
 * @file QueueTopology.h
 * @brief Compile-time producer and consumer topology vocabulary shared by bounded queue storage.
 * @ingroup Core
 */
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>

namespace Mashiro::Concurrency {

    /** @brief Number of execution agents permitted to invoke one queue endpoint concurrently. */
    enum class QueueCardinality : std::uint8_t {
        Single,
        Multiple,
    };

    /** @brief Whether both endpoints are confined to one thread or may execute concurrently. */
    enum class QueueSharing : std::uint8_t {
        SingleThread,
        Concurrent,
    };

    /** @brief Structural compile-time description of a queue's producer and consumer topology. */
    struct QueueTopology {
        QueueCardinality producers;
        QueueCardinality consumers;
        QueueSharing sharing;

        constexpr auto operator<=>(const QueueTopology&) const = default;
    };

    namespace QueueTopologies {

        inline constexpr QueueTopology Local{
            QueueCardinality::Single,
            QueueCardinality::Single,
            QueueSharing::SingleThread,
        };
        inline constexpr QueueTopology Spsc{
            QueueCardinality::Single,
            QueueCardinality::Single,
            QueueSharing::Concurrent,
        };
        inline constexpr QueueTopology Mpsc{
            QueueCardinality::Multiple,
            QueueCardinality::Single,
            QueueSharing::Concurrent,
        };
        inline constexpr QueueTopology Spmc{
            QueueCardinality::Single,
            QueueCardinality::Multiple,
            QueueSharing::Concurrent,
        };
        inline constexpr QueueTopology Mpmc{
            QueueCardinality::Multiple,
            QueueCardinality::Multiple,
            QueueSharing::Concurrent,
        };

    } // namespace QueueTopologies

    /** @brief Return whether a topology belongs to the queue protocol family. */
    [[nodiscard]] consteval bool IsQueueTopologyValid(QueueTopology topology) {
        return topology.sharing == QueueSharing::Concurrent ||
               (topology.producers == QueueCardinality::Single && topology.consumers == QueueCardinality::Single);
    }

    /**
     * @brief Reduce exact participant counts to the topology that affects generated synchronization.
     * @tparam Producers Number of producer threads.
     * @tparam Consumers Number of consumer threads.
     * @tparam SharedThreads Threads that may invoke both producer and consumer operations.
     */
    template<std::size_t Producers, std::size_t Consumers, std::size_t SharedThreads = 0>
    [[nodiscard]] consteval QueueTopology StaticQueueTopology() {
        static_assert(Producers > 0 && Consumers > 0, "a queue requires at least one producer and one consumer");
        static_assert(SharedThreads <= Producers && SharedThreads <= Consumers,
                      "shared queue threads must belong to both endpoint sets");
        constexpr std::size_t uniqueThreads = Producers + Consumers - SharedThreads;
        return {
            Producers == 1 ? QueueCardinality::Single : QueueCardinality::Multiple,
            Consumers == 1 ? QueueCardinality::Single : QueueCardinality::Multiple,
            uniqueThreads == 1 ? QueueSharing::SingleThread : QueueSharing::Concurrent,
        };
    }

} // namespace Mashiro::Concurrency

/**
 * @file MapTopology.h
 * @brief Compile-time writer, reader, and ordering vocabulary for associative containers.
 * @ingroup Core
 */
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>

namespace Mashiro::Concurrency {

    /** @brief Number of execution agents permitted to mutate one associative container concurrently. */
    enum class MapCardinality : std::uint8_t {
        Single,
        Multiple,
    };

    /** @brief Whether readers and writers are confined to one thread or may execute concurrently. */
    enum class MapSharing : std::uint8_t {
        SingleThread,
        Concurrent,
    };

    /**
     * @brief Key-order contract exposed by an associative container.
     *
     * Ordered mode keeps hash-indexed point operations and guarantees key order for @c Snapshot; it is not a tree
     * insertion protocol and therefore does not add a writer-wide rebalancing lock to the hot path.
     */
    enum class MapOrder : std::uint8_t {
        Unordered,
        Ordered,
    };

    /** @brief Structural compile-time description of a map or set's writer and reader topology. */
    struct MapTopology {
        MapCardinality writers;
        MapCardinality readers;
        MapSharing sharing;

        constexpr auto operator<=>(const MapTopology&) const = default;
    };

    namespace MapTopologies {

        inline constexpr MapTopology Local{
            MapCardinality::Single,
            MapCardinality::Single,
            MapSharing::SingleThread,
        };
        inline constexpr MapTopology Spsc{
            MapCardinality::Single,
            MapCardinality::Single,
            MapSharing::Concurrent,
        };
        inline constexpr MapTopology Mpsc{
            MapCardinality::Multiple,
            MapCardinality::Single,
            MapSharing::Concurrent,
        };
        inline constexpr MapTopology Spmc{
            MapCardinality::Single,
            MapCardinality::Multiple,
            MapSharing::Concurrent,
        };
        inline constexpr MapTopology Mpmc{
            MapCardinality::Multiple,
            MapCardinality::Multiple,
            MapSharing::Concurrent,
        };

    } // namespace MapTopologies

    /** @brief Return whether a topology belongs to the associative-container protocol family. */
    [[nodiscard]] consteval bool IsMapTopologyValid(MapTopology topology) {
        return topology.sharing == MapSharing::Concurrent ||
               (topology.writers == MapCardinality::Single && topology.readers == MapCardinality::Single);
    }

    /**
     * @brief Reduce exact participant counts to the topology that affects generated synchronization.
     * @tparam Writers Number of mutating threads.
     * @tparam Readers Number of lookup threads.
     * @tparam SharedThreads Threads that may invoke both mutation and lookup operations.
     */
    template<std::size_t Writers, std::size_t Readers, std::size_t SharedThreads = 0>
    [[nodiscard]] consteval MapTopology StaticMapTopology() {
        static_assert(Writers > 0 && Readers > 0, "an associative container requires a writer and a reader");
        static_assert(SharedThreads <= Writers && SharedThreads <= Readers,
                      "shared map threads must belong to both endpoint sets");
        constexpr std::size_t uniqueThreads = Writers + Readers - SharedThreads;
        return {
            Writers == 1 ? MapCardinality::Single : MapCardinality::Multiple,
            Readers == 1 ? MapCardinality::Single : MapCardinality::Multiple,
            uniqueThreads == 1 ? MapSharing::SingleThread : MapSharing::Concurrent,
        };
    }

} // namespace Mashiro::Concurrency

#pragma once

#include <meta>
#include <type_traits>
#include <concepts>

namespace Mashiro {
    
    namespace Concurrency {
        
        // =========================================================================
        // Contention-domain vocabulary (open type set)
        // =========================================================================

        /// @brief Root of the contention-domain hierarchy. Derive to declare a write-ownership role.
        struct ContentionDomain {};

        /// @brief Domains whose members are never written concurrently (read-only after construction),
        ///        hence exempt from internal false-sharing conflict checks.
        struct InertContentionDomain : ContentionDomain {};

        /// @brief A type usable as a member contention-domain annotation.
        template<typename A>
        concept ContentionDomainTag = std::derived_from<A, ContentionDomain> && std::is_empty_v<A>;

        /// @brief Written by a single producer thread only.
        struct ProducerOwned : ContentionDomain {};
        /// @brief Written by a single consumer thread only.
        struct ConsumerOwned : ContentionDomain {};
        /// @brief Hot word hammered by all threads (CAS/RMW); must own its cache line.
        struct Contended : ContentionDomain {};
        /// @brief Bulk storage touched by several agents at disjoint offsets (e.g. a slot array).
        struct SharedStorage : ContentionDomain {};
        /// @brief Read-only after construction; co-locatable with anything (no concurrent write).
        struct Immutable : InertContentionDomain {};

    }

}

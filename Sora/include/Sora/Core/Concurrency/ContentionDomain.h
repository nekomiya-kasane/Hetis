#pragma once

#include <concepts>
#include <type_traits>

namespace Sora {

    namespace Concurrency {

        namespace $ {

            /// @brief Root of the contention-domain hierarchy. Derive to declare a write-ownership role.
            struct ContentionDomain {};
            
            /// @brief Domains whose members are never written concurrently, hence exempt from conflict checks.
            struct InertContentionDomain : ContentionDomain {};
            /// @brief Written by a single producer thread only.
            struct ProducerOwned : ContentionDomain {};
            /// @brief Written by a single consumer thread only.
            struct ConsumerOwned : ContentionDomain {};
            /// @brief Hot word hammered by all threads (CAS/RMW); must own its cache line.
            struct Contended : ContentionDomain {};
            /// @brief Bulk storage touched by several agents at disjoint offsets, such as a slot array.
            struct SharedStorage : ContentionDomain {};
            /// @brief Read-only after construction; co-locatable with anything because there is no concurrent write.
            struct Immutable : InertContentionDomain {};

        } // namespace $

        namespace Concept {

            /// @brief A type usable as a member contention-domain annotation.
            template<typename A>
            concept ContentionDomainTag =
                std::derived_from<A, Sora::Concurrency::$::ContentionDomain> && std::is_empty_v<A>;

        } // namespace Concept

    } // namespace Concurrency

    namespace $ {

        using Sora::Concurrency::$::ConsumerOwned;
        using Sora::Concurrency::$::Contended;
        using Sora::Concurrency::$::ContentionDomain;
        using Sora::Concurrency::$::Immutable;
        using Sora::Concurrency::$::InertContentionDomain;
        using Sora::Concurrency::$::ProducerOwned;
        using Sora::Concurrency::$::SharedStorage;

    } // namespace $

    namespace Concept {

        using Sora::Concurrency::Concept::ContentionDomainTag;

    }

} // namespace Sora
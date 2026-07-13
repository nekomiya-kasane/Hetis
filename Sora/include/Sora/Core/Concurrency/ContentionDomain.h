/**
 * @file ContentionDomain.h
 * @brief Define the open contention-domain vocabulary used to classify concurrently accessed data members.
 * @details A contention domain is an empty tag type derived from @ref Sora::Concurrency::$::ContentionDomain. Domain
 * identity represents write ownership: members assigned to the same active domain may share cache lines, while members
 * assigned to distinct active domains require layout separation. Applications may introduce additional domain types by
 * deriving from the root tag, without modifying the central vocabulary.
 * @ingroup Core
 */
#pragma once

#include <concepts>
#include <type_traits>

namespace Sora {

    namespace Concurrency {

        namespace $ {

            /** @brief Root of the open contention-domain hierarchy used to declare write-ownership roles. */
            struct ContentionDomain {};

            /**
             * @brief Root for domains whose members are never written concurrently and are exempt from conflict checks.
             */
            struct InertContentionDomain : ContentionDomain {};

            /** @brief Identify members written exclusively by one producer thread. */
            struct ProducerOwned : ContentionDomain {};

            /** @brief Identify members written exclusively by one consumer thread. */
            struct ConsumerOwned : ContentionDomain {};

            /**
             * @brief Identify contended atomic state updated by multiple threads through read-modify-write operations.
             */
            struct Contended : ContentionDomain {};

            /** @brief Identify bulk storage concurrently accessed by multiple agents at logically disjoint offsets. */
            struct SharedStorage : ContentionDomain {};

            /**
             * @brief Identify post-construction immutable state that may share cache lines with any domain.
             */
            struct Immutable : InertContentionDomain {};

        } // namespace $

        namespace Concept {

            /**
             * @brief Determine whether @p A is a valid contention-domain annotation type.
             * @tparam A Candidate annotation type that publicly derives from the domain root and contains no state.
             */
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

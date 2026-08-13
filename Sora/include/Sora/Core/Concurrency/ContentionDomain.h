/**
 * @file ContentionDomain.h
 * @brief Define the open contention-domain vocabulary used to classify concurrently accessed data members.
 * @details
 * A contention domain is an empty tag type that describes both access policy and agent identity. Generic writer
 *
 * domains remain isolated even from the same domain. Only an explicitly cohesive protocol may opt into same-domain
 *
 * sharing. Read-only state may share only with other read-only state. Independent region-coverage tags state whether
 *
 * an aggregate member is cohesive or proves its own internal isolation.
 *
 * @ingroup Core
 */
#pragma once

#include <cstddef>
#include <concepts>
#include <type_traits>

namespace Sora {

    namespace Concurrency {

        namespace $ {

            /** @brief Root of the open contention-domain hierarchy used to declare concurrent access roles. */
            struct ContentionDomain {};

            /** @brief Root for domains read, but never written, during concurrent operation. */
            struct ReadOnlyContentionDomain : ContentionDomain {};

            /** @brief Root for domains written by exactly one agent during concurrent operation. */
            struct SingleWriterContentionDomain : ContentionDomain {};

            /** @brief Root for a single-writer protocol whose same-domain regions may share a cache line. */
            struct CohesiveSingleWriterContentionDomain : SingleWriterContentionDomain {};

            /** @brief Root for domains whose storage may be written by multiple concurrent agents. */
            struct MultiWriterContentionDomain : ContentionDomain {};

            /** @brief Root for a proven cohesive multi-writer protocol whose same-domain regions may share a line. */
            struct CohesiveMultiWriterContentionDomain : MultiWriterContentionDomain {};

            /** @brief Root for storage that is not accessed during concurrent operation. */
            struct InactiveContentionDomain : ContentionDomain {};

            /** @brief Root for annotations that prove coverage of a composite contention region. */
            struct ContentionRegionCoverage {};

            /** @brief State that every subregion follows the enclosing member's one cohesive access protocol. */
            struct CohesiveContentionRegion : ContentionRegionCoverage {};

            /** @brief State that a composite member independently proves isolation among its internal subregions. */
            struct InternallyIsolatedContentionRegion : ContentionRegionCoverage {};

            /** @brief Identify members written exclusively by one producer thread. */
            struct ProducerOwned : SingleWriterContentionDomain {};

            /** @brief Identify members written exclusively by one consumer thread. */
            struct ConsumerOwned : SingleWriterContentionDomain {};

            /** @brief Identify contended atomic state updated by multiple threads via read-modify-write operations. */
            struct Contended : MultiWriterContentionDomain {};

            /** @brief Identify bulk storage concurrently accessed by multiple agents at logically disjoint offsets. */
            struct SharedStorage : MultiWriterContentionDomain {};

            /** @brief Identify immutable state that may share cache lines with other read-only state. */
            struct Immutable : ReadOnlyContentionDomain {};

            /** @brief Identify compile-time-erased or quiescent state absent from concurrent memory traffic. */
            struct Inactive : InactiveContentionDomain {};

        } // namespace $

        namespace Concept {

            /**
             * @brief Determine whether @p A is a valid contention-domain annotation type.
             * @tparam A
             * Empty annotation type deriving from exactly one access-policy root.
             */
            template<typename A>
            concept ContentionDomainTag =
                std::is_empty_v<A> && std::derived_from<A, Sora::Concurrency::$::ContentionDomain> &&
                !std::derived_from<A, Sora::Concurrency::$::ContentionRegionCoverage> &&
                (static_cast<size_t>(std::derived_from<A, $::ReadOnlyContentionDomain>) +
                     static_cast<size_t>(std::derived_from<A, $::SingleWriterContentionDomain>) +
                     static_cast<size_t>(std::derived_from<A, $::MultiWriterContentionDomain>) +
                     static_cast<size_t>(std::derived_from<A, $::InactiveContentionDomain>) ==
                 1);

            /** @brief Determine whether @p A is a valid composite-region coverage annotation. */
            template<typename A>
            concept ContentionRegionCoverageTag =
                std::is_empty_v<A> && std::derived_from<A, Sora::Concurrency::$::ContentionRegionCoverage> &&
                !std::derived_from<A, Sora::Concurrency::$::ContentionDomain>;

        } // namespace Concept

    } // namespace Concurrency

    namespace $ {

        using Sora::Concurrency::$::CohesiveContentionRegion;
        using Sora::Concurrency::$::CohesiveMultiWriterContentionDomain;
        using Sora::Concurrency::$::CohesiveSingleWriterContentionDomain;
        using Sora::Concurrency::$::ConsumerOwned;
        using Sora::Concurrency::$::Contended;
        using Sora::Concurrency::$::ContentionDomain;
        using Sora::Concurrency::$::ContentionRegionCoverage;
        using Sora::Concurrency::$::Immutable;
        using Sora::Concurrency::$::Inactive;
        using Sora::Concurrency::$::InactiveContentionDomain;
        using Sora::Concurrency::$::InternallyIsolatedContentionRegion;
        using Sora::Concurrency::$::MultiWriterContentionDomain;
        using Sora::Concurrency::$::ProducerOwned;
        using Sora::Concurrency::$::ReadOnlyContentionDomain;
        using Sora::Concurrency::$::SharedStorage;
        using Sora::Concurrency::$::SingleWriterContentionDomain;

    } // namespace $

    namespace Concept {

        using Sora::Concurrency::Concept::ContentionDomainTag;
        using Sora::Concurrency::Concept::ContentionRegionCoverageTag;

    } // namespace Concept

} // namespace Sora

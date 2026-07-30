/**
 * @file QueueTopology.h
 * @brief Compile-time producer and consumer topology vocabulary shared by bounded queue storage.
 * @ingroup Core
 */
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>

namespace Sora {

    namespace Concurrency {

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

            inline constexpr bool IsValid() const noexcept {
                return sharing == QueueSharing::Concurrent ||
                       (producers == QueueCardinality::Single && consumers == QueueCardinality::Single);
            }
        };

        inline namespace QueueTopologies {

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

        namespace Traits {

            /**
             * @brief Reduce exact participant counts to the topology that affects generated synchronization.
             * @tparam Producers Number of producer threads.
             * @tparam Consumers Number of consumer threads.
             * @tparam SharedThreads Threads that may invoke both producer and consumer operations.
             */
            template<size_t Producers, size_t Consumers, size_t SharedThreads = 0>
            [[nodiscard]] consteval QueueTopology GetQueueTopology() {
                static_assert(Producers > 0 && Consumers > 0,
                              "a queue requires at least one producer and one consumer");
                static_assert(SharedThreads <= Producers && SharedThreads <= Consumers,
                              "shared queue threads must belong to both endpoint sets");
                constexpr size_t uniqueThreads = Producers + Consumers - SharedThreads;
                return QueueTopology{
                    .producers = Producers == 1 ? QueueCardinality::Single : QueueCardinality::Multiple,
                    .consumers = Consumers == 1 ? QueueCardinality::Single : QueueCardinality::Multiple,
                    .sharing = uniqueThreads == 1 ? QueueSharing::SingleThread : QueueSharing::Concurrent,
                };
            }

        } // namespace Traits

        using Traits::GetQueueTopology;

    } // namespace Concurrency

    using Concurrency::QueueCardinality;
    using Concurrency::QueueSharing;
    using Concurrency::QueueTopology;

    using Concurrency::QueueTopologies::Local;
    using Concurrency::QueueTopologies::Mpmc;
    using Concurrency::QueueTopologies::Mpsc;
    using Concurrency::QueueTopologies::Spmc;
    using Concurrency::QueueTopologies::Spsc;

    namespace Traits {

        inline namespace Concurrency {

            using Sora::Concurrency::Traits::GetQueueTopology;

        } // namespace Concurrency

    } // namespace Traits

} // namespace Sora

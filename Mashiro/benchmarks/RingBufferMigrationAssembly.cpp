/**
 * @file RingBufferMigrationAssembly.cpp
 * @brief Stable exported probes for comparing Mashiro and Sora RingBuffer hot-path code generation.
 * @ingroup Core
 */

#include <Mashiro/Core/RingBuffer.h>
#include <Sora/Core/Concurrency/RingBuffer.h>

#include <cstdint>

namespace {

    struct Payload {
        std::uint64_t id;
    };

    template<Mashiro::Concurrency::QueueTopology MashiroTopology, Sora::Concurrency::QueueTopology SoraTopology>
    struct Rings {
        using MashiroType = Mashiro::RingBuffer<Payload, 1024, MashiroTopology>;
        using SoraType = Sora::Concurrency::RingBuffer<Payload, 1024, SoraTopology>;

        static_assert(sizeof(MashiroType) == sizeof(SoraType));
        static_assert(alignof(MashiroType) == alignof(SoraType));
    };

    using Local = Rings<Mashiro::Concurrency::QueueTopologies::Local, Sora::Concurrency::QueueTopologies::Local>;
    using Spsc = Rings<Mashiro::Concurrency::QueueTopologies::Spsc, Sora::Concurrency::QueueTopologies::Spsc>;
    using Mpsc = Rings<Mashiro::Concurrency::QueueTopologies::Mpsc, Sora::Concurrency::QueueTopologies::Mpsc>;
    using Spmc = Rings<Mashiro::Concurrency::QueueTopologies::Spmc, Sora::Concurrency::QueueTopologies::Spmc>;
    using Mpmc = Rings<Mashiro::Concurrency::QueueTopologies::Mpmc, Sora::Concurrency::QueueTopologies::Mpmc>;

} // namespace

extern "C" {

[[gnu::noinline]] bool MashiroLocalPush(Local::MashiroType* ring, Payload value) noexcept {
    return ring->TryPush(value);
}
[[gnu::noinline]] bool SoraLocalPush(Local::SoraType* ring, Payload value) noexcept {
    return ring->TryPush(value);
}
[[gnu::noinline]] bool MashiroLocalPop(Local::MashiroType* ring, Payload* value) noexcept {
    return ring->TryPop(*value);
}
[[gnu::noinline]] bool SoraLocalPop(Local::SoraType* ring, Payload* value) noexcept {
    return ring->TryPop(*value);
}

[[gnu::noinline]] bool MashiroSpscPush(Spsc::MashiroType* ring, Payload value) noexcept {
    return ring->TryPush(value);
}
[[gnu::noinline]] bool SoraSpscPush(Spsc::SoraType* ring, Payload value) noexcept {
    return ring->TryPush(value);
}
[[gnu::noinline]] bool MashiroSpscPop(Spsc::MashiroType* ring, Payload* value) noexcept {
    return ring->TryPop(*value);
}
[[gnu::noinline]] bool SoraSpscPop(Spsc::SoraType* ring, Payload* value) noexcept {
    return ring->TryPop(*value);
}

[[gnu::noinline]] bool MashiroMpscPush(Mpsc::MashiroType* ring, Payload value) noexcept {
    return ring->TryPush(value);
}
[[gnu::noinline]] bool SoraMpscPush(Mpsc::SoraType* ring, Payload value) noexcept {
    return ring->TryPush(value);
}
[[gnu::noinline]] bool MashiroMpscPop(Mpsc::MashiroType* ring, Payload* value) noexcept {
    return ring->TryPop(*value);
}
[[gnu::noinline]] bool SoraMpscPop(Mpsc::SoraType* ring, Payload* value) noexcept {
    return ring->TryPop(*value);
}

[[gnu::noinline]] bool MashiroSpmcPush(Spmc::MashiroType* ring, Payload value) noexcept {
    return ring->TryPush(value);
}
[[gnu::noinline]] bool SoraSpmcPush(Spmc::SoraType* ring, Payload value) noexcept {
    return ring->TryPush(value);
}
[[gnu::noinline]] bool MashiroSpmcPop(Spmc::MashiroType* ring, Payload* value) noexcept {
    return ring->TryPop(*value);
}
[[gnu::noinline]] bool SoraSpmcPop(Spmc::SoraType* ring, Payload* value) noexcept {
    return ring->TryPop(*value);
}

[[gnu::noinline]] bool MashiroMpmcPush(Mpmc::MashiroType* ring, Payload value) noexcept {
    return ring->TryPush(value);
}
[[gnu::noinline]] bool SoraMpmcPush(Mpmc::SoraType* ring, Payload value) noexcept {
    return ring->TryPush(value);
}
[[gnu::noinline]] bool MashiroMpmcPop(Mpmc::MashiroType* ring, Payload* value) noexcept {
    return ring->TryPop(*value);
}
[[gnu::noinline]] bool SoraMpmcPop(Mpmc::SoraType* ring, Payload* value) noexcept {
    return ring->TryPop(*value);
}

} // extern "C"

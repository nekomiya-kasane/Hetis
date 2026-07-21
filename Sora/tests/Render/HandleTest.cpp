/**
 * @file HandleTest.cpp
 * @brief Verify Render handle encoding, stable pool growth, stale-handle rejection, and deferred reclamation.
 * @ingroup Testing
 */

#include <Sora/Render/HandlePool.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace Render = Sora::Render;

namespace {

    struct Record {
        int value = 0;
    };

    struct ThrowingRecord {
        int value = 0;

        explicit ThrowingRecord(int initial) : value(initial) {
            if (initial < 0) {
                throw std::runtime_error{"construction rejected"};
            }
        }
    };

    struct LiveCounter {
        inline static int count = 0;

        int value = 0;

        explicit LiveCounter(int initial) noexcept : value(initial) { ++count; }
        ~LiveCounter() { --count; }
    };

    inline constexpr Render::HandlePoolPolicy kOneSlotPolicy{
        .maximumCount = 1,
        .allocatedChunkSize = 1,
        .initialCount = 1,
    };

    inline constexpr Render::HandlePoolPolicy kTwoSlotPolicy{
        .maximumCount = 2,
        .allocatedChunkSize = 1,
        .initialCount = 1,
    };

    inline constexpr Render::HandlePoolPolicy kFourSlotPolicy{
        .maximumCount = 4,
        .allocatedChunkSize = 2,
        .initialCount = 2,
    };

    inline constexpr Render::HandlePoolPolicy kConcurrentPolicy{
        .maximumCount = 64,
        .allocatedChunkSize = 8,
        .initialCount = 8,
    };

    static_assert(sizeof(Render::BufferHandle) == sizeof(std::uint64_t));
    static_assert(std::is_trivially_copyable_v<Render::BufferHandle>);
    static_assert(!std::is_same_v<Render::BufferHandle, Render::TextureHandle>);
    static_assert(Render::HandlePoolValue<Record>);

} // namespace

TEST_CASE("Render handles preserve their packed type, backend, index, and generation", "[Sora.Render.Handle]") {
    constexpr Render::BufferHandle nullHandle;

    static_assert(!nullHandle.IsValid());
    static_assert(!static_cast<bool>(nullHandle));

    constexpr Render::BufferHandle handle = Render::BufferHandle::Pack(17, 0x1234'5678U, Render::Backend::Vulkan);
    static_assert(handle.IsValid());
    static_assert(handle.GetGeneration() == 17);
    static_assert(handle.GetIndex() == 0x1234'5678U);
    static_assert(handle.GetKind() == Render::HandleKind::Buffer);
    static_assert(handle.GetBackend() == Render::Backend::Vulkan);

    REQUIRE(handle.GetTypeTag() == static_cast<std::uint8_t>(Render::HandleKind::Buffer));
    REQUIRE(handle.GetBackendTag() == static_cast<std::uint8_t>(Render::Backend::Vulkan));
}

TEST_CASE("HandlePool grows without moving live payloads and rejects stale handles", "[Sora.Render.HandlePool]") {
    Render::HandlePool<Record, Render::HandleKind::Buffer, kFourSlotPolicy> pool;
    REQUIRE(pool.Capacity() == 2);
    REQUIRE(pool.FreeCount() == 2);

    auto first = pool.Emplace(Render::Backend::Vulkan, Record{.value = 11});
    auto second = pool.Emplace(Render::Backend::Vulkan, Record{.value = 22});
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    Record* firstAddress = first->value;
    pool.SetDebugName(first->handle, "PrimaryVertexBuffer");
#ifndef NDEBUG
    REQUIRE(pool.GetDebugName(first->handle) == "PrimaryVertexBuffer");
#else
    REQUIRE(pool.GetDebugName(first->handle).empty());
#endif

    auto third = pool.Emplace(Render::Backend::Vulkan, Record{.value = 33});
    REQUIRE(third.has_value());
    REQUIRE(pool.Capacity() == 4);
    REQUIRE(pool.Lookup(first->handle) == firstAddress);
    REQUIRE(pool.Lookup(first->handle)->value == 11);
    REQUIRE(pool.LiveCount() == 3);

    const Render::BufferHandle stale = first->handle;
    REQUIRE(pool.Free(stale));
    REQUIRE(pool.Lookup(stale) == nullptr);
    REQUIRE(pool.GetDebugName(stale).empty());
    REQUIRE_FALSE(pool.Free(stale));

    auto fourth = pool.Emplace(Render::Backend::Direct3D12, Record{.value = 44});
    REQUIRE(fourth.has_value());
    REQUIRE(fourth->handle.GetBackend() == Render::Backend::Direct3D12);
    REQUIRE(fourth->handle != stale);
    REQUIRE(fourth->handle.GetIndex() != stale.GetIndex());
    REQUIRE(pool.Lookup(fourth->handle)->value == 44);
}

TEST_CASE("HandlePool restores a slot when payload construction throws", "[Sora.Render.HandlePool]") {
    Render::HandlePool<ThrowingRecord, Render::HandleKind::Texture, kOneSlotPolicy> pool;

    REQUIRE_THROWS_AS(
        [&pool] {
            auto rejected = pool.Emplace(Render::Backend::Vulkan, -1);
            static_cast<void>(rejected);
        }(),
        std::runtime_error);
    REQUIRE(pool.FreeCount() == 1);
    REQUIRE(pool.LiveCount() == 0);

    auto allocation = pool.Emplace(Render::Backend::Vulkan, 73);
    REQUIRE(allocation.has_value());
    REQUIRE(allocation->value->value == 73);
}

TEST_CASE("HandlePool reports exhaustion and reuses released slots", "[Sora.Render.HandlePool]") {
    Render::HandlePool<Record, Render::HandleKind::Sampler, kTwoSlotPolicy> pool;
    auto first = pool.Allocate(Render::Backend::Vulkan);
    auto second = pool.Allocate(Render::Backend::Vulkan);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(pool.Capacity() == 2);

    const auto exhausted = pool.Allocate(Render::Backend::Vulkan);
    REQUIRE_FALSE(exhausted.has_value());
    REQUIRE(exhausted.error() == Sora::ErrorCode::ResourceExhausted);

    const Render::SamplerHandle stale = first->handle;
    REQUIRE(pool.Free(stale));
    auto reused = pool.Allocate(Render::Backend::Vulkan);
    REQUIRE(reused.has_value());
    REQUIRE(reused->handle.GetIndex() == stale.GetIndex());
    REQUIRE(reused->handle.GetGeneration() == stale.GetGeneration() + 1);
    REQUIRE(pool.Lookup(stale) == nullptr);
}

TEST_CASE("HandlePool deferred tokens cannot reclaim a later payload", "[Sora.Render.HandlePool]") {
    using Pool = Render::HandlePool<Record, Render::HandleKind::Fence, kOneSlotPolicy>;

    Pool pool;
    auto first = pool.Emplace(Render::Backend::Vulkan, Record{.value = 101});
    REQUIRE(first.has_value());

    const auto firstToken = pool.MarkDead(first->handle);
    REQUIRE(firstToken.has_value());
    const Pool::ReclaimToken firstReclaim = firstToken.value_or(Pool::ReclaimToken{});
    REQUIRE(firstReclaim.IsValid());
    REQUIRE(pool.Lookup(first->handle) == nullptr);
    Record* firstDead = pool.LookupDead(firstReclaim);
    REQUIRE(firstDead != nullptr);
    REQUIRE(firstDead->value == 101);
    REQUIRE(pool.LiveCount() == 0);
    REQUIRE(pool.PendingReclaimCount() == 1);
    REQUIRE(pool.Reclaim(firstReclaim));
    REQUIRE_FALSE(pool.Reclaim(firstReclaim));

    auto second = pool.Emplace(Render::Backend::Vulkan, Record{.value = 202});
    REQUIRE(second.has_value());
    const auto secondToken = pool.MarkDead(second->handle);
    REQUIRE(secondToken.has_value());
    const Pool::ReclaimToken secondReclaim = secondToken.value_or(Pool::ReclaimToken{});
    REQUIRE(secondReclaim.IsValid());
    REQUIRE(secondReclaim != firstReclaim);
    REQUIRE(pool.LookupDead(firstReclaim) == nullptr);
    REQUIRE_FALSE(pool.Reclaim(firstReclaim));
    Record* secondDead = pool.LookupDead(secondReclaim);
    REQUIRE(secondDead != nullptr);
    REQUIRE(secondDead->value == 202);
    REQUIRE(pool.Reclaim(secondReclaim));
}

TEST_CASE("HandlePool destroys live and deferred payloads exactly once", "[Sora.Render.HandlePool]") {
    LiveCounter::count = 0;
    {
        Render::HandlePool<LiveCounter, Render::HandleKind::Semaphore, kTwoSlotPolicy> pool;
        auto live = pool.Emplace(Render::Backend::Vulkan, 3);
        auto dead = pool.Emplace(Render::Backend::Vulkan, 5);
        REQUIRE(live.has_value());
        REQUIRE(dead.has_value());
        REQUIRE(LiveCounter::count == 2);
        REQUIRE(pool.MarkDead(dead->handle).has_value());
    }
    REQUIRE(LiveCounter::count == 0);
}

TEST_CASE("HandlePool retires a slot instead of wrapping its generation", "[Sora.Render.HandlePool]") {
    Render::HandlePool<Record, Render::HandleKind::PipelineCache, kOneSlotPolicy> pool;
    auto allocation = pool.Allocate(Render::Backend::Vulkan);
    bool sequenceValid = allocation.has_value();

    for (std::uint32_t generation = 1; generation < std::numeric_limits<std::uint16_t>::max(); ++generation) {
        sequenceValid = sequenceValid && allocation->handle.GetGeneration() == generation;
        sequenceValid = sequenceValid && pool.Free(allocation->handle);
        allocation = pool.Allocate(Render::Backend::Vulkan);
        sequenceValid = sequenceValid && allocation.has_value();
    }

    REQUIRE(sequenceValid);
    REQUIRE(allocation->handle.GetGeneration() == std::numeric_limits<std::uint16_t>::max());
    REQUIRE(pool.Free(allocation->handle));
    REQUIRE(pool.RetiredCount() == 1);
    REQUIRE(pool.FreeCount() == 0);
    const auto exhausted = pool.Allocate(Render::Backend::Vulkan);
    REQUIRE_FALSE(exhausted.has_value());
    REQUIRE(exhausted.error() == Sora::ErrorCode::ResourceExhausted);
}

TEST_CASE("HandlePool serializes concurrent allocation and release", "[Sora.Render.HandlePool]") {
    Render::HandlePool<Record, Render::HandleKind::CommandBuffer, kConcurrentPolicy> pool;
    std::atomic<bool> failed = false;
    std::vector<std::thread> workers;
    workers.reserve(8);

    for (int worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&pool, &failed, worker] {
            for (int iteration = 0; iteration < 1'000; ++iteration) {
                auto allocation = pool.Emplace(Render::Backend::Vulkan, Record{.value = worker});
                if (!allocation || pool.Lookup(allocation->handle) != allocation->value ||
                    allocation->value->value != worker || !pool.Free(allocation->handle)) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    REQUIRE_FALSE(failed.load(std::memory_order_relaxed));
    REQUIRE(pool.LiveCount() == 0);
    REQUIRE(pool.FreeCount() == pool.Capacity());
}

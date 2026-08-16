/**
 * @file AsyncQueueMigrationAssembly.cpp
 * @brief Stable exported probes for inspecting AsyncQueue sync-wait migration code generation.
 * @ingroup Core
 */

#if defined(SORA_MIGRATION_TARGET)
#    include <Sora/Core/Concurrency/AsyncQueue.h>
#    include <Sora/Core/Concurrency/RingBuffer.h>
namespace Migration = Sora;
#else
#    include <Mashiro/Concurrency/AsyncQueue.h>
#    include <Mashiro/Core/RingBuffer.h>
namespace Migration = Mashiro;
#endif

#include <stdexec/execution.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utility>

namespace {

    template<std::size_t Size>
    struct Payload {
        std::uint64_t id{};
        std::array<std::byte, Size - sizeof(std::uint64_t)> data{};
    };

    template<>
    struct Payload<sizeof(std::uint64_t)> {
        std::uint64_t id{};
    };

#if defined(SORA_MIGRATION_TARGET)
    template<typename T, std::size_t Capacity, Migration::Concurrency::QueueTopology Topology>
    using Ring = Migration::Concurrency::RingBuffer<T, Capacity, Topology>;
#else
    template<typename T, std::size_t Capacity, Migration::Concurrency::QueueTopology Topology>
    using Ring = Migration::RingBuffer<T, Capacity, Topology>;
#endif

    template<typename T, Migration::Concurrency::QueueTopology Topology>
    using Queue = Migration::Concurrency::AsyncQueue<Ring<T, 1024, Topology>>;

    using Spsc8 = Queue<Payload<8>, Migration::Concurrency::QueueTopologies::Spsc>;
    using Mpsc8 = Queue<Payload<8>, Migration::Concurrency::QueueTopologies::Mpsc>;
    using Mpmc8 = Queue<Payload<8>, Migration::Concurrency::QueueTopologies::Mpmc>;
    using Spsc64 = Queue<Payload<64>, Migration::Concurrency::QueueTopologies::Spsc>;
    using Mpsc64 = Queue<Payload<64>, Migration::Concurrency::QueueTopologies::Mpsc>;
    using Mpmc64 = Queue<Payload<64>, Migration::Concurrency::QueueTopologies::Mpmc>;

    template<typename QueueType, typename T>
    [[nodiscard]] std::uint8_t Push(QueueType* queue, T value) {
        auto producer = queue->Producer();
        auto result = stdexec::sync_wait(producer.Push(std::move(value)));
        return result.has_value() ? static_cast<std::uint8_t>(std::get<0>(*result).disposition) : std::uint8_t{0xff};
    }

    template<typename QueueType, typename T>
    [[nodiscard]] bool Pop(QueueType* queue, T* value) {
        auto consumer = queue->Consumer();
        auto result = stdexec::sync_wait(consumer.Pop());
        if (!result.has_value()) {
            return false;
        }
        *value = std::move(std::get<0>(*result));
        return true;
    }

} // namespace

extern "C" {

[[gnu::noinline]] std::uint8_t MigrationSpsc8AsyncPush(Spsc8* queue, Payload<8> value) {
    return Push(queue, value);
}
[[gnu::noinline]] bool MigrationSpsc8AsyncPop(Spsc8* queue, Payload<8>* value) {
    return Pop(queue, value);
}
[[gnu::noinline]] std::uint8_t MigrationMpsc8AsyncPush(Mpsc8* queue, Payload<8> value) {
    return Push(queue, value);
}
[[gnu::noinline]] bool MigrationMpsc8AsyncPop(Mpsc8* queue, Payload<8>* value) {
    return Pop(queue, value);
}
[[gnu::noinline]] std::uint8_t MigrationMpmc8AsyncPush(Mpmc8* queue, Payload<8> value) {
    return Push(queue, value);
}
[[gnu::noinline]] bool MigrationMpmc8AsyncPop(Mpmc8* queue, Payload<8>* value) {
    return Pop(queue, value);
}

[[gnu::noinline]] std::uint8_t MigrationSpsc64AsyncPush(Spsc64* queue, Payload<64> value) {
    return Push(queue, value);
}
[[gnu::noinline]] bool MigrationSpsc64AsyncPop(Spsc64* queue, Payload<64>* value) {
    return Pop(queue, value);
}
[[gnu::noinline]] std::uint8_t MigrationMpsc64AsyncPush(Mpsc64* queue, Payload<64> value) {
    return Push(queue, value);
}
[[gnu::noinline]] bool MigrationMpsc64AsyncPop(Mpsc64* queue, Payload<64>* value) {
    return Pop(queue, value);
}
[[gnu::noinline]] std::uint8_t MigrationMpmc64AsyncPush(Mpmc64* queue, Payload<64> value) {
    return Push(queue, value);
}
[[gnu::noinline]] bool MigrationMpmc64AsyncPop(Mpmc64* queue, Payload<64>* value) {
    return Pop(queue, value);
}

} // extern "C"

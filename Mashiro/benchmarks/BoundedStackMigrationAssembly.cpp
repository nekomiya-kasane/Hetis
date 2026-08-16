/**
 * @file BoundedStackMigrationAssembly.cpp
 * @brief Stable exported probes for inspecting bounded-stack hot-path code generation.
 * @ingroup Core
 */

#if defined(SORA_MIGRATION_TARGET)
#    include <Sora/Core/Concurrency/BoundedStack.h>
#else
#    include <Mashiro/Core/BoundedStack.h>
#endif

#include <cstdint>

#if defined(SORA_MIGRATION_TARGET)
#    define Mashiro Sora
#endif

namespace {

    struct Payload {
        std::uint64_t id;
    };

    template<Mashiro::Concurrency::QueueTopology Topology>
    using Stack = Mashiro::BoundedStack<Payload, 1024, Topology>;

    using Local = Stack<Mashiro::Concurrency::QueueTopologies::Local>;
    using Spsc = Stack<Mashiro::Concurrency::QueueTopologies::Spsc>;
    using Mpsc = Stack<Mashiro::Concurrency::QueueTopologies::Mpsc>;
    using Spmc = Stack<Mashiro::Concurrency::QueueTopologies::Spmc>;
    using Mpmc = Stack<Mashiro::Concurrency::QueueTopologies::Mpmc>;

} // namespace

extern "C" {

[[gnu::noinline]] bool MashiroLocalStackPush(Local* stack, Payload value) noexcept {
    return stack->TryPush(value);
}

[[gnu::noinline]] bool MashiroLocalStackPop(Local* stack, Payload* value) noexcept {
    return stack->TryPop(*value);
}

[[gnu::noinline]] bool MashiroSpscStackPush(Spsc* stack, Payload value) noexcept {
    return stack->TryPush(value);
}

[[gnu::noinline]] bool MashiroSpscStackPop(Spsc* stack, Payload* value) noexcept {
    return stack->TryPop(*value);
}

[[gnu::noinline]] bool MashiroMpscStackPush(Mpsc* stack, Payload value) noexcept {
    return stack->TryPush(value);
}

[[gnu::noinline]] bool MashiroMpscStackPop(Mpsc* stack, Payload* value) noexcept {
    return stack->TryPop(*value);
}

[[gnu::noinline]] bool MashiroSpmcStackPush(Spmc* stack, Payload value) noexcept {
    return stack->TryPush(value);
}

[[gnu::noinline]] bool MashiroSpmcStackPop(Spmc* stack, Payload* value) noexcept {
    return stack->TryPop(*value);
}

[[gnu::noinline]] bool MashiroMpmcStackPush(Mpmc* stack, Payload value) noexcept {
    return stack->TryPush(value);
}

[[gnu::noinline]] bool MashiroMpmcStackPop(Mpmc* stack, Payload* value) noexcept {
    return stack->TryPop(*value);
}

} // extern "C"

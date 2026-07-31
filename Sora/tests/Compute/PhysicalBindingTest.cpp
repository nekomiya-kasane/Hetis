#include "BackendTestSupport.h"
#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>
#include <Sora/Compute/Runtime/PhysicalBinding.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace {

    constexpr Sora::Compute::ValueType kFloat{Sora::Compute::ScalarKind::Float32, 1};
    constexpr Sora::Compute::MemoryLocation kLocation{
        .domain = {1, 1}, .device = {2, 2}, .backend = Sora::Compute::BackendKind::Cpu};

    struct NativeAllocation {
        std::atomic<uint32_t>* destroyCount = nullptr;
    };

    void DestroyAllocation(void* opaque) noexcept {
        std::unique_ptr<NativeAllocation> allocation{static_cast<NativeAllocation*>(opaque)};
        allocation->destroyCount->fetch_add(1, std::memory_order_relaxed);
    }

    consteval auto MakeSaxpyProgram() {
        Sora::Compute::KernelBuilder<3, 4> scalar;
        const auto alpha = scalar.AddValueParameter(kFloat);
        const auto x = scalar.AddValueParameter(kFloat);
        const auto y = scalar.AddValueParameter(kFloat);
        const auto value = scalar.Ternary(Sora::Compute::OpCode::Fma, alpha, x, y);
        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<4, 3, 4> program{domain, std::move(scalar).Finish(value),
                                                             Sora::Compute::PartitionContract::Elementwise(1)};
        const auto alphaArgument = program.AddValueArgument(kFloat);
        const auto xArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, 1,
                                                         Sora::Compute::LayoutKind::Contiguous, 64, {1});
        const auto yArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, 1,
                                                         Sora::Compute::LayoutKind::Contiguous, 64, {2});
        const auto outputArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Write, kFloat, 1,
                                                              Sora::Compute::LayoutKind::Contiguous, 64, {3});
        program.BindArgument(alphaArgument);
        program.BindArgument(xArgument);
        program.BindArgument(yArgument);
        return std::move(program).Finish(outputArgument);
    }

    consteval auto MakeReadWriteSaxpyProgram() {
        Sora::Compute::KernelBuilder<4, 6> scalar;
        const auto alpha = scalar.AddValueParameter(kFloat);
        const auto x = scalar.AddValueParameter(kFloat);
        const auto y = scalar.AddValueParameter(kFloat);
        const auto previous = scalar.AddValueParameter(kFloat);
        const auto saxpy = scalar.Ternary(Sora::Compute::OpCode::Fma, alpha, x, y);
        const auto value = scalar.Binary(Sora::Compute::OpCode::Add, saxpy, previous);
        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<4, 4, 6> program{domain, std::move(scalar).Finish(value),
                                                             Sora::Compute::PartitionContract::Elementwise(1)};
        const auto alphaArgument = program.AddValueArgument(kFloat);
        const auto xArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, 1,
                                                         Sora::Compute::LayoutKind::Contiguous, 64, {1});
        const auto yArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, 1,
                                                         Sora::Compute::LayoutKind::Contiguous, 64, {2});
        const auto outputArgument = program.AddBufferArgument(Sora::Compute::AccessMode::ReadWrite, kFloat, 1,
                                                              Sora::Compute::LayoutKind::Contiguous, 64, {3});
        program.BindArgument(alphaArgument);
        program.BindArgument(xArgument);
        program.BindArgument(yArgument);
        program.BindArgument(outputArgument);
        return std::move(program).Finish(outputArgument);
    }

    constexpr auto kSaxpy = MakeSaxpyProgram();
    constexpr auto kSaxpyReadWrite = MakeReadWriteSaxpyProgram();

    Sora::Compute::Allocation MakeAllocation(std::atomic<uint32_t>& destroyCount,
                                             Sora::Compute::PhysicalMemoryId memory, uint64_t memoryOffset = 0,
                                             uint64_t alignment = 64, uint64_t size = 1024) {
        auto native = std::make_unique<NativeAllocation>(&destroyCount);
        auto allocation = Sora::Compute::Allocation::Adopt(Sora::Test::TestBackendService(),
                                                           {
                                                               .location = kLocation,
                                                               .memory = memory,
                                                               .properties = Sora::Compute::MemoryProperty::HostVisible,
                                                               .memoryOffset = memoryOffset,
                                                               .sizeBytes = size,
                                                               .alignment = alignment,
                                                               .state = native.release(),
                                                               .destroy = DestroyAllocation,
                                                           });
        REQUIRE(allocation.has_value());
        return *allocation;
    }

    Sora::Compute::Buffer MakeBuffer(Sora::Compute::BufferId id) {
        auto buffer = Sora::Compute::Buffer::Create({
            .id = id,
            .elementType = kFloat,
            .layout = Sora::Compute::LayoutKind::Contiguous,
            .rank = 1,
            .sizeBytes = 256,
            .alignment = 64,
        });
        REQUIRE(buffer.has_value());
        return *buffer;
    }

    Sora::Compute::BufferView View(Sora::Compute::Buffer buffer) {
        constexpr std::array extents{uint64_t{64}};
        auto view = Sora::Compute::BufferView::Contiguous(std::move(buffer), extents);
        REQUIRE(view.has_value());
        return std::move(*view);
    }

    Sora::Compute::ScalarArgument FloatArgument(float value) {
        const auto bytes = std::bit_cast<std::array<std::byte, sizeof(float)>>(value);
        auto argument = Sora::Compute::ScalarArgument::Create(kFloat, bytes);
        REQUIRE(argument.has_value());
        return std::move(*argument);
    }

    template<typename Program>
    Sora::Compute::KernelInvocation MakeInvocationWithProgram(const Program& program, Sora::Compute::Buffer x,
                                                              Sora::Compute::Buffer y, Sora::Compute::Buffer output) {
        std::vector<Sora::Compute::KernelArgumentBinding> arguments;
        arguments.emplace_back(FloatArgument(2.0F));
        arguments.emplace_back(View(std::move(x)));
        arguments.emplace_back(View(std::move(y)));
        arguments.emplace_back(View(std::move(output)));
        constexpr std::array extents{uint64_t{64}};
        auto invocation = Sora::Compute::BindKernelProgram(program, extents, std::move(arguments));
        REQUIRE(invocation.has_value());

        return std::move(*invocation);
    }

    Sora::Compute::KernelInvocation MakeInvocation(Sora::Compute::Buffer x, Sora::Compute::Buffer y,
                                                   Sora::Compute::Buffer output) {
        return MakeInvocationWithProgram(kSaxpy, std::move(x), std::move(y), std::move(output));
    }

    Sora::Compute::KernelInvocation MakeReadWriteInvocation(Sora::Compute::Buffer x, Sora::Compute::Buffer y,
                                                            Sora::Compute::Buffer output) {
        return MakeInvocationWithProgram(kSaxpyReadWrite, std::move(x), std::move(y), std::move(output));
    }

    Sora::Compute::ResidentReplica Resident(Sora::Compute::Allocation allocation) {
        return {.allocation = std::move(allocation),
                .bufferRegion = *Sora::Compute::ByteRegion::Make(0, 256),
                .allocationOffset = 0,
                .version = 1};
    }

    Sora::Compute::PendingReplica Pending(Sora::Compute::Allocation allocation) {
        auto pending =
            Sora::Compute::PendingReplica::Create(std::move(allocation), *Sora::Compute::ByteRegion::Make(0, 256));
        REQUIRE(pending.has_value());
        return std::move(*pending);
    }

} // namespace

TEST_CASE("Physical binding materializes one scalar arena and complete affine buffer descriptors",
          "[Sora][Compute][Runtime][Binding]") {
    std::atomic<uint32_t> destroyCount = 0;
    auto invocation = MakeInvocation(MakeBuffer({1, 1}), MakeBuffer({1, 2}), MakeBuffer({1, 3}));
    const std::array sources{
        Sora::Compute::PhysicalBufferSource{Resident(MakeAllocation(destroyCount, {1, 1}))},
        Sora::Compute::PhysicalBufferSource{Resident(MakeAllocation(destroyCount, {2, 2}))},
        Sora::Compute::PhysicalBufferSource{Pending(MakeAllocation(destroyCount, {3, 3}))},
    };

    const auto bindings = Sora::Compute::MaterializeKernelBindings(invocation, sources);
    REQUIRE(bindings.has_value());
    REQUIRE(bindings->scalarData.size() == sizeof(float));
    REQUIRE(bindings->scalars.size() == 1);
    REQUIRE(bindings->scalars[0].binding == 0);
    REQUIRE(bindings->buffers.size() == 3);
    REQUIRE(bindings->buffers[0].binding == 1);
    REQUIRE(bindings->buffers[0].kind == Sora::Compute::BackendBindingKind::AffineElements);
    REQUIRE(bindings->buffers[0].extents[0] == 64);
    REQUIRE(bindings->buffers[0].byteStrides[0] == sizeof(float));
    REQUIRE(bindings->buffers[2].access == Sora::Compute::AccessMode::Write);
}

TEST_CASE("Physical binding materializes shard-local affine descriptors without global address arithmetic",
          "[Sora][Compute][Runtime][Binding][Partition]") {
    std::atomic<uint32_t> destroyCount = 0;
    auto invocation = MakeInvocation(MakeBuffer({1, 11}), MakeBuffer({1, 12}), MakeBuffer({1, 13}));
    const std::array sources{
        Sora::Compute::PhysicalBufferSource{Resident(MakeAllocation(destroyCount, {11, 1}))},
        Sora::Compute::PhysicalBufferSource{Resident(MakeAllocation(destroyCount, {12, 2}))},
        Sora::Compute::PhysicalBufferSource{Pending(MakeAllocation(destroyCount, {13, 3}))},
    };
    const Sora::Compute::DispatchSlice slice{.rank = 1, .origin = {16}, .extents = {16}, .workItemCount = 16};

    const auto bindings = Sora::Compute::MaterializeKernelBindings(invocation, sources, slice);
    REQUIRE(bindings.has_value());
    REQUIRE(bindings->buffers.size() == 3);
    for (const Sora::Compute::BackendBinding& binding : bindings->buffers) {
        REQUIRE(binding.offset == 64);
        REQUIRE(binding.size == 64);
        REQUIRE(binding.extents[0] == 16);
        REQUIRE(binding.byteStrides[0] == sizeof(float));
    }
}

TEST_CASE("Physical binding proves deferred noalias contracts against canonical storage ranges",
          "[Sora][Compute][Runtime][Binding][Alias]") {
    std::atomic<uint32_t> destroyCount = 0;
    auto invocation = MakeInvocation(MakeBuffer({2, 1}), MakeBuffer({2, 2}), MakeBuffer({2, 3}));
    constexpr Sora::Compute::PhysicalMemoryId shared{9, 9};
    const std::array overlapping{
        Sora::Compute::PhysicalBufferSource{Resident(MakeAllocation(destroyCount, shared, 0))},
        Sora::Compute::PhysicalBufferSource{Resident(MakeAllocation(destroyCount, shared, 128))},
        Sora::Compute::PhysicalBufferSource{Pending(MakeAllocation(destroyCount, {10, 10}))},
    };
    const auto rejected = Sora::Compute::MaterializeKernelBindings(invocation, overlapping);
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error() ==
            Sora::Compute::PhysicalBindingError{Sora::Compute::PhysicalBindingErrorCode::AliasContractViolation, 1, 2});

    const std::array disjoint{
        Sora::Compute::PhysicalBufferSource{Resident(MakeAllocation(destroyCount, shared, 0, 64, 256))},
        Sora::Compute::PhysicalBufferSource{Resident(MakeAllocation(destroyCount, shared, 256, 64, 256))},
        Sora::Compute::PhysicalBufferSource{Pending(MakeAllocation(destroyCount, {11, 11}))},
    };
    REQUIRE(Sora::Compute::MaterializeKernelBindings(invocation, disjoint).has_value());
}

TEST_CASE("Physical binding enforces signature alignment after placement",
          "[Sora][Compute][Runtime][Binding][Alignment]") {
    std::atomic<uint32_t> destroyCount = 0;
    auto invocation = MakeInvocation(MakeBuffer({3, 1}), MakeBuffer({3, 2}), MakeBuffer({3, 3}));
    const std::array sources{
        Sora::Compute::PhysicalBufferSource{Resident(MakeAllocation(destroyCount, {20, 20}, 0, 32))},
        Sora::Compute::PhysicalBufferSource{Resident(MakeAllocation(destroyCount, {21, 21}))},
        Sora::Compute::PhysicalBufferSource{Pending(MakeAllocation(destroyCount, {22, 22}))},
    };
    const auto rejected = Sora::Compute::MaterializeKernelBindings(invocation, sources);
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error().code == Sora::Compute::PhysicalBindingErrorCode::InsufficientAlignment);
    REQUIRE(rejected.error().argument == 1);
}

TEST_CASE("Physical binding distinguishes ready read storage from unpublished write targets",
          "[Sora][Compute][Runtime][Binding][State]") {
    std::atomic<uint32_t> destroyCount = 0;
    auto invocation = MakeInvocation(MakeBuffer({4, 1}), MakeBuffer({4, 2}), MakeBuffer({4, 3}));
    const std::array sources{
        Sora::Compute::PhysicalBufferSource{Resident(MakeAllocation(destroyCount, {30, 30}))},
        Sora::Compute::PhysicalBufferSource{Resident(MakeAllocation(destroyCount, {31, 31}))},
        Sora::Compute::PhysicalBufferSource{Resident(MakeAllocation(destroyCount, {32, 32}))},
    };
    const auto rejected = Sora::Compute::MaterializeKernelBindings(invocation, sources);
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error().code == Sora::Compute::PhysicalBindingErrorCode::SourceKindMismatch);
    REQUIRE(rejected.error().argument == 3);
}

TEST_CASE("Physical binding accepts graph-local pending storage for downstream reads",
          "[Sora][Compute][Runtime][Binding][State]") {
    std::atomic<uint32_t> destroyCount = 0;
    auto invocation = MakeInvocation(MakeBuffer({5, 1}), MakeBuffer({5, 2}), MakeBuffer({5, 3}));
    const std::array sources{
        Sora::Compute::PhysicalBufferSource{Pending(MakeAllocation(destroyCount, {40, 40}))},
        Sora::Compute::PhysicalBufferSource{Resident(MakeAllocation(destroyCount, {41, 41}))},
        Sora::Compute::PhysicalBufferSource{Pending(MakeAllocation(destroyCount, {42, 42}))},
    };

    const auto bindings = Sora::Compute::MaterializeKernelBindings(invocation, sources);
    REQUIRE(bindings.has_value());
    REQUIRE(bindings->buffers[0].access == Sora::Compute::AccessMode::Read);
    REQUIRE(bindings->buffers[0].allocation.memory == Sora::Compute::PhysicalMemoryId{40, 40});
}

TEST_CASE("Physical binding accepts pending storage for graph-local read-write dispatches",
          "[Sora][Compute][Runtime][Binding][State]") {
    std::atomic<uint32_t> destroyCount = 0;
    auto invocation = MakeReadWriteInvocation(MakeBuffer({6, 1}), MakeBuffer({6, 2}), MakeBuffer({6, 3}));
    const std::array sources{
        Sora::Compute::PhysicalBufferSource{Resident(MakeAllocation(destroyCount, {50, 50}))},
        Sora::Compute::PhysicalBufferSource{Resident(MakeAllocation(destroyCount, {51, 51}))},
        Sora::Compute::PhysicalBufferSource{Pending(MakeAllocation(destroyCount, {52, 52}))},
    };

    const auto bindings = Sora::Compute::MaterializeKernelBindings(invocation, sources);
    REQUIRE(bindings.has_value());
    REQUIRE(bindings->buffers[2].access == Sora::Compute::AccessMode::ReadWrite);
}

TEST_CASE("Physical binding rejects published storage as any writable dispatch target",
          "[Sora][Compute][Runtime][Binding][State]") {
    std::atomic<uint32_t> destroyCount = 0;
    auto invocation = MakeReadWriteInvocation(MakeBuffer({7, 1}), MakeBuffer({7, 2}), MakeBuffer({7, 3}));
    const std::array sources{
        Sora::Compute::PhysicalBufferSource{Resident(MakeAllocation(destroyCount, {60, 60}))},
        Sora::Compute::PhysicalBufferSource{Resident(MakeAllocation(destroyCount, {61, 61}))},
        Sora::Compute::PhysicalBufferSource{Resident(MakeAllocation(destroyCount, {62, 62}))},
    };

    const auto rejected = Sora::Compute::MaterializeKernelBindings(invocation, sources);
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error().code == Sora::Compute::PhysicalBindingErrorCode::SourceKindMismatch);
    REQUIRE(rejected.error().argument == 3);
}

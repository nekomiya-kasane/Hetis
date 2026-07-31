#include <Sora/Compute/Backend/VulkanBackend.h>
#include <Sora/Compute/Execution/SubmitSender.h>
#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>
#include <Sora/Compute/Runtime/PhysicalSegment.h>
#include <Sora/Compute/Runtime/PipelineBindingLayout.h>
#include <Sora/Compute/Runtime/RuntimeDiscovery.h>

#include <catch2/catch_test_macros.hpp>
#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace {

    constexpr Sora::Compute::ValueType kFloat{Sora::Compute::ScalarKind::Float32, 1};
    constexpr Sora::Compute::ValueType kDouble{Sora::Compute::ScalarKind::Float64, 1};
    constexpr Sora::Compute::ValueType kFloat3{Sora::Compute::ScalarKind::Float32, 3};
    constexpr Sora::Compute::ValueType kIndex{Sora::Compute::ScalarKind::UInt64, 1};

    consteval auto MakeSaxpyProgram() {
        Sora::Compute::KernelBuilder<3, 4> scalar;
        const auto alpha = scalar.AddValueParameter(kFloat);
        const auto x = scalar.AddValueParameter(kFloat);
        const auto y = scalar.AddValueParameter(kFloat);
        auto module = std::move(scalar).Finish(scalar.Ternary(Sora::Compute::OpCode::Fma, alpha, x, y));

        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<4, 3, 4> program{
            domain, module, Sora::Compute::PartitionContract::Elementwise(domain.rank)};
        const auto alphaArgument = program.AddValueArgument(kFloat);
        const auto xArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, domain.rank,
                                                         Sora::Compute::LayoutKind::Contiguous, 64, {1});
        const auto yArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, domain.rank,
                                                         Sora::Compute::LayoutKind::Contiguous, 64, {2});
        const auto output = program.AddBufferArgument(Sora::Compute::AccessMode::Write, kFloat, domain.rank,
                                                      Sora::Compute::LayoutKind::Contiguous, 64, {3});
        program.BindArgument(alphaArgument);
        program.BindArgument(xArgument);
        program.BindArgument(yArgument);
        return std::move(program).Finish(output);
    }

    constexpr auto kSaxpy = MakeSaxpyProgram();

    consteval auto MakeFloat64Program() {
        Sora::Compute::KernelBuilder<2, 3> scalar;
        const auto x = scalar.AddValueParameter(kDouble);
        const auto y = scalar.AddValueParameter(kDouble);
        auto module = std::move(scalar).Finish(scalar.Binary(Sora::Compute::OpCode::Add, x, y));

        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<3, 2, 3> program{
            domain, module, Sora::Compute::PartitionContract::Elementwise(domain.rank)};
        const auto xArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kDouble, domain.rank);
        const auto yArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kDouble, domain.rank);
        const auto output = program.AddBufferArgument(Sora::Compute::AccessMode::Write, kDouble, domain.rank);
        program.BindArgument(xArgument);
        program.BindArgument(yArgument);
        return std::move(program).Finish(output);
    }

    consteval auto MakeVec3Program(Sora::Compute::LayoutKind layout) {
        Sora::Compute::KernelBuilder<2, 3> scalar;
        const auto x = scalar.AddValueParameter(kFloat3);
        const auto y = scalar.AddValueParameter(kFloat3);
        auto module = std::move(scalar).Finish(scalar.Binary(Sora::Compute::OpCode::Add, x, y));

        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<3, 2, 3> program{
            domain, module, Sora::Compute::PartitionContract::Elementwise(domain.rank)};
        const auto xArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat3, domain.rank, layout);
        const auto yArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat3, domain.rank, layout);
        const auto output = program.AddBufferArgument(Sora::Compute::AccessMode::Write, kFloat3, domain.rank, layout);
        program.BindArgument(xArgument);
        program.BindArgument(yArgument);
        return std::move(program).Finish(output);
    }

    consteval auto MakeIndex2DProgram(uint8_t axis,
                                      Sora::Compute::LayoutKind layout = Sora::Compute::LayoutKind::Contiguous) {
        Sora::Compute::KernelBuilder<1, 1> scalar;
        const auto index = scalar.AddValueParameter(kIndex);
        constexpr Sora::Compute::IndexDomain domain{.rank = 2};
        Sora::Compute::KernelProgramBuilder<1, 1, 1> program{
            domain, std::move(scalar).Finish(index), Sora::Compute::PartitionContract::Elementwise(domain.rank)};
        const auto output = program.AddBufferArgument(Sora::Compute::AccessMode::Write, kIndex, domain.rank, layout);
        program.BindLogicalIndex(axis);
        return std::move(program).Finish(output);
    }

    template<Sora::Compute::PortableScalar T>
    consteval auto MakeScalarConstantProgram(T constant) {
        Sora::Compute::KernelBuilder<0, 1> scalar;
        const auto value = scalar.Constant(constant);
        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<1, 0, 1> program{
            domain, std::move(scalar).Finish(value), Sora::Compute::PartitionContract::Elementwise(domain.rank)};
        constexpr Sora::Compute::ValueType type{Sora::Compute::ScalarKindOf<T>(), 1};
        const auto output = program.AddBufferArgument(Sora::Compute::AccessMode::Write, type, domain.rank);
        return std::move(program).Finish(output);
    }

    constexpr auto kFloat64Program = MakeFloat64Program();
    constexpr auto kVec3AosProgram = MakeVec3Program(Sora::Compute::LayoutKind::Contiguous);
    constexpr auto kVec3SoaProgram = MakeVec3Program(Sora::Compute::LayoutKind::StructureOfArrays);
    constexpr auto kColumnIndex2DProgram = MakeIndex2DProgram(1);
    constexpr auto kStridedColumnIndex2DProgram = MakeIndex2DProgram(1, Sora::Compute::LayoutKind::Strided);
    constexpr auto kRowIndex2DProgram = MakeIndex2DProgram(0);
    constexpr auto kBoolConstantProgram = MakeScalarConstantProgram(true);
    constexpr auto kInt8ConstantProgram = MakeScalarConstantProgram(int8_t{-7});
    constexpr auto kInt16ConstantProgram = MakeScalarConstantProgram(int16_t{-300});
    constexpr auto kIntegerConstantProgram = MakeScalarConstantProgram(int32_t{42});

    template<typename Bytes>
    [[nodiscard]] Sora::Compute::BackendCompileRequest MakeRequest(const Sora::Compute::VulkanBackend& backend,
                                                                   Sora::Compute::SemanticHash moduleHash,
                                                                   const Bytes& bytes) noexcept {
        return {.moduleData = bytes.Bytes().data(),
                .moduleSize = bytes.Bytes().size(),
                .moduleHash = moduleHash,
                .target = backend.Target(),
                .artifactKind = Sora::Compute::ArtifactKind::DeviceExecutable};
    }

    struct MaterializedKernel {
        Sora::Compute::Artifact executable{};
        Sora::Compute::Artifact pipeline{};
    };

    struct MaterializationError {
        bool pipeline = false;
        uint64_t code = 0;
        uint64_t detail = 0;
    };

    template<typename Program>
    [[nodiscard]] std::expected<MaterializedKernel, MaterializationError>
    Materialize(const Sora::Compute::VulkanBackend& backend, const Program& program) noexcept {
        const auto bytes = program.Serialize();
        const auto request = MakeRequest(backend, program.Hash(), bytes);
        auto executable = backend.Compiler().Compile(request);
        if (!executable) {
            return std::unexpected(MaterializationError{.code = static_cast<uint64_t>(executable.error().code),
                                                        .detail = executable.error().detail});
        }
        const Sora::Compute::PipelineStateKey state{
            .state = backend.PipelineStateIdentity(),
            .bindingLayout = Sora::Compute::PipelineBindingLayoutIdentity(program.Arguments())};
        auto pipeline = backend.PipelineFactory().CreatePipeline(*executable, state);
        if (!pipeline) {
            return std::unexpected(MaterializationError{.pipeline = true,
                                                        .code = static_cast<uint64_t>(pipeline.error().code),
                                                        .detail = pipeline.error().detail});
        }
        return MaterializedKernel{std::move(*executable), std::move(*pipeline)};
    }

    [[nodiscard]] Sora::Compute::Allocation Allocate(const Sora::Compute::MemoryAllocator& allocator,
                                                     Sora::Compute::MemoryLocation location,
                                                     Sora::Compute::MemoryPropertyMask required, uint64_t sizeBytes) {
        auto allocation = allocator.Allocate({.location = location,
                                              .requiredProperties = required,
                                              .preferredProperties = required,
                                              .sizeBytes = sizeBytes,
                                              .alignment = 64});
        const Sora::Compute::MemoryAllocationError error =
            allocation ? Sora::Compute::MemoryAllocationError{} : allocation.error();
        INFO("allocation code=" << static_cast<uint32_t>(error.code) << ", detail=" << error.detail);
        REQUIRE(allocation);
        return std::move(*allocation);
    }

    [[nodiscard]] const Sora::Compute::TransferEngine& FindTransfer(const Sora::Compute::VulkanBackend& backend,
                                                                    Sora::Compute::MemoryLocation source,
                                                                    Sora::Compute::MemoryLocation destination) {
        for (const Sora::Compute::TransferEngine& engine : backend.TransferEngines()) {
            if (engine.Source() == source && engine.Destination() == destination &&
                engine.Kind() == Sora::Compute::TransferKind::Copy) {
                return engine;
            }
        }
        FAIL("Vulkan copy engine not found");
    }

    template<typename T>
    [[nodiscard]] std::span<T> AsWritable(std::span<std::byte> bytes) {
        REQUIRE(bytes.size() % sizeof(T) == 0);
        return {reinterpret_cast<T*>(bytes.data()), bytes.size() / sizeof(T)};
    }

    template<typename T>
    [[nodiscard]] std::span<const T> AsReadable(std::span<std::byte> bytes) {
        REQUIRE(bytes.size() % sizeof(T) == 0);
        return {reinterpret_cast<const T*>(bytes.data()), bytes.size() / sizeof(T)};
    }

    [[nodiscard]] Sora::Compute::BackendBinding ContiguousBinding(const Sora::Compute::Allocation& allocation,
                                                                  uint32_t binding, Sora::Compute::AccessMode access,
                                                                  uint64_t offset, uint64_t count) {
        return {.allocation = allocation.View(),
                .offset = offset,
                .size = count * sizeof(float),
                .binding = binding,
                .access = access,
                .kind = Sora::Compute::BackendBindingKind::AffineElements,
                .elementType = kFloat,
                .layout = Sora::Compute::LayoutKind::Contiguous,
                .rank = 1,
                .extents = {count},
                .byteStrides = {sizeof(float)},
                .componentStrideBytes = sizeof(float)};
    }

    [[nodiscard]] Sora::Compute::BackendBinding IntegerBinding(const Sora::Compute::Allocation& allocation,
                                                               uint64_t count) {
        constexpr Sora::Compute::ValueType type{Sora::Compute::ScalarKind::Int32, 1};
        return {.allocation = allocation.View(),
                .size = count * sizeof(int32_t),
                .binding = 0,
                .access = Sora::Compute::AccessMode::Write,
                .kind = Sora::Compute::BackendBindingKind::AffineElements,
                .elementType = type,
                .layout = Sora::Compute::LayoutKind::Contiguous,
                .rank = 1,
                .extents = {count},
                .byteStrides = {sizeof(int32_t)},
                .componentStrideBytes = sizeof(int32_t)};
    }

} // namespace

TEST_CASE("Vulkan queue forms a timeline signal-only join without a command buffer",
          "[Sora][Compute][VulkanBackend][Event]") {
    auto created = Sora::Compute::VulkanBackend::Create();
    if (!created) {
        SKIP("No compatible Vulkan compute device");
    }
    auto backend = std::move(*created);

    auto first = backend.ExecutionQueue().Submit({});
    auto second = backend.ExecutionQueue().Submit({});
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    const std::array waits{first->View(), second->View()};
    const Sora::Compute::BackendSubmitRequest joinRequest{.waits = waits.data(),
                                                          .waitCount = static_cast<uint32_t>(waits.size())};
    auto joined = backend.ExecutionQueue().Submit(joinRequest);

    REQUIRE(joined.has_value());
    const Sora::Compute::EventObservation observation = joined->WaitUntilTerminal();
    REQUIRE(observation.status == Sora::Compute::EventStatus::Completed);
    REQUIRE(observation.executionNanoseconds == 0);
    REQUIRE(first->Query().status == Sora::Compute::EventStatus::Completed);
    REQUIRE(second->Query().status == Sora::Compute::EventStatus::Completed);
    REQUIRE(backend.Snapshot().observationSequence >= 3);
}

TEST_CASE("Vulkan backend executes SAXPY through portable allocation, transfer, queue, and timeline APIs",
          "[Sora][Compute][VulkanBackend][EndToEnd]") {
    constexpr uint64_t count = 4096;
    constexpr uint64_t dataBytes = count * sizeof(float);
    constexpr uint64_t deviceOffset = 64;
    constexpr uint64_t deviceBytes = deviceOffset + dataBytes;
    constexpr float alpha = 1.75F;

    auto created = Sora::Compute::VulkanBackend::Create();
    if (!created) {
        SKIP("No compatible Slang/Vulkan compute device");
    }
    auto backend = std::move(*created);
    REQUIRE(backend.Device().features.Contains(Sora::Compute::DeviceFeature::TimelineEvents));
    REQUIRE(backend.Device().features.Contains(Sora::Compute::DeviceFeature::AsynchronousCopy));
    REQUIRE(backend.Snapshot().state == Sora::Compute::DeviceState::Available);

    constexpr auto hostProperties =
        Sora::Compute::MemoryProperty::HostVisible | Sora::Compute::MemoryProperty::HostCoherent;
    constexpr Sora::Compute::MemoryPropertyMask deviceProperties{Sora::Compute::MemoryProperty::DeviceLocal};
    auto xHost = Allocate(backend.HostAllocator(), backend.HostLocation(), hostProperties, dataBytes);
    auto yHost = Allocate(backend.HostAllocator(), backend.HostLocation(), hostProperties, dataBytes);
    auto outputHost = Allocate(backend.HostAllocator(), backend.HostLocation(), hostProperties, dataBytes);
    auto xDevice = Allocate(backend.DeviceAllocator(), backend.DeviceLocation(), deviceProperties, deviceBytes);
    auto yDevice = Allocate(backend.DeviceAllocator(), backend.DeviceLocation(), deviceProperties, deviceBytes);
    auto outputDevice = Allocate(backend.DeviceAllocator(), backend.DeviceLocation(), deviceProperties, deviceBytes);

    auto xBytes = backend.Map(xHost);
    auto yBytes = backend.Map(yHost);
    REQUIRE(xBytes);
    REQUIRE(yBytes);
    auto x = AsWritable<float>(*xBytes);
    auto y = AsWritable<float>(*yBytes);
    for (uint64_t index = 0; index < count; ++index) {
        x[index] = static_cast<float>(index) * 0.25F;
        y[index] = 1000.0F - static_cast<float>(index) * 0.125F;
    }

    const auto& upload = FindTransfer(backend, backend.HostLocation(), backend.DeviceLocation());
    const std::array uploadRegions{Sora::Compute::TransferRegion{.targetOffset = deviceOffset, .sizeBytes = dataBytes}};
    auto uploadedX = upload.Submit(xHost, xDevice, uploadRegions);
    auto uploadedY = upload.Submit(yHost, yDevice, uploadRegions);
    REQUIRE(uploadedX);
    REQUIRE(uploadedY);

    auto materialized = Materialize(backend, kSaxpy);
    REQUIRE(materialized);
    const std::array bindings{
        ContiguousBinding(xDevice, 1, Sora::Compute::AccessMode::Read, deviceOffset, count),
        ContiguousBinding(yDevice, 2, Sora::Compute::AccessMode::Read, deviceOffset, count),
        ContiguousBinding(outputDevice, 3, Sora::Compute::AccessMode::Write, deviceOffset, count),
    };
    const auto alphaBytes = std::bit_cast<std::array<std::byte, sizeof(float)>>(alpha);
    const Sora::Compute::BackendScalarBinding scalar{
        .dataOffset = 0, .size = sizeof(float), .binding = 0, .type = kFloat};
    const Sora::Compute::BackendCommand command{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                                                .bindingCount = bindings.size(),
                                                .scalarBindingCount = 1,
                                                .artifact = materialized->pipeline.View(),
                                                .workgroupCount = {static_cast<uint32_t>(count / 64), 1, 1}};
    const auto makeBatch = [&](std::vector<Sora::Compute::Event> waits) {
        return Sora::Compute::ExecutableBatch::Create({
            .queue = backend.ExecutionQueue(),
            .commands = {command},
            .bindings = std::vector<Sora::Compute::BackendBinding>{bindings.begin(), bindings.end()},
            .scalarData = std::vector<std::byte>{alphaBytes.begin(), alphaBytes.end()},
            .scalarBindings = {scalar},
            .artifacts = {materialized->pipeline},
            .allocations = {xDevice, yDevice, outputDevice},
            .waits = std::move(waits),
        });
    };
    auto batch = makeBatch({uploadedX->Completion(), uploadedY->Completion()});
    REQUIRE(batch);
    exec::static_thread_pool pool{1};
    auto completed = stdexec::sync_wait(Sora::Compute::SubmitOn(pool.get_scheduler(), std::move(*batch)));
    REQUIRE(completed);
    const Sora::Compute::EventObservation firstObservation = std::get<0>(*completed);
    REQUIRE(firstObservation.status == Sora::Compute::EventStatus::Completed);
    const Sora::Compute::QueueView queue = backend.ExecutionQueue().View();
    REQUIRE((firstObservation.executionNanoseconds != 0) == queue.reportsExecutionNanoseconds);
    REQUIRE(uploadedX->Completion().Query().status == Sora::Compute::EventStatus::Completed);
    REQUIRE(uploadedY->Completion().Query().status == Sora::Compute::EventStatus::Completed);

    Sora::Compute::PerformanceModel performance;
    const Sora::Compute::KernelCalibrationKey calibrationKey{
        .kernel = kSaxpy.Hash(), .target = backend.Target(), .specialization = {}, .queue = queue.id};
    auto observedBatch = makeBatch({});
    REQUIRE(observedBatch);
    Sora::Compute::PhysicalSegment segment{
        std::move(*observedBatch),
        {.key = calibrationKey, .workItemCount = count, .deterministic = true},
        performance.KernelObserver(),
    };
    auto observedSubmission = std::move(segment).Submit();
    REQUIRE(observedSubmission);
    const Sora::Compute::EventObservation observed = observedSubmission->WaitUntilTerminal();
    REQUIRE(observed.status == Sora::Compute::EventStatus::Completed);
    REQUIRE((observed.executionNanoseconds != 0) == queue.reportsExecutionNanoseconds);
    const auto estimate = performance.Snapshot()->Estimate(calibrationKey, count);
    if (queue.reportsExecutionNanoseconds) {
        REQUIRE(estimate.has_value());
        REQUIRE(estimate->sampleCount == 1);
    } else {
        REQUIRE_FALSE(estimate.has_value());
    }

    const auto& download = FindTransfer(backend, backend.DeviceLocation(), backend.HostLocation());
    const std::array downloadRegions{
        Sora::Compute::TransferRegion{.sourceOffset = deviceOffset, .sizeBytes = dataBytes}};
    auto downloaded = download.Submit(outputDevice, outputHost, downloadRegions);
    REQUIRE(downloaded);
    REQUIRE(downloaded->WaitUntilTerminal().status == Sora::Compute::EventStatus::Completed);

    auto outputBytes = backend.Map(outputHost);
    REQUIRE(outputBytes);
    const auto output = AsReadable<float>(*outputBytes);
    for (uint64_t index = 0; index < count; ++index) {
        REQUIRE(std::abs(output[index] - std::fma(alpha, x[index], y[index])) <= 1.0e-5F);
    }
    REQUIRE(backend.Snapshot().generation == 1);
    REQUIRE(backend.Snapshot().observationSequence >= 4);
}

TEST_CASE("Vulkan backend preserves global logical indices for shard-local dispatches",
          "[Sora][Compute][VulkanBackend][Partition]") {
    constexpr uint64_t globalRows = 17;
    constexpr uint64_t columns = 19;
    constexpr uint64_t originRow = 5;
    constexpr uint64_t shardRows = 4;
    constexpr uint64_t shardCount = shardRows * columns;
    constexpr uint64_t sizeBytes = shardCount * sizeof(uint64_t);
    auto created = Sora::Compute::VulkanBackend::Create();
    if (!created) {
        SKIP("No compatible Slang/Vulkan compute device");
    }
    auto backend = std::move(*created);
    auto materialized = Materialize(backend, kRowIndex2DProgram);
    REQUIRE(materialized);
    constexpr Sora::Compute::MemoryPropertyMask deviceProperties{Sora::Compute::MemoryProperty::DeviceLocal};
    constexpr auto hostProperties =
        Sora::Compute::MemoryProperty::HostVisible | Sora::Compute::MemoryProperty::HostCoherent;
    auto device = Allocate(backend.DeviceAllocator(), backend.DeviceLocation(), deviceProperties, sizeBytes);
    auto host = Allocate(backend.HostAllocator(), backend.HostLocation(), hostProperties, sizeBytes);
    const Sora::Compute::BackendBinding binding{.allocation = device.View(),
                                                .size = sizeBytes,
                                                .binding = 0,
                                                .access = Sora::Compute::AccessMode::Write,
                                                .kind = Sora::Compute::BackendBindingKind::AffineElements,
                                                .elementType = kIndex,
                                                .layout = Sora::Compute::LayoutKind::Contiguous,
                                                .rank = 2,
                                                .extents = {shardRows, columns},
                                                .byteStrides = {columns * sizeof(uint64_t), sizeof(uint64_t)},
                                                .componentStrideBytes = sizeof(uint64_t)};
    const Sora::Compute::BackendCommand command{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                                                .bindingCount = 1,
                                                .artifact = materialized->pipeline.View(),
                                                .dispatchOrigin = {originRow, 0, 0},
                                                .dispatchExtents = {shardRows, columns, 0},
                                                .globalExtents = {globalRows, columns, 0},
                                                .workgroupCount = {1, 3, 1}};
    const Sora::Compute::BackendSubmitRequest request{
        .commands = &command, .commandCount = 1, .bindings = &binding, .bindingCount = 1};
    auto submitted = backend.ExecutionQueue().Submit(request);
    REQUIRE(submitted);
    REQUIRE(submitted->WaitUntilTerminal().status == Sora::Compute::EventStatus::Completed);
    const auto& download = FindTransfer(backend, backend.DeviceLocation(), backend.HostLocation());
    const std::array downloadRegions{Sora::Compute::TransferRegion{.sizeBytes = sizeBytes}};
    auto downloaded = download.Submit(device, host, downloadRegions);
    REQUIRE(downloaded);
    REQUIRE(downloaded->WaitUntilTerminal().status == Sora::Compute::EventStatus::Completed);
    auto mapped = backend.Map(host);
    REQUIRE(mapped);
    const auto output = AsReadable<uint64_t>(*mapped);
    for (uint64_t row = 0; row < shardRows; ++row) {
        for (uint64_t column = 0; column < columns; ++column) {
            REQUIRE(output[row * columns + column] == originRow + row);
        }
    }
}

TEST_CASE("Vulkan backend preserves axis-one origins for strided column shards",
          "[Sora][Compute][VulkanBackend][Partition][Strided]") {
    constexpr uint64_t rows = 7;
    constexpr uint64_t globalColumns = 19;
    constexpr uint64_t originColumn = 5;
    constexpr uint64_t shardColumns = 4;
    constexpr uint64_t storageElements = (rows - 1) * globalColumns + shardColumns;
    constexpr uint64_t sizeBytes = storageElements * sizeof(uint64_t);
    auto created = Sora::Compute::VulkanBackend::Create();
    if (!created) {
        SKIP("No compatible Slang/Vulkan compute device");
    }
    auto backend = std::move(*created);
    auto materialized = Materialize(backend, kStridedColumnIndex2DProgram);
    REQUIRE(materialized);
    constexpr Sora::Compute::MemoryPropertyMask deviceProperties{Sora::Compute::MemoryProperty::DeviceLocal};
    constexpr auto hostProperties =
        Sora::Compute::MemoryProperty::HostVisible | Sora::Compute::MemoryProperty::HostCoherent;
    auto device = Allocate(backend.DeviceAllocator(), backend.DeviceLocation(), deviceProperties, sizeBytes);
    auto host = Allocate(backend.HostAllocator(), backend.HostLocation(), hostProperties, sizeBytes);
    const Sora::Compute::BackendBinding binding{.allocation = device.View(),
                                                .size = sizeBytes,
                                                .binding = 0,
                                                .access = Sora::Compute::AccessMode::Write,
                                                .kind = Sora::Compute::BackendBindingKind::AffineElements,
                                                .elementType = kIndex,
                                                .layout = Sora::Compute::LayoutKind::Strided,
                                                .rank = 2,
                                                .extents = {rows, shardColumns},
                                                .byteStrides = {globalColumns * sizeof(uint64_t), sizeof(uint64_t)},
                                                .componentStrideBytes = sizeof(uint64_t)};
    const Sora::Compute::BackendCommand command{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                                                .bindingCount = 1,
                                                .artifact = materialized->pipeline.View(),
                                                .dispatchOrigin = {0, originColumn, 0},
                                                .dispatchExtents = {rows, shardColumns, 0},
                                                .globalExtents = {rows, globalColumns, 0},
                                                .workgroupCount = {1, 1, 1}};
    const Sora::Compute::BackendSubmitRequest request{
        .commands = &command, .commandCount = 1, .bindings = &binding, .bindingCount = 1};
    auto submitted = backend.ExecutionQueue().Submit(request);
    REQUIRE(submitted);
    REQUIRE(submitted->WaitUntilTerminal().status == Sora::Compute::EventStatus::Completed);
    const auto& download = FindTransfer(backend, backend.DeviceLocation(), backend.HostLocation());
    const std::array downloadRegions{Sora::Compute::TransferRegion{.sizeBytes = sizeBytes}};
    auto downloaded = download.Submit(device, host, downloadRegions);
    REQUIRE(downloaded);
    REQUIRE(downloaded->WaitUntilTerminal().status == Sora::Compute::EventStatus::Completed);
    auto mapped = backend.Map(host);
    REQUIRE(mapped);
    const auto output = AsReadable<uint64_t>(*mapped);
    for (uint64_t row = 0; row < rows; ++row) {
        for (uint64_t column = 0; column < shardColumns; ++column) {
            REQUIRE(output[row * globalColumns + column] == originColumn + column);
        }
    }
}

TEST_CASE("Vulkan backend orders dependent commands within one batch", "[Sora][Compute][VulkanBackend][Execution]") {
    constexpr uint64_t count = 4096;
    constexpr uint64_t dataBytes = count * sizeof(int32_t);
    auto created = Sora::Compute::VulkanBackend::Create();
    if (!created) {
        SKIP("No compatible Slang/Vulkan compute device");
    }
    auto backend = std::move(*created);
    auto materialized = Materialize(backend, kIntegerConstantProgram);
    REQUIRE(materialized);

    constexpr Sora::Compute::MemoryPropertyMask deviceProperties{Sora::Compute::MemoryProperty::DeviceLocal};
    constexpr auto hostProperties =
        Sora::Compute::MemoryProperty::HostVisible | Sora::Compute::MemoryProperty::HostCoherent;
    auto produced = Allocate(backend.DeviceAllocator(), backend.DeviceLocation(), deviceProperties, dataBytes);
    auto copied = Allocate(backend.DeviceAllocator(), backend.DeviceLocation(), deviceProperties, dataBytes);
    auto host = Allocate(backend.HostAllocator(), backend.HostLocation(), hostProperties, dataBytes);
    const Sora::Compute::BackendBinding binding = IntegerBinding(produced, count);
    const std::array commands{
        Sora::Compute::BackendCommand{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                                      .bindingCount = 1,
                                      .artifact = materialized->pipeline.View(),
                                      .workgroupCount = {static_cast<uint32_t>(count / 64), 1, 1}},
        Sora::Compute::BackendCommand{.kind = Sora::Compute::BackendCommandKind::Copy,
                                      .source = produced.View(),
                                      .target = copied.View(),
                                      .sizeBytes = dataBytes},
    };
    Sora::Compute::ExecutableBatchStorage storage{.queue = backend.ExecutionQueue(),
                                                  .commands = {commands.begin(), commands.end()},
                                                  .bindings = {binding},
                                                  .artifacts = {materialized->pipeline},
                                                  .allocations = {produced, copied}};
    auto batch = Sora::Compute::ExecutableBatch::Create(std::move(storage));
    REQUIRE(batch);
    auto submitted = std::move(*batch).Submit();
    REQUIRE(submitted);

    const auto& download = FindTransfer(backend, backend.DeviceLocation(), backend.HostLocation());
    const std::array downloadRegions{Sora::Compute::TransferRegion{.sizeBytes = dataBytes}};
    auto downloaded = download.Submit(copied, host, downloadRegions, {submitted->Completion()});
    REQUIRE(downloaded);
    REQUIRE(downloaded->WaitUntilTerminal().status == Sora::Compute::EventStatus::Completed);
    auto mapped = backend.Map(host);
    REQUIRE(mapped);
    for (const int32_t value : AsReadable<int32_t>(*mapped)) {
        REQUIRE(value == 42);
    }
}

TEST_CASE("Vulkan queue supports concurrent submissions with isolated command state",
          "[Sora][Compute][VulkanBackend][Concurrency]") {
    constexpr size_t taskCount = 4;
    constexpr uint64_t count = 4096;
    constexpr uint64_t dataBytes = count * sizeof(int32_t);
    auto created = Sora::Compute::VulkanBackend::Create();
    if (!created) {
        SKIP("No compatible Slang/Vulkan compute device");
    }
    auto backend = std::move(*created);
    auto materialized = Materialize(backend, kIntegerConstantProgram);
    REQUIRE(materialized);

    constexpr Sora::Compute::MemoryPropertyMask deviceProperties{Sora::Compute::MemoryProperty::DeviceLocal};
    constexpr auto hostProperties =
        Sora::Compute::MemoryProperty::HostVisible | Sora::Compute::MemoryProperty::HostCoherent;
    std::array<Sora::Compute::Allocation, taskCount> deviceAllocations;
    std::array<Sora::Compute::Allocation, taskCount> hostAllocations;
    std::array<Sora::Compute::BackendBinding, taskCount> bindings;
    std::array<Sora::Compute::BackendCommand, taskCount> commands;
    for (size_t index = 0; index < taskCount; ++index) {
        deviceAllocations[index] =
            Allocate(backend.DeviceAllocator(), backend.DeviceLocation(), deviceProperties, dataBytes);
        hostAllocations[index] = Allocate(backend.HostAllocator(), backend.HostLocation(), hostProperties, dataBytes);
        bindings[index] = IntegerBinding(deviceAllocations[index], count);
        commands[index] = {.kind = Sora::Compute::BackendCommandKind::Dispatch,
                           .bindingCount = 1,
                           .artifact = materialized->pipeline.View(),
                           .workgroupCount = {static_cast<uint32_t>(count / 64), 1, 1}};
    }

    std::barrier start{static_cast<ptrdiff_t>(taskCount)};
    std::array<std::atomic_bool, taskCount> results{};
    std::array<uint64_t, taskCount> executionNanoseconds{};
    std::array<std::jthread, taskCount> tasks;
    for (size_t index = 0; index < taskCount; ++index) {
        tasks[index] = std::jthread([&backend, &bindings, &commands, &results, &executionNanoseconds, &start, index] {
            start.arrive_and_wait();
            const Sora::Compute::BackendSubmitRequest request{
                .commands = &commands[index], .commandCount = 1, .bindings = &bindings[index], .bindingCount = 1};
            auto submitted = backend.ExecutionQueue().Submit(request);
            const Sora::Compute::EventObservation observation =
                submitted ? submitted->WaitUntilTerminal() : Sora::Compute::EventObservation{};
            executionNanoseconds[index] = observation.executionNanoseconds;
            results[index].store(submitted && observation.status == Sora::Compute::EventStatus::Completed,
                                 std::memory_order_relaxed);
        });
    }
    for (auto& task : tasks) {
        task.join();
    }
    for (const auto& result : results) {
        REQUIRE(result.load(std::memory_order_relaxed));
    }
    for (const uint64_t duration : executionNanoseconds) {
        REQUIRE((duration != 0) == backend.ExecutionQueue().View().reportsExecutionNanoseconds);
    }

    const auto& download = FindTransfer(backend, backend.DeviceLocation(), backend.HostLocation());
    for (size_t index = 0; index < taskCount; ++index) {
        const std::array regions{Sora::Compute::TransferRegion{.sizeBytes = dataBytes}};
        auto downloaded = download.Submit(deviceAllocations[index], hostAllocations[index], regions);
        REQUIRE(downloaded);
        REQUIRE(downloaded->WaitUntilTerminal().status == Sora::Compute::EventStatus::Completed);
        auto mapped = backend.Map(hostAllocations[index]);
        REQUIRE(mapped);
        for (const int32_t value : AsReadable<int32_t>(*mapped)) {
            REQUIRE(value == 42);
        }
    }
}

TEST_CASE("Vulkan transfer resources outlive the backend facade", "[Sora][Compute][VulkanBackend][Lifetime]") {
    constexpr uint64_t dataBytes = 4096;
    Sora::Compute::Allocation host;
    Sora::Compute::Allocation device;
    std::vector<Sora::Compute::TransferSubmission> retained;
    {
        auto created = Sora::Compute::VulkanBackend::Create();
        if (!created) {
            SKIP("No compatible Slang/Vulkan compute device");
        }
        auto backend = std::move(*created);
        constexpr auto hostProperties =
            Sora::Compute::MemoryProperty::HostVisible | Sora::Compute::MemoryProperty::HostCoherent;
        constexpr Sora::Compute::MemoryPropertyMask deviceProperties{Sora::Compute::MemoryProperty::DeviceLocal};
        host = Allocate(backend.HostAllocator(), backend.HostLocation(), hostProperties, dataBytes);
        device = Allocate(backend.DeviceAllocator(), backend.DeviceLocation(), deviceProperties, dataBytes);
        const auto& upload = FindTransfer(backend, backend.HostLocation(), backend.DeviceLocation());
        const std::array regions{Sora::Compute::TransferRegion{.sizeBytes = dataBytes}};
        auto submitted = upload.Submit(host, device, regions);
        REQUIRE(submitted);
        retained.push_back(std::move(*submitted));
    }

    REQUIRE(retained.front().WaitUntilTerminal().status == Sora::Compute::EventStatus::Completed);
    retained.clear();
    device = {};
    host = {};
}

TEST_CASE("Vulkan backend preserves service, mapping, and range identities",
          "[Sora][Compute][VulkanBackend][Validation]") {
    auto firstCreated = Sora::Compute::VulkanBackend::Create();
    auto secondCreated = Sora::Compute::VulkanBackend::Create();
    if (!firstCreated || !secondCreated) {
        SKIP("Two compatible Slang/Vulkan services are unavailable");
    }
    auto first = std::move(*firstCreated);
    auto second = std::move(*secondCreated);
    constexpr uint64_t sizeBytes = 4096;
    constexpr auto hostProperties =
        Sora::Compute::MemoryProperty::HostVisible | Sora::Compute::MemoryProperty::HostCoherent;
    constexpr Sora::Compute::MemoryPropertyMask deviceProperties{Sora::Compute::MemoryProperty::DeviceLocal};
    auto host = Allocate(first.HostAllocator(), first.HostLocation(), hostProperties, sizeBytes);
    auto device = Allocate(first.DeviceAllocator(), first.DeviceLocation(), deviceProperties, sizeBytes);

    auto foreign = second.Map(host);
    REQUIRE_FALSE(foreign);
    REQUIRE(foreign.error().code == Sora::Compute::VulkanBackendErrorCode::ForeignAllocation);
    auto deviceLocal = first.Map(device);
    REQUIRE_FALSE(deviceLocal);
    REQUIRE(deviceLocal.error().code == Sora::Compute::VulkanBackendErrorCode::NotHostVisible);
    auto outside = first.Map(host, sizeBytes + 1, 1);
    REQUIRE_FALSE(outside);
    REQUIRE(outside.error().code == Sora::Compute::VulkanBackendErrorCode::RangeOutOfBounds);

    auto materialized = Materialize(first, kIntegerConstantProgram);
    REQUIRE(materialized);
    const auto binding = ContiguousBinding(device, 0, Sora::Compute::AccessMode::Write, 0, sizeBytes / sizeof(float));
    const Sora::Compute::BackendCommand command{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                                                .bindingCount = 1,
                                                .artifact = materialized->pipeline.View(),
                                                .workgroupCount = {1, 1, 1}};
    const Sora::Compute::BackendSubmitRequest request{
        .commands = &command, .commandCount = 1, .bindings = &binding, .bindingCount = 1};
    auto foreignSubmission = second.ExecutionQueue().Submit(request);
    REQUIRE_FALSE(foreignSubmission);
    REQUIRE(foreignSubmission.error().code == Sora::Compute::SubmissionErrorCode::InvalidRequest);
}

TEST_CASE("Slang and Vulkan materialize exact executable and pipeline artifacts") {
    auto backend = Sora::Compute::VulkanBackend::Create();
    if (!backend) {
        SKIP("No compatible Slang/Vulkan compute device: code=" << static_cast<uint32_t>(backend.error().code)
                                                                << ", detail=" << backend.error().detail);
    }

    REQUIRE(backend->Device().backend == Sora::Compute::BackendKind::Vulkan);
    REQUIRE(backend->Device().id == backend->Target().device);
    REQUIRE(backend->Target().PersistentCacheable());
    REQUIRE(backend->Compiler().Backend() == Sora::Compute::BackendKind::Vulkan);
    REQUIRE(backend->PipelineFactory().Device() == backend->Device().id);

    const auto programBytes = kSaxpy.Serialize();
    const auto request = MakeRequest(*backend, kSaxpy.Hash(), programBytes);
    auto executable = backend->Compiler().Compile(request);
    INFO("compile code=" << static_cast<uint32_t>(executable ? Sora::Compute::CompileErrorCode::None
                                                             : executable.error().code)
                         << ", detail=" << (executable ? 0 : executable.error().detail));
    REQUIRE(executable);
    REQUIRE(executable->Key().Kind() == Sora::Compute::ArtifactKind::DeviceExecutable);
    REQUIRE(executable->Key() == request.ExpectedKey());

    const Sora::Compute::PipelineStateKey pipelineState{
        .state = backend->PipelineStateIdentity(),
        .bindingLayout = Sora::Compute::PipelineBindingLayoutIdentity(kSaxpy.Arguments()),
    };
    auto pipeline = backend->PipelineFactory().CreatePipeline(*executable, pipelineState);
    INFO("pipeline code=" << static_cast<uint32_t>(pipeline ? Sora::Compute::PipelineErrorCode::None
                                                            : pipeline.error().code)
                          << ", detail=" << (pipeline ? 0 : pipeline.error().detail));
    REQUIRE(pipeline);
    REQUIRE(pipeline->Key().Kind() == Sora::Compute::ArtifactKind::DevicePipeline);
    REQUIRE(pipeline->Key() == backend->PipelineFactory().ExpectedKey(*executable, pipelineState));

    auto executableBytes = backend->Compiler().Export(*executable);
    REQUIRE(executableBytes);
    REQUIRE_FALSE(executableBytes->empty());
    auto importedExecutable = backend->Compiler().Import(request, *executableBytes);
    REQUIRE(importedExecutable);
    REQUIRE(importedExecutable->Key() == executable->Key());

    auto pipelineCache = backend->PipelineFactory().Export(*pipeline);
    REQUIRE(pipelineCache);
    REQUIRE_FALSE(pipelineCache->empty());
    auto importedPipeline = backend->PipelineFactory().Import(*importedExecutable, pipelineState, *pipelineCache);
    INFO("pipeline import code=" << static_cast<uint32_t>(importedPipeline ? Sora::Compute::PipelineErrorCode::None
                                                                           : importedPipeline.error().code)
                                 << ", detail=" << (importedPipeline ? 0 : importedPipeline.error().detail));
    REQUIRE(importedPipeline);
    REQUIRE(importedPipeline->Key() == pipeline->Key());
}

TEST_CASE("Vulkan backend rejects corrupt artifacts and schema identity "
          "mismatches") {
    auto backend = Sora::Compute::VulkanBackend::Create();
    if (!backend) {
        SKIP("No compatible Slang/Vulkan compute device");
    }
    const auto programBytes = kSaxpy.Serialize();
    const auto request = MakeRequest(*backend, kSaxpy.Hash(), programBytes);
    auto executable = backend->Compiler().Compile(request);
    REQUIRE(executable);

    const Sora::Compute::PipelineStateKey validState{
        .state = backend->PipelineStateIdentity(),
        .bindingLayout = Sora::Compute::PipelineBindingLayoutIdentity(kSaxpy.Arguments()),
    };
    const Sora::Compute::PipelineStateKey wrongLayout{
        .state = backend->PipelineStateIdentity(),
        .bindingLayout = {validState.bindingLayout.low ^ 1U, validState.bindingLayout.high},
    };
    auto rejected = backend->PipelineFactory().CreatePipeline(*executable, wrongLayout);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == Sora::Compute::PipelineErrorCode::UnsupportedExecutable);

    auto bytes = backend->Compiler().Export(*executable);
    REQUIRE(bytes);
    (*bytes)[bytes->size() / 2] ^= std::byte{0x80};
    auto corrupt = backend->Compiler().Import(request, *bytes);
    REQUIRE_FALSE(corrupt);
    REQUIRE(corrupt.error().code == Sora::Compute::CompileErrorCode::ArtifactImportFailed);

    auto pipeline = backend->PipelineFactory().CreatePipeline(*executable, validState);
    REQUIRE(pipeline);
    auto pipelineBytes = backend->PipelineFactory().Export(*pipeline);
    REQUIRE(pipelineBytes);
    (*pipelineBytes)[pipelineBytes->size() - 1] ^= std::byte{0x40};
    auto corruptPipeline = backend->PipelineFactory().Import(*executable, validState, *pipelineBytes);
    REQUIRE_FALSE(corruptPipeline);
    REQUIRE(corruptPipeline.error().code == Sora::Compute::PipelineErrorCode::ArtifactImportFailed);
}

TEST_CASE("Vulkan backend materializes supported scalar, vector, layout, and "
          "domain forms") {
    auto backend = Sora::Compute::VulkanBackend::Create();
    if (!backend) {
        SKIP("No compatible Slang/Vulkan compute device");
    }

    const auto verify = [&backend](const auto& program) {
        auto materialized = Materialize(*backend, program);
        INFO("stage=" << (materialized ? 0 : materialized.error().pipeline));
        INFO("code=" << (materialized ? 0 : materialized.error().code));
        INFO("detail=" << (materialized ? 0 : materialized.error().detail));
        REQUIRE(materialized);
        REQUIRE(materialized->executable.Key().Kind() == Sora::Compute::ArtifactKind::DeviceExecutable);
        REQUIRE(materialized->pipeline.Key().Kind() == Sora::Compute::ArtifactKind::DevicePipeline);
    };

    verify(kVec3AosProgram);
    verify(kVec3SoaProgram);
    verify(kIntegerConstantProgram);
    if (backend->Device().features.Contains(Sora::Compute::DeviceFeature::Int8)) {
        verify(kBoolConstantProgram);
        verify(kInt8ConstantProgram);
    }
    if (backend->Device().features.Contains(Sora::Compute::DeviceFeature::Int16)) {
        verify(kInt16ConstantProgram);
    }
    if (backend->Device().features.Contains(Sora::Compute::DeviceFeature::Float64)) {
        verify(kFloat64Program);
    }
    if (backend->Device().features.Contains(Sora::Compute::DeviceFeature::Int64)) {
        verify(kColumnIndex2DProgram);
    }
}

TEST_CASE("Vulkan artifact envelopes reject valid payloads from a different "
          "semantic key") {
    auto backend = Sora::Compute::VulkanBackend::Create();
    if (!backend) {
        SKIP("No compatible Slang/Vulkan compute device");
    }

    const auto saxpyBytes = kSaxpy.Serialize();
    const auto saxpyRequest = MakeRequest(*backend, kSaxpy.Hash(), saxpyBytes);
    auto saxpyExecutable = backend->Compiler().Compile(saxpyRequest);
    REQUIRE(saxpyExecutable);

    const auto constantBytes = kIntegerConstantProgram.Serialize();
    const auto constantRequest = MakeRequest(*backend, kIntegerConstantProgram.Hash(), constantBytes);
    auto constantExecutable = backend->Compiler().Compile(constantRequest);
    REQUIRE(constantExecutable);
    auto constantArtifact = backend->Compiler().Export(*constantExecutable);
    REQUIRE(constantArtifact);

    auto substitutedExecutable = backend->Compiler().Import(saxpyRequest, *constantArtifact);
    REQUIRE_FALSE(substitutedExecutable);
    REQUIRE(substitutedExecutable.error().code == Sora::Compute::CompileErrorCode::ArtifactImportFailed);

    const Sora::Compute::PipelineStateKey saxpyState{
        .state = backend->PipelineStateIdentity(),
        .bindingLayout = Sora::Compute::PipelineBindingLayoutIdentity(kSaxpy.Arguments())};
    auto saxpyPipeline = backend->PipelineFactory().CreatePipeline(*saxpyExecutable, saxpyState);
    REQUIRE(saxpyPipeline);
    auto saxpyCache = backend->PipelineFactory().Export(*saxpyPipeline);
    REQUIRE(saxpyCache);

    const Sora::Compute::PipelineStateKey constantState{
        .state = backend->PipelineStateIdentity(),
        .bindingLayout = Sora::Compute::PipelineBindingLayoutIdentity(kIntegerConstantProgram.Arguments())};
    auto substitutedPipeline = backend->PipelineFactory().Import(*constantExecutable, constantState, *saxpyCache);
    REQUIRE_FALSE(substitutedPipeline);
    REQUIRE(substitutedPipeline.error().code == Sora::Compute::PipelineErrorCode::ArtifactImportFailed);
}

TEST_CASE("Vulkan backend rejects an excessive compiler worker count") {
    auto invalid = Sora::Compute::VulkanBackend::Create({.compilerWorkerCount = 17});
    REQUIRE_FALSE(invalid.has_value());
    REQUIRE(invalid.error().code == Sora::Compute::VulkanBackendErrorCode::InvalidConfiguration);
    REQUIRE(invalid.error().detail == 17);

    auto invalidSet = Sora::Compute::VulkanBackend::CreateAll({.compilerWorkerCount = 17});
    REQUIRE_FALSE(invalidSet.has_value());
    REQUIRE(invalidSet.error().code == Sora::Compute::VulkanBackendErrorCode::InvalidConfiguration);
    REQUIRE(invalidSet.error().detail == 17);
}

TEST_CASE("Vulkan backend set constructs every compatible device in stable identity order") {
    using namespace Sora::Compute;

    static_assert(RuntimeBackendRange<std::vector<VulkanBackend>>);
    auto backends = VulkanBackend::CreateAll({.compilerWorkerCount = 1});
    if (!backends) {
        SKIP("No compatible Slang/Vulkan compute device");
    }
    REQUIRE_FALSE(backends->empty());
    REQUIRE(std::ranges::is_sorted(*backends, {}, [](const VulkanBackend& backend) { return backend.Device().id; }));
    REQUIRE(std::ranges::adjacent_find(
                *backends, {}, [](const VulkanBackend& backend) { return backend.Device().id; }) == backends->end());

    size_t transferCount = 0;
    for (const VulkanBackend& backend : *backends) {
        REQUIRE(backend.Target().device == backend.Device().id);
        REQUIRE(backend.Target().identityStability == TargetIdentityStability::MachineStable);
        REQUIRE(Materialize(backend, kSaxpy));
        auto hostAllocation = backend.HostAllocator().Allocate(
            {.location = backend.HostLocation(),
             .requiredProperties = MemoryProperty::HostVisible | MemoryProperty::HostCoherent,
             .sizeBytes = 256,
             .alignment = 64});
        REQUIRE(hostAllocation.has_value());
        AllocationView forged = hostAllocation->View();
        forged.memory.low ^= 1;
        const auto forgedMapping = backend.HostAccessor().Read(forged, 0, 64);
        REQUIRE_FALSE(forgedMapping.has_value());
        REQUIRE(forgedMapping.error().code == HostMemoryErrorCode::ForeignAllocation);
        transferCount += backend.TransferEngines().size();
    }

    auto contribution = MakeRuntimeDiscoveryContribution(RuntimeProviderId{0x564B, 1}, *backends);
    REQUIRE(contribution.has_value());
    REQUIRE(contribution->resources.deviceObservers.size() == backends->size());
    REQUIRE(contribution->resources.queues.size() == backends->size());
    REQUIRE(contribution->resources.allocators.size() == backends->size() * 2);
    REQUIRE(contribution->resources.hostMemoryAccessors.size() == backends->size());
    REQUIRE(contribution->resources.transferEngines.size() == transferCount);
    REQUIRE(contribution->resources.compilers.size() == backends->size());
    REQUIRE(contribution->resources.pipelineFactories.size() == backends->size());
    REQUIRE(contribution->resources.pipelineConfigurations.size() == backends->size());
    REQUIRE(contribution->devices.size() == backends->size());

    std::vector<RuntimeDiscoveryContribution> contributions;
    contributions.push_back(std::move(*contribution));
    auto snapshot = RuntimeDiscoverySnapshot::Create(std::move(contributions));
    REQUIRE(snapshot.has_value());
    auto epoch = std::move(*snapshot).BuildEpoch(1);
    REQUIRE(epoch.has_value());
    REQUIRE(epoch->Devices().size() == backends->size());
    REQUIRE(epoch->Resources().Inventory().Transfers().size() ==
            transferCount + backends->size() * (backends->size() - 1U));

    if (backends->size() >= 2) {
        constexpr uint64_t bytes = 4096;
        auto source = (*backends)[0].HostAllocator().Allocate({
            .location = (*backends)[0].HostLocation(),
            .requiredProperties = MemoryProperty::HostVisible | MemoryProperty::HostCoherent,
            .sizeBytes = bytes,
            .alignment = 64,
        });
        auto destination = (*backends)[1].HostAllocator().Allocate({
            .location = (*backends)[1].HostLocation(),
            .requiredProperties = MemoryProperty::HostVisible | MemoryProperty::HostCoherent,
            .sizeBytes = bytes,
            .alignment = 64,
        });
        REQUIRE(source.has_value());
        REQUIRE(destination.has_value());
        auto sourceBytes = (*backends)[0].HostAccessor().Write(*source, 0, bytes);
        auto destinationBytes = (*backends)[1].HostAccessor().Write(*destination, 0, bytes);
        REQUIRE(sourceBytes.has_value());
        REQUIRE(destinationBytes.has_value());
        for (size_t index = 0; index < sourceBytes->size(); ++index) {
            (*sourceBytes)[index] = std::byte{static_cast<uint8_t>(index * 29U + 3U)};
        }
        std::ranges::fill(*destinationBytes, std::byte{0});
        const auto descriptor = std::ranges::find_if(
            epoch->Resources().Inventory().Transfers(), [&](const PhysicalTransferDescriptor& transfer) {
                return transfer.kind == TransferKind::HostBridge &&
                       transfer.sourceService == (*backends)[0].Observer().Service() &&
                       transfer.service == (*backends)[1].Observer().Service();
            });
        REQUIRE(descriptor != epoch->Resources().Inventory().Transfers().end());
        const TransferEngine* bridge = epoch->Resources().FindTransferEngine(descriptor->service, descriptor->engine);
        REQUIRE(bridge != nullptr);
        const std::array regions{Sora::Compute::TransferRegion{.sizeBytes = bytes}};
        auto copied = bridge->Submit(*source, *destination, regions);
        REQUIRE(copied.has_value());
        REQUIRE(copied->WaitUntilTerminal().status == EventStatus::Completed);
        REQUIRE(std::ranges::equal(*sourceBytes, *destinationBytes));
    }
}

TEST_CASE("Vulkan backend supports one compiler worker") {
    auto backend = Sora::Compute::VulkanBackend::Create({.compilerWorkerCount = 1});
    if (!backend) {
        SKIP("No compatible Slang/Vulkan compute device");
    }
    REQUIRE(Materialize(*backend, kSaxpy));
}

TEST_CASE("Vulkan backend supports two compiler workers") {
    auto backend = Sora::Compute::VulkanBackend::Create({.compilerWorkerCount = 2});
    if (!backend) {
        SKIP("No compatible Slang/Vulkan compute device");
    }
    REQUIRE(Materialize(*backend, kSaxpy));
}

TEST_CASE("Vulkan backend supports concurrent host materialization and cache access") {
    auto backend = Sora::Compute::VulkanBackend::Create({.compilerWorkerCount = 2});
    if (!backend) {
        SKIP("No compatible Slang/Vulkan compute device");
    }

    std::array<std::atomic_bool, 4> results{};
    std::array<std::jthread, 4> tasks;
    for (size_t index = 0; index < tasks.size(); ++index) {
        tasks[index] = std::jthread([&backend, &results, index] {
            auto materialized = Materialize(*backend, kSaxpy);
            if (!materialized) {
                return;
            }
            auto cache = backend->PipelineFactory().Export(materialized->pipeline);
            results[index].store(cache && !cache->empty(), std::memory_order_relaxed);
        });
    }
    for (auto& task : tasks) {
        task.join();
    }
    for (const auto& result : results) {
        REQUIRE(result.load(std::memory_order_relaxed));
    }
}

TEST_CASE("Vulkan artifacts retain native state independently of backend and "
          "executable order") {
    Sora::Compute::Artifact executable;
    Sora::Compute::Artifact pipeline;
    {
        auto backend = Sora::Compute::VulkanBackend::Create();
        if (!backend) {
            SKIP("No compatible Slang/Vulkan compute device");
        }
        auto materialized = Materialize(*backend, kSaxpy);
        REQUIRE(materialized);
        executable = std::move(materialized->executable);
        pipeline = std::move(materialized->pipeline);
    }

    REQUIRE(executable);
    REQUIRE(pipeline);
    executable = {};
    REQUIRE(pipeline);
    pipeline = {};
}

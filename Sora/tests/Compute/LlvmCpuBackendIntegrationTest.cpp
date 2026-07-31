/**
 * @file LlvmCpuBackendIntegrationTest.cpp
 * @brief End-to-end integration tests for the LLVM native provider through CpuBackend.
 * @ingroup Compute
 */

#include <Sora/Compute/Backend/CpuBackend.h>
#include <Sora/Compute/Backend/LlvmCpuCodeProvider.h>
#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <utility>

namespace {

    constexpr Sora::Compute::ValueType kFloat{Sora::Compute::ScalarKind::Float32, 1};

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

    consteval auto MakeIndexProgram() {
        constexpr Sora::Compute::ValueType indexType{Sora::Compute::ScalarKind::UInt64, 1};
        Sora::Compute::KernelBuilder<1, 1> scalar;
        const auto index = scalar.AddValueParameter(indexType);
        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<1, 1, 1> program{domain, std::move(scalar).Finish(index),
                                                             Sora::Compute::PartitionContract::Elementwise(1)};
        const auto output = program.AddBufferArgument(Sora::Compute::AccessMode::Write, indexType, 1);
        program.BindLogicalIndex(0);
        return std::move(program).Finish(output);
    }

    constexpr auto kIndex = MakeIndexProgram();

    consteval auto MakeColumnIndexProgram() {
        constexpr Sora::Compute::ValueType indexType{Sora::Compute::ScalarKind::UInt64, 1};
        Sora::Compute::KernelBuilder<1, 1> scalar;
        const auto index = scalar.AddValueParameter(indexType);
        constexpr Sora::Compute::IndexDomain domain{.rank = 2};
        Sora::Compute::KernelProgramBuilder<1, 1, 1> program{
            domain, std::move(scalar).Finish(index), Sora::Compute::PartitionContract::Elementwise(domain.rank)};
        const auto output = program.AddBufferArgument(Sora::Compute::AccessMode::Write, indexType, domain.rank,
                                                      Sora::Compute::LayoutKind::Strided);
        program.BindLogicalIndex(1);
        return std::move(program).Finish(output);
    }

    constexpr auto kColumnIndex = MakeColumnIndexProgram();

    [[nodiscard]] Sora::Compute::CpuCodeProvider CreateProvider() {
#if defined(_WIN32)
        const std::array searchPaths{std::filesystem::path{LR"(C:\Program Files\LLVM\bin)"}};
        auto created = Sora::Compute::CreateLlvmCpuCodeProvider(
            {.searchPaths = std::span<const std::filesystem::path>{searchPaths}});
#else
        auto created = Sora::Compute::CreateLlvmCpuCodeProvider();
#endif
        const Sora::Compute::CompileError error = created ? Sora::Compute::CompileError{} : created.error();
        INFO("LLVM provider error code=" << static_cast<uint32_t>(error.code) << ", detail=" << error.detail);
        REQUIRE(created.has_value());
        return std::move(*created);
    }

    [[nodiscard]] Sora::Compute::Allocation Allocate(Sora::Compute::CpuBackend& backend, uint64_t sizeBytes) {
        auto allocated = backend.DeviceAllocator().Allocate({
            .location = backend.DeviceLocation(),
            .requiredProperties =
                Sora::Compute::MemoryProperty::HostVisible | Sora::Compute::MemoryProperty::HostCoherent,
            .preferredProperties = Sora::Compute::MemoryProperty::Unified,
            .sizeBytes = sizeBytes,
            .alignment = 64,
        });
        const Sora::Compute::MemoryAllocationError error =
            allocated ? Sora::Compute::MemoryAllocationError{} : allocated.error();
        INFO("allocation error code=" << static_cast<uint32_t>(error.code) << ", detail=" << error.detail);
        REQUIRE(allocated.has_value());
        return std::move(*allocated);
    }

    [[nodiscard]] Sora::Compute::BackendBinding Bind(const Sora::Compute::Allocation& allocation, uint32_t binding,
                                                     Sora::Compute::AccessMode access, uint64_t count) {
        return {.allocation = allocation.View(),
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

    [[nodiscard]] std::span<float> Floats(std::span<std::byte> bytes) noexcept {
        return {reinterpret_cast<float*>(bytes.data()), bytes.size() / sizeof(float)};
    }

    [[nodiscard]] std::span<uint64_t> Indices(std::span<std::byte> bytes) noexcept {
        return {reinterpret_cast<uint64_t*>(bytes.data()), bytes.size() / sizeof(uint64_t)};
    }

} // namespace

TEST_CASE("LLVM provider executes through the CPU backend compiler and queue",
          "[Sora][Compute][CpuBackend][LLVM][Integration]") {
    constexpr uint64_t count = 4096;
    constexpr uint64_t sizeBytes = count * sizeof(float);
    constexpr float alpha = 1.75F;
    auto provider = CreateProvider();
    const auto compilerIdentity = provider.CompilerIdentity();
    const auto driverIdentity = provider.DriverIdentity();
    auto created = Sora::Compute::CpuBackend::Create(
        {.workerCount = 2, .minimumParallelWorkItems = 128, .minimumSimdWorkItems = 16}, std::move(provider));
    REQUIRE(created.has_value());
    auto backend = std::move(*created);
    REQUIRE(backend.Target().compilerIdentity == compilerIdentity);
    REQUIRE(backend.Target().driverIdentity == driverIdentity);

    auto xAllocation = Allocate(backend, sizeBytes);
    auto yAllocation = Allocate(backend, sizeBytes);
    auto outputAllocation = Allocate(backend, sizeBytes);
    auto xBytes = backend.Map(xAllocation);
    auto yBytes = backend.Map(yAllocation);
    REQUIRE(xBytes.has_value());
    REQUIRE(yBytes.has_value());
    const auto x = Floats(*xBytes);
    const auto y = Floats(*yBytes);
    for (uint64_t index = 0; index < count; ++index) {
        x[index] = static_cast<float>(index) * 0.25F;
        y[index] = 1000.0F - static_cast<float>(index) * 0.125F;
    }

    const auto programBytes = kSaxpy.Serialize();
    const Sora::Compute::BackendCompileRequest compileRequest{
        .moduleData = programBytes.Bytes().data(),
        .moduleSize = programBytes.Bytes().size(),
        .moduleHash = kSaxpy.Hash(),
        .target = backend.Target(),
        .artifactKind = Sora::Compute::ArtifactKind::DeviceExecutable,
    };
    auto artifact = backend.Compiler().Compile(compileRequest);
    REQUIRE(artifact.has_value());

    const std::array bindings{
        Bind(xAllocation, 1, Sora::Compute::AccessMode::Read, count),
        Bind(yAllocation, 2, Sora::Compute::AccessMode::Read, count),
        Bind(outputAllocation, 3, Sora::Compute::AccessMode::Write, count),
    };
    const auto alphaBytes = std::bit_cast<std::array<std::byte, sizeof(float)>>(alpha);
    const Sora::Compute::BackendScalarBinding scalar{
        .dataOffset = 0, .size = sizeof(float), .binding = 0, .type = kFloat};
    const Sora::Compute::BackendCommand command{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                                                .bindingCount = bindings.size(),
                                                .scalarBindingCount = 1,
                                                .artifact = artifact->View(),
                                                .workgroupCount = {64, 1, 1}};
    const Sora::Compute::BackendSubmitRequest request{.commands = &command,
                                                      .commandCount = 1,
                                                      .bindings = bindings.data(),
                                                      .bindingCount = bindings.size(),
                                                      .scalarData = alphaBytes.data(),
                                                      .scalarDataSize = alphaBytes.size(),
                                                      .scalarBindings = &scalar,
                                                      .scalarBindingCount = 1};
    auto submitted = backend.ExecutionQueue().Submit(request);
    REQUIRE(submitted.has_value());
    REQUIRE(submitted->WaitUntilTerminal().status == Sora::Compute::EventStatus::Completed);

    auto outputBytes = backend.Map(outputAllocation);
    REQUIRE(outputBytes.has_value());
    const auto output = Floats(*outputBytes);
    for (uint64_t index = 0; index < count; ++index) {
        REQUIRE(output[index] == std::fma(alpha, x[index], y[index]));
    }
    const Sora::Compute::CpuBackendStatistics statistics = backend.Statistics();
    REQUIRE(statistics.submittedBatches == 1);
    REQUIRE(statistics.nativeWorkItems == count);
    REQUIRE(statistics.scalarWorkItems == 0);
    REQUIRE(statistics.simdWorkItems == 0);
}

TEST_CASE("LLVM provider preserves global logical indices for shard-local native invocations",
          "[Sora][Compute][CpuBackend][LLVM][Partition]") {
    constexpr uint64_t globalCount = 256;
    constexpr uint64_t origin = 96;
    constexpr uint64_t shardCount = 64;
    auto provider = CreateProvider();
    auto created = Sora::Compute::CpuBackend::Create({.workerCount = 1}, std::move(provider));
    REQUIRE(created.has_value());
    auto backend = std::move(*created);
    auto outputAllocation = Allocate(backend, shardCount * sizeof(uint64_t));
    const auto bytes = kIndex.Serialize();
    const Sora::Compute::BackendCompileRequest compileRequest{.moduleData = bytes.Bytes().data(),
                                                              .moduleSize = bytes.Bytes().size(),
                                                              .moduleHash = kIndex.Hash(),
                                                              .target = backend.Target(),
                                                              .artifactKind =
                                                                  Sora::Compute::ArtifactKind::DeviceExecutable};
    auto artifact = backend.Compiler().Compile(compileRequest);
    REQUIRE(artifact.has_value());
    const Sora::Compute::BackendBinding binding{.allocation = outputAllocation.View(),
                                                .size = shardCount * sizeof(uint64_t),
                                                .binding = 0,
                                                .access = Sora::Compute::AccessMode::Write,
                                                .kind = Sora::Compute::BackendBindingKind::AffineElements,
                                                .elementType = {Sora::Compute::ScalarKind::UInt64, 1},
                                                .layout = Sora::Compute::LayoutKind::Contiguous,
                                                .rank = 1,
                                                .extents = {shardCount},
                                                .byteStrides = {sizeof(uint64_t)},
                                                .componentStrideBytes = sizeof(uint64_t)};
    const Sora::Compute::BackendCommand command{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                                                .bindingCount = 1,
                                                .artifact = artifact->View(),
                                                .dispatchOrigin = {origin, 0, 0},
                                                .dispatchExtents = {shardCount, 0, 0},
                                                .globalExtents = {globalCount, 0, 0},
                                                .workgroupCount = {1, 1, 1}};
    const Sora::Compute::BackendSubmitRequest request{
        .commands = &command, .commandCount = 1, .bindings = &binding, .bindingCount = 1};
    auto submitted = backend.ExecutionQueue().Submit(request);
    REQUIRE(submitted.has_value());
    REQUIRE(submitted->WaitUntilTerminal().status == Sora::Compute::EventStatus::Completed);
    auto outputBytes = backend.Map(outputAllocation);
    REQUIRE(outputBytes.has_value());
    const auto output = Indices(*outputBytes);
    for (uint64_t index = 0; index < shardCount; ++index) {
        REQUIRE(output[index] == origin + index);
    }
    REQUIRE(backend.Statistics().nativeWorkItems == shardCount);
}

TEST_CASE("LLVM provider preserves axis-one origins for strided native column shards",
          "[Sora][Compute][CpuBackend][LLVM][Partition][Strided]") {
    constexpr uint64_t rows = 7;
    constexpr uint64_t globalColumns = 19;
    constexpr uint64_t originColumn = 5;
    constexpr uint64_t shardColumns = 4;
    constexpr uint64_t storageElements = (rows - 1) * globalColumns + shardColumns;
    auto provider = CreateProvider();
    auto created = Sora::Compute::CpuBackend::Create({.workerCount = 1}, std::move(provider));
    REQUIRE(created.has_value());
    auto backend = std::move(*created);
    auto outputAllocation = Allocate(backend, storageElements * sizeof(uint64_t));
    const auto bytes = kColumnIndex.Serialize();
    const Sora::Compute::BackendCompileRequest compileRequest{.moduleData = bytes.Bytes().data(),
                                                              .moduleSize = bytes.Bytes().size(),
                                                              .moduleHash = kColumnIndex.Hash(),
                                                              .target = backend.Target(),
                                                              .artifactKind =
                                                                  Sora::Compute::ArtifactKind::DeviceExecutable};
    auto artifact = backend.Compiler().Compile(compileRequest);
    REQUIRE(artifact.has_value());
    const Sora::Compute::BackendBinding binding{.allocation = outputAllocation.View(),
                                                .size = storageElements * sizeof(uint64_t),
                                                .binding = 0,
                                                .access = Sora::Compute::AccessMode::Write,
                                                .kind = Sora::Compute::BackendBindingKind::AffineElements,
                                                .elementType = {Sora::Compute::ScalarKind::UInt64, 1},
                                                .layout = Sora::Compute::LayoutKind::Strided,
                                                .rank = 2,
                                                .extents = {rows, shardColumns},
                                                .byteStrides = {globalColumns * sizeof(uint64_t), sizeof(uint64_t)},
                                                .componentStrideBytes = sizeof(uint64_t)};
    const Sora::Compute::BackendCommand command{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                                                .bindingCount = 1,
                                                .artifact = artifact->View(),
                                                .dispatchOrigin = {0, originColumn, 0},
                                                .dispatchExtents = {rows, shardColumns, 0},
                                                .globalExtents = {rows, globalColumns, 0},
                                                .workgroupCount = {1, 1, 1}};
    const Sora::Compute::BackendSubmitRequest request{
        .commands = &command, .commandCount = 1, .bindings = &binding, .bindingCount = 1};
    auto submitted = backend.ExecutionQueue().Submit(request);
    REQUIRE(submitted.has_value());
    REQUIRE(submitted->WaitUntilTerminal().status == Sora::Compute::EventStatus::Completed);
    auto outputBytes = backend.Map(outputAllocation);
    REQUIRE(outputBytes.has_value());
    const auto output = Indices(*outputBytes);
    for (uint64_t row = 0; row < rows; ++row) {
        for (uint64_t column = 0; column < shardColumns; ++column) {
            REQUIRE(output[row * globalColumns + column] == originColumn + column);
        }
    }
    REQUIRE(backend.Statistics().nativeWorkItems == rows * shardColumns);
}

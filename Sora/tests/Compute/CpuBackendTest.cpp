#include <Sora/Compute/Backend/CpuBackend.h>
#include <Sora/Compute/ElementwiseKernel.h>
#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>
#include <Sora/Math/PortableCompile.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace {

    constexpr Sora::Compute::ValueType kFloat{Sora::Compute::ScalarKind::Float32, 1};
    constexpr Sora::Compute::ValueType kVec3{Sora::Compute::ScalarKind::Float32, 3};

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

    consteval auto MakeSoaAddProgram() {
        constexpr auto definition = Sora::Math::CompilePortable<float, 2>(
            [](auto first, auto second) { return Sora::Math::Add(first, second); });
        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        return Sora::Compute::LiftElementwise<3>(
            definition, domain,
            {.layout = Sora::Compute::LayoutKind::StructureOfArrays, .minimumAlignment = 64, .aliasClass = {3}},
            Sora::Compute::ElementwiseInputSpec::Buffer(Sora::Compute::LayoutKind::StructureOfArrays, 64, {1}),
            Sora::Compute::ElementwiseInputSpec::Buffer(Sora::Compute::LayoutKind::StructureOfArrays, 64, {2}));
    }

    consteval auto MakeIndexProgram(uint8_t axis,
                                    Sora::Compute::LayoutKind layout = Sora::Compute::LayoutKind::Contiguous) {
        constexpr Sora::Compute::ValueType indexType{Sora::Compute::ScalarKind::UInt64, 1};
        Sora::Compute::KernelBuilder<1, 1> scalar;
        const auto index = scalar.AddValueParameter(indexType);
        auto module = std::move(scalar).Finish(index);

        constexpr Sora::Compute::IndexDomain domain{.rank = 2};
        Sora::Compute::KernelProgramBuilder<1, 1, 1> program{
            domain, module, Sora::Compute::PartitionContract::Elementwise(domain.rank)};
        const auto output = program.AddBufferArgument(Sora::Compute::AccessMode::Write, indexType, domain.rank, layout);
        program.BindLogicalIndex(axis);
        return std::move(program).Finish(output);
    }

    constexpr auto kSaxpy = MakeSaxpyProgram();
    constexpr auto kSoaAdd = MakeSoaAddProgram();
    constexpr auto kColumnIndex = MakeIndexProgram(1);
    constexpr auto kStridedColumnIndex = MakeIndexProgram(1, Sora::Compute::LayoutKind::Strided);
    constexpr auto kRowIndex = MakeIndexProgram(0);

    Sora::Compute::Allocation Allocate(const Sora::Compute::MemoryAllocator& allocator,
                                       Sora::Compute::MemoryLocation location, uint64_t sizeBytes) {
        const Sora::Compute::BackendAllocationRequest request{
            .location = location,
            .requiredProperties =
                Sora::Compute::MemoryProperty::HostVisible | Sora::Compute::MemoryProperty::HostCoherent,
            .preferredProperties = Sora::Compute::MemoryProperty::Unified,
            .sizeBytes = sizeBytes,
            .alignment = 64,
        };
        auto allocation = allocator.Allocate(request);
        const Sora::Compute::MemoryAllocationError error =
            allocation ? Sora::Compute::MemoryAllocationError{} : allocation.error();
        INFO("allocation error code=" << static_cast<uint32_t>(error.code) << ", detail=" << error.detail);
        REQUIRE(allocation.has_value());
        return std::move(*allocation);
    }

    template<size_t ArgumentCapacity, size_t ParameterCapacity, size_t InstructionCapacity>
    Sora::Compute::Artifact
    Compile(Sora::Compute::CpuBackend& backend,
            const Sora::Compute::KernelProgram<ArgumentCapacity, ParameterCapacity, InstructionCapacity>& program) {
        const auto bytes = program.Serialize();
        const Sora::Compute::BackendCompileRequest request{
            .moduleData = bytes.Bytes().data(),
            .moduleSize = bytes.Bytes().size(),
            .moduleHash = program.Hash(),
            .target = backend.Target(),
            .artifactKind = Sora::Compute::ArtifactKind::DeviceExecutable,
        };
        auto artifact = backend.Compiler().Compile(request);
        REQUIRE(artifact.has_value());
        return std::move(*artifact);
    }

    const Sora::Compute::TransferEngine& FindTransfer(const Sora::Compute::CpuBackend& backend,
                                                      Sora::Compute::MemoryLocation source,
                                                      Sora::Compute::MemoryLocation destination,
                                                      Sora::Compute::TransferKind kind) {
        for (const Sora::Compute::TransferEngine& engine : backend.TransferEngines()) {
            if (engine.Source() == source && engine.Destination() == destination && engine.Kind() == kind) {
                return engine;
            }
        }
        FAIL("CPU transfer engine not found");
    }

    Sora::Compute::BackendBinding ContiguousBinding(const Sora::Compute::Allocation& allocation, uint32_t binding,
                                                    Sora::Compute::AccessMode access, Sora::Compute::ValueType type,
                                                    uint64_t count) {
        return {.allocation = allocation.View(),
                .offset = 0,
                .size = count * type.ByteSize(),
                .binding = binding,
                .access = access,
                .kind = Sora::Compute::BackendBindingKind::AffineElements,
                .elementType = type,
                .layout = Sora::Compute::LayoutKind::Contiguous,
                .rank = 1,
                .extents = {count},
                .byteStrides = {type.ByteSize()},
                .componentStrideBytes = Sora::Compute::ScalarByteSize(type.scalar)};
    }

    Sora::Compute::BackendBinding SoaBinding(const Sora::Compute::Allocation& allocation, uint32_t binding,
                                             Sora::Compute::AccessMode access, uint64_t count) {
        const uint64_t planeBytes = count * sizeof(float);
        return {.allocation = allocation.View(),
                .offset = 0,
                .size = planeBytes * kVec3.lanes,
                .binding = binding,
                .access = access,
                .kind = Sora::Compute::BackendBindingKind::AffineElements,
                .elementType = kVec3,
                .layout = Sora::Compute::LayoutKind::StructureOfArrays,
                .rank = 1,
                .extents = {count},
                .byteStrides = {sizeof(float)},
                .componentStrideBytes = planeBytes};
    }

    template<typename T>
    std::span<T> AsWritable(std::span<std::byte> bytes) {
        return {reinterpret_cast<T*>(bytes.data()), bytes.size() / sizeof(T)};
    }

    template<typename T>
    std::span<const T> AsReadable(std::span<std::byte> bytes) {
        return {reinterpret_cast<const T*>(bytes.data()), bytes.size() / sizeof(T)};
    }

    constexpr Sora::Compute::SemanticHash kFakeCompilerIdentity{0x46414B455F434F4DULL, 0x50494C45525F3031ULL};
    constexpr Sora::Compute::SemanticHash kFakeDriverIdentity{0x46414B455F445249ULL, 0x5645525F30303031ULL};
    constexpr std::array kFakeObjectBytes{
        std::byte{0x53}, std::byte{0x4F}, std::byte{0x52}, std::byte{0x41},
        std::byte{0x4A}, std::byte{0x49}, std::byte{0x54}, std::byte{0x31},
    };

    struct FakeProviderStatistics {
        std::atomic<uint64_t> compileCalls = 0;
        std::atomic<uint64_t> importCalls = 0;
        std::atomic<uint64_t> exportCalls = 0;
        std::atomic<uint64_t> executeCalls = 0;
        std::atomic<uint64_t> executedWorkItems = 0;
        std::atomic<uint64_t> destroyedArtifacts = 0;
        std::atomic<bool> providerDestroyed = false;
        std::atomic<bool> codeLeaseDestroyed = false;
    };

    struct FakeProviderState {
        std::shared_ptr<FakeProviderStatistics> statistics;
    };

    struct FakeArtifactState {
        std::shared_ptr<FakeProviderStatistics> statistics;
    };

    struct FakeCodeOwner {
        ~FakeCodeOwner() noexcept { statistics->codeLeaseDestroyed.store(true, std::memory_order_release); }

        std::shared_ptr<FakeProviderStatistics> statistics;
    };

    void DestroyFakeProvider(void* opaque) noexcept {
        auto* state = static_cast<FakeProviderState*>(opaque);
        state->statistics->providerDestroyed.store(true, std::memory_order_release);
        delete state;
    }

    void DestroyFakeArtifact(void* opaque) noexcept {
        auto* state = static_cast<FakeArtifactState*>(opaque);
        state->statistics->destroyedArtifacts.fetch_add(1, std::memory_order_relaxed);
        delete state;
    }

    void DestroyFakeObjectBytes(void* opaque) noexcept {
        delete[] static_cast<std::byte*>(opaque);
    }

    void MaterializeFakeArtifact(FakeProviderState& provider, Sora::Compute::CpuCodeArtifactResult* result) noexcept {
        auto* artifact = new (std::nothrow) FakeArtifactState{provider.statistics};
        if (artifact == nullptr) {
            result->error = {Sora::Compute::CompileErrorCode::OutOfMemory, 0};
            return;
        }
        result->payload = {.state = artifact, .destroy = DestroyFakeArtifact, .execute = nullptr};
        result->error = {};
    }

    void ExecuteFakeSaxpy(void* opaque, const Sora::Compute::CpuCodeInvocation* invocation) noexcept {
        auto& artifact = *static_cast<FakeArtifactState*>(opaque);
        artifact.statistics->executeCalls.fetch_add(1, std::memory_order_relaxed);
        artifact.statistics->executedWorkItems.fetch_add(invocation->end - invocation->begin,
                                                         std::memory_order_relaxed);
        float alpha = 0.0F;
        std::memcpy(&alpha, invocation->scalars[0].data, sizeof(alpha));
        const auto& x = invocation->buffers[1];
        const auto& y = invocation->buffers[2];
        const auto& output = invocation->buffers[3];
        for (uint64_t index = invocation->begin; index < invocation->end; ++index) {
            float xValue = 0.0F;
            float yValue = 0.0F;
            std::memcpy(&xValue, x.data + index * x.byteStrides[0], sizeof(xValue));
            std::memcpy(&yValue, y.data + index * y.byteStrides[0], sizeof(yValue));
            const float value = std::fma(alpha, xValue, yValue);
            std::memcpy(output.data + index * output.byteStrides[0], &value, sizeof(value));
        }
    }

    void CompileFakeCode(void* opaque, const Sora::Compute::CpuCodeCompileRequest* request,
                         Sora::Compute::CpuCodeArtifactResult* result) noexcept {
        auto& provider = *static_cast<FakeProviderState*>(opaque);
        provider.statistics->compileCalls.fetch_add(1, std::memory_order_relaxed);
        if (!Sora::Compute::ValidCpuCodeProviderAbi(request->abiVersion, request->structSize, sizeof(*request)) ||
            request->programHash != kSaxpy.Hash()) {
            result->error = {Sora::Compute::CompileErrorCode::UnsupportedIr, 0};
            return;
        }
        MaterializeFakeArtifact(provider, result);
        if (result->error.code == Sora::Compute::CompileErrorCode::None) {
            result->payload.execute = ExecuteFakeSaxpy;
        }
    }

    void ImportFakeCode(void* opaque, const Sora::Compute::CpuCodeImportRequest* request,
                        Sora::Compute::CpuCodeArtifactResult* result) noexcept {
        auto& provider = *static_cast<FakeProviderState*>(opaque);
        provider.statistics->importCalls.fetch_add(1, std::memory_order_relaxed);
        if (!Sora::Compute::ValidCpuCodeProviderAbi(request->abiVersion, request->structSize, sizeof(*request)) ||
            request->compileRequest == nullptr || !std::ranges::equal(request->ObjectBytes(), kFakeObjectBytes)) {
            result->error = {Sora::Compute::CompileErrorCode::ArtifactImportFailed, 0};
            return;
        }
        MaterializeFakeArtifact(provider, result);
        if (result->error.code == Sora::Compute::CompileErrorCode::None) {
            result->payload.execute = ExecuteFakeSaxpy;
        }
    }

    void ExportFakeCode(void* opaque, const Sora::Compute::CpuCodeExportRequest* request,
                        Sora::Compute::CpuCodeExportResult* result) noexcept {
        auto& provider = *static_cast<FakeProviderState*>(opaque);
        provider.statistics->exportCalls.fetch_add(1, std::memory_order_relaxed);
        if (!Sora::Compute::ValidCpuCodeProviderAbi(request->abiVersion, request->structSize, sizeof(*request)) ||
            request->artifactState == nullptr) {
            result->error = {Sora::Compute::CompileErrorCode::InvalidArtifact, 0};
            return;
        }
        auto bytes = std::unique_ptr<std::byte[]>{new (std::nothrow) std::byte[kFakeObjectBytes.size()]};
        if (!bytes) {
            result->error = {Sora::Compute::CompileErrorCode::OutOfMemory, kFakeObjectBytes.size()};
            return;
        }
        std::ranges::copy(kFakeObjectBytes, bytes.get());
        result->payload = {.data = bytes.get(),
                           .size = kFakeObjectBytes.size(),
                           .state = bytes.release(),
                           .destroy = DestroyFakeObjectBytes};
        result->error = {};
    }

    struct FakeProviderFixture {
        Sora::Compute::CpuCodeProvider provider;
        std::shared_ptr<FakeProviderStatistics> statistics;
    };

    FakeProviderFixture MakeFakeProvider() {
        auto statistics = std::make_shared<FakeProviderStatistics>();
        auto* state = new FakeProviderState{statistics};
        auto code = std::make_shared<FakeCodeOwner>(statistics);
        auto adopted = Sora::Compute::CpuCodeProvider::Adopt({.compilerIdentity = kFakeCompilerIdentity,
                                                              .driverIdentity = kFakeDriverIdentity,
                                                              .state = state,
                                                              .destroy = DestroyFakeProvider,
                                                              .compile = CompileFakeCode,
                                                              .importArtifact = ImportFakeCode,
                                                              .exportArtifact = ExportFakeCode},
                                                             Sora::Compute::BackendCodeLease::Retain(std::move(code)));
        REQUIRE(adopted.has_value());
        return {.provider = std::move(*adopted), .statistics = std::move(statistics)};
    }

} // namespace

TEST_CASE("CPU queue forms a signal-only join without synthetic commands", "[Sora][Compute][CpuBackend][Event]") {
    auto created = Sora::Compute::CpuBackend::Create();
    REQUIRE(created.has_value());
    auto backend = std::move(*created);
    REQUIRE(backend.ExecutionQueue().View().reportsExecutionNanoseconds);

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
    REQUIRE(backend.Statistics().submittedBatches == 3);
}

TEST_CASE("CPU backend executes portable SAXPY through coherent aliases, SIMD bulk, and asynchronous copy",
          "[Sora][Compute][CpuBackend][EndToEnd]") {
    constexpr uint64_t count = 4096;
    constexpr uint64_t sizeBytes = count * sizeof(float);
    constexpr float alpha = 1.75F;
    auto created = Sora::Compute::CpuBackend::Create(
        {.workerCount = 4, .minimumParallelWorkItems = 128, .minimumSimdWorkItems = 16});
    REQUIRE(created.has_value());
    auto backend = std::move(*created);

    auto xHost = Allocate(backend.HostAllocator(), backend.HostLocation(), sizeBytes);
    auto yHost = Allocate(backend.HostAllocator(), backend.HostLocation(), sizeBytes);
    auto outputDevice = Allocate(backend.DeviceAllocator(), backend.DeviceLocation(), sizeBytes);
    auto outputHost = Allocate(backend.HostAllocator(), backend.HostLocation(), sizeBytes);
    auto xBytes = backend.Map(xHost);
    auto yBytes = backend.Map(yHost);
    REQUIRE(xBytes.has_value());
    REQUIRE(yBytes.has_value());
    auto x = AsWritable<float>(*xBytes);
    auto y = AsWritable<float>(*yBytes);
    for (uint64_t index = 0; index < count; ++index) {
        x[index] = static_cast<float>(index) * 0.25F;
        y[index] = 1000.0F - static_cast<float>(index) * 0.125F;
    }

    const auto& uploadAlias = FindTransfer(backend, backend.HostLocation(), backend.DeviceLocation(),
                                           Sora::Compute::TransferKind::CoherentAlias);
    auto xDevice = uploadAlias.ImportAlias(xHost, 0, sizeBytes);
    auto yDevice = uploadAlias.ImportAlias(yHost, 0, sizeBytes);
    REQUIRE(xDevice.has_value());
    REQUIRE(yDevice.has_value());
    auto artifact = Compile(backend, kSaxpy);

    const std::array bindings{
        ContiguousBinding(*xDevice, 1, Sora::Compute::AccessMode::Read, kFloat, count),
        ContiguousBinding(*yDevice, 2, Sora::Compute::AccessMode::Read, kFloat, count),
        ContiguousBinding(outputDevice, 3, Sora::Compute::AccessMode::Write, kFloat, count),
    };
    const auto alphaBytes = std::bit_cast<std::array<std::byte, sizeof(float)>>(alpha);
    const Sora::Compute::BackendScalarBinding scalar{
        .dataOffset = 0, .size = sizeof(float), .binding = 0, .type = kFloat};
    const Sora::Compute::BackendCommand command{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                                                .firstBinding = 0,
                                                .bindingCount = bindings.size(),
                                                .firstScalarBinding = 0,
                                                .scalarBindingCount = 1,
                                                .artifact = artifact.View(),
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

    const auto& downloadCopy =
        FindTransfer(backend, backend.DeviceLocation(), backend.HostLocation(), Sora::Compute::TransferKind::Copy);
    const std::array regions{Sora::Compute::TransferRegion{.sizeBytes = sizeBytes}};
    auto copied = downloadCopy.Submit(outputDevice, outputHost, regions);
    REQUIRE(copied.has_value());
    REQUIRE(copied->WaitUntilTerminal().status == Sora::Compute::EventStatus::Completed);
    auto outputBytes = backend.Map(outputHost);
    REQUIRE(outputBytes.has_value());
    const auto output = AsReadable<float>(*outputBytes);
    for (uint64_t index = 0; index < count; ++index) {
        REQUIRE(output[index] == std::fma(alpha, x[index], y[index]));
    }
    const auto statistics = backend.Statistics();
    REQUIRE(statistics.submittedBatches == 1);
    REQUIRE(statistics.parallelDispatches == 1);
    REQUIRE(statistics.simdWorkItems == count);
    REQUIRE(statistics.scalarWorkItems == 0);
}

TEST_CASE("CPU backend vectorizes three-component SoA bindings without changing value semantics",
          "[Sora][Compute][CpuBackend][SoA][Simd]") {
    constexpr uint64_t count = 1024;
    constexpr uint64_t planeBytes = count * sizeof(float);
    constexpr uint64_t sizeBytes = planeBytes * kVec3.lanes;
    auto created = Sora::Compute::CpuBackend::Create(
        {.workerCount = 2, .minimumParallelWorkItems = 64, .minimumSimdWorkItems = 8});
    REQUIRE(created.has_value());
    auto backend = std::move(*created);
    auto xStorage = Allocate(backend.DeviceAllocator(), backend.DeviceLocation(), sizeBytes);
    auto yStorage = Allocate(backend.DeviceAllocator(), backend.DeviceLocation(), sizeBytes);
    auto outputStorage = Allocate(backend.DeviceAllocator(), backend.DeviceLocation(), sizeBytes);
    auto xBytes = backend.Map(xStorage);
    auto yBytes = backend.Map(yStorage);
    REQUIRE(xBytes.has_value());
    REQUIRE(yBytes.has_value());
    auto x = AsWritable<float>(*xBytes);
    auto y = AsWritable<float>(*yBytes);
    for (uint8_t component = 0; component < kVec3.lanes; ++component) {
        for (uint64_t index = 0; index < count; ++index) {
            const uint64_t offset = component * count + index;
            x[offset] = static_cast<float>(component * 1000U + index);
            y[offset] = static_cast<float>(component * 100U) - static_cast<float>(index) * 0.5F;
        }
    }
    auto artifact = Compile(backend, kSoaAdd);
    const std::array bindings{
        SoaBinding(xStorage, 0, Sora::Compute::AccessMode::Read, count),
        SoaBinding(yStorage, 1, Sora::Compute::AccessMode::Read, count),
        SoaBinding(outputStorage, 2, Sora::Compute::AccessMode::Write, count),
    };
    const Sora::Compute::BackendCommand command{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                                                .bindingCount = bindings.size(),
                                                .artifact = artifact.View(),
                                                .workgroupCount = {16, 1, 1}};
    const Sora::Compute::BackendSubmitRequest request{
        .commands = &command, .commandCount = 1, .bindings = bindings.data(), .bindingCount = bindings.size()};
    auto submitted = backend.ExecutionQueue().Submit(request);
    REQUIRE(submitted.has_value());
    REQUIRE(submitted->WaitUntilTerminal().status == Sora::Compute::EventStatus::Completed);
    auto outputBytes = backend.Map(outputStorage);
    REQUIRE(outputBytes.has_value());
    const auto output = AsReadable<float>(*outputBytes);
    for (uint64_t index = 0; index < output.size(); ++index) {
        REQUIRE(output[index] == x[index] + y[index]);
    }
    REQUIRE(backend.Statistics().simdWorkItems == count);
}

TEST_CASE("CPU backend resolves multidimensional logical indices on the scalar reference path",
          "[Sora][Compute][CpuBackend][Index]") {
    constexpr uint64_t rows = 17;
    constexpr uint64_t columns = 19;
    constexpr uint64_t count = rows * columns;
    auto created =
        Sora::Compute::CpuBackend::Create({.workerCount = 1, .minimumParallelWorkItems = 1, .minimumSimdWorkItems = 8});
    REQUIRE(created.has_value());
    auto backend = std::move(*created);
    auto outputStorage = Allocate(backend.DeviceAllocator(), backend.DeviceLocation(), count * sizeof(uint64_t));
    auto artifact = Compile(backend, kColumnIndex);
    const Sora::Compute::BackendBinding binding{.allocation = outputStorage.View(),
                                                .offset = 0,
                                                .size = count * sizeof(uint64_t),
                                                .binding = 0,
                                                .access = Sora::Compute::AccessMode::Write,
                                                .kind = Sora::Compute::BackendBindingKind::AffineElements,
                                                .elementType = {Sora::Compute::ScalarKind::UInt64, 1},
                                                .layout = Sora::Compute::LayoutKind::Contiguous,
                                                .rank = 2,
                                                .extents = {rows, columns},
                                                .byteStrides = {columns * sizeof(uint64_t), sizeof(uint64_t)},
                                                .componentStrideBytes = sizeof(uint64_t)};
    const Sora::Compute::BackendCommand command{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                                                .bindingCount = 1,
                                                .artifact = artifact.View(),
                                                .workgroupCount = {1, 1, 1}};
    const Sora::Compute::BackendSubmitRequest request{
        .commands = &command, .commandCount = 1, .bindings = &binding, .bindingCount = 1};
    auto submitted = backend.ExecutionQueue().Submit(request);
    REQUIRE(submitted.has_value());
    REQUIRE(submitted->WaitUntilTerminal().status == Sora::Compute::EventStatus::Completed);
    auto outputBytes = backend.Map(outputStorage);
    REQUIRE(outputBytes.has_value());
    const auto output = AsReadable<uint64_t>(*outputBytes);
    for (uint64_t row = 0; row < rows; ++row) {
        for (uint64_t column = 0; column < columns; ++column) {
            REQUIRE(output[row * columns + column] == column);
        }
    }
    const auto statistics = backend.Statistics();
    REQUIRE(statistics.parallelDispatches == 0);
    REQUIRE(statistics.scalarWorkItems == count);
    REQUIRE(statistics.simdWorkItems == 0);
}

TEST_CASE("CPU backend applies a global logical origin to a shard-local affine view",
          "[Sora][Compute][CpuBackend][Index][Partition]") {
    constexpr uint64_t globalRows = 17;
    constexpr uint64_t columns = 19;
    constexpr uint64_t originRow = 5;
    constexpr uint64_t shardRows = 4;
    constexpr uint64_t shardCount = shardRows * columns;
    auto created = Sora::Compute::CpuBackend::Create({.workerCount = 1});
    REQUIRE(created.has_value());
    auto backend = std::move(*created);
    auto outputStorage = Allocate(backend.DeviceAllocator(), backend.DeviceLocation(), shardCount * sizeof(uint64_t));
    auto artifact = Compile(backend, kRowIndex);
    const Sora::Compute::BackendBinding binding{.allocation = outputStorage.View(),
                                                .size = shardCount * sizeof(uint64_t),
                                                .binding = 0,
                                                .access = Sora::Compute::AccessMode::Write,
                                                .kind = Sora::Compute::BackendBindingKind::AffineElements,
                                                .elementType = {Sora::Compute::ScalarKind::UInt64, 1},
                                                .layout = Sora::Compute::LayoutKind::Contiguous,
                                                .rank = 2,
                                                .extents = {shardRows, columns},
                                                .byteStrides = {columns * sizeof(uint64_t), sizeof(uint64_t)},
                                                .componentStrideBytes = sizeof(uint64_t)};
    const Sora::Compute::BackendCommand command{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                                                .bindingCount = 1,
                                                .artifact = artifact.View(),
                                                .dispatchOrigin = {originRow, 0, 0},
                                                .dispatchExtents = {shardRows, columns, 0},
                                                .globalExtents = {globalRows, columns, 0},
                                                .workgroupCount = {1, 3, 1}};
    const Sora::Compute::BackendSubmitRequest request{
        .commands = &command, .commandCount = 1, .bindings = &binding, .bindingCount = 1};
    auto submitted = backend.ExecutionQueue().Submit(request);
    REQUIRE(submitted.has_value());
    REQUIRE(submitted->WaitUntilTerminal().status == Sora::Compute::EventStatus::Completed);
    auto outputBytes = backend.Map(outputStorage);
    REQUIRE(outputBytes.has_value());
    const auto output = AsReadable<uint64_t>(*outputBytes);
    for (uint64_t row = 0; row < shardRows; ++row) {
        for (uint64_t column = 0; column < columns; ++column) {
            REQUIRE(output[row * columns + column] == originRow + row);
        }
    }
}

TEST_CASE("CPU backend preserves axis-one origins for strided column shards",
          "[Sora][Compute][CpuBackend][Index][Partition][Strided]") {
    constexpr uint64_t rows = 7;
    constexpr uint64_t globalColumns = 19;
    constexpr uint64_t originColumn = 5;
    constexpr uint64_t shardColumns = 4;
    constexpr uint64_t storageElements = (rows - 1) * globalColumns + shardColumns;
    auto created = Sora::Compute::CpuBackend::Create({.workerCount = 1});
    REQUIRE(created.has_value());
    auto backend = std::move(*created);
    auto outputStorage =
        Allocate(backend.DeviceAllocator(), backend.DeviceLocation(), storageElements * sizeof(uint64_t));
    auto artifact = Compile(backend, kStridedColumnIndex);
    const Sora::Compute::BackendBinding binding{.allocation = outputStorage.View(),
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
                                                .artifact = artifact.View(),
                                                .dispatchOrigin = {0, originColumn, 0},
                                                .dispatchExtents = {rows, shardColumns, 0},
                                                .globalExtents = {rows, globalColumns, 0},
                                                .workgroupCount = {1, 1, 1}};
    const Sora::Compute::BackendSubmitRequest request{
        .commands = &command, .commandCount = 1, .bindings = &binding, .bindingCount = 1};
    auto submitted = backend.ExecutionQueue().Submit(request);
    REQUIRE(submitted.has_value());
    REQUIRE(submitted->WaitUntilTerminal().status == Sora::Compute::EventStatus::Completed);
    auto outputBytes = backend.Map(outputStorage);
    REQUIRE(outputBytes.has_value());
    const auto output = AsReadable<uint64_t>(*outputBytes);
    for (uint64_t row = 0; row < rows; ++row) {
        for (uint64_t column = 0; column < shardColumns; ++column) {
            REQUIRE(output[row * globalColumns + column] == originColumn + column);
        }
    }
}

TEST_CASE("CPU queues reject physical resources from another backend service instance",
          "[Sora][Compute][CpuBackend][Ownership]") {
    auto firstCreated = Sora::Compute::CpuBackend::Create({.workerCount = 1});
    auto secondCreated = Sora::Compute::CpuBackend::Create({.workerCount = 1});
    REQUIRE(firstCreated.has_value());
    REQUIRE(secondCreated.has_value());
    auto first = std::move(*firstCreated);
    auto second = std::move(*secondCreated);
    auto firstAllocation = Allocate(first.DeviceAllocator(), first.DeviceLocation(), 64);
    auto secondAllocation = Allocate(second.DeviceAllocator(), second.DeviceLocation(), 64);
    const Sora::Compute::BackendCommand copy{.kind = Sora::Compute::BackendCommandKind::Copy,
                                             .source = secondAllocation.View(),
                                             .target = firstAllocation.View(),
                                             .sizeBytes = 64};
    const Sora::Compute::BackendSubmitRequest request{.commands = &copy, .commandCount = 1};
    const auto submitted = first.ExecutionQueue().Submit(request);
    REQUIRE_FALSE(submitted.has_value());
    REQUIRE(submitted.error().code == Sora::Compute::SubmissionErrorCode::InvalidRequest);
    REQUIRE(first.Statistics().submittedBatches == 0);
}

TEST_CASE("CPU compiler exports canonical programs and reconstructs independent live artifacts",
          "[Sora][Compute][CpuBackend][Artifact][Persistence]") {
    auto created = Sora::Compute::CpuBackend::Create({.workerCount = 1});
    REQUIRE(created.has_value());
    auto backend = std::move(*created);
    REQUIRE(backend.Compiler().SupportsPersistentArtifacts());

    const auto programBytes = kSaxpy.Serialize();
    const Sora::Compute::BackendCompileRequest request{
        .moduleData = programBytes.Bytes().data(),
        .moduleSize = programBytes.Bytes().size(),
        .moduleHash = kSaxpy.Hash(),
        .target = backend.Target(),
        .artifactKind = Sora::Compute::ArtifactKind::DeviceExecutable,
    };
    auto compiled = backend.Compiler().Compile(request);
    REQUIRE(compiled.has_value());
    auto exported = backend.Compiler().Export(*compiled);
    REQUIRE(exported.has_value());
    REQUIRE(std::ranges::equal(*exported, programBytes.Bytes()));

    auto imported = backend.Compiler().Import(request, *exported);
    REQUIRE(imported.has_value());
    REQUIRE(imported->Key() == compiled->Key());
    REQUIRE(imported->View().state != compiled->View().state);

    exported->back() ^= std::byte{0x01};
    auto corrupted = backend.Compiler().Import(request, *exported);
    REQUIRE_FALSE(corrupted.has_value());
    REQUIRE(corrupted.error().code == Sora::Compute::CompileErrorCode::ArtifactImportFailed);
}

TEST_CASE("CPU code providers reject artifacts created by another provider instance",
          "[Sora][Compute][CpuBackend][NativeProvider][Ownership]") {
    auto first = MakeFakeProvider();
    auto second = MakeFakeProvider();
    const auto programBytes = kSaxpy.Serialize();
    const Sora::Compute::TargetFingerprint target{
        .device = {0x46414B455F435055ULL, 0x4445564943455F31ULL},
        .backend = Sora::Compute::BackendKind::Cpu,
        .compilerIdentity = kFakeCompilerIdentity,
        .driverIdentity = kFakeDriverIdentity,
    };
    const Sora::Compute::CpuCodeCompileRequest request{
        .programData = programBytes.Bytes().data(),
        .programSize = programBytes.Bytes().size(),
        .programHash = kSaxpy.Hash(),
        .target = target,
    };
    auto artifact = first.provider.Compile(request);
    REQUIRE(artifact.has_value());
    auto owned = first.provider.Export(*artifact);
    REQUIRE(owned.has_value());
    REQUIRE(std::ranges::equal(*owned, kFakeObjectBytes));
    auto foreign = second.provider.Export(*artifact);
    REQUIRE_FALSE(foreign.has_value());
    REQUIRE(foreign.error().code == Sora::Compute::CompileErrorCode::InvalidRequest);
}

TEST_CASE("CPU native provider preserves target identity, range granularity, persistence, and code lifetime",
          "[Sora][Compute][CpuBackend][NativeProvider]") {
    constexpr uint64_t count = 257;
    constexpr uint64_t sizeBytes = count * sizeof(float);
    constexpr float alpha = 2.25F;
    auto fixture = MakeFakeProvider();
    const auto statistics = fixture.statistics;
    std::optional<Sora::Compute::Artifact> retained;
    {
        auto created = Sora::Compute::CpuBackend::Create(
            {.workerCount = 1, .minimumParallelWorkItems = 1, .minimumSimdWorkItems = 8}, std::move(fixture.provider));
        REQUIRE(created.has_value());
        auto backend = std::move(*created);
        REQUIRE(backend.Target().compilerIdentity == kFakeCompilerIdentity);
        REQUIRE(backend.Target().driverIdentity == kFakeDriverIdentity);

        auto xStorage = Allocate(backend.DeviceAllocator(), backend.DeviceLocation(), sizeBytes);
        auto yStorage = Allocate(backend.DeviceAllocator(), backend.DeviceLocation(), sizeBytes);
        auto outputStorage = Allocate(backend.DeviceAllocator(), backend.DeviceLocation(), sizeBytes);
        auto xBytes = backend.Map(xStorage);
        auto yBytes = backend.Map(yStorage);
        REQUIRE(xBytes.has_value());
        REQUIRE(yBytes.has_value());
        auto x = AsWritable<float>(*xBytes);
        auto y = AsWritable<float>(*yBytes);
        for (uint64_t index = 0; index < count; ++index) {
            x[index] = static_cast<float>(index) * 0.5F;
            y[index] = 900.0F - static_cast<float>(index) * 0.25F;
        }

        const auto programBytes = kSaxpy.Serialize();
        const Sora::Compute::BackendCompileRequest compileRequest{
            .moduleData = programBytes.Bytes().data(),
            .moduleSize = programBytes.Bytes().size(),
            .moduleHash = kSaxpy.Hash(),
            .target = backend.Target(),
            .artifactKind = Sora::Compute::ArtifactKind::DeviceExecutable,
        };
        auto compiled = backend.Compiler().Compile(compileRequest);
        REQUIRE(compiled.has_value());
        REQUIRE(statistics->compileCalls.load(std::memory_order_relaxed) == 1);
        auto exported = backend.Compiler().Export(*compiled);
        REQUIRE(exported.has_value());
        REQUIRE(std::ranges::equal(*exported, kFakeObjectBytes));
        REQUIRE(statistics->exportCalls.load(std::memory_order_relaxed) == 1);
        auto imported = backend.Compiler().Import(compileRequest, *exported);
        REQUIRE(imported.has_value());
        REQUIRE(imported->Key() == compiled->Key());
        REQUIRE(imported->View().state != compiled->View().state);
        REQUIRE(statistics->importCalls.load(std::memory_order_relaxed) == 1);

        exported->back() ^= std::byte{0x01};
        auto corrupted = backend.Compiler().Import(compileRequest, *exported);
        REQUIRE_FALSE(corrupted.has_value());
        REQUIRE(corrupted.error().code == Sora::Compute::CompileErrorCode::ArtifactImportFailed);

        const std::array bindings{
            ContiguousBinding(xStorage, 1, Sora::Compute::AccessMode::Read, kFloat, count),
            ContiguousBinding(yStorage, 2, Sora::Compute::AccessMode::Read, kFloat, count),
            ContiguousBinding(outputStorage, 3, Sora::Compute::AccessMode::Write, kFloat, count),
        };
        const auto alphaBytes = std::bit_cast<std::array<std::byte, sizeof(float)>>(alpha);
        const Sora::Compute::BackendScalarBinding scalar{
            .dataOffset = 0, .size = sizeof(float), .binding = 0, .type = kFloat};
        const Sora::Compute::BackendCommand command{.kind = Sora::Compute::BackendCommandKind::Dispatch,
                                                    .bindingCount = bindings.size(),
                                                    .scalarBindingCount = 1,
                                                    .artifact = imported->View(),
                                                    .workgroupCount = {1, 1, 1}};
        const Sora::Compute::BackendSubmitRequest submitRequest{.commands = &command,
                                                                .commandCount = 1,
                                                                .bindings = bindings.data(),
                                                                .bindingCount = bindings.size(),
                                                                .scalarData = alphaBytes.data(),
                                                                .scalarDataSize = alphaBytes.size(),
                                                                .scalarBindings = &scalar,
                                                                .scalarBindingCount = 1};
        auto submitted = backend.ExecutionQueue().Submit(submitRequest);
        REQUIRE(submitted.has_value());
        REQUIRE(submitted->WaitUntilTerminal().status == Sora::Compute::EventStatus::Completed);
        auto outputBytes = backend.Map(outputStorage);
        REQUIRE(outputBytes.has_value());
        const auto output = AsReadable<float>(*outputBytes);
        for (uint64_t index = 0; index < count; ++index) {
            REQUIRE(output[index] == std::fma(alpha, x[index], y[index]));
        }
        const auto backendStatistics = backend.Statistics();
        REQUIRE(backendStatistics.nativeWorkItems == count);
        REQUIRE(backendStatistics.scalarWorkItems == 0);
        REQUIRE(backendStatistics.simdWorkItems == 0);
        REQUIRE(statistics->executeCalls.load(std::memory_order_relaxed) == 1);
        REQUIRE(statistics->executedWorkItems.load(std::memory_order_relaxed) == count);
        backend.ResetStatistics();
        REQUIRE(backend.Statistics() == Sora::Compute::CpuBackendStatistics{});

        compiled = {};
        retained.emplace(std::move(*imported));
        REQUIRE_FALSE(statistics->providerDestroyed.load(std::memory_order_acquire));
        REQUIRE_FALSE(statistics->codeLeaseDestroyed.load(std::memory_order_acquire));
    }
    REQUIRE_FALSE(statistics->providerDestroyed.load(std::memory_order_acquire));
    retained.reset();
    REQUIRE(statistics->destroyedArtifacts.load(std::memory_order_relaxed) == 2);
    REQUIRE(statistics->providerDestroyed.load(std::memory_order_acquire));
    REQUIRE(statistics->codeLeaseDestroyed.load(std::memory_order_acquire));
}

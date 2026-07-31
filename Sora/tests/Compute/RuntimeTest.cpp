#include "BackendTestSupport.h"
#include <Sora/Compute/CompilerService.h>
#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>
#include <Sora/Compute/Runtime/PerformanceModel.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <system_error>
#include <thread>
#include <vector>

namespace {

    struct MockCompilerState {
        std::atomic<uint32_t> compileCount = 0;
        std::atomic<uint32_t> importCount = 0;
        std::atomic<uint32_t> exportCount = 0;
        std::atomic<uint32_t> bytesDestroyCount = 0;
        std::atomic<uint32_t> destroyCount = 0;
        std::atomic<size_t> observedCacheSize = SIZE_MAX;
        std::chrono::milliseconds compileDelay{10};
        bool returnWrongKey = false;
    };

    struct MockNativeArtifact {
        MockCompilerState* owner = nullptr;
        Sora::Compute::ArtifactCache* reentrantCache = nullptr;
    };

    struct MockArtifactBytes {
        MockCompilerState* owner = nullptr;
        std::array<std::byte, 4> bytes{};
    };

    void DestroyMockArtifact(void* opaque) noexcept {
        std::unique_ptr<MockNativeArtifact> artifact{static_cast<MockNativeArtifact*>(opaque)};
        if (artifact->reentrantCache != nullptr) {
            artifact->owner->observedCacheSize.store(artifact->reentrantCache->Size(), std::memory_order_relaxed);
        }
        artifact->owner->destroyCount.fetch_add(1, std::memory_order_relaxed);
    }

    void DestroyMockArtifactBytes(void* opaque) noexcept {
        std::unique_ptr<MockArtifactBytes> bytes{static_cast<MockArtifactBytes*>(opaque)};
        bytes->owner->bytesDestroyCount.fetch_add(1, std::memory_order_relaxed);
    }

    void DestroyTestService(void* opaque) noexcept {
        delete static_cast<uint32_t*>(opaque);
    }

    Sora::Compute::BackendService MakeTestService(Sora::Compute::BackendServiceId id) {
        auto code = Sora::Compute::BackendCodeLease::Retain(std::make_shared<uint32_t>());
        auto service = Sora::Compute::BackendService::Adopt(
            {.id = id, .state = new uint32_t{}, .destroy = DestroyTestService}, std::move(code));
        REQUIRE(service.has_value());
        return *service;
    }

    void CompileMock(void* opaque, const Sora::Compute::BackendCompileRequest* request,
                     Sora::Compute::BackendCompileResult* result) noexcept {
        auto& state = *static_cast<MockCompilerState*>(opaque);
        state.compileCount.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(state.compileDelay);
        auto native = std::unique_ptr<MockNativeArtifact>{new (std::nothrow) MockNativeArtifact{&state}};
        if (!native) {
            result->error = {Sora::Compute::CompileErrorCode::OutOfMemory, 0};
            return;
        }
        auto key = request->ExpectedKey();
        if (state.returnWrongKey) {
            auto wrongRequest = *request;
            ++wrongRequest.moduleHash.low;
            key = wrongRequest.ExpectedKey();
        }
        result->payload = {
            .key = key, .device = request->target.device, .state = native.release(), .destroy = DestroyMockArtifact};
        result->error = {};
    }

    void ImportMock(void* opaque, const Sora::Compute::BackendArtifactImportRequest* request,
                    Sora::Compute::BackendCompileResult* result) noexcept {
        auto& state = *static_cast<MockCompilerState*>(opaque);
        state.importCount.fetch_add(1, std::memory_order_relaxed);
        constexpr std::array expected{std::byte{0x53}, std::byte{0x4B}, std::byte{0x49}, std::byte{0x52}};
        if (request->compileRequest == nullptr || !std::ranges::equal(request->ArtifactBytes(), expected)) {
            result->error = {Sora::Compute::CompileErrorCode::ArtifactImportFailed, 0};
            return;
        }
        auto native = std::unique_ptr<MockNativeArtifact>{new (std::nothrow) MockNativeArtifact{&state}};
        if (!native) {
            result->error = {Sora::Compute::CompileErrorCode::OutOfMemory, 0};
            return;
        }
        result->payload = {.key = request->compileRequest->ExpectedKey(),
                           .device = request->compileRequest->target.device,
                           .state = native.release(),
                           .destroy = DestroyMockArtifact};
        result->error = {};
    }

    void ExportMock(void* opaque, const Sora::Compute::BackendArtifactExportRequest* request,
                    Sora::Compute::BackendArtifactExportResult* result) noexcept {
        auto& state = *static_cast<MockCompilerState*>(opaque);
        state.exportCount.fetch_add(1, std::memory_order_relaxed);
        auto* artifact = static_cast<MockNativeArtifact*>(request->artifact.state);
        if (artifact == nullptr || artifact->owner != &state) {
            result->error = {Sora::Compute::CompileErrorCode::InvalidArtifact, 0};
            return;
        }
        auto bytes = std::unique_ptr<MockArtifactBytes>{new (std::nothrow) MockArtifactBytes{
            .owner = &state,
            .bytes = {std::byte{0x53}, std::byte{0x4B}, std::byte{0x49}, std::byte{0x52}},
        }};
        if (!bytes) {
            result->error = {Sora::Compute::CompileErrorCode::OutOfMemory, 0};
            return;
        }
        result->payload = {.data = bytes->bytes.data(),
                           .size = bytes->bytes.size(),
                           .state = bytes.release(),
                           .destroy = DestroyMockArtifactBytes};
        result->error = {};
    }

    class TemporaryDirectory final {
    public:
        TemporaryDirectory() {
            static std::atomic<uint64_t> sequence = 0;
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path_ =
                std::filesystem::temp_directory_path() / std::format("sora-compiler-service-test-{}-{}", stamp,
                                                                     sequence.fetch_add(1, std::memory_order_relaxed));
        }

        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }

        [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

    private:
        std::filesystem::path path_;
    };

    consteval auto MakeProgram() {
        constexpr Sora::Compute::ValueType indexType{Sora::Compute::ScalarKind::UInt64, 1};
        Sora::Compute::KernelBuilder<1, 1> scalar;
        const auto index = scalar.AddValueParameter(indexType);
        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<1, 1, 1> builder{
            domain, std::move(scalar).Finish(index), Sora::Compute::PartitionContract::Elementwise(domain.rank)};
        const auto output = builder.AddBufferArgument(Sora::Compute::AccessMode::Write, indexType, domain.rank);
        builder.BindLogicalIndex(0);
        return std::move(builder).Finish(output);
    }

    constexpr auto kProgram = MakeProgram();
    constexpr auto kProgramBytes = kProgram.Serialize();
    constexpr std::array kArtifactBytes{std::byte{0x53}, std::byte{0x4B}, std::byte{0x49}, std::byte{0x52}};
    constexpr Sora::Compute::SemanticHash kModuleHash = kProgram.Hash();
    constexpr Sora::Compute::TargetFingerprint kTarget{
        .device = {0xC001, 0xD00D},
        .backend = Sora::Compute::BackendKind::Cpu,
        .hostArchitecture = Sora::CpuArchitecture::X64,
        .operatingSystem = Sora::OperatingSystem::Windows,
        .binaryFormat = Sora::BinaryFormat::PE,
        .byteOrder = Sora::ByteOrder::Little,
        .abi = Sora::AbiFamily::MSVC,
        .identityStability = Sora::Compute::TargetIdentityStability::MachineStable,
        .addressBits = 64,
        .features = Sora::Compute::DeviceFeature::FusedMultiplyAdd,
        .architectureId = 0x7838365F3634,
        .fixedVectorBits = 256,
        .compilerIdentity = {1, 2},
        .driverIdentity = {3, 4},
    };
    constexpr Sora::Compute::SpecializationKey kSpecialization{
        .shape = {5, 6},
        .layout = {7, 8},
        .tuning = {9, 10},
    };
    constexpr Sora::Compute::BackendCompileRequest kRequest{
        .moduleData = kProgramBytes.Bytes().data(),
        .moduleSize = kProgramBytes.Bytes().size(),
        .moduleHash = kModuleHash,
        .target = kTarget,
        .specialization = kSpecialization,
    };

    void RegisterPackage(Sora::Compute::PortablePackageRegistry& registry,
                         std::vector<Sora::Compute::PackagedArtifact> artifacts) {
        auto program = Sora::Compute::ProgramPackage::Create(kProgram);
        REQUIRE(program.has_value());
        auto package = Sora::Compute::PortablePackage::Create(std::move(*program), std::move(artifacts));
        REQUIRE(package.has_value());
        REQUIRE(registry.Register(std::move(*package)).has_value());
    }

    Sora::Compute::BackendCompiler MakeCompiler(MockCompilerState& state) {
        auto compiler = Sora::Compute::BackendCompiler::Create(Sora::Test::TestBackendService(),
                                                               {.identity = kTarget.compilerIdentity,
                                                                .backend = kTarget.backend,
                                                                .state = &state,
                                                                .compile = CompileMock,
                                                                .importArtifact = ImportMock,
                                                                .exportArtifact = ExportMock});
        REQUIRE(compiler.has_value());
        return *compiler;
    }

    struct ReentrantResolutionObserverState {
        Sora::Compute::CompilerService* service = nullptr;
        const Sora::Compute::BackendCompiler* compiler = nullptr;
        const Sora::Compute::BackendCompileRequest* request = nullptr;
        std::optional<std::expected<Sora::Compute::Artifact, Sora::Compute::CompileError>> result;
        uint32_t observationCount = 0;
    };

    void ObserveAndReenterResolution(void* opaque, const Sora::Compute::ArtifactResolutionObservation&) noexcept {
        auto& state = *static_cast<ReentrantResolutionObserverState*>(opaque);
        ++state.observationCount;
        state.result = state.service->Resolve(*state.compiler, *state.request);
    }

} // namespace

TEST_CASE("Artifact stage keys encode exactly their compatibility domain", "[Sora][Compute][Artifact][Identity]") {
    STATIC_REQUIRE(kRequest.ExpectedKey().Words().size() == 26);
    REQUIRE(kTarget.PersistentCacheable());

    auto portableA = kRequest;
    portableA.artifactKind = Sora::Compute::ArtifactKind::PortableLowering;
    auto portableB = portableA;
    portableB.target = {};
    portableB.target.compilerIdentity = portableA.target.compilerIdentity;
    portableB.specialization = {};
    REQUIRE(portableA.ExpectedKey() == portableB.ExpectedKey());

    std::array variants{kTarget, kTarget, kTarget, kTarget, kTarget, kTarget, kTarget, kTarget,
                        kTarget, kTarget, kTarget, kTarget, kTarget, kTarget, kTarget, kTarget};
    ++variants[0].device.low;
    variants[1].backend = Sora::Compute::BackendKind::Vulkan;
    variants[2].hostArchitecture = Sora::CpuArchitecture::X86;
    variants[3].operatingSystem = Sora::OperatingSystem::Linux;
    variants[4].binaryFormat = Sora::BinaryFormat::ELF;
    variants[5].byteOrder = Sora::ByteOrder::Big;
    variants[6].abi = Sora::AbiFamily::Itanium;
    variants[7].identityStability = Sora::Compute::TargetIdentityStability::ProcessStable;
    variants[8].addressBits = 32;
    variants[9].features = Sora::Compute::DeviceFeature::Int64;
    ++variants[10].architectureId;
    variants[11].fixedVectorBits = 128;
    variants[12].scalableVectorBits = 128;
    ++variants[13].runtimeIdentity.low;
    ++variants[14].compilerIdentity.low;
    ++variants[15].driverIdentity.low;

    const auto expected = kRequest.ExpectedKey();
    for (size_t index = 1; index < variants.size() - 1; ++index) {
        auto changed = kRequest;
        changed.target = variants[index];
        REQUIRE(changed.ExpectedKey() != expected);
    }
    auto changedDevice = kRequest;
    changedDevice.target = variants[0];
    REQUIRE(changedDevice.ExpectedKey() == expected);
    auto changedDriver = kRequest;
    changedDriver.target = variants.back();
    REQUIRE(changedDriver.ExpectedKey() == expected);

    auto changedBackendAbi = kRequest;
    ++changedBackendAbi.target.backendAbiVersion;
    REQUIRE(changedBackendAbi.ExpectedKey() != expected);

    auto processStable = kTarget;
    processStable.identityStability = Sora::Compute::TargetIdentityStability::ProcessStable;
    REQUIRE_FALSE(processStable.PersistentCacheable());

    auto vulkanA = kTarget;
    vulkanA.backend = Sora::Compute::BackendKind::Vulkan;
    vulkanA.executableDomain = Sora::Compute::ExecutableTargetDomain::BackendPortable;
    auto vulkanB = vulkanA;
    ++vulkanB.device.low;
    ++vulkanB.architectureId;
    ++vulkanB.driverIdentity.low;
    const auto vulkanExecutableA = Sora::Compute::MakeArtifactKey(
        Sora::Compute::ExecutableKey{.lowered = kProgram.Hash(), .target = vulkanA, .specialization = kSpecialization});
    const auto vulkanExecutableB = Sora::Compute::MakeArtifactKey(
        Sora::Compute::ExecutableKey{.lowered = kProgram.Hash(), .target = vulkanB, .specialization = kSpecialization});
    REQUIRE(vulkanExecutableA == vulkanExecutableB);

    auto deviceArchitectureA = kTarget;
    deviceArchitectureA.executableDomain = Sora::Compute::ExecutableTargetDomain::DeviceArchitecture;
    auto deviceArchitectureB = deviceArchitectureA;
    deviceArchitectureB.operatingSystem = Sora::OperatingSystem::Linux;
    deviceArchitectureB.binaryFormat = Sora::BinaryFormat::ELF;
    deviceArchitectureB.abi = Sora::AbiFamily::Itanium;
    const auto deviceArchitectureKeyA = Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
        .lowered = kProgram.Hash(), .target = deviceArchitectureA, .specialization = kSpecialization});
    const auto deviceArchitectureKeyB = Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
        .lowered = kProgram.Hash(), .target = deviceArchitectureB, .specialization = kSpecialization});
    REQUIRE(deviceArchitectureKeyA == deviceArchitectureKeyB);
    ++deviceArchitectureB.architectureId;
    REQUIRE(deviceArchitectureKeyA !=
            Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
                .lowered = kProgram.Hash(), .target = deviceArchitectureB, .specialization = kSpecialization}));

    auto hostPlatformA = kTarget;
    hostPlatformA.executableDomain = Sora::Compute::ExecutableTargetDomain::HostPlatform;
    auto hostPlatformB = hostPlatformA;
    ++hostPlatformB.architectureId;
    hostPlatformB.features = Sora::Compute::DeviceFeature::Int64;
    const auto hostPlatformKeyA = Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
        .lowered = kProgram.Hash(), .target = hostPlatformA, .specialization = kSpecialization});
    const auto hostPlatformKeyB = Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
        .lowered = kProgram.Hash(), .target = hostPlatformB, .specialization = kSpecialization});
    REQUIRE(hostPlatformKeyA == hostPlatformKeyB);
    hostPlatformB.operatingSystem = Sora::OperatingSystem::Linux;
    REQUIRE(hostPlatformKeyA !=
            Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
                .lowered = kProgram.Hash(), .target = hostPlatformB, .specialization = kSpecialization}));

    auto differentDomain = vulkanA;
    differentDomain.executableDomain = Sora::Compute::ExecutableTargetDomain::DeviceArchitecture;
    REQUIRE(vulkanExecutableA !=
            Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
                .lowered = kProgram.Hash(), .target = differentDomain, .specialization = kSpecialization}));

    const auto pipelineA = Sora::Compute::MakeArtifactKey(
        Sora::Compute::PipelineKey{.executable = Sora::Compute::ArtifactIdentity(vulkanExecutableA),
                                   .target = vulkanA,
                                   .builder = {0xB001, 0xB002},
                                   .state = {0x5001, 0x5002},
                                   .bindingLayout = {0xA001, 0xA002}});
    const auto pipelineB = Sora::Compute::MakeArtifactKey(
        Sora::Compute::PipelineKey{.executable = Sora::Compute::ArtifactIdentity(vulkanExecutableB),
                                   .target = vulkanB,
                                   .builder = {0xB001, 0xB002},
                                   .state = {0x5001, 0x5002},
                                   .bindingLayout = {0xA001, 0xA002}});
    REQUIRE(pipelineA != pipelineB);
    auto missingDriver = kTarget;
    missingDriver.driverIdentity = {};
    REQUIRE_FALSE(missingDriver.PersistentCacheable());
}

TEST_CASE("Artifact shares ownership and destroys backend state exactly once", "[Sora][Compute][Artifact]") {
    MockCompilerState state{};
    {
        auto native = std::make_unique<MockNativeArtifact>(&state);
        auto adopted =
            Sora::Compute::Artifact::Adopt(Sora::Test::TestBackendService(), {
                                                                                 .key = kRequest.ExpectedKey(),
                                                                                 .device = kRequest.target.device,
                                                                                 .state = native.release(),
                                                                                 .destroy = DestroyMockArtifact,
                                                                             });
        REQUIRE(adopted.has_value());
        {
            const Sora::Compute::Artifact copy = *adopted;
            REQUIRE(copy.View().state == adopted->View().state);
            REQUIRE(copy.UseCount() == 2);
        }
        REQUIRE(state.destroyCount.load(std::memory_order_relaxed) == 0);
    }
    REQUIRE(state.destroyCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("Artifact rejects incomplete stage identities before adopting "
          "backend state",
          "[Sora][Compute][Artifact][Validation]") {
    MockCompilerState state{};
    auto native = std::make_unique<MockNativeArtifact>(&state);
    auto artifact = Sora::Compute::Artifact::Adopt(Sora::Test::TestBackendService(),
                                                   {.state = native.release(), .destroy = DestroyMockArtifact});

    REQUIRE_FALSE(artifact.has_value());
    REQUIRE(artifact.error() == Sora::Compute::ArtifactError::InvalidPayload);
    REQUIRE(state.destroyCount.load(std::memory_order_relaxed) == 1);
    STATIC_REQUIRE_FALSE(Sora::Compute::MakeArtifactKey(Sora::Compute::LoweringKey{}).Valid());
    STATIC_REQUIRE_FALSE(Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{}).Valid());
    STATIC_REQUIRE_FALSE(Sora::Compute::MakeArtifactKey(Sora::Compute::PipelineKey{}).Valid());
}

TEST_CASE("Backend compiler rejects a mismatched artifact key transactionally", "[Sora][Compute][Backend]") {
    MockCompilerState state{.returnWrongKey = true};
    const auto compiler = MakeCompiler(state);

    const auto result = compiler.Compile(kRequest);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == Sora::Compute::CompileErrorCode::InvalidArtifact);
    REQUIRE(state.compileCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(state.destroyCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("Backend compiler rejects a target outside its executable compatibility domain",
          "[Sora][Compute][Backend][Validation]") {
    MockCompilerState state{};
    const auto compiler = MakeCompiler(state);
    auto request = kRequest;
    request.target.executableDomain = Sora::Compute::ExecutableTargetDomain::BackendPortable;

    REQUIRE_FALSE(compiler.Accepts(request));
    const auto result = compiler.Compile(request);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == Sora::Compute::CompileErrorCode::InvalidRequest);
    REQUIRE(state.compileCount.load(std::memory_order_relaxed) == 0);

    auto native = std::make_unique<MockNativeArtifact>(&state);
    const auto artifact =
        Sora::Compute::Artifact::Adopt(Sora::Test::TestBackendService(), {.key = request.ExpectedKey(),
                                                                          .device = request.target.device,
                                                                          .state = native.release(),
                                                                          .destroy = DestroyMockArtifact});
    REQUIRE(artifact.has_value());
    const auto exported = compiler.Export(*artifact);
    REQUIRE_FALSE(exported.has_value());
    REQUIRE(exported.error().code == Sora::Compute::CompileErrorCode::InvalidRequest);
    REQUIRE(state.exportCount.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("CompilerService deduplicates concurrent compilation and caches the result",
          "[Sora][Compute][Compiler][Concurrency]") {
    constexpr size_t callerCount = 8;
    MockCompilerState state{};
    {
        const auto compiler = MakeCompiler(state);
        Sora::Compute::ArtifactCache cache{4};
        Sora::Compute::CompilerService service{cache};
        std::barrier start{callerCount};
        std::array<std::optional<std::expected<Sora::Compute::Artifact, Sora::Compute::CompileError>>, callerCount>
            results;
        std::array<std::jthread, callerCount> callers;

        for (size_t index = 0; index < callerCount; ++index) {
            callers[index] = std::jthread{[&, index] {
                start.arrive_and_wait();
                results[index] = service.Resolve(compiler, kRequest);
            }};
        }
        for (std::jthread& caller : callers) {
            caller.join();
        }

        REQUIRE(state.compileCount.load(std::memory_order_relaxed) == 1);
        REQUIRE(cache.Size() == 1);
        void* expectedState = nullptr;
        for (const auto& result : results) {
            REQUIRE(result.has_value());
            REQUIRE(result->has_value());
            if (expectedState == nullptr) {
                expectedState = result->value().View().state;
            }
            REQUIRE(result->value().View().state == expectedState);
        }

        const auto cached = service.Resolve(compiler, kRequest);
        REQUIRE(cached.has_value());
        REQUIRE(cached->View().state == expectedState);
        REQUIRE(state.compileCount.load(std::memory_order_relaxed) == 1);

        cache.Clear();
        for (auto& result : results) {
            result.reset();
        }
        REQUIRE(state.destroyCount.load(std::memory_order_relaxed) == 0);
    }
    REQUIRE(state.destroyCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("CompilerService publishes a cold result before invoking a reentrant resolution observer",
          "[Sora][Compute][Compiler][Telemetry][Reentrancy]") {
    MockCompilerState state{.compileDelay = std::chrono::milliseconds{0}};
    const auto compiler = MakeCompiler(state);
    Sora::Compute::ArtifactCache cache{2};
    ReentrantResolutionObserverState observerState{};
    Sora::Compute::CompilerService service{
        cache,
        {.context = &observerState, .observe = ObserveAndReenterResolution},
    };
    observerState.service = &service;
    observerState.compiler = &compiler;
    observerState.request = &kRequest;

    const auto resolved = service.Resolve(compiler, kRequest);
    REQUIRE(resolved.has_value());
    REQUIRE(observerState.observationCount == 1);
    REQUIRE(observerState.result.has_value());
    REQUIRE(observerState.result->has_value());
    REQUIRE(observerState.result->value().View().state == resolved->View().state);
    REQUIRE(state.compileCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(service.Statistics().memoryCacheHits == 1);
}

TEST_CASE("CompilerService deduplicates persistent materialization across independent stores",
          "[Sora][Compute][Compiler][Concurrency][Persistence][Lease]") {
    TemporaryDirectory temporary;
    const Sora::Compute::ArtifactStoreConfig config{
        .root = temporary.Path(),
        .maximumPayloadBytes = 4096,
        .materializationLeaseTimeout = std::chrono::seconds{2},
        .leasePollInterval = std::chrono::milliseconds{1},
        .durableWrites = false,
    };
    auto firstStore = Sora::Compute::ArtifactStore::Create(config);
    auto secondStore = Sora::Compute::ArtifactStore::Create(config);
    REQUIRE(firstStore.has_value());
    REQUIRE(secondStore.has_value());

    MockCompilerState state{.compileDelay = std::chrono::milliseconds{50}};
    const auto compiler = MakeCompiler(state);
    Sora::Compute::ArtifactCache firstCache{2};
    Sora::Compute::ArtifactCache secondCache{2};
    Sora::Compute::CompilerService firstService{firstCache, *firstStore};
    Sora::Compute::CompilerService secondService{secondCache, *secondStore};
    std::barrier start{2};
    std::array<std::optional<std::expected<Sora::Compute::Artifact, Sora::Compute::CompileError>>, 2> results;
    std::array<std::jthread, 2> callers{
        std::jthread{[&] {
            start.arrive_and_wait();
            results[0] = firstService.Resolve(compiler, kRequest);
        }},
        std::jthread{[&] {
            start.arrive_and_wait();
            results[1] = secondService.Resolve(compiler, kRequest);
        }},
    };
    for (auto& caller : callers) {
        caller.join();
    }

    REQUIRE(results[0].has_value());
    REQUIRE(results[0]->has_value());
    REQUIRE(results[1].has_value());
    REQUIRE(results[1]->has_value());
    REQUIRE(state.compileCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(state.exportCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(state.importCount.load(std::memory_order_relaxed) == 1);

    const auto firstStatistics = firstService.Statistics();
    const auto secondStatistics = secondService.Statistics();
    REQUIRE(firstStatistics.compilations + secondStatistics.compilations == 1);
    REQUIRE(firstStatistics.materializationLeaseAcquisitions + secondStatistics.materializationLeaseAcquisitions == 2);
    REQUIRE(firstStatistics.materializationLeaseContentions + secondStatistics.materializationLeaseContentions == 1);
    REQUIRE(firstStatistics.postLeaseDiskCacheHits + secondStatistics.postLeaseDiskCacheHits == 1);
    REQUIRE(firstStatistics.materializationLeaseTimeouts + secondStatistics.materializationLeaseTimeouts == 0);
    REQUIRE(firstStatistics.materializationLeaseFailures + secondStatistics.materializationLeaseFailures == 0);
}

TEST_CASE("CompilerService preserves live creation after a materialization lease timeout",
          "[Sora][Compute][Compiler][Persistence][Lease][Fallback]") {
    TemporaryDirectory temporary;
    const Sora::Compute::ArtifactStoreConfig config{
        .root = temporary.Path(),
        .maximumPayloadBytes = 4096,
        .materializationLeaseTimeout = std::chrono::milliseconds{20},
        .leasePollInterval = std::chrono::milliseconds{1},
        .durableWrites = false,
    };
    auto leaderStore = Sora::Compute::ArtifactStore::Create(config);
    auto followerStore = Sora::Compute::ArtifactStore::Create(config);
    REQUIRE(leaderStore.has_value());
    REQUIRE(followerStore.has_value());
    auto held = leaderStore->AcquireMaterializationLease(kRequest.ExpectedKey());
    REQUIRE(held.has_value());

    MockCompilerState state{};
    const auto compiler = MakeCompiler(state);
    Sora::Compute::ArtifactCache cache{2};
    Sora::Compute::CompilerService service{cache, *followerStore};
    const auto resolved = service.Resolve(compiler, kRequest);
    REQUIRE(resolved.has_value());
    REQUIRE(state.compileCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(state.exportCount.load(std::memory_order_relaxed) == 1);
    const auto statistics = service.Statistics();
    REQUIRE(statistics.compilations == 1);
    REQUIRE(statistics.materializationLeaseAcquisitions == 0);
    REQUIRE(statistics.materializationLeaseTimeouts == 1);
    REQUIRE(statistics.materializationLeaseFailures == 0);
}

TEST_CASE("ArtifactCache evicts the least recently used artifact", "[Sora][Compute][Artifact][Cache]") {
    MockCompilerState state{};
    Sora::Compute::ArtifactCache cache{2};
    auto makeArtifact = [&state](uint64_t moduleId) {
        auto request = kRequest;
        request.moduleHash.low = moduleId;
        const auto key = request.ExpectedKey();
        auto native = std::make_unique<MockNativeArtifact>(&state);
        return Sora::Compute::Artifact::Adopt(Sora::Test::TestBackendService(), {
                                                                                    .key = key,
                                                                                    .device = request.target.device,
                                                                                    .state = native.release(),
                                                                                    .destroy = DestroyMockArtifact,
                                                                                });
    };

    auto first = makeArtifact(1);
    auto second = makeArtifact(2);
    auto third = makeArtifact(3);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(third.has_value());
    REQUIRE(cache.Insert(*first).has_value());
    REQUIRE(cache.Insert(*second).has_value());
    const Sora::Compute::BackendServiceId service = Sora::Test::TestBackendService().Id();
    REQUIRE(cache.Find({first->Key(), service}).has_value());
    REQUIRE(cache.Insert(*third).has_value());

    REQUIRE(cache.Find({first->Key(), service}).has_value());
    REQUIRE_FALSE(cache.Find({second->Key(), service}).has_value());
    REQUIRE(cache.Find({third->Key(), service}).has_value());
}

TEST_CASE("ArtifactCache availability snapshots remain immutable and retain no "
          "artifact ownership",
          "[Sora][Compute][Artifact][Cache][Planning]") {
    MockCompilerState state{};
    Sora::Compute::ArtifactCache cache{2};
    const auto service = Sora::Test::TestBackendService().Id();
    auto makeArtifact = [&state](uint64_t moduleId) {
        auto request = kRequest;
        request.moduleHash.low = moduleId;
        auto native = std::make_unique<MockNativeArtifact>(&state);
        return Sora::Compute::Artifact::Adopt(Sora::Test::TestBackendService(), {.key = request.ExpectedKey(),
                                                                                 .device = request.target.device,
                                                                                 .state = native.release(),
                                                                                 .destroy = DestroyMockArtifact});
    };

    auto first = makeArtifact(11);
    auto second = makeArtifact(12);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    const Sora::Compute::LiveArtifactKey firstKey{first->Key(), service};
    const Sora::Compute::LiveArtifactKey secondKey{second->Key(), service};
    REQUIRE(cache.Insert(std::move(*first)).has_value());
    REQUIRE(cache.Insert(std::move(*second)).has_value());

    const auto frozen = cache.SnapshotAvailability();
    REQUIRE(frozen.has_value());
    REQUIRE(frozen->Keys().size() == 2);
    REQUIRE(frozen->Contains(firstKey));
    REQUIRE(frozen->Contains(secondKey));
    REQUIRE_FALSE(frozen->Contains({}));

    REQUIRE(cache.Erase(firstKey));
    const auto current = cache.SnapshotAvailability();
    REQUIRE(current.has_value());
    REQUIRE(current->Keys().size() == 1);
    REQUIRE_FALSE(current->Contains(firstKey));
    REQUIRE(current->Contains(secondKey));
    REQUIRE(frozen->Contains(firstKey));
    REQUIRE(state.destroyCount.load(std::memory_order_relaxed) == 1);

    cache.Clear();
    REQUIRE(state.destroyCount.load(std::memory_order_relaxed) == 2);
    REQUIRE(frozen->Contains(secondKey));
}

TEST_CASE("ArtifactCache releases backend resources after unlocking", "[Sora][Compute][Artifact][Cache]") {
    MockCompilerState state{};
    Sora::Compute::ArtifactCache cache{1};
    {
        auto native = std::make_unique<MockNativeArtifact>(&state, &cache);
        auto artifact =
            Sora::Compute::Artifact::Adopt(Sora::Test::TestBackendService(), {
                                                                                 .key = kRequest.ExpectedKey(),
                                                                                 .device = kRequest.target.device,
                                                                                 .state = native.release(),
                                                                                 .destroy = DestroyMockArtifact,
                                                                             });
        REQUIRE(artifact.has_value());
        REQUIRE(cache.Insert(*artifact).has_value());
    }

    REQUIRE(state.destroyCount.load(std::memory_order_relaxed) == 0);
    cache.Clear();
    REQUIRE(state.destroyCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(state.observedCacheSize.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("ArtifactCache invalidates one backend service without disturbing another",
          "[Sora][Compute][Artifact][Cache][DeviceGeneration]") {
    MockCompilerState state{};
    Sora::Compute::ArtifactCache cache{4};
    const auto& firstService = Sora::Test::TestBackendService();
    auto secondService = MakeTestService({0x5345434F4E445F53ULL, 1});
    auto makeArtifact = [&state](Sora::Compute::BackendService service, uint64_t moduleId) {
        auto request = kRequest;
        request.moduleHash.low = moduleId;
        auto native = std::make_unique<MockNativeArtifact>(&state);
        return Sora::Compute::Artifact::Adopt(std::move(service), {.key = request.ExpectedKey(),
                                                                   .device = request.target.device,
                                                                   .state = native.release(),
                                                                   .destroy = DestroyMockArtifact});
    };
    auto first = makeArtifact(firstService, 101);
    auto second = makeArtifact(secondService, 102);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(cache.Insert(*first).has_value());
    REQUIRE(cache.Insert(*second).has_value());
    const auto firstKey = first->Key();
    const auto secondKey = second->Key();
    first = {};
    second = {};

    const auto invalidated = cache.InvalidateService(firstService.Id());
    REQUIRE(invalidated.has_value());
    REQUIRE(*invalidated == 1);
    REQUIRE(cache.Size() == 1);
    REQUIRE_FALSE(cache.Find({firstKey, firstService.Id()}).has_value());
    REQUIRE(cache.Find({secondKey, secondService.Id()}).has_value());
    REQUIRE(state.destroyCount.load(std::memory_order_relaxed) == 1);

    auto retired = makeArtifact(firstService, 103);
    REQUIRE(retired.has_value());
    const auto rejected = cache.Insert(*retired);
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error() == Sora::Compute::ArtifactCacheError::RetiredService);

    Sora::Compute::ArtifactCache zeroCapacityCache{0};
    REQUIRE(zeroCapacityCache.InvalidateService(firstService.Id()).has_value());
    const auto zeroCapacityRejected = zeroCapacityCache.Insert(*retired);
    REQUIRE_FALSE(zeroCapacityRejected.has_value());
    REQUIRE(zeroCapacityRejected.error() == Sora::Compute::ArtifactCacheError::RetiredService);
}

TEST_CASE("BackendCompiler rejects bytes that do not match the semantic hash", "[Sora][Compute][Backend]") {
    MockCompilerState state{};
    const auto compiler = MakeCompiler(state);
    auto invalid = kRequest;
    ++invalid.moduleHash.low;

    const auto result = compiler.Compile(invalid);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == Sora::Compute::CompileErrorCode::InvalidRequest);
    REQUIRE(state.compileCount.load(std::memory_order_relaxed) == 0);

    auto valid = compiler.Compile(kRequest);
    REQUIRE(valid.has_value());
    Sora::Compute::ArtifactCache cache{2};
    REQUIRE(cache.Insert(*valid).has_value());
    Sora::Compute::CompilerService service{cache};
    std::vector<std::byte> changedBytes(kRequest.ModuleBytes().begin(), kRequest.ModuleBytes().end());
    changedBytes[0] ^= std::byte{0x01};
    auto mismatchedBytes = kRequest;
    mismatchedBytes.moduleData = changedBytes.data();
    const auto cached = service.Resolve(compiler, mismatchedBytes);
    REQUIRE_FALSE(cached.has_value());
    REQUIRE(cached.error().code == Sora::Compute::CompileErrorCode::InvalidRequest);
    REQUIRE(state.compileCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("Pipeline creation cannot enter the generic backend compiler", "[Sora][Compute][Backend][Validation]") {
    MockCompilerState state{};
    const auto compiler = MakeCompiler(state);
    auto pipelineRequest = kRequest;
    pipelineRequest.artifactKind = Sora::Compute::ArtifactKind::DevicePipeline;

    REQUIRE_FALSE(pipelineRequest.ExpectedKey().Valid());
    const auto direct = compiler.Compile(pipelineRequest);
    REQUIRE_FALSE(direct.has_value());
    REQUIRE(direct.error().code == Sora::Compute::CompileErrorCode::InvalidRequest);
    REQUIRE(state.compileCount.load(std::memory_order_relaxed) == 0);

    Sora::Compute::ArtifactCache cache{4};
    Sora::Compute::CompilerService service{cache};
    const auto coordinated = service.Resolve(compiler, pipelineRequest);
    REQUIRE_FALSE(coordinated.has_value());
    REQUIRE(coordinated.error().code == Sora::Compute::CompileErrorCode::InvalidRequest);
    REQUIRE(cache.Size() == 0);
    REQUIRE(state.compileCount.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("CompilerService repairs backend-invalid disk entries and imports "
          "the repaired artifact after restart",
          "[Sora][Compute][Compiler][Persistence]") {
    TemporaryDirectory temporary;
    auto createdStore = Sora::Compute::ArtifactStore::Create({
        .root = temporary.Path(),
        .maximumPayloadBytes = 4096,
        .durableWrites = false,
    });
    REQUIRE(createdStore.has_value());
    auto store = std::move(*createdStore);

    MockCompilerState firstState{};
    {
        const auto compiler = MakeCompiler(firstState);
        REQUIRE(compiler.SupportsPersistentArtifacts());
        Sora::Compute::ArtifactCache cache{2};
        Sora::Compute::CompilerService service{cache, store};
        auto resolved = service.Resolve(compiler, kRequest);
        REQUIRE(resolved.has_value());
        const auto statistics = service.Statistics();
        REQUIRE(statistics.diskCacheMisses == 1);
        REQUIRE(statistics.compilations == 1);
        REQUIRE(statistics.diskCacheHits == 0);
        REQUIRE(statistics.artifactExportFailures == 0);
        REQUIRE(statistics.diskStoreFailures == 0);
        REQUIRE(firstState.compileCount.load(std::memory_order_relaxed) == 1);
        REQUIRE(firstState.exportCount.load(std::memory_order_relaxed) == 1);
        REQUIRE(firstState.bytesDestroyCount.load(std::memory_order_relaxed) == 1);
    }

    constexpr std::array invalidBackendBytes{std::byte{0x42}, std::byte{0x41}, std::byte{0x44}};
    REQUIRE(store.Store(kRequest.ExpectedKey(), invalidBackendBytes).has_value());
    MockCompilerState repairingState{};
    {
        const auto compiler = MakeCompiler(repairingState);
        Sora::Compute::ArtifactCache cache{2};
        Sora::Compute::CompilerService service{cache, store};
        auto resolved = service.Resolve(compiler, kRequest);
        REQUIRE(resolved.has_value());
        const auto statistics = service.Statistics();
        REQUIRE(statistics.artifactImportFailures == 1);
        REQUIRE(statistics.compilations == 1);
        REQUIRE(statistics.diskCacheHits == 0);
        REQUIRE(repairingState.importCount.load(std::memory_order_relaxed) == 1);
        REQUIRE(repairingState.compileCount.load(std::memory_order_relaxed) == 1);
        REQUIRE(repairingState.exportCount.load(std::memory_order_relaxed) == 1);
        REQUIRE(repairingState.bytesDestroyCount.load(std::memory_order_relaxed) == 1);
    }

    MockCompilerState restartedState{};
    {
        const auto compiler = MakeCompiler(restartedState);
        Sora::Compute::ArtifactCache cache{2};
        Sora::Compute::CompilerService service{cache, store};
        auto resolved = service.Resolve(compiler, kRequest);
        REQUIRE(resolved.has_value());
        const auto statistics = service.Statistics();
        REQUIRE(statistics.diskCacheHits == 1);
        REQUIRE(statistics.compilations == 0);
        REQUIRE(statistics.artifactImportFailures == 0);
        REQUIRE(restartedState.importCount.load(std::memory_order_relaxed) == 1);
        REQUIRE(restartedState.compileCount.load(std::memory_order_relaxed) == 0);
        REQUIRE(restartedState.exportCount.load(std::memory_order_relaxed) == 0);
    }
}

TEST_CASE("CompilerService prefers an exact packaged artifact before disk and JIT",
          "[Sora][Compute][Compiler][PortablePackage]") {
    TemporaryDirectory temporary;
    auto createdStore = Sora::Compute::ArtifactStore::Create({
        .root = temporary.Path(),
        .maximumPayloadBytes = 4096,
        .durableWrites = false,
    });
    REQUIRE(createdStore.has_value());
    auto store = std::move(*createdStore);
    constexpr std::array invalidDiskBytes{std::byte{0x42}, std::byte{0x41}, std::byte{0x44}};
    REQUIRE(store.Store(kRequest.ExpectedKey(), invalidDiskBytes).has_value());

    Sora::Compute::PortablePackageRegistry packages;
    RegisterPackage(packages, {{.key = kRequest.ExpectedKey(),
                                .payload = std::vector<std::byte>(kArtifactBytes.begin(), kArtifactBytes.end())}});
    MockCompilerState state{};
    const auto compiler = MakeCompiler(state);
    Sora::Compute::ArtifactCache cache{2};
    Sora::Compute::PerformanceModel performance;
    Sora::Compute::CompilerService service{cache, store, packages, performance.ResolutionObserver()};

    auto resolved = service.Resolve(compiler, kRequest);
    REQUIRE(resolved.has_value());
    REQUIRE(state.importCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(state.compileCount.load(std::memory_order_relaxed) == 0);
    auto statistics = service.Statistics();
    REQUIRE(statistics.packageArtifactHits == 1);
    REQUIRE(statistics.diskCacheHits == 0);
    REQUIRE(statistics.compilations == 0);
    const auto observed = performance.Snapshot();
    REQUIRE(observed);
    REQUIRE(observed->Estimate(kRequest.ExpectedKey(), Sora::Compute::ArtifactResolutionPath::PackageImport));
    REQUIRE(
        observed->Estimate(kRequest.ExpectedKey(), Sora::Compute::ArtifactResolutionPath::PackageImport)->sampleCount ==
        1);

    REQUIRE(service.Resolve(compiler, kRequest).has_value());
    statistics = service.Statistics();
    REQUIRE(statistics.memoryCacheHits == 1);
    REQUIRE(statistics.packageArtifactHits == 1);
    REQUIRE(state.importCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(performance.Snapshot()
                ->Estimate(kRequest.ExpectedKey(), Sora::Compute::ArtifactResolutionPath::PackageImport)
                ->sampleCount == 1);
}

TEST_CASE("CompilerService treats a packaged artifact for another exact target "
          "as a miss and recompiles",
          "[Sora][Compute][Compiler][PortablePackage][Target]") {
    Sora::Compute::PortablePackageRegistry packages;
    RegisterPackage(packages, {{.key = kRequest.ExpectedKey(),
                                .payload = std::vector<std::byte>(kArtifactBytes.begin(), kArtifactBytes.end())}});
    MockCompilerState state{};
    const auto compiler = MakeCompiler(state);
    Sora::Compute::ArtifactCache cache{2};
    Sora::Compute::CompilerService service{cache, packages};
    auto otherTarget = kRequest;
    otherTarget.target.device = {0xC002, 0xD00E};
    ++otherTarget.target.architectureId;

    const auto resolved = service.Resolve(compiler, otherTarget);
    REQUIRE(resolved.has_value());
    REQUIRE(state.importCount.load(std::memory_order_relaxed) == 0);
    REQUIRE(state.compileCount.load(std::memory_order_relaxed) == 1);
    const auto statistics = service.Statistics();
    REQUIRE(statistics.packageArtifactHits == 0);
    REQUIRE(statistics.packageArtifactMisses == 1);
    REQUIRE(statistics.compilations == 1);
}

TEST_CASE("CompilerService records rejected packaged bytes and falls back to JIT",
          "[Sora][Compute][Compiler][PortablePackage][Fallback]") {
    Sora::Compute::PortablePackageRegistry packages;
    RegisterPackage(packages, {{.key = kRequest.ExpectedKey(), .payload = {std::byte{0x42}}}});
    MockCompilerState state{};
    const auto compiler = MakeCompiler(state);
    Sora::Compute::ArtifactCache cache{2};
    Sora::Compute::CompilerService service{cache, packages};

    const auto resolved = service.Resolve(compiler, kRequest);
    REQUIRE(resolved.has_value());
    REQUIRE(state.importCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(state.compileCount.load(std::memory_order_relaxed) == 1);
    const auto statistics = service.Statistics();
    REQUIRE(statistics.packageArtifactImportFailures == 1);
    REQUIRE(statistics.packageArtifactHits == 0);
    REQUIRE(statistics.compilations == 1);
}

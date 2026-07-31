/**
 * @file BackendServiceTest.cpp
 * @brief Lifecycle and simulated plugin-unload tests for owned compute backend services.
 */

#include <Sora/Compute/Backend.h>
#include <Sora/Compute/Backend/MemoryApi.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>
#include <memory>
#include <new>

namespace {

    struct LifecycleTrace {
        std::atomic<uint32_t> resourceDestroyCount = 0;
        std::atomic<uint32_t> serviceDestroyCount = 0;
        std::atomic<uint32_t> moduleDestroyCount = 0;
        std::atomic<uint32_t> reentrantProbeCount = 0;
        std::atomic<uint32_t> orderingViolationCount = 0;
        std::atomic<bool> serviceAlive = true;
        std::atomic<bool> codeAlive = true;
    };

    struct SimulatedModule {
        LifecycleTrace* trace = nullptr;

        ~SimulatedModule() {
            if (trace->serviceAlive.load(std::memory_order_acquire)) {
                trace->orderingViolationCount.fetch_add(1, std::memory_order_relaxed);
            }
            trace->codeAlive.store(false, std::memory_order_release);
            trace->moduleDestroyCount.fetch_add(1, std::memory_order_relaxed);
        }
    };

    struct NativeService {
        LifecycleTrace* trace = nullptr;
        std::atomic<uint64_t> nextMemory = 1;
    };

    struct NativeResource {
        LifecycleTrace* trace = nullptr;
    };

    void DestroyService(void* opaque) noexcept {
        std::unique_ptr<NativeService> service{static_cast<NativeService*>(opaque)};
        LifecycleTrace& trace = *service->trace;
        if (!trace.codeAlive.load(std::memory_order_acquire)) {
            trace.orderingViolationCount.fetch_add(1, std::memory_order_relaxed);
        }
        trace.reentrantProbeCount.fetch_add(1, std::memory_order_relaxed);
        trace.serviceAlive.store(false, std::memory_order_release);
        trace.serviceDestroyCount.fetch_add(1, std::memory_order_relaxed);
    }

    void DestroyResource(void* opaque) noexcept {
        std::unique_ptr<NativeResource> resource{static_cast<NativeResource*>(opaque)};
        LifecycleTrace& trace = *resource->trace;
        if (!trace.serviceAlive.load(std::memory_order_acquire) || !trace.codeAlive.load(std::memory_order_acquire)) {
            trace.orderingViolationCount.fetch_add(1, std::memory_order_relaxed);
        }
        trace.resourceDestroyCount.fetch_add(1, std::memory_order_relaxed);
    }

    std::expected<Sora::Compute::BackendService, Sora::Compute::BackendServiceError>
    MakeService(LifecycleTrace& trace, Sora::Compute::BackendServiceId id) {
        auto module = std::make_shared<SimulatedModule>(&trace);
        auto code = Sora::Compute::BackendCodeLease::Retain(module);
        auto service = std::make_unique<NativeService>(&trace);
        return Sora::Compute::BackendService::Adopt({.id = id, .state = service.release(), .destroy = DestroyService},
                                                    std::move(code));
    }

    constexpr Sora::Compute::DeviceId kDevice{1, 1};
    constexpr Sora::Compute::MemoryLocation kLocation{
        .domain = {2, 2}, .device = kDevice, .backend = Sora::Compute::BackendKind::Cpu};

    void Allocate(void* opaque, const Sora::Compute::BackendAllocationRequest* request,
                  Sora::Compute::BackendAllocationResult* result) noexcept {
        auto& service = *static_cast<NativeService*>(opaque);
        auto resource = std::unique_ptr<NativeResource>{new (std::nothrow) NativeResource{service.trace}};
        if (!resource) {
            result->error = {Sora::Compute::MemoryAllocationErrorCode::OutOfMemory, 0};
            return;
        }
        result->allocation = {.location = request->location,
                              .memory = {service.nextMemory.fetch_add(1, std::memory_order_relaxed), 1},
                              .properties = request->requiredProperties,
                              .sizeBytes = request->sizeBytes,
                              .alignment = request->alignment,
                              .state = resource.release(),
                              .destroy = DestroyResource};
        result->error = {Sora::Compute::MemoryAllocationErrorCode::None, 0};
    }

    void Compile(void* opaque, const Sora::Compute::BackendCompileRequest* request,
                 Sora::Compute::BackendCompileResult* result) noexcept {
        auto& service = *static_cast<NativeService*>(opaque);
        auto resource = std::unique_ptr<NativeResource>{new (std::nothrow) NativeResource{service.trace}};
        if (!resource) {
            result->error = {Sora::Compute::CompileErrorCode::OutOfMemory, 0};
            return;
        }
        result->payload = {.key = request->ExpectedKey(),
                           .device = request->target.device,
                           .state = resource.release(),
                           .destroy = DestroyResource};
        result->error = {};
    }

    void RequireRetirementOrder(const LifecycleTrace& trace) {
        REQUIRE(trace.resourceDestroyCount.load(std::memory_order_relaxed) == 1);
        REQUIRE(trace.serviceDestroyCount.load(std::memory_order_relaxed) == 1);
        REQUIRE(trace.moduleDestroyCount.load(std::memory_order_relaxed) == 1);
        REQUIRE(trace.reentrantProbeCount.load(std::memory_order_relaxed) == 1);
        REQUIRE(trace.orderingViolationCount.load(std::memory_order_relaxed) == 0);
    }

} // namespace

TEST_CASE("Allocated resources pin service state and plugin code after the allocator retires",
          "[Sora][Compute][Backend][Service][Memory][Lifecycle]") {
    LifecycleTrace trace;
    Sora::Compute::Allocation escaped;
    {
        auto service = MakeService(trace, {1, 1});
        REQUIRE(service.has_value());
        auto allocator = Sora::Compute::MemoryAllocator::Create(
            *service, {.location = kLocation,
                       .supportedProperties = Sora::Compute::MemoryProperty::DeviceLocal,
                       .state = service->State(),
                       .allocate = Allocate});
        REQUIRE(allocator.has_value());
        auto allocation = allocator->Allocate({.location = kLocation,
                                               .requiredProperties = Sora::Compute::MemoryProperty::DeviceLocal,
                                               .sizeBytes = 4096,
                                               .alignment = 256});
        REQUIRE(allocation.has_value());
        escaped = *allocation;
    }

    REQUIRE(trace.resourceDestroyCount.load(std::memory_order_relaxed) == 0);
    REQUIRE(trace.serviceDestroyCount.load(std::memory_order_relaxed) == 0);
    REQUIRE(trace.moduleDestroyCount.load(std::memory_order_relaxed) == 0);
    escaped = {};
    RequireRetirementOrder(trace);
}

TEST_CASE("Compiled artifacts pin compiler service and plugin code after the facade retires",
          "[Sora][Compute][Backend][Service][Compiler][Lifecycle]") {
    LifecycleTrace trace;
    Sora::Compute::Artifact escaped;
    constexpr std::array module{std::byte{0x53}, std::byte{0x4B}, std::byte{0x49}, std::byte{0x52}};
    constexpr Sora::Compute::SemanticHash compilerIdentity{3, 4};
    {
        auto service = MakeService(trace, {2, 2});
        REQUIRE(service.has_value());
        auto compiler = Sora::Compute::BackendCompiler::Create(*service, {.identity = compilerIdentity,
                                                                          .backend = Sora::Compute::BackendKind::Cpu,
                                                                          .state = service->State(),
                                                                          .compile = Compile});
        REQUIRE(compiler.has_value());
        const Sora::Compute::BackendCompileRequest request{
            .moduleData = module.data(),
            .moduleSize = module.size(),
            .moduleHash = Sora::Compute::Detail::HashBytes(module),
            .target = {.device = kDevice,
                       .backend = Sora::Compute::BackendKind::Cpu,
                       .hostArchitecture = Sora::CpuArchitecture::X64,
                       .operatingSystem = Sora::OperatingSystem::Windows,
                       .binaryFormat = Sora::BinaryFormat::PE,
                       .byteOrder = Sora::ByteOrder::Little,
                       .abi = Sora::AbiFamily::MSVC,
                       .addressBits = 64,
                       .architectureId = 0x7838365F3634,
                       .compilerIdentity = compilerIdentity,
                       .driverIdentity = {5, 6}},
        };
        auto artifact = compiler->Compile(request);
        REQUIRE(artifact.has_value());
        escaped = *artifact;
    }

    REQUIRE(trace.resourceDestroyCount.load(std::memory_order_relaxed) == 0);
    REQUIRE(trace.serviceDestroyCount.load(std::memory_order_relaxed) == 0);
    REQUIRE(trace.moduleDestroyCount.load(std::memory_order_relaxed) == 0);
    escaped = {};
    RequireRetirementOrder(trace);
}

TEST_CASE("BackendService rejects a missing code owner and releases transferred state",
          "[Sora][Compute][Backend][Service][Validation]") {
    std::atomic<uint32_t> destroyCount = 0;
    struct Probe {
        std::atomic<uint32_t>* count = nullptr;
    };
    auto destroy = +[](void* opaque) noexcept {
        std::unique_ptr<Probe> probe{static_cast<Probe*>(opaque)};
        probe->count->fetch_add(1, std::memory_order_relaxed);
    };
    auto state = std::make_unique<Probe>(&destroyCount);
    const auto service =
        Sora::Compute::BackendService::Adopt({.id = {7, 7}, .state = state.release(), .destroy = destroy}, {});
    REQUIRE_FALSE(service.has_value());
    REQUIRE(service.error() == Sora::Compute::BackendServiceError::MissingCodeLease);
    REQUIRE(destroyCount.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("BackendCompiler rejects runtime-only errors returned across the backend ABI",
          "[Sora][Compute][Backend][Service][Compiler][Validation]") {
    LifecycleTrace trace;
    constexpr std::array module{std::byte{0x53}, std::byte{0x4B}, std::byte{0x49}, std::byte{0x52}};
    constexpr Sora::Compute::SemanticHash compilerIdentity{8, 9};
    {
        auto service = MakeService(trace, {8, 8});
        REQUIRE(service.has_value());
        auto compile = +[](void*, const Sora::Compute::BackendCompileRequest*,
                           Sora::Compute::BackendCompileResult* result) noexcept {
            result->error = {Sora::Compute::CompileErrorCode::InvalidApi, 17};
        };
        auto compiler = Sora::Compute::BackendCompiler::Create(*service, {.identity = compilerIdentity,
                                                                          .backend = Sora::Compute::BackendKind::Cpu,
                                                                          .state = service->State(),
                                                                          .compile = compile});
        REQUIRE(compiler.has_value());
        const Sora::Compute::BackendCompileRequest request{
            .moduleData = module.data(),
            .moduleSize = module.size(),
            .moduleHash = Sora::Compute::Detail::HashBytes(module),
            .target = {.device = kDevice,
                       .backend = Sora::Compute::BackendKind::Cpu,
                       .hostArchitecture = Sora::CpuArchitecture::X64,
                       .operatingSystem = Sora::OperatingSystem::Windows,
                       .binaryFormat = Sora::BinaryFormat::PE,
                       .byteOrder = Sora::ByteOrder::Little,
                       .abi = Sora::AbiFamily::MSVC,
                       .addressBits = 64,
                       .architectureId = 0x7838365F3634,
                       .compilerIdentity = compilerIdentity,
                       .driverIdentity = {10, 11}},
        };
        const auto result = compiler->Compile(request);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code == Sora::Compute::CompileErrorCode::InvalidArtifact);
    }

    REQUIRE(trace.resourceDestroyCount.load(std::memory_order_relaxed) == 0);
    REQUIRE(trace.serviceDestroyCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(trace.moduleDestroyCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(trace.reentrantProbeCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(trace.orderingViolationCount.load(std::memory_order_relaxed) == 0);
}

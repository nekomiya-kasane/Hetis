#include "BackendTestSupport.h"

#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>
#include <Sora/Compute/Runtime/PhysicalGraphPlan.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace {

    constexpr Sora::Compute::ValueType kFloat{Sora::Compute::ScalarKind::Float32, 1};
    constexpr Sora::Compute::BackendServiceId kService{0x534F52415F544553ULL, 0x545F4241434B454EULL};
    constexpr Sora::Compute::DeviceId kCpu{1, 1};
    constexpr Sora::Compute::DeviceId kGpu{2, 2};
    constexpr Sora::Compute::MemoryLocation kHost{
        .domain = {10, 10}, .device = kCpu, .backend = Sora::Compute::BackendKind::Cpu};
    constexpr Sora::Compute::MemoryLocation kDevice{
        .domain = {20, 20}, .device = kGpu, .backend = Sora::Compute::BackendKind::Vulkan};
    constexpr Sora::Compute::MemoryLocation kSharedCpu{
        .domain = {21, 21}, .device = kCpu, .backend = Sora::Compute::BackendKind::Cpu};
    constexpr Sora::Compute::MemoryLocation kSharedGpu{
        .domain = {21, 21}, .device = kGpu, .backend = Sora::Compute::BackendKind::Vulkan};
    constexpr Sora::Compute::SemanticHash kCompilerIdentity{30, 31};
    constexpr Sora::Compute::SemanticHash kPipelineFactoryIdentity{34, 35};
    constexpr Sora::Compute::SemanticHash kPipelineStateIdentity{36, 37};

    [[nodiscard]] constexpr Sora::Compute::TargetFingerprint GpuTarget() noexcept {
        return {.device = kGpu,
                .backend = Sora::Compute::BackendKind::Vulkan,
                .hostArchitecture = Sora::CpuArchitecture::X64,
                .operatingSystem = Sora::OperatingSystem::Windows,
                .binaryFormat = Sora::BinaryFormat::PE,
                .byteOrder = Sora::ByteOrder::Little,
                .abi = Sora::AbiFamily::MSVC,
                .identityStability = Sora::Compute::TargetIdentityStability::MachineStable,
                .executableDomain = Sora::Compute::ExecutableTargetDomain::BackendPortable,
                .addressBits = 64,
                .architectureId = 2,
                .compilerIdentity = kCompilerIdentity,
                .driverIdentity = {32, 33}};
    }

    consteval auto MakeSaxpyProgram() {
        Sora::Compute::KernelBuilder<3, 4> scalar;
        const auto alpha = scalar.AddValueParameter(kFloat);
        const auto x = scalar.AddValueParameter(kFloat);
        const auto y = scalar.AddValueParameter(kFloat);
        const auto result = scalar.Ternary(Sora::Compute::OpCode::Fma, alpha, x, y);
        Sora::Compute::KernelProgramBuilder<4, 3, 4> program{Sora::Compute::IndexDomain{.rank = 1},
                                                             std::move(scalar).Finish(result),
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

    constexpr auto kSaxpy = MakeSaxpyProgram();

    struct MockState {
        std::atomic<uint32_t> backendCallCount = 0;
        std::atomic<uint32_t> allocationDestroyCount = 0;
        std::atomic<uint64_t> nextMemory = 1;
        Sora::Compute::DeviceState deviceState = Sora::Compute::DeviceState::Available;
        uint64_t deviceGeneration = 1;
    };

    void ObserveDevice(void* opaque, Sora::Compute::DeviceSnapshot* snapshot) noexcept {
        const auto& state = *static_cast<const MockState*>(opaque);
        *snapshot = {.id = kGpu,
                     .state = state.deviceState,
                     .generation = state.deviceGeneration,
                     .availableMemoryBytes = 1ULL << 30};
    }

    struct NativeAllocation {
        MockState* owner = nullptr;
    };

    void DestroyAllocation(void* opaque) noexcept {
        std::unique_ptr<NativeAllocation> allocation{static_cast<NativeAllocation*>(opaque)};
        allocation->owner->allocationDestroyCount.fetch_add(1, std::memory_order_relaxed);
    }

    void DestroyQueue(void* state) noexcept {
        delete static_cast<uint32_t*>(state);
    }

    void UnexpectedAllocate(void* state, const Sora::Compute::BackendAllocationRequest*,
                            Sora::Compute::BackendAllocationResult*) noexcept {
        static_cast<MockState*>(state)->backendCallCount.fetch_add(1, std::memory_order_relaxed);
    }

    void UnexpectedTransfer(void* state, const Sora::Compute::BackendTransferRequest*,
                            Sora::Compute::BackendTransferResult*) noexcept {
        static_cast<MockState*>(state)->backendCallCount.fetch_add(1, std::memory_order_relaxed);
    }

    void UnexpectedCompile(void* state, const Sora::Compute::BackendCompileRequest*,
                           Sora::Compute::BackendCompileResult*) noexcept {
        static_cast<MockState*>(state)->backendCallCount.fetch_add(1, std::memory_order_relaxed);
    }

    void UnexpectedCreatePipeline(void* state, const Sora::Compute::BackendPipelineRequest*,
                                  Sora::Compute::BackendPipelineResult*) noexcept {
        static_cast<MockState*>(state)->backendCallCount.fetch_add(1, std::memory_order_relaxed);
    }

    void UnexpectedSubmit(void*, const Sora::Compute::BackendSubmitRequest*,
                          Sora::Compute::BackendSubmitResult*) noexcept {}

    Sora::Compute::Allocation MakeAllocation(MockState& state, Sora::Compute::MemoryLocation location,
                                             uint64_t sizeBytes = 64) {
        auto native = std::make_unique<NativeAllocation>(&state);
        auto allocation = Sora::Compute::Allocation::Adopt(
            Sora::Test::TestBackendService(), {.location = location,
                                               .memory = {state.nextMemory.fetch_add(1, std::memory_order_relaxed), 1},
                                               .properties = Sora::Compute::MemoryProperty::DeviceLocal,
                                               .sizeBytes = sizeBytes,
                                               .alignment = 64,
                                               .state = native.release(),
                                               .destroy = DestroyAllocation});
        REQUIRE(allocation.has_value());
        return *allocation;
    }

    Sora::Compute::Buffer MakeBuffer(Sora::Compute::BufferId id) {
        auto buffer = Sora::Compute::Buffer::Create({.id = id,
                                                     .elementType = kFloat,
                                                     .layout = Sora::Compute::LayoutKind::Contiguous,
                                                     .rank = 1,
                                                     .sizeBytes = 64,
                                                     .alignment = 64});
        REQUIRE(buffer.has_value());
        return *buffer;
    }

    Sora::Compute::BufferView View(Sora::Compute::Buffer buffer) {
        constexpr std::array extents{uint64_t{16}};
        auto view = Sora::Compute::BufferView::Contiguous(std::move(buffer), extents);
        REQUIRE(view.has_value());
        return *view;
    }

    Sora::Compute::BufferSlice Slice(Sora::Compute::Buffer buffer) {
        const auto whole = Sora::Compute::ByteRegion::Make(0, buffer.Describe().sizeBytes);
        REQUIRE(whole.has_value());
        auto slice = Sora::Compute::BufferSlice::Create(std::move(buffer), *whole);
        REQUIRE(slice.has_value());
        return *slice;
    }

    Sora::Compute::BufferSlice Slice(Sora::Compute::Buffer buffer, Sora::Compute::ByteRegion region) {
        auto slice = Sora::Compute::BufferSlice::Create(std::move(buffer), region);
        REQUIRE(slice.has_value());
        return *slice;
    }

    Sora::Compute::ScalarArgument Alpha() {
        const auto bytes = std::bit_cast<std::array<std::byte, sizeof(float)>>(2.0F);
        auto alpha = Sora::Compute::ScalarArgument::Create(kFloat, bytes);
        REQUIRE(alpha.has_value());
        return *alpha;
    }

    Sora::Compute::PhysicalResourceCatalog
    MakeCatalog(MockState& state,
                Sora::Compute::ArtifactKind dispatchArtifactKind = Sora::Compute::ArtifactKind::DeviceExecutable,
                bool includePipelineFactory = true,
                Sora::Compute::ExecutableTargetDomain executableDomain =
                    Sora::Compute::ExecutableTargetDomain::BackendPortable) {
        auto queue = Sora::Compute::Queue::Adopt(Sora::Test::TestBackendService(),
                                                 {.id = {1, 1},
                                                  .device = kGpu,
                                                  .backend = Sora::Compute::BackendKind::Vulkan,
                                                  .capabilities = Sora::Compute::QueueCapability::Dispatch |
                                                                  Sora::Compute::QueueCapability::Copy |
                                                                  Sora::Compute::QueueCapability::Fill,
                                                  .dispatchArtifactKind = dispatchArtifactKind,
                                                  .state = new uint32_t{},
                                                  .destroy = DestroyQueue,
                                                  .submit = UnexpectedSubmit});
        REQUIRE(queue.has_value());
        auto allocator = Sora::Compute::MemoryAllocator::Create(
            Sora::Test::TestBackendService(), {.location = kDevice,
                                               .supportedProperties = Sora::Compute::MemoryProperty::DeviceLocal,
                                               .state = &state,
                                               .allocate = UnexpectedAllocate});
        REQUIRE(allocator.has_value());
        auto transfer = Sora::Compute::TransferEngine::Create(Sora::Test::TestBackendService(),
                                                              {.engine = {2, 2},
                                                               .sourceService = Sora::Test::TestBackendService().Id(),
                                                               .source = kHost,
                                                               .destination = kDevice,
                                                               .kind = Sora::Compute::TransferKind::Copy,
                                                               .completionDevice = kGpu,
                                                               .completionBackend = Sora::Compute::BackendKind::Vulkan,
                                                               .state = &state,
                                                               .transfer = UnexpectedTransfer});
        REQUIRE(transfer.has_value());
        auto alias = Sora::Compute::TransferEngine::Create(Sora::Test::TestBackendService(),
                                                           {.engine = {3, 3},
                                                            .sourceService = Sora::Test::TestBackendService().Id(),
                                                            .source = kSharedCpu,
                                                            .destination = kSharedGpu,
                                                            .kind = Sora::Compute::TransferKind::CoherentAlias,
                                                            .state = &state,
                                                            .transfer = UnexpectedTransfer});
        REQUIRE(alias.has_value());
        auto compiler = Sora::Compute::BackendCompiler::Create(Sora::Test::TestBackendService(),
                                                               {.identity = kCompilerIdentity,
                                                                .backend = Sora::Compute::BackendKind::Vulkan,
                                                                .executableDomain = executableDomain,
                                                                .state = &state,
                                                                .compile = UnexpectedCompile});
        REQUIRE(compiler.has_value());
        auto factory = Sora::Compute::BackendPipelineFactory::Create(
            Sora::Test::TestBackendService(), GpuTarget(),
            {.identity = kPipelineFactoryIdentity, .state = &state, .create = UnexpectedCreatePipeline});
        REQUIRE(factory.has_value());
        auto observer = Sora::Compute::DeviceObserver::Create(
            Sora::Test::TestBackendService(),
            {.device = kGpu, .backend = Sora::Compute::BackendKind::Vulkan, .state = &state, .observe = ObserveDevice});
        REQUIRE(observer.has_value());
        Sora::Compute::PhysicalResourceCatalogStorage storage{.deviceObservers = {*observer},
                                                              .queues = {*queue},
                                                              .allocators = {*allocator},
                                                              .transferEngines = {*transfer, *alias},
                                                              .compilers = {*compiler}};
        if (includePipelineFactory) {
            storage.pipelineFactories.push_back(*factory);
        }
        auto catalog = Sora::Compute::PhysicalResourceCatalog::Create(std::move(storage));
        REQUIRE(catalog.has_value());
        return std::move(*catalog);
    }

    Sora::Compute::PhysicalGraphPlanStorage MakeValidPlan(MockState& state, bool pipeline = false) {
        auto x = MakeBuffer({40, 1});
        auto y = MakeBuffer({40, 2});
        auto output = MakeBuffer({40, 3});
        const auto xView = View(x);
        const auto yView = View(y);
        const auto outputView = View(output);
        std::vector<Sora::Compute::KernelArgumentBinding> bindings;
        bindings.emplace_back(Alpha());
        bindings.emplace_back(xView);
        bindings.emplace_back(yView);
        bindings.emplace_back(outputView);
        constexpr std::array extents{uint64_t{16}};
        auto invocation = Sora::Compute::BindKernelProgram(kSaxpy, extents, std::move(bindings));
        REQUIRE(invocation.has_value());
        auto package = Sora::Compute::ProgramPackage::Create(kSaxpy);
        REQUIRE(package.has_value());
        const auto whole = Sora::Compute::ByteRegion::Make(0, 64);
        REQUIRE(whole.has_value());

        Sora::Compute::PhysicalGraphPlanStorage storage;
        storage.allocations.push_back(
            {.id = {0},
             .buffer = x,
             .service = kService,
             .location = kDevice,
             .bufferRegion = *whole,
             .version = 1,
             .materialization = Sora::Compute::PlannedAllocate{
                 .requiredProperties = Sora::Compute::MemoryProperty::DeviceLocal, .alignment = 64}});
        storage.allocations.push_back(
            {.id = {1},
             .buffer = output,
             .service = kService,
             .location = kDevice,
             .bufferRegion = *whole,
             .version = 1,
             .materialization =
                 Sora::Compute::PlannedAllocate{.requiredProperties = Sora::Compute::MemoryProperty::DeviceLocal,
                                                .alignment = 64},
             .publication = Sora::Compute::PlannedPublication{
                 .kind = Sora::Compute::PlannedPublicationKind::WholeWrite, .expectedVersion = 0, .producer = {1}}});
        storage.segments.emplace_back(Sora::Compute::PlannedTransfer{
            .logicalNode = {0},
            .service = kService,
            .engine = {2, 2},
            .buffer = x.Describe().id,
            .region = *whole,
            .version = 1,
            .source = Sora::Compute::ReadyStorageRef{x.Describe().id, {MakeAllocation(state, kHost), *whole, 0, 1}},
            .destination = {0}});
        storage.segments.emplace_back(Sora::Compute::PlannedDispatch{
            .logicalNode = {0},
            .queue =
                {.service = kService, .backend = Sora::Compute::BackendKind::Vulkan, .device = kGpu, .queue = {1, 1}},
            .program = *package,
            .invocation = *invocation,
            .slice = Sora::Compute::DispatchSlice::Whole(invocation->Domain()),
            .target = GpuTarget(),
            .geometry = {.workgroupCount = {1, 1, 1}},
            .pipeline = pipeline ? std::optional<Sora::Compute::PlannedPipeline>{{kPipelineFactoryIdentity,
                                                                                  kPipelineStateIdentity}}
                                 : std::nullopt,
            .bufferStorage = {
                {.input = Sora::Compute::PlannedStorageRef{Sora::Compute::PlannedAllocationRef{.allocation = {0},
                                                                                               .readyAfter = {0}}}},
                {.input = Sora::Compute::PlannedStorageRef{Sora::Compute::ReadyStorageRef{
                     y.Describe().id, {MakeAllocation(state, kDevice), *whole, 0, 1}}}},
                {.output = {1}},
            }});
        storage.edges.push_back({{0}, {1}});
        return storage;
    }

    Sora::Compute::PhysicalGraphPlanStorage MakePrefilledAssemblyPlan(MockState& state) {
        auto buffer = MakeBuffer({41, 1});
        const auto whole = Sora::Compute::ByteRegion::Make(0, 64);
        const std::array regions{*Sora::Compute::ByteRegion::Make(0, 32), *Sora::Compute::ByteRegion::Make(32, 32)};
        REQUIRE(whole.has_value());

        Sora::Compute::PhysicalGraphPlanStorage storage;
        storage.allocations.push_back(
            {.id = {0},
             .purpose = Sora::Compute::PlannedAllocationPurpose::DispatchInputAssembly,
             .buffer = buffer,
             .service = kService,
             .location = kDevice,
             .bufferRegion = *whole,
             .version = 0,
             .materialization = Sora::Compute::PlannedAllocate{
                 .requiredProperties = Sora::Compute::MemoryProperty::DeviceLocal, .alignment = 64}});
        std::vector<Sora::Compute::PlannedAssemblyPart> parts;
        for (uint32_t index = 0; index < regions.size(); ++index) {
            auto replica =
                Sora::Compute::ResidentReplica::Create(MakeAllocation(state, kHost, regions[index].Size()),
                                                       Sora::Compute::ByteCoverageView::Interval(regions[index]), 0, 1);
            REQUIRE(replica.has_value());
            storage.segments.emplace_back(
                Sora::Compute::PlannedTransfer{.logicalNode = {0},
                                               .service = kService,
                                               .engine = {2, 2},
                                               .buffer = buffer.Describe().id,
                                               .region = regions[index],
                                               .version = 1,
                                               .source = Sora::Compute::ReadyStorageRef{buffer.Describe().id, *replica},
                                               .destination = {0},
                                               .target = Sora::Compute::PlannedTransferTarget::AssemblyPart});
            parts.push_back({.transferProducer = {index}, .region = regions[index]});
        }
        storage.segments.emplace_back(Sora::Compute::PlannedAssembly{
            .logicalNode = {0},
            .queue =
                {.service = kService, .backend = Sora::Compute::BackendKind::Vulkan, .device = kGpu, .queue = {1, 1}},
            .destination = {0},
            .parts = std::move(parts)});
        storage.edges = {{{0}, {2}}, {{1}, {2}}};
        return storage;
    }

} // namespace

TEST_CASE("PhysicalGraphPlan freezes a transfer-to-dispatch pipeline without "
          "backend side effects",
          "[Sora][Compute][Runtime][PhysicalGraphPlan]") {
    MockState state;
    auto catalog = MakeCatalog(state);
    auto plan = Sora::Compute::PhysicalGraphPlan::Create(MakeValidPlan(state), catalog);
    REQUIRE(plan.has_value());
    REQUIRE(plan->Allocations().size() == 2);
    REQUIRE(plan->Segments().size() == 2);
    REQUIRE(plan->Edges().size() == 1);
    REQUIRE(state.backendCallCount.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("Pipeline binding layout identity isolates the immutable backend "
          "binding ABI",
          "[Sora][Compute][Runtime][PhysicalGraphPlan][Pipeline][Identity]") {
    const auto identity = Sora::Compute::PipelineBindingLayoutIdentity(kSaxpy.Arguments());
    REQUIRE(Sora::Compute::ValidSemanticHash(identity));

    auto arguments = kSaxpy.arguments;
    ++arguments[1].aliasClass.value;
    REQUIRE(Sora::Compute::PipelineBindingLayoutIdentity(arguments) == identity);
    ++arguments[1].minimumAlignment;
    REQUIRE_FALSE(Sora::Compute::PipelineBindingLayoutIdentity(arguments) == identity);
}

TEST_CASE("PhysicalGraphPlan models coherent alias materialization, queue "
          "copy, and fill",
          "[Sora][Compute][Runtime][PhysicalGraphPlan][Operations]") {
    MockState state;
    auto catalog = MakeCatalog(state);
    const auto whole = Sora::Compute::ByteRegion::Make(0, 64);
    REQUIRE(whole.has_value());

    SECTION("coherent alias feeds queue copy without a synthetic transfer segment") {
        auto source = MakeBuffer({50, 1});
        auto destination = MakeBuffer({50, 2});
        auto copy = Sora::Compute::CopyOperation::Create(Slice(source), Slice(destination));
        REQUIRE(copy.has_value());
        Sora::Compute::PhysicalGraphPlanStorage storage;
        storage.allocations.push_back(
            {.id = {0},
             .buffer = source,
             .service = kService,
             .location = kSharedGpu,
             .bufferRegion = *whole,
             .version = 1,
             .materialization = Sora::Compute::PlannedCoherentAlias{
                 .engine = {3, 3},
                 .source = Sora::Compute::ReadyStorageRef{source.Describe().id,
                                                          {MakeAllocation(state, kSharedCpu), *whole, 0, 1}}}});
        storage.allocations.push_back(
            {.id = {1},
             .buffer = destination,
             .service = kService,
             .location = kDevice,
             .bufferRegion = *whole,
             .version = 1,
             .materialization =
                 Sora::Compute::PlannedAllocate{.requiredProperties = Sora::Compute::MemoryProperty::DeviceLocal,
                                                .alignment = 64},
             .publication = Sora::Compute::PlannedPublication{
                 .kind = Sora::Compute::PlannedPublicationKind::WholeWrite, .expectedVersion = 0, .producer = {0}}});
        storage.segments.emplace_back(Sora::Compute::PlannedCopy{
            .logicalNode = {0},
            .queue =
                {.service = kService, .backend = Sora::Compute::BackendKind::Vulkan, .device = kGpu, .queue = {1, 1}},
            .operation = *copy,
            .source = Sora::Compute::PlannedAllocationRef{.allocation = {0}},
            .destination = {1}});
        const auto plan = Sora::Compute::PhysicalGraphPlan::Create(std::move(storage), catalog);
        REQUIRE(plan.has_value());
        REQUIRE(plan->Segments().size() == 1);
        REQUIRE(plan->Edges().empty());
        REQUIRE(state.backendCallCount.load(std::memory_order_relaxed) == 0);
    }

    SECTION("fill produces one publishable allocation") {
        auto destination = MakeBuffer({51, 1});
        constexpr std::array patternBytes{std::byte{0xA5}};
        auto pattern = Sora::Compute::FillPattern::Create(patternBytes);
        REQUIRE(pattern.has_value());
        auto fill = Sora::Compute::FillOperation::Create(Slice(destination), *pattern);
        REQUIRE(fill.has_value());
        Sora::Compute::PhysicalGraphPlanStorage storage;
        storage.allocations.push_back(
            {.id = {0},
             .buffer = destination,
             .service = kService,
             .location = kDevice,
             .bufferRegion = *whole,
             .version = 1,
             .materialization =
                 Sora::Compute::PlannedAllocate{.requiredProperties = Sora::Compute::MemoryProperty::DeviceLocal,
                                                .alignment = 64},
             .publication = Sora::Compute::PlannedPublication{
                 .kind = Sora::Compute::PlannedPublicationKind::WholeWrite, .expectedVersion = 0, .producer = {0}}});
        storage.segments.emplace_back(Sora::Compute::PlannedFill{
            .logicalNode = {0},
            .queue =
                {.service = kService, .backend = Sora::Compute::BackendKind::Vulkan, .device = kGpu, .queue = {1, 1}},
            .operation = *fill,
            .destination = {0}});
        const auto plan = Sora::Compute::PhysicalGraphPlan::Create(std::move(storage), catalog);
        REQUIRE(plan.has_value());
        REQUIRE(plan->Segments().size() == 1);
        REQUIRE(state.backendCallCount.load(std::memory_order_relaxed) == 0);
    }

    SECTION("two partial fills jointly publish one exact logical version") {
        auto destination = MakeBuffer({51, 2});
        const auto first = Sora::Compute::ByteRegion::Make(0, 32);
        const auto second = Sora::Compute::ByteRegion::Make(32, 32);
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        constexpr std::array patternBytes{std::byte{0x3C}};
        auto pattern = Sora::Compute::FillPattern::Create(patternBytes);
        REQUIRE(pattern.has_value());

        Sora::Compute::PhysicalGraphPlanStorage storage;
        const std::array regions{*first, *second};
        for (uint32_t index = 0; index < regions.size(); ++index) {
            auto fill = Sora::Compute::FillOperation::Create(Slice(destination, regions[index]), *pattern);
            REQUIRE(fill.has_value());
            storage.allocations.push_back(
                {.id = {index},
                 .buffer = destination,
                 .service = kService,
                 .location = kDevice,
                 .bufferRegion = regions[index],
                 .version = 1,
                 .materialization =
                     Sora::Compute::PlannedAllocate{.requiredProperties = Sora::Compute::MemoryProperty::DeviceLocal,
                                                    .alignment = 64},
                 .publication =
                     Sora::Compute::PlannedPublication{.kind = Sora::Compute::PlannedPublicationKind::PartitionedWrite,
                                                       .expectedVersion = 0,
                                                       .producer = {index},
                                                       .partitionIndex = index,
                                                       .partitionCount = static_cast<uint32_t>(regions.size())}});
            storage.segments.emplace_back(
                Sora::Compute::PlannedFill{.logicalNode = {index},
                                           .queue = {.service = kService,
                                                     .backend = Sora::Compute::BackendKind::Vulkan,
                                                     .device = kGpu,
                                                     .queue = {1, 1}},
                                           .operation = *fill,
                                           .destination = {index}});
        }

        const auto plan = Sora::Compute::PhysicalGraphPlan::Create(std::move(storage), catalog);
        REQUIRE(plan.has_value());
        REQUIRE(plan->Allocations().size() == 2);
        REQUIRE(plan->Segments().size() == 2);
        REQUIRE(plan->Edges().empty());
        REQUIRE(state.backendCallCount.load(std::memory_order_relaxed) == 0);
    }

    SECTION("bounded publication proof reports capacity instead of invalid coverage") {
        constexpr std::array extents{uint64_t{1'000'000}};
        constexpr std::array strides{uint64_t{8}};
        constexpr auto footprint =
            Sora::Compute::AffineByteFootprint::Create(extents, strides, sizeof(float), 0, sizeof(float), 1);
        static_assert(footprint.has_value());
        auto destination = Sora::Compute::Buffer::Create({.id = {51, 3},
                                                          .elementType = {Sora::Compute::ScalarKind::Float32, 1},
                                                          .layout = Sora::Compute::LayoutKind::Strided,
                                                          .rank = 1,
                                                          .sizeBytes = footprint->BoundingRegion().End(),
                                                          .alignment = 64},
                                                         *footprint);
        REQUIRE(destination.has_value());
        constexpr std::array patternBytes{std::byte{0x5A}};
        const auto pattern = Sora::Compute::FillPattern::Create(patternBytes);
        REQUIRE(pattern.has_value());

        Sora::Compute::PhysicalGraphPlanStorage storage;
        constexpr std::array offsets{uint64_t{0}, uint64_t{8}};
        for (uint32_t index = 0; index < offsets.size(); ++index) {
            const auto region = Sora::Compute::ByteRegion::Make(offsets[index], sizeof(float));
            REQUIRE(region.has_value());
            auto fill = Sora::Compute::FillOperation::Create(Slice(*destination, *region), *pattern);
            REQUIRE(fill.has_value());
            storage.allocations.push_back(
                {.id = {index},
                 .buffer = *destination,
                 .service = kService,
                 .location = kDevice,
                 .bufferRegion = *region,
                 .version = 1,
                 .materialization =
                     Sora::Compute::PlannedAllocate{.requiredProperties = Sora::Compute::MemoryProperty::DeviceLocal,
                                                    .alignment = 64},
                 .publication =
                     Sora::Compute::PlannedPublication{.kind = Sora::Compute::PlannedPublicationKind::PartitionedWrite,
                                                       .expectedVersion = 0,
                                                       .producer = {index},
                                                       .partitionIndex = index,
                                                       .partitionCount = static_cast<uint32_t>(offsets.size())}});
            storage.segments.emplace_back(
                Sora::Compute::PlannedFill{.logicalNode = {index},
                                           .queue = {.service = kService,
                                                     .backend = Sora::Compute::BackendKind::Vulkan,
                                                     .device = kGpu,
                                                     .queue = {1, 1}},
                                           .operation = *fill,
                                           .destination = {index}});
        }
        const auto plan = Sora::Compute::PhysicalGraphPlan::Create(std::move(storage), catalog);
        REQUIRE_FALSE(plan.has_value());
        REQUIRE(plan.error().code == Sora::Compute::PhysicalGraphPlanErrorCode::CapacityExceeded);
    }
}

TEST_CASE("PhysicalGraphPlan reports assembly command projection capacity explicitly",
          "[Sora][Compute][Runtime][PhysicalGraphPlan][Assembly][Capacity]") {
    MockState state;
    auto catalog = MakeCatalog(state);
    constexpr std::array extents{uint64_t{257}, uint64_t{2}};
    constexpr std::array strides{uint64_t{16}, uint64_t{4}};
    constexpr auto target = Sora::Compute::AffineByteFootprint::Create(extents, strides, 4, 0, 4, 1);
    constexpr std::array columnExtents{uint64_t{257}, uint64_t{1}};
    constexpr auto first = Sora::Compute::AffineByteFootprint::Create(columnExtents, strides, 4, 0, 4, 1);
    constexpr auto second = Sora::Compute::AffineByteFootprint::Create(columnExtents, strides, 4, 4, 4, 1);
    static_assert(target && first && second);

    auto buffer = Sora::Compute::Buffer::Create({.id = {11, 1},
                                                 .elementType = kFloat,
                                                 .layout = Sora::Compute::LayoutKind::Strided,
                                                 .rank = 2,
                                                 .sizeBytes = target->BoundingRegion().End(),
                                                 .alignment = 64},
                                                *target);
    REQUIRE(buffer.has_value());
    auto firstReplica =
        Sora::Compute::ResidentReplica::Create(MakeAllocation(state, kDevice, first->BoundingRegion().Size()),
                                               Sora::Compute::ByteCoverageView::Affine(*first), 0, 1);
    auto secondReplica =
        Sora::Compute::ResidentReplica::Create(MakeAllocation(state, kDevice, second->BoundingRegion().Size()),
                                               Sora::Compute::ByteCoverageView::Affine(*second), 0, 1);
    REQUIRE(firstReplica.has_value());
    REQUIRE(secondReplica.has_value());

    Sora::Compute::PhysicalGraphPlanStorage storage;
    storage.allocations.push_back(
        {.id = {0},
         .purpose = Sora::Compute::PlannedAllocationPurpose::DispatchInputAssembly,
         .buffer = *buffer,
         .service = kService,
         .location = kDevice,
         .bufferRegion = target->BoundingRegion(),
         .byteFootprint = *target,
         .version = 0,
         .materialization = Sora::Compute::PlannedAllocate{
             .requiredProperties = Sora::Compute::MemoryProperty::DeviceLocal, .alignment = 64}});
    storage.segments.emplace_back(Sora::Compute::PlannedAssembly{
        .logicalNode = {0},
        .queue = {.service = kService, .backend = Sora::Compute::BackendKind::Vulkan, .device = kGpu, .queue = {1, 1}},
        .destination = {0},
        .parts = {
            {.source = Sora::Compute::ReadyStorageRef{buffer->Describe().id, *firstReplica},
             .region = first->BoundingRegion(),
             .byteFootprint = *first},
            {.source = Sora::Compute::ReadyStorageRef{buffer->Describe().id, *secondReplica},
             .region = second->BoundingRegion(),
             .byteFootprint = *second},
        }});
    const auto plan = Sora::Compute::PhysicalGraphPlan::Create(std::move(storage), catalog);
    REQUIRE_FALSE(plan.has_value());
    REQUIRE(plan.error().code == Sora::Compute::PhysicalGraphPlanErrorCode::CapacityExceeded);
    REQUIRE(plan.error().segment == 0);
    REQUIRE(state.backendCallCount.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("PhysicalGraphPlan rejects missing data dependencies and false producers",
          "[Sora][Compute][Runtime][PhysicalGraphPlan][Validation]") {
    MockState state;
    auto catalog = MakeCatalog(state);

    SECTION("missing transfer-to-dispatch edge") {
        auto storage = MakeValidPlan(state);
        storage.edges.clear();
        const auto plan = Sora::Compute::PhysicalGraphPlan::Create(std::move(storage), catalog);
        REQUIRE_FALSE(plan.has_value());
        REQUIRE(plan.error().code == Sora::Compute::PhysicalGraphPlanErrorCode::MissingDataDependency);
    }
    SECTION("publication producer does not write the allocation") {
        auto storage = MakeValidPlan(state);
        storage.allocations[1].publication->producer = {0};
        const auto plan = Sora::Compute::PhysicalGraphPlan::Create(std::move(storage), catalog);
        REQUIRE_FALSE(plan.has_value());
        REQUIRE(plan.error().code == Sora::Compute::PhysicalGraphPlanErrorCode::InvalidPublication);
    }
    SECTION("physical resources are absent") {
        auto emptyCatalog = Sora::Compute::PhysicalResourceCatalog::Create({});
        REQUIRE(emptyCatalog.has_value());
        const auto plan = Sora::Compute::PhysicalGraphPlan::Create(MakeValidPlan(state), *emptyCatalog);
        REQUIRE_FALSE(plan.has_value());
        REQUIRE(plan.error().code == Sora::Compute::PhysicalGraphPlanErrorCode::MissingPhysicalResource);
    }
    SECTION("compiler executable domain differs from the dispatch target") {
        auto mismatchedCatalog = MakeCatalog(state, Sora::Compute::ArtifactKind::DeviceExecutable, true,
                                             Sora::Compute::ExecutableTargetDomain::TargetNative);
        const auto plan = Sora::Compute::PhysicalGraphPlan::Create(MakeValidPlan(state), mismatchedCatalog);
        REQUIRE_FALSE(plan.has_value());
        REQUIRE(plan.error().code == Sora::Compute::PhysicalGraphPlanErrorCode::InvalidSegment);
    }
    SECTION("dispatch geometry contains a zero launch dimension") {
        auto storage = MakeValidPlan(state);
        std::get<Sora::Compute::PlannedDispatch>(storage.segments[1]).geometry.workgroupCount[0] = 0;
        const auto plan = Sora::Compute::PhysicalGraphPlan::Create(std::move(storage), catalog);
        REQUIRE_FALSE(plan.has_value());
        REQUIRE(plan.error().code == Sora::Compute::PhysicalGraphPlanErrorCode::InvalidSegment);
    }
    SECTION("executable queue rejects a pipeline plan") {
        auto storage = MakeValidPlan(state);
        std::get<Sora::Compute::PlannedDispatch>(storage.segments[1]).pipeline =
            Sora::Compute::PlannedPipeline{kPipelineFactoryIdentity, kPipelineStateIdentity};
        const auto plan = Sora::Compute::PhysicalGraphPlan::Create(std::move(storage), catalog);
        REQUIRE_FALSE(plan.has_value());
        REQUIRE(plan.error().code == Sora::Compute::PhysicalGraphPlanErrorCode::InvalidSegment);
    }
    SECTION("pipeline queue requires an explicit pipeline plan") {
        auto pipelineCatalog = MakeCatalog(state, Sora::Compute::ArtifactKind::DevicePipeline);
        const auto plan = Sora::Compute::PhysicalGraphPlan::Create(MakeValidPlan(state), pipelineCatalog);
        REQUIRE_FALSE(plan.has_value());
        REQUIRE(plan.error().code == Sora::Compute::PhysicalGraphPlanErrorCode::InvalidSegment);
    }
    SECTION("pipeline queue accepts a valid factory and state") {
        auto pipelineCatalog = MakeCatalog(state, Sora::Compute::ArtifactKind::DevicePipeline);
        const auto plan = Sora::Compute::PhysicalGraphPlan::Create(MakeValidPlan(state, true), pipelineCatalog);
        REQUIRE(plan.has_value());
    }
    SECTION("pipeline queue rejects a missing factory") {
        auto pipelineCatalog = MakeCatalog(state, Sora::Compute::ArtifactKind::DevicePipeline, false);
        const auto plan = Sora::Compute::PhysicalGraphPlan::Create(MakeValidPlan(state, true), pipelineCatalog);
        REQUIRE_FALSE(plan.has_value());
        REQUIRE(plan.error().code == Sora::Compute::PhysicalGraphPlanErrorCode::MissingPhysicalResource);
    }
    SECTION("whole-write allocation alignment is weaker than the logical buffer "
            "alignment") {
        auto storage = MakeValidPlan(state);
        std::get<Sora::Compute::PlannedAllocate>(storage.allocations[1].materialization).alignment = 32;
        const auto plan = Sora::Compute::PhysicalGraphPlan::Create(std::move(storage), catalog);
        REQUIRE_FALSE(plan.has_value());
        REQUIRE(plan.error().code == Sora::Compute::PhysicalGraphPlanErrorCode::InvalidPublication);
        REQUIRE(plan.error().related == 1);
    }
    SECTION("whole-write publication conflicts retain the earliest related "
            "allocation") {
        auto destination = MakeBuffer({52, 1});
        const auto whole = Sora::Compute::ByteRegion::Make(0, destination.Describe().sizeBytes);
        REQUIRE(whole.has_value());
        constexpr std::array patternBytes{std::byte{0x5A}};
        auto pattern = Sora::Compute::FillPattern::Create(patternBytes);
        REQUIRE(pattern.has_value());
        auto fill = Sora::Compute::FillOperation::Create(Slice(destination), *pattern);
        REQUIRE(fill.has_value());

        Sora::Compute::PhysicalGraphPlanStorage storage;
        for (uint32_t index = 0; index < 2; ++index) {
            storage.allocations.push_back(
                {.id = {index},
                 .buffer = destination,
                 .service = kService,
                 .location = kDevice,
                 .bufferRegion = *whole,
                 .version = 1,
                 .materialization =
                     Sora::Compute::PlannedAllocate{.requiredProperties = Sora::Compute::MemoryProperty::DeviceLocal,
                                                    .alignment = 64},
                 .publication =
                     Sora::Compute::PlannedPublication{.kind = Sora::Compute::PlannedPublicationKind::WholeWrite,
                                                       .expectedVersion = 0,
                                                       .producer = {index}}});
            storage.segments.emplace_back(
                Sora::Compute::PlannedFill{.logicalNode = {index},
                                           .queue = {.service = kService,
                                                     .backend = Sora::Compute::BackendKind::Vulkan,
                                                     .device = kGpu,
                                                     .queue = {1, 1}},
                                           .operation = *fill,
                                           .destination = {index}});
        }

        const auto plan = Sora::Compute::PhysicalGraphPlan::Create(std::move(storage), catalog);
        REQUIRE_FALSE(plan.has_value());
        REQUIRE(plan.error() == Sora::Compute::PhysicalGraphPlanError{
                                    .code = Sora::Compute::PhysicalGraphPlanErrorCode::InvalidPublication,
                                    .segment = 1,
                                    .related = 0,
                                });
    }
}

TEST_CASE("PhysicalGraphPlan validates direct-fill assembly producers as an exact event conjunction",
          "[Sora][Compute][Runtime][PhysicalGraphPlan][Assembly][Prefill]") {
    MockState state;
    auto catalog = MakeCatalog(state);

    SECTION("two disjoint producers form one assembly") {
        auto plan = Sora::Compute::PhysicalGraphPlan::Create(MakePrefilledAssemblyPlan(state), catalog);
        REQUIRE(plan.has_value());
        REQUIRE(state.backendCallCount.load(std::memory_order_relaxed) == 0);
    }
    SECTION("every prefill producer requires a direct edge") {
        auto storage = MakePrefilledAssemblyPlan(state);
        storage.edges.pop_back();
        const auto plan = Sora::Compute::PhysicalGraphPlan::Create(std::move(storage), catalog);
        REQUIRE_FALSE(plan.has_value());
        REQUIRE(plan.error().code == Sora::Compute::PhysicalGraphPlanErrorCode::MissingDataDependency);
        REQUIRE(plan.error().related == 1);
    }
    SECTION("part coverage must equal its transfer producer coverage") {
        auto storage = MakePrefilledAssemblyPlan(state);
        auto& assembly = std::get<Sora::Compute::PlannedAssembly>(storage.segments[2]);
        assembly.parts[1].region = assembly.parts[0].region;
        const auto plan = Sora::Compute::PhysicalGraphPlan::Create(std::move(storage), catalog);
        REQUIRE_FALSE(plan.has_value());
        REQUIRE(plan.error().code == Sora::Compute::PhysicalGraphPlanErrorCode::InvalidSegment);
    }
    SECTION("overlapping direct-fill targets are rejected") {
        auto storage = MakePrefilledAssemblyPlan(state);
        auto& first = std::get<Sora::Compute::PlannedTransfer>(storage.segments[0]);
        auto& second = std::get<Sora::Compute::PlannedTransfer>(storage.segments[1]);
        second.region = first.region;
        second.source = first.source;
        auto& assembly = std::get<Sora::Compute::PlannedAssembly>(storage.segments[2]);
        assembly.parts[1].region = first.region;
        const auto plan = Sora::Compute::PhysicalGraphPlan::Create(std::move(storage), catalog);
        REQUIRE_FALSE(plan.has_value());
        REQUIRE(plan.error().code == Sora::Compute::PhysicalGraphPlanErrorCode::InvalidSegment);
    }
}

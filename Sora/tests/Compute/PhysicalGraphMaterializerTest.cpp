#include "BackendTestSupport.h"

#include <Sora/Compute/Execution/ComputeContext.h>
#include <Sora/Compute/Graph/GraphBuilder.h>
#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>
#include <Sora/Compute/Runtime/PhysicalGraphMaterializer.h>

#include <catch2/catch_test_macros.hpp>
#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

namespace {

    constexpr Sora::Compute::ValueType kFloat{Sora::Compute::ScalarKind::Float32, 1};
    constexpr Sora::Compute::BackendServiceId kService{0x534F52415F544553ULL, 0x545F4241434B454EULL};
    constexpr Sora::Compute::DeviceId kCpu{1, 1};
    constexpr Sora::Compute::DeviceId kGpu{2, 2};
    constexpr Sora::Compute::MemoryLocation kHost{
        .domain = {10, 10}, .device = kCpu, .backend = Sora::Compute::BackendKind::Cpu};
    constexpr Sora::Compute::MemoryLocation kSharedCpu{
        .domain = {20, 20}, .device = kCpu, .backend = Sora::Compute::BackendKind::Cpu};
    constexpr Sora::Compute::MemoryLocation kSharedGpu{
        .domain = {20, 20}, .device = kGpu, .backend = Sora::Compute::BackendKind::Vulkan};
    constexpr Sora::Compute::MemoryLocation kDevice{
        .domain = {30, 30}, .device = kGpu, .backend = Sora::Compute::BackendKind::Vulkan};
    constexpr Sora::Compute::SemanticHash kCompilerIdentity{40, 41};
    constexpr Sora::Compute::SemanticHash kPipelineFactoryIdentity{42, 43};
    constexpr Sora::Compute::SemanticHash kPipelineStateIdentity{44, 45};
    constexpr Sora::Compute::TransferEngineId kCopyEngine{50, 51};
    constexpr Sora::Compute::TransferEngineId kAliasEngine{52, 53};
    constexpr Sora::Compute::QueueId kQueue{60, 61};
    constexpr Sora::Compute::TargetFingerprint kGpuTarget{
        .device = kGpu,
        .backend = Sora::Compute::BackendKind::Vulkan,
        .hostArchitecture = Sora::CpuArchitecture::X64,
        .operatingSystem = Sora::OperatingSystem::Windows,
        .binaryFormat = Sora::BinaryFormat::PE,
        .byteOrder = Sora::ByteOrder::Little,
        .abi = Sora::AbiFamily::MSVC,
        .identityStability = Sora::Compute::TargetIdentityStability::MachineStable,
        .executableDomain = Sora::Compute::ExecutableTargetDomain::BackendPortable,
        .addressBits = 64,
        .features = Sora::Compute::DeviceFeature::TimelineEvents,
        .architectureId = 71,
        .compilerIdentity = kCompilerIdentity,
        .driverIdentity = {72, 1},
    };
    constexpr std::array kExecutableSeedBytes{std::byte{0x53}, std::byte{0x45}, std::byte{0x58}, std::byte{0x45}};
    constexpr std::array kPipelineSeedBytes{std::byte{0x53}, std::byte{0x50}, std::byte{0x49}, std::byte{0x50}};

    class TemporaryDirectory final {
    public:
        TemporaryDirectory() {
            static std::atomic<uint64_t> sequence = 0;
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path_ =
                std::filesystem::temp_directory_path() / std::format("sora-compute-context-store-test-{}-{}", stamp,
                                                                     sequence.fetch_add(1, std::memory_order_relaxed));
        }

        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }

        TemporaryDirectory(const TemporaryDirectory&) = delete;
        TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

        [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

    private:
        std::filesystem::path path_;
    };

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

    constexpr uint64_t kStridedElementCount = 16;
    constexpr uint64_t kStridedBufferBytes = (kStridedElementCount - 1) * 8 + sizeof(float);
    constexpr std::array kStridedExtents{kStridedElementCount};
    constexpr std::array kStridedByteStrides{uint64_t{8}};
    constexpr auto kStridedFootprint = Sora::Compute::AffineByteFootprint::Create(kStridedExtents, kStridedByteStrides,
                                                                                  sizeof(float), 0, sizeof(float), 1);
    static_assert(kStridedFootprint.has_value());

    consteval auto MakeStridedCopyProgram() {
        Sora::Compute::KernelBuilder<1, 1> scalar;
        const auto value = scalar.AddValueParameter(kFloat);
        Sora::Compute::KernelProgramBuilder<2, 1, 1> program{Sora::Compute::IndexDomain{.rank = 1},
                                                             std::move(scalar).Finish(value),
                                                             Sora::Compute::PartitionContract::Elementwise(1)};
        const auto input = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, 1,
                                                     Sora::Compute::LayoutKind::Strided, 64, {1});
        const auto output = program.AddBufferArgument(Sora::Compute::AccessMode::Write, kFloat, 1,
                                                      Sora::Compute::LayoutKind::Strided, 64, {1});
        program.BindArgument(input);
        return std::move(program).Finish(output);
    }

    constexpr auto kStridedCopy = MakeStridedCopyProgram();

    struct SubmittedCommandObservation {
        Sora::Compute::BackendCommandKind kind = Sora::Compute::BackendCommandKind::Dispatch;
        Sora::Compute::PhysicalMemoryId source{};
        Sora::Compute::PhysicalMemoryId target{};
        uint64_t sourceOffset = 0;
        uint64_t targetOffset = 0;
        uint64_t sizeBytes = 0;
    };

    struct MockState {
        uint32_t allocateCount = 0;
        uint32_t aliasCount = 0;
        uint32_t transferCount = 0;
        uint32_t transferRegionCount = 0;
        uint32_t compileCount = 0;
        uint32_t artifactImportCount = 0;
        uint32_t artifactExportCount = 0;
        uint32_t pipelineCreateCount = 0;
        uint32_t pipelineImportCount = 0;
        uint32_t pipelineExportCount = 0;
        uint32_t submitCount = 0;
        uint32_t artifactDestroyCount = 0;
        uint32_t eventDestroyCount = 0;
        uint32_t eventWaitCount = 0;
        Sora::Compute::DeviceId transferCompletionDevice = kGpu;
        Sora::Compute::BackendKind transferCompletionBackend = Sora::Compute::BackendKind::Vulkan;
        uint32_t nextAllocationId = 1;
        uint64_t nextMemory = 1;
        uint32_t submissionFailuresRemaining = 0;
        Sora::Compute::SubmissionErrorCode submissionFailure = Sora::Compute::SubmissionErrorCode::QueueUnavailable;
        bool failCompile = false;
        bool failPipeline = false;
        bool failAlias = false;
        bool overlapAllocations = false;
        Sora::Compute::DeviceState deviceState = Sora::Compute::DeviceState::Available;
        uint64_t deviceGeneration = 1;
        std::array<uint32_t, 16> destroyedAllocations{};
        uint32_t destroyedAllocationCount = 0;
        std::array<Sora::Compute::BackendCommandKind, 8> submittedCommands{};
        std::array<uint32_t, 8> submittedCommandCounts{};
        std::array<uint32_t, 8> submittedBindingCounts{};
        std::array<uint32_t, 8> submittedWaitCounts{};
        std::array<std::array<SubmittedCommandObservation, 8>, 8> submittedCommandDetails{};
        std::array<Sora::Compute::PhysicalMemoryId, 8> transferSources{};
        std::array<Sora::Compute::PhysicalMemoryId, 8> transferTargets{};
        std::array<Sora::Compute::TransferRegion, 256> transferRegions{};
        std::array<uint32_t, 3> dispatchWorkgroups{};
        uint32_t dispatchReadBindingCount = 0;
        Sora::Compute::BackendBinding dispatchReadBinding{};
        Sora::Compute::ArtifactKind submittedDispatchArtifactKind = Sora::Compute::ArtifactKind::PortableLowering;
        Sora::Compute::SemanticHash pipelineBindingLayout{};
    };

    void ObserveDevice(void* opaque, Sora::Compute::DeviceSnapshot* snapshot) noexcept {
        const auto& state = *static_cast<const MockState*>(opaque);
        *snapshot = {.id = kGpu,
                     .state = state.deviceState,
                     .generation = state.deviceGeneration,
                     .observationSequence = state.submitCount,
                     .availableMemoryBytes = 1ULL << 30};
    }

    struct NativeAllocation {
        MockState* owner = nullptr;
        uint32_t id = 0;
        bool tracked = false;
    };

    struct NativeArtifact {
        MockState* owner = nullptr;
    };

    struct NativeArtifactBytes {
        std::array<std::byte, 4> bytes{};
    };

    struct NativeEvent {
        MockState* owner = nullptr;
        Sora::Compute::EventStatus status = Sora::Compute::EventStatus::Pending;
    };

    void DestroyAllocation(void* opaque) noexcept {
        std::unique_ptr<NativeAllocation> allocation{static_cast<NativeAllocation*>(opaque)};
        if (allocation->tracked) {
            allocation->owner->destroyedAllocations[allocation->owner->destroyedAllocationCount++] = allocation->id;
        }
    }

    void DestroyArtifact(void* opaque) noexcept {
        std::unique_ptr<NativeArtifact> artifact{static_cast<NativeArtifact*>(opaque)};
        ++artifact->owner->artifactDestroyCount;
    }

    void DestroyArtifactBytes(void* opaque) noexcept {
        delete static_cast<NativeArtifactBytes*>(opaque);
    }

    void DestroyEvent(void* opaque) noexcept {
        std::unique_ptr<NativeEvent> event{static_cast<NativeEvent*>(opaque)};
        ++event->owner->eventDestroyCount;
    }

    void QueryEvent(void* opaque, Sora::Compute::BackendEventObservation* observation) noexcept {
        const auto& event = *static_cast<NativeEvent*>(opaque);
        observation->status = event.status;
        observation->error = {};
    }

    void WaitEvent(void* opaque, uint64_t, Sora::Compute::BackendEventObservation* observation) noexcept {
        auto& event = *static_cast<NativeEvent*>(opaque);
        ++event.owner->eventWaitCount;
        event.status = Sora::Compute::EventStatus::Completed;
        observation->status = event.status;
        observation->error = {};
    }

    Sora::Compute::BackendEventPayload
    MakeEventPayload(MockState& state, Sora::Compute::DeviceId device = kGpu,
                     Sora::Compute::BackendKind backend = Sora::Compute::BackendKind::Vulkan) noexcept {
        auto event = std::unique_ptr<NativeEvent>{new (std::nothrow) NativeEvent{&state}};
        if (!event) {
            return {};
        }
        return {.device = device,
                .backend = backend,
                .state = event.release(),
                .destroy = DestroyEvent,
                .query = QueryEvent,
                .wait = WaitEvent};
    }

    void Allocate(void* opaque, const Sora::Compute::BackendAllocationRequest* request,
                  Sora::Compute::BackendAllocationResult* result) noexcept {
        auto& state = *static_cast<MockState*>(opaque);
        ++state.allocateCount;
        auto native = std::unique_ptr<NativeAllocation>{new (std::nothrow)
                                                            NativeAllocation{&state, state.nextAllocationId++, true}};
        if (!native) {
            result->error = {Sora::Compute::MemoryAllocationErrorCode::OutOfMemory, 0};
            return;
        }
        result->allocation = {
            .location = request->location,
            .memory = state.overlapAllocations ? Sora::Compute::PhysicalMemoryId{77, 77}
                                               : Sora::Compute::PhysicalMemoryId{state.nextMemory++, 1},
            .properties = request->requiredProperties | Sora::Compute::MemoryProperty::HostVisible,
            .sizeBytes = request->sizeBytes,
            .alignment = request->alignment,
            .state = native.release(),
            .destroy = DestroyAllocation,
        };
        result->error = {Sora::Compute::MemoryAllocationErrorCode::None, 0};
    }

    void Transfer(void* opaque, const Sora::Compute::BackendTransferRequest* request,
                  Sora::Compute::BackendTransferResult* result) noexcept {
        auto& state = *static_cast<MockState*>(opaque);
        const uint32_t transfer = state.transferCount++;
        state.transferSources[transfer] = request->source.memory;
        state.transferTargets[transfer] = request->target.memory;
        state.transferRegionCount = request->regionCount;
        for (size_t index = 0; index < std::min(request->Regions().size(), state.transferRegions.size()); ++index) {
            state.transferRegions[index] = request->Regions()[index];
        }
        result->completion = MakeEventPayload(state, state.transferCompletionDevice, state.transferCompletionBackend);
        result->error = result->completion.state != nullptr
                            ? Sora::Compute::TransferError{Sora::Compute::TransferErrorCode::None, 0}
                            : Sora::Compute::TransferError{Sora::Compute::TransferErrorCode::OutOfMemory, 0};
    }

    void ImportAlias(void* opaque, const Sora::Compute::BackendTransferRequest* request,
                     Sora::Compute::BackendTransferResult* result) noexcept {
        auto& state = *static_cast<MockState*>(opaque);
        ++state.aliasCount;
        if (state.failAlias) {
            result->error = {Sora::Compute::TransferErrorCode::EngineUnavailable, 71};
            return;
        }
        auto native = std::unique_ptr<NativeAllocation>{new (std::nothrow)
                                                            NativeAllocation{&state, state.nextAllocationId++, true}};
        if (!native) {
            result->error = {Sora::Compute::TransferErrorCode::OutOfMemory, 0};
            return;
        }
        const Sora::Compute::TransferRegion region = request->Regions().front();
        result->imported = {.location = request->destination,
                            .memory = request->source.memory,
                            .properties = request->source.properties,
                            .memoryOffset = request->source.memoryOffset + region.sourceOffset,
                            .sizeBytes = region.sizeBytes,
                            .alignment = request->source.alignment,
                            .state = native.release(),
                            .destroy = DestroyAllocation};
        result->error = {Sora::Compute::TransferErrorCode::None, 0};
    }

    void Compile(void* opaque, const Sora::Compute::BackendCompileRequest* request,
                 Sora::Compute::BackendCompileResult* result) noexcept {
        auto& state = *static_cast<MockState*>(opaque);
        ++state.compileCount;
        if (state.failCompile) {
            result->error = {Sora::Compute::CompileErrorCode::CompilationFailed, 81};
            return;
        }
        auto artifact = std::unique_ptr<NativeArtifact>{new (std::nothrow) NativeArtifact{&state}};
        if (!artifact) {
            result->error = {Sora::Compute::CompileErrorCode::OutOfMemory, 0};
            return;
        }
        result->payload = {.key = request->ExpectedKey(),
                           .device = request->target.device,
                           .state = artifact.release(),
                           .destroy = DestroyArtifact};
        result->error = {Sora::Compute::CompileErrorCode::None, 0};
    }

    void ImportArtifact(void* opaque, const Sora::Compute::BackendArtifactImportRequest* request,
                        Sora::Compute::BackendCompileResult* result) noexcept {
        auto& state = *static_cast<MockState*>(opaque);
        ++state.artifactImportCount;
        if (request->compileRequest == nullptr || !std::ranges::equal(request->ArtifactBytes(), kExecutableSeedBytes)) {
            result->error = {Sora::Compute::CompileErrorCode::ArtifactImportFailed, 0};
            return;
        }
        auto artifact = std::unique_ptr<NativeArtifact>{new (std::nothrow) NativeArtifact{&state}};
        if (!artifact) {
            result->error = {Sora::Compute::CompileErrorCode::OutOfMemory, 0};
            return;
        }
        result->payload = {.key = request->compileRequest->ExpectedKey(),
                           .device = request->compileRequest->target.device,
                           .state = artifact.release(),
                           .destroy = DestroyArtifact};
        result->error = {};
    }

    void RejectArtifactExport(void*, const Sora::Compute::BackendArtifactExportRequest*,
                              Sora::Compute::BackendArtifactExportResult* result) noexcept {
        result->error = {Sora::Compute::CompileErrorCode::ArtifactExportFailed, 0};
    }

    void ExportArtifact(void* opaque, const Sora::Compute::BackendArtifactExportRequest* request,
                        Sora::Compute::BackendArtifactExportResult* result) noexcept {
        auto& state = *static_cast<MockState*>(opaque);
        ++state.artifactExportCount;
        const auto* artifact = static_cast<const NativeArtifact*>(request->artifact.state);
        if (artifact == nullptr || artifact->owner != &state) {
            result->error = {Sora::Compute::CompileErrorCode::InvalidArtifact, 0};
            return;
        }
        auto bytes = std::unique_ptr<NativeArtifactBytes>{new (std::nothrow) NativeArtifactBytes{kExecutableSeedBytes}};
        if (!bytes) {
            result->error = {Sora::Compute::CompileErrorCode::OutOfMemory, 0};
            return;
        }
        result->payload = {.data = bytes->bytes.data(),
                           .size = bytes->bytes.size(),
                           .state = bytes.release(),
                           .destroy = DestroyArtifactBytes};
        result->error = {};
    }

    void CreatePipeline(void* opaque, const Sora::Compute::BackendPipelineRequest* request,
                        Sora::Compute::BackendPipelineResult* result) noexcept {
        auto& state = *static_cast<MockState*>(opaque);
        ++state.pipelineCreateCount;
        state.pipelineBindingLayout = request->pipeline.bindingLayout;
        if (state.failPipeline) {
            result->error = {Sora::Compute::PipelineErrorCode::CreationFailed, 91};
            return;
        }
        auto artifact = std::unique_ptr<NativeArtifact>{new (std::nothrow) NativeArtifact{&state}};
        if (!artifact) {
            result->error = {Sora::Compute::PipelineErrorCode::OutOfMemory, 0};
            return;
        }
        result->payload = {.key = request->ExpectedKey(),
                           .device = request->pipeline.target.device,
                           .state = artifact.release(),
                           .destroy = DestroyArtifact};
        result->error = {Sora::Compute::PipelineErrorCode::None, 0};
    }

    void ImportPipeline(void* opaque, const Sora::Compute::BackendPipelineImportRequest* request,
                        Sora::Compute::BackendPipelineResult* result) noexcept {
        auto& state = *static_cast<MockState*>(opaque);
        ++state.pipelineImportCount;
        if (request->pipelineRequest == nullptr || !std::ranges::equal(request->ArtifactBytes(), kPipelineSeedBytes)) {
            result->error = {Sora::Compute::PipelineErrorCode::ArtifactImportFailed, 0};
            return;
        }
        auto artifact = std::unique_ptr<NativeArtifact>{new (std::nothrow) NativeArtifact{&state}};
        if (!artifact) {
            result->error = {Sora::Compute::PipelineErrorCode::OutOfMemory, 0};
            return;
        }
        result->payload = {.key = request->pipelineRequest->ExpectedKey(),
                           .device = request->pipelineRequest->pipeline.target.device,
                           .state = artifact.release(),
                           .destroy = DestroyArtifact};
        result->error = {};
    }

    void RejectPipelineExport(void*, const Sora::Compute::BackendArtifactExportRequest*,
                              Sora::Compute::BackendPipelineExportResult* result) noexcept {
        result->error = {Sora::Compute::PipelineErrorCode::ArtifactExportFailed, 0};
    }

    void ExportPipeline(void* opaque, const Sora::Compute::BackendArtifactExportRequest* request,
                        Sora::Compute::BackendPipelineExportResult* result) noexcept {
        auto& state = *static_cast<MockState*>(opaque);
        ++state.pipelineExportCount;
        const auto* artifact = static_cast<const NativeArtifact*>(request->artifact.state);
        if (artifact == nullptr || artifact->owner != &state) {
            result->error = {Sora::Compute::PipelineErrorCode::InvalidArtifact, 0};
            return;
        }
        auto bytes = std::unique_ptr<NativeArtifactBytes>{new (std::nothrow) NativeArtifactBytes{kPipelineSeedBytes}};
        if (!bytes) {
            result->error = {Sora::Compute::PipelineErrorCode::OutOfMemory, 0};
            return;
        }
        result->payload = {.data = bytes->bytes.data(),
                           .size = bytes->bytes.size(),
                           .state = bytes.release(),
                           .destroy = DestroyArtifactBytes};
        result->error = {};
    }

    void DestroyQueue(void*) noexcept {}

    void Submit(void* opaque, const Sora::Compute::BackendSubmitRequest* request,
                Sora::Compute::BackendSubmitResult* result) noexcept {
        auto& state = *static_cast<MockState*>(opaque);
        const uint32_t index = state.submitCount++;
        if (state.submissionFailuresRemaining != 0) {
            --state.submissionFailuresRemaining;
            result->event = {};
            result->error = {state.submissionFailure, 0};
            return;
        }
        state.submittedCommandCounts[index] = request->commandCount;
        state.submittedBindingCounts[index] = request->bindingCount;
        state.submittedWaitCounts[index] = request->waitCount;
        if (request->commandCount != 0) {
            state.submittedCommands[index] = request->commands[0].kind;
        }
        for (uint32_t commandIndex = 0;
             commandIndex < std::min<uint32_t>(request->commandCount, state.submittedCommandDetails[index].size());
             ++commandIndex) {
            const Sora::Compute::BackendCommand& command = request->commands[commandIndex];
            state.submittedCommandDetails[index][commandIndex] = {.kind = command.kind,
                                                                  .source = command.source.memory,
                                                                  .target = command.target.memory,
                                                                  .sourceOffset = command.sourceOffset,
                                                                  .targetOffset = command.targetOffset,
                                                                  .sizeBytes = command.sizeBytes};
        }
        if (request->commandCount != 0 && request->commands[0].kind == Sora::Compute::BackendCommandKind::Dispatch) {
            const Sora::Compute::BackendCommand& command = request->commands[0];
            state.submittedDispatchArtifactKind = command.artifact.kind;
            for (size_t axis = 0; axis < state.dispatchWorkgroups.size(); ++axis) {
                state.dispatchWorkgroups[axis] = command.workgroupCount[axis];
            }
            for (const Sora::Compute::BackendBinding& binding :
                 request->Bindings().subspan(command.firstBinding, command.bindingCount)) {
                if (binding.access == Sora::Compute::AccessMode::Read) {
                    ++state.dispatchReadBindingCount;
                    state.dispatchReadBinding = binding;
                }
            }
        }
        result->event = MakeEventPayload(state);
        result->error = result->event.state != nullptr
                            ? Sora::Compute::SubmissionError{Sora::Compute::SubmissionErrorCode::None, 0}
                            : Sora::Compute::SubmissionError{Sora::Compute::SubmissionErrorCode::OutOfMemory, 0};
    }

    Sora::Compute::Allocation MakeAllocation(MockState& state, Sora::Compute::MemoryLocation location,
                                             uint64_t alignment = 64, uint64_t sizeBytes = 64) {
        auto native = std::make_unique<NativeAllocation>(&state, 0, false);
        auto allocation = Sora::Compute::Allocation::Adopt(
            Sora::Test::TestBackendService(),
            {.location = location,
             .memory = {1000 + state.nextMemory++, 2},
             .properties = Sora::Compute::MemoryProperty::HostVisible | Sora::Compute::MemoryProperty::DeviceLocal,
             .sizeBytes = sizeBytes,
             .alignment = alignment,
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

    Sora::Compute::ResidentReplica InitializeBuffer(MockState& state, Sora::Compute::Buffer buffer,
                                                    Sora::Compute::MemoryLocation location) {
        auto committed = buffer.CommitWholeWrite(MakeAllocation(state, location));
        REQUIRE(committed.has_value());
        REQUIRE(*committed == 1);
        const auto whole = Sora::Compute::ByteRegion::Make(0, 64);
        REQUIRE(whole.has_value());
        auto replica = buffer.AcquireReplica(location, *whole);
        REQUIRE(replica.has_value());
        return *replica;
    }

    Sora::Compute::ResidentReplica ForgedReplica(MockState& state, Sora::Compute::MemoryLocation location,
                                                 uint64_t alignment) {
        const auto whole = Sora::Compute::ByteRegion::Make(0, 64);
        REQUIRE(whole.has_value());
        return {.allocation = MakeAllocation(state, location, alignment),
                .bufferRegion = *whole,
                .allocationOffset = 0,
                .version = 1};
    }

    Sora::Compute::BufferView View(Sora::Compute::Buffer buffer) {
        constexpr std::array extents{uint64_t{16}};
        auto view = Sora::Compute::BufferView::Contiguous(std::move(buffer), extents);
        REQUIRE(view.has_value());
        return *view;
    }

    Sora::Compute::Buffer MakeStridedBuffer(Sora::Compute::BufferId id) {
        auto buffer = Sora::Compute::Buffer::Create({.id = id,
                                                     .elementType = kFloat,
                                                     .layout = Sora::Compute::LayoutKind::Strided,
                                                     .rank = 1,
                                                     .sizeBytes = kStridedBufferBytes,
                                                     .alignment = 64},
                                                    *kStridedFootprint);
        REQUIRE(buffer.has_value());
        return *buffer;
    }

    Sora::Compute::BufferView StridedView(Sora::Compute::Buffer buffer) {
        auto view = Sora::Compute::BufferView::Create(std::move(buffer), {.layout = Sora::Compute::LayoutKind::Strided,
                                                                          .rank = 1,
                                                                          .extents = {kStridedElementCount},
                                                                          .byteStrides = {8},
                                                                          .componentStrideBytes = sizeof(float)});
        REQUIRE(view.has_value());
        return *view;
    }

    Sora::Compute::BufferSlice Slice(Sora::Compute::Buffer buffer) {
        const auto whole = Sora::Compute::ByteRegion::Make(0, 64);
        REQUIRE(whole.has_value());
        auto slice = Sora::Compute::BufferSlice::Create(std::move(buffer), *whole);
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
                bool persistentArtifacts = false) {
        auto queue = Sora::Compute::Queue::Adopt(Sora::Test::TestBackendService(),
                                                 {.id = kQueue,
                                                  .device = kGpu,
                                                  .backend = Sora::Compute::BackendKind::Vulkan,
                                                  .capabilities = Sora::Compute::QueueCapability::Dispatch |
                                                                  Sora::Compute::QueueCapability::Copy |
                                                                  Sora::Compute::QueueCapability::Fill,
                                                  .dispatchArtifactKind = dispatchArtifactKind,
                                                  .state = &state,
                                                  .destroy = DestroyQueue,
                                                  .submit = Submit});
        REQUIRE(queue.has_value());
        auto allocator = Sora::Compute::MemoryAllocator::Create(
            Sora::Test::TestBackendService(), {.location = kDevice,
                                               .supportedProperties = Sora::Compute::MemoryProperty::HostVisible |
                                                                      Sora::Compute::MemoryProperty::DeviceLocal,
                                               .state = &state,
                                               .allocate = Allocate});
        REQUIRE(allocator.has_value());
        auto copy = Sora::Compute::TransferEngine::Create(Sora::Test::TestBackendService(),
                                                          {.engine = kCopyEngine,
                                                           .sourceService = Sora::Test::TestBackendService().Id(),
                                                           .source = kHost,
                                                           .destination = kDevice,
                                                           .kind = Sora::Compute::TransferKind::Copy,
                                                           .completionDevice = state.transferCompletionDevice,
                                                           .completionBackend = state.transferCompletionBackend,
                                                           .state = &state,
                                                           .transfer = Transfer});
        REQUIRE(copy.has_value());
        auto alias = Sora::Compute::TransferEngine::Create(Sora::Test::TestBackendService(),
                                                           {.engine = kAliasEngine,
                                                            .sourceService = Sora::Test::TestBackendService().Id(),
                                                            .source = kSharedCpu,
                                                            .destination = kSharedGpu,
                                                            .kind = Sora::Compute::TransferKind::CoherentAlias,
                                                            .state = &state,
                                                            .transfer = ImportAlias});
        REQUIRE(alias.has_value());
        auto compiler = Sora::Compute::BackendCompiler::Create(
            Sora::Test::TestBackendService(),
            {.identity = kCompilerIdentity,
             .backend = Sora::Compute::BackendKind::Vulkan,
             .executableDomain = Sora::Compute::ExecutableTargetDomain::BackendPortable,
             .state = &state,
             .compile = Compile,
             .importArtifact = ImportArtifact,
             .exportArtifact = persistentArtifacts ? ExportArtifact : RejectArtifactExport});
        REQUIRE(compiler.has_value());
        auto observer = Sora::Compute::DeviceObserver::Create(
            Sora::Test::TestBackendService(),
            {.device = kGpu, .backend = Sora::Compute::BackendKind::Vulkan, .state = &state, .observe = ObserveDevice});
        REQUIRE(observer.has_value());
        auto factory = Sora::Compute::BackendPipelineFactory::Create(
            Sora::Test::TestBackendService(), kGpuTarget,
            {.identity = kPipelineFactoryIdentity,
             .state = &state,
             .create = CreatePipeline,
             .importPipeline = ImportPipeline,
             .exportPipeline = persistentArtifacts ? ExportPipeline : RejectPipelineExport});
        REQUIRE(factory.has_value());
        auto catalog = Sora::Compute::PhysicalResourceCatalog::Create({.deviceObservers = {*observer},
                                                                       .queues = {*queue},
                                                                       .allocators = {*allocator},
                                                                       .transferEngines = {*copy, *alias},
                                                                       .compilers = {*compiler},
                                                                       .pipelineFactories = {*factory}});
        REQUIRE(catalog.has_value());
        return std::move(*catalog);
    }

    struct PlanFixture {
        Sora::Compute::Buffer output;
        Sora::Compute::PhysicalGraphPlan plan;
    };

    struct PlanStorageFixture {
        Sora::Compute::Buffer output;
        Sora::Compute::PhysicalGraphPlanStorage storage;
    };

    PlanStorageFixture MakeDispatchPlanStorage(MockState& state, bool invalidReadyAlignment = false,
                                               bool aliasInput = false, bool pipeline = false,
                                               uint64_t bufferIdentity = 70) {
        auto x = MakeBuffer({bufferIdentity, 1});
        auto y = MakeBuffer({bufferIdentity, 2});
        auto output = MakeBuffer({bufferIdentity, 3});
        std::vector<Sora::Compute::KernelArgumentBinding> bindings;
        bindings.emplace_back(Alpha());
        bindings.emplace_back(View(x));
        bindings.emplace_back(View(y));
        bindings.emplace_back(View(output));
        constexpr std::array extents{uint64_t{16}};
        auto invocation = Sora::Compute::BindKernelProgram(kSaxpy, extents, std::move(bindings));
        REQUIRE(invocation.has_value());
        const auto geometry = Sora::Compute::PlanDispatchGeometry(invocation->Domain());
        REQUIRE(geometry.has_value());
        auto package = Sora::Compute::ProgramPackage::Create(kSaxpy);
        REQUIRE(package.has_value());
        const auto whole = Sora::Compute::ByteRegion::Make(0, 64);
        REQUIRE(whole.has_value());
        const auto yReplica =
            invalidReadyAlignment ? ForgedReplica(state, kDevice, 32) : InitializeBuffer(state, y, kDevice);

        Sora::Compute::PhysicalGraphPlanStorage storage;
        Sora::Compute::PlannedStorageRef xStorage;
        Sora::Compute::PhysicalPlanSegmentId xProducer{};
        if (aliasInput) {
            const auto xReplica = InitializeBuffer(state, x, kSharedCpu);
            storage.allocations.push_back(
                {.id = {0},
                 .buffer = x,
                 .service = kService,
                 .location = kSharedGpu,
                 .bufferRegion = *whole,
                 .version = 1,
                 .materialization = Sora::Compute::PlannedCoherentAlias{
                     .engine = kAliasEngine, .source = Sora::Compute::ReadyStorageRef{x.Describe().id, xReplica}}});
            xStorage = Sora::Compute::PlannedAllocationRef{.allocation = {0}};
        } else {
            const auto xReplica = InitializeBuffer(state, x, kHost);
            storage.allocations.push_back(
                {.id = {0},
                 .buffer = x,
                 .service = kService,
                 .location = kDevice,
                 .bufferRegion = *whole,
                 .version = 1,
                 .materialization = Sora::Compute::PlannedAllocate{
                     .requiredProperties = Sora::Compute::MemoryProperty::DeviceLocal, .alignment = 64}});
            storage.segments.emplace_back(
                Sora::Compute::PlannedTransfer{.logicalNode = {0},
                                               .service = kService,
                                               .engine = kCopyEngine,
                                               .buffer = x.Describe().id,
                                               .region = *whole,
                                               .version = 1,
                                               .source = Sora::Compute::ReadyStorageRef{x.Describe().id, xReplica},
                                               .destination = {0}});
            xProducer = {0};
            xStorage = Sora::Compute::PlannedAllocationRef{.allocation = {0}, .readyAfter = xProducer};
        }
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
             .publication = Sora::Compute::PlannedPublication{.kind = Sora::Compute::PlannedPublicationKind::WholeWrite,
                                                              .expectedVersion = 0,
                                                              .producer = {aliasInput ? 0U : 1U}}});
        const Sora::Compute::PhysicalPlanSegmentId dispatchId{aliasInput ? 0U : 1U};
        storage.segments.emplace_back(Sora::Compute::PlannedDispatch{
            .logicalNode = {0},
            .queue =
                {.service = kService, .backend = Sora::Compute::BackendKind::Vulkan, .device = kGpu, .queue = kQueue},
            .program = *package,
            .invocation = *invocation,
            .slice = Sora::Compute::DispatchSlice::Whole(invocation->Domain()),
            .target = kGpuTarget,
            .geometry = *geometry,
            .pipeline = pipeline ? std::optional<Sora::Compute::PlannedPipeline>{{kPipelineFactoryIdentity,
                                                                                  kPipelineStateIdentity}}
                                 : std::nullopt,
            .bufferStorage = {
                {.input = xStorage},
                {.input = Sora::Compute::PlannedStorageRef{Sora::Compute::ReadyStorageRef{y.Describe().id, yReplica}}},
                {.output = {1}},
            }});
        if (xProducer.Valid()) {
            storage.edges.push_back({xProducer, dispatchId});
        }
        return {output, std::move(storage)};
    }

    PlanFixture MakeDispatchPlan(MockState& state, const Sora::Compute::PhysicalResourceCatalog& catalog,
                                 bool invalidReadyAlignment = false, bool aliasInput = false, bool pipeline = false) {
        auto fixture = MakeDispatchPlanStorage(state, invalidReadyAlignment, aliasInput, pipeline);
        auto plan = Sora::Compute::PhysicalGraphPlan::Create(std::move(fixture.storage), catalog);
        REQUIRE(plan.has_value());
        return {std::move(fixture.output), std::move(*plan)};
    }

    PlanFixture MakeAffinePartitionPlan(MockState& state, const Sora::Compute::PhysicalResourceCatalog& catalog) {
        auto input = MakeStridedBuffer({75, 1});
        auto output = MakeStridedBuffer({75, 2});
        REQUIRE(input.CommitWholeWrite(MakeAllocation(state, kDevice, 64, 128)).has_value());
        auto inputReplica = input.AcquireReplica(kDevice, Sora::Compute::ByteCoverageView::Affine(*kStridedFootprint));
        REQUIRE(inputReplica.has_value());

        std::vector<Sora::Compute::KernelArgumentBinding> bindings;
        bindings.emplace_back(StridedView(input));
        bindings.emplace_back(StridedView(output));
        auto invocation = Sora::Compute::BindKernelProgram(kStridedCopy, kStridedExtents, std::move(bindings));
        REQUIRE(invocation.has_value());
        auto package = Sora::Compute::ProgramPackage::Create(kStridedCopy);
        REQUIRE(package.has_value());
        constexpr std::array shardExtents{kStridedElementCount / 2, kStridedElementCount / 2};
        auto partition = Sora::Compute::ElementwisePartitionPlan::Create(invocation->Domain(), kStridedCopy.partition,
                                                                         0, shardExtents);
        REQUIRE(partition.has_value());

        Sora::Compute::PhysicalGraphPlanStorage storage;
        for (uint32_t index = 0; index < partition->Shards().size(); ++index) {
            const Sora::Compute::DispatchSlice shard = partition->Shards()[index];
            const std::span<const uint64_t> origin{shard.origin.data(), shard.rank};
            const std::span<const uint64_t> extents{shard.extents.data(), shard.rank};
            auto outputShard = StridedView(output).SubView(origin, extents);
            REQUIRE(outputShard.has_value());
            const Sora::Compute::AffineByteFootprint footprint = outputShard->Footprint();
            storage.allocations.push_back(
                {.id = {index},
                 .buffer = output,
                 .service = kService,
                 .location = kDevice,
                 .bufferRegion = footprint.BoundingRegion(),
                 .byteFootprint = footprint,
                 .version = 1,
                 .materialization =
                     Sora::Compute::PlannedAllocate{.requiredProperties = Sora::Compute::MemoryProperty::DeviceLocal,
                                                    .alignment = 64},
                 .publication = Sora::Compute::PlannedPublication{
                     .kind = Sora::Compute::PlannedPublicationKind::PartitionedWrite,
                     .expectedVersion = 0,
                     .producer = {index},
                     .partitionIndex = index,
                     .partitionCount = static_cast<uint32_t>(partition->Shards().size())}});
            const auto geometry = Sora::Compute::PlanDispatchSliceGeometry(shard);
            REQUIRE(geometry.has_value());
            storage.segments.emplace_back(Sora::Compute::PlannedDispatch{
                .logicalNode = {0},
                .queue = {.service = kService,
                          .backend = Sora::Compute::BackendKind::Vulkan,
                          .device = kGpu,
                          .queue = kQueue},
                .program = *package,
                .invocation = *invocation,
                .slice = shard,
                .target = kGpuTarget,
                .geometry = *geometry,
                .bufferStorage = {
                    {.input = Sora::Compute::PlannedStorageRef{Sora::Compute::ReadyStorageRef{input.Describe().id,
                                                                                              *inputReplica}}},
                    {.output = {index}},
                }});
        }
        auto plan = Sora::Compute::PhysicalGraphPlan::Create(std::move(storage), catalog);
        REQUIRE(plan.has_value());
        return {std::move(output), std::move(*plan)};
    }

    PlanFixture MakeAssemblyDispatchPlan(MockState& state, const Sora::Compute::PhysicalResourceCatalog& catalog,
                                         uint32_t prefilledPartCount = 0) {
        REQUIRE(prefilledPartCount <= 2);
        auto input = MakeStridedBuffer({76, 1});
        auto output = MakeStridedBuffer({76, 2});
        constexpr uint64_t shardElementCount = kStridedElementCount / 2;
        constexpr std::array shardExtents{shardElementCount};
        std::array<Sora::Compute::AffineByteFootprint, 2> footprints{};
        std::array<Sora::Compute::PartitionedWritePart, 2> sourceParts{};
        for (uint32_t index = 0; index < sourceParts.size(); ++index) {
            const std::array origin{static_cast<uint64_t>(index) * shardElementCount};
            auto shard = StridedView(input).SubView(origin, shardExtents);
            REQUIRE(shard.has_value());
            footprints[index] = shard->Footprint();
            const Sora::Compute::MemoryLocation sourceLocation = index < prefilledPartCount ? kHost : kDevice;
            sourceParts[index] = {
                .region = footprints[index].BoundingRegion(),
                .allocation = MakeAllocation(state, sourceLocation, 64, footprints[index].BoundingRegion().Size()),
                .byteFootprint = footprints[index]};
        }
        Sora::Compute::CompletionTransaction publication;
        REQUIRE(publication.AddPartitionedWrite(input, 0, sourceParts).has_value());
        REQUIRE(std::move(publication).Commit().has_value());

        std::vector<Sora::Compute::KernelArgumentBinding> bindings;
        bindings.emplace_back(StridedView(input));
        bindings.emplace_back(StridedView(output));
        auto invocation = Sora::Compute::BindKernelProgram(kStridedCopy, kStridedExtents, std::move(bindings));
        REQUIRE(invocation.has_value());
        auto package = Sora::Compute::ProgramPackage::Create(kStridedCopy);
        REQUIRE(package.has_value());
        const auto geometry = Sora::Compute::PlanDispatchGeometry(invocation->Domain());
        REQUIRE(geometry.has_value());

        Sora::Compute::PhysicalGraphPlanStorage storage;
        storage.allocations.push_back(
            {.id = {0},
             .purpose = Sora::Compute::PlannedAllocationPurpose::DispatchInputAssembly,
             .buffer = input,
             .service = kService,
             .location = kDevice,
             .bufferRegion = kStridedFootprint->BoundingRegion(),
             .byteFootprint = *kStridedFootprint,
             .version = 0,
             .materialization = Sora::Compute::PlannedAllocate{
                 .requiredProperties = Sora::Compute::MemoryProperty::DeviceLocal, .alignment = 64}});
        storage.allocations.push_back(
            {.id = {1},
             .buffer = output,
             .service = kService,
             .location = kDevice,
             .bufferRegion = kStridedFootprint->BoundingRegion(),
             .byteFootprint = *kStridedFootprint,
             .version = 1,
             .materialization = Sora::Compute::PlannedAllocate{
                 .requiredProperties = Sora::Compute::MemoryProperty::DeviceLocal, .alignment = 64}});

        std::vector<Sora::Compute::PlannedAssemblyPart> assemblyParts;
        assemblyParts.reserve(sourceParts.size());
        for (uint32_t index = 0; index < sourceParts.size(); ++index) {
            const Sora::Compute::MemoryLocation sourceLocation = index < prefilledPartCount ? kHost : kDevice;
            auto replica =
                input.AcquireReplica(sourceLocation, Sora::Compute::ByteCoverageView::Affine(footprints[index]));
            REQUIRE(replica.has_value());
            if (index < prefilledPartCount) {
                const Sora::Compute::PhysicalPlanSegmentId producer{static_cast<uint32_t>(storage.segments.size())};
                storage.segments.emplace_back(Sora::Compute::PlannedTransfer{
                    .logicalNode = {0},
                    .service = kService,
                    .engine = kCopyEngine,
                    .buffer = input.Describe().id,
                    .region = footprints[index].BoundingRegion(),
                    .byteFootprint = footprints[index],
                    .version = 1,
                    .source = Sora::Compute::ReadyStorageRef{input.Describe().id, *replica},
                    .destination = {0},
                    .target = Sora::Compute::PlannedTransferTarget::AssemblyPart});
                assemblyParts.push_back({.transferProducer = producer,
                                         .region = footprints[index].BoundingRegion(),
                                         .byteFootprint = footprints[index]});
            } else {
                assemblyParts.push_back({.source = Sora::Compute::ReadyStorageRef{input.Describe().id, *replica},
                                         .region = footprints[index].BoundingRegion(),
                                         .byteFootprint = footprints[index]});
            }
        }
        const Sora::Compute::PhysicalPlanSegmentId assemblyId{static_cast<uint32_t>(storage.segments.size())};
        for (const Sora::Compute::PlannedAssemblyPart& part : assemblyParts) {
            if (part.transferProducer.Valid()) {
                storage.edges.push_back({part.transferProducer, assemblyId});
            }
        }
        storage.segments.emplace_back(Sora::Compute::PlannedAssembly{
            .logicalNode = {0},
            .queue =
                {.service = kService, .backend = Sora::Compute::BackendKind::Vulkan, .device = kGpu, .queue = kQueue},
            .destination = {0},
            .parts = std::move(assemblyParts)});
        const Sora::Compute::PhysicalPlanSegmentId dispatchId{static_cast<uint32_t>(storage.segments.size())};
        storage.allocations[1].publication = Sora::Compute::PlannedPublication{
            .kind = Sora::Compute::PlannedPublicationKind::WholeWrite, .expectedVersion = 0, .producer = dispatchId};
        storage.segments.emplace_back(Sora::Compute::PlannedDispatch{
            .logicalNode = {0},
            .queue =
                {.service = kService, .backend = Sora::Compute::BackendKind::Vulkan, .device = kGpu, .queue = kQueue},
            .program = *package,
            .invocation = *invocation,
            .slice = Sora::Compute::DispatchSlice::Whole(invocation->Domain()),
            .target = kGpuTarget,
            .geometry = *geometry,
            .bufferStorage = {
                {.input = Sora::Compute::PlannedStorageRef{Sora::Compute::PlannedAllocationRef{
                     .allocation = {0}, .readyAfter = assemblyId}}},
                {.output = {1}},
            }});
        storage.edges.push_back({assemblyId, dispatchId});
        auto plan = Sora::Compute::PhysicalGraphPlan::Create(std::move(storage), catalog);
        REQUIRE(plan.has_value());
        return {std::move(output), std::move(*plan)};
    }

    Sora::Compute::PortablePackage MakeDispatchPackage(const Sora::Compute::PlannedDispatch& dispatch,
                                                       Sora::Compute::TargetFingerprint target) {
        const auto executable = Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
            .lowered = dispatch.program.Hash(), .target = target, .specialization = dispatch.specialization});
        std::vector<Sora::Compute::PackagedArtifact> artifacts{{
            .key = executable,
            .payload = std::vector<std::byte>(kExecutableSeedBytes.begin(), kExecutableSeedBytes.end()),
        }};
        if (dispatch.pipeline) {
            artifacts.push_back({
                .key = Sora::Compute::MakeArtifactKey(Sora::Compute::PipelineKey{
                    .executable = Sora::Compute::ArtifactIdentity(executable),
                    .target = target,
                    .builder = dispatch.pipeline->factory,
                    .state = dispatch.pipeline->state,
                    .bindingLayout = Sora::Compute::PipelineBindingLayoutIdentity(dispatch.invocation.Signature()),
                }),
                .payload = std::vector<std::byte>(kPipelineSeedBytes.begin(), kPipelineSeedBytes.end()),
            });
        }
        auto package = Sora::Compute::PortablePackage::Create(dispatch.program, std::move(artifacts));
        REQUIRE(package.has_value());
        return std::move(*package);
    }

    PlanStorageFixture MakeFillCopyPlanStorage(MockState& state) {
        auto source = MakeBuffer({80, 1});
        auto output = MakeBuffer({80, 2});
        const auto whole = Sora::Compute::ByteRegion::Make(0, 64);
        REQUIRE(whole.has_value());
        constexpr std::array patternBytes{std::byte{0x5A}};
        auto pattern = Sora::Compute::FillPattern::Create(patternBytes);
        REQUIRE(pattern.has_value());
        auto fill = Sora::Compute::FillOperation::Create(Slice(source), *pattern);
        auto copy = Sora::Compute::CopyOperation::Create(Slice(source), Slice(output));
        REQUIRE(fill.has_value());
        REQUIRE(copy.has_value());

        Sora::Compute::PhysicalGraphPlanStorage storage;
        storage.allocations.push_back(
            {.id = {0},
             .buffer = source,
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
        storage.segments.emplace_back(Sora::Compute::PlannedFill{
            .logicalNode = {0},
            .queue =
                {.service = kService, .backend = Sora::Compute::BackendKind::Vulkan, .device = kGpu, .queue = kQueue},
            .operation = *fill,
            .destination = {0}});
        storage.segments.emplace_back(Sora::Compute::PlannedCopy{
            .logicalNode = {1},
            .queue =
                {.service = kService, .backend = Sora::Compute::BackendKind::Vulkan, .device = kGpu, .queue = kQueue},
            .operation = *copy,
            .source = Sora::Compute::PlannedAllocationRef{.allocation = {0}, .readyAfter = {0}},
            .destination = {1}});
        storage.edges.push_back({{0}, {1}});
        return {output, std::move(storage)};
    }

    PlanFixture MakeFillCopyPlan(MockState& state, const Sora::Compute::PhysicalResourceCatalog& catalog) {
        auto fixture = MakeFillCopyPlanStorage(state);
        auto plan = Sora::Compute::PhysicalGraphPlan::Create(std::move(fixture.storage), catalog);
        REQUIRE(plan.has_value());
        return {std::move(fixture.output), std::move(*plan)};
    }

    enum class TestGraphPlanningError : uint8_t {
        InvalidInput,
    };

    std::expected<Sora::Compute::RuntimeEpoch, Sora::Compute::RuntimeRecoveryError>
    RejectRuntimeRecovery(const Sora::Compute::RuntimeRecoveryRequest& request) noexcept {
        return std::unexpected(Sora::Compute::RuntimeRecoveryError{
            .code = Sora::Compute::RuntimeRecoveryErrorCode::EpochRebuildFailed,
            .service = request.failedService,
            .expectedSequence = request.sequence,
        });
    }

    struct ArchiveVerifier {
        uint32_t* calls = nullptr;
        bool accept = true;

        std::expected<Sora::Compute::PortablePackageTrustGrant, Sora::Compute::PortablePackageTrustError>
        Verify(const Sora::Compute::PortablePackageVerificationRequest& request) const noexcept {
            ++*calls;
            if (!accept || request.context != Sora::Compute::kPortablePackageSignatureContext) {
                return std::unexpected(Sora::Compute::PortablePackageTrustError::InvalidSignature);
            }
            return Sora::Compute::PortablePackageTrustGrant{
                .scheme = request.signature.scheme,
                .authority = {0xA1, 0xA2},
                .subject = {0xB1, 0xB2},
                .policyRevision = 7,
            };
        }
    };

    static_assert(Sora::Compute::PortablePackageSignatureVerifier<ArchiveVerifier>);

    Sora::Compute::RuntimeEpoch
    MakeRuntimeEpoch(MockState& state,
                     Sora::Compute::ArtifactKind dispatchArtifactKind = Sora::Compute::ArtifactKind::DeviceExecutable,
                     bool persistentArtifacts = false) {
        auto catalog = MakeCatalog(state, dispatchArtifactKind, persistentArtifacts);
        constexpr Sora::Compute::DeviceDescriptor descriptor{.id = kGpu,
                                                             .backend = Sora::Compute::BackendKind::Vulkan,
                                                             .kind = Sora::Compute::DeviceKind::DiscreteGpu,
                                                             .features = Sora::Compute::DeviceFeature::TimelineEvents,
                                                             .architectureId = 71,
                                                             .totalMemoryBytes = 1ULL << 30,
                                                             .maximumAllocationBytes = 1ULL << 30,
                                                             .computeUnitCount = 16,
                                                             .subgroupWidthMinimum = 32,
                                                             .subgroupWidthMaximum = 32,
                                                             .maximumWorkgroupInvocations = 1024};
        auto epoch = Sora::Compute::RuntimeEpoch::Create(1, std::move(catalog),
                                                         {{.service = Sora::Test::TestBackendService().Id(),
                                                           .descriptor = descriptor,
                                                           .target = Sora::Compute::RuntimeTarget(kGpuTarget)}});
        REQUIRE(epoch.has_value());
        return std::move(*epoch);
    }

    Sora::Compute::ExecutionGraph MakeLogicalFillGraph(Sora::Compute::BufferId id) {
        auto logicalBuffer = MakeBuffer(id);
        constexpr std::array patternBytes{std::byte{0x3C}};
        constexpr auto pattern = Sora::Compute::FillPattern::Create(patternBytes);
        static_assert(pattern.has_value());
        auto fill = Sora::Compute::FillOperation::Create(Slice(logicalBuffer), *pattern);
        REQUIRE(fill.has_value());
        Sora::Compute::GraphBuilder builder;
        REQUIRE(builder.AddFill(std::move(*fill)).has_value());
        auto graph = std::move(builder).Finish();
        REQUIRE(graph.has_value());
        return std::move(*graph);
    }

    Sora::Compute::KernelInvocation InvokeStridedCopy(Sora::Compute::BufferView input,
                                                      Sora::Compute::BufferView output) {
        std::vector<Sora::Compute::KernelArgumentBinding> bindings;
        bindings.emplace_back(std::move(input));
        bindings.emplace_back(std::move(output));
        auto invocation = Sora::Compute::BindKernelProgram(kStridedCopy, kStridedExtents, std::move(bindings));
        REQUIRE(invocation.has_value());
        return std::move(*invocation);
    }

    Sora::Compute::ExecutionGraph MakeLogicalFusionChainGraph(uint64_t identity, size_t dispatches) {
        REQUIRE(dispatches >= 2);
        Sora::Compute::GraphBuilder builder;
        auto previous = StridedView(MakeStridedBuffer({identity, 1}));
        for (size_t index = 0; index < dispatches; ++index) {
            auto next = StridedView(MakeStridedBuffer({identity, index + 2}));
            REQUIRE(builder.AddDispatch(InvokeStridedCopy(previous, next)).has_value());
            previous = std::move(next);
        }
        auto graph = std::move(builder).Finish();
        REQUIRE(graph.has_value());
        return std::move(*graph);
    }

    Sora::Compute::ExecutionGraph MakeLogicalFusionGraph(uint64_t identity) {
        return MakeLogicalFusionChainGraph(identity, 2);
    }

    Sora::Compute::ExecutionGraph MakeLogicalIndependentFusionGraph(uint64_t identity) {
        auto firstSource = MakeStridedBuffer({identity, 1});
        auto firstIntermediate = MakeStridedBuffer({identity, 2});
        auto firstOutput = MakeStridedBuffer({identity, 3});
        auto secondSource = MakeStridedBuffer({identity, 4});
        auto secondIntermediate = MakeStridedBuffer({identity, 5});
        auto secondOutput = MakeStridedBuffer({identity, 6});
        const auto firstIntermediateView = StridedView(firstIntermediate);
        const auto secondIntermediateView = StridedView(secondIntermediate);
        Sora::Compute::GraphBuilder builder;
        REQUIRE(builder.AddDispatch(InvokeStridedCopy(StridedView(firstSource), firstIntermediateView)).has_value());
        REQUIRE(builder.AddDispatch(InvokeStridedCopy(firstIntermediateView, StridedView(firstOutput))).has_value());
        REQUIRE(builder.AddDispatch(InvokeStridedCopy(StridedView(secondSource), secondIntermediateView)).has_value());
        REQUIRE(builder.AddDispatch(InvokeStridedCopy(secondIntermediateView, StridedView(secondOutput))).has_value());
        auto graph = std::move(builder).Finish();
        REQUIRE(graph.has_value());
        return std::move(*graph);
    }

    constexpr Sora::Compute::GraphFusionPolicy ForcedFusionPolicy() noexcept {
        return {.launchLatencyNanoseconds = 1,
                .memoryBandwidthBytesPerSecond = 1,
                .compilationLatencyNanoseconds = 0,
                .expectedReuseCount = 1};
    }

    template<typename Error, stdexec::sender Sender>
    Error WaitForContextError(Sender sender) {
        std::optional<Error> observed;
        auto recovered = std::move(sender) | stdexec::upon_error([&observed](Error error) noexcept {
                             observed.emplace(std::move(error));
                             return Sora::Compute::EventObservation{};
                         });
        auto completed = stdexec::sync_wait(std::move(recovered));
        REQUIRE(completed.has_value());
        REQUIRE(observed.has_value());
        return std::move(*observed);
    }

} // namespace

TEST_CASE("ComputeContext owns the plan-materialize-submit path behind one "
          "logical graph API",
          "[Sora][Compute][Runtime][ComputeContext]") {
    MockState state;
    auto epoch = MakeRuntimeEpoch(state);
    auto logicalGraph = MakeLogicalFillGraph({90, 1});

    uint32_t planningCalls = 0;
    Sora::Compute::Buffer plannedOutput;
    auto planner = [&](const Sora::Compute::ExecutionGraph& graph, const Sora::Compute::GraphPlanningContext& planning,
                       Sora::Compute::GraphPlanningServices services) noexcept
        -> std::expected<Sora::Compute::PhysicalGraphPlanStorage, TestGraphPlanningError> {
        static_assert(std::same_as<decltype(services.packages), const Sora::Compute::ProgramCatalogSnapshot&>);
        static_assert(std::same_as<decltype(services.resources), const Sora::Compute::PhysicalResourceInventory&>);
        static_assert(std::same_as<decltype(services.artifacts), const Sora::Compute::ArtifactAvailabilitySnapshot&>);
        static_assert(std::same_as<decltype(services.performance), const Sora::Compute::PerformanceSnapshot&>);
        ++planningCalls;
        if (graph.NodeCount() != 1 || planning.epochSequence != 1 || planning.devices.size() != 1 ||
            planning.snapshots.size() != 1 || planning.snapshots[0].state != Sora::Compute::DeviceState::Available ||
            services.packages.PackageCount() != 0 || services.resources.Queues().size() != 1 ||
            !services.artifacts.Keys().empty() || !services.performance.KernelEntries().empty() ||
            !services.performance.TransferEntries().empty()) {
            return std::unexpected(TestGraphPlanningError::InvalidInput);
        }
        auto fixture = MakeFillCopyPlanStorage(state);
        plannedOutput = fixture.output;
        return std::move(fixture.storage);
    };
    static_assert(Sora::Compute::GraphPlanningEngine<decltype(planner)>);

    auto context = Sora::Compute::CreateComputeContext(
        std::move(epoch),
        {.artifactCacheCapacity = 4,
         .performance = {.maximumKernelKeys = 8, .maximumTransferKeys = 16, .maximumSamplesPerBucket = 32}},
        planner, RejectRuntimeRecovery);
    REQUIRE(context.has_value());
    REQUIRE(context->Recovery().InvalidationRegistry().CacheCount() == 1);
    REQUIRE(context->Performance().Limits().maximumKernelKeys == 8);
    REQUIRE(context->Performance().Limits().maximumTransferKeys == 16);
    REQUIRE(context->Performance().Limits().maximumSamplesPerBucket == 32);

    exec::static_thread_pool pool{1};
    auto completed = stdexec::sync_wait(context->Submit(pool.get_scheduler(), std::move(logicalGraph)));
    REQUIRE(completed.has_value());
    REQUIRE(std::get<0>(*completed).status == Sora::Compute::EventStatus::Completed);
    REQUIRE(planningCalls == 1);
    REQUIRE(state.submitCount == 2);
    REQUIRE(state.submittedCommands[0] == Sora::Compute::BackendCommandKind::Fill);
    REQUIRE(state.submittedCommands[1] == Sora::Compute::BackendCommandKind::Copy);
    REQUIRE(plannedOutput.CurrentVersion() == 1);
    REQUIRE(context->Recovery().InvalidationRegistry().BufferCount() == 3);
}

TEST_CASE("ComputeContext applies graph fusion lazily without mutating authoritative package admissions",
          "[Sora][Compute][Runtime][ComputeContext][GraphOptimizer]") {
    SECTION("profitable fusion is visible only through the submission catalog after sender start") {
        MockState state;
        uint32_t planningCalls = 0;
        auto planner = [&](const Sora::Compute::ExecutionGraph& graph, const Sora::Compute::GraphPlanningContext&,
                           Sora::Compute::GraphPlanningServices services) noexcept
            -> std::expected<Sora::Compute::PhysicalGraphPlanStorage, TestGraphPlanningError> {
            ++planningCalls;
            const auto* invocation =
                graph.NodeCount() == 1 ? std::get_if<Sora::Compute::KernelInvocation>(&graph.Operation({0})) : nullptr;
            if (invocation == nullptr || services.packages.GeneratedProgramCount() != 1 ||
                !services.packages.Resolve(invocation->ProgramHash())) {
                return std::unexpected(TestGraphPlanningError::InvalidInput);
            }
            return std::move(MakeDispatchPlanStorage(state).storage);
        };
        auto context = Sora::Compute::CreateComputeContext(MakeRuntimeEpoch(state), {.fusion = ForcedFusionPolicy()},
                                                           planner, RejectRuntimeRecovery);
        REQUIRE(context.has_value());
        REQUIRE(context->RegisterProgram(kStridedCopy).has_value());
        REQUIRE(context->Packages().Snapshot()->PackageCount() == 1);

        exec::static_thread_pool pool{1};
        auto sender = context->Submit(pool.get_scheduler(), MakeLogicalFusionGraph(95));
        static_assert(!std::copy_constructible<decltype(sender)>);
        REQUIRE(planningCalls == 0);
        REQUIRE(context->OptimizationStatistics() == Sora::Compute::GraphOptimizationStatistics{});
        REQUIRE(context->Packages().Snapshot()->PackageCount() == 1);

        const auto completed = stdexec::sync_wait(std::move(sender));
        REQUIRE(completed.has_value());
        REQUIRE(planningCalls == 1);
        REQUIRE(context->OptimizationStatistics() ==
                Sora::Compute::GraphOptimizationStatistics{.attempts = 1, .fused = 1});
        REQUIRE(context->Packages().Snapshot()->PackageCount() == 1);
    }

    SECTION("unprofitable fusion preserves the original graph and publishes no generated program") {
        MockState state;
        uint32_t planningCalls = 0;
        auto planner = [&](const Sora::Compute::ExecutionGraph& graph, const Sora::Compute::GraphPlanningContext&,
                           Sora::Compute::GraphPlanningServices services) noexcept
            -> std::expected<Sora::Compute::PhysicalGraphPlanStorage, TestGraphPlanningError> {
            ++planningCalls;
            if (graph.NodeCount() != 2 || services.packages.GeneratedProgramCount() != 0) {
                return std::unexpected(TestGraphPlanningError::InvalidInput);
            }
            return std::move(MakeDispatchPlanStorage(state).storage);
        };
        const Sora::Compute::GraphFusionPolicy policy{
            .launchLatencyNanoseconds = 0,
            .memoryBandwidthBytesPerSecond = std::numeric_limits<uint64_t>::max(),
            .compilationLatencyNanoseconds = 1'000'000,
            .expectedReuseCount = 1,
        };
        auto context = Sora::Compute::CreateComputeContext(MakeRuntimeEpoch(state), {.fusion = policy}, planner,
                                                           RejectRuntimeRecovery);
        REQUIRE(context.has_value());
        REQUIRE(context->RegisterProgram(kStridedCopy).has_value());

        exec::static_thread_pool pool{1};
        const auto completed = stdexec::sync_wait(context->Submit(pool.get_scheduler(), MakeLogicalFusionGraph(96)));
        REQUIRE(completed.has_value());
        REQUIRE(planningCalls == 1);
        REQUIRE(context->OptimizationStatistics() ==
                Sora::Compute::GraphOptimizationStatistics{.attempts = 1, .skippedUnprofitable = 1});
        REQUIRE(context->Packages().Snapshot()->PackageCount() == 1);
    }

    SECTION("publication budget exhaustion preserves the original graph and records a normal skip") {
        MockState state;
        uint32_t planningCalls = 0;
        auto planner = [&](const Sora::Compute::ExecutionGraph& graph, const Sora::Compute::GraphPlanningContext&,
                           Sora::Compute::GraphPlanningServices services) noexcept
            -> std::expected<Sora::Compute::PhysicalGraphPlanStorage, TestGraphPlanningError> {
            ++planningCalls;
            if (graph.NodeCount() != 2 || services.packages.GeneratedProgramCount() != 0) {
                return std::unexpected(TestGraphPlanningError::InvalidInput);
            }
            return std::move(MakeDispatchPlanStorage(state).storage);
        };
        auto policy = ForcedFusionPolicy();
        policy.maximumGeneratedBytes = 1;
        auto context = Sora::Compute::CreateComputeContext(MakeRuntimeEpoch(state), {.fusion = policy}, planner,
                                                           RejectRuntimeRecovery);
        REQUIRE(context.has_value());
        REQUIRE(context->RegisterProgram(kStridedCopy).has_value());

        exec::static_thread_pool pool{1};
        const auto completed = stdexec::sync_wait(context->Submit(pool.get_scheduler(), MakeLogicalFusionGraph(98)));
        REQUIRE(completed.has_value());
        REQUIRE(planningCalls == 1);
        REQUIRE(context->OptimizationStatistics() ==
                Sora::Compute::GraphOptimizationStatistics{.attempts = 1, .skippedBudget = 1});
        REQUIRE(context->Packages().Snapshot()->PackageCount() == 1);
    }
}

TEST_CASE("ComputeContext presents maximal chains and independent fusion regions through one submission catalog",
          "[Sora][Compute][Runtime][ComputeContext][GraphOptimizer][Regions]") {
    SECTION("a three-dispatch chain reaches planning as one generated dispatch") {
        MockState state;
        uint32_t planningCalls = 0;
        auto planner = [&](const Sora::Compute::ExecutionGraph& graph, const Sora::Compute::GraphPlanningContext&,
                           Sora::Compute::GraphPlanningServices services) noexcept
            -> std::expected<Sora::Compute::PhysicalGraphPlanStorage, TestGraphPlanningError> {
            ++planningCalls;
            if (graph.NodeCount() != 1 || services.packages.GeneratedProgramCount() != 1) {
                return std::unexpected(TestGraphPlanningError::InvalidInput);
            }
            const auto* invocation = std::get_if<Sora::Compute::KernelInvocation>(&graph.Operation({0}));
            if (invocation == nullptr || !services.packages.Resolve(invocation->ProgramHash())) {
                return std::unexpected(TestGraphPlanningError::InvalidInput);
            }
            return std::move(MakeDispatchPlanStorage(state).storage);
        };
        auto context = Sora::Compute::CreateComputeContext(MakeRuntimeEpoch(state), {.fusion = ForcedFusionPolicy()},
                                                           planner, RejectRuntimeRecovery);
        REQUIRE(context.has_value());
        REQUIRE(context->RegisterProgram(kStridedCopy).has_value());

        exec::static_thread_pool pool{1};
        const auto completed =
            stdexec::sync_wait(context->Submit(pool.get_scheduler(), MakeLogicalFusionChainGraph(99, 3)));
        REQUIRE(completed.has_value());
        REQUIRE(planningCalls == 1);
        REQUIRE(context->OptimizationStatistics() ==
                Sora::Compute::GraphOptimizationStatistics{.attempts = 1, .fused = 1});
        REQUIRE(context->Packages().Snapshot()->PackageCount() == 1);
    }

    SECTION("two independent regions share one generated program identity") {
        MockState state;
        uint32_t planningCalls = 0;
        auto planner = [&](const Sora::Compute::ExecutionGraph& graph, const Sora::Compute::GraphPlanningContext&,
                           Sora::Compute::GraphPlanningServices services) noexcept
            -> std::expected<Sora::Compute::PhysicalGraphPlanStorage, TestGraphPlanningError> {
            ++planningCalls;
            if (graph.NodeCount() != 2 || graph.EdgeCount() != 0 || services.packages.GeneratedProgramCount() != 1) {
                return std::unexpected(TestGraphPlanningError::InvalidInput);
            }
            for (uint32_t index = 0; index < graph.NodeCount(); ++index) {
                const auto* invocation = std::get_if<Sora::Compute::KernelInvocation>(&graph.Operation({index}));
                if (invocation == nullptr || !services.packages.Resolve(invocation->ProgramHash())) {
                    return std::unexpected(TestGraphPlanningError::InvalidInput);
                }
            }
            return std::move(MakeDispatchPlanStorage(state).storage);
        };
        auto context = Sora::Compute::CreateComputeContext(MakeRuntimeEpoch(state), {.fusion = ForcedFusionPolicy()},
                                                           planner, RejectRuntimeRecovery);
        REQUIRE(context.has_value());
        REQUIRE(context->RegisterProgram(kStridedCopy).has_value());

        exec::static_thread_pool pool{1};
        const auto completed =
            stdexec::sync_wait(context->Submit(pool.get_scheduler(), MakeLogicalIndependentFusionGraph(100)));
        REQUIRE(completed.has_value());
        REQUIRE(planningCalls == 1);
        REQUIRE(context->OptimizationStatistics() ==
                Sora::Compute::GraphOptimizationStatistics{.attempts = 1, .fused = 1});
        REQUIRE(context->Packages().Snapshot()->PackageCount() == 1);
    }
}

TEST_CASE("ComputeContext recovery retries reuse one prepared fused graph",
          "[Sora][Compute][Runtime][ComputeContext][GraphOptimizer][Recovery]") {
    MockState state;
    state.submissionFailuresRemaining = 1;
    uint32_t planningCalls = 0;
    auto planner = [&](const Sora::Compute::ExecutionGraph& graph, const Sora::Compute::GraphPlanningContext& planning,
                       Sora::Compute::GraphPlanningServices services) noexcept
        -> std::expected<Sora::Compute::PhysicalGraphPlanStorage, TestGraphPlanningError> {
        ++planningCalls;
        if (graph.NodeCount() != 1 || services.packages.GeneratedProgramCount() != 1 ||
            planning.attempt + 1 != planningCalls) {
            return std::unexpected(TestGraphPlanningError::InvalidInput);
        }
        return std::move(MakeDispatchPlanStorage(state, false, false, false, 70 + planningCalls).storage);
    };
    auto context = Sora::Compute::CreateComputeContext(MakeRuntimeEpoch(state), {.fusion = ForcedFusionPolicy()},
                                                       planner, RejectRuntimeRecovery);
    REQUIRE(context.has_value());
    REQUIRE(context->RegisterProgram(kStridedCopy).has_value());

    exec::static_thread_pool pool{1};
    const auto completed = stdexec::sync_wait(context->Submit(pool.get_scheduler(), MakeLogicalFusionGraph(97)));
    REQUIRE(completed.has_value());
    REQUIRE(planningCalls == 2);
    REQUIRE(context->OptimizationStatistics() == Sora::Compute::GraphOptimizationStatistics{.attempts = 1, .fused = 1});
    REQUIRE(context->Packages().Snapshot()->PackageCount() == 1);
}

TEST_CASE("ComputeContext resolves exact portable-package seeds and JITs target mismatches transparently",
          "[Sora][Compute][Runtime][ComputeContext][PortablePackage]") {
    SECTION("exact executable seed bypasses compilation") {
        MockState state;
        auto fixture = MakeDispatchPlanStorage(state);
        const auto& dispatch = std::get<Sora::Compute::PlannedDispatch>(fixture.storage.segments.back());
        auto package = MakeDispatchPackage(dispatch, kGpuTarget);
        auto archive = Sora::Compute::PortablePackageArchive::Create(package);
        REQUIRE(archive.has_value());
        auto planner = [storage = std::move(fixture.storage)](
                           const Sora::Compute::ExecutionGraph&, const Sora::Compute::GraphPlanningContext&,
                           Sora::Compute::GraphPlanningServices services) mutable noexcept
            -> std::expected<Sora::Compute::PhysicalGraphPlanStorage, TestGraphPlanningError> {
            if (services.packages.ArtifactCount() != 1) {
                return std::unexpected(TestGraphPlanningError::InvalidInput);
            }
            return std::move(storage);
        };
        auto context =
            Sora::Compute::CreateComputeContext(MakeRuntimeEpoch(state), {}, std::move(planner), RejectRuntimeRecovery);
        REQUIRE(context.has_value());
        const auto registered = context->RegisterTrustedPackageArchive(archive->Bytes());
        REQUIRE(registered.has_value());
        REQUIRE(registered->Provenance().size() == 1);
        REQUIRE(registered->Provenance().front() == Sora::Compute::PortablePackageProvenance::TrustedArchive());

        exec::static_thread_pool pool{1};
        const auto completed = stdexec::sync_wait(context->Submit(pool.get_scheduler(), MakeLogicalFillGraph({94, 1})));
        REQUIRE(completed.has_value());
        REQUIRE(state.artifactImportCount == 1);
        REQUIRE(state.compileCount == 0);
        REQUIRE(state.pipelineImportCount == 0);
        REQUIRE(state.pipelineCreateCount == 0);
    }

    SECTION("exact executable and pipeline seeds bypass both creation stages") {
        MockState state;
        auto fixture = MakeDispatchPlanStorage(state, false, false, true);
        const auto& dispatch = std::get<Sora::Compute::PlannedDispatch>(fixture.storage.segments.back());
        auto package = MakeDispatchPackage(dispatch, kGpuTarget);
        auto archive = Sora::Compute::PortablePackageArchive::Create(package);
        REQUIRE(archive.has_value());
        auto planner = [storage = std::move(fixture.storage)](
                           const Sora::Compute::ExecutionGraph&, const Sora::Compute::GraphPlanningContext&,
                           Sora::Compute::GraphPlanningServices services) mutable noexcept
            -> std::expected<Sora::Compute::PhysicalGraphPlanStorage, TestGraphPlanningError> {
            if (services.packages.ArtifactCount() != 2) {
                return std::unexpected(TestGraphPlanningError::InvalidInput);
            }
            return std::move(storage);
        };
        auto context =
            Sora::Compute::CreateComputeContext(MakeRuntimeEpoch(state, Sora::Compute::ArtifactKind::DevicePipeline),
                                                {}, std::move(planner), RejectRuntimeRecovery);
        REQUIRE(context.has_value());
        constexpr std::array keyIdentity{std::byte{0x11}};
        constexpr std::array signatureBytes{std::byte{0x22}};
        const Sora::Compute::PortablePackageDetachedSignature signature{
            .scheme = 1,
            .keyIdentity = keyIdentity,
            .signature = signatureBytes,
        };
        uint32_t verificationCalls = 0;
        const auto registered = context->RegisterVerifiedPackageArchive(archive->Bytes(), signature,
                                                                        ArchiveVerifier{.calls = &verificationCalls});
        REQUIRE(registered.has_value());
        REQUIRE(registered->Provenance().size() == 1);
        REQUIRE(registered->Provenance().front() == Sora::Compute::PortablePackageProvenance::Verified({
                                                        .scheme = signature.scheme,
                                                        .authority = {0xA1, 0xA2},
                                                        .subject = {0xB1, 0xB2},
                                                        .policyRevision = 7,
                                                    }));
        REQUIRE(verificationCalls == 1);

        exec::static_thread_pool pool{1};
        const auto completed = stdexec::sync_wait(context->Submit(pool.get_scheduler(), MakeLogicalFillGraph({94, 2})));
        REQUIRE(completed.has_value());
        REQUIRE(state.artifactImportCount == 1);
        REQUIRE(state.compileCount == 0);
        REQUIRE(state.pipelineImportCount == 1);
        REQUIRE(state.pipelineCreateCount == 0);

        const auto revoked = context->RevokePackageAdmissions(
            Sora::Compute::PortablePackageAdmissionRevocation::ForSubject(1, {0xA1, 0xA2}, {0xB1, 0xB2}));
        REQUIRE(revoked.has_value());
        REQUIRE(revoked->provenanceRemoved == 1);
        REQUIRE(revoked->packagesUpdated == 0);
        REQUIRE(revoked->packagesErased == 1);
        REQUIRE(context->Packages().Snapshot()->PackageCount() == 0);
        REQUIRE(registered->Provenance().size() == 1);
    }

    SECTION("Vulkan device mismatch reuses executable bytes and recreates the pipeline") {
        MockState state;
        auto fixture = MakeDispatchPlanStorage(state, false, false, true);
        const auto& dispatch = std::get<Sora::Compute::PlannedDispatch>(fixture.storage.segments.back());
        auto packagedTarget = kGpuTarget;
        ++packagedTarget.architectureId;
        auto package = MakeDispatchPackage(dispatch, packagedTarget);
        auto planner = [storage = std::move(fixture.storage)](
                           const Sora::Compute::ExecutionGraph&, const Sora::Compute::GraphPlanningContext&,
                           Sora::Compute::GraphPlanningServices services) mutable noexcept
            -> std::expected<Sora::Compute::PhysicalGraphPlanStorage, TestGraphPlanningError> {
            if (services.packages.ArtifactCount() != 2) {
                return std::unexpected(TestGraphPlanningError::InvalidInput);
            }
            return std::move(storage);
        };
        auto context =
            Sora::Compute::CreateComputeContext(MakeRuntimeEpoch(state, Sora::Compute::ArtifactKind::DevicePipeline),
                                                {}, std::move(planner), RejectRuntimeRecovery);
        REQUIRE(context.has_value());
        REQUIRE(context->RegisterPackage(std::move(package)).has_value());

        exec::static_thread_pool pool{1};
        const auto completed = stdexec::sync_wait(context->Submit(pool.get_scheduler(), MakeLogicalFillGraph({94, 3})));
        REQUIRE(completed.has_value());
        REQUIRE(state.artifactImportCount == 1);
        REQUIRE(state.compileCount == 0);
        REQUIRE(state.pipelineImportCount == 0);
        REQUIRE(state.pipelineCreateCount == 1);
    }
}

TEST_CASE("ComputeContext reuses persistent target artifacts across independent process-local contexts",
          "[Sora][Compute][Runtime][ComputeContext][ArtifactStore]") {
    TemporaryDirectory temporary;
    const auto run = [&](MockState& state, Sora::Compute::BufferId logicalId) {
        auto fixture = MakeDispatchPlanStorage(state, false, false, true);
        auto planner = [storage = std::move(fixture.storage)](const Sora::Compute::ExecutionGraph&,
                                                              const Sora::Compute::GraphPlanningContext&,
                                                              Sora::Compute::GraphPlanningServices) mutable noexcept
            -> std::expected<Sora::Compute::PhysicalGraphPlanStorage, TestGraphPlanningError> {
            return std::move(storage);
        };
        Sora::Compute::ComputeContextConfig config{
            .artifactCacheCapacity = 0,
            .artifactStore =
                Sora::Compute::ArtifactStoreConfig{
                    .root = temporary.Path(),
                    .maximumPayloadBytes = 4096,
                    .durableWrites = false,
                },
        };
        auto context = Sora::Compute::CreateComputeContext(
            MakeRuntimeEpoch(state, Sora::Compute::ArtifactKind::DevicePipeline, true), std::move(config),
            std::move(planner), RejectRuntimeRecovery);
        REQUIRE(context.has_value());

        exec::static_thread_pool pool{1};
        const auto completed =
            stdexec::sync_wait(context->Submit(pool.get_scheduler(), MakeLogicalFillGraph(logicalId)));
        REQUIRE(completed.has_value());
        REQUIRE(std::get<0>(*completed).status == Sora::Compute::EventStatus::Completed);
    };

    MockState cold;
    run(cold, {94, 4});
    REQUIRE(cold.compileCount == 1);
    REQUIRE(cold.artifactExportCount == 1);
    REQUIRE(cold.artifactImportCount == 0);
    REQUIRE(cold.pipelineCreateCount == 1);
    REQUIRE(cold.pipelineExportCount == 1);
    REQUIRE(cold.pipelineImportCount == 0);

    MockState warm;
    run(warm, {94, 5});
    REQUIRE(warm.compileCount == 0);
    REQUIRE(warm.artifactExportCount == 0);
    REQUIRE(warm.artifactImportCount == 1);
    REQUIRE(warm.pipelineCreateCount == 0);
    REQUIRE(warm.pipelineExportCount == 0);
    REQUIRE(warm.pipelineImportCount == 1);
}

TEST_CASE("ComputeContext archive registration preserves trust and publication error domains",
          "[Sora][Compute][Runtime][ComputeContext][PortablePackage][Error]") {
    MockState state;
    auto fixture = MakeDispatchPlanStorage(state);
    const auto& dispatch = std::get<Sora::Compute::PlannedDispatch>(fixture.storage.segments.back());
    auto package = MakeDispatchPackage(dispatch, kGpuTarget);
    auto archive = Sora::Compute::PortablePackageArchive::Create(package);
    REQUIRE(archive.has_value());

    auto planner = [](const Sora::Compute::ExecutionGraph&, const Sora::Compute::GraphPlanningContext&,
                      Sora::Compute::GraphPlanningServices) noexcept
        -> std::expected<Sora::Compute::PhysicalGraphPlanStorage, TestGraphPlanningError> {
        return std::unexpected(TestGraphPlanningError::InvalidInput);
    };
    auto context = Sora::Compute::CreateComputeContext(
        MakeRuntimeEpoch(state),
        {.packages = {.maximumPackages = 1, .maximumArtifacts = 0, .maximumOwnedBytes = 1'024 * 1'024}}, planner,
        RejectRuntimeRecovery);
    REQUIRE(context.has_value());

    const auto publication = context->RegisterTrustedPackageArchive(archive->Bytes());
    REQUIRE_FALSE(publication.has_value());
    const auto* registryError = std::get_if<Sora::Compute::PortablePackageRegistryError>(&publication.error());
    REQUIRE(registryError != nullptr);
    REQUIRE(registryError->code == Sora::Compute::PortablePackageRegistryErrorCode::ArtifactCapacityExceeded);
    REQUIRE(context->Packages().Snapshot()->PackageCount() == 0);

    constexpr std::array keyIdentity{std::byte{0x11}};
    constexpr std::array signatureBytes{std::byte{0x22}};
    const Sora::Compute::PortablePackageDetachedSignature signature{
        .scheme = 1,
        .keyIdentity = keyIdentity,
        .signature = signatureBytes,
    };
    uint32_t verificationCalls = 0;
    const auto trust = context->RegisterVerifiedPackageArchive(
        archive->Bytes(), signature, ArchiveVerifier{.calls = &verificationCalls, .accept = false});
    REQUIRE_FALSE(trust.has_value());
    const auto* archiveError = std::get_if<Sora::Compute::PortablePackageArchiveError>(&trust.error());
    REQUIRE(archiveError != nullptr);
    REQUIRE(archiveError->code == Sora::Compute::PortablePackageArchiveErrorCode::SignatureRejected);
    REQUIRE(verificationCalls == 1);
    REQUIRE(context->Packages().Snapshot()->PackageCount() == 0);
}

TEST_CASE("ComputeContext preserves planning and materialization failures "
          "without exposing an attempt factory",
          "[Sora][Compute][Runtime][ComputeContext][Error]") {
    SECTION("planning failure and sender lifetime") {
        MockState state;
        uint32_t planningCalls = 0;
        auto planner = [&](const Sora::Compute::ExecutionGraph&, const Sora::Compute::GraphPlanningContext&,
                           Sora::Compute::GraphPlanningServices) noexcept
            -> std::expected<Sora::Compute::PhysicalGraphPlanStorage, TestGraphPlanningError> {
            ++planningCalls;
            return std::unexpected(TestGraphPlanningError::InvalidInput);
        };
        auto context = Sora::Compute::CreateComputeContext(MakeRuntimeEpoch(state), {}, planner, RejectRuntimeRecovery);
        REQUIRE(context.has_value());
        using Context = typename decltype(context)::value_type;

        exec::static_thread_pool pool{1};
        auto sender = context->Submit(pool.get_scheduler(), MakeLogicalFillGraph({91, 1}));
        context = std::unexpected(
            Sora::Compute::ComputeContextError{Sora::Compute::ComputeContextErrorCode::InvalidConfiguration});
        auto failure = WaitForContextError<typename Context::Error>(std::move(sender));
        const auto* build =
            std::get_if<Sora::Compute::GraphAttemptBuildFailure<typename Context::PreparationError>>(&failure);
        REQUIRE(build != nullptr);
        const auto* planning = std::get_if<Sora::Compute::GraphPlanningFailure<TestGraphPlanningError>>(&build->error);
        REQUIRE(planning != nullptr);
        REQUIRE(planning->error == TestGraphPlanningError::InvalidInput);
        REQUIRE(planningCalls == 1);
        REQUIRE(state.allocateCount == 0);
        REQUIRE(state.submitCount == 0);
    }

    SECTION("transactional materialization failure") {
        MockState state;
        state.overlapAllocations = true;
        auto planner = [&](const Sora::Compute::ExecutionGraph&, const Sora::Compute::GraphPlanningContext&,
                           Sora::Compute::GraphPlanningServices) noexcept
            -> std::expected<Sora::Compute::PhysicalGraphPlanStorage, TestGraphPlanningError> {
            auto fixture = MakeFillCopyPlanStorage(state);
            return std::move(fixture.storage);
        };
        auto context = Sora::Compute::CreateComputeContext(MakeRuntimeEpoch(state), {}, planner, RejectRuntimeRecovery);
        REQUIRE(context.has_value());
        using Context = typename decltype(context)::value_type;

        exec::static_thread_pool pool{1};
        auto failure = WaitForContextError<typename Context::Error>(
            context->Submit(pool.get_scheduler(), MakeLogicalFillGraph({92, 1})));
        const auto* build =
            std::get_if<Sora::Compute::GraphAttemptBuildFailure<typename Context::PreparationError>>(&failure);
        REQUIRE(build != nullptr);
        const auto* materialization = std::get_if<Sora::Compute::GraphMaterializationFailure>(&build->error);
        REQUIRE(materialization != nullptr);
        REQUIRE(materialization->error.code ==
                Sora::Compute::PhysicalGraphMaterializationErrorCode::BatchConstructionFailed);
        REQUIRE(materialization->error.segment == 1);
        REQUIRE(state.submitCount == 0);
        REQUIRE(state.destroyedAllocationCount == 2);
    }

    SECTION("pure plan validation failure") {
        MockState state;
        auto planner = [](const Sora::Compute::ExecutionGraph&, const Sora::Compute::GraphPlanningContext&,
                          Sora::Compute::GraphPlanningServices) noexcept
            -> std::expected<Sora::Compute::PhysicalGraphPlanStorage, TestGraphPlanningError> { return {}; };
        auto context = Sora::Compute::CreateComputeContext(MakeRuntimeEpoch(state), {}, planner, RejectRuntimeRecovery);
        REQUIRE(context.has_value());
        using Context = typename decltype(context)::value_type;

        exec::static_thread_pool pool{1};
        auto failure = WaitForContextError<typename Context::Error>(
            context->Submit(pool.get_scheduler(), MakeLogicalFillGraph({93, 1})));
        const auto* build =
            std::get_if<Sora::Compute::GraphAttemptBuildFailure<typename Context::PreparationError>>(&failure);
        REQUIRE(build != nullptr);
        const auto* validation = std::get_if<Sora::Compute::GraphPlanValidationFailure>(&build->error);
        REQUIRE(validation != nullptr);
        REQUIRE(validation->error.code == Sora::Compute::PhysicalGraphPlanErrorCode::EmptyPlan);
        REQUIRE(state.allocateCount == 0);
        REQUIRE(state.submitCount == 0);
    }
}

TEST_CASE("PhysicalGraphMaterializer builds and executes transfer-dispatch "
          "publication transactionally",
          "[Sora][Compute][Runtime][PhysicalGraphMaterializer]") {
    MockState state;
    auto catalog = MakeCatalog(state);
    auto fixture = MakeDispatchPlan(state, catalog);
    Sora::Compute::ArtifactCache cache{4};
    Sora::Compute::CompilerService compilerService{cache};
    Sora::Compute::PipelineService pipelineService{cache};

    auto graph = Sora::Compute::MaterializePhysicalGraph(fixture.plan, catalog, {compilerService, pipelineService});
    REQUIRE(graph.has_value());
    REQUIRE(graph->SegmentCount() == 2);
    REQUIRE(state.allocateCount == 2);
    REQUIRE(state.compileCount == 1);
    REQUIRE(state.transferCount == 0);
    REQUIRE(state.submitCount == 0);
    REQUIRE(fixture.output.CurrentVersion() == 0);

    auto submitted = std::move(*graph).Submit();
    REQUIRE(submitted.has_value());
    REQUIRE(state.transferCount == 1);
    REQUIRE(state.submitCount == 1);
    REQUIRE(state.submittedCommands[0] == Sora::Compute::BackendCommandKind::Dispatch);
    REQUIRE(state.submittedDispatchArtifactKind == Sora::Compute::ArtifactKind::DeviceExecutable);
    REQUIRE(state.submittedWaitCounts[0] == 1);
    REQUIRE(state.dispatchWorkgroups == std::array<uint32_t, 3>{1, 1, 1});
    REQUIRE(fixture.output.CurrentVersion() == 0);

    const auto observation = submitted->Wait(UINT64_MAX);
    REQUIRE(observation.status == Sora::Compute::EventStatus::Completed);
    REQUIRE(fixture.output.CurrentVersion() == 1);
    const auto snapshot = fixture.output.SnapshotResidency();
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->replicas.size() == 1);
    REQUIRE(snapshot->replicas[0].allocation.Location() == kDevice);
}

TEST_CASE("PhysicalGraphMaterializer preserves and publishes an exact affine partition",
          "[Sora][Compute][Runtime][PhysicalGraphMaterializer][Partition][Affine]") {
    MockState state;
    auto catalog = MakeCatalog(state);
    auto fixture = MakeAffinePartitionPlan(state, catalog);
    Sora::Compute::ArtifactCache cache{4};
    Sora::Compute::CompilerService compilerService{cache};
    Sora::Compute::PipelineService pipelineService{cache};

    auto graph = Sora::Compute::MaterializePhysicalGraph(fixture.plan, catalog, {compilerService, pipelineService});
    REQUIRE(graph.has_value());
    REQUIRE(graph->SegmentCount() == 2);
    REQUIRE(state.allocateCount == 2);
    REQUIRE(state.compileCount == 1);
    REQUIRE(fixture.output.CurrentVersion() == 0);

    auto submitted = std::move(*graph).Submit();
    REQUIRE(submitted.has_value());
    REQUIRE(state.submitCount == 2);
    REQUIRE(fixture.output.CurrentVersion() == 0);

    const auto observation = submitted->Wait(UINT64_MAX);
    REQUIRE(observation.status == Sora::Compute::EventStatus::Completed);
    REQUIRE(fixture.output.CurrentVersion() == 1);
    const auto snapshot = fixture.output.SnapshotResidency();
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->replicas.size() == 2);
    std::array<Sora::Compute::ByteCoverageView, 2> coverages{};
    for (size_t index = 0; index < snapshot->replicas.size(); ++index) {
        REQUIRE(snapshot->replicas[index].byteFootprint != nullptr);
        coverages[index] = snapshot->replicas[index].Coverage();
    }
    REQUIRE(Sora::Compute::ExactCoverageUnion<16>(fixture.output.LogicalCoverage(), coverages) ==
            Sora::Compute::CoverageEquivalence::Equal);
    const auto padding = Sora::Compute::ByteRegion::Make(sizeof(float), sizeof(float));
    REQUIRE(padding.has_value());
    const auto gap = fixture.output.AcquireReplica(kDevice, *padding);
    REQUIRE_FALSE(gap.has_value());
    REQUIRE(gap.error() == Sora::Compute::MemoryError::NotResident);
}

TEST_CASE("PhysicalGraphMaterializer projects an exact affine transfer into one scatter gather submission",
          "[Sora][Compute][Runtime][PhysicalGraphMaterializer][Transfer][Affine]") {
    MockState state;
    auto catalog = MakeCatalog(state);
    auto input = MakeStridedBuffer({75, 3});
    REQUIRE(input.CommitWholeWrite(MakeAllocation(state, kHost, 64, kStridedBufferBytes)).has_value());
    auto inputReplica = input.AcquireReplica(kHost, Sora::Compute::ByteCoverageView::Affine(*kStridedFootprint));
    REQUIRE(inputReplica.has_value());

    Sora::Compute::PhysicalGraphPlanStorage storage;
    storage.allocations.push_back(
        {.id = {0},
         .buffer = input,
         .service = kService,
         .location = kDevice,
         .bufferRegion = kStridedFootprint->BoundingRegion(),
         .byteFootprint = *kStridedFootprint,
         .version = 1,
         .materialization = Sora::Compute::PlannedAllocate{
             .requiredProperties = Sora::Compute::MemoryProperty::DeviceLocal, .alignment = 64}});
    storage.segments.emplace_back(Sora::Compute::PlannedTransfer{
        .logicalNode = {0},
        .service = kService,
        .engine = kCopyEngine,
        .buffer = input.Describe().id,
        .region = kStridedFootprint->BoundingRegion(),
        .byteFootprint = *kStridedFootprint,
        .version = 1,
        .source = Sora::Compute::ReadyStorageRef{input.Describe().id, *inputReplica},
        .destination = {0},
    });
    auto plan = Sora::Compute::PhysicalGraphPlan::Create(std::move(storage), catalog);
    REQUIRE(plan.has_value());
    Sora::Compute::ArtifactCache cache{4};
    Sora::Compute::CompilerService compilerService{cache};
    Sora::Compute::PipelineService pipelineService{cache};

    auto graph = Sora::Compute::MaterializePhysicalGraph(*plan, catalog, {compilerService, pipelineService});
    REQUIRE(graph.has_value());
    REQUIRE(graph->SegmentCount() == 1);
    auto submitted = std::move(*graph).Submit();
    REQUIRE(submitted.has_value());
    REQUIRE(state.transferCount == 1);
    REQUIRE(state.transferRegionCount == kStridedElementCount);
    for (uint64_t index = 0; index < kStridedElementCount; ++index) {
        const Sora::Compute::TransferRegion expected{
            .sourceOffset = index * 8, .targetOffset = index * 8, .sizeBytes = sizeof(float)};
        REQUIRE(state.transferRegions[index] == expected);
    }
    REQUIRE(submitted->Wait(UINT64_MAX).status == Sora::Compute::EventStatus::Completed);
}

TEST_CASE("PhysicalGraphMaterializer assembles an affine partition cover into one dispatch input",
          "[Sora][Compute][Runtime][PhysicalGraphMaterializer][Assembly][Affine]") {
    MockState state;
    auto catalog = MakeCatalog(state);
    auto fixture = MakeAssemblyDispatchPlan(state, catalog);
    Sora::Compute::ArtifactCache cache{4};
    Sora::Compute::CompilerService compilerService{cache};
    Sora::Compute::PipelineService pipelineService{cache};

    auto graph = Sora::Compute::MaterializePhysicalGraph(fixture.plan, catalog, {compilerService, pipelineService});
    REQUIRE(graph.has_value());
    REQUIRE(graph->SegmentCount() == 2);
    REQUIRE(state.allocateCount == 2);
    REQUIRE(state.compileCount == 1);
    REQUIRE(state.submitCount == 0);
    REQUIRE(fixture.output.CurrentVersion() == 0);

    auto submitted = std::move(*graph).Submit();
    REQUIRE(submitted.has_value());
    REQUIRE(state.submitCount == 2);
    REQUIRE(state.submittedCommandCounts[0] == 2);
    REQUIRE(state.submittedCommandCounts[1] == 1);
    REQUIRE(state.submittedBindingCounts[0] == 0);
    REQUIRE(state.submittedBindingCounts[1] == 2);
    REQUIRE(state.submittedWaitCounts[0] == 0);
    REQUIRE(state.submittedWaitCounts[1] == 1);

    const SubmittedCommandObservation& first = state.submittedCommandDetails[0][0];
    const SubmittedCommandObservation& second = state.submittedCommandDetails[0][1];
    REQUIRE(first.kind == Sora::Compute::BackendCommandKind::Copy);
    REQUIRE(second.kind == Sora::Compute::BackendCommandKind::Copy);
    REQUIRE(first.source != second.source);
    REQUIRE(first.target == second.target);
    REQUIRE(first.source != first.target);
    REQUIRE(second.source != second.target);
    REQUIRE(first.sourceOffset == 0);
    REQUIRE(second.sourceOffset == 0);
    REQUIRE(first.targetOffset == 0);
    REQUIRE(second.targetOffset == 64);
    REQUIRE(first.sizeBytes == 60);
    REQUIRE(second.sizeBytes == 60);

    REQUIRE(state.submittedCommands[1] == Sora::Compute::BackendCommandKind::Dispatch);
    REQUIRE(state.dispatchReadBindingCount == 1);
    REQUIRE(state.dispatchReadBinding.kind == Sora::Compute::BackendBindingKind::AffineElements);
    REQUIRE(state.dispatchReadBinding.access == Sora::Compute::AccessMode::Read);
    REQUIRE(state.dispatchReadBinding.allocation.memory == first.target);
    REQUIRE(state.dispatchReadBinding.offset == 0);
    REQUIRE(state.dispatchReadBinding.size == kStridedBufferBytes);
    REQUIRE(state.dispatchReadBinding.rank == 1);
    REQUIRE(state.dispatchReadBinding.extents[0] == kStridedElementCount);
    REQUIRE(state.dispatchReadBinding.byteStrides[0] == kStridedByteStrides[0]);

    REQUIRE(submitted->Wait(UINT64_MAX).status == Sora::Compute::EventStatus::Completed);
    REQUIRE(fixture.output.CurrentVersion() == 1);
}

TEST_CASE("PhysicalGraphMaterializer joins two direct-fill transfers with a signal-only assembly",
          "[Sora][Compute][Runtime][PhysicalGraphMaterializer][Assembly][Prefill]") {
    MockState state;
    auto catalog = MakeCatalog(state);
    auto fixture = MakeAssemblyDispatchPlan(state, catalog, 2);
    Sora::Compute::ArtifactCache cache{4};
    Sora::Compute::CompilerService compilerService{cache};
    Sora::Compute::PipelineService pipelineService{cache};

    auto graph = Sora::Compute::MaterializePhysicalGraph(fixture.plan, catalog, {compilerService, pipelineService});
    REQUIRE(graph.has_value());
    REQUIRE(graph->SegmentCount() == 4);
    REQUIRE(state.allocateCount == 2);
    REQUIRE(state.submitCount == 0);
    REQUIRE(state.transferCount == 0);

    auto submitted = std::move(*graph).Submit();
    REQUIRE(submitted.has_value());
    REQUIRE(state.transferCount == 2);
    REQUIRE(state.transferSources[0] != state.transferSources[1]);
    REQUIRE(state.transferTargets[0] == state.transferTargets[1]);
    REQUIRE(state.transferSources[0] != state.transferTargets[0]);
    REQUIRE(state.transferSources[1] != state.transferTargets[1]);
    REQUIRE(state.submitCount == 2);
    REQUIRE(state.submittedCommandCounts[0] == 0);
    REQUIRE(state.submittedWaitCounts[0] == 2);
    REQUIRE(state.submittedCommandCounts[1] == 1);
    REQUIRE(state.submittedWaitCounts[1] == 1);
    REQUIRE(state.dispatchReadBindingCount == 1);
    REQUIRE(state.dispatchReadBinding.allocation.memory == state.transferTargets[0]);
    REQUIRE(submitted->Wait(UINT64_MAX).status == Sora::Compute::EventStatus::Completed);
    REQUIRE(fixture.output.CurrentVersion() == 1);
}

TEST_CASE("PhysicalGraphMaterializer combines a remote prefill with one local assembly copy",
          "[Sora][Compute][Runtime][PhysicalGraphMaterializer][Assembly][Mixed]") {
    MockState state;
    auto catalog = MakeCatalog(state);
    auto fixture = MakeAssemblyDispatchPlan(state, catalog, 1);
    Sora::Compute::ArtifactCache cache{4};
    Sora::Compute::CompilerService compilerService{cache};
    Sora::Compute::PipelineService pipelineService{cache};

    auto graph = Sora::Compute::MaterializePhysicalGraph(fixture.plan, catalog, {compilerService, pipelineService});
    REQUIRE(graph.has_value());
    REQUIRE(graph->SegmentCount() == 3);
    auto submitted = std::move(*graph).Submit();
    REQUIRE(submitted.has_value());
    REQUIRE(state.transferCount == 1);
    REQUIRE(state.submitCount == 2);
    REQUIRE(state.submittedCommandCounts[0] == 1);
    REQUIRE(state.submittedWaitCounts[0] == 1);
    REQUIRE(state.submittedCommandDetails[0][0].kind == Sora::Compute::BackendCommandKind::Copy);
    REQUIRE(state.submittedCommandDetails[0][0].target == state.transferTargets[0]);
    REQUIRE(state.submittedCommandDetails[0][0].source != state.transferTargets[0]);
    REQUIRE(state.submittedCommandCounts[1] == 1);
    REQUIRE(state.submittedWaitCounts[1] == 1);
    REQUIRE(state.dispatchReadBindingCount == 1);
    REQUIRE(state.dispatchReadBinding.allocation.memory == state.transferTargets[0]);
    REQUIRE(submitted->Wait(UINT64_MAX).status == Sora::Compute::EventStatus::Completed);
    REQUIRE(fixture.output.CurrentVersion() == 1);
}

TEST_CASE("PhysicalGraphMaterializer resolves and submits a driver pipeline "
          "without exposing the second stage",
          "[Sora][Compute][Runtime][PhysicalGraphMaterializer][Pipeline]") {
    MockState state;
    auto catalog = MakeCatalog(state, Sora::Compute::ArtifactKind::DevicePipeline);
    auto fixture = MakeDispatchPlan(state, catalog, false, false, true);
    const auto& dispatch = std::get<Sora::Compute::PlannedDispatch>(fixture.plan.Segments()[1]);
    const auto expectedBindingLayout = Sora::Compute::PipelineBindingLayoutIdentity(dispatch.invocation.Signature());
    Sora::Compute::ArtifactCache cache{0};
    Sora::Compute::CompilerService compilerService{cache};
    Sora::Compute::PipelineService pipelineService{cache};

    {
        auto graph = Sora::Compute::MaterializePhysicalGraph(fixture.plan, catalog, {compilerService, pipelineService});
        REQUIRE(graph.has_value());
        REQUIRE(state.compileCount == 1);
        REQUIRE(state.pipelineCreateCount == 1);
        REQUIRE(state.pipelineBindingLayout == expectedBindingLayout);
        REQUIRE(state.artifactDestroyCount == 1);

        auto submitted = std::move(*graph).Submit();
        REQUIRE(submitted.has_value());
        REQUIRE(state.submittedDispatchArtifactKind == Sora::Compute::ArtifactKind::DevicePipeline);
        REQUIRE(submitted->Wait(UINT64_MAX).status == Sora::Compute::EventStatus::Completed);
    }
    REQUIRE(state.artifactDestroyCount == 2);
}

TEST_CASE("PhysicalGraphMaterializer lowers cross-domain dependencies to host "
          "completion",
          "[Sora][Compute][Runtime][PhysicalGraphMaterializer][Dependency]") {
    MockState state;
    state.transferCompletionDevice = kCpu;
    state.transferCompletionBackend = Sora::Compute::BackendKind::Cpu;
    auto catalog = MakeCatalog(state);
    auto fixture = MakeDispatchPlan(state, catalog);
    Sora::Compute::ArtifactCache cache{4};
    Sora::Compute::CompilerService compilerService{cache};
    Sora::Compute::PipelineService pipelineService{cache};

    auto graph = Sora::Compute::MaterializePhysicalGraph(fixture.plan, catalog, {compilerService, pipelineService});
    REQUIRE(graph.has_value());
    auto submitted = std::move(*graph).Submit();
    REQUIRE(submitted.has_value());
    REQUIRE(state.transferCount == 1);
    REQUIRE(state.eventWaitCount == 1);
    REQUIRE(state.submitCount == 1);
    REQUIRE(state.submittedWaitCounts[0] == 0);
    REQUIRE(submitted->Wait(UINT64_MAX).status == Sora::Compute::EventStatus::Completed);
    REQUIRE(fixture.output.CurrentVersion() == 1);
}

TEST_CASE("PhysicalGraphMaterializer imports coherent aliases as graph-local "
          "dispatch inputs",
          "[Sora][Compute][Runtime][PhysicalGraphMaterializer][Alias]") {
    MockState state;
    auto catalog = MakeCatalog(state);
    auto fixture = MakeDispatchPlan(state, catalog, false, true);
    Sora::Compute::ArtifactCache cache{4};
    Sora::Compute::CompilerService compilerService{cache};
    Sora::Compute::PipelineService pipelineService{cache};

    auto graph = Sora::Compute::MaterializePhysicalGraph(fixture.plan, catalog, {compilerService, pipelineService});
    REQUIRE(graph.has_value());
    REQUIRE(graph->SegmentCount() == 1);
    REQUIRE(state.aliasCount == 1);
    REQUIRE(state.allocateCount == 1);
    REQUIRE(state.compileCount == 1);
    REQUIRE(state.submitCount == 0);

    auto submitted = std::move(*graph).Submit();
    REQUIRE(submitted.has_value());
    REQUIRE(state.submitCount == 1);
    REQUIRE(state.submittedWaitCounts[0] == 0);
    REQUIRE(submitted->Wait(UINT64_MAX).status == Sora::Compute::EventStatus::Completed);
    REQUIRE(fixture.output.CurrentVersion() == 1);
}

TEST_CASE("PhysicalGraphMaterializer validates bindings before JIT and rolls "
          "back compile failures in reverse order",
          "[Sora][Compute][Runtime][PhysicalGraphMaterializer][Rollback]") {
    SECTION("physical binding failure precedes compilation") {
        MockState state;
        auto catalog = MakeCatalog(state);
        auto fixture = MakeDispatchPlan(state, catalog, true);
        Sora::Compute::ArtifactCache cache{0};
        Sora::Compute::CompilerService compilerService{cache};
        Sora::Compute::PipelineService pipelineService{cache};

        const auto graph =
            Sora::Compute::MaterializePhysicalGraph(fixture.plan, catalog, {compilerService, pipelineService});
        REQUIRE_FALSE(graph.has_value());
        REQUIRE(graph.error().code == Sora::Compute::PhysicalGraphMaterializationErrorCode::BindingFailed);
        REQUIRE(graph.error().subject == 2);
        REQUIRE(state.compileCount == 0);
        REQUIRE(state.transferCount == 0);
        REQUIRE(state.submitCount == 0);
        REQUIRE(state.destroyedAllocationCount == 2);
        REQUIRE(state.destroyedAllocations[0] == 2);
        REQUIRE(state.destroyedAllocations[1] == 1);
    }
    SECTION("compiler failure releases every created allocation in reverse order") {
        MockState state;
        state.failCompile = true;
        auto catalog = MakeCatalog(state);
        auto fixture = MakeDispatchPlan(state, catalog);
        Sora::Compute::ArtifactCache cache{0};
        Sora::Compute::CompilerService compilerService{cache};
        Sora::Compute::PipelineService pipelineService{cache};

        const auto graph =
            Sora::Compute::MaterializePhysicalGraph(fixture.plan, catalog, {compilerService, pipelineService});
        REQUIRE_FALSE(graph.has_value());
        REQUIRE(graph.error().code == Sora::Compute::PhysicalGraphMaterializationErrorCode::CompilationFailed);
        REQUIRE(state.compileCount == 1);
        REQUIRE(state.transferCount == 0);
        REQUIRE(state.submitCount == 0);
        REQUIRE(state.destroyedAllocationCount == 2);
        REQUIRE(state.destroyedAllocations[0] == 2);
        REQUIRE(state.destroyedAllocations[1] == 1);
    }
    SECTION("pipeline failure releases the executable and every created "
            "allocation in reverse order") {
        MockState state;
        state.failPipeline = true;
        auto catalog = MakeCatalog(state, Sora::Compute::ArtifactKind::DevicePipeline);
        auto fixture = MakeDispatchPlan(state, catalog, false, false, true);
        Sora::Compute::ArtifactCache cache{0};
        Sora::Compute::CompilerService compilerService{cache};
        Sora::Compute::PipelineService pipelineService{cache};

        const auto graph =
            Sora::Compute::MaterializePhysicalGraph(fixture.plan, catalog, {compilerService, pipelineService});
        REQUIRE_FALSE(graph.has_value());
        REQUIRE(graph.error().code == Sora::Compute::PhysicalGraphMaterializationErrorCode::PipelineResolutionFailed);
        REQUIRE(graph.error().cause == static_cast<uint32_t>(Sora::Compute::PipelineErrorCode::CreationFailed));
        REQUIRE(graph.error().detail == 91);
        REQUIRE(state.compileCount == 1);
        REQUIRE(state.pipelineCreateCount == 1);
        REQUIRE(state.artifactDestroyCount == 1);
        REQUIRE(state.submitCount == 0);
        REQUIRE(state.destroyedAllocationCount == 2);
        REQUIRE(state.destroyedAllocations[0] == 2);
        REQUIRE(state.destroyedAllocations[1] == 1);
    }
}

TEST_CASE("PhysicalGraphMaterializer materializes fill-copy batches and "
          "rejects overlapping physical placement",
          "[Sora][Compute][Runtime][PhysicalGraphMaterializer][Batch]") {
    SECTION("disjoint allocations execute with a device-side dependency") {
        MockState state;
        auto catalog = MakeCatalog(state);
        auto fixture = MakeFillCopyPlan(state, catalog);
        Sora::Compute::ArtifactCache cache{0};
        Sora::Compute::CompilerService compilerService{cache};
        Sora::Compute::PipelineService pipelineService{cache};

        auto graph = Sora::Compute::MaterializePhysicalGraph(fixture.plan, catalog, {compilerService, pipelineService});
        REQUIRE(graph.has_value());
        auto submitted = std::move(*graph).Submit();
        REQUIRE(submitted.has_value());
        REQUIRE(state.submitCount == 2);
        REQUIRE(state.submittedCommands[0] == Sora::Compute::BackendCommandKind::Fill);
        REQUIRE(state.submittedCommands[1] == Sora::Compute::BackendCommandKind::Copy);
        REQUIRE(state.submittedWaitCounts[0] == 0);
        REQUIRE(state.submittedWaitCounts[1] == 1);
        REQUIRE(submitted->Wait(UINT64_MAX).status == Sora::Compute::EventStatus::Completed);
        REQUIRE(fixture.output.CurrentVersion() == 1);
    }
    SECTION("overlapping backend allocations fail before submission") {
        MockState state;
        state.overlapAllocations = true;
        auto catalog = MakeCatalog(state);
        auto fixture = MakeFillCopyPlan(state, catalog);
        Sora::Compute::ArtifactCache cache{0};
        Sora::Compute::CompilerService compilerService{cache};
        Sora::Compute::PipelineService pipelineService{cache};

        const auto graph =
            Sora::Compute::MaterializePhysicalGraph(fixture.plan, catalog, {compilerService, pipelineService});
        REQUIRE_FALSE(graph.has_value());
        REQUIRE(graph.error().code == Sora::Compute::PhysicalGraphMaterializationErrorCode::BatchConstructionFailed);
        REQUIRE(graph.error().segment == 1);
        REQUIRE(state.submitCount == 0);
        REQUIRE(state.destroyedAllocationCount == 2);
        REQUIRE(state.destroyedAllocations[0] == 2);
        REQUIRE(state.destroyedAllocations[1] == 1);
    }
}

TEST_CASE("PhysicalGraphMaterializer reports coherent import failure without "
          "allocation or compilation side effects",
          "[Sora][Compute][Runtime][PhysicalGraphMaterializer][Alias]") {
    MockState state;
    state.failAlias = true;
    auto catalog = MakeCatalog(state);
    auto fixture = MakeDispatchPlan(state, catalog, false, true);
    Sora::Compute::ArtifactCache cache{0};
    Sora::Compute::CompilerService compilerService{cache};
    Sora::Compute::PipelineService pipelineService{cache};

    const auto graph =
        Sora::Compute::MaterializePhysicalGraph(fixture.plan, catalog, {compilerService, pipelineService});
    REQUIRE_FALSE(graph.has_value());
    REQUIRE(graph.error().code == Sora::Compute::PhysicalGraphMaterializationErrorCode::AliasImportFailed);
    REQUIRE(graph.error().subject == 0);
    REQUIRE(state.aliasCount == 1);
    REQUIRE(state.allocateCount == 0);
    REQUIRE(state.compileCount == 0);
    REQUIRE(state.submitCount == 0);
}

TEST_CASE("PhysicalGraphMaterializer rejects an invalidated device generation before "
          "backend side effects",
          "[Sora][Compute][Runtime][PhysicalGraphMaterializer][DeviceGeneration]") {
    MockState state;
    auto catalog = MakeCatalog(state);
    auto fixture = MakeDispatchPlan(state, catalog);
    ++state.deviceGeneration;
    Sora::Compute::ArtifactCache cache{4};
    Sora::Compute::CompilerService compilerService{cache};
    Sora::Compute::PipelineService pipelineService{cache};

    const auto graph =
        Sora::Compute::MaterializePhysicalGraph(fixture.plan, catalog, {compilerService, pipelineService});
    REQUIRE_FALSE(graph.has_value());
    REQUIRE(graph.error().code == Sora::Compute::PhysicalGraphMaterializationErrorCode::DeviceInvalidated);
    REQUIRE(graph.error().cause == static_cast<uint32_t>(Sora::Compute::DeviceObservationError::GenerationChanged));
    REQUIRE(state.allocateCount == 0);
    REQUIRE(state.aliasCount == 0);
    REQUIRE(state.compileCount == 0);
    REQUIRE(state.submitCount == 0);
}

TEST_CASE("ExecutableGraph rejects device loss before accepting its first segment",
          "[Sora][Compute][Runtime][ExecutableGraph][DeviceGeneration]") {
    MockState state;
    auto catalog = MakeCatalog(state);
    auto fixture = MakeDispatchPlan(state, catalog);
    Sora::Compute::ArtifactCache cache{4};
    Sora::Compute::CompilerService compilerService{cache};
    Sora::Compute::PipelineService pipelineService{cache};
    auto graph = Sora::Compute::MaterializePhysicalGraph(fixture.plan, catalog, {compilerService, pipelineService});
    REQUIRE(graph.has_value());
    state.deviceState = Sora::Compute::DeviceState::Lost;
    ++state.deviceGeneration;

    const auto submitted = std::move(*graph).Submit();
    REQUIRE_FALSE(submitted.has_value());
    REQUIRE(submitted.error().node == UINT32_MAX);
    REQUIRE(submitted.error().error.code == Sora::Compute::SubmissionErrorCode::DeviceLost);
    REQUIRE(state.transferCount == 0);
    REQUIRE(state.submitCount == 0);
    REQUIRE(fixture.output.CurrentVersion() == 0);
}

TEST_CASE("GraphSubmission rejects device loss before publishing completed replicas",
          "[Sora][Compute][Runtime][ExecutableGraph][DeviceGeneration]["
          "Publication]") {
    MockState state;
    auto catalog = MakeCatalog(state);
    auto fixture = MakeDispatchPlan(state, catalog);
    Sora::Compute::ArtifactCache cache{4};
    Sora::Compute::CompilerService compilerService{cache};
    Sora::Compute::PipelineService pipelineService{cache};
    auto graph = Sora::Compute::MaterializePhysicalGraph(fixture.plan, catalog, {compilerService, pipelineService});
    REQUIRE(graph.has_value());
    auto submitted = std::move(*graph).Submit();
    REQUIRE(submitted.has_value());
    state.deviceState = Sora::Compute::DeviceState::Lost;
    ++state.deviceGeneration;

    const auto observation = submitted->Wait(UINT64_MAX);
    REQUIRE(observation.status == Sora::Compute::EventStatus::Failed);
    REQUIRE(observation.error.code == Sora::Compute::SubmissionErrorCode::DeviceLost);
    REQUIRE(fixture.output.CurrentVersion() == 0);
    REQUIRE(fixture.output.ReplicaCount() == 0);
}

/**
 * @file CpuBackend.cpp
 * @brief Built-in CPU backend implementation.
 * @ingroup Compute
 */

#include <Sora/Compute/Backend/CpuBackend.h>

#include <Sora/Core/PAL/CpuInfo.h>
#include <Sora/Compute/KernelProgramPackage.h>

#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <mutex>
#include <new>
#include <span>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#    define NOMINMAX
#    include <Windows.h>
#endif

namespace Sora::Compute {

    namespace {

        [[nodiscard]] constexpr TargetIdentityStability CpuTargetIdentityStability(const PAL::CpuInfo& cpu) noexcept {
            if (cpu.architecture == CpuArchitecture::X86 || cpu.architecture == CpuArchitecture::X64) {
                return cpu.x86.vendor.front() != '\0' ? TargetIdentityStability::MachineStable
                                                      : TargetIdentityStability::ProcessStable;
            }
            return TargetIdentityStability::ProcessStable;
        }

        using CpuProgram =
            KernelProgram<kCpuMaximumKernelArguments, kCpuMaximumScalarParameters, kCpuMaximumInstructions>;

        constexpr DeviceId kCpuDevice{0x534F52415F435055ULL, 0x4445564943455F31ULL};
        constexpr DeviceId kHostDevice{0x534F52415F484F53ULL, 0x545F4D454D4F5259ULL};
        constexpr MemoryDomainId kHostDomain{0x534F52415F434F48ULL, 0x4552454E545F3031ULL};
        constexpr QueueId kCpuQueue{0x534F52415F515545ULL, 0x55455F4350555F31ULL};
        constexpr SemanticHash kCpuCompilerIdentity{0x7D527A5B83654C21ULL, 0xC32554EAEAA72219ULL};
        constexpr SemanticHash kCpuDriverIdentity{0xC14CF68B66975131ULL, 0x0A1B8EB4E8C1072DULL};

        constexpr MemoryPropertyMask kHostProperties = MemoryPropertyMask(MemoryProperty::HostVisible) |
                                                       MemoryPropertyMask(MemoryProperty::HostCoherent) |
                                                       MemoryPropertyMask(MemoryProperty::Unified);
        constexpr MemoryPropertyMask kDeviceProperties =
            kHostProperties | MemoryPropertyMask(MemoryProperty::DeviceLocal);

        enum class CpuTransferIndex : size_t {
            HostToDeviceAlias,
            DeviceToHostAlias,
            HostToDeviceCopy,
            DeviceToHostCopy,
            Count,
        };

        struct CpuServiceState;
        struct CpuAllocationHandle;
        struct CpuArtifactHandle;
        struct CpuEventHandle;

        struct CpuEventState {
            std::mutex mutex;
            std::condition_variable completed;
            EventObservation observation{};
        };

        struct CpuEventHandle {
            CpuServiceState* owner = nullptr;
            std::shared_ptr<CpuEventState> state;
        };

        struct CpuStorage {
            CpuStorage(std::byte* value, size_t byteCount, size_t byteAlignment) noexcept
                : data(value), size(byteCount), alignment(byteAlignment) {}

            ~CpuStorage() noexcept { ::operator delete(data, std::align_val_t{alignment}); }

            std::byte* data = nullptr;
            size_t size = 0;
            size_t alignment = 0;
        };

        struct CpuAllocationHandle {
            CpuServiceState* owner = nullptr;
            std::shared_ptr<CpuStorage> storage;
            MemoryLocation location{};
            PhysicalMemoryId memory{};
            MemoryPropertyMask properties{};
            uint64_t storageOffset = 0;
            uint64_t sizeBytes = 0;
            uint64_t alignment = 1;
        };

        struct CpuArtifactHandle {
            CpuServiceState* owner = nullptr;
            ArtifactKey key{};
            CpuProgram program{};
            bool simdEligible = false;
            std::optional<CpuCodeArtifact> nativeCode{};
        };

        struct CpuQueueHandle {
            CpuServiceState* owner = nullptr;
        };

        struct CpuTransferState {
            CpuServiceState* owner = nullptr;
            MemoryLocation source{};
            MemoryLocation destination{};
            TransferKind kind = TransferKind::Copy;
        };

        struct CpuJob {
            explicit CpuJob(std::shared_ptr<CpuEventState> event) noexcept : completion(std::move(event)) {}
            virtual ~CpuJob() = default;
            [[nodiscard]] virtual SubmissionError Execute(CpuServiceState& service) noexcept = 0;

            std::shared_ptr<CpuEventState> completion;
            uint64_t executionNanoseconds = 0;
        };

        struct CpuServiceState {
            CpuServiceState(CpuBackendConfig value, uint32_t workers, CpuCodeProvider provider) noexcept
                : config(value),
                  codeProvider(std::move(provider)),
                  compilerIdentity(codeProvider ? codeProvider.CompilerIdentity() : kCpuCompilerIdentity),
                  driverIdentity(codeProvider ? codeProvider.DriverIdentity() : kCpuDriverIdentity),
                  workerPool(workers),
                  queueThread([this](std::stop_token stop) { RunQueue(stop); }) {}

            ~CpuServiceState() noexcept {
                queueThread.request_stop();
                jobsReady.notify_all();
                queueThread.join();
            }

            CpuServiceState(const CpuServiceState&) = delete;
            CpuServiceState& operator=(const CpuServiceState&) = delete;

            void RunQueue(std::stop_token stop) noexcept {
                for (;;) {
                    std::unique_ptr<CpuJob> job;
                    {
                        std::unique_lock lock{jobsMutex};
                        jobsReady.wait(lock, [&] { return stop.stop_requested() || !jobs.empty(); });
                        if (jobs.empty()) {
                            if (stop.stop_requested()) {
                                return;
                            }
                            continue;
                        }
                        job = std::move(jobs.front());
                        jobs.pop_front();
                        queuedJobs.fetch_sub(1, std::memory_order_relaxed);
                    }
                    const SubmissionError error = job->Execute(*this);
                    const EventStatus status = error.code == SubmissionErrorCode::None         ? EventStatus::Completed
                                               : error.code == SubmissionErrorCode::Stopped    ? EventStatus::Stopped
                                               : error.code == SubmissionErrorCode::DeviceLost ? EventStatus::DeviceLost
                                                                                               : EventStatus::Failed;
                    {
                        std::scoped_lock lock{job->completion->mutex};
                        job->completion->observation = {
                            .status = status, .error = error, .executionNanoseconds = job->executionNanoseconds};
                    }
                    job->completion->completed.notify_all();
                }
            }

            [[nodiscard]] bool Enqueue(std::unique_ptr<CpuJob> job) noexcept {
                try {
                    {
                        std::scoped_lock lock{jobsMutex};
                        jobs.push_back(std::move(job));
                        queuedJobs.fetch_add(1, std::memory_order_relaxed);
                    }
                    jobsReady.notify_one();
                    return true;
                } catch (const std::bad_alloc&) {
                    return false;
                }
            }

            template<typename T>
            [[nodiscard]] bool Contains(const std::unordered_set<T*>& registry, T* value) const noexcept {
                std::scoped_lock lock{registryMutex};
                return registry.contains(value);
            }

            CpuBackendConfig config{};
            CpuCodeProvider codeProvider{};
            SemanticHash compilerIdentity{};
            SemanticHash driverIdentity{};
            exec::static_thread_pool workerPool;
            std::mutex jobsMutex;
            std::condition_variable jobsReady;
            std::deque<std::unique_ptr<CpuJob>> jobs;
            std::atomic<uint64_t> queuedJobs = 0;
            std::jthread queueThread;

            mutable std::mutex registryMutex;
            std::unordered_set<CpuAllocationHandle*> allocations;
            std::unordered_set<CpuArtifactHandle*> artifacts;
            std::unordered_set<CpuEventHandle*> events;

            std::array<CpuTransferState, static_cast<size_t>(CpuTransferIndex::Count)> transfers{};
            BackendServiceId serviceId{};
            TargetFingerprint target{};
            DeviceFeatureMask features{};
            uint64_t architectureId = 0;
            bool simdEnabled = false;
            std::atomic<uint64_t> nextMemoryId = 1;
            std::atomic<uint64_t> submittedBatches = 0;
            std::atomic<uint64_t> parallelDispatches = 0;
            std::atomic<uint64_t> scalarWorkItems = 0;
            std::atomic<uint64_t> simdWorkItems = 0;
            std::atomic<uint64_t> nativeWorkItems = 0;
        };

        [[nodiscard]] uint64_t TotalPhysicalMemory() noexcept {
#if defined(_WIN32)
            MEMORYSTATUSEX status{.dwLength = sizeof(MEMORYSTATUSEX)};
            return GlobalMemoryStatusEx(&status) != 0 ? status.ullTotalPhys : 0;
#else
            return 0;
#endif
        }

        [[nodiscard]] uint64_t AvailablePhysicalMemory() noexcept {
#if defined(_WIN32)
            MEMORYSTATUSEX status{.dwLength = sizeof(MEMORYSTATUSEX)};
            return GlobalMemoryStatusEx(&status) != 0 ? status.ullAvailPhys : 0;
#else
            return 0;
#endif
        }

        [[nodiscard]] DeviceFeatureMask CpuDeviceFeatures(const PAL::CpuInfo& cpu) noexcept {
            DeviceFeatureMask features = DeviceFeatures(DeviceFeature::Float64, DeviceFeature::Int64,
                                                        DeviceFeature::UnifiedMemory, DeviceFeature::AsynchronousCopy);
            if (cpu.Supports(PAL::CpuFeature::Fma)) {
                features |= DeviceFeature::FusedMultiplyAdd;
            }
            return features;
        }

        [[nodiscard]] uint32_t ResolveWorkerCount(uint32_t requested) noexcept {
            if (requested != 0) {
                return requested;
            }
            return std::max(std::thread::hardware_concurrency(), uint32_t{1});
        }

        [[nodiscard]] constexpr uint64_t ResolveMinimumParallelWorkItems(uint32_t workers,
                                                                         uint64_t requested) noexcept {
            if (requested != 0) {
                return requested;
            }
            if (workers <= 1) {
                return std::numeric_limits<uint64_t>::max();
            }
            constexpr uint64_t minimumWorkItemsPerWorker = 32U * 1024U;
            return static_cast<uint64_t>(workers) * minimumWorkItemsPerWorker;
        }

        void DestroyService(void* opaque) noexcept {
            delete static_cast<CpuServiceState*>(opaque);
        }

        void DestroyQueue(void* opaque) noexcept {
            delete static_cast<CpuQueueHandle*>(opaque);
        }

        void DestroyAllocation(void* opaque) noexcept {
            auto* allocation = static_cast<CpuAllocationHandle*>(opaque);
            {
                std::scoped_lock lock{allocation->owner->registryMutex};
                allocation->owner->allocations.erase(allocation);
            }
            delete allocation;
        }

        void DestroyArtifact(void* opaque) noexcept {
            auto* artifact = static_cast<CpuArtifactHandle*>(opaque);
            {
                std::scoped_lock lock{artifact->owner->registryMutex};
                artifact->owner->artifacts.erase(artifact);
            }
            delete artifact;
        }

        void DestroyArtifactBytes(void* opaque) noexcept {
            delete[] static_cast<std::byte*>(opaque);
        }

        void DestroyEvent(void* opaque) noexcept {
            auto* event = static_cast<CpuEventHandle*>(opaque);
            {
                std::scoped_lock lock{event->owner->registryMutex};
                event->owner->events.erase(event);
            }
            delete event;
        }

        [[nodiscard]] std::shared_ptr<CpuEventState> ResolveEvent(CpuServiceState& service, void* opaque) noexcept {
            auto* handle = static_cast<CpuEventHandle*>(opaque);
            std::scoped_lock lock{service.registryMutex};
            if (!service.events.contains(handle)) {
                return {};
            }
            return handle->state;
        }

        void QueryEvent(void* opaque, BackendEventObservation* observation) noexcept {
            auto* handle = static_cast<CpuEventHandle*>(opaque);
            std::scoped_lock lock{handle->state->mutex};
            observation->status = handle->state->observation.status;
            observation->error = handle->state->observation.error;
            observation->executionNanoseconds = handle->state->observation.executionNanoseconds;
        }

        void WaitEvent(void* opaque, uint64_t timeoutNanoseconds, BackendEventObservation* observation) noexcept {
            auto* handle = static_cast<CpuEventHandle*>(opaque);
            std::unique_lock lock{handle->state->mutex};
            const auto terminal = [&] { return handle->state->observation.Terminal(); };
            if (timeoutNanoseconds == UINT64_MAX) {
                handle->state->completed.wait(lock, terminal);
            } else {
                static_cast<void>(
                    handle->state->completed.wait_for(lock, std::chrono::nanoseconds{timeoutNanoseconds}, terminal));
            }
            observation->status = handle->state->observation.status;
            observation->error = handle->state->observation.error;
            observation->executionNanoseconds = handle->state->observation.executionNanoseconds;
        }

        [[nodiscard]] std::expected<BackendEventPayload, SubmissionError> MakeEvent(CpuServiceState& service) noexcept {
            try {
                auto state = std::make_shared<CpuEventState>();
                state->observation = {.status = EventStatus::Pending, .error = {}};
                auto handle = std::make_unique<CpuEventHandle>();
                handle->owner = &service;
                handle->state = std::move(state);
                {
                    std::scoped_lock lock{service.registryMutex};
                    service.events.insert(handle.get());
                }
                return BackendEventPayload{.device = kCpuDevice,
                                           .backend = BackendKind::Cpu,
                                           .state = handle.release(),
                                           .destroy = DestroyEvent,
                                           .query = QueryEvent,
                                           .wait = WaitEvent};
            } catch (const std::bad_alloc&) {
                return std::unexpected(SubmissionError{SubmissionErrorCode::OutOfMemory, 0});
            }
        }

        class EventPayloadOwner final {
        public:
            ~EventPayloadOwner() noexcept { Detail::ReleaseEventPayload(payload); }

            [[nodiscard]] BackendEventPayload Release() noexcept {
                return std::exchange(payload, BackendEventPayload{});
            }

            BackendEventPayload payload{};
        };

        [[nodiscard]] EventObservation WaitForEvent(CpuServiceState& service, EventView view) noexcept {
            std::shared_ptr<CpuEventState> state = ResolveEvent(service, view.state);
            if (!state) {
                return {.status = EventStatus::Failed, .error = {SubmissionErrorCode::InvalidEvent, 0}};
            }
            std::unique_lock lock{state->mutex};
            state->completed.wait(lock, [&] { return state->observation.Terminal(); });
            return state->observation;
        }

        [[nodiscard]] SubmissionError WaitForAll(CpuServiceState& service, std::span<const EventView> waits) noexcept {
            for (const EventView wait : waits) {
                const EventObservation observation = WaitForEvent(service, wait);
                if (observation.status != EventStatus::Completed) {
                    return observation.error.code == SubmissionErrorCode::None
                               ? SubmissionError{SubmissionErrorCode::SubmissionFailed, 0}
                               : observation.error;
                }
            }
            return {};
        }

        [[nodiscard]] CpuAllocationHandle* ResolveAllocation(CpuServiceState& service, AllocationView view) noexcept {
            auto* handle = static_cast<CpuAllocationHandle*>(view.state);
            std::scoped_lock lock{service.registryMutex};
            if (!service.allocations.contains(handle) || handle->owner != &service ||
                view.service != service.serviceId || handle->location != view.location ||
                handle->memory != view.memory || handle->properties != view.properties ||
                handle->storageOffset != view.memoryOffset || handle->sizeBytes != view.sizeBytes ||
                handle->alignment != view.alignment) {
                return nullptr;
            }
            return handle;
        }

        [[nodiscard]] CpuArtifactHandle* ResolveArtifact(CpuServiceState& service, ArtifactView view) noexcept {
            auto* handle = static_cast<CpuArtifactHandle*>(view.state);
            std::scoped_lock lock{service.registryMutex};
            return service.artifacts.contains(handle) && handle->owner == &service ? handle : nullptr;
        }

        [[nodiscard]] std::byte* AllocationAddress(CpuAllocationHandle& allocation, uint64_t offset) noexcept {
            return allocation.storage->data + static_cast<size_t>(allocation.storageOffset + offset);
        }

        void MapHostMemory(void* opaque, const BackendHostMemoryRequest* request,
                           BackendHostMemoryResult* result) noexcept {
            auto& service = *static_cast<CpuServiceState*>(opaque);
            const MemoryLocation hostLocation{
                .domain = kHostDomain, .device = kHostDevice, .backend = BackendKind::Cpu};
            if (!ValidBackendAbi(request->abiVersion, request->structSize, sizeof(BackendHostMemoryRequest)) ||
                request->location != hostLocation || request->allocation.location != hostLocation ||
                request->allocation.service != service.serviceId || request->sizeBytes == 0 ||
                !request->allocation.Covers(request->offset, request->sizeBytes) ||
                !Detail::ValidHostMemoryAccess(request->access)) {
                result->error = {HostMemoryErrorCode::InvalidRequest, 0};
                return;
            }
            CpuAllocationHandle* allocation = ResolveAllocation(service, request->allocation);
            if (allocation == nullptr ||
                !request->allocation.properties.Contains(Detail::kCoherentHostMemoryProperties)) {
                result->error = {HostMemoryErrorCode::ForeignAllocation, 0};
                return;
            }
            result->address = AllocationAddress(*allocation, request->offset);
            result->sizeBytes = request->sizeBytes;
            result->error = {};
        }

        void Allocate(void* opaque, const BackendAllocationRequest* request, BackendAllocationResult* result) noexcept {
            auto& service = *static_cast<CpuServiceState*>(opaque);
            if (request->sizeBytes > std::numeric_limits<size_t>::max() ||
                request->alignment > std::numeric_limits<size_t>::max()) {
                result->error = {MemoryAllocationErrorCode::OutOfMemory, request->sizeBytes};
                return;
            }
            const MemoryPropertyMask available =
                request->location.device == kCpuDevice ? kDeviceProperties : kHostProperties;
            if (!available.Contains(request->requiredProperties)) {
                result->error = {MemoryAllocationErrorCode::UnsupportedProperties, request->requiredProperties.Bits()};
                return;
            }
            const size_t size = static_cast<size_t>(request->sizeBytes);
            const size_t alignment = std::max(static_cast<size_t>(request->alignment), alignof(std::max_align_t));
            std::byte* data = static_cast<std::byte*>(::operator new(size, std::align_val_t{alignment}, std::nothrow));
            if (data == nullptr) {
                result->error = {MemoryAllocationErrorCode::OutOfMemory, request->sizeBytes};
                return;
            }
            std::byte* unownedData = data;
            try {
                auto storage = std::make_shared<CpuStorage>(data, size, alignment);
                unownedData = nullptr;
                auto handle = std::make_unique<CpuAllocationHandle>();
                handle->owner = &service;
                handle->storage = std::move(storage);
                handle->location = request->location;
                handle->memory = {service.nextMemoryId.fetch_add(1, std::memory_order_relaxed), 0x4350555F4D454D31ULL};
                handle->properties = available;
                handle->sizeBytes = request->sizeBytes;
                handle->alignment = alignment;
                {
                    std::scoped_lock lock{service.registryMutex};
                    service.allocations.insert(handle.get());
                }
                result->allocation = {.location = handle->location,
                                      .memory = handle->memory,
                                      .properties = handle->properties,
                                      .memoryOffset = 0,
                                      .sizeBytes = handle->sizeBytes,
                                      .alignment = handle->alignment,
                                      .state = handle.release(),
                                      .destroy = DestroyAllocation};
                result->error = {};
            } catch (const std::bad_alloc&) {
                if (unownedData != nullptr) {
                    ::operator delete(unownedData, std::align_val_t{alignment});
                }
                result->error = {MemoryAllocationErrorCode::OutOfMemory, request->sizeBytes};
            }
        }

        [[nodiscard]] constexpr bool IsFloating(ScalarKind scalar) noexcept {
            return scalar == ScalarKind::Float32 || scalar == ScalarKind::Float64;
        }

        [[nodiscard]] bool ValidateCpuProgram(const CpuProgram& program, bool& simdEligible) noexcept {
            if (program.argumentCount > kCpuMaximumKernelArguments ||
                program.inputCount > kCpuMaximumScalarParameters ||
                program.scalar.instructionCount > kCpuMaximumInstructions) {
                return false;
            }
            for (const KernelArgumentDesc& argument : program.Arguments()) {
                if (argument.elementType.scalar == ScalarKind::Float16 ||
                    argument.elementType.lanes > kCpuMaximumValueLanes) {
                    return false;
                }
            }
            const ValueType resultType = program.scalar.instructions[program.scalar.result.index].resultType;
            simdEligible = IsFloating(resultType.scalar) && resultType.lanes <= kCpuMaximumValueLanes;
            for (const Instruction& instruction : program.scalar.Instructions()) {
                if (instruction.resultType.scalar == ScalarKind::Float16 ||
                    instruction.resultType.lanes > kCpuMaximumValueLanes) {
                    return false;
                }
                if (instruction.opcode != OpCode::Input && instruction.opcode != OpCode::Constant &&
                    !IsFloating(instruction.resultType.scalar)) {
                    return false;
                }
                simdEligible = simdEligible && instruction.resultType == resultType;
            }
            return true;
        }

        [[nodiscard]] bool ValidCpuArtifactRequest(const CpuServiceState& service,
                                                   const BackendCompileRequest& request) noexcept {
            return request.artifactKind == ArtifactKind::DeviceExecutable && request.target == service.target;
        }

        void MaterializeCpuArtifact(CpuServiceState& service, const BackendCompileRequest& request,
                                    std::span<const std::byte> importedBytes, CompileErrorCode invalidBytes,
                                    BackendCompileResult* result) noexcept {
            if (!ValidCpuArtifactRequest(service, request)) {
                result->error = {CompileErrorCode::UnsupportedTarget, request.target.architectureId};
                return;
            }
            std::span<const std::byte> programBytes = request.ModuleBytes();
            if (!service.codeProvider && !importedBytes.empty()) {
                programBytes = importedBytes;
            }
            if (Detail::HashBytes(programBytes) != request.moduleHash) {
                result->error = {invalidBytes, 0};
                return;
            }
            auto decoded = DecodeKernelProgramPackage<kCpuMaximumKernelArguments, kCpuMaximumScalarParameters,
                                                      kCpuMaximumInstructions>(programBytes);
            if (!decoded) {
                result->error = {invalidBytes, static_cast<uint64_t>(decoded.error().code)};
                return;
            }
            bool simdEligible = false;
            if (!ValidateCpuProgram(*decoded, simdEligible)) {
                result->error = {invalidBytes, 0};
                return;
            }
            std::optional<CpuCodeArtifact> nativeCode;
            if (service.codeProvider) {
                const CpuCodeCompileRequest codeRequest{.programData = request.moduleData,
                                                        .programSize = request.moduleSize,
                                                        .programHash = request.moduleHash,
                                                        .target = request.target,
                                                        .specialization = request.specialization};
                auto materialized = importedBytes.empty() ? service.codeProvider.Compile(codeRequest)
                                                          : service.codeProvider.Import(codeRequest, importedBytes);
                if (!materialized) {
                    result->error = materialized.error();
                    return;
                }
                nativeCode.emplace(std::move(*materialized));
            }
            try {
                auto artifact = std::make_unique<CpuArtifactHandle>();
                artifact->owner = &service;
                artifact->key = request.ExpectedKey();
                artifact->program = std::move(*decoded);
                artifact->simdEligible = !nativeCode && service.simdEnabled && simdEligible;
                artifact->nativeCode = std::move(nativeCode);
                {
                    std::scoped_lock lock{service.registryMutex};
                    service.artifacts.insert(artifact.get());
                }
                result->payload = {.key = request.ExpectedKey(),
                                   .device = service.target.device,
                                   .state = artifact.release(),
                                   .destroy = DestroyArtifact};
                result->error = {};
            } catch (const std::bad_alloc&) {
                result->error = {CompileErrorCode::OutOfMemory, 0};
            }
        }

        void Compile(void* opaque, const BackendCompileRequest* request, BackendCompileResult* result) noexcept {
            auto& service = *static_cast<CpuServiceState*>(opaque);
            MaterializeCpuArtifact(service, *request, {}, CompileErrorCode::UnsupportedIr, result);
        }

        void ImportArtifact(void* opaque, const BackendArtifactImportRequest* request,
                            BackendCompileResult* result) noexcept {
            if (!ValidBackendAbi(request->abiVersion, request->structSize, sizeof(BackendArtifactImportRequest)) ||
                request->compileRequest == nullptr || request->artifactData == nullptr || request->artifactSize == 0 ||
                request->artifactSize > std::numeric_limits<size_t>::max()) {
                result->error = {CompileErrorCode::InvalidRequest, 0};
                return;
            }
            auto& service = *static_cast<CpuServiceState*>(opaque);
            MaterializeCpuArtifact(service, *request->compileRequest, request->ArtifactBytes(),
                                   CompileErrorCode::ArtifactImportFailed, result);
        }

        void ExportArtifact(void* opaque, const BackendArtifactExportRequest* request,
                            BackendArtifactExportResult* result) noexcept {
            if (!ValidBackendAbi(request->abiVersion, request->structSize, sizeof(BackendArtifactExportRequest))) {
                result->error = {CompileErrorCode::InvalidRequest, 0};
                return;
            }
            auto& service = *static_cast<CpuServiceState*>(opaque);
            CpuArtifactHandle* artifact = ResolveArtifact(service, request->artifact);
            if (artifact == nullptr || request->key != artifact->key || request->key.Backend() != BackendKind::Cpu ||
                request->artifact.device != kCpuDevice) {
                result->error = {CompileErrorCode::InvalidArtifact, 0};
                return;
            }
            std::vector<std::byte> nativeBytes;
            std::span<const std::byte> bytesToExport;
            decltype(artifact->program.Serialize()) serialized;
            if (artifact->nativeCode) {
                auto exported = service.codeProvider.Export(*artifact->nativeCode);
                if (!exported) {
                    result->error = exported.error();
                    return;
                }
                nativeBytes = std::move(*exported);
                bytesToExport = nativeBytes;
            } else {
                serialized = artifact->program.Serialize();
                bytesToExport = serialized.Bytes();
            }
            auto bytes = std::unique_ptr<std::byte[]>{new (std::nothrow) std::byte[bytesToExport.size()]};
            if (!bytes) {
                result->error = {CompileErrorCode::OutOfMemory, bytesToExport.size()};
                return;
            }
            std::memcpy(bytes.get(), bytesToExport.data(), bytesToExport.size());
            result->payload = {.data = bytes.get(),
                               .size = bytesToExport.size(),
                               .state = bytes.release(),
                               .destroy = DestroyArtifactBytes};
            result->error = {};
        }

        struct CpuBufferRef {
            CpuAllocationHandle* allocation = nullptr;
            BackendBinding binding{};
        };

        struct CpuDispatchContext {
            CpuArtifactHandle* artifact = nullptr;
            std::array<CpuBufferRef, kCpuMaximumKernelArguments> buffers{};
            std::array<const BackendScalarBinding*, kCpuMaximumKernelArguments> scalars{};
            const std::byte* scalarData = nullptr;
            std::array<CpuCodeBuffer, kCpuMaximumKernelArguments> nativeBuffers{};
            std::array<CpuCodeScalar, kCpuMaximumKernelArguments> nativeScalars{};
            uint64_t workItemCount = 0;
            std::array<uint64_t, kMaximumIndexRank> origin{};
            std::array<uint64_t, kMaximumIndexRank> extents{};
        };

        [[nodiscard]] bool PrepareDispatch(CpuServiceState& service, const BackendCommand& command,
                                           std::span<const BackendBinding> bindings,
                                           std::span<const BackendScalarBinding> scalarBindings,
                                           const std::byte* scalarData, CpuDispatchContext& context) noexcept {
            CpuArtifactHandle* artifact = ResolveArtifact(service, command.artifact);
            if (artifact == nullptr || artifact->program.output.index >= artifact->program.argumentCount) {
                return false;
            }
            context.artifact = artifact;
            context.scalarData = scalarData;
            for (const BackendBinding& binding : bindings) {
                if (binding.binding >= artifact->program.argumentCount ||
                    artifact->program.arguments[binding.binding].kind != ParameterKind::Buffer) {
                    return false;
                }
                CpuAllocationHandle* allocation = ResolveAllocation(service, binding.allocation);
                if (allocation == nullptr) {
                    return false;
                }
                context.buffers[binding.binding] = {allocation, binding};
            }
            for (const BackendScalarBinding& binding : scalarBindings) {
                if (binding.binding >= artifact->program.argumentCount ||
                    artifact->program.arguments[binding.binding].kind != ParameterKind::Value) {
                    return false;
                }
                context.scalars[binding.binding] = &binding;
            }
            const CpuBufferRef& output = context.buffers[artifact->program.output.index];
            if (output.allocation == nullptr || output.binding.rank != artifact->program.domain.rank) {
                return false;
            }
            const bool explicitSlice = command.dispatchExtents[0] != 0;
            context.workItemCount = 1;
            for (uint8_t axis = 0; axis < output.binding.rank; ++axis) {
                const uint64_t extent = explicitSlice ? command.dispatchExtents[axis] : output.binding.extents[axis];
                const uint64_t globalExtent = explicitSlice ? command.globalExtents[axis] : extent;
                const uint64_t origin = explicitSlice ? command.dispatchOrigin[axis] : 0;
                if (extent == 0 || context.workItemCount > UINT64_MAX / extent ||
                    output.binding.extents[axis] != extent || globalExtent == 0 || origin > globalExtent ||
                    extent > globalExtent - origin ||
                    (artifact->program.domain.staticExtents[axis] != 0 &&
                     artifact->program.domain.staticExtents[axis] != globalExtent)) {
                    return false;
                }
                context.origin[axis] = origin;
                context.extents[axis] = extent;
                context.workItemCount *= extent;
            }
            for (uint8_t axis = output.binding.rank; axis < kMaximumIndexRank; ++axis) {
                if (explicitSlice && (command.dispatchOrigin[axis] != 0 || command.dispatchExtents[axis] != 0 ||
                                      command.globalExtents[axis] != 0)) {
                    return false;
                }
            }
            for (uint32_t argument = 0; argument < artifact->program.argumentCount; ++argument) {
                const KernelArgumentDesc& expected = artifact->program.arguments[argument];
                if (expected.kind == ParameterKind::Buffer) {
                    const CpuBufferRef& actual = context.buffers[argument];
                    if (actual.allocation == nullptr || actual.binding.elementType != expected.elementType ||
                        actual.binding.rank != expected.rank ||
                        !LayoutSatisfies(actual.binding.layout, expected.layout) ||
                        actual.binding.access != expected.access) {
                        return false;
                    }
                    for (uint8_t axis = 0; axis < expected.rank; ++axis) {
                        if (actual.binding.extents[axis] != context.extents[axis]) {
                            return false;
                        }
                    }
                } else if (context.scalars[argument] == nullptr ||
                           context.scalars[argument]->type != expected.elementType) {
                    return false;
                }
                if (artifact->nativeCode) {
                    if (expected.kind == ParameterKind::Buffer) {
                        const CpuBufferRef& actual = context.buffers[argument];
                        CpuCodeBuffer& native = context.nativeBuffers[argument];
                        native.data = AllocationAddress(*actual.allocation, actual.binding.offset);
                        native.sizeBytes = actual.binding.size;
                        native.access = actual.binding.access;
                        native.elementType = actual.binding.elementType;
                        native.layout = actual.binding.layout;
                        native.rank = actual.binding.rank;
                        std::ranges::copy(actual.binding.extents, native.extents);
                        std::ranges::copy(actual.binding.byteStrides, native.byteStrides);
                        native.componentStrideBytes = actual.binding.componentStrideBytes;
                    } else {
                        const BackendScalarBinding& actual = *context.scalars[argument];
                        context.nativeScalars[argument] = {
                            .data = context.scalarData + actual.dataOffset,
                            .sizeBytes = actual.size,
                            .type = actual.type,
                        };
                    }
                }
            }
            return true;
        }

        [[nodiscard]] uint64_t LogicalAxis(const CpuDispatchContext& context, uint64_t linear,
                                           uint32_t requestedAxis) noexcept {
            for (uint8_t axis = context.artifact->program.domain.rank; axis-- > 0;) {
                const uint64_t coordinate = linear % context.extents[axis];
                if (axis == requestedAxis) {
                    return context.origin[axis] + coordinate;
                }
                linear /= context.extents[axis];
            }
            return 0;
        }

        [[nodiscard]] uint64_t ElementOffset(const BackendBinding& binding, uint64_t linear,
                                             uint8_t component) noexcept {
            const uint64_t scalarBytes = ScalarByteSize(binding.elementType.scalar);
            if (binding.layout == LayoutKind::Contiguous) {
                return linear * binding.elementType.ByteSize() + component * scalarBytes;
            }
            if (binding.layout == LayoutKind::StructureOfArrays) {
                return linear * scalarBytes + component * binding.componentStrideBytes;
            }
            uint64_t result = component * binding.componentStrideBytes;
            for (uint8_t axis = binding.rank; axis-- > 0;) {
                const uint64_t coordinate = linear % binding.extents[axis];
                linear /= binding.extents[axis];
                result += coordinate * binding.byteStrides[axis];
            }
            return result;
        }

        [[nodiscard]] std::byte* ElementAddress(const CpuBufferRef& buffer, uint64_t linear,
                                                uint8_t component) noexcept {
            return AllocationAddress(*buffer.allocation,
                                     buffer.binding.offset + ElementOffset(buffer.binding, linear, component));
        }

        struct CpuValue {
            std::array<std::byte, kCpuMaximumValueLanes * sizeof(uint64_t)> bytes;
        };

        template<typename T>
        [[nodiscard]] T LoadValue(const CpuValue& value, uint8_t component) noexcept {
            T result;
            std::memcpy(&result, value.bytes.data() + component * sizeof(T), sizeof(T));
            return result;
        }

        template<typename T>
        void StoreValue(CpuValue& value, uint8_t component, T scalar) noexcept {
            std::memcpy(value.bytes.data() + component * sizeof(T), &scalar, sizeof(T));
        }

        template<typename T>
        [[nodiscard]] T DecodeImmediate(uint64_t immediate) noexcept {
            if constexpr (std::same_as<T, float>) {
                return std::bit_cast<float>(static_cast<uint32_t>(immediate));
            } else {
                return std::bit_cast<double>(immediate);
            }
        }

        template<typename T>
        [[nodiscard]] T EvaluateFloating(OpCode opcode, T first, T second, T third) noexcept {
            switch (opcode) {
                case OpCode::Add:
                    return first + second;
                case OpCode::Sub:
                    return first - second;
                case OpCode::Mul:
                    return first * second;
                case OpCode::Div:
                    return first / second;
                case OpCode::Neg:
                    return -first;
                case OpCode::Inv:
                    return T{1} / first;
                case OpCode::Square:
                    return first * first;
                case OpCode::Abs:
                    return std::abs(first);
                case OpCode::Sin:
                    return std::sin(first);
                case OpCode::Cos:
                    return std::cos(first);
                case OpCode::Tan:
                    return std::tan(first);
                case OpCode::Asin:
                    return std::asin(first);
                case OpCode::Acos:
                    return std::acos(first);
                case OpCode::Atan:
                    return std::atan(first);
                case OpCode::Atan2:
                    return std::atan2(first, second);
                case OpCode::Exp:
                    return std::exp(first);
                case OpCode::Log:
                    return std::log(first);
                case OpCode::Sqrt:
                    return std::sqrt(first);
                case OpCode::Pow:
                    return std::pow(first, second);
                case OpCode::Fma:
                    return std::fma(first, second, third);
                case OpCode::Mfs:
                    return std::fma(first, second, -third);
                case OpCode::Nms:
                    return std::fma(-first, second, -third);
                case OpCode::Nma:
                    return std::fma(-first, second, third);
                case OpCode::Lerp:
                    return std::lerp(first, second, third);
                case OpCode::Clamp:
                    return std::clamp(first, second, third);
                case OpCode::Saturate:
                    return std::clamp(first, T{0}, T{1});
                case OpCode::Sign:
                    return static_cast<T>((T{0} < first) - (first < T{0}));
                case OpCode::Input:
                case OpCode::Constant:
                    return {};
            }
            return {};
        }

        void LoadScalarInput(const CpuDispatchContext& context, uint32_t parameter, uint64_t linear,
                             CpuValue& value) noexcept {
            const CpuProgram& program = context.artifact->program;
            const ScalarInputBinding source = program.inputs[parameter];
            const ValueType type = program.scalar.parameters[parameter].elementType;
            if (source.kind == ScalarInputKind::LogicalIndex) {
                const uint64_t coordinate = LogicalAxis(context, linear, source.sourceIndex);
                StoreValue(value, 0, coordinate);
                return;
            }
            const KernelArgumentDesc& argument = program.arguments[source.sourceIndex];
            if (argument.kind == ParameterKind::Value) {
                const BackendScalarBinding& scalar = *context.scalars[source.sourceIndex];
                std::memcpy(value.bytes.data(), context.scalarData + scalar.dataOffset, scalar.size);
                return;
            }
            const CpuBufferRef& buffer = context.buffers[source.sourceIndex];
            const size_t scalarBytes = ScalarByteSize(type.scalar);
            for (uint8_t component = 0; component < type.lanes; ++component) {
                std::memcpy(value.bytes.data() + component * scalarBytes, ElementAddress(buffer, linear, component),
                            scalarBytes);
            }
        }

        void ExecuteScalarItem(const CpuDispatchContext& context, uint64_t linear) noexcept {
            const CpuProgram& program = context.artifact->program;
            std::array<CpuValue, kCpuMaximumInstructions> values;
            for (uint32_t index = 0; index < program.scalar.instructionCount; ++index) {
                const Instruction& instruction = program.scalar.instructions[index];
                CpuValue& output = values[index];
                if (instruction.opcode == OpCode::Input) {
                    LoadScalarInput(context, static_cast<uint32_t>(instruction.immediate), linear, output);
                    continue;
                }
                const uint8_t scalarBytes = ScalarByteSize(instruction.resultType.scalar);
                if (instruction.opcode == OpCode::Constant) {
                    for (uint8_t component = 0; component < instruction.resultType.lanes; ++component) {
                        std::memcpy(output.bytes.data() + component * scalarBytes, &instruction.immediate, scalarBytes);
                    }
                    continue;
                }
                const CpuValue& firstValue = values[instruction.operands[0].index];
                const CpuValue* secondValue =
                    instruction.operandCount > 1 ? &values[instruction.operands[1].index] : nullptr;
                const CpuValue* thirdValue =
                    instruction.operandCount > 2 ? &values[instruction.operands[2].index] : nullptr;
                for (uint8_t component = 0; component < instruction.resultType.lanes; ++component) {
                    if (instruction.resultType.scalar == ScalarKind::Float32) {
                        const float first = LoadValue<float>(firstValue, component);
                        const float second = secondValue != nullptr ? LoadValue<float>(*secondValue, component) : 0.0F;
                        const float third = thirdValue != nullptr ? LoadValue<float>(*thirdValue, component) : 0.0F;
                        StoreValue(output, component, EvaluateFloating(instruction.opcode, first, second, third));
                    } else {
                        const double first = LoadValue<double>(firstValue, component);
                        const double second = secondValue != nullptr ? LoadValue<double>(*secondValue, component) : 0.0;
                        const double third = thirdValue != nullptr ? LoadValue<double>(*thirdValue, component) : 0.0;
                        StoreValue(output, component, EvaluateFloating(instruction.opcode, first, second, third));
                    }
                }
            }
            const Instruction& resultInstruction = program.scalar.instructions[program.scalar.result.index];
            const CpuValue& result = values[program.scalar.result.index];
            const CpuBufferRef& output = context.buffers[program.output.index];
            const size_t scalarBytes = ScalarByteSize(resultInstruction.resultType.scalar);
            for (uint8_t component = 0; component < resultInstruction.resultType.lanes; ++component) {
                std::memcpy(ElementAddress(output, linear, component), result.bytes.data() + component * scalarBytes,
                            scalarBytes);
            }
        }

        using FloatVector = float __attribute__((ext_vector_type(4)));
        using DoubleVector = double __attribute__((ext_vector_type(2)));

        template<typename T>
        struct NativeVector;

        template<>
        struct NativeVector<float> {
            using Type = FloatVector;
            static constexpr uint64_t width = 4;
        };

        template<>
        struct NativeVector<double> {
            using Type = DoubleVector;
            static constexpr uint64_t width = 2;
        };

        template<typename T>
        [[nodiscard]] typename NativeVector<T>::Type Splat(T value) noexcept {
            typename NativeVector<T>::Type result{};
            for (uint64_t lane = 0; lane < NativeVector<T>::width; ++lane) {
                result[lane] = value;
            }
            return result;
        }

        template<typename T>
        [[nodiscard]] typename NativeVector<T>::Type EvaluateVector(OpCode opcode, typename NativeVector<T>::Type first,
                                                                    typename NativeVector<T>::Type second,
                                                                    typename NativeVector<T>::Type third) noexcept {
            using V = typename NativeVector<T>::Type;
            switch (opcode) {
                case OpCode::Add:
                    return first + second;
                case OpCode::Sub:
                    return first - second;
                case OpCode::Mul:
                    return first * second;
                case OpCode::Div:
                    return first / second;
                case OpCode::Neg:
                    return -first;
                case OpCode::Inv:
                    return Splat<T>(T{1}) / first;
                case OpCode::Square:
                    return first * first;
                default: {
                    V result{};
                    for (uint64_t lane = 0; lane < NativeVector<T>::width; ++lane) {
                        result[lane] = EvaluateFloating(opcode, first[lane], second[lane], third[lane]);
                    }
                    return result;
                }
            }
        }

        template<typename T>
        void ExecuteVectorGroup(const CpuDispatchContext& context, uint64_t linear) noexcept {
            using V = typename NativeVector<T>::Type;
            constexpr uint64_t width = NativeVector<T>::width;
            const CpuProgram& program = context.artifact->program;
            std::array<std::array<V, kCpuMaximumValueLanes>, kCpuMaximumInstructions> values;
            for (uint32_t index = 0; index < program.scalar.instructionCount; ++index) {
                const Instruction& instruction = program.scalar.instructions[index];
                if (instruction.opcode == OpCode::Input) {
                    const uint32_t parameter = static_cast<uint32_t>(instruction.immediate);
                    const ScalarInputBinding source = program.inputs[parameter];
                    const KernelArgumentDesc& argument = program.arguments[source.sourceIndex];
                    for (uint8_t component = 0; component < instruction.resultType.lanes; ++component) {
                        V input{};
                        if (argument.kind == ParameterKind::Value) {
                            const BackendScalarBinding& scalar = *context.scalars[source.sourceIndex];
                            T value;
                            std::memcpy(&value, context.scalarData + scalar.dataOffset + component * sizeof(T),
                                        sizeof(T));
                            input = Splat<T>(value);
                        } else {
                            const CpuBufferRef& buffer = context.buffers[source.sourceIndex];
                            for (uint64_t lane = 0; lane < width; ++lane) {
                                T value;
                                std::memcpy(&value, ElementAddress(buffer, linear + lane, component), sizeof(T));
                                input[lane] = value;
                            }
                        }
                        values[index][component] = input;
                    }
                    continue;
                }
                if (instruction.opcode == OpCode::Constant) {
                    const T value = DecodeImmediate<T>(instruction.immediate);
                    for (uint8_t component = 0; component < instruction.resultType.lanes; ++component) {
                        values[index][component] = Splat<T>(value);
                    }
                    continue;
                }
                for (uint8_t component = 0; component < instruction.resultType.lanes; ++component) {
                    const V first = values[instruction.operands[0].index][component];
                    const V second =
                        instruction.operandCount > 1 ? values[instruction.operands[1].index][component] : V{};
                    const V third =
                        instruction.operandCount > 2 ? values[instruction.operands[2].index][component] : V{};
                    values[index][component] = EvaluateVector<T>(instruction.opcode, first, second, third);
                }
            }
            const Instruction& resultInstruction = program.scalar.instructions[program.scalar.result.index];
            const CpuBufferRef& output = context.buffers[program.output.index];
            for (uint8_t component = 0; component < resultInstruction.resultType.lanes; ++component) {
                const V result = values[program.scalar.result.index][component];
                for (uint64_t lane = 0; lane < width; ++lane) {
                    const T value = result[lane];
                    std::memcpy(ElementAddress(output, linear + lane, component), &value, sizeof(T));
                }
            }
        }

        void ExecuteRange(CpuServiceState& service, const CpuDispatchContext& context, uint64_t begin,
                          uint64_t end) noexcept {
            if (context.artifact->nativeCode) {
                CpuCodeInvocation invocation{.buffers = context.nativeBuffers.data(),
                                             .scalars = context.nativeScalars.data(),
                                             .argumentCount = context.artifact->program.argumentCount,
                                             .rank = context.artifact->program.domain.rank,
                                             .begin = begin,
                                             .end = end};
                std::ranges::copy(context.origin, invocation.origin);
                std::ranges::copy(context.extents, invocation.extents);
                context.artifact->nativeCode->Execute(invocation);
                service.nativeWorkItems.fetch_add(end - begin, std::memory_order_relaxed);
                return;
            }
            uint64_t vectorEnd = begin;
            if (context.artifact->simdEligible && end - begin >= service.config.minimumSimdWorkItems) {
                const ScalarKind scalar =
                    context.artifact->program.scalar.instructions[context.artifact->program.scalar.result.index]
                        .resultType.scalar;
                const uint64_t width =
                    scalar == ScalarKind::Float32 ? NativeVector<float>::width : NativeVector<double>::width;
                vectorEnd = begin + ((end - begin) / width) * width;
                if (scalar == ScalarKind::Float32) {
                    for (uint64_t index = begin; index < vectorEnd; index += width) {
                        ExecuteVectorGroup<float>(context, index);
                    }
                } else {
                    for (uint64_t index = begin; index < vectorEnd; index += width) {
                        ExecuteVectorGroup<double>(context, index);
                    }
                }
                service.simdWorkItems.fetch_add(vectorEnd - begin, std::memory_order_relaxed);
            }
            for (uint64_t index = vectorEnd; index < end; ++index) {
                ExecuteScalarItem(context, index);
            }
            service.scalarWorkItems.fetch_add(end - vectorEnd, std::memory_order_relaxed);
        }

        [[nodiscard]] SubmissionError ExecuteDispatch(CpuServiceState& service,
                                                      const CpuDispatchContext& context) noexcept {
            if (service.config.workerCount == 1 || context.workItemCount < service.config.minimumParallelWorkItems) {
                ExecuteRange(service, context, 0, context.workItemCount);
                return {};
            }
            try {
                auto sender = stdexec::schedule(service.workerPool.get_scheduler()) |
                              stdexec::bulk_chunked(stdexec::par, context.workItemCount,
                                                    [&service, &context](uint64_t begin, uint64_t end) noexcept {
                                                        ExecuteRange(service, context, begin, end);
                                                    });
                const auto completed = stdexec::sync_wait(std::move(sender));
                if (!completed) {
                    return {SubmissionErrorCode::Stopped, 0};
                }
                service.parallelDispatches.fetch_add(1, std::memory_order_relaxed);
                return {};
            } catch (const std::bad_alloc&) {
                return {SubmissionErrorCode::OutOfMemory, 0};
            } catch (...) {
                return {SubmissionErrorCode::SubmissionFailed, 0};
            }
        }

        class CpuBatchJob final : public CpuJob {
        public:
            CpuBatchJob(const BackendSubmitRequest& request, std::shared_ptr<CpuEventState> event)
                : CpuJob(std::move(event)) {
                if (request.commandCount != 0) {
                    commands_.assign(request.Commands().begin(), request.Commands().end());
                }
                if (request.bindingCount != 0) {
                    bindings_.assign(request.Bindings().begin(), request.Bindings().end());
                }
                if (request.scalarBindingCount != 0) {
                    scalarBindings_.assign(request.ScalarBindings().begin(), request.ScalarBindings().end());
                }
                if (request.waitCount != 0) {
                    waits_.assign(request.Waits().begin(), request.Waits().end());
                }
                if (request.scalarDataSize != 0) {
                    scalarData_.assign(request.scalarData, request.scalarData + request.scalarDataSize);
                }
            }

            [[nodiscard]] bool Validate(CpuServiceState& service) noexcept {
                for (const BackendCommand& command : commands_) {
                    if (command.kind == BackendCommandKind::Dispatch) {
                        CpuDispatchContext context;
                        if (!PrepareDispatch(service, command,
                                             std::span{bindings_}.subspan(command.firstBinding, command.bindingCount),
                                             std::span{scalarBindings_}.subspan(command.firstScalarBinding,
                                                                                command.scalarBindingCount),
                                             scalarData_.data(), context)) {
                            return false;
                        }
                    } else {
                        if ((command.source && ResolveAllocation(service, command.source) == nullptr) ||
                            (command.target && ResolveAllocation(service, command.target) == nullptr)) {
                            return false;
                        }
                    }
                }
                for (const EventView wait : waits_) {
                    if (!ResolveEvent(service, wait.state)) {
                        return false;
                    }
                }
                return true;
            }

            [[nodiscard]] SubmissionError Execute(CpuServiceState& service) noexcept override {
                if (const SubmissionError waited = WaitForAll(service, waits_);
                    waited.code != SubmissionErrorCode::None) {
                    return waited;
                }
                if (commands_.empty()) {
                    return {};
                }
                const auto start = std::chrono::steady_clock::now();
                const SubmissionError result = ExecuteCommands(service);
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
                executionNanoseconds = elapsed.count() <= 0 ? 1 : static_cast<uint64_t>(elapsed.count());
                return result;
            }

        private:
            [[nodiscard]] SubmissionError ExecuteCommands(CpuServiceState& service) noexcept {
                for (const BackendCommand& command : commands_) {
                    if (command.kind == BackendCommandKind::Dispatch) {
                        CpuDispatchContext context;
                        if (!PrepareDispatch(service, command,
                                             std::span{bindings_}.subspan(command.firstBinding, command.bindingCount),
                                             std::span{scalarBindings_}.subspan(command.firstScalarBinding,
                                                                                command.scalarBindingCount),
                                             scalarData_.data(), context)) {
                            return {SubmissionErrorCode::InvalidRequest, 0};
                        }
                        if (const SubmissionError error = ExecuteDispatch(service, context);
                            error.code != SubmissionErrorCode::None) {
                            return error;
                        }
                    } else if (command.kind == BackendCommandKind::Copy) {
                        CpuAllocationHandle* source = ResolveAllocation(service, command.source);
                        CpuAllocationHandle* target = ResolveAllocation(service, command.target);
                        std::memcpy(AllocationAddress(*target, command.targetOffset),
                                    AllocationAddress(*source, command.sourceOffset),
                                    static_cast<size_t>(command.sizeBytes));
                    } else {
                        CpuAllocationHandle* target = ResolveAllocation(service, command.target);
                        std::byte* destination = AllocationAddress(*target, command.targetOffset);
                        for (uint64_t offset = 0; offset < command.sizeBytes; offset += command.fillPatternSize) {
                            std::memcpy(destination + offset, command.fillPattern, command.fillPatternSize);
                        }
                    }
                }
                return {};
            }

            std::vector<BackendCommand> commands_;
            std::vector<BackendBinding> bindings_;
            std::vector<std::byte> scalarData_;
            std::vector<BackendScalarBinding> scalarBindings_;
            std::vector<EventView> waits_;
        };

        class CpuTransferJob final : public CpuJob {
        public:
            CpuTransferJob(CpuAllocationHandle* source, CpuAllocationHandle* target,
                           std::span<const TransferRegion> regions, std::vector<EventView> waits,
                           std::shared_ptr<CpuEventState> event)
                : CpuJob(std::move(event)),
                  source_(source),
                  target_(target),
                  regions_(regions.size() == 1 ? RegionStorage{regions.front()}
                                               : RegionStorage{std::in_place_type<std::vector<TransferRegion>>,
                                                               regions.begin(), regions.end()}),
                  waits_(std::move(waits)) {}

            [[nodiscard]] SubmissionError Execute(CpuServiceState& service) noexcept override {
                if (const SubmissionError waited = WaitForAll(service, waits_);
                    waited.code != SubmissionErrorCode::None) {
                    return waited;
                }
                std::visit(
                    [&](const auto& regions) {
                        using Regions = std::remove_cvref_t<decltype(regions)>;
                        if constexpr (std::same_as<Regions, TransferRegion>) {
                            Copy(regions);
                        } else {
                            for (const TransferRegion region : regions) {
                                Copy(region);
                            }
                        }
                    },
                    regions_);
                return {};
            }

        private:
            using RegionStorage = std::variant<TransferRegion, std::vector<TransferRegion>>;

            void Copy(TransferRegion region) const noexcept {
                std::memcpy(AllocationAddress(*target_, region.targetOffset),
                            AllocationAddress(*source_, region.sourceOffset), static_cast<size_t>(region.sizeBytes));
            }

            CpuAllocationHandle* source_ = nullptr;
            CpuAllocationHandle* target_ = nullptr;
            RegionStorage regions_;
            std::vector<EventView> waits_;
        };

        void Submit(void* opaque, const BackendSubmitRequest* request, BackendSubmitResult* result) noexcept {
            auto& service = *static_cast<CpuQueueHandle*>(opaque)->owner;
            EventPayloadOwner event;
            try {
                auto created = MakeEvent(service);
                if (!created) {
                    result->error = created.error();
                    return;
                }
                event.payload = *created;
                auto* eventHandle = static_cast<CpuEventHandle*>(event.payload.state);
                auto job = std::make_unique<CpuBatchJob>(*request, eventHandle->state);
                if (!job->Validate(service)) {
                    result->error = {SubmissionErrorCode::InvalidRequest, 0};
                    return;
                }
                if (!service.Enqueue(std::move(job))) {
                    result->error = {SubmissionErrorCode::OutOfMemory, 0};
                    return;
                }
                service.submittedBatches.fetch_add(1, std::memory_order_relaxed);
                result->event = event.Release();
                result->error = {};
            } catch (const std::bad_alloc&) {
                result->error = {SubmissionErrorCode::OutOfMemory, 0};
            }
        }

        [[nodiscard]] AllocationPayload MakeAlias(CpuServiceState& service, const BackendTransferRequest& request,
                                                  CpuAllocationHandle& source) {
            const TransferRegion region = request.Regions().front();
            auto handle = std::make_unique<CpuAllocationHandle>();
            handle->owner = &service;
            handle->storage = source.storage;
            handle->location = request.destination;
            handle->memory = request.source.memory;
            handle->properties = request.source.properties;
            handle->storageOffset = source.storageOffset + region.sourceOffset;
            handle->sizeBytes = region.sizeBytes;
            handle->alignment = region.sourceOffset == 0 ? request.source.alignment
                                                         : std::gcd(request.source.alignment, region.sourceOffset);
            {
                std::scoped_lock lock{service.registryMutex};
                service.allocations.insert(handle.get());
            }
            return {.location = handle->location,
                    .memory = handle->memory,
                    .properties = handle->properties,
                    .memoryOffset = handle->storageOffset,
                    .sizeBytes = handle->sizeBytes,
                    .alignment = handle->alignment,
                    .state = handle.release(),
                    .destroy = DestroyAllocation};
        }

        void Transfer(void* opaque, const BackendTransferRequest* request, BackendTransferResult* result) noexcept {
            auto& transfer = *static_cast<CpuTransferState*>(opaque);
            CpuServiceState& service = *transfer.owner;
            CpuAllocationHandle* source = ResolveAllocation(service, request->source);
            if (source == nullptr) {
                result->error = {TransferErrorCode::InvalidRequest, 0};
                return;
            }
            try {
                if (request->kind == TransferKind::CoherentAlias) {
                    result->imported = MakeAlias(service, *request, *source);
                    result->error = {};
                    return;
                }
                CpuAllocationHandle* target = ResolveAllocation(service, request->target);
                if (target == nullptr) {
                    result->error = {TransferErrorCode::InvalidRequest, 0};
                    return;
                }
                for (const EventView wait : request->Waits()) {
                    if (!ResolveEvent(service, wait.state)) {
                        result->error = {TransferErrorCode::InvalidRequest, 0};
                        return;
                    }
                }
                EventPayloadOwner event;
                auto created = MakeEvent(service);
                if (!created) {
                    result->error = {TransferErrorCode::OutOfMemory, 0};
                    return;
                }
                event.payload = *created;
                auto* eventHandle = static_cast<CpuEventHandle*>(event.payload.state);
                std::vector<EventView> waits{request->Waits().begin(), request->Waits().end()};
                auto job = std::make_unique<CpuTransferJob>(source, target, request->Regions(), std::move(waits),
                                                            eventHandle->state);
                if (!service.Enqueue(std::move(job))) {
                    result->error = {TransferErrorCode::OutOfMemory, 0};
                    return;
                }
                result->completion = event.Release();
                result->error = {};
            } catch (const std::bad_alloc&) {
                result->error = {TransferErrorCode::OutOfMemory, 0};
            }
        }

        [[nodiscard]] TransferEngineId TransferId(CpuTransferIndex index) noexcept {
            return {0x534F52415F54524EULL, 0x4350550000000000ULL | static_cast<uint64_t>(index) + 1U};
        }

        struct StaticCpuCode {};

        void ObserveCpuDevice(void* opaque, DeviceSnapshot* snapshot) noexcept {
            const auto& state = *static_cast<const CpuServiceState*>(opaque);
            *snapshot = {.id = kCpuDevice,
                         .state = DeviceState::Available,
                         .generation = 1,
                         .observationSequence = state.submittedBatches.load(std::memory_order_relaxed),
                         .availableMemoryBytes = AvailablePhysicalMemory(),
                         .estimatedQueueDelayNanoseconds = 0,
                         .loadPermille = 0};
        }

    } // namespace

    std::expected<CpuBackend, CpuBackendError> CpuBackend::Create(CpuBackendConfig config,
                                                                  CpuCodeProvider provider) noexcept {
        const uint32_t workers = ResolveWorkerCount(config.workerCount);
        if (workers == 0 || config.minimumSimdWorkItems == 0) {
            return std::unexpected(CpuBackendError{CpuBackendErrorCode::InvalidConfiguration, workers});
        }
        config.workerCount = workers;
        config.minimumParallelWorkItems = ResolveMinimumParallelWorkItems(workers, config.minimumParallelWorkItems);
        try {
            auto state = std::make_unique<CpuServiceState>(config, workers, std::move(provider));
            static std::atomic<uint64_t> nextServiceId = 1;
            const BackendServiceId serviceId{nextServiceId.fetch_add(1, std::memory_order_relaxed),
                                             0x534F52415F435055ULL};
            auto service = BackendService::Adopt({.id = serviceId, .state = state.get(), .destroy = DestroyService},
                                                 BackendCodeLease::Retain(std::make_shared<StaticCpuCode>()));
            if (!service) {
                return std::unexpected(CpuBackendError{CpuBackendErrorCode::ResourceConstructionFailed,
                                                       static_cast<uint64_t>(service.error())});
            }
            static_cast<void>(state.release());
            auto& native = *static_cast<CpuServiceState*>(service->State());
            native.serviceId = serviceId;
            const PAL::CpuInfo cpu = PAL::DetectHostCpuInfo();
            const DeviceFeatureMask features = CpuDeviceFeatures(cpu);
            const uint64_t architectureId = cpu.ArchitectureId();
            native.features = features;
            native.architectureId = architectureId;
            native.simdEnabled = config.enableSimd && cpu.SupportsFixedVectorBits(128);
            const uint64_t totalMemory = TotalPhysicalMemory();
            const DeviceDescriptor device{.id = kCpuDevice,
                                          .backend = BackendKind::Cpu,
                                          .kind = DeviceKind::Cpu,
                                          .features = features,
                                          .architectureId = architectureId,
                                          .totalMemoryBytes = totalMemory,
                                          .maximumAllocationBytes = totalMemory,
                                          .computeUnitCount = workers,
                                          .subgroupWidthMinimum = 1,
                                          .subgroupWidthMaximum = native.simdEnabled ? 4U : 1U,
                                          .maximumWorkgroupInvocations = UINT32_MAX};
            const TargetFingerprint target{.device = kCpuDevice,
                                           .backend = BackendKind::Cpu,
                                           .hostArchitecture = cpu.architecture,
                                           .operatingSystem = kOperatingSystem,
                                           .binaryFormat = kBinaryFormat,
                                           .byteOrder = cpu.byteOrder,
                                           .abi = kAbiFamily,
                                           .identityStability = CpuTargetIdentityStability(cpu),
                                           .executableDomain = ExecutableTargetDomain::TargetNative,
                                           .addressBits = cpu.addressBits,
                                           .features = features,
                                           .architectureId = architectureId,
                                           .fixedVectorBits = cpu.MaximumFixedVectorBits(),
                                           .scalableVectorBits = cpu.currentScalableVectorBits,
                                           .compilerIdentity = native.compilerIdentity,
                                           .driverIdentity = native.driverIdentity};
            native.target = target;
            const MemoryLocation deviceLocation{
                .domain = kHostDomain, .device = kCpuDevice, .backend = BackendKind::Cpu};
            const MemoryLocation hostLocation{
                .domain = kHostDomain, .device = kHostDevice, .backend = BackendKind::Cpu};
            auto deviceAllocator = MemoryAllocator::Create(*service, {.location = deviceLocation,
                                                                      .supportedProperties = kDeviceProperties,
                                                                      .state = &native,
                                                                      .allocate = Allocate});
            auto hostAllocator = MemoryAllocator::Create(*service, {.location = hostLocation,
                                                                    .supportedProperties = kHostProperties,
                                                                    .state = &native,
                                                                    .allocate = Allocate});
            auto hostAccessor = HostMemoryAccessor::Create(
                *service, {.location = hostLocation, .state = &native, .map = MapHostMemory});
            auto compiler = BackendCompiler::Create(*service, {.identity = native.compilerIdentity,
                                                               .backend = BackendKind::Cpu,
                                                               .executableDomain = ExecutableTargetDomain::TargetNative,
                                                               .state = &native,
                                                               .compile = Compile,
                                                               .importArtifact = ImportArtifact,
                                                               .exportArtifact = ExportArtifact});
            auto observer = DeviceObserver::Create(
                *service,
                {.device = kCpuDevice, .backend = BackendKind::Cpu, .state = &native, .observe = ObserveCpuDevice});
            auto queueNative = std::make_unique<CpuQueueHandle>(CpuQueueHandle{&native});
            auto queue = Queue::Adopt(
                *service, {.id = kCpuQueue,
                           .device = kCpuDevice,
                           .backend = BackendKind::Cpu,
                           .capabilities = QueueCapability::Dispatch | QueueCapability::Copy | QueueCapability::Fill,
                           .state = queueNative.get(),
                           .destroy = DestroyQueue,
                           .submit = Submit,
                           .reportsExecutionNanoseconds = true});
            if (!deviceAllocator || !hostAllocator || !hostAccessor || !compiler || !observer || !queue) {
                return std::unexpected(CpuBackendError{CpuBackendErrorCode::ResourceConstructionFailed, 0});
            }
            static_cast<void>(queueNative.release());

            const std::array transferDescriptions{
                std::tuple{CpuTransferIndex::HostToDeviceAlias, hostLocation, deviceLocation,
                           TransferKind::CoherentAlias},
                std::tuple{CpuTransferIndex::DeviceToHostAlias, deviceLocation, hostLocation,
                           TransferKind::CoherentAlias},
                std::tuple{CpuTransferIndex::HostToDeviceCopy, hostLocation, deviceLocation, TransferKind::Copy},
                std::tuple{CpuTransferIndex::DeviceToHostCopy, deviceLocation, hostLocation, TransferKind::Copy},
            };
            std::vector<TransferEngine> transfers;
            transfers.reserve(transferDescriptions.size());
            for (const auto& [index, source, destination, kind] : transferDescriptions) {
                CpuTransferState& transferState = native.transfers[static_cast<size_t>(index)];
                transferState = {.owner = &native, .source = source, .destination = destination, .kind = kind};
                auto engine = TransferEngine::Create(*service, {.engine = TransferId(index),
                                                                .sourceService = service->Id(),
                                                                .source = source,
                                                                .destination = destination,
                                                                .kind = kind,
                                                                .completionDevice = kCpuDevice,
                                                                .completionBackend = BackendKind::Cpu,
                                                                .state = &transferState,
                                                                .transfer = Transfer});
                if (!engine) {
                    return std::unexpected(CpuBackendError{CpuBackendErrorCode::ResourceConstructionFailed,
                                                           static_cast<uint64_t>(engine.error().code)});
                }
                transfers.push_back(std::move(*engine));
            }
            return CpuBackend{std::move(*service),
                              std::move(*observer),
                              device,
                              target,
                              deviceLocation,
                              hostLocation,
                              std::move(*deviceAllocator),
                              std::move(*hostAllocator),
                              std::move(*hostAccessor),
                              std::move(transfers),
                              std::move(*compiler),
                              std::move(*queue)};
        } catch (const std::system_error& error) {
            return std::unexpected(CpuBackendError{CpuBackendErrorCode::ResourceConstructionFailed,
                                                   static_cast<uint64_t>(error.code().value())});
        } catch (const std::bad_alloc&) {
            return std::unexpected(CpuBackendError{CpuBackendErrorCode::OutOfMemory, 0});
        } catch (...) {
            return std::unexpected(CpuBackendError{CpuBackendErrorCode::ResourceConstructionFailed, 0});
        }
    }

    DeviceSnapshot CpuBackend::Snapshot() const noexcept {
        auto snapshot = observer_.Observe();
        return snapshot ? *snapshot
                        : DeviceSnapshot{.id = kCpuDevice, .state = DeviceState::Unavailable, .generation = 1};
    }

    std::expected<std::span<std::byte>, CpuBackendError> CpuBackend::Map(const Allocation& allocation, uint64_t offset,
                                                                         uint64_t sizeBytes) const noexcept {
        if (!service_ || !allocation || allocation.View().service != service_.Id()) {
            return std::unexpected(CpuBackendError{CpuBackendErrorCode::ForeignAllocation, 0});
        }
        auto& state = *static_cast<CpuServiceState*>(service_.State());
        CpuAllocationHandle* handle = ResolveAllocation(state, allocation.View());
        if (handle == nullptr || !allocation.View().properties.Contains(kHostProperties) ||
            offset > handle->sizeBytes) {
            return std::unexpected(CpuBackendError{CpuBackendErrorCode::ForeignAllocation, 0});
        }
        const uint64_t available = handle->sizeBytes - offset;
        const uint64_t resolvedSize = sizeBytes == UINT64_MAX ? available : sizeBytes;
        if (resolvedSize > available || resolvedSize > std::numeric_limits<size_t>::max()) {
            return std::unexpected(CpuBackendError{CpuBackendErrorCode::RangeOutOfBounds, resolvedSize});
        }
        return std::span<std::byte>{AllocationAddress(*handle, offset), static_cast<size_t>(resolvedSize)};
    }

    CpuBackendStatistics CpuBackend::Statistics() const noexcept {
        if (!service_) {
            return {};
        }
        const auto& state = *static_cast<const CpuServiceState*>(service_.State());
        return {.submittedBatches = state.submittedBatches.load(std::memory_order_relaxed),
                .parallelDispatches = state.parallelDispatches.load(std::memory_order_relaxed),
                .scalarWorkItems = state.scalarWorkItems.load(std::memory_order_relaxed),
                .simdWorkItems = state.simdWorkItems.load(std::memory_order_relaxed),
                .nativeWorkItems = state.nativeWorkItems.load(std::memory_order_relaxed)};
    }

    void CpuBackend::ResetStatistics() const noexcept {
        if (!service_) {
            return;
        }
        auto& state = *static_cast<CpuServiceState*>(service_.State());
        state.submittedBatches.store(0, std::memory_order_relaxed);
        state.parallelDispatches.store(0, std::memory_order_relaxed);
        state.scalarWorkItems.store(0, std::memory_order_relaxed);
        state.simdWorkItems.store(0, std::memory_order_relaxed);
        state.nativeWorkItems.store(0, std::memory_order_relaxed);
    }

} // namespace Sora::Compute

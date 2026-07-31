/**
 * @file VulkanExecution.cpp
 * @brief VMA allocation, timeline event, transfer, and ordered Vulkan queue
 * implementation.
 * @ingroup Compute
 */
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION

#include "VulkanExecution.h"

#if defined(__clang__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wnullability-completeness"
#endif
#include <vk_mem_alloc.h>
#if defined(__clang__)
#    pragma clang diagnostic pop
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Sora::Compute::Detail {

    namespace {

        inline constexpr uint64_t kVulkanDeviceDomainTag = 0x564B5F4445564943ULL;
        inline constexpr uint64_t kVulkanHostDomainTag = 0x564B5F484F53545FULL;
        inline constexpr uint64_t kVulkanHostDeviceTag = 0x534F52415F484F53ULL;
        inline constexpr uint64_t kVulkanQueueTag = 0x534F52415F564B51ULL;
        inline constexpr uint64_t kVulkanMemoryTag = 0x534F52415F564B4DULL;
        inline constexpr uint64_t kVulkanUploadTag = 0x534F52415F565550ULL;
        inline constexpr uint64_t kVulkanDownloadTag = 0x534F52415F56444EULL;
        inline constexpr uint32_t kTimestampSlotsPerPool = 256;
        inline constexpr uint64_t kUnresolvedExecutionNanoseconds = UINT64_MAX;

        enum class VulkanSubmissionDetail : uint64_t {
            InvalidPipeline = 1,
            InvalidDispatchAbi,
            InvalidBinding,
            InvalidMetadata,
            InvalidCopyAllocation,
            InvalidTimelineWait,
            TimelineExhausted,
        };

        [[nodiscard]] uint64_t DriverDetail(VkResult result) noexcept {
            return static_cast<uint64_t>(static_cast<int64_t>(result));
        }

        [[nodiscard]] MemoryDomainId MakeDomain(DeviceId device, uint64_t tag) noexcept {
            return {.low = device.low ^ tag, .high = device.high ^ std::rotl(tag, 17)};
        }

        [[nodiscard]] DeviceId MakeHostDevice(DeviceId device) noexcept {
            DeviceId host{.low = device.low ^ kVulkanHostDeviceTag,
                          .high = device.high ^ std::rotl(kVulkanHostDeviceTag, 29)};
            if (!host.Valid() || host == device) {
                host = {.low = kVulkanHostDeviceTag, .high = device.high ^ UINT64_MAX};
            }
            return host;
        }

        [[nodiscard]] MemoryPropertyMask ToMemoryProperties(VkMemoryPropertyFlags flags) noexcept {
            MemoryPropertyMask result;
            if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
                result = result | MemoryProperty::HostVisible;
            }
            if ((flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0) {
                result = result | MemoryProperty::HostCoherent;
            }
            if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
                result = result | MemoryProperty::DeviceLocal;
            }
            if ((flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                result = result | MemoryProperty::Unified;
            }
            return result;
        }

        [[nodiscard]] VkMemoryPropertyFlags ToVulkanMemoryProperties(MemoryPropertyMask properties) noexcept {
            VkMemoryPropertyFlags result = 0;
            if (properties.Contains(MemoryProperty::HostVisible)) {
                result |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
            }
            if (properties.Contains(MemoryProperty::HostCoherent)) {
                result |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            }
            if (properties.Contains(MemoryProperty::DeviceLocal)) {
                result |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            }
            return result;
        }

        [[nodiscard]] bool HasMemoryType(const VkPhysicalDeviceMemoryProperties& memory,
                                         VkMemoryPropertyFlags required) noexcept {
            for (uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
                if ((memory.memoryTypes[index].propertyFlags & required) == required) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] uint64_t EffectiveAlignment(AllocationView allocation, uint64_t offset) noexcept {
            if (offset == 0) {
                return allocation.alignment;
            }
            return std::min<uint64_t>(allocation.alignment, uint64_t{1} << std::countr_zero(offset));
        }

        void StoreU32(std::span<std::byte> destination, uint32_t offset, uint32_t value) noexcept {
            if constexpr (std::endian::native == std::endian::big) {
                value = std::byteswap(value);
            }
            std::memcpy(destination.data() + offset, &value, sizeof(value));
        }

        void AtomicFetchMaximum(std::atomic<uint64_t>& target, uint64_t value) noexcept {
            uint64_t observed = target.load(std::memory_order_relaxed);
            while (observed < value && !target.compare_exchange_weak(observed, value, std::memory_order_relaxed,
                                                                     std::memory_order_relaxed)) {
            }
        }

        [[nodiscard]] constexpr SubmissionError InvalidSubmission(VulkanSubmissionDetail detail) noexcept {
            return {SubmissionErrorCode::InvalidRequest, std::to_underlying(detail)};
        }

        [[nodiscard]] SubmissionError DriverSubmissionError(VkResult result) noexcept {
            if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
                return {SubmissionErrorCode::OutOfMemory, DriverDetail(result)};
            }
            if (result == VK_ERROR_DEVICE_LOST) {
                return {SubmissionErrorCode::DeviceLost, DriverDetail(result)};
            }
            return {SubmissionErrorCode::SubmissionFailed, DriverDetail(result)};
        }

        struct DescriptorWindow {
            VkDeviceSize offset = 0;
            VkDeviceSize range = 0;
            uint32_t shaderOffset = 0;
        };

        [[nodiscard]] std::optional<DescriptorWindow>
        ResolveDescriptorWindow(const BackendBinding& binding, const VkPhysicalDeviceLimits& limits) noexcept {
            const uint64_t alignment = std::max<uint64_t>(limits.minStorageBufferOffsetAlignment, 1);
            if (!std::has_single_bit(alignment)) {
                return std::nullopt;
            }
            const uint64_t descriptorOffset = binding.offset & ~(alignment - 1);
            const uint64_t shaderOffset = binding.offset - descriptorOffset;
            if (shaderOffset > UINT32_MAX || binding.size > limits.maxStorageBufferRange ||
                shaderOffset > limits.maxStorageBufferRange - binding.size) {
                return std::nullopt;
            }
            return DescriptorWindow{.offset = descriptorOffset,
                                    .range = shaderOffset + binding.size,
                                    .shaderOffset = static_cast<uint32_t>(shaderOffset)};
        }

        struct VulkanAllocatorState;

        struct VulkanTimestampPool {
            VkQueryPool handle = VK_NULL_HANDLE;
            std::array<uint32_t, kTimestampSlotsPerPool> freeSlots{};
            uint32_t nextUnusedSlot = 0;
            uint32_t freeSlotCount = 0;
        };

        struct VulkanTimestampLease {
            VulkanTimestampLease() noexcept = default;
            VulkanTimestampLease(VulkanExecutionState* state, VulkanTimestampPool* queryPool,
                                 uint32_t querySlot) noexcept
                : owner(state), pool(queryPool), slot(querySlot) {}
            VulkanTimestampLease(const VulkanTimestampLease&) = delete;
            VulkanTimestampLease& operator=(const VulkanTimestampLease&) = delete;
            VulkanTimestampLease(VulkanTimestampLease&& other) noexcept
                : owner(std::exchange(other.owner, nullptr)),
                  pool(std::exchange(other.pool, nullptr)),
                  slot(std::exchange(other.slot, UINT32_MAX)) {}
            VulkanTimestampLease& operator=(VulkanTimestampLease&& other) noexcept {
                if (this != &other) {
                    Reset();
                    owner = std::exchange(other.owner, nullptr);
                    pool = std::exchange(other.pool, nullptr);
                    slot = std::exchange(other.slot, UINT32_MAX);
                }
                return *this;
            }
            ~VulkanTimestampLease() noexcept { Reset(); }

            [[nodiscard]] explicit operator bool() const noexcept {
                return owner != nullptr && pool != nullptr && slot != UINT32_MAX;
            }

            void Reset() noexcept;

            VulkanExecutionState* owner = nullptr;
            VulkanTimestampPool* pool = nullptr;
            uint32_t slot = UINT32_MAX;
        };

        struct VulkanAllocationHandle {
            VulkanExecutionState* owner = nullptr;
            VkBuffer buffer = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            void* mapped = nullptr;
            MemoryLocation location{};
            PhysicalMemoryId memory{};
            MemoryPropertyMask properties{};
            uint64_t sizeBytes = 0;
            uint64_t alignment = 1;
        };

        struct VulkanSubmissionResources {
            VulkanExecutionState* owner = nullptr;
            VkCommandPool commandPool = VK_NULL_HANDLE;
            VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
            VkBuffer metadataBuffer = VK_NULL_HANDLE;
            VmaAllocation metadataAllocation = VK_NULL_HANDLE;
            void* metadataMapped = nullptr;
            VulkanTimestampLease timestamp{};

            void Reset() noexcept;
        };

        struct VulkanEventState {
            VulkanExecutionState* owner = nullptr;
            uint64_t value = 0;
            VulkanSubmissionResources resources{};
            std::atomic<uint64_t> executionNanoseconds{kUnresolvedExecutionNanoseconds};
        };

        struct VulkanQueueHandle {
            VulkanExecutionState* owner = nullptr;
        };

        struct VulkanTransferState {
            VulkanExecutionState* owner = nullptr;
            TransferEngineId id{};
            MemoryLocation source{};
            MemoryLocation destination{};
        };

        struct VulkanAllocatorState {
            VulkanExecutionState* owner = nullptr;
            MemoryLocation location{};
            MemoryPropertyMask supportedProperties{};
            bool hostVisible = false;
        };

    } // namespace

    class VulkanExecutionState final {
    public:
        explicit VulkanExecutionState(VulkanExecutionCreateInfo value) noexcept : info(value) {}

        VulkanExecutionCreateInfo info{};
        VmaAllocator allocator = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        VkSemaphore timeline = VK_NULL_HANDLE;
        MemoryLocation deviceLocation{};
        MemoryLocation hostLocation{};
        BackendServiceId serviceId{};
        VulkanAllocatorState deviceAllocator{};
        VulkanAllocatorState hostAllocator{};
        std::array<VulkanTransferState, 2> transfers{};
        std::mutex submitMutex{};
        std::mutex timestampMutex{};
        std::vector<std::unique_ptr<VulkanTimestampPool>> timestampPools{};
        uint64_t nextTimelineValue = 1;
        std::atomic<uint64_t> submittedBatches{0};
        std::atomic<uint64_t> completedBatches{0};
        std::atomic<uint64_t> nextMemoryId{1};
        std::atomic_bool deviceLost{false};

        ~VulkanExecutionState() noexcept {
            for (const auto& pool : timestampPools) {
                info.deviceApi->vkDestroyQueryPool(info.device, pool->handle, nullptr);
            }
            if (timeline != VK_NULL_HANDLE) {
                info.deviceApi->vkDestroySemaphore(info.device, timeline, nullptr);
            }
            if (allocator != VK_NULL_HANDLE) {
                vmaDestroyAllocator(allocator);
            }
        }

        void MarkDeviceLost(VkResult result) noexcept {
            if (result == VK_ERROR_DEVICE_LOST) {
                deviceLost.store(true, std::memory_order_release);
            }
        }
    };

    namespace {

        [[nodiscard]] bool SupportsDeviceTimestamps(const VulkanExecutionState& state) noexcept {
            const float period = state.info.properties.limits.timestampPeriod;
            return state.info.timestampValidBits > 0 && state.info.timestampValidBits <= 64 && period > 0.0F &&
                   std::isfinite(period);
        }

        [[nodiscard]] std::expected<VulkanTimestampLease, VkResult>
        AcquireTimestampLease(VulkanExecutionState& state) noexcept {
            if (!SupportsDeviceTimestamps(state)) {
                return VulkanTimestampLease{};
            }
            std::scoped_lock lock{state.timestampMutex};
            for (const auto& pool : state.timestampPools) {
                if (pool->freeSlotCount != 0) {
                    const uint32_t slot = pool->freeSlots[--pool->freeSlotCount];
                    return VulkanTimestampLease{&state, pool.get(), slot};
                }
                if (pool->nextUnusedSlot < kTimestampSlotsPerPool) {
                    return VulkanTimestampLease{&state, pool.get(), pool->nextUnusedSlot++};
                }
            }

            auto pool = std::unique_ptr<VulkanTimestampPool>{new (std::nothrow) VulkanTimestampPool};
            if (!pool) {
                return std::unexpected(VK_ERROR_OUT_OF_HOST_MEMORY);
            }
            const VkQueryPoolCreateInfo createInfo{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                                   .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                                   .queryCount = kTimestampSlotsPerPool * 2};
            const VkResult result =
                state.info.deviceApi->vkCreateQueryPool(state.info.device, &createInfo, nullptr, &pool->handle);
            if (result != VK_SUCCESS) {
                return std::unexpected(result);
            }
            VulkanTimestampPool* created = pool.get();
            try {
                state.timestampPools.push_back(std::move(pool));
            } catch (...) {
                state.info.deviceApi->vkDestroyQueryPool(state.info.device, created->handle, nullptr);
                return std::unexpected(VK_ERROR_OUT_OF_HOST_MEMORY);
            }
            created->nextUnusedSlot = 1;
            return VulkanTimestampLease{&state, created, 0};
        }

        void VulkanTimestampLease::Reset() noexcept {
            if (!*this) {
                return;
            }
            std::scoped_lock lock{owner->timestampMutex};
            if (pool->freeSlotCount < kTimestampSlotsPerPool) {
                pool->freeSlots[pool->freeSlotCount++] = slot;
            }
            owner = nullptr;
            pool = nullptr;
            slot = UINT32_MAX;
        }

        [[nodiscard]] constexpr uint64_t TimestampDelta(uint64_t start, uint64_t end, uint32_t validBits) noexcept {
            if (validBits == 64) {
                return end - start;
            }
            return (end - start) & ((uint64_t{1} << validBits) - 1);
        }

        static_assert(TimestampDelta(250, 5, 8) == 11);
        static_assert(TimestampDelta(UINT64_MAX - 2, 1, 64) == 4);

        [[nodiscard]] uint64_t TimestampNanoseconds(uint64_t ticks, float period) noexcept {
            const long double nanoseconds =
                static_cast<long double>(std::max<uint64_t>(ticks, 1)) * static_cast<long double>(period);
            if (nanoseconds >= static_cast<long double>(UINT64_MAX)) {
                return UINT64_MAX;
            }
            return std::max<uint64_t>(static_cast<uint64_t>(std::ceil(nanoseconds)), 1);
        }

        [[nodiscard]] std::expected<uint64_t, VkResult> ReadExecutionNanoseconds(VulkanEventState& event) noexcept {
            uint64_t cached = event.executionNanoseconds.load(std::memory_order_acquire);
            if (cached != kUnresolvedExecutionNanoseconds) {
                return cached;
            }
            if (!event.resources.timestamp) {
                event.executionNanoseconds.store(0, std::memory_order_release);
                return 0;
            }

            VulkanExecutionState& state = *event.owner;
            std::scoped_lock lock{state.timestampMutex};
            cached = event.executionNanoseconds.load(std::memory_order_relaxed);
            if (cached != kUnresolvedExecutionNanoseconds) {
                return cached;
            }
            const VulkanTimestampLease& lease = event.resources.timestamp;
            std::array<uint64_t, 2> timestamps{};
            const VkResult result = state.info.deviceApi->vkGetQueryPoolResults(
                state.info.device, lease.pool->handle, lease.slot * 2, 2, sizeof(timestamps), timestamps.data(),
                sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
            if (result != VK_SUCCESS) {
                return std::unexpected(result);
            }
            const uint64_t ticks = TimestampDelta(timestamps[0], timestamps[1], state.info.timestampValidBits);
            const uint64_t duration = TimestampNanoseconds(ticks, state.info.properties.limits.timestampPeriod);
            event.executionNanoseconds.store(duration, std::memory_order_release);
            return duration;
        }

        void RecordTimestampStart(VulkanExecutionState& state, VkCommandBuffer commandBuffer,
                                  const VulkanTimestampLease& timestamp) noexcept {
            if (!timestamp) {
                return;
            }
            const uint32_t firstQuery = timestamp.slot * 2;
            state.info.deviceApi->vkCmdResetQueryPool(commandBuffer, timestamp.pool->handle, firstQuery, 2);
            state.info.deviceApi->vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                                      timestamp.pool->handle, firstQuery);
        }

        void RecordTimestampEnd(VulkanExecutionState& state, VkCommandBuffer commandBuffer,
                                const VulkanTimestampLease& timestamp) noexcept {
            if (!timestamp) {
                return;
            }
            state.info.deviceApi->vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                                      timestamp.pool->handle, timestamp.slot * 2 + 1);
        }

        void VulkanSubmissionResources::Reset() noexcept {
            if (owner == nullptr) {
                return;
            }
            if (metadataBuffer != VK_NULL_HANDLE) {
                vmaDestroyBuffer(owner->allocator, metadataBuffer, metadataAllocation);
            }
            if (descriptorPool != VK_NULL_HANDLE) {
                owner->info.deviceApi->vkDestroyDescriptorPool(owner->info.device, descriptorPool, nullptr);
            }
            if (commandPool != VK_NULL_HANDLE) {
                owner->info.deviceApi->vkDestroyCommandPool(owner->info.device, commandPool, nullptr);
            }
            *this = {};
        }

        [[nodiscard]] EventObservation Observe(VulkanEventState& event, uint64_t timeoutNanoseconds,
                                               bool wait) noexcept {
            VulkanExecutionState& state = *event.owner;
            if (state.deviceLost.load(std::memory_order_acquire)) {
                return {.status = EventStatus::DeviceLost, .error = {SubmissionErrorCode::DeviceLost, 0}};
            }
            uint64_t completed = 0;
            VkResult result = VK_SUCCESS;
            if (wait) {
                const VkSemaphoreWaitInfo waitInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                                   .semaphoreCount = 1,
                                                   .pSemaphores = &state.timeline,
                                                   .pValues = &event.value};
                result = state.info.deviceApi->vkWaitSemaphores(state.info.device, &waitInfo, timeoutNanoseconds);
                if (result == VK_TIMEOUT) {
                    return {.status = EventStatus::Pending};
                }
            } else {
                result =
                    state.info.deviceApi->vkGetSemaphoreCounterValue(state.info.device, state.timeline, &completed);
                if (result == VK_SUCCESS && completed < event.value) {
                    return {.status = EventStatus::Pending};
                }
            }
            if (result == VK_SUCCESS) {
                AtomicFetchMaximum(state.completedBatches, event.value);
                auto duration = ReadExecutionNanoseconds(event);
                if (!duration) {
                    state.MarkDeviceLost(duration.error());
                    if (duration.error() == VK_ERROR_DEVICE_LOST) {
                        return {.status = EventStatus::DeviceLost,
                                .error = {SubmissionErrorCode::DeviceLost, DriverDetail(duration.error())}};
                    }
                    return {.status = EventStatus::Completed};
                }
                return {.status = EventStatus::Completed, .executionNanoseconds = *duration};
            }
            state.MarkDeviceLost(result);
            return result == VK_ERROR_DEVICE_LOST
                       ? EventObservation{.status = EventStatus::DeviceLost,
                                          .error = {SubmissionErrorCode::DeviceLost, DriverDetail(result)}}
                       : EventObservation{.status = EventStatus::Failed,
                                          .error = {SubmissionErrorCode::SubmissionFailed, DriverDetail(result)}};
        }

        void WriteObservation(BackendEventObservation* output, EventObservation observation) noexcept {
            *output = {.status = observation.status,
                       .error = observation.error,
                       .executionNanoseconds = observation.executionNanoseconds};
        }

        void QueryEvent(void* opaque, BackendEventObservation* output) noexcept {
            WriteObservation(output, Observe(*static_cast<VulkanEventState*>(opaque), 0, false));
        }

        void WaitEvent(void* opaque, uint64_t timeoutNanoseconds, BackendEventObservation* output) noexcept {
            WriteObservation(output, Observe(*static_cast<VulkanEventState*>(opaque), timeoutNanoseconds, true));
        }

        void DestroyEvent(void* opaque) noexcept {
            auto event = std::unique_ptr<VulkanEventState>{static_cast<VulkanEventState*>(opaque)};
            static_cast<void>(Observe(*event, UINT64_MAX, true));
            event->resources.Reset();
        }

        [[nodiscard]] BackendEventPayload ReleaseEvent(std::unique_ptr<VulkanEventState> event,
                                                       DeviceId device) noexcept {
            return {.device = device,
                    .backend = BackendKind::Vulkan,
                    .state = event.release(),
                    .destroy = DestroyEvent,
                    .query = QueryEvent,
                    .wait = WaitEvent};
        }

        void DestroyAllocation(void* opaque) noexcept {
            auto allocation = std::unique_ptr<VulkanAllocationHandle>{static_cast<VulkanAllocationHandle*>(opaque)};
            vmaDestroyBuffer(allocation->owner->allocator, allocation->buffer, allocation->allocation);
        }

        [[nodiscard]] bool ValidAllocationHandle(AllocationView view, const VulkanExecutionState& state,
                                                 const VulkanAllocationHandle* handle) noexcept {
            return handle != nullptr && handle->owner == &state && handle->buffer != VK_NULL_HANDLE &&
                   handle->allocation != VK_NULL_HANDLE && view.service == state.serviceId &&
                   view.location == handle->location && view.memory == handle->memory &&
                   view.properties == handle->properties && view.memoryOffset == 0 &&
                   view.sizeBytes == handle->sizeBytes && view.alignment == handle->alignment;
        }

        [[nodiscard]] VulkanAllocationHandle* AllocationHandle(AllocationView view,
                                                               VulkanExecutionState& state) noexcept {
            auto* handle = static_cast<VulkanAllocationHandle*>(view.state);
            return ValidAllocationHandle(view, state, handle) ? handle : nullptr;
        }

        [[nodiscard]] const VulkanAllocationHandle* AllocationHandle(AllocationView view,
                                                                     const VulkanExecutionState& state) noexcept {
            const auto* handle = static_cast<const VulkanAllocationHandle*>(view.state);
            return ValidAllocationHandle(view, state, handle) ? handle : nullptr;
        }

        void MapHostMemory(void* opaque, const BackendHostMemoryRequest* request,
                           BackendHostMemoryResult* result) noexcept {
            auto& state = *static_cast<VulkanExecutionState*>(opaque);
            if (!ValidBackendAbi(request->abiVersion, request->structSize, sizeof(BackendHostMemoryRequest)) ||
                request->location != state.hostLocation || request->allocation.location != state.hostLocation ||
                request->allocation.service != state.serviceId || request->sizeBytes == 0 ||
                !request->allocation.Covers(request->offset, request->sizeBytes) ||
                !Detail::ValidHostMemoryAccess(request->access)) {
                result->error = {HostMemoryErrorCode::InvalidRequest, 0};
                return;
            }
            VulkanAllocationHandle* allocation = AllocationHandle(request->allocation, state);
            if (allocation == nullptr || allocation->mapped == nullptr ||
                !allocation->properties.Contains(Detail::kCoherentHostMemoryProperties)) {
                result->error = {HostMemoryErrorCode::ForeignAllocation, 0};
                return;
            }
            result->address = static_cast<std::byte*>(allocation->mapped) + request->offset;
            result->sizeBytes = request->sizeBytes;
            result->error = {};
        }

        void Allocate(void* opaque, const BackendAllocationRequest* request, BackendAllocationResult* output) noexcept {
            auto& allocatorState = *static_cast<VulkanAllocatorState*>(opaque);
            VulkanExecutionState& state = *allocatorState.owner;
            if (state.deviceLost.load(std::memory_order_acquire)) {
                output->error = {MemoryAllocationErrorCode::DeviceLost, 0};
                return;
            }
            if (request->location != allocatorState.location || request->sizeBytes == 0 ||
                request->sizeBytes > state.info.descriptor.maximumAllocationBytes) {
                output->error = {MemoryAllocationErrorCode::InvalidRequest, request->sizeBytes};
                return;
            }
            try {
                const VkBufferCreateInfo bufferInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                                    .size = request->sizeBytes,
                                                    .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
                VmaAllocationCreateInfo allocationInfo{};
                allocationInfo.usage = allocatorState.hostVisible ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST
                                                                  : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
                allocationInfo.requiredFlags = ToVulkanMemoryProperties(request->requiredProperties);
                allocationInfo.preferredFlags = ToVulkanMemoryProperties(request->preferredProperties);
                if (allocatorState.hostVisible) {
                    allocationInfo.flags =
                        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                }
                VkBuffer buffer = VK_NULL_HANDLE;
                VmaAllocation allocation = VK_NULL_HANDLE;
                VmaAllocationInfo resultInfo{};
                const VkResult result =
                    vmaCreateBufferWithAlignment(state.allocator, &bufferInfo, &allocationInfo, request->alignment,
                                                 &buffer, &allocation, &resultInfo);
                if (result != VK_SUCCESS) {
                    state.MarkDeviceLost(result);
                    output->error = {result == VK_ERROR_OUT_OF_DEVICE_MEMORY || result == VK_ERROR_OUT_OF_HOST_MEMORY
                                         ? MemoryAllocationErrorCode::OutOfMemory
                                     : result == VK_ERROR_DEVICE_LOST ? MemoryAllocationErrorCode::DeviceLost
                                                                      : MemoryAllocationErrorCode::AllocationFailed,
                                     DriverDetail(result)};
                    return;
                }
                VkMemoryPropertyFlags flags = 0;
                vmaGetMemoryTypeProperties(state.allocator, resultInfo.memoryType, &flags);
                const MemoryPropertyMask properties = ToMemoryProperties(flags);
                auto handle = std::unique_ptr<VulkanAllocationHandle>{new (std::nothrow) VulkanAllocationHandle{
                    .owner = &state,
                    .buffer = buffer,
                    .allocation = allocation,
                    .mapped = resultInfo.pMappedData,
                    .location = request->location,
                    .memory = {state.nextMemoryId.fetch_add(1, std::memory_order_relaxed), kVulkanMemoryTag},
                    .properties = properties,
                    .sizeBytes = request->sizeBytes,
                    .alignment = request->alignment,
                }};
                if (!handle) {
                    vmaDestroyBuffer(state.allocator, buffer, allocation);
                    output->error = {MemoryAllocationErrorCode::OutOfMemory, 0};
                    return;
                }
                output->allocation = {.location = handle->location,
                                      .memory = handle->memory,
                                      .properties = handle->properties,
                                      .sizeBytes = handle->sizeBytes,
                                      .alignment = handle->alignment,
                                      .state = handle.release(),
                                      .destroy = DestroyAllocation};
                output->error = {};
            } catch (const std::bad_alloc&) {
                output->error = {MemoryAllocationErrorCode::OutOfMemory, 0};
            } catch (...) {
                output->error = {MemoryAllocationErrorCode::AllocationFailed, 0};
            }
        }

        [[nodiscard]] const BackendBinding* FindBufferBinding(const BackendSubmitRequest& request,
                                                              const BackendCommand& command,
                                                              uint32_t binding) noexcept {
            for (uint32_t index = 0; index < command.bindingCount; ++index) {
                const BackendBinding& candidate = request.bindings[command.firstBinding + index];
                if (candidate.binding == binding) {
                    return &candidate;
                }
            }
            return nullptr;
        }

        [[nodiscard]] const BackendScalarBinding* FindScalarBinding(const BackendSubmitRequest& request,
                                                                    const BackendCommand& command,
                                                                    uint32_t binding) noexcept {
            for (uint32_t index = 0; index < command.scalarBindingCount; ++index) {
                const BackendScalarBinding& candidate = request.scalarBindings[command.firstScalarBinding + index];
                if (candidate.binding == binding) {
                    return &candidate;
                }
            }
            return nullptr;
        }

        [[nodiscard]] bool ValidateDispatchBinding(const BackendBinding& binding, const KernelArgumentDesc& argument,
                                                   const IndexDomain& domain, std::span<const uint64_t> globalExtents,
                                                   const VkPhysicalDeviceLimits& limits) noexcept {
            if (binding.kind != BackendBindingKind::AffineElements || binding.elementType != argument.elementType ||
                binding.layout != argument.layout || binding.access != argument.access || binding.rank != domain.rank ||
                argument.rank != domain.rank ||
                EffectiveAlignment(binding.allocation, binding.offset) < argument.minimumAlignment ||
                binding.componentStrideBytes > UINT32_MAX || !ResolveDescriptorWindow(binding, limits)) {
                return false;
            }
            for (uint8_t axis = 0; axis < domain.rank; ++axis) {
                if (binding.extents[axis] > UINT32_MAX || binding.byteStrides[axis] > UINT32_MAX ||
                    globalExtents[axis] == 0 || globalExtents[axis] > UINT32_MAX ||
                    (domain.staticExtents[axis] != 0 && domain.staticExtents[axis] != globalExtents[axis])) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool BuildDispatchMetadata(const BackendSubmitRequest& request, const BackendCommand& command,
                                                 const VulkanDispatchPipeline& pipeline,
                                                 const VkPhysicalDeviceLimits& limits,
                                                 std::span<std::byte> metadata) noexcept {
            const VulkanKernelProgram& program = *pipeline.program;
            const VulkanDispatchAbi& abi = *pipeline.abi;
            if (metadata.size() != abi.metadataSize || command.bindingCount + 1 != abi.storageBufferCount) {
                return false;
            }
            const bool explicitSlice = command.dispatchExtents[0] != 0;
            std::array<uint64_t, 3> extents{1, 1, 1};
            std::array<uint64_t, 3> globalExtents{1, 1, 1};
            std::array<uint64_t, 3> origin{};
            if (explicitSlice) {
                for (uint8_t axis = 0; axis < program.domain.rank; ++axis) {
                    if (command.dispatchExtents[axis] == 0 || command.globalExtents[axis] == 0 ||
                        command.dispatchExtents[axis] > UINT32_MAX || command.globalExtents[axis] > UINT32_MAX ||
                        command.dispatchOrigin[axis] > command.globalExtents[axis] ||
                        command.dispatchExtents[axis] > command.globalExtents[axis] - command.dispatchOrigin[axis]) {
                        return false;
                    }
                    extents[axis] = command.dispatchExtents[axis];
                    globalExtents[axis] = command.globalExtents[axis];
                    origin[axis] = command.dispatchOrigin[axis];
                }
                for (uint8_t axis = program.domain.rank; axis < 3; ++axis) {
                    if (command.dispatchOrigin[axis] != 0 || command.dispatchExtents[axis] != 0 ||
                        command.globalExtents[axis] != 0) {
                        return false;
                    }
                }
            }
            uint32_t bufferCount = 0;
            uint32_t scalarCount = 0;
            for (uint32_t argumentIndex = 0; argumentIndex < program.argumentCount; ++argumentIndex) {
                const KernelArgumentDesc& argument = program.arguments[argumentIndex];
                const uint32_t metadataOffset = abi.argumentMetadataOffsets[argumentIndex];
                if (argument.kind == ParameterKind::Buffer) {
                    const BackendBinding* binding = FindBufferBinding(request, command, argumentIndex);
                    if (binding == nullptr) {
                        return false;
                    }
                    if (bufferCount == 0) {
                        for (uint8_t axis = 0; axis < program.domain.rank; ++axis) {
                            if (explicitSlice && extents[axis] != binding->extents[axis]) {
                                return false;
                            }
                            if (!explicitSlice) {
                                extents[axis] = binding->extents[axis];
                                globalExtents[axis] = binding->extents[axis];
                            }
                        }
                    }
                    if (!ValidateDispatchBinding(*binding, argument, program.domain, globalExtents, limits)) {
                        return false;
                    }
                    const DescriptorWindow window = *ResolveDescriptorWindow(*binding, limits);
                    if (bufferCount != 0) {
                        for (uint8_t axis = 0; axis < program.domain.rank; ++axis) {
                            if (extents[axis] != binding->extents[axis]) {
                                return false;
                            }
                        }
                    }
                    StoreU32(metadata, metadataOffset, window.shaderOffset);
                    StoreU32(metadata, metadataOffset + 4, std::to_underlying(binding->layout));
                    StoreU32(metadata, metadataOffset + 8, static_cast<uint32_t>(binding->componentStrideBytes));
                    for (uint8_t axis = 0; axis < program.domain.rank; ++axis) {
                        StoreU32(metadata, metadataOffset + 16U + axis * 4U,
                                 static_cast<uint32_t>(binding->byteStrides[axis]));
                    }
                    ++bufferCount;
                } else {
                    const BackendScalarBinding* binding = FindScalarBinding(request, command, argumentIndex);
                    if (binding == nullptr || binding->type != argument.elementType ||
                        binding->size != argument.elementType.ByteSize()) {
                        return false;
                    }
                    std::memcpy(metadata.data() + metadataOffset, request.scalarData + binding->dataOffset,
                                binding->size);
                    ++scalarCount;
                }
            }
            if (bufferCount != command.bindingCount || scalarCount != command.scalarBindingCount) {
                return false;
            }
            for (uint8_t axis = 0; axis < 3; ++axis) {
                StoreU32(metadata, axis * 4U, static_cast<uint32_t>(extents[axis]));
                StoreU32(metadata, 16U + axis * 4U, static_cast<uint32_t>(origin[axis]));
                const uint64_t expected = (extents[axis] + abi.localSize[axis] - 1U) / abi.localSize[axis];
                if (expected != command.workgroupCount[axis]) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] std::expected<void, SubmissionError>
        CreateSubmissionResources(VulkanExecutionState& state, const BackendSubmitRequest& request,
                                  VulkanSubmissionResources& resources, uint64_t& metadataBytes,
                                  uint32_t& descriptorCount, uint32_t& dispatchCount) noexcept {
            resources.owner = &state;
            metadataBytes = 0;
            descriptorCount = 0;
            dispatchCount = 0;
            for (const BackendCommand& command : request.Commands()) {
                if (command.kind != BackendCommandKind::Dispatch) {
                    continue;
                }
                VulkanDispatchPipeline pipeline{};
                if (!state.info.resolvePipeline(state.info.pipelineState, command.artifact, pipeline) ||
                    pipeline.abi == nullptr || pipeline.program == nullptr || pipeline.abi->metadataSize == 0 ||
                    pipeline.abi->storageBufferCount == 0 ||
                    pipeline.abi->storageBufferCount > kVulkanMaximumKernelArguments + 1 ||
                    pipeline.abi->metadataSize > state.info.properties.limits.maxStorageBufferRange ||
                    descriptorCount > UINT32_MAX - pipeline.abi->storageBufferCount) {
                    return std::unexpected(InvalidSubmission(VulkanSubmissionDetail::InvalidDispatchAbi));
                }
                const uint64_t alignment = state.info.properties.limits.minStorageBufferOffsetAlignment;
                const uint64_t aligned =
                    alignment == 0 ? metadataBytes : (metadataBytes + alignment - 1) & ~(alignment - 1);
                if (aligned < metadataBytes || pipeline.abi->metadataSize > UINT64_MAX - aligned) {
                    return std::unexpected(SubmissionError{SubmissionErrorCode::OutOfMemory, metadataBytes});
                }
                metadataBytes = aligned + pipeline.abi->metadataSize;
                descriptorCount += pipeline.abi->storageBufferCount;
                ++dispatchCount;
            }
            if (request.commandCount == 0) {
                return {};
            }
            const VkCommandPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                                   .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                                   .queueFamilyIndex = state.info.queueFamily};
            VkResult result = state.info.deviceApi->vkCreateCommandPool(state.info.device, &poolInfo, nullptr,
                                                                        &resources.commandPool);
            if (result != VK_SUCCESS) {
                return std::unexpected(DriverSubmissionError(result));
            }
            auto timestamp = AcquireTimestampLease(state);
            if (!timestamp) {
                state.MarkDeviceLost(timestamp.error());
                if (timestamp.error() == VK_ERROR_DEVICE_LOST) {
                    return std::unexpected(DriverSubmissionError(timestamp.error()));
                }
            } else {
                resources.timestamp = std::move(*timestamp);
            }
            if (dispatchCount != 0) {
                const VkDescriptorPoolSize poolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    .descriptorCount = descriptorCount};
                const VkDescriptorPoolCreateInfo descriptorInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                                .maxSets = dispatchCount,
                                                                .poolSizeCount = 1,
                                                                .pPoolSizes = &poolSize};
                result = state.info.deviceApi->vkCreateDescriptorPool(state.info.device, &descriptorInfo, nullptr,
                                                                      &resources.descriptorPool);
                if (result != VK_SUCCESS) {
                    return std::unexpected(DriverSubmissionError(result));
                }
                const VkBufferCreateInfo bufferInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                                    .size = metadataBytes,
                                                    .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
                VmaAllocationCreateInfo allocationInfo{};
                allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
                allocationInfo.requiredFlags =
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
                allocationInfo.flags =
                    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo mapped{};
                result = vmaCreateBuffer(state.allocator, &bufferInfo, &allocationInfo, &resources.metadataBuffer,
                                         &resources.metadataAllocation, &mapped);
                resources.metadataMapped = mapped.pMappedData;
                if (result == VK_SUCCESS && resources.metadataMapped == nullptr) {
                    result = VK_ERROR_MEMORY_MAP_FAILED;
                }
            }
            return result == VK_SUCCESS ? std::expected<void, SubmissionError>{}
                                        : std::unexpected(DriverSubmissionError(result));
        }

        [[nodiscard]] std::expected<void, SubmissionError>
        RecordDispatch(VulkanExecutionState& state, VkCommandBuffer commandBuffer, const BackendSubmitRequest& request,
                       const BackendCommand& command, VulkanSubmissionResources& resources, uint64_t& metadataOffset) {
            VulkanDispatchPipeline pipeline{};
            if (!state.info.resolvePipeline(state.info.pipelineState, command.artifact, pipeline)) {
                return std::unexpected(InvalidSubmission(VulkanSubmissionDetail::InvalidPipeline));
            }
            const uint64_t alignment = state.info.properties.limits.minStorageBufferOffsetAlignment;
            if (alignment != 0) {
                metadataOffset = (metadataOffset + alignment - 1) & ~(alignment - 1);
            }
            auto metadata = std::span{static_cast<std::byte*>(resources.metadataMapped) + metadataOffset,
                                      static_cast<size_t>(pipeline.abi->metadataSize)};
            std::fill(metadata.begin(), metadata.end(), std::byte{0});
            if (!BuildDispatchMetadata(request, command, pipeline, state.info.properties.limits, metadata)) {
                return std::unexpected(InvalidSubmission(VulkanSubmissionDetail::InvalidMetadata));
            }

            const VkDescriptorSetAllocateInfo setInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                      .descriptorPool = resources.descriptorPool,
                                                      .descriptorSetCount = 1,
                                                      .pSetLayouts = &pipeline.descriptorSetLayout};
            VkDescriptorSet set = VK_NULL_HANDLE;
            VkResult result = state.info.deviceApi->vkAllocateDescriptorSets(state.info.device, &setInfo, &set);
            if (result != VK_SUCCESS) {
                return std::unexpected(DriverSubmissionError(result));
            }
            std::array<VkDescriptorBufferInfo, kVulkanMaximumKernelArguments + 1> bufferInfos{};
            std::array<VkWriteDescriptorSet, kVulkanMaximumKernelArguments + 1> writes{};
            uint32_t descriptorIndex = 0;
            for (uint32_t argument = 0; argument < pipeline.program->argumentCount; ++argument) {
                if (pipeline.program->arguments[argument].kind != ParameterKind::Buffer) {
                    continue;
                }
                const BackendBinding* binding = FindBufferBinding(request, command, argument);
                if (binding == nullptr) {
                    return std::unexpected(InvalidSubmission(VulkanSubmissionDetail::InvalidBinding));
                }
                VulkanAllocationHandle* allocation = AllocationHandle(binding->allocation, state);
                const auto window = ResolveDescriptorWindow(*binding, state.info.properties.limits);
                if (allocation == nullptr || !window) {
                    return std::unexpected(InvalidSubmission(VulkanSubmissionDetail::InvalidBinding));
                }
                bufferInfos[descriptorIndex] = {
                    .buffer = allocation->buffer, .offset = window->offset, .range = window->range};
                writes[descriptorIndex] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = argument,
                                           .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           .pBufferInfo = &bufferInfos[descriptorIndex]};
                ++descriptorIndex;
            }
            bufferInfos[descriptorIndex] = {
                .buffer = resources.metadataBuffer, .offset = metadataOffset, .range = pipeline.abi->metadataSize};
            writes[descriptorIndex] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = pipeline.abi->metadataBinding,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .pBufferInfo = &bufferInfos[descriptorIndex]};
            ++descriptorIndex;
            state.info.deviceApi->vkUpdateDescriptorSets(state.info.device, descriptorIndex, writes.data(), 0, nullptr);
            state.info.deviceApi->vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
            state.info.deviceApi->vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                                          pipeline.pipelineLayout, 0, 1, &set, 0, nullptr);
            state.info.deviceApi->vkCmdDispatch(commandBuffer, command.workgroupCount[0], command.workgroupCount[1],
                                                command.workgroupCount[2]);
            metadataOffset += pipeline.abi->metadataSize;
            return {};
        }

        [[nodiscard]] std::expected<void, SubmissionError>
        RecordCopy(VulkanExecutionState& state, VkCommandBuffer commandBuffer, const BackendCommand& command) noexcept {
            VulkanAllocationHandle* source = AllocationHandle(command.source, state);
            VulkanAllocationHandle* target = AllocationHandle(command.target, state);
            if (source == nullptr || target == nullptr) {
                return std::unexpected(InvalidSubmission(VulkanSubmissionDetail::InvalidCopyAllocation));
            }
            const VkBufferCopy region{
                .srcOffset = command.sourceOffset, .dstOffset = command.targetOffset, .size = command.sizeBytes};
            state.info.deviceApi->vkCmdCopyBuffer(commandBuffer, source->buffer, target->buffer, 1, &region);
            return {};
        }

        [[nodiscard]] std::expected<void, SubmissionError>
        RecordTransferRegions(VulkanExecutionState& state, VkCommandBuffer commandBuffer, const BackendCommand& command,
                              std::span<const TransferRegion> regions) {
            VulkanAllocationHandle* source = AllocationHandle(command.source, state);
            VulkanAllocationHandle* target = AllocationHandle(command.target, state);
            if (source == nullptr || target == nullptr || regions.empty() || regions.size() > UINT32_MAX) {
                return std::unexpected(InvalidSubmission(VulkanSubmissionDetail::InvalidCopyAllocation));
            }
            if (regions.size() == 1) {
                const TransferRegion region = regions.front();
                const VkBufferCopy native{
                    .srcOffset = region.sourceOffset, .dstOffset = region.targetOffset, .size = region.sizeBytes};
                state.info.deviceApi->vkCmdCopyBuffer(commandBuffer, source->buffer, target->buffer, 1, &native);
                return {};
            }
            std::vector<VkBufferCopy> nativeRegions;
            nativeRegions.reserve(regions.size());
            for (const TransferRegion region : regions) {
                nativeRegions.push_back(
                    {.srcOffset = region.sourceOffset, .dstOffset = region.targetOffset, .size = region.sizeBytes});
            }
            state.info.deviceApi->vkCmdCopyBuffer(commandBuffer, source->buffer, target->buffer,
                                                  static_cast<uint32_t>(nativeRegions.size()), nativeRegions.data());
            return {};
        }

        void RecordDeviceBarrier(VulkanExecutionState& state, VkCommandBuffer commandBuffer) noexcept {
            const VkMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                          .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                                          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                                           VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
            state.info.deviceApi->vkCmdPipelineBarrier(
                commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0,
                nullptr);
        }

        void RecordHostBarrier(VulkanExecutionState& state, VkCommandBuffer commandBuffer) noexcept {
            const VkMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                          .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                                          .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
            state.info.deviceApi->vkCmdPipelineBarrier(
                commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        }

        void Submit(VulkanExecutionState& state, const BackendSubmitRequest* request, BackendSubmitResult* output,
                    std::span<const TransferRegion> transferRegions = {}) noexcept {
            if (state.deviceLost.load(std::memory_order_acquire)) {
                output->error = {SubmissionErrorCode::DeviceLost, 0};
                return;
            }
            try {
                auto event = std::unique_ptr<VulkanEventState>{new (std::nothrow) VulkanEventState};
                if (!event) {
                    output->error = {SubmissionErrorCode::OutOfMemory, 0};
                    return;
                }
                event->owner = &state;
                uint64_t metadataBytes = 0;
                uint32_t descriptorCount = 0;
                uint32_t dispatchCount = 0;
                auto resources = CreateSubmissionResources(state, *request, event->resources, metadataBytes,
                                                           descriptorCount, dispatchCount);
                if (!resources) {
                    event->resources.Reset();
                    if (resources.error().code == SubmissionErrorCode::DeviceLost) {
                        state.deviceLost.store(true, std::memory_order_release);
                    }
                    output->error = resources.error();
                    return;
                }
                VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
                VkResult result = VK_SUCCESS;
                std::optional<SubmissionError> recordingError;
                if (request->commandCount != 0) {
                    const VkCommandBufferAllocateInfo allocationInfo{.sType =
                                                                         VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                                     .commandPool = event->resources.commandPool,
                                                                     .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                                     .commandBufferCount = 1};
                    result = state.info.deviceApi->vkAllocateCommandBuffers(state.info.device, &allocationInfo,
                                                                            &commandBuffer);
                    const VkCommandBufferBeginInfo beginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                                             .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
                    if (result == VK_SUCCESS) {
                        result = state.info.deviceApi->vkBeginCommandBuffer(commandBuffer, &beginInfo);
                    }
                    if (result == VK_SUCCESS) {
                        RecordTimestampStart(state, commandBuffer, event->resources.timestamp);
                    }
                    uint64_t metadataOffset = 0;
                    for (uint32_t index = 0; result == VK_SUCCESS && !recordingError && index < request->commandCount;
                         ++index) {
                        const BackendCommand& command = request->commands[index];
                        auto recorded =
                            !transferRegions.empty()
                                ? RecordTransferRegions(state, commandBuffer, command, transferRegions)
                            : command.kind == BackendCommandKind::Dispatch
                                ? RecordDispatch(state, commandBuffer, *request, command, event->resources,
                                                 metadataOffset)
                            : command.kind == BackendCommandKind::Copy
                                ? RecordCopy(state, commandBuffer, command)
                                : std::expected<void, SubmissionError>{std::unexpected(SubmissionError{
                                      SubmissionErrorCode::UnsupportedCommand, std::to_underlying(command.kind)})};
                        if (!recorded) {
                            recordingError = recorded.error();
                        } else if (index + 1 < request->commandCount) {
                            RecordDeviceBarrier(state, commandBuffer);
                        }
                    }
                    if (result == VK_SUCCESS && !recordingError) {
                        RecordHostBarrier(state, commandBuffer);
                        RecordTimestampEnd(state, commandBuffer, event->resources.timestamp);
                        result = state.info.deviceApi->vkEndCommandBuffer(commandBuffer);
                    }
                }
                if (result != VK_SUCCESS || recordingError) {
                    event->resources.Reset();
                    state.MarkDeviceLost(result);
                    output->error = recordingError ? *recordingError : DriverSubmissionError(result);
                    return;
                }

                uint64_t waitValue = 0;
                for (const EventView& wait : request->Waits()) {
                    const auto* waitEvent = static_cast<const VulkanEventState*>(wait.state);
                    if (waitEvent == nullptr || waitEvent->owner != &state) {
                        event->resources.Reset();
                        output->error = {SubmissionErrorCode::InvalidRequest, 0};
                        return;
                    }
                    waitValue = std::max(waitValue, waitEvent->value);
                }
                std::scoped_lock lock{state.submitMutex};
                if (state.nextTimelineValue == 0 || waitValue >= state.nextTimelineValue) {
                    event->resources.Reset();
                    output->error =
                        InvalidSubmission(state.nextTimelineValue == 0 ? VulkanSubmissionDetail::TimelineExhausted
                                                                       : VulkanSubmissionDetail::InvalidTimelineWait);
                    return;
                }
                if (state.nextTimelineValue > state.info.maximumTimelineValueDifference) {
                    const uint64_t minimumCompleted =
                        state.nextTimelineValue - state.info.maximumTimelineValueDifference;
                    const VkSemaphoreWaitInfo windowWaitInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                                             .semaphoreCount = 1,
                                                             .pSemaphores = &state.timeline,
                                                             .pValues = &minimumCompleted};
                    result = state.info.deviceApi->vkWaitSemaphores(state.info.device, &windowWaitInfo, UINT64_MAX);
                    if (result != VK_SUCCESS) {
                        event->resources.Reset();
                        state.MarkDeviceLost(result);
                        output->error = DriverSubmissionError(result);
                        return;
                    }
                }
                event->value = state.nextTimelineValue;
                const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                const VkTimelineSemaphoreSubmitInfo timelineInfo{
                    .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                    .waitSemaphoreValueCount = waitValue == 0 ? 0U : 1U,
                    .pWaitSemaphoreValues = waitValue == 0 ? nullptr : &waitValue,
                    .signalSemaphoreValueCount = 1,
                    .pSignalSemaphoreValues = &event->value};
                const VkSubmitInfo submitInfo{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                              .pNext = &timelineInfo,
                                              .waitSemaphoreCount = waitValue == 0 ? 0U : 1U,
                                              .pWaitSemaphores = waitValue == 0 ? nullptr : &state.timeline,
                                              .pWaitDstStageMask = waitValue == 0 ? nullptr : &waitStage,
                                              .commandBufferCount = commandBuffer == VK_NULL_HANDLE ? 0U : 1U,
                                              .pCommandBuffers =
                                                  commandBuffer == VK_NULL_HANDLE ? nullptr : &commandBuffer,
                                              .signalSemaphoreCount = 1,
                                              .pSignalSemaphores = &state.timeline};
                result = state.info.deviceApi->vkQueueSubmit(state.queue, 1, &submitInfo, VK_NULL_HANDLE);
                if (result != VK_SUCCESS) {
                    event->resources.Reset();
                    state.MarkDeviceLost(result);
                    output->error = DriverSubmissionError(result);
                    return;
                }
                ++state.nextTimelineValue;
                state.submittedBatches.fetch_add(1, std::memory_order_relaxed);
                output->event = ReleaseEvent(std::move(event), state.info.descriptor.id);
                output->error = {};
            } catch (const std::bad_alloc&) {
                output->error = {SubmissionErrorCode::OutOfMemory, 0};
            } catch (...) {
                output->error = {SubmissionErrorCode::SubmissionFailed, 0};
            }
        }

        void SubmitQueue(void* opaque, const BackendSubmitRequest* request, BackendSubmitResult* output) noexcept {
            auto* queue = static_cast<VulkanQueueHandle*>(opaque);
            if (queue == nullptr || queue->owner == nullptr) {
                if (output != nullptr) {
                    output->error = {SubmissionErrorCode::QueueUnavailable, 0};
                }
                return;
            }
            Submit(*queue->owner, request, output);
        }

        void DestroyQueue(void* opaque) noexcept {
            delete static_cast<VulkanQueueHandle*>(opaque);
        }

        void Transfer(void* opaque, const BackendTransferRequest* request, BackendTransferResult* output) noexcept {
            auto& transfer = *static_cast<VulkanTransferState*>(opaque);
            if (request->engine != transfer.id || request->source.location != transfer.source ||
                request->target.location != transfer.destination) {
                output->error = {TransferErrorCode::InvalidRequest, 0};
                return;
            }
            const TransferRegion first = request->Regions().front();
            const BackendCommand command{.kind = BackendCommandKind::Copy,
                                         .source = request->source,
                                         .target = request->target,
                                         .sourceOffset = first.sourceOffset,
                                         .targetOffset = first.targetOffset,
                                         .sizeBytes = first.sizeBytes};
            const BackendSubmitRequest submitRequest{
                .commands = &command, .commandCount = 1, .waits = request->waits, .waitCount = request->waitCount};
            BackendSubmitResult submitted{};
            Submit(*transfer.owner, &submitRequest, &submitted, request->Regions());
            if (submitted.event.state == nullptr) {
                output->error = {
                    submitted.error.code == SubmissionErrorCode::OutOfMemory  ? TransferErrorCode::OutOfMemory
                    : submitted.error.code == SubmissionErrorCode::DeviceLost ? TransferErrorCode::DeviceLost
                                                                              : TransferErrorCode::TransferFailed,
                    submitted.error.detail};
                return;
            }
            output->completion = submitted.event;
            output->error = {};
        }

    } // namespace

    void VulkanExecutionDeleter::operator()(VulkanExecutionState* state) const noexcept {
        delete state;
    }

    std::expected<VulkanExecutionBundle, VulkanBackendError>
    CreateVulkanExecution(BackendService service, const VulkanExecutionCreateInfo& info) noexcept {
        if (!service || info.instanceApi == nullptr || info.deviceApi == nullptr || info.instance == VK_NULL_HANDLE ||
            info.physicalDevice == VK_NULL_HANDLE || info.device == VK_NULL_HANDLE || info.queueFamily == UINT32_MAX ||
            !info.descriptor.id.Valid() || info.maximumTimelineValueDifference == 0 ||
            info.resolvePipeline == nullptr || info.pipelineState == nullptr ||
            !HasMemoryType(info.memoryProperties, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
            !HasMemoryType(info.memoryProperties,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::ResourceConstructionFailed, 0});
        }
        try {
            auto state = std::unique_ptr<VulkanExecutionState, VulkanExecutionDeleter>{new (std::nothrow)
                                                                                           VulkanExecutionState{info}};
            if (!state) {
                return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::OutOfMemory, 0});
            }
            state->serviceId = service.Id();
            VmaVulkanFunctions functions{};
            functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
            functions.vkGetDeviceProcAddr = info.instanceApi->vkGetDeviceProcAddr;
            const VmaAllocatorCreateInfo allocatorInfo{.physicalDevice = info.physicalDevice,
                                                       .device = info.device,
                                                       .pVulkanFunctions = &functions,
                                                       .instance = info.instance,
                                                       .vulkanApiVersion = VK_API_VERSION_1_2};
            VkResult result = vmaCreateAllocator(&allocatorInfo, &state->allocator);
            if (result != VK_SUCCESS) {
                return std::unexpected(
                    VulkanBackendError{VulkanBackendErrorCode::ResourceConstructionFailed, DriverDetail(result)});
            }
            info.deviceApi->vkGetDeviceQueue(info.device, info.queueFamily, 0, &state->queue);
            const VkSemaphoreTypeCreateInfo typeInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                                     .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
                                                     .initialValue = 0};
            const VkSemaphoreCreateInfo semaphoreInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                                      .pNext = &typeInfo};
            result = info.deviceApi->vkCreateSemaphore(info.device, &semaphoreInfo, nullptr, &state->timeline);
            if (result != VK_SUCCESS || state->queue == VK_NULL_HANDLE) {
                return std::unexpected(
                    VulkanBackendError{VulkanBackendErrorCode::ResourceConstructionFailed, DriverDetail(result)});
            }

            const DeviceId hostDevice = MakeHostDevice(info.descriptor.id);
            state->deviceLocation = {.domain = MakeDomain(info.descriptor.id, kVulkanDeviceDomainTag),
                                     .device = info.descriptor.id,
                                     .backend = BackendKind::Vulkan};
            state->hostLocation = {.domain = MakeDomain(info.descriptor.id, kVulkanHostDomainTag),
                                   .device = hostDevice,
                                   .backend = BackendKind::Vulkan};
            const MemoryPropertyMask deviceProperties{MemoryProperty::DeviceLocal};
            const MemoryPropertyMask hostProperties = MemoryProperty::HostVisible | MemoryProperty::HostCoherent;
            state->deviceAllocator = {.owner = state.get(),
                                      .location = state->deviceLocation,
                                      .supportedProperties = deviceProperties,
                                      .hostVisible = false};
            state->hostAllocator = {.owner = state.get(),
                                    .location = state->hostLocation,
                                    .supportedProperties = hostProperties,
                                    .hostVisible = true};
            auto deviceAllocator = MemoryAllocator::Create(service, {.location = state->deviceLocation,
                                                                     .supportedProperties = deviceProperties,
                                                                     .state = &state->deviceAllocator,
                                                                     .allocate = Allocate});
            auto hostAllocator = MemoryAllocator::Create(service, {.location = state->hostLocation,
                                                                   .supportedProperties = hostProperties,
                                                                   .state = &state->hostAllocator,
                                                                   .allocate = Allocate});
            auto hostAccessor = HostMemoryAccessor::Create(
                service, {.location = state->hostLocation, .state = state.get(), .map = MapHostMemory});
            auto queueHandle = std::unique_ptr<VulkanQueueHandle>{new (std::nothrow) VulkanQueueHandle{state.get()}};
            if (!queueHandle) {
                return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::OutOfMemory, 0});
            }
            auto queue = Queue::Adopt(service, {.id = {info.descriptor.id.low ^ kVulkanQueueTag,
                                                       info.descriptor.id.high ^ std::rotl(kVulkanQueueTag, 11)},
                                                .device = info.descriptor.id,
                                                .backend = BackendKind::Vulkan,
                                                .capabilities = QueueCapability::Dispatch | QueueCapability::Copy,
                                                .dispatchArtifactKind = ArtifactKind::DevicePipeline,
                                                .state = queueHandle.get(),
                                                .destroy = DestroyQueue,
                                                .submit = SubmitQueue,
                                                .reportsExecutionNanoseconds = SupportsDeviceTimestamps(*state)});
            if (!deviceAllocator || !hostAllocator || !hostAccessor || !queue) {
                return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::ResourceConstructionFailed, 0});
            }
            static_cast<void>(queueHandle.release());

            state->transfers = {
                VulkanTransferState{.owner = state.get(),
                                    .id = {info.descriptor.id.low ^ kVulkanUploadTag, kVulkanUploadTag},
                                    .source = state->hostLocation,
                                    .destination = state->deviceLocation},
                VulkanTransferState{.owner = state.get(),
                                    .id = {info.descriptor.id.low ^ kVulkanDownloadTag, kVulkanDownloadTag},
                                    .source = state->deviceLocation,
                                    .destination = state->hostLocation},
            };
            std::vector<TransferEngine> transfers;
            transfers.reserve(state->transfers.size());
            for (VulkanTransferState& transfer : state->transfers) {
                auto engine = TransferEngine::Create(service, {.engine = transfer.id,
                                                               .sourceService = service.Id(),
                                                               .source = transfer.source,
                                                               .destination = transfer.destination,
                                                               .kind = TransferKind::Copy,
                                                               .completionDevice = info.descriptor.id,
                                                               .completionBackend = BackendKind::Vulkan,
                                                               .state = &transfer,
                                                               .transfer = Transfer});
                if (!engine) {
                    return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::ResourceConstructionFailed,
                                                              static_cast<uint64_t>(engine.error().code)});
                }
                transfers.push_back(std::move(*engine));
            }
            const MemoryLocation deviceLocation = state->deviceLocation;
            const MemoryLocation hostLocation = state->hostLocation;
            return VulkanExecutionBundle{.state = std::move(state),
                                         .deviceLocation = deviceLocation,
                                         .hostLocation = hostLocation,
                                         .deviceAllocator = std::move(*deviceAllocator),
                                         .hostAllocator = std::move(*hostAllocator),
                                         .hostAccessor = std::move(*hostAccessor),
                                         .transfers = std::move(transfers),
                                         .queue = std::move(*queue)};
        } catch (const std::bad_alloc&) {
            return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::OutOfMemory, 0});
        } catch (...) {
            return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::ResourceConstructionFailed, 0});
        }
    }

    DeviceSnapshot QueryVulkanExecution(const VulkanExecutionState& state) noexcept {
        if (state.deviceLost.load(std::memory_order_acquire)) {
            return {.id = state.info.descriptor.id,
                    .state = DeviceState::Lost,
                    .generation = 2,
                    .observationSequence = state.submittedBatches.load(std::memory_order_relaxed)};
        }
        std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
        vmaGetHeapBudgets(state.allocator, budgets.data());
        uint64_t available = 0;
        for (uint32_t heap = 0; heap < state.info.memoryProperties.memoryHeapCount; ++heap) {
            if ((state.info.memoryProperties.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
                const uint64_t remaining =
                    budgets[heap].usage < budgets[heap].budget ? budgets[heap].budget - budgets[heap].usage : 0;
                available = available > UINT64_MAX - remaining ? UINT64_MAX : available + remaining;
            }
        }
        uint64_t completed = 0;
        const VkResult result =
            state.info.deviceApi->vkGetSemaphoreCounterValue(state.info.device, state.timeline, &completed);
        if (result == VK_ERROR_DEVICE_LOST) {
            const_cast<VulkanExecutionState&>(state).deviceLost.store(true, std::memory_order_release);
            return {.id = state.info.descriptor.id,
                    .state = DeviceState::Lost,
                    .generation = 2,
                    .observationSequence = state.submittedBatches.load(std::memory_order_relaxed)};
        }
        const uint64_t submitted = state.submittedBatches.load(std::memory_order_relaxed);
        const uint64_t outstanding = submitted > completed ? submitted - completed : 0;
        return {.id = state.info.descriptor.id,
                .state = result == VK_SUCCESS ? DeviceState::Available : DeviceState::Unavailable,
                .generation = 1,
                .observationSequence = submitted,
                .availableMemoryBytes = available,
                .estimatedQueueDelayNanoseconds = 0,
                .loadPermille = static_cast<uint16_t>(std::min<uint64_t>(outstanding * 125, 1000))};
    }

    std::expected<std::span<std::byte>, VulkanBackendError>
    MapVulkanAllocation(const VulkanExecutionState& state, BackendServiceId service, const Allocation& allocation,
                        uint64_t offset, uint64_t sizeBytes) noexcept {
        if (!allocation || allocation.View().service != service) {
            return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::ForeignAllocation, 0});
        }
        const AllocationView view = allocation.View();
        const VulkanAllocationHandle* handle = AllocationHandle(view, state);
        if (handle == nullptr) {
            return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::ForeignAllocation, 0});
        }
        if (handle->mapped == nullptr || !handle->properties.Contains(MemoryProperty::HostVisible)) {
            return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::NotHostVisible, 0});
        }
        const uint64_t resolvedSize =
            sizeBytes == UINT64_MAX ? view.sizeBytes - std::min(offset, view.sizeBytes) : sizeBytes;
        if (!view.Covers(offset, resolvedSize)) {
            return std::unexpected(VulkanBackendError{VulkanBackendErrorCode::RangeOutOfBounds, offset});
        }
        return std::span{static_cast<std::byte*>(handle->mapped) + offset, static_cast<size_t>(resolvedSize)};
    }

} // namespace Sora::Compute::Detail

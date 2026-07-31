/**
 * @file VulkanExecution.h
 * @brief Private VMA allocation, timeline event, transfer, and queue
 * construction boundary.
 * @ingroup Compute
 */
#pragma once

#include "SlangKernelLowering.h"

#include <Sora/Compute/Backend/HostMemoryApi.h>
#include <Sora/Compute/Backend/MemoryApi.h>
#include <Sora/Compute/Backend/Pipeline.h>
#include <Sora/Compute/Backend/Queue.h>
#include <Sora/Compute/Backend/TransferApi.h>
#include <Sora/Compute/Backend/VulkanBackend.h>

#include <volk.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

namespace Sora::Compute::Detail {

    /** @brief Backend-private live pipeline state required to record one dispatch.
     */
    struct VulkanDispatchPipeline {
        VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        const VulkanKernelProgram* program = nullptr;
        const VulkanDispatchAbi* abi = nullptr;
    };

    using ResolveVulkanDispatchPipeline = bool (*)(void* state, ArtifactView artifact,
                                                   VulkanDispatchPipeline& pipeline) noexcept;

    /** @brief Immutable device facts and callbacks consumed by the Vulkan execution
     * factory. */
    struct VulkanExecutionCreateInfo {
        VolkInstanceTable* instanceApi = nullptr;
        VolkDeviceTable* deviceApi = nullptr;
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        uint32_t queueFamily = UINT32_MAX;
        uint32_t timestampValidBits = 0;
        VkPhysicalDeviceProperties properties{};
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        uint64_t maximumTimelineValueDifference = 0;
        DeviceDescriptor descriptor{};
        void* pipelineState = nullptr;
        ResolveVulkanDispatchPipeline resolvePipeline = nullptr;
    };

    class VulkanExecutionState;

    struct VulkanExecutionDeleter {
        void operator()(VulkanExecutionState* state) const noexcept;
    };

    /** @brief Transactional result containing one unpublished execution state and
     * all validated facades. */
    struct VulkanExecutionBundle {
        std::unique_ptr<VulkanExecutionState, VulkanExecutionDeleter> state{};
        MemoryLocation deviceLocation{};
        MemoryLocation hostLocation{};
        MemoryAllocator deviceAllocator;
        MemoryAllocator hostAllocator;
        HostMemoryAccessor hostAccessor;
        std::vector<TransferEngine> transfers{};
        Queue queue{};
    };

    /** @brief Construct VMA, queue, timeline, allocators, transfers, and their
     * backend-neutral facades. */
    [[nodiscard]] std::expected<VulkanExecutionBundle, VulkanBackendError>
    CreateVulkanExecution(BackendService service, const VulkanExecutionCreateInfo& info) noexcept;

    /** @brief Sample the execution state without synchronizing the queue. */
    [[nodiscard]] DeviceSnapshot QueryVulkanExecution(const VulkanExecutionState& state) noexcept;

    /** @brief Validate and map one coherent allocation owned by @p service. */
    [[nodiscard]] std::expected<std::span<std::byte>, VulkanBackendError>
    MapVulkanAllocation(const VulkanExecutionState& state, BackendServiceId service, const Allocation& allocation,
                        uint64_t offset, uint64_t sizeBytes) noexcept;

} // namespace Sora::Compute::Detail

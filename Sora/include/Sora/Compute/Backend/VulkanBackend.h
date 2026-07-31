/**
 * @file VulkanBackend.h
 * @brief Complete Vulkan compute backend with Slang JIT, VMA memory, timeline events, and ordered dispatch.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Backend.h>
#include <Sora/Compute/Backend/DeviceObserver.h>
#include <Sora/Compute/Backend/HostMemoryApi.h>
#include <Sora/Compute/Backend/MemoryApi.h>
#include <Sora/Compute/Backend/Pipeline.h>
#include <Sora/Compute/Backend/Queue.h>
#include <Sora/Compute/Backend/TransferApi.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>
#include <vector>

namespace Sora::Compute {

    inline constexpr size_t kVulkanMaximumKernelArguments = 32;
    inline constexpr size_t kVulkanMaximumScalarParameters = 32;
    inline constexpr size_t kVulkanMaximumInstructions = 256;
    inline constexpr uint8_t kVulkanMaximumValueLanes = 4;

    /** @brief Stable failure while constructing or using the Vulkan compute backend. */
    enum class VulkanBackendErrorCode : uint8_t {
        InvalidConfiguration,
        VulkanLoaderUnavailable,
        InstanceCreationFailed,
        PhysicalDeviceEnumerationFailed,
        NoCompatibleDevice,
        DeviceCreationFailed,
        SlangInitializationFailed,
        ResourceConstructionFailed,
        ForeignAllocation,
        NotHostVisible,
        RangeOutOfBounds,
        DeviceLost,
        OutOfMemory,
    };

    /** @brief Vulkan backend failure with a driver or requested-range diagnostic value. */
    struct VulkanBackendError {
        VulkanBackendErrorCode code = VulkanBackendErrorCode::ResourceConstructionFailed;
        uint64_t detail = 0;

        friend constexpr bool operator==(VulkanBackendError, VulkanBackendError) noexcept = default;
    };

    /** @brief Physical-device and compiler policy for one Vulkan backend service. */
    struct VulkanBackendConfig {
        DeviceId preferredDevice{};
        bool preferDiscreteDevice = true;
        bool preferDedicatedComputeQueue = true;
        uint16_t compilerWorkerCount = 0; ///< Zero selects an implementation-defined bounded pool.

        friend constexpr bool operator==(VulkanBackendConfig, VulkanBackendConfig) noexcept = default;
    };

    /** @brief Construction policy for the complete set of compatible Vulkan devices. */
    struct VulkanBackendSetConfig {
        bool preferDedicatedComputeQueue = true;
        uint16_t compilerWorkerCount = 0; ///< Zero selects one bounded pool shared by the complete set.

        friend constexpr bool operator==(VulkanBackendSetConfig, VulkanBackendSetConfig) noexcept = default;
    };

    /**
     * @brief Complete Vulkan backend resource set for one physical device and exact driver context.
     * @details Compiler, pipeline, allocation, transfer, queue, and event handles share one BackendServiceId. Host
     * staging and device-local memory are distinct residency domains connected by asynchronous copy engines.
     */
    class VulkanBackend final {
    public:
        VulkanBackend() = delete;
        VulkanBackend(const VulkanBackend&) = delete;
        VulkanBackend& operator=(const VulkanBackend&) = delete;
        VulkanBackend(VulkanBackend&&) noexcept = default;
        VulkanBackend& operator=(VulkanBackend&&) noexcept = default;

        /** @brief Discover a compatible device and construct one shared Slang/Vulkan service. */
        [[nodiscard]] static std::expected<VulkanBackend, VulkanBackendError>
        Create(VulkanBackendConfig config = {}) noexcept;

        /** @brief Construct every compatible physical device in stable DeviceId order. */
        [[nodiscard]] static std::expected<std::vector<VulkanBackend>, VulkanBackendError>
        CreateAll(VulkanBackendSetConfig config = {}) noexcept;

        /** @brief Return whether this value owns a complete compiler and execution service. */
        [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(service_); }

        /** @brief Return the immutable physical-device descriptor. */
        [[nodiscard]] const DeviceDescriptor& Device() const noexcept { return device_; }

        /** @brief Sample device loss, VMA memory availability, and accepted queue depth. */
        [[nodiscard]] DeviceSnapshot Snapshot() const noexcept;

        /** @brief Return the service-scoped health source used to guard planning and submission. */
        [[nodiscard]] const DeviceObserver& Observer() const noexcept { return observer_; }

        /** @brief Return the exact SPIR-V compiler and Vulkan driver compatibility
         * fingerprint. */
        [[nodiscard]] const TargetFingerprint& Target() const noexcept { return target_; }

        /** @brief Return the GPU-local memory location consumed by dispatch. */
        [[nodiscard]] MemoryLocation DeviceLocation() const noexcept { return deviceLocation_; }

        /** @brief Return the host-visible staging memory location used by portable transfers. */
        [[nodiscard]] MemoryLocation HostLocation() const noexcept { return hostLocation_; }

        /** @brief Return the VMA allocator producing GPU-local storage buffers. */
        [[nodiscard]] const MemoryAllocator& DeviceAllocator() const noexcept { return deviceAllocator_; }

        /** @brief Return the VMA allocator producing persistently mapped coherent staging buffers. */
        [[nodiscard]] const MemoryAllocator& HostAllocator() const noexcept { return hostAllocator_; }

        /** @brief Return validated coherent host-mapping authority for portable inter-service bridges. */
        [[nodiscard]] const HostMemoryAccessor& HostAccessor() const noexcept { return hostAccessor_; }

        /** @brief Return host-to-device and device-to-host copy engines in stable identity order. */
        [[nodiscard]] std::span<const TransferEngine> TransferEngines() const noexcept { return transfers_; }

        /** @brief Return the compiler producing validated SPIR-V executable
         * artifacts. */
        [[nodiscard]] const BackendCompiler& Compiler() const noexcept { return compiler_; }

        /** @brief Return the factory producing driver-owned Vulkan compute pipelines.
         */
        [[nodiscard]] const BackendPipelineFactory& PipelineFactory() const noexcept { return pipelineFactory_; }

        /** @brief Return the ordered Vulkan compute/copy queue using DevicePipeline dispatch artifacts. */
        [[nodiscard]] const Queue& ExecutionQueue() const noexcept { return queue_; }

        /**
         * @brief Borrow a persistently mapped coherent byte span from one host-visible Vulkan allocation.
         * @pre The returned span does not outlive @p allocation and is externally synchronized against queue writes.
         */
        [[nodiscard]] std::expected<std::span<std::byte>, VulkanBackendError>
        Map(const Allocation& allocation, uint64_t offset = 0, uint64_t sizeBytes = UINT64_MAX) const noexcept;

        /** @brief Return the backend policy identity used as
         * PipelineStateKey::state. */
        [[nodiscard]] SemanticHash PipelineStateIdentity() const noexcept { return pipelineStateIdentity_; }

    private:
        struct ConstructionInput;

        [[nodiscard]] static std::expected<VulkanBackend, VulkanBackendError>
        CreateForDevice(ConstructionInput input) noexcept;

        VulkanBackend(BackendService service, DeviceObserver observer, DeviceDescriptor device,
                      TargetFingerprint target, MemoryLocation deviceLocation, MemoryLocation hostLocation,
                      MemoryAllocator deviceAllocator, MemoryAllocator hostAllocator, HostMemoryAccessor hostAccessor,
                      std::vector<TransferEngine> transfers, BackendCompiler compiler,
                      BackendPipelineFactory pipelineFactory, Queue queue, SemanticHash pipelineStateIdentity) noexcept
            : service_(std::move(service)),
              observer_(std::move(observer)),
              device_(device),
              target_(target),
              deviceLocation_(deviceLocation),
              hostLocation_(hostLocation),
              deviceAllocator_(std::move(deviceAllocator)),
              hostAllocator_(std::move(hostAllocator)),
              hostAccessor_(std::move(hostAccessor)),
              transfers_(std::move(transfers)),
              compiler_(std::move(compiler)),
              pipelineFactory_(std::move(pipelineFactory)),
              queue_(std::move(queue)),
              pipelineStateIdentity_(pipelineStateIdentity) {}

        BackendService service_{};
        DeviceObserver observer_{};
        DeviceDescriptor device_{};
        TargetFingerprint target_{};
        MemoryLocation deviceLocation_{};
        MemoryLocation hostLocation_{};
        MemoryAllocator deviceAllocator_;
        MemoryAllocator hostAllocator_;
        HostMemoryAccessor hostAccessor_;
        std::vector<TransferEngine> transfers_{};
        BackendCompiler compiler_;
        BackendPipelineFactory pipelineFactory_;
        Queue queue_{};
        SemanticHash pipelineStateIdentity_{};
    };

} // namespace Sora::Compute

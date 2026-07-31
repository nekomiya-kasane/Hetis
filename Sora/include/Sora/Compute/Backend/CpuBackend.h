/**
 * @file CpuBackend.h
 * @brief Portable host CPU backend with runtime compilation, SIMD lowering, and structured bulk execution.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Backend.h>
#include <Sora/Compute/Backend/CpuCodeProvider.h>
#include <Sora/Compute/Backend/DeviceObserver.h>
#include <Sora/Compute/Backend/HostMemoryApi.h>
#include <Sora/Compute/Backend/MemoryApi.h>
#include <Sora/Compute/Backend/Queue.h>
#include <Sora/Compute/Backend/TransferApi.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>
#include <vector>

namespace Sora::Compute {

    inline constexpr size_t kCpuMaximumKernelArguments = 32;
    inline constexpr size_t kCpuMaximumScalarParameters = 32;
    inline constexpr size_t kCpuMaximumInstructions = 256;
    inline constexpr uint8_t kCpuMaximumValueLanes = 4;

    /** @brief Stable failure while constructing or using the built-in CPU backend. */
    enum class CpuBackendErrorCode : uint8_t {
        InvalidConfiguration,
        ResourceConstructionFailed,
        ForeignAllocation,
        RangeOutOfBounds,
        OutOfMemory,
    };

    /** @brief CPU backend failure with the nested resource error or requested byte range. */
    struct CpuBackendError {
        CpuBackendErrorCode code = CpuBackendErrorCode::ResourceConstructionFailed;
        uint64_t detail = 0;

        friend constexpr bool operator==(CpuBackendError, CpuBackendError) noexcept = default;
    };

    /** @brief Runtime policy controlling CPU parallel and data-parallel crossover points. */
    struct CpuBackendConfig {
        uint32_t workerCount = 0;
        uint64_t minimumParallelWorkItems = 0; ///< Zero selects 32K work items per resolved worker.
        uint64_t minimumSimdWorkItems = 64;
        bool enableSimd = true;

        friend constexpr bool operator==(CpuBackendConfig, CpuBackendConfig) noexcept = default;
    };

    /** @brief Monotonic execution counters sampled without synchronizing the queue. */
    struct CpuBackendStatistics {
        uint64_t submittedBatches = 0;
        uint64_t parallelDispatches = 0;
        uint64_t scalarWorkItems = 0;
        uint64_t simdWorkItems = 0;
        uint64_t nativeWorkItems = 0;

        friend constexpr bool operator==(CpuBackendStatistics, CpuBackendStatistics) noexcept = default;
    };

    /**
     * @brief Complete built-in CPU backend resource set for one host process.
     * @details The compute and host locations share one coherence domain but have distinct addressability identities.
     * Coherent alias engines preserve physical-memory identity; copy engines produce asynchronous completion events.
     */
    class CpuBackend final {
    public:
        CpuBackend() = delete;
        CpuBackend(const CpuBackend&) = delete;
        CpuBackend& operator=(const CpuBackend&) = delete;
        CpuBackend(CpuBackend&&) noexcept = default;
        CpuBackend& operator=(CpuBackend&&) noexcept = default;

        /** @brief Discover the host and construct services with an optional target-native code provider. */
        [[nodiscard]] static std::expected<CpuBackend, CpuBackendError> Create(CpuBackendConfig config = {},
                                                                               CpuCodeProvider provider = {}) noexcept;

        /** @brief Return whether this value owns a complete backend service. */
        [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(service_); }

        /** @brief Return the immutable compute-device descriptor. */
        [[nodiscard]] const DeviceDescriptor& Device() const noexcept { return device_; }

        /** @brief Sample dynamic availability, memory pressure, and accepted queue depth. */
        [[nodiscard]] DeviceSnapshot Snapshot() const noexcept;

        /** @brief Return the service-scoped health source used to guard planning and submission. */
        [[nodiscard]] const DeviceObserver& Observer() const noexcept { return observer_; }

        /** @brief Return the exact runtime-compilation target for this process and physical CPU. */
        [[nodiscard]] const TargetFingerprint& Target() const noexcept { return target_; }

        /** @brief Return the backend-addressable compute memory location. */
        [[nodiscard]] MemoryLocation DeviceLocation() const noexcept { return deviceLocation_; }

        /** @brief Return the process-host memory location used for portable upload and download routes. */
        [[nodiscard]] MemoryLocation HostLocation() const noexcept { return hostLocation_; }

        /** @brief Return the allocator producing queue-addressable CPU allocations. */
        [[nodiscard]] const MemoryAllocator& DeviceAllocator() const noexcept { return deviceAllocator_; }

        /** @brief Return the allocator producing host-addressable coherent allocations. */
        [[nodiscard]] const MemoryAllocator& HostAllocator() const noexcept { return hostAllocator_; }

        /** @brief Return validated coherent host-mapping authority for portable inter-service bridges. */
        [[nodiscard]] const HostMemoryAccessor& HostAccessor() const noexcept { return hostAccessor_; }

        /** @brief Return alias and copy engines in deterministic identity order. */
        [[nodiscard]] std::span<const TransferEngine> TransferEngines() const noexcept { return transfers_; }

        /** @brief Return the runtime KernelProgram compiler. */
        [[nodiscard]] const BackendCompiler& Compiler() const noexcept { return compiler_; }

        /** @brief Return the ordered dispatch/copy/fill queue. */
        [[nodiscard]] const Queue& ExecutionQueue() const noexcept { return queue_; }

        /**
         * @brief Borrow a coherent host span from one CPU allocation subrange.
         * @pre The returned span does not outlive @p allocation and is externally synchronized against queue writes.
         */
        [[nodiscard]] std::expected<std::span<std::byte>, CpuBackendError>
        Map(const Allocation& allocation, uint64_t offset = 0, uint64_t sizeBytes = UINT64_MAX) const noexcept;

        /** @brief Sample scalar, SIMD, and parallel path usage. */
        [[nodiscard]] CpuBackendStatistics Statistics() const noexcept;

        /** @brief Reset execution counters after all earlier submissions have reached terminal state. */
        void ResetStatistics() const noexcept;

    private:
        CpuBackend(BackendService service, DeviceObserver observer, DeviceDescriptor device, TargetFingerprint target,
                   MemoryLocation deviceLocation, MemoryLocation hostLocation, MemoryAllocator deviceAllocator,
                   MemoryAllocator hostAllocator, HostMemoryAccessor hostAccessor,
                   std::vector<TransferEngine> transfers, BackendCompiler compiler, Queue queue) noexcept
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
              queue_(std::move(queue)) {}

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
        Queue queue_{};
    };

} // namespace Sora::Compute

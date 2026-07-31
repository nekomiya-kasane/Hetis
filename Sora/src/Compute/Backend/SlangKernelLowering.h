/**
 * @file SlangKernelLowering.h
 * @brief Private KernelProgram-v1 to Slang compute-module lowering boundary.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Backend/VulkanBackend.h>
#include <Sora/Compute/KernelProgram.h>

#include <array>
#include <cstdint>
#include <expected>
#include <string>

namespace Sora::Compute::Detail {

    inline constexpr char kVulkanEntryPoint[] = "sora_kernel";
    inline constexpr uint32_t kVulkanMetadataHeaderSize = 32;
    inline constexpr uint32_t kVulkanBufferMetadataSize = 32;
    inline constexpr uint32_t kInvalidVulkanMetadataOffset = UINT32_MAX;

    using VulkanKernelProgram =
        KernelProgram<kVulkanMaximumKernelArguments, kVulkanMaximumScalarParameters, kVulkanMaximumInstructions>;

    /** @brief Fully determined descriptor, workgroup, and metadata contract emitted
     * into one SPIR-V executable. */
    struct VulkanDispatchAbi {
        std::array<uint32_t, kVulkanMaximumKernelArguments> argumentMetadataOffsets{};
        std::array<uint32_t, 3> localSize{1, 1, 1};
        uint32_t metadataBinding = 0;
        uint32_t metadataSize = 0;
        uint32_t storageBufferCount = 0;

        friend constexpr bool operator==(const VulkanDispatchAbi&, const VulkanDispatchAbi&) noexcept = default;
    };

    /** @brief Generated source and the exact private dispatch ABI encoded by that
     * source. */
    struct SlangKernelSource {
        std::string text{};
        VulkanDispatchAbi dispatchAbi{};
    };

    /** @brief Return whether this backend can preserve the program's scalar and
     * operation semantics. */
    [[nodiscard]] bool SupportsVulkanProgram(const VulkanKernelProgram& program, DeviceFeatureMask features) noexcept;

    /** @brief Generate one self-contained Slang compute module from a validated
     * KernelProgram. */
    [[nodiscard]] std::expected<SlangKernelSource, CompileError>
    LowerKernelProgramToSlang(const VulkanKernelProgram& program, DeviceFeatureMask features) noexcept;

} // namespace Sora::Compute::Detail

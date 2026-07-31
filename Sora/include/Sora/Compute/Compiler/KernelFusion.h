/**
 * @file KernelFusion.h
 * @brief Semantics-preserving composition of canonical elementwise kernel programs.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Compiler/KernelProgramImage.h>

#include <cstdint>
#include <expected>
#include <type_traits>

namespace Sora::Compute {

    /** @brief Stable reason two canonical programs cannot be fused through one intermediate argument. */
    enum class KernelFusionErrorCode : uint8_t {
        InvalidProducer,
        InvalidConsumer,
        IncompatibleDomain,
        IncompatiblePolicy,
        InvalidIntermediate,
        CapacityExceeded,
        OutOfMemory,
    };

    /** @brief Kernel-fusion failure with the relevant argument or nested package error. */
    struct KernelFusionError {
        KernelFusionErrorCode code = KernelFusionErrorCode::InvalidIntermediate;
        uint32_t argument = kInvalidKernelArgumentIndex;
        KernelProgramPackageError package{};

        friend constexpr bool operator==(KernelFusionError, KernelFusionError) noexcept = default;
    };

    /**
     * @brief Inline @p producer into the unique scalar input sourced from @p consumerIntermediate.
     * @details The producer output and consumer intermediate arguments are removed. The returned canonical program
     * retains consumer output semantics and the intersection of both partition contracts.
     */
    [[nodiscard]] std::expected<KernelProgramImage, KernelFusionError>
    FuseElementwiseKernels(KernelProgramView producer, KernelProgramView consumer,
                           KernelArgumentId consumerIntermediate, KernelProgramLimits limits = {}) noexcept;

    static_assert(std::is_trivially_copyable_v<KernelFusionError>);

} // namespace Sora::Compute

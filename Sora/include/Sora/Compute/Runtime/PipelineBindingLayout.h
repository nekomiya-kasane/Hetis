/**
 * @file PipelineBindingLayout.h
 * @brief Stable pipeline-layout identity derived from a kernel argument ABI
 * schema.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/KernelProgram.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace Sora::Compute {

    /** @brief Version of the canonical binding-layout identity schema. */
    inline constexpr uint32_t kPipelineBindingLayoutSchemaVersion = 1;

    /**
     * @brief Hash the immutable binding ABI of @p arguments without incorporating
     * invocation-specific values.
     * @details The identity records signature order, binding kind, access, value
     * type, rank, layout, alignment, and the tightly packed scalar rule. Extents,
     * strides, allocation identities, and launch geometry remain outside this
     * compatibility domain so dynamic invocations do not fragment the pipeline
     * cache.
     */
    [[nodiscard]] constexpr SemanticHash
    PipelineBindingLayoutIdentity(std::span<const KernelArgumentDesc> arguments) noexcept {
        if (arguments.size() > std::numeric_limits<uint32_t>::max()) {
            return {};
        }

        Detail::SemanticHashBuilder builder;
        builder.AppendLittleEndian(uint32_t{0x4C425053U}); // "SPBL" in little-endian byte order.
        builder.AppendLittleEndian(kPipelineBindingLayoutSchemaVersion);
        builder.AppendLittleEndian(static_cast<uint32_t>(arguments.size()));
        builder.AppendLittleEndian(uint8_t{0}); // Tightly packed scalars in signature order.
        for (uint32_t index = 0; index < arguments.size(); ++index) {
            const KernelArgumentDesc& argument = arguments[index];
            builder.AppendLittleEndian(index);
            builder.AppendLittleEndian(argument.kind);
            builder.AppendLittleEndian(argument.access);
            builder.AppendLittleEndian(argument.elementType.scalar);
            builder.AppendLittleEndian(argument.elementType.lanes);
            builder.AppendLittleEndian(argument.rank);
            builder.AppendLittleEndian(argument.layout);
            builder.AppendLittleEndian(argument.minimumAlignment);
        }
        return builder.Finish();
    }

} // namespace Sora::Compute

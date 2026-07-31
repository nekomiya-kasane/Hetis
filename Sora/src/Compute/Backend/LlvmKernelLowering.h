/**
 * @file LlvmKernelLowering.h
 * @brief Private KernelProgram-v1 to LLVM-IR lowering boundary.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Backend/CpuBackend.h>

#include <cstdint>
#include <expected>
#include <string>

namespace Sora::Compute::Detail {

    inline constexpr char kLlvmCpuEntrySymbol[] = "sora_cpu_kernel_entry";

    using LlvmCpuProgram =
        KernelProgram<kCpuMaximumKernelArguments, kCpuMaximumScalarParameters, kCpuMaximumInstructions>;

    /** @brief Host compiler function-sanitizer prefix required by the typed CPU entry ABI. */
    struct LlvmFunctionSanitizerAbi {
        uint32_t signature = 0;
        uint32_t typeHash = 0;

        /** @brief Return whether the host compiler emitted a readable function-sanitizer prefix. */
        [[nodiscard]] constexpr bool Enabled() const noexcept { return signature != 0; }
    };

    /** @brief Lower one validated CPU KernelProgram into target-independent opaque-pointer LLVM IR. */
    [[nodiscard]] std::expected<std::string, CompileError>
    LowerKernelProgramToLlvmIr(const LlvmCpuProgram& program, LlvmFunctionSanitizerAbi sanitizerAbi) noexcept;

} // namespace Sora::Compute::Detail

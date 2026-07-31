/**
 * @file KernelValidation.h
 * @brief Structural and type validation for canonical scalar Kernel IR.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/KernelIR.h>

#include <cstdint>
#include <expected>
#include <limits>
#include <span>

namespace Sora::Compute {

    /** @brief Structural failure in a portable scalar module. */
    enum class KernelIrErrorCode : uint8_t {
        InvalidCount,
        InvalidPolicy,
        InvalidParameter,
        InvalidInstruction,
        InvalidOperand,
        TypeMismatch,
        InvalidResult,
        ArgumentMismatch,
        UnsupportedType,
    };

    /** @brief Precise scalar-IR diagnostic with the failing parameter or instruction index. */
    struct KernelIrError {
        KernelIrErrorCode code = KernelIrErrorCode::InvalidInstruction;
        uint32_t index = kInvalidValueIndex;

        friend constexpr bool operator==(KernelIrError, KernelIrError) noexcept = default;
    };

    namespace Detail {

        [[nodiscard]] constexpr std::expected<void, KernelIrError> ValidateParameter(const ParameterDesc& parameter,
                                                                                     uint32_t index) noexcept {
            if (!ValidParameterKind(parameter.kind) || !ValidAccessMode(parameter.access) ||
                !ValidLayoutKind(parameter.layout) || !parameter.elementType.Valid()) {
                return std::unexpected(KernelIrError{KernelIrErrorCode::InvalidParameter, index});
            }
            if (parameter.kind == ParameterKind::Value &&
                (parameter.rank != 0 || parameter.layout != LayoutKind::Scalar ||
                 parameter.access != AccessMode::Read)) {
                return std::unexpected(KernelIrError{KernelIrErrorCode::InvalidParameter, index});
            }
            if (parameter.kind == ParameterKind::Buffer &&
                (parameter.rank == 0 || parameter.layout == LayoutKind::Scalar)) {
                return std::unexpected(KernelIrError{KernelIrErrorCode::InvalidParameter, index});
            }
            return {};
        }

        template<typename ResultTypeAt>
        [[nodiscard]] constexpr std::expected<void, KernelIrError>
        ValidateInstruction(const Instruction& instruction, std::span<const ParameterDesc> parameters, uint32_t index,
                            ResultTypeAt&& resultTypeAt) noexcept {
            if (!ValidOpCode(instruction.opcode) || !instruction.resultType.Valid() ||
                instruction.operandCount != OperandCount(instruction.opcode)) {
                return std::unexpected(KernelIrError{KernelIrErrorCode::InvalidInstruction, index});
            }
            if (instruction.flags != 0 || !ValidInstructionImmediate(instruction)) {
                return std::unexpected(KernelIrError{KernelIrErrorCode::InvalidInstruction, index});
            }
            if (instruction.opcode == OpCode::Input &&
                (instruction.immediate >= parameters.size() ||
                 parameters[instruction.immediate].elementType != instruction.resultType)) {
                return std::unexpected(KernelIrError{KernelIrErrorCode::InvalidParameter, index});
            }
            for (uint8_t operandIndex = 0; operandIndex < instruction.operandCount; ++operandIndex) {
                const ValueId operand = instruction.operands[operandIndex];
                if (!operand.Valid() || operand.index >= index) {
                    return std::unexpected(KernelIrError{KernelIrErrorCode::InvalidOperand, index});
                }
                if (resultTypeAt(operand.index) != instruction.resultType) {
                    return std::unexpected(KernelIrError{KernelIrErrorCode::TypeMismatch, index});
                }
            }
            for (uint8_t operandIndex = instruction.operandCount; operandIndex < instruction.operands.size();
                 ++operandIndex) {
                if (instruction.operands[operandIndex].Valid()) {
                    return std::unexpected(KernelIrError{KernelIrErrorCode::InvalidOperand, index});
                }
            }
            return {};
        }

    } // namespace Detail

    /** @brief Validate signature, dominance, operand arity, homogeneous types, and result identity. */
    [[nodiscard]] constexpr std::expected<void, KernelIrError> Validate(KernelModuleView module) noexcept {
        if (!ValidFloatingPointMode(module.numericalPolicy.floatingPoint) ||
            !ValidReductionMode(module.numericalPolicy.reduction)) {
            return std::unexpected(KernelIrError{KernelIrErrorCode::InvalidPolicy, kInvalidValueIndex});
        }
        if (module.parameters.size() > std::numeric_limits<uint32_t>::max() ||
            module.instructions.size() > std::numeric_limits<uint32_t>::max()) {
            return std::unexpected(KernelIrError{KernelIrErrorCode::InvalidCount, kInvalidValueIndex});
        }
        for (uint32_t index = 0; index < module.parameters.size(); ++index) {
            if (auto result = Detail::ValidateParameter(module.parameters[index], index); !result) {
                return result;
            }
        }
        for (uint32_t index = 0; index < module.instructions.size(); ++index) {
            if (auto result = Detail::ValidateInstruction(
                    module.instructions[index], module.parameters, index,
                    [module](uint32_t operand) constexpr noexcept { return module.instructions[operand].resultType; });
                !result) {
                return result;
            }
        }
        if (!module.result.Valid() || module.result.index >= module.instructions.size()) {
            return std::unexpected(KernelIrError{KernelIrErrorCode::InvalidResult, module.result.index});
        }
        return {};
    }

    /** @brief Validate one fixed-capacity scalar module through the common representation-independent view. */
    template<size_t ParameterCapacity, size_t InstructionCapacity>
    [[nodiscard]] constexpr std::expected<void, KernelIrError>
    Validate(const KernelModule<ParameterCapacity, InstructionCapacity>& module) noexcept {
        if (module.parameterCount > ParameterCapacity || module.instructionCount > InstructionCapacity) {
            return std::unexpected(KernelIrError{KernelIrErrorCode::InvalidCount, kInvalidValueIndex});
        }
        return Validate(module.View());
    }

} // namespace Sora::Compute

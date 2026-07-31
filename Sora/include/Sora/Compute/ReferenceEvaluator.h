/**
 * @file ReferenceEvaluator.h
 * @brief Validation and scalar reference execution for portable Kernel IR.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelValidation.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace Sora::Compute {

    namespace Detail {

        template<std::floating_point Scalar>
        [[nodiscard]] constexpr Scalar DecodeConstant(uint64_t immediate) noexcept {
            if constexpr (std::same_as<Scalar, float>) {
                return std::bit_cast<float>(static_cast<uint32_t>(immediate));
            } else {
                return std::bit_cast<double>(immediate);
            }
        }

        template<std::floating_point Scalar>
        [[nodiscard]] constexpr Scalar EvaluateInstruction(OpCode opcode, Scalar first, Scalar second,
                                                           Scalar third) noexcept {
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
                    return Scalar{1} / first;
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
                    return std::clamp(first, Scalar{0}, Scalar{1});
                case OpCode::Sign:
                    return static_cast<Scalar>((Scalar{0} < first) - (first < Scalar{0}));
                case OpCode::Input:
                case OpCode::Constant:
                    return {};
            }
            return {};
        }

    } // namespace Detail

    /** @brief Execute a validated homogeneous floating-point Kernel IR module as a scalar correctness oracle. */
    template<std::floating_point Scalar, size_t ParameterCapacity, size_t InstructionCapacity,
             size_t Extent = std::dynamic_extent>
    [[nodiscard]] constexpr std::expected<Scalar, KernelIrError>
    EvaluateReference(const KernelModule<ParameterCapacity, InstructionCapacity>& module,
                      std::span<const Scalar, Extent> args) {
        if (auto valid = Validate(module); !valid) {
            return std::unexpected(valid.error());
        }
        if (args.size() != module.parameterCount) {
            return std::unexpected(KernelIrError{KernelIrErrorCode::ArgumentMismatch, kInvalidValueIndex});
        }
        constexpr ScalarKind expectedKind = ScalarKindOf<Scalar>();
        for (const ParameterDesc& parameter : module.Parameters()) {
            if (parameter.kind != ParameterKind::Value || parameter.elementType != ValueType{expectedKind, 1}) {
                return std::unexpected(KernelIrError{KernelIrErrorCode::UnsupportedType, kInvalidValueIndex});
            }
        }

        std::array<Scalar, InstructionCapacity> values{};
        for (uint32_t index = 0; index < module.instructionCount; ++index) {
            const Instruction& instruction = module.instructions[index];
            if (instruction.opcode == OpCode::Input) {
                values[index] = args[instruction.immediate];
                continue;
            }
            if (instruction.opcode == OpCode::Constant) {
                if (instruction.resultType != ValueType{expectedKind, 1}) {
                    return std::unexpected(KernelIrError{KernelIrErrorCode::UnsupportedType, index});
                }
                values[index] = Detail::DecodeConstant<Scalar>(instruction.immediate);
                continue;
            }
            const Scalar first = values[instruction.operands[0].index];
            const Scalar second = instruction.operandCount > 1 ? values[instruction.operands[1].index] : Scalar{};
            const Scalar third = instruction.operandCount > 2 ? values[instruction.operands[2].index] : Scalar{};
            values[index] = Detail::EvaluateInstruction(instruction.opcode, first, second, third);
        }
        return values[module.result.index];
    }

} // namespace Sora::Compute

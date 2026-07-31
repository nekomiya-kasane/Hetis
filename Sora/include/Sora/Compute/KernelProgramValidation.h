/**
 * @file KernelProgramValidation.h
 * @brief Semantic validation for canonical elementwise kernel programs.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/KernelProgram.h>
#include <Sora/Compute/KernelValidation.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>

namespace Sora::Compute {

    /** @brief Stable semantic failure in a structured kernel program. */
    enum class KernelProgramErrorCode : uint8_t {
        InvalidCount,
        InvalidPattern,
        InvalidDomain,
        InvalidPartition,
        InvalidScalarModule,
        InvalidArgument,
        InvalidInput,
        DuplicateInput,
        UnusedArgument,
        InvalidOutput,
        TypeMismatch,
    };

    /** @brief Kernel-program diagnostic with primary and related indices or nested error code. */
    struct KernelProgramError {
        KernelProgramErrorCode code = KernelProgramErrorCode::InvalidPattern;
        uint32_t index = kInvalidKernelArgumentIndex;
        uint32_t related = kInvalidKernelArgumentIndex;

        friend constexpr bool operator==(KernelProgramError, KernelProgramError) noexcept = default;
    };

    namespace Detail {

        [[nodiscard]] constexpr bool ValidProgramArgument(const KernelArgumentDesc& argument,
                                                          IndexDomain domain) noexcept {
            if (!argument.elementType.Valid() || !std::has_single_bit(argument.minimumAlignment)) {
                return false;
            }
            if (argument.kind == ParameterKind::Value) {
                return argument.access == AccessMode::Read && argument.rank == 0 &&
                       argument.layout == LayoutKind::Scalar && argument.minimumAlignment == 1 &&
                       !argument.aliasClass.Known();
            }
            return argument.kind == ParameterKind::Buffer && argument.rank == domain.rank &&
                   argument.layout != LayoutKind::Scalar;
        }

        [[nodiscard]] constexpr bool Readable(AccessMode access) noexcept {
            return access == AccessMode::Read || access == AccessMode::ReadWrite;
        }

        [[nodiscard]] constexpr bool Writable(AccessMode access) noexcept {
            return access == AccessMode::Write || access == AccessMode::ReadWrite;
        }

        /** @brief Validate representation-independent elementwise program semantics using caller-owned scratch. */
        [[nodiscard]] constexpr std::expected<void, KernelProgramError>
        ValidateKernelProgramSemantics(KernelPattern pattern, IndexDomain domain, PartitionContract partition,
                                       std::span<const KernelArgumentDesc> arguments,
                                       std::span<const ParameterDesc> scalarParameters,
                                       std::span<const ScalarInputBinding> inputs, ValueType scalarResultType,
                                       KernelArgumentId outputId, std::span<uint8_t> useCounts) noexcept {
            if (arguments.empty() || arguments.size() > std::numeric_limits<uint32_t>::max() ||
                scalarParameters.size() != inputs.size() || useCounts.size() < arguments.size()) {
                return std::unexpected(KernelProgramError{KernelProgramErrorCode::InvalidCount});
            }
            if (pattern != KernelPattern::Elementwise) {
                return std::unexpected(KernelProgramError{KernelProgramErrorCode::InvalidPattern});
            }
            if (!domain.Valid()) {
                return std::unexpected(KernelProgramError{KernelProgramErrorCode::InvalidDomain});
            }
            if (!partition.Valid(domain)) {
                return std::unexpected(KernelProgramError{KernelProgramErrorCode::InvalidPartition});
            }
            if (!outputId.Valid() || outputId.index >= arguments.size()) {
                return std::unexpected(KernelProgramError{KernelProgramErrorCode::InvalidOutput, outputId.index});
            }
            for (uint32_t argumentIndex = 0; argumentIndex < arguments.size(); ++argumentIndex) {
                if (!ValidProgramArgument(arguments[argumentIndex], domain)) {
                    return std::unexpected(KernelProgramError{KernelProgramErrorCode::InvalidArgument, argumentIndex});
                }
            }

            const KernelArgumentDesc& output = arguments[outputId.index];
            if (output.kind != ParameterKind::Buffer || !Writable(output.access) ||
                output.elementType != scalarResultType) {
                return std::unexpected(KernelProgramError{KernelProgramErrorCode::InvalidOutput, outputId.index});
            }

            std::ranges::fill(useCounts.first(arguments.size()), uint8_t{0});
            for (uint32_t inputIndex = 0; inputIndex < inputs.size(); ++inputIndex) {
                const ParameterDesc& scalarParameter = scalarParameters[inputIndex];
                if (scalarParameter.kind != ParameterKind::Value || scalarParameter.access != AccessMode::Read ||
                    scalarParameter.rank != 0 || scalarParameter.layout != LayoutKind::Scalar) {
                    return std::unexpected(KernelProgramError{KernelProgramErrorCode::InvalidInput, inputIndex});
                }
                const ScalarInputBinding input = inputs[inputIndex];
                if (input.kind == ScalarInputKind::LogicalIndex) {
                    if (input.sourceIndex >= domain.rank ||
                        scalarParameter.elementType != ValueType{ScalarKind::UInt64, 1}) {
                        return std::unexpected(KernelProgramError{KernelProgramErrorCode::InvalidInput, inputIndex});
                    }
                    continue;
                }
                if (input.kind != ScalarInputKind::Argument || input.sourceIndex >= arguments.size()) {
                    return std::unexpected(KernelProgramError{KernelProgramErrorCode::InvalidInput, inputIndex});
                }
                const KernelArgumentDesc& argument = arguments[input.sourceIndex];
                if (!Readable(argument.access) || argument.elementType != scalarParameter.elementType) {
                    return std::unexpected(
                        KernelProgramError{KernelProgramErrorCode::TypeMismatch, inputIndex, input.sourceIndex});
                }
                if (++useCounts[input.sourceIndex] != 1) {
                    return std::unexpected(
                        KernelProgramError{KernelProgramErrorCode::DuplicateInput, inputIndex, input.sourceIndex});
                }
            }

            for (uint32_t argumentIndex = 0; argumentIndex < arguments.size(); ++argumentIndex) {
                const KernelArgumentDesc& argument = arguments[argumentIndex];
                const bool isOutput = argumentIndex == outputId.index;
                if (Writable(argument.access) && !isOutput) {
                    return std::unexpected(KernelProgramError{KernelProgramErrorCode::InvalidOutput, argumentIndex});
                }
                const uint8_t expectedUses = Readable(argument.access) ? 1 : 0;
                if (useCounts[argumentIndex] != expectedUses) {
                    return std::unexpected(
                        KernelProgramError{KernelProgramErrorCode::UnusedArgument, argumentIndex, expectedUses});
                }
            }
            return {};
        }

    } // namespace Detail

    /** @brief Validate one representation-independent elementwise program using caller-owned scratch. */
    [[nodiscard]] constexpr std::expected<void, KernelProgramError>
    ValidateKernelProgram(KernelProgramView program, std::span<uint8_t> useCounts) noexcept {
        if (auto scalar = Validate(program.scalar); !scalar) {
            return std::unexpected(KernelProgramError{KernelProgramErrorCode::InvalidScalarModule, scalar.error().index,
                                                      static_cast<uint32_t>(scalar.error().code)});
        }
        return Detail::ValidateKernelProgramSemantics(program.pattern, program.domain, program.partition,
                                                      program.arguments, program.scalar.parameters, program.inputs,
                                                      program.scalar.ResultType(), program.output, useCounts);
    }

    /**
     * @brief Validate elementwise signature, scalar input mapping, output store, effects, and partition contract.
     * @details KernelProgram v1 intentionally rejects reductions, scans, stencils, multiple stores, and arbitrary
     * memory access because the required collective and structured-region semantics are not present in scalar IR.
     */
    template<size_t ArgumentCapacity, size_t ScalarParameterCapacity, size_t InstructionCapacity>
    [[nodiscard]] constexpr std::expected<void, KernelProgramError> ValidateKernelProgram(
        const KernelProgram<ArgumentCapacity, ScalarParameterCapacity, InstructionCapacity>& program) noexcept {
        if (program.argumentCount > ArgumentCapacity || program.inputCount > ScalarParameterCapacity) {
            return std::unexpected(KernelProgramError{KernelProgramErrorCode::InvalidCount});
        }
        std::array<uint8_t, ArgumentCapacity> useCounts{};
        return ValidateKernelProgram(program.View(), useCounts);
    }

} // namespace Sora::Compute

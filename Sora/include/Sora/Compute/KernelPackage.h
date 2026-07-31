/**
 * @file KernelPackage.h
 * @brief Allocation-free decoding of canonical portable Kernel IR packages.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/KernelIR.h>
#include <Sora/Compute/WireReader.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace Sora::Compute {

    /** @brief Canonical package decoding failure. */
    enum class KernelPackageErrorCode : uint8_t {
        Truncated,
        InvalidMagic,
        UnsupportedVersion,
        CapacityExceeded,
        InvalidEncoding,
        TrailingBytes,
    };

    /** @brief Package error with the byte offset at which decoding failed. */
    struct KernelPackageError {
        KernelPackageErrorCode code = KernelPackageErrorCode::InvalidEncoding;
        size_t offset = 0;

        friend constexpr bool operator==(KernelPackageError, KernelPackageError) noexcept = default;
    };

    namespace Detail {

        template<typename E>
        [[nodiscard]] constexpr bool EnumInRange(E value, E first, E last) noexcept {
            return value >= first && value <= last;
        }

        [[nodiscard]] constexpr bool Valid(ValueType type) noexcept {
            return EnumInRange(type.scalar, ScalarKind::Bool, ScalarKind::Float64) && type.lanes != 0;
        }

    } // namespace Detail

    /**
     * @brief Decode a canonical SKIR v1 byte sequence into a caller-selected fixed-capacity module.
     * @details This function validates wire structure and enum domains. Call @ref Validate afterwards to prove SSA
     * dominance, operand arity, signature consistency, and result validity.
     */
    template<size_t ParameterCapacity, size_t InstructionCapacity>
    [[nodiscard]] constexpr std::expected<KernelModule<ParameterCapacity, InstructionCapacity>, KernelPackageError>
    DecodeKernelPackage(std::span<const std::byte> bytes) noexcept {
        Detail::WireReader reader{bytes};
        auto magic = reader.Read<uint32_t>();
        auto major = reader.Read<uint16_t>();
        auto minor = reader.Read<uint16_t>();
        auto floatingPoint = reader.Read<FloatingPointMode>();
        auto reduction = reader.Read<ReductionMode>();
        auto headerReserved = reader.Read<uint16_t>();
        auto parameterCount = reader.Read<uint32_t>();
        auto instructionCount = reader.Read<uint32_t>();
        auto resultIndex = reader.Read<uint32_t>();
        if (!magic || !major || !minor || !floatingPoint || !reduction || !headerReserved || !parameterCount ||
            !instructionCount || !resultIndex) {
            return std::unexpected(KernelPackageError{KernelPackageErrorCode::Truncated, reader.Offset()});
        }
        if (*magic != kKernelIrMagic) {
            return std::unexpected(KernelPackageError{KernelPackageErrorCode::InvalidMagic, 0});
        }
        if (*major != kKernelIrMajorVersion || *minor > kKernelIrMinorVersion) {
            return std::unexpected(KernelPackageError{KernelPackageErrorCode::UnsupportedVersion, sizeof(uint32_t)});
        }
        if (*headerReserved != 0 ||
            !Detail::EnumInRange(*floatingPoint, FloatingPointMode::Strict, FloatingPointMode::Fast) ||
            !Detail::EnumInRange(*reduction, ReductionMode::Deterministic, ReductionMode::Reorderable)) {
            return std::unexpected(KernelPackageError{KernelPackageErrorCode::InvalidEncoding, 8});
        }
        if (*parameterCount > ParameterCapacity || *instructionCount > InstructionCapacity) {
            return std::unexpected(KernelPackageError{KernelPackageErrorCode::CapacityExceeded, 12});
        }

        KernelModule<ParameterCapacity, InstructionCapacity> module{};
        module.numericalPolicy = {.floatingPoint = *floatingPoint, .reduction = *reduction};
        module.parameterCount = *parameterCount;
        module.instructionCount = *instructionCount;
        module.result = ValueId{*resultIndex};

        for (uint32_t index = 0; index < module.parameterCount; ++index) {
            auto kind = reader.Read<ParameterKind>();
            auto access = reader.Read<AccessMode>();
            auto scalar = reader.Read<ScalarKind>();
            auto lanes = reader.Read<uint8_t>();
            auto rank = reader.Read<uint8_t>();
            auto layout = reader.Read<LayoutKind>();
            auto reserved = reader.Read<uint16_t>();
            if (!kind || !access || !scalar || !lanes || !rank || !layout || !reserved) {
                return std::unexpected(KernelPackageError{KernelPackageErrorCode::Truncated, reader.Offset()});
            }
            const ValueType type{.scalar = *scalar, .lanes = *lanes};
            if (*reserved != 0 || !Detail::EnumInRange(*kind, ParameterKind::Value, ParameterKind::Buffer) ||
                !Detail::EnumInRange(*access, AccessMode::Read, AccessMode::ReadWrite) ||
                !Detail::EnumInRange(*layout, LayoutKind::Scalar, LayoutKind::StructureOfArrays) ||
                !Detail::Valid(type)) {
                return std::unexpected(KernelPackageError{KernelPackageErrorCode::InvalidEncoding, reader.Offset()});
            }
            module.parameters[index] = {
                .kind = *kind,
                .access = *access,
                .elementType = type,
                .rank = *rank,
                .layout = *layout,
            };
        }

        for (uint32_t index = 0; index < module.instructionCount; ++index) {
            auto opcode = reader.Read<OpCode>();
            auto scalar = reader.Read<ScalarKind>();
            auto lanes = reader.Read<uint8_t>();
            auto operandCount = reader.Read<uint8_t>();
            std::array<ValueId, 3> operands{};
            for (ValueId& operand : operands) {
                auto operandIndex = reader.Read<uint32_t>();
                if (!operandIndex) {
                    return std::unexpected(
                        KernelPackageError{KernelPackageErrorCode::Truncated, operandIndex.error().offset});
                }
                operand = ValueId{*operandIndex};
            }
            auto immediate = reader.Read<uint64_t>();
            auto flags = reader.Read<uint32_t>();
            auto reserved = reader.Read<uint32_t>();
            if (!opcode || !scalar || !lanes || !operandCount || !immediate || !flags || !reserved) {
                return std::unexpected(KernelPackageError{KernelPackageErrorCode::Truncated, reader.Offset()});
            }
            const Instruction instruction{
                .opcode = *opcode,
                .resultType = {.scalar = *scalar, .lanes = *lanes},
                .operandCount = *operandCount,
                .operands = operands,
                .immediate = *immediate,
                .flags = *flags,
            };
            if (*reserved != 0 || !Detail::EnumInRange(*opcode, OpCode::Input, OpCode::Sign) ||
                *operandCount != OperandCount(*opcode) || *flags != 0 || !Detail::Valid(instruction.resultType) ||
                !ValidInstructionImmediate(instruction)) {
                return std::unexpected(KernelPackageError{KernelPackageErrorCode::InvalidEncoding, reader.Offset()});
            }
            module.instructions[index] = instruction;
        }
        if (!reader.AtEnd()) {
            return std::unexpected(KernelPackageError{KernelPackageErrorCode::TrailingBytes, reader.Offset()});
        }
        return module;
    }

} // namespace Sora::Compute

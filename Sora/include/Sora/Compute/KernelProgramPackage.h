/**
 * @file KernelProgramPackage.h
 * @brief Allocation-free decoding of canonical elementwise kernel-program packages.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/KernelPackage.h>
#include <Sora/Compute/KernelProgramValidation.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace Sora::Compute {

    /** @brief Stable failure while decoding or canonicalizing a kernel-program package. */
    enum class KernelProgramPackageErrorCode : uint8_t {
        Truncated,
        InvalidMagic,
        UnsupportedVersion,
        CapacityExceeded,
        InvalidEncoding,
        InvalidScalarModule,
        InvalidSemantics,
        TrailingBytes,
        NonCanonicalEncoding,
        OutOfMemory,
    };

    /** @brief Program-package error with byte offset and nested error identity. */
    struct KernelProgramPackageError {
        KernelProgramPackageErrorCode code = KernelProgramPackageErrorCode::InvalidEncoding;
        size_t offset = 0;
        uint32_t detail = 0;

        friend constexpr bool operator==(KernelProgramPackageError, KernelProgramPackageError) noexcept = default;
    };

    /** @brief Decode, structurally validate, and semantically prove one canonical KernelProgram v1 package. */
    template<size_t ArgumentCapacity, size_t ScalarParameterCapacity, size_t InstructionCapacity>
    [[nodiscard]] constexpr std::expected<KernelProgram<ArgumentCapacity, ScalarParameterCapacity, InstructionCapacity>,
                                          KernelProgramPackageError>
    DecodeKernelProgramPackage(std::span<const std::byte> bytes) noexcept {
        Detail::WireReader reader{bytes};
        auto magic = reader.Read<uint32_t>();
        auto major = reader.Read<uint16_t>();
        auto minor = reader.Read<uint16_t>();
        auto pattern = reader.Read<KernelPattern>();
        auto rank = reader.Read<uint8_t>();
        auto partitionKind = reader.Read<PartitionKind>();
        auto splittableAxisMask = reader.Read<uint8_t>();
        auto argumentCount = reader.Read<uint32_t>();
        auto inputCount = reader.Read<uint32_t>();
        auto outputIndex = reader.Read<uint32_t>();
        auto scalarByteSize = reader.Read<uint32_t>();
        if (!magic || !major || !minor || !pattern || !rank || !partitionKind || !splittableAxisMask ||
            !argumentCount || !inputCount || !outputIndex || !scalarByteSize) {
            return std::unexpected(
                KernelProgramPackageError{KernelProgramPackageErrorCode::Truncated, reader.Offset()});
        }
        if (*magic != kKernelProgramMagic) {
            return std::unexpected(KernelProgramPackageError{KernelProgramPackageErrorCode::InvalidMagic, 0});
        }
        if (*major != kKernelProgramMajorVersion || *minor > kKernelProgramMinorVersion) {
            return std::unexpected(
                KernelProgramPackageError{KernelProgramPackageErrorCode::UnsupportedVersion, sizeof(uint32_t)});
        }
        if (*pattern != KernelPattern::Elementwise ||
            (*partitionKind != PartitionKind::Indivisible && *partitionKind != PartitionKind::Elementwise)) {
            return std::unexpected(KernelProgramPackageError{KernelProgramPackageErrorCode::InvalidEncoding, 8});
        }
        if (*argumentCount > ArgumentCapacity || *inputCount > ScalarParameterCapacity) {
            return std::unexpected(KernelProgramPackageError{KernelProgramPackageErrorCode::CapacityExceeded, 12});
        }

        KernelProgram<ArgumentCapacity, ScalarParameterCapacity, InstructionCapacity> program{};
        program.pattern = *pattern;
        program.domain.rank = *rank;
        program.partition = {.kind = *partitionKind, .splittableAxisMask = *splittableAxisMask};
        program.argumentCount = *argumentCount;
        program.inputCount = *inputCount;
        program.output = {*outputIndex};
        for (uint64_t& extent : program.domain.staticExtents) {
            auto value = reader.Read<uint64_t>();
            if (!value) {
                return std::unexpected(
                    KernelProgramPackageError{KernelProgramPackageErrorCode::Truncated, value.error().offset});
            }
            extent = *value;
        }

        for (uint32_t index = 0; index < program.argumentCount; ++index) {
            auto kind = reader.Read<ParameterKind>();
            auto access = reader.Read<AccessMode>();
            auto scalar = reader.Read<ScalarKind>();
            auto lanes = reader.Read<uint8_t>();
            auto argumentRank = reader.Read<uint8_t>();
            auto layout = reader.Read<LayoutKind>();
            auto reserved = reader.Read<uint16_t>();
            auto minimumAlignment = reader.Read<uint32_t>();
            auto aliasClass = reader.Read<uint32_t>();
            if (!kind || !access || !scalar || !lanes || !argumentRank || !layout || !reserved || !minimumAlignment ||
                !aliasClass) {
                return std::unexpected(
                    KernelProgramPackageError{KernelProgramPackageErrorCode::Truncated, reader.Offset()});
            }
            const ValueType type{.scalar = *scalar, .lanes = *lanes};
            if (*reserved != 0 || !Detail::EnumInRange(*kind, ParameterKind::Value, ParameterKind::Buffer) ||
                !Detail::EnumInRange(*access, AccessMode::Read, AccessMode::ReadWrite) ||
                !Detail::EnumInRange(*layout, LayoutKind::Scalar, LayoutKind::StructureOfArrays) ||
                !Detail::Valid(type)) {
                return std::unexpected(
                    KernelProgramPackageError{KernelProgramPackageErrorCode::InvalidEncoding, reader.Offset()});
            }
            program.arguments[index] = {
                .kind = *kind,
                .access = *access,
                .elementType = type,
                .rank = *argumentRank,
                .layout = *layout,
                .minimumAlignment = *minimumAlignment,
                .aliasClass = {*aliasClass},
            };
        }

        for (uint32_t index = 0; index < program.inputCount; ++index) {
            auto kind = reader.Read<ScalarInputKind>();
            auto reserved8 = reader.Read<uint8_t>();
            auto reserved16 = reader.Read<uint16_t>();
            auto sourceIndex = reader.Read<uint32_t>();
            if (!kind || !reserved8 || !reserved16 || !sourceIndex) {
                return std::unexpected(
                    KernelProgramPackageError{KernelProgramPackageErrorCode::Truncated, reader.Offset()});
            }
            if ((*kind != ScalarInputKind::Argument && *kind != ScalarInputKind::LogicalIndex) || *reserved8 != 0 ||
                *reserved16 != 0) {
                return std::unexpected(
                    KernelProgramPackageError{KernelProgramPackageErrorCode::InvalidEncoding, reader.Offset()});
            }
            program.inputs[index] = {.kind = *kind, .sourceIndex = *sourceIndex};
        }

        const size_t scalarOffset = reader.Offset();
        auto scalarBytes = reader.ReadBytes(*scalarByteSize);
        if (!scalarBytes) {
            return std::unexpected(
                KernelProgramPackageError{KernelProgramPackageErrorCode::Truncated, scalarBytes.error().offset});
        }
        auto scalar = DecodeKernelPackage<ScalarParameterCapacity, InstructionCapacity>(*scalarBytes);
        if (!scalar) {
            return std::unexpected(KernelProgramPackageError{
                KernelProgramPackageErrorCode::InvalidScalarModule,
                scalarOffset + scalar.error().offset,
                static_cast<uint32_t>(scalar.error().code),
            });
        }
        if (!reader.AtEnd()) {
            return std::unexpected(
                KernelProgramPackageError{KernelProgramPackageErrorCode::TrailingBytes, reader.Offset()});
        }
        program.scalar = *scalar;
        if (auto semantic = ValidateKernelProgram(program); !semantic) {
            return std::unexpected(KernelProgramPackageError{
                KernelProgramPackageErrorCode::InvalidSemantics,
                0,
                static_cast<uint32_t>(semantic.error().code),
            });
        }
        return program;
    }

} // namespace Sora::Compute

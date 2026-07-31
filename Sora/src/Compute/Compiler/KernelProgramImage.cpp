/**
 * @file KernelProgramImage.cpp
 * @brief Dynamic decoding and canonicalization of KernelProgram v1.
 * @ingroup Compute
 */

#include <Sora/Compute/Compiler/KernelProgramImage.h>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace Sora::Compute {
    namespace {

        [[nodiscard]] constexpr KernelProgramPackageError Failure(KernelProgramPackageErrorCode code, size_t offset = 0,
                                                                  uint32_t detail = 0) noexcept {
            return {.code = code, .offset = offset, .detail = detail};
        }

        template<std::unsigned_integral T>
        void AppendLittleEndian(std::vector<std::byte>& bytes, T value) {
            for (size_t index = 0; index < sizeof(T); ++index) {
                bytes.push_back(std::byte((value >> (index * 8U)) & T{0xFF}));
            }
        }

        template<typename E>
            requires std::is_enum_v<E>
        void AppendLittleEndian(std::vector<std::byte>& bytes, E value) {
            using Representation = std::make_unsigned_t<std::underlying_type_t<E>>;
            AppendLittleEndian(bytes, static_cast<Representation>(value));
        }

        [[nodiscard]] std::expected<std::vector<std::byte>, KernelProgramPackageError>
        Encode(KernelProgramView program, KernelProgramLimits limits) noexcept {
            const uint64_t scalarSize = Detail::kKernelIrHeaderWireSize +
                                        uint64_t{program.scalar.parameters.size()} * Detail::kParameterWireSize +
                                        uint64_t{program.scalar.instructions.size()} * Detail::kInstructionWireSize;
            const uint64_t totalSize = KernelProgram<1, 0, 1>::kHeaderWireSize +
                                       uint64_t{program.arguments.size()} * KernelProgram<1, 0, 1>::kArgumentWireSize +
                                       uint64_t{program.inputs.size()} * KernelProgram<1, 1, 1>::kInputBindingWireSize +
                                       scalarSize;
            if (!limits.Valid() || totalSize > limits.maximumBytes || totalSize > std::numeric_limits<size_t>::max() ||
                scalarSize > std::numeric_limits<uint32_t>::max()) {
                return std::unexpected(Failure(KernelProgramPackageErrorCode::CapacityExceeded));
            }

            try {
                std::vector<std::byte> bytes;
                bytes.reserve(static_cast<size_t>(totalSize));
                AppendLittleEndian(bytes, kKernelProgramMagic);
                AppendLittleEndian(bytes, kKernelProgramMajorVersion);
                AppendLittleEndian(bytes, kKernelProgramMinorVersion);
                AppendLittleEndian(bytes, program.pattern);
                AppendLittleEndian(bytes, program.domain.rank);
                AppendLittleEndian(bytes, program.partition.kind);
                AppendLittleEndian(bytes, program.partition.splittableAxisMask);
                AppendLittleEndian(bytes, static_cast<uint32_t>(program.arguments.size()));
                AppendLittleEndian(bytes, static_cast<uint32_t>(program.inputs.size()));
                AppendLittleEndian(bytes, program.output.index);
                AppendLittleEndian(bytes, static_cast<uint32_t>(scalarSize));
                for (const uint64_t extent : program.domain.staticExtents) {
                    AppendLittleEndian(bytes, extent);
                }
                for (const KernelArgumentDesc& argument : program.arguments) {
                    AppendLittleEndian(bytes, argument.kind);
                    AppendLittleEndian(bytes, argument.access);
                    AppendLittleEndian(bytes, argument.elementType.scalar);
                    AppendLittleEndian(bytes, argument.elementType.lanes);
                    AppendLittleEndian(bytes, argument.rank);
                    AppendLittleEndian(bytes, argument.layout);
                    AppendLittleEndian(bytes, uint16_t{0});
                    AppendLittleEndian(bytes, argument.minimumAlignment);
                    AppendLittleEndian(bytes, argument.aliasClass.value);
                }
                for (const ScalarInputBinding input : program.inputs) {
                    AppendLittleEndian(bytes, input.kind);
                    AppendLittleEndian(bytes, uint8_t{0});
                    AppendLittleEndian(bytes, uint16_t{0});
                    AppendLittleEndian(bytes, input.sourceIndex);
                }

                AppendLittleEndian(bytes, kKernelIrMagic);
                AppendLittleEndian(bytes, kKernelIrMajorVersion);
                AppendLittleEndian(bytes, kKernelIrMinorVersion);
                AppendLittleEndian(bytes, program.scalar.numericalPolicy.floatingPoint);
                AppendLittleEndian(bytes, program.scalar.numericalPolicy.reduction);
                AppendLittleEndian(bytes, uint16_t{0});
                AppendLittleEndian(bytes, static_cast<uint32_t>(program.scalar.parameters.size()));
                AppendLittleEndian(bytes, static_cast<uint32_t>(program.scalar.instructions.size()));
                AppendLittleEndian(bytes, program.scalar.result.index);
                for (const ParameterDesc& parameter : program.scalar.parameters) {
                    AppendLittleEndian(bytes, parameter.kind);
                    AppendLittleEndian(bytes, parameter.access);
                    AppendLittleEndian(bytes, parameter.elementType.scalar);
                    AppendLittleEndian(bytes, parameter.elementType.lanes);
                    AppendLittleEndian(bytes, parameter.rank);
                    AppendLittleEndian(bytes, parameter.layout);
                    AppendLittleEndian(bytes, uint16_t{0});
                }
                for (const Instruction& instruction : program.scalar.instructions) {
                    AppendLittleEndian(bytes, instruction.opcode);
                    AppendLittleEndian(bytes, instruction.resultType.scalar);
                    AppendLittleEndian(bytes, instruction.resultType.lanes);
                    AppendLittleEndian(bytes, instruction.operandCount);
                    for (const ValueId operand : instruction.operands) {
                        AppendLittleEndian(bytes, operand.index);
                    }
                    AppendLittleEndian(bytes, instruction.immediate);
                    AppendLittleEndian(bytes, instruction.flags);
                    AppendLittleEndian(bytes, uint32_t{0});
                }
                return bytes;
            } catch (const std::length_error&) {
                return std::unexpected(Failure(KernelProgramPackageErrorCode::CapacityExceeded));
            } catch (const std::bad_alloc&) {
                return std::unexpected(Failure(KernelProgramPackageErrorCode::OutOfMemory));
            }
        }

        [[nodiscard]] bool FitsLimits(const KernelProgramImageStorage& storage, KernelProgramLimits limits) noexcept {
            return limits.Valid() && storage.arguments.size() <= limits.maximumArguments &&
                   storage.inputs.size() <= limits.maximumScalarParameters &&
                   storage.scalarParameters.size() <= limits.maximumScalarParameters &&
                   storage.instructions.size() <= limits.maximumInstructions &&
                   storage.arguments.size() <= std::numeric_limits<uint32_t>::max() &&
                   storage.inputs.size() <= std::numeric_limits<uint32_t>::max() &&
                   storage.scalarParameters.size() <= std::numeric_limits<uint32_t>::max() &&
                   storage.instructions.size() <= std::numeric_limits<uint32_t>::max();
        }

        [[nodiscard]] std::expected<void, KernelProgramPackageError>
        DecodeScalar(std::span<const std::byte> bytes, KernelProgramImageStorage& storage, KernelProgramLimits limits) {
            Detail::WireReader reader{bytes};
            const auto magic = reader.Read<uint32_t>();
            const auto major = reader.Read<uint16_t>();
            const auto minor = reader.Read<uint16_t>();
            const auto floatingPoint = reader.Read<FloatingPointMode>();
            const auto reduction = reader.Read<ReductionMode>();
            const auto reserved = reader.Read<uint16_t>();
            const auto parameterCount = reader.Read<uint32_t>();
            const auto instructionCount = reader.Read<uint32_t>();
            const auto result = reader.Read<uint32_t>();
            if (!magic || !major || !minor || !floatingPoint || !reduction || !reserved || !parameterCount ||
                !instructionCount || !result) {
                return std::unexpected(Failure(KernelProgramPackageErrorCode::InvalidScalarModule, reader.Offset(),
                                               static_cast<uint32_t>(KernelPackageErrorCode::Truncated)));
            }
            if (*magic != kKernelIrMagic) {
                return std::unexpected(Failure(KernelProgramPackageErrorCode::InvalidScalarModule, 0,
                                               static_cast<uint32_t>(KernelPackageErrorCode::InvalidMagic)));
            }
            if (*major != kKernelIrMajorVersion || *minor > kKernelIrMinorVersion) {
                return std::unexpected(Failure(KernelProgramPackageErrorCode::InvalidScalarModule, sizeof(uint32_t),
                                               static_cast<uint32_t>(KernelPackageErrorCode::UnsupportedVersion)));
            }
            if (*reserved != 0 || !ValidFloatingPointMode(*floatingPoint) || !ValidReductionMode(*reduction)) {
                return std::unexpected(Failure(KernelProgramPackageErrorCode::InvalidScalarModule, 8,
                                               static_cast<uint32_t>(KernelPackageErrorCode::InvalidEncoding)));
            }
            if (*parameterCount > limits.maximumScalarParameters || *instructionCount > limits.maximumInstructions) {
                return std::unexpected(Failure(KernelProgramPackageErrorCode::CapacityExceeded, 12));
            }
            const uint64_t requiredBytes = Detail::kKernelIrHeaderWireSize +
                                           uint64_t{*parameterCount} * Detail::kParameterWireSize +
                                           uint64_t{*instructionCount} * Detail::kInstructionWireSize;
            if (requiredBytes > bytes.size()) {
                return std::unexpected(Failure(KernelProgramPackageErrorCode::InvalidScalarModule, bytes.size(),
                                               static_cast<uint32_t>(KernelPackageErrorCode::Truncated)));
            }

            storage.numericalPolicy = {.floatingPoint = *floatingPoint, .reduction = *reduction};
            storage.scalarParameters.reserve(*parameterCount);
            storage.instructions.reserve(*instructionCount);
            for (uint32_t index = 0; index < *parameterCount; ++index) {
                const auto kind = reader.Read<ParameterKind>();
                const auto access = reader.Read<AccessMode>();
                const auto scalar = reader.Read<ScalarKind>();
                const auto lanes = reader.Read<uint8_t>();
                const auto rank = reader.Read<uint8_t>();
                const auto layout = reader.Read<LayoutKind>();
                const auto parameterReserved = reader.Read<uint16_t>();
                if (!kind || !access || !scalar || !lanes || !rank || !layout || !parameterReserved) {
                    return std::unexpected(Failure(KernelProgramPackageErrorCode::InvalidScalarModule, reader.Offset(),
                                                   static_cast<uint32_t>(KernelPackageErrorCode::Truncated)));
                }
                const ParameterDesc parameter{.kind = *kind,
                                              .access = *access,
                                              .elementType = {.scalar = *scalar, .lanes = *lanes},
                                              .rank = *rank,
                                              .layout = *layout};
                if (*parameterReserved != 0 || !ValidParameterKind(parameter.kind) ||
                    !ValidAccessMode(parameter.access) || !ValidLayoutKind(parameter.layout) ||
                    !parameter.elementType.Valid()) {
                    return std::unexpected(Failure(KernelProgramPackageErrorCode::InvalidScalarModule, reader.Offset(),
                                                   static_cast<uint32_t>(KernelPackageErrorCode::InvalidEncoding)));
                }
                storage.scalarParameters.push_back(parameter);
            }
            for (uint32_t index = 0; index < *instructionCount; ++index) {
                const auto opcode = reader.Read<OpCode>();
                const auto scalar = reader.Read<ScalarKind>();
                const auto lanes = reader.Read<uint8_t>();
                const auto operandCount = reader.Read<uint8_t>();
                std::array<ValueId, 3> operands{};
                for (ValueId& operand : operands) {
                    const auto operandIndex = reader.Read<uint32_t>();
                    if (!operandIndex) {
                        return std::unexpected(Failure(KernelProgramPackageErrorCode::InvalidScalarModule,
                                                       operandIndex.error().offset,
                                                       static_cast<uint32_t>(KernelPackageErrorCode::Truncated)));
                    }
                    operand.index = *operandIndex;
                }
                const auto immediate = reader.Read<uint64_t>();
                const auto flags = reader.Read<uint32_t>();
                const auto instructionReserved = reader.Read<uint32_t>();
                if (!opcode || !scalar || !lanes || !operandCount || !immediate || !flags || !instructionReserved) {
                    return std::unexpected(Failure(KernelProgramPackageErrorCode::InvalidScalarModule, reader.Offset(),
                                                   static_cast<uint32_t>(KernelPackageErrorCode::Truncated)));
                }
                const Instruction instruction{.opcode = *opcode,
                                              .resultType = {.scalar = *scalar, .lanes = *lanes},
                                              .operandCount = *operandCount,
                                              .operands = operands,
                                              .immediate = *immediate,
                                              .flags = *flags};
                if (*instructionReserved != 0 || !ValidOpCode(instruction.opcode) || !instruction.resultType.Valid() ||
                    instruction.operandCount != OperandCount(instruction.opcode) || instruction.flags != 0 ||
                    !ValidInstructionImmediate(instruction)) {
                    return std::unexpected(Failure(KernelProgramPackageErrorCode::InvalidScalarModule, reader.Offset(),
                                                   static_cast<uint32_t>(KernelPackageErrorCode::InvalidEncoding)));
                }
                storage.instructions.push_back(instruction);
            }
            if (!reader.AtEnd()) {
                return std::unexpected(Failure(KernelProgramPackageErrorCode::InvalidScalarModule, reader.Offset(),
                                               static_cast<uint32_t>(KernelPackageErrorCode::TrailingBytes)));
            }
            storage.result = {*result};
            return {};
        }

        [[nodiscard]] std::expected<KernelProgramImageStorage, KernelProgramPackageError>
        DecodeStorage(std::span<const std::byte> bytes, KernelProgramLimits limits) {
            if (!limits.Valid() || bytes.size() > limits.maximumBytes) {
                return std::unexpected(Failure(KernelProgramPackageErrorCode::CapacityExceeded));
            }
            Detail::WireReader reader{bytes};
            const auto magic = reader.Read<uint32_t>();
            const auto major = reader.Read<uint16_t>();
            const auto minor = reader.Read<uint16_t>();
            const auto pattern = reader.Read<KernelPattern>();
            const auto rank = reader.Read<uint8_t>();
            const auto partitionKind = reader.Read<PartitionKind>();
            const auto splittableAxisMask = reader.Read<uint8_t>();
            const auto argumentCount = reader.Read<uint32_t>();
            const auto inputCount = reader.Read<uint32_t>();
            const auto output = reader.Read<uint32_t>();
            const auto scalarByteSize = reader.Read<uint32_t>();
            if (!magic || !major || !minor || !pattern || !rank || !partitionKind || !splittableAxisMask ||
                !argumentCount || !inputCount || !output || !scalarByteSize) {
                return std::unexpected(Failure(KernelProgramPackageErrorCode::Truncated, reader.Offset()));
            }
            if (*magic != kKernelProgramMagic) {
                return std::unexpected(Failure(KernelProgramPackageErrorCode::InvalidMagic));
            }
            if (*major != kKernelProgramMajorVersion || *minor > kKernelProgramMinorVersion) {
                return std::unexpected(Failure(KernelProgramPackageErrorCode::UnsupportedVersion, sizeof(uint32_t)));
            }
            if (*pattern != KernelPattern::Elementwise ||
                (*partitionKind != PartitionKind::Indivisible && *partitionKind != PartitionKind::Elementwise)) {
                return std::unexpected(Failure(KernelProgramPackageErrorCode::InvalidEncoding, 8));
            }
            if (*argumentCount > limits.maximumArguments || *inputCount > limits.maximumScalarParameters) {
                return std::unexpected(Failure(KernelProgramPackageErrorCode::CapacityExceeded, 12));
            }
            const uint64_t requiredBytes = KernelProgram<1, 0, 1>::kHeaderWireSize +
                                           uint64_t{*argumentCount} * KernelProgram<1, 0, 1>::kArgumentWireSize +
                                           uint64_t{*inputCount} * KernelProgram<1, 1, 1>::kInputBindingWireSize +
                                           *scalarByteSize;
            if (requiredBytes > bytes.size()) {
                return std::unexpected(Failure(KernelProgramPackageErrorCode::Truncated, bytes.size()));
            }

            KernelProgramImageStorage storage;
            storage.pattern = *pattern;
            storage.domain.rank = *rank;
            storage.partition = {.kind = *partitionKind, .splittableAxisMask = *splittableAxisMask};
            storage.output = {*output};
            for (uint64_t& extent : storage.domain.staticExtents) {
                const auto value = reader.Read<uint64_t>();
                if (!value) {
                    return std::unexpected(Failure(KernelProgramPackageErrorCode::Truncated, value.error().offset));
                }
                extent = *value;
            }

            storage.arguments.reserve(*argumentCount);
            for (uint32_t index = 0; index < *argumentCount; ++index) {
                const auto kind = reader.Read<ParameterKind>();
                const auto access = reader.Read<AccessMode>();
                const auto scalar = reader.Read<ScalarKind>();
                const auto lanes = reader.Read<uint8_t>();
                const auto argumentRank = reader.Read<uint8_t>();
                const auto layout = reader.Read<LayoutKind>();
                const auto reserved = reader.Read<uint16_t>();
                const auto minimumAlignment = reader.Read<uint32_t>();
                const auto aliasClass = reader.Read<uint32_t>();
                if (!kind || !access || !scalar || !lanes || !argumentRank || !layout || !reserved ||
                    !minimumAlignment || !aliasClass) {
                    return std::unexpected(Failure(KernelProgramPackageErrorCode::Truncated, reader.Offset()));
                }
                const KernelArgumentDesc argument{.kind = *kind,
                                                  .access = *access,
                                                  .elementType = {.scalar = *scalar, .lanes = *lanes},
                                                  .rank = *argumentRank,
                                                  .layout = *layout,
                                                  .minimumAlignment = *minimumAlignment,
                                                  .aliasClass = {*aliasClass}};
                if (*reserved != 0 || !ValidParameterKind(argument.kind) || !ValidAccessMode(argument.access) ||
                    !ValidLayoutKind(argument.layout) || !argument.elementType.Valid()) {
                    return std::unexpected(Failure(KernelProgramPackageErrorCode::InvalidEncoding, reader.Offset()));
                }
                storage.arguments.push_back(argument);
            }

            storage.inputs.reserve(*inputCount);
            for (uint32_t index = 0; index < *inputCount; ++index) {
                const auto kind = reader.Read<ScalarInputKind>();
                const auto reserved8 = reader.Read<uint8_t>();
                const auto reserved16 = reader.Read<uint16_t>();
                const auto sourceIndex = reader.Read<uint32_t>();
                if (!kind || !reserved8 || !reserved16 || !sourceIndex) {
                    return std::unexpected(Failure(KernelProgramPackageErrorCode::Truncated, reader.Offset()));
                }
                if ((*kind != ScalarInputKind::Argument && *kind != ScalarInputKind::LogicalIndex) || *reserved8 != 0 ||
                    *reserved16 != 0) {
                    return std::unexpected(Failure(KernelProgramPackageErrorCode::InvalidEncoding, reader.Offset()));
                }
                storage.inputs.push_back({.kind = *kind, .sourceIndex = *sourceIndex});
            }

            const size_t scalarOffset = reader.Offset();
            const auto scalarBytes = reader.ReadBytes(*scalarByteSize);
            if (!scalarBytes) {
                return std::unexpected(Failure(KernelProgramPackageErrorCode::Truncated, scalarBytes.error().offset));
            }
            if (!reader.AtEnd()) {
                return std::unexpected(Failure(KernelProgramPackageErrorCode::TrailingBytes, reader.Offset()));
            }
            if (auto scalar = DecodeScalar(*scalarBytes, storage, limits); !scalar) {
                KernelProgramPackageError error = scalar.error();
                if (error.code == KernelProgramPackageErrorCode::InvalidScalarModule ||
                    error.code == KernelProgramPackageErrorCode::CapacityExceeded) {
                    error.offset += scalarOffset;
                }
                return std::unexpected(error);
            }
            return storage;
        }

    } // namespace

    KernelProgramImage::KernelProgramImage(KernelProgramImageStorage storage, std::vector<std::byte> bytes,
                                           SemanticHash hash) noexcept
        : pattern_(storage.pattern),
          domain_(storage.domain),
          partition_(storage.partition),
          numericalPolicy_(storage.numericalPolicy),
          arguments_(std::move(storage.arguments)),
          inputs_(std::move(storage.inputs)),
          scalarParameters_(std::move(storage.scalarParameters)),
          instructions_(std::move(storage.instructions)),
          result_(storage.result),
          output_(storage.output),
          bytes_(std::move(bytes)),
          hash_(hash) {}

    KernelProgramView KernelProgramImage::View() const noexcept {
        return {.pattern = pattern_,
                .domain = domain_,
                .partition = partition_,
                .scalar = {.numericalPolicy = numericalPolicy_,
                           .parameters = scalarParameters_,
                           .instructions = instructions_,
                           .result = result_},
                .arguments = arguments_,
                .inputs = inputs_,
                .output = output_};
    }

    std::expected<KernelProgramImage, KernelProgramPackageError>
    KernelProgramImage::Create(KernelProgramImageStorage storage, KernelProgramLimits limits) noexcept {
        if (!FitsLimits(storage, limits)) {
            return std::unexpected(Failure(KernelProgramPackageErrorCode::CapacityExceeded));
        }
        try {
            std::vector<uint8_t> useCounts(storage.arguments.size());
            const KernelProgramView view{.pattern = storage.pattern,
                                         .domain = storage.domain,
                                         .partition = storage.partition,
                                         .scalar = {.numericalPolicy = storage.numericalPolicy,
                                                    .parameters = storage.scalarParameters,
                                                    .instructions = storage.instructions,
                                                    .result = storage.result},
                                         .arguments = storage.arguments,
                                         .inputs = storage.inputs,
                                         .output = storage.output};
            if (auto valid = ValidateKernelProgram(view, useCounts); !valid) {
                return std::unexpected(Failure(KernelProgramPackageErrorCode::InvalidSemantics, 0,
                                               static_cast<uint32_t>(valid.error().code)));
            }
            auto bytes = Encode(view, limits);
            if (!bytes) {
                return std::unexpected(bytes.error());
            }
            const SemanticHash hash = Detail::HashBytes(*bytes);
            return KernelProgramImage{std::move(storage), std::move(*bytes), hash};
        } catch (const std::length_error&) {
            return std::unexpected(Failure(KernelProgramPackageErrorCode::CapacityExceeded));
        } catch (const std::bad_alloc&) {
            return std::unexpected(Failure(KernelProgramPackageErrorCode::OutOfMemory));
        }
    }

    std::expected<KernelProgramImage, KernelProgramPackageError>
    KernelProgramImage::Decode(std::span<const std::byte> bytes, KernelProgramLimits limits) noexcept {
        try {
            auto storage = DecodeStorage(bytes, limits);
            if (!storage) {
                return std::unexpected(storage.error());
            }
            auto image = Create(std::move(*storage), limits);
            if (!image) {
                return std::unexpected(image.error());
            }
            if (!std::ranges::equal(image->Bytes(), bytes)) {
                return std::unexpected(Failure(KernelProgramPackageErrorCode::NonCanonicalEncoding));
            }
            return image;
        } catch (const std::length_error&) {
            return std::unexpected(Failure(KernelProgramPackageErrorCode::CapacityExceeded));
        } catch (const std::bad_alloc&) {
            return std::unexpected(Failure(KernelProgramPackageErrorCode::OutOfMemory));
        }
    }

} // namespace Sora::Compute

/**
 * @file KernelFusion.cpp
 * @brief SSA composition for canonical elementwise kernel programs.
 * @ingroup Compute
 */

#include <Sora/Compute/Compiler/KernelFusion.h>

#include <algorithm>
#include <limits>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace Sora::Compute {
    namespace {

        [[nodiscard]] constexpr PartitionContract FusePartitions(PartitionContract producer,
                                                                 PartitionContract consumer) noexcept {
            if (producer.kind != PartitionKind::Elementwise || consumer.kind != PartitionKind::Elementwise) {
                return PartitionContract::Indivisible();
            }
            const uint8_t axes = producer.splittableAxisMask & consumer.splittableAxisMask;
            return axes == 0 ? PartitionContract::Indivisible()
                             : PartitionContract{.kind = PartitionKind::Elementwise, .splittableAxisMask = axes};
        }

        [[nodiscard]] KernelFusionError Translate(KernelProgramPackageError error) noexcept {
            KernelFusionErrorCode code = KernelFusionErrorCode::InvalidConsumer;
            if (error.code == KernelProgramPackageErrorCode::CapacityExceeded) {
                code = KernelFusionErrorCode::CapacityExceeded;
            } else if (error.code == KernelProgramPackageErrorCode::OutOfMemory) {
                code = KernelFusionErrorCode::OutOfMemory;
            }
            return {.code = code, .package = error};
        }

        [[nodiscard]] std::expected<void, KernelFusionError> ValidateView(KernelProgramView program,
                                                                          KernelFusionErrorCode invalidCode) noexcept {
            try {
                std::vector<uint8_t> useCounts(program.arguments.size());
                if (!ValidateKernelProgram(program, useCounts)) {
                    return std::unexpected(KernelFusionError{invalidCode});
                }
                return {};
            } catch (const std::bad_alloc&) {
                return std::unexpected(KernelFusionError{KernelFusionErrorCode::OutOfMemory});
            }
        }

    } // namespace

    std::expected<KernelProgramImage, KernelFusionError> FuseElementwiseKernels(KernelProgramView producer,
                                                                                KernelProgramView consumer,
                                                                                KernelArgumentId consumerIntermediate,
                                                                                KernelProgramLimits limits) noexcept {
        if (auto valid = ValidateView(producer, KernelFusionErrorCode::InvalidProducer); !valid) {
            return std::unexpected(valid.error());
        }
        if (auto valid = ValidateView(consumer, KernelFusionErrorCode::InvalidConsumer); !valid) {
            return std::unexpected(valid.error());
        }
        if (producer.domain != consumer.domain) {
            return std::unexpected(KernelFusionError{KernelFusionErrorCode::IncompatibleDomain});
        }
        if (producer.scalar.numericalPolicy != consumer.scalar.numericalPolicy) {
            return std::unexpected(KernelFusionError{KernelFusionErrorCode::IncompatiblePolicy});
        }
        if (!consumerIntermediate.Valid() || consumerIntermediate.index >= consumer.arguments.size() ||
            producer.output.index >= producer.arguments.size() || consumer.output == consumerIntermediate) {
            return std::unexpected(
                KernelFusionError{KernelFusionErrorCode::InvalidIntermediate, consumerIntermediate.index});
        }

        const KernelArgumentDesc& producerOutput = producer.arguments[producer.output.index];
        const KernelArgumentDesc& consumerInput = consumer.arguments[consumerIntermediate.index];
        if (producerOutput.kind != ParameterKind::Buffer || producerOutput.access != AccessMode::Write ||
            consumerInput.kind != ParameterKind::Buffer || consumerInput.access != AccessMode::Read ||
            producerOutput.elementType != consumerInput.elementType || producerOutput.rank != consumerInput.rank ||
            producerOutput.layout != consumerInput.layout) {
            return std::unexpected(
                KernelFusionError{KernelFusionErrorCode::InvalidIntermediate, consumerIntermediate.index});
        }

        try {
            KernelProgramImageStorage fused;
            fused.pattern = KernelPattern::Elementwise;
            fused.domain = producer.domain;
            fused.partition = FusePartitions(producer.partition, consumer.partition);
            fused.numericalPolicy = producer.scalar.numericalPolicy;

            std::vector<KernelArgumentId> producerArguments(producer.arguments.size());
            std::vector<KernelArgumentId> consumerArguments(consumer.arguments.size());
            fused.arguments.reserve(producer.arguments.size() + consumer.arguments.size() - 2);
            for (uint32_t index = 0; index < producer.arguments.size(); ++index) {
                if (index == producer.output.index) {
                    continue;
                }
                KernelArgumentDesc argument = producer.arguments[index];
                argument.aliasClass = {};
                producerArguments[index] = {static_cast<uint32_t>(fused.arguments.size())};
                fused.arguments.push_back(argument);
            }
            for (uint32_t index = 0; index < consumer.arguments.size(); ++index) {
                if (index == consumerIntermediate.index) {
                    continue;
                }
                consumerArguments[index] = {static_cast<uint32_t>(fused.arguments.size())};
                fused.arguments.push_back(consumer.arguments[index]);
            }
            fused.output = consumerArguments[consumer.output.index];

            fused.scalarParameters.reserve(producer.scalar.parameters.size() + consumer.scalar.parameters.size() - 1);
            fused.inputs.reserve(producer.inputs.size() + consumer.inputs.size() - 1);
            for (uint32_t index = 0; index < producer.scalar.parameters.size(); ++index) {
                fused.scalarParameters.push_back(producer.scalar.parameters[index]);
                ScalarInputBinding input = producer.inputs[index];
                if (input.kind == ScalarInputKind::Argument) {
                    if (input.sourceIndex >= producerArguments.size() ||
                        !producerArguments[input.sourceIndex].Valid()) {
                        return std::unexpected(KernelFusionError{KernelFusionErrorCode::InvalidProducer});
                    }
                    input.sourceIndex = producerArguments[input.sourceIndex].index;
                }
                fused.inputs.push_back(input);
            }

            fused.instructions.assign(producer.scalar.instructions.begin(), producer.scalar.instructions.end());
            const ValueId producerResult = producer.scalar.result;
            std::vector<ValueId> consumerValues(consumer.scalar.instructions.size());
            std::vector<std::optional<uint32_t>> consumerParameters(consumer.scalar.parameters.size());
            for (uint32_t index = 0; index < consumer.scalar.parameters.size(); ++index) {
                const ScalarInputBinding input = consumer.inputs[index];
                if (input.kind == ScalarInputKind::Argument && input.sourceIndex == consumerIntermediate.index) {
                    continue;
                }
                consumerParameters[index] = static_cast<uint32_t>(fused.scalarParameters.size());
                fused.scalarParameters.push_back(consumer.scalar.parameters[index]);
                ScalarInputBinding remapped = input;
                if (remapped.kind == ScalarInputKind::Argument) {
                    if (remapped.sourceIndex >= consumerArguments.size() ||
                        !consumerArguments[remapped.sourceIndex].Valid()) {
                        return std::unexpected(KernelFusionError{KernelFusionErrorCode::InvalidConsumer});
                    }
                    remapped.sourceIndex = consumerArguments[remapped.sourceIndex].index;
                }
                fused.inputs.push_back(remapped);
            }

            for (uint32_t index = 0; index < consumer.scalar.instructions.size(); ++index) {
                Instruction instruction = consumer.scalar.instructions[index];
                if (instruction.opcode == OpCode::Input) {
                    if (instruction.immediate >= consumerParameters.size()) {
                        return std::unexpected(KernelFusionError{KernelFusionErrorCode::InvalidConsumer});
                    }
                    const auto parameter = consumerParameters[instruction.immediate];
                    if (!parameter) {
                        consumerValues[index] = producerResult;
                        continue;
                    }
                    instruction.immediate = *parameter;
                }
                for (uint8_t operand = 0; operand < instruction.operandCount; ++operand) {
                    const uint32_t source = instruction.operands[operand].index;
                    if (source >= consumerValues.size() || !consumerValues[source].Valid()) {
                        return std::unexpected(KernelFusionError{KernelFusionErrorCode::InvalidConsumer});
                    }
                    instruction.operands[operand] = consumerValues[source];
                }
                consumerValues[index] = {static_cast<uint32_t>(fused.instructions.size())};
                fused.instructions.push_back(instruction);
            }
            fused.result = consumerValues[consumer.scalar.result.index];

            auto image = KernelProgramImage::Create(std::move(fused), limits);
            if (!image) {
                return std::unexpected(Translate(image.error()));
            }
            return std::move(*image);
        } catch (const std::length_error&) {
            return std::unexpected(KernelFusionError{KernelFusionErrorCode::CapacityExceeded});
        } catch (const std::bad_alloc&) {
            return std::unexpected(KernelFusionError{KernelFusionErrorCode::OutOfMemory});
        }
    }

} // namespace Sora::Compute

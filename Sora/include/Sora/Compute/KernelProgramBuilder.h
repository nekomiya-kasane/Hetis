/**
 * @file KernelProgramBuilder.h
 * @brief Constexpr construction of canonical elementwise kernel programs.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/KernelProgramValidation.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>

namespace Sora::Compute {

    /** @brief Fixed-capacity builder for a scalar module's elementwise dispatch semantics. */
    template<size_t ArgumentCapacity, size_t ScalarParameterCapacity, size_t InstructionCapacity>
    class KernelProgramBuilder {
        using Program = KernelProgram<ArgumentCapacity, ScalarParameterCapacity, InstructionCapacity>;

    public:
        /** @brief Start a program around an already validated-or-validatable pure scalar module. */
        constexpr KernelProgramBuilder(IndexDomain domain, typename Program::ScalarModule scalar,
                                       PartitionContract partition = PartitionContract::Indivisible()) noexcept {
            program_.domain = domain;
            program_.partition = partition;
            program_.scalar = std::move(scalar);
        }

        /** @brief Add a scalar runtime value argument. */
        [[nodiscard]] constexpr KernelArgumentId AddValueArgument(ValueType type) {
            return AddArgument({.kind = ParameterKind::Value,
                                .access = AccessMode::Read,
                                .elementType = type,
                                .rank = 0,
                                .layout = LayoutKind::Scalar,
                                .minimumAlignment = 1,
                                .aliasClass = {}});
        }

        /** @brief Add a ranked buffer argument with explicit access and layout semantics. */
        [[nodiscard]] constexpr KernelArgumentId AddBufferArgument(AccessMode access, ValueType elementType,
                                                                   uint8_t rank,
                                                                   LayoutKind layout = LayoutKind::Contiguous,
                                                                   uint32_t minimumAlignment = 1,
                                                                   AliasClassId aliasClass = {}) {
            return AddArgument({.kind = ParameterKind::Buffer,
                                .access = access,
                                .elementType = elementType,
                                .rank = rank,
                                .layout = layout,
                                .minimumAlignment = minimumAlignment,
                                .aliasClass = aliasClass});
        }

        /** @brief Bind the next scalar parameter to a value argument or per-element buffer load. */
        constexpr void BindArgument(KernelArgumentId argument) {
            if (program_.inputCount == ScalarParameterCapacity || !argument.Valid()) {
                throw "KernelProgramBuilder: invalid or excess argument input";
            }
            program_.inputs[program_.inputCount++] = {
                .kind = ScalarInputKind::Argument,
                .sourceIndex = argument.index,
            };
        }

        /** @brief Bind the next UInt64 scalar parameter to one logical index axis. */
        constexpr void BindLogicalIndex(uint8_t axis) {
            if (program_.inputCount == ScalarParameterCapacity) {
                throw "KernelProgramBuilder: excess logical-index input";
            }
            program_.inputs[program_.inputCount++] = {
                .kind = ScalarInputKind::LogicalIndex,
                .sourceIndex = axis,
            };
        }

        /** @brief Finalize without throwing, returning the precise semantic validation error. */
        [[nodiscard]] constexpr std::expected<Program, KernelProgramError>
        TryFinish(KernelArgumentId output) && noexcept {
            program_.output = output;
            if (auto valid = ValidateKernelProgram(program_); !valid) {
                return std::unexpected(valid.error());
            }
            return program_;
        }

        /** @brief Finalize during constant evaluation; invalid programs produce a compile-time diagnostic. */
        [[nodiscard]] constexpr Program Finish(KernelArgumentId output) && {
            auto result = std::move(*this).TryFinish(output);
            if (!result) {
                throw "KernelProgramBuilder: semantically invalid program";
            }
            return std::move(*result);
        }

    private:
        [[nodiscard]] constexpr KernelArgumentId AddArgument(KernelArgumentDesc argument) {
            if (program_.argumentCount == ArgumentCapacity) {
                throw "KernelProgramBuilder: argument capacity exceeded";
            }
            const KernelArgumentId result{program_.argumentCount++};
            program_.arguments[result.index] = argument;
            return result;
        }

        Program program_{};
    };

} // namespace Sora::Compute

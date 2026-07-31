/**
 * @file ElementwiseKernel.h
 * @brief Compile-time lifting of scalar mathematical definitions into exact-layout kernel programs.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/KernelDefinition.h>
#include <Sora/Compute/KernelProgramBuilder.h>

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace Sora::Compute {

    /** @brief Projection of one mathematical input onto a runtime value or per-element buffer. */
    struct ElementwiseInputSpec {
        ParameterKind kind = ParameterKind::Value;
        LayoutKind layout = LayoutKind::Scalar;
        uint32_t minimumAlignment = 1;
        AliasClassId aliasClass{};

        /** @brief Supply the lifted input as one runtime short-vector value. */
        [[nodiscard]] static consteval ElementwiseInputSpec Value() noexcept { return {}; }

        /** @brief Supply the lifted input from one exact-layout read buffer. */
        [[nodiscard]] static consteval ElementwiseInputSpec Buffer(LayoutKind layout, uint32_t minimumAlignment = 1,
                                                                   AliasClassId aliasClass = {}) noexcept {
            return {.kind = ParameterKind::Buffer,
                    .layout = layout,
                    .minimumAlignment = minimumAlignment,
                    .aliasClass = aliasClass};
        }

        friend constexpr bool operator==(ElementwiseInputSpec, ElementwiseInputSpec) noexcept = default;
    };

    /** @brief Exact layout and alias contract of the per-element output buffer. */
    struct ElementwiseOutputSpec {
        LayoutKind layout = LayoutKind::Contiguous;
        uint32_t minimumAlignment = 1;
        AliasClassId aliasClass{};

        friend constexpr bool operator==(ElementwiseOutputSpec, ElementwiseOutputSpec) noexcept = default;
    };

    namespace Detail {

        [[nodiscard]] constexpr bool ExactElementwiseLayout(LayoutKind layout) noexcept {
            return layout == LayoutKind::Contiguous || layout == LayoutKind::StructureOfArrays;
        }

        [[nodiscard]] constexpr bool Componentwise(OpCode opcode) noexcept {
            switch (opcode) {
                case OpCode::Input:
                case OpCode::Constant:
                case OpCode::Add:
                case OpCode::Sub:
                case OpCode::Mul:
                case OpCode::Div:
                case OpCode::Neg:
                case OpCode::Inv:
                case OpCode::Square:
                case OpCode::Abs:
                case OpCode::Sin:
                case OpCode::Cos:
                case OpCode::Tan:
                case OpCode::Asin:
                case OpCode::Acos:
                case OpCode::Atan:
                case OpCode::Atan2:
                case OpCode::Exp:
                case OpCode::Log:
                case OpCode::Sqrt:
                case OpCode::Pow:
                case OpCode::Fma:
                case OpCode::Mfs:
                case OpCode::Nms:
                case OpCode::Nma:
                case OpCode::Lerp:
                case OpCode::Clamp:
                case OpCode::Saturate:
                case OpCode::Sign:
                    return true;
            }
            return false;
        }

        template<uint8_t Lanes, PortableKernelModule Module>
        [[nodiscard]] consteval Module LiftElementwiseModule(Module module) {
            if (!Validate(module)) {
                throw "LiftElementwise: source module is invalid";
            }
            for (uint32_t index = 0; index < module.parameterCount; ++index) {
                ParameterDesc& parameter = module.parameters[index];
                if (parameter.kind != ParameterKind::Value || parameter.access != AccessMode::Read ||
                    parameter.rank != 0 || parameter.layout != LayoutKind::Scalar || parameter.elementType.lanes != 1) {
                    throw "LiftElementwise: source parameters must be scalar values";
                }
                parameter.elementType.lanes = Lanes;
            }
            for (uint32_t index = 0; index < module.instructionCount; ++index) {
                Instruction& instruction = module.instructions[index];
                if (!Componentwise(instruction.opcode) || instruction.resultType.lanes != 1) {
                    throw "LiftElementwise: source instructions must be scalar and componentwise";
                }
                instruction.resultType.lanes = Lanes;
            }
            if (!Validate(module)) {
                throw "LiftElementwise: lifted module is invalid";
            }
            return module;
        }

        consteval void ValidateElementwiseInput(ElementwiseInputSpec input) {
            if (input.kind == ParameterKind::Value) {
                if (input.layout != LayoutKind::Scalar || input.minimumAlignment != 1 || input.aliasClass.Known()) {
                    throw "LiftElementwise: value input has non-canonical buffer metadata";
                }
                return;
            }
            if (input.kind != ParameterKind::Buffer || !ExactElementwiseLayout(input.layout) ||
                !std::has_single_bit(input.minimumAlignment)) {
                throw "LiftElementwise: buffer input requires an exact layout and power-of-two alignment";
            }
        }

        consteval void ValidateElementwiseOutput(ElementwiseOutputSpec output) {
            if (!ExactElementwiseLayout(output.layout) || !std::has_single_bit(output.minimumAlignment)) {
                throw "LiftElementwise: output requires an exact layout and power-of-two alignment";
            }
        }

    } // namespace Detail

    /**
     * @brief Lift one scalar definition to the finite product algebra represented by @p Lanes components.
     * @details Every source parameter and instruction is lifted uniformly. Constants therefore broadcast to every
     * component without adding an instruction. Direct calls on the original definition remain independent of runtime
     * planning, while the returned program enters the normal Compute compilation and heterogeneous submission path.
     * Strided layout is intentionally rejected because it would weaken exact-layout optimization guarantees.
     */
    template<uint8_t Lanes = 1, PortableKernelDefinition Definition, typename... Inputs>
        requires(Lanes >= 1 && Lanes <= 4) && (std::same_as<std::remove_cvref_t<Inputs>, ElementwiseInputSpec> && ...)
    [[nodiscard]] consteval auto LiftElementwise(const Definition& definition, IndexDomain domain,
                                                 ElementwiseOutputSpec output, Inputs... inputs) {
        using Module = std::remove_cvref_t<decltype(definition.PortableModule())>;
        static_assert(requires {
            Module::kParameterCapacity;
            Module::kInstructionCapacity;
        });

        if (sizeof...(Inputs) != definition.PortableModule().parameterCount) {
            throw "LiftElementwise: input projection count does not match the mathematical definition";
        }
        Detail::ValidateElementwiseOutput(output);
        (Detail::ValidateElementwiseInput(inputs), ...);

        auto module = Detail::LiftElementwiseModule<Lanes>(definition.PortableModule());
        std::array<ValueType, sizeof...(Inputs)> parameterTypes{};
        for (uint32_t index = 0; index < parameterTypes.size(); ++index) {
            parameterTypes[index] = module.parameters[index].elementType;
        }
        const ValueType resultType = module.instructions[module.result.index].resultType;
        KernelProgramBuilder<sizeof...(Inputs) + 1, Module::kParameterCapacity, Module::kInstructionCapacity> builder{
            domain, std::move(module), PartitionContract::Elementwise(domain.rank)};

        uint32_t parameterIndex = 0;
        auto bind = [&](ElementwiseInputSpec input) {
            const ValueType type = parameterTypes[parameterIndex++];
            const KernelArgumentId argument =
                input.kind == ParameterKind::Value
                    ? builder.AddValueArgument(type)
                    : builder.AddBufferArgument(AccessMode::Read, type, domain.rank, input.layout,
                                                input.minimumAlignment, input.aliasClass);
            builder.BindArgument(argument);
        };
        (bind(inputs), ...);

        const KernelArgumentId result = builder.AddBufferArgument(
            AccessMode::Write, resultType, domain.rank, output.layout, output.minimumAlignment, output.aliasClass);
        return std::move(builder).Finish(result);
    }

    static_assert(std::is_trivially_copyable_v<ElementwiseInputSpec>);
    static_assert(std::is_trivially_copyable_v<ElementwiseOutputSpec>);

} // namespace Sora::Compute

/**
 * @file KernelBuilder.h
 * @brief Compile-time construction of canonical Kernel IR modules.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/KernelIR.h>

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace Sora::Compute {

    /** @brief Arithmetic scalar with a stable Kernel IR v1 representation. */
    template<typename T>
    concept PortableScalar = std::same_as<std::remove_cv_t<T>, bool> ||
                             (std::integral<std::remove_cv_t<T>> && sizeof(T) <= sizeof(uint64_t)) ||
                             std::same_as<std::remove_cv_t<T>, float> || std::same_as<std::remove_cv_t<T>, double>;

    /** @brief Map a C++ arithmetic type to its portable scalar representation. */
    template<PortableScalar T>
    [[nodiscard]] consteval ScalarKind ScalarKindOf() {
        using U = std::remove_cv_t<T>;
        if constexpr (std::same_as<U, bool>) {
            return ScalarKind::Bool;
        } else if constexpr (std::same_as<U, float>) {
            static_assert(std::numeric_limits<float>::is_iec559);
            return ScalarKind::Float32;
        } else if constexpr (std::same_as<U, double>) {
            static_assert(std::numeric_limits<double>::is_iec559);
            return ScalarKind::Float64;
        } else if constexpr (std::signed_integral<U>) {
            if constexpr (sizeof(U) == 1) {
                return ScalarKind::Int8;
            } else if constexpr (sizeof(U) == 2) {
                return ScalarKind::Int16;
            } else if constexpr (sizeof(U) == 4) {
                return ScalarKind::Int32;
            } else {
                return ScalarKind::Int64;
            }
        } else if constexpr (sizeof(U) == 1) {
            return ScalarKind::UInt8;
        } else if constexpr (sizeof(U) == 2) {
            return ScalarKind::UInt16;
        } else if constexpr (sizeof(U) == 4) {
            return ScalarKind::UInt32;
        } else {
            return ScalarKind::UInt64;
        }
    }

    /** @brief Encode one scalar constant into the canonical 64-bit instruction immediate. */
    template<PortableScalar T>
    [[nodiscard]] constexpr uint64_t EncodeConstant(T value) noexcept {
        using U = std::remove_cv_t<T>;
        if constexpr (std::same_as<U, float>) {
            return std::bit_cast<uint32_t>(value);
        } else if constexpr (std::same_as<U, double>) {
            return std::bit_cast<uint64_t>(value);
        } else if constexpr (std::same_as<U, bool>) {
            return value ? 1U : 0U;
        } else {
            return static_cast<uint64_t>(static_cast<std::make_unsigned_t<U>>(value));
        }
    }

    /** @brief Exact-capacity builder used during constant evaluation to produce one Kernel IR module. */
    template<size_t ParameterCapacity, size_t InstructionCapacity>
    class KernelBuilder {
    public:
        /** @brief Construct a builder under an explicit numerical contract. */
        constexpr explicit KernelBuilder(NumericalPolicy policy = {}) noexcept { module_.numericalPolicy = policy; }

        /** @brief Declare a scalar input and emit its SSA input instruction. */
        [[nodiscard]] constexpr ValueId AddValueParameter(ValueType type) {
            if (!type.Valid() || module_.parameterCount == ParameterCapacity) {
                throw "KernelBuilder: invalid or excess value parameter";
            }
            const uint32_t parameterIndex = module_.parameterCount++;
            module_.parameters[parameterIndex] = {
                .kind = ParameterKind::Value,
                .access = AccessMode::Read,
                .elementType = type,
                .rank = 0,
                .layout = LayoutKind::Scalar,
            };
            return Emit({.opcode = OpCode::Input,
                         .resultType = type,
                         .operandCount = 0,
                         .operands = {},
                         .immediate = parameterIndex,
                         .flags = 0});
        }

        /** @brief Emit a constant already converted to the module's result scalar type. */
        template<PortableScalar T>
        [[nodiscard]] constexpr ValueId Constant(T value) {
            constexpr ValueType type{.scalar = ScalarKindOf<T>(), .lanes = 1};
            return Emit({.opcode = OpCode::Constant,
                         .resultType = type,
                         .operandCount = 0,
                         .operands = {},
                         .immediate = EncodeConstant(value),
                         .flags = 0});
        }

        /** @brief Emit a unary instruction. */
        [[nodiscard]] constexpr ValueId Unary(OpCode opcode, ValueId operand) {
            RequireArity(opcode, 1);
            const ValueType type = TypeOf(operand);
            return Emit({.opcode = opcode,
                         .resultType = type,
                         .operandCount = 1,
                         .operands = {operand, {}, {}},
                         .immediate = 0,
                         .flags = 0});
        }

        /** @brief Emit a binary instruction whose operands have identical types. */
        [[nodiscard]] constexpr ValueId Binary(OpCode opcode, ValueId first, ValueId second) {
            RequireArity(opcode, 2);
            const ValueType type = CommonType(first, second);
            return Emit({.opcode = opcode,
                         .resultType = type,
                         .operandCount = 2,
                         .operands = {first, second, {}},
                         .immediate = 0,
                         .flags = 0});
        }

        /** @brief Emit a ternary instruction whose operands have identical types. */
        [[nodiscard]] constexpr ValueId Ternary(OpCode opcode, ValueId first, ValueId second, ValueId third) {
            RequireArity(opcode, 3);
            const ValueType type = CommonType(first, second);
            if (TypeOf(third) != type) {
                throw "KernelBuilder: ternary operand type mismatch";
            }
            return Emit({.opcode = opcode,
                         .resultType = type,
                         .operandCount = 3,
                         .operands = {first, second, third},
                         .immediate = 0,
                         .flags = 0});
        }

        /** @brief Finalize the exact-capacity module with @p result as its sole returned value. */
        [[nodiscard]] constexpr KernelModule<ParameterCapacity, InstructionCapacity> Finish(ValueId result) && {
            static_assert(InstructionCapacity > 0, "A kernel must contain at least one instruction");
            if (!result.Valid() || result.index >= module_.instructionCount ||
                module_.parameterCount != ParameterCapacity || module_.instructionCount != InstructionCapacity) {
                throw "KernelBuilder: incomplete module";
            }
            module_.result = result;
            return module_;
        }

    private:
        [[nodiscard]] constexpr ValueId Emit(Instruction instruction) {
            if (module_.instructionCount == InstructionCapacity || !instruction.resultType.Valid()) {
                throw "KernelBuilder: instruction capacity exceeded or invalid result type";
            }
            const ValueId result{module_.instructionCount++};
            module_.instructions[result.index] = instruction;
            return result;
        }

        constexpr void RequireArity(OpCode opcode, uint8_t expected) const {
            if (OperandCount(opcode) != expected) {
                throw "KernelBuilder: opcode arity mismatch";
            }
        }

        [[nodiscard]] constexpr ValueType TypeOf(ValueId value) const {
            if (!value.Valid() || value.index >= module_.instructionCount) {
                throw "KernelBuilder: operand is not defined before use";
            }
            return module_.instructions[value.index].resultType;
        }

        [[nodiscard]] constexpr ValueType CommonType(ValueId first, ValueId second) const {
            const ValueType type = TypeOf(first);
            if (TypeOf(second) != type) {
                throw "KernelBuilder: binary operand type mismatch";
            }
            return type;
        }

        KernelModule<ParameterCapacity, InstructionCapacity> module_{};
    };

} // namespace Sora::Compute

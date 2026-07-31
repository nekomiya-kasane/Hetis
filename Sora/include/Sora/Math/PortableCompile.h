/**
 * @file PortableCompile.h
 * @brief Compile Math CPO expressions into direct evaluators paired with canonical Compute Kernel IR.
 * @ingroup Math
 */
#pragma once

#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelDefinition.h>
#include <Sora/Math/Compile.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace Sora::Math {

    inline constexpr Compute::NumericalPolicy kContractedKernelPolicy{
        .floatingPoint = Compute::FloatingPointMode::Contract,
        .reduction = Compute::ReductionMode::Deterministic,
    };

    namespace PortableDetail {

        template<typename>
        inline constexpr bool kUnsupportedOperation = false;

        template<auto Operation>
        [[nodiscard]] consteval Compute::OpCode OpCodeOf() {
            using T = std::remove_cv_t<decltype(Operation)>;
            if constexpr (std::same_as<T, CPO::AddFn>) {
                return Compute::OpCode::Add;
            } else if constexpr (std::same_as<T, CPO::SubFn>) {
                return Compute::OpCode::Sub;
            } else if constexpr (std::same_as<T, CPO::MulFn>) {
                return Compute::OpCode::Mul;
            } else if constexpr (std::same_as<T, CPO::DivFn>) {
                return Compute::OpCode::Div;
            } else if constexpr (std::same_as<T, CPO::NegFn>) {
                return Compute::OpCode::Neg;
            } else if constexpr (std::same_as<T, CPO::InvFn>) {
                return Compute::OpCode::Inv;
            } else if constexpr (std::same_as<T, CPO::SquareFn>) {
                return Compute::OpCode::Square;
            } else if constexpr (std::same_as<T, CPO::AbsFn>) {
                return Compute::OpCode::Abs;
            } else if constexpr (std::same_as<T, CPO::SinFn>) {
                return Compute::OpCode::Sin;
            } else if constexpr (std::same_as<T, CPO::CosFn>) {
                return Compute::OpCode::Cos;
            } else if constexpr (std::same_as<T, CPO::TanFn>) {
                return Compute::OpCode::Tan;
            } else if constexpr (std::same_as<T, CPO::AsinFn>) {
                return Compute::OpCode::Asin;
            } else if constexpr (std::same_as<T, CPO::AcosFn>) {
                return Compute::OpCode::Acos;
            } else if constexpr (std::same_as<T, CPO::AtanFn>) {
                return Compute::OpCode::Atan;
            } else if constexpr (std::same_as<T, CPO::Atan2Fn>) {
                return Compute::OpCode::Atan2;
            } else if constexpr (std::same_as<T, CPO::ExpFn>) {
                return Compute::OpCode::Exp;
            } else if constexpr (std::same_as<T, CPO::LogFn>) {
                return Compute::OpCode::Log;
            } else if constexpr (std::same_as<T, CPO::SqrtFn>) {
                return Compute::OpCode::Sqrt;
            } else if constexpr (std::same_as<T, CPO::PowFn>) {
                return Compute::OpCode::Pow;
            } else if constexpr (std::same_as<T, CPO::FmaFn>) {
                return Compute::OpCode::Fma;
            } else if constexpr (std::same_as<T, CPO::MfsFn>) {
                return Compute::OpCode::Mfs;
            } else if constexpr (std::same_as<T, CPO::NmsFn>) {
                return Compute::OpCode::Nms;
            } else if constexpr (std::same_as<T, CPO::NmaFn>) {
                return Compute::OpCode::Nma;
            } else if constexpr (std::same_as<T, CPO::LerpFn>) {
                return Compute::OpCode::Lerp;
            } else if constexpr (std::same_as<T, CPO::ClampFn>) {
                return Compute::OpCode::Clamp;
            } else if constexpr (std::same_as<T, CPO::SaturateFn>) {
                return Compute::OpCode::Saturate;
            } else if constexpr (std::same_as<T, CPO::SignFn>) {
                return Compute::OpCode::Sign;
            } else {
                static_assert(kUnsupportedOperation<T>, "Math operation has no portable Kernel IR opcode");
            }
        }

        template<typename E>
        struct NonInputInstructionCount;

        template<size_t Index>
        struct NonInputInstructionCount<Detail::StagedInput<Index>> : std::integral_constant<size_t, 0> {};

        template<typename T>
        struct NonInputInstructionCount<Detail::StagedConstant<T>> : std::integral_constant<size_t, 1> {};

        template<auto Operation, Detail::StagedExpression E>
        struct NonInputInstructionCount<Detail::StagedUnary<Operation, E>>
            : std::integral_constant<size_t, 1 + NonInputInstructionCount<E>::value> {};

        template<auto Operation, Detail::StagedExpression First, Detail::StagedExpression Second>
        struct NonInputInstructionCount<Detail::StagedBinary<Operation, First, Second>>
            : std::integral_constant<size_t, 1 + NonInputInstructionCount<First>::value +
                                                 NonInputInstructionCount<Second>::value> {};

        template<auto Operation, Detail::StagedExpression First, Detail::StagedExpression Second,
                 Detail::StagedExpression Third>
        struct NonInputInstructionCount<Detail::StagedTernary<Operation, First, Second, Third>>
            : std::integral_constant<size_t, 1 + NonInputInstructionCount<First>::value +
                                                 NonInputInstructionCount<Second>::value +
                                                 NonInputInstructionCount<Third>::value> {};

        template<auto Operation, Detail::StagedExpression X, Detail::StagedExpression C>
        struct NonInputInstructionCount<Detail::StagedFusedSquare<Operation, X, C>>
            : std::integral_constant<size_t,
                                     1 + NonInputInstructionCount<X>::value + NonInputInstructionCount<C>::value> {};

        template<Compute::PortableScalar Scalar, size_t ParameterCapacity, size_t InstructionCapacity, size_t Arity,
                 size_t Index>
        [[nodiscard]] constexpr Compute::ValueId Emit(Compute::KernelBuilder<ParameterCapacity, InstructionCapacity>&,
                                                      const Detail::StagedInput<Index>&,
                                                      const std::array<Compute::ValueId, Arity>& inputs) {
            static_assert(Index < Arity);
            return inputs[Index];
        }

        template<Compute::PortableScalar Scalar, size_t ParameterCapacity, size_t InstructionCapacity, size_t Arity,
                 typename T>
            requires std::convertible_to<T, Scalar>
        [[nodiscard]] constexpr Compute::ValueId
        Emit(Compute::KernelBuilder<ParameterCapacity, InstructionCapacity>& builder,
             const Detail::StagedConstant<T>& expression, const std::array<Compute::ValueId, Arity>&) {
            return builder.Constant(static_cast<Scalar>(expression.value));
        }

        template<Compute::PortableScalar Scalar, size_t ParameterCapacity, size_t InstructionCapacity, size_t Arity,
                 auto Operation, Detail::StagedExpression E>
        [[nodiscard]] constexpr Compute::ValueId
        Emit(Compute::KernelBuilder<ParameterCapacity, InstructionCapacity>& builder,
             const Detail::StagedUnary<Operation, E>& expression, const std::array<Compute::ValueId, Arity>& inputs) {
            return builder.Unary(OpCodeOf<Operation>(), Emit<Scalar>(builder, expression.Operand(), inputs));
        }

        template<Compute::PortableScalar Scalar, size_t ParameterCapacity, size_t InstructionCapacity, size_t Arity,
                 auto Operation, Detail::StagedExpression First, Detail::StagedExpression Second>
        [[nodiscard]] constexpr Compute::ValueId
        Emit(Compute::KernelBuilder<ParameterCapacity, InstructionCapacity>& builder,
             const Detail::StagedBinary<Operation, First, Second>& expression,
             const std::array<Compute::ValueId, Arity>& inputs) {
            const Compute::ValueId first = Emit<Scalar>(builder, expression.FirstOperand(), inputs);
            const Compute::ValueId second = Emit<Scalar>(builder, expression.SecondOperand(), inputs);
            return builder.Binary(OpCodeOf<Operation>(), first, second);
        }

        template<Compute::PortableScalar Scalar, size_t ParameterCapacity, size_t InstructionCapacity, size_t Arity,
                 auto Operation, Detail::StagedExpression First, Detail::StagedExpression Second,
                 Detail::StagedExpression Third>
        [[nodiscard]] constexpr Compute::ValueId
        Emit(Compute::KernelBuilder<ParameterCapacity, InstructionCapacity>& builder,
             const Detail::StagedTernary<Operation, First, Second, Third>& expression,
             const std::array<Compute::ValueId, Arity>& inputs) {
            const Compute::ValueId first = Emit<Scalar>(builder, expression.FirstOperand(), inputs);
            const Compute::ValueId second = Emit<Scalar>(builder, expression.SecondOperand(), inputs);
            const Compute::ValueId third = Emit<Scalar>(builder, expression.ThirdOperand(), inputs);
            return builder.Ternary(OpCodeOf<Operation>(), first, second, third);
        }

        template<Compute::PortableScalar Scalar, size_t ParameterCapacity, size_t InstructionCapacity, size_t Arity,
                 auto Operation, Detail::StagedExpression X, Detail::StagedExpression C>
        [[nodiscard]] constexpr Compute::ValueId
        Emit(Compute::KernelBuilder<ParameterCapacity, InstructionCapacity>& builder,
             const Detail::StagedFusedSquare<Operation, X, C>& expression,
             const std::array<Compute::ValueId, Arity>& inputs) {
            const Compute::ValueId value = Emit<Scalar>(builder, expression.ValueOperand(), inputs);
            const Compute::ValueId addend = Emit<Scalar>(builder, expression.AddendOperand(), inputs);
            return builder.Ternary(OpCodeOf<Operation>(), value, value, addend);
        }

        template<Compute::PortableScalar Scalar, size_t Arity, size_t InstructionCount, Detail::StagedExpression E>
        [[nodiscard]] consteval auto BuildModule(const E& expression, Compute::NumericalPolicy policy) {
            Compute::KernelBuilder<Arity, InstructionCount> builder{policy};
            std::array<Compute::ValueId, Arity> inputs{};
            constexpr Compute::ValueType type{.scalar = Compute::ScalarKindOf<Scalar>(), .lanes = 1};
            for (Compute::ValueId& input : inputs) {
                input = builder.AddValueParameter(type);
            }
            const Compute::ValueId result = Emit<Scalar>(builder, expression, inputs);
            return std::move(builder).Finish(result);
        }

    } // namespace PortableDetail

    /**
     * @brief Compile one Math CPO definition into a direct evaluator and canonical portable Kernel IR.
     * @tparam Policy Numerical transformations permitted by every backend; strict mode disables implicit contraction.
     */
    template<Compute::PortableScalar Scalar, size_t Arity, Compute::NumericalPolicy Policy = kContractedKernelPolicy,
             typename Function>
    [[nodiscard]] consteval auto CompilePortable(Function&& function) {
        using Inputs = [:Detail::StagedInputTuple<Arity>():];
        auto staged = Detail::Stage(std::apply(std::forward<Function>(function), Inputs{}));
        if constexpr (Policy.floatingPoint == Compute::FloatingPointMode::Strict) {
            using E = decltype(staged);
            constexpr size_t instructionCount = Arity + PortableDetail::NonInputInstructionCount<E>::value;
            auto module = PortableDetail::BuildModule<Scalar, Arity, instructionCount>(staged, Policy);
            return Compute::KernelDefinition{CompiledKernel<E, Arity>{std::move(staged)}, std::move(module)};
        } else {
            auto expression = Detail::Normalize(staged);
            using E = decltype(expression);
            constexpr size_t instructionCount = Arity + PortableDetail::NonInputInstructionCount<E>::value;
            auto module = PortableDetail::BuildModule<Scalar, Arity, instructionCount>(expression, Policy);
            return Compute::KernelDefinition{CompiledKernel<E, Arity>{std::move(expression)}, std::move(module)};
        }
    }

} // namespace Sora::Math

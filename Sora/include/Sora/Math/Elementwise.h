/**
 * @file Elementwise.h
 * @brief Compile direct mathematical definitions into exact-layout elementwise programs.
 * @ingroup Math
 */
#pragma once

#include <Sora/Compute/ElementwiseKernel.h>
#include <Sora/Math/PortableCompile.h>

#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

namespace Sora::Math {

    /**
     * @brief Definition carrying one zero-overhead evaluator and one exact-layout bulk program.
     * @tparam Definition Scalar/SIMD mathematical evaluator paired with canonical scalar Kernel IR.
     * @tparam ProgramType Exact-layout elementwise program generated from @p Definition.
     */
    template<Compute::PortableKernelDefinition Definition, typename ProgramType>
    class ElementwiseDefinition final {
    public:
        /** @brief Construct from a scalar definition and its semantically equivalent lifted program. */
        constexpr ElementwiseDefinition(Definition definition, ProgramType program) noexcept(
            std::is_nothrow_move_constructible_v<Definition> && std::is_nothrow_move_constructible_v<ProgramType>)
            : definition_(std::move(definition)), program_(std::move(program)) {}

        /** @brief Invoke the direct scalar or SIMD-carrier evaluator without runtime dispatch. */
        template<typename Self, typename... Args>
            requires std::invocable<decltype(std::forward_like<Self>(std::declval<Self>().definition_)), Args...>
        [[nodiscard, gnu::always_inline]] constexpr decltype(auto)
        operator()(this Self&& self, Args&&... args) noexcept(
            std::is_nothrow_invocable_v<decltype(std::forward_like<Self>(self.definition_)), Args...>) {
            return std::invoke(std::forward_like<Self>(self.definition_), std::forward<Args>(args)...);
        }

        /** @brief Access the scalar mathematical definition while preserving cv-ref qualification. */
        template<typename Self>
        [[nodiscard]] constexpr decltype(auto) ScalarDefinition(this Self&& self) noexcept {
            return std::forward_like<Self>(self.definition_);
        }

        /** @brief Access the exact-layout portable program while preserving cv-ref qualification. */
        template<typename Self>
        [[nodiscard]] constexpr decltype(auto) Program(this Self&& self) noexcept {
            return std::forward_like<Self>(self.program_);
        }

        /** @brief Return the canonical identity of the exact-layout bulk program. */
        [[nodiscard]] constexpr Compute::SemanticHash Hash() const noexcept { return program_.Hash(); }

    private:
        SORA_NO_UNIQUE_ADDRESS Definition definition_;
        ProgramType program_;
    };

    template<typename Definition, typename Program>
    ElementwiseDefinition(Definition, Program) -> ElementwiseDefinition<Definition, Program>;

    /** @brief Value exposing direct mathematical evaluation and one exact-layout elementwise program. */
    template<typename T>
    concept CompiledElementwiseDefinition = requires(const T& definition) {
        requires Compute::PortableKernelDefinition<std::remove_cvref_t<decltype(definition.ScalarDefinition())>>;
        { definition.Program().Hash() } -> std::same_as<Compute::SemanticHash>;
        definition.Program().View();
        definition.Program().Arguments();
    };

    /**
     * @brief Compile one mathematical expression for direct use and exact-layout heterogeneous bulk execution.
     * @tparam Scalar Portable scalar domain used to stage the mathematical expression.
     * @tparam Lanes Number of homogeneous components processed by each logical work item.
     * @tparam Policy Numerical transformations permitted for every direct and portable backend.
     * @tparam Function Staged mathematical expression factory.
     * @tparam Inputs Exact runtime projection specifications for the expression inputs.
     * @param[in] function Mathematical expression to stage once at translation time.
     * @param[in] domain Static rank and optional static extents of the bulk index domain.
     * @param[in] output Exact layout, alignment, and alias contract of the output buffer.
     * @param[in] inputs Value or buffer projections in expression-parameter order.
     * @return One definition sharing direct semantics and a canonical exact-layout program.
     * @details The direct evaluator remains a normal inline function object. Runtime planning, JIT, and device
     * selection are reachable only through @ref ElementwiseDefinition::Program.
     */
    template<Compute::PortableScalar Scalar, uint8_t Lanes = 1,
             Compute::NumericalPolicy Policy = kContractedKernelPolicy, typename Function, typename... Inputs>
        requires(Lanes >= 1 && Lanes <= 4) &&
                (std::same_as<std::remove_cvref_t<Inputs>, Compute::ElementwiseInputSpec> && ...)
    [[nodiscard]] consteval auto CompileElementwise(Function&& function, Compute::IndexDomain domain,
                                                    Compute::ElementwiseOutputSpec output, Inputs... inputs) {
        auto definition = CompilePortable<Scalar, sizeof...(Inputs), Policy>(std::forward<Function>(function));
        auto program = Compute::LiftElementwise<Lanes>(definition, domain, output, inputs...);
        return ElementwiseDefinition{std::move(definition), std::move(program)};
    }

} // namespace Sora::Math

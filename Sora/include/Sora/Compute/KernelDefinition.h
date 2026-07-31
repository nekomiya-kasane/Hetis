/**
 * @file KernelDefinition.h
 * @brief Unified direct-execution and portable-module kernel definition.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/KernelIR.h>
#include <Sora/Platform.h>

#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

namespace Sora::Compute {

    /** @brief Value exposing a canonical portable kernel module. */
    template<typename T>
    concept PortableKernelModule = requires(const T& module) {
        { module.Hash() } -> std::same_as<SemanticHash>;
        module.Serialize();
        module.Parameters();
        module.Instructions();
    };

    /** @brief Mathematical definition exposing one canonical portable kernel module. */
    template<typename T>
    concept PortableKernelDefinition = requires(const T& definition) {
        requires PortableKernelModule<std::remove_cvref_t<decltype(definition.PortableModule())>>;
    };

    /**
     * @brief One mathematical definition carrying both its zero-overhead direct evaluator and portable Kernel IR.
     * @details Direct scalar and SIMD calls invoke @p Direct without consulting a runtime planner. The module is read
     * only by bulk execution, compilation, caching, and diagnostic paths.
     */
    template<typename Direct, PortableKernelModule Module>
    class KernelDefinition {
    public:
        /** @brief Construct from the direct evaluator and its semantically equivalent portable module. */
        constexpr KernelDefinition(Direct direct, Module module) noexcept(std::is_nothrow_move_constructible_v<Direct>)
            : direct_(std::move(direct)), module_(std::move(module)) {}

        /** @brief Invoke the direct evaluator without runtime resolution or type erasure. */
        template<typename Self, typename... Args>
            requires std::invocable<decltype(std::forward_like<Self>(std::declval<Self>().direct_)), Args...>
        [[nodiscard, gnu::always_inline]] constexpr decltype(auto)
        operator()(this Self&& self, Args&&... args) noexcept(
            std::is_nothrow_invocable_v<decltype(std::forward_like<Self>(self.direct_)), Args...>) {
            return std::invoke(std::forward_like<Self>(self.direct_), std::forward<Args>(args)...);
        }

        /** @brief Access the direct evaluator while preserving the definition's cv-ref qualification. */
        template<typename Self>
        [[nodiscard]] constexpr decltype(auto) DirectEvaluator(this Self&& self) noexcept {
            return std::forward_like<Self>(self.direct_);
        }

        /** @brief Access the portable module while preserving the definition's cv-ref qualification. */
        template<typename Self>
        [[nodiscard]] constexpr decltype(auto) PortableModule(this Self&& self) noexcept {
            return std::forward_like<Self>(self.module_);
        }

        /** @brief Return the stable semantic identity of the portable module. */
        [[nodiscard]] constexpr SemanticHash Hash() const noexcept { return module_.Hash(); }

    private:
        SORA_NO_UNIQUE_ADDRESS Direct direct_;
        Module module_;
    };

    template<typename Direct, typename Module>
    KernelDefinition(Direct, Module) -> KernelDefinition<Direct, Module>;

} // namespace Sora::Compute

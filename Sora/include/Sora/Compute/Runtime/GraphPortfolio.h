/**
 * @file GraphPortfolio.h
 * @brief Bounded logical graph variants and deterministic physical-candidate ordering.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Runtime/GraphOptimizer.h>
#include <Sora/Compute/Runtime/GraphPlanner.h>

#include <cassert>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace Sora::Compute {

    /** @brief Closed transform family used as the first coordinate of a stable graph-variant identity. */
    enum class GraphVariantKind : uint8_t {
        Original,
        Fused,
        DeviceSpecific,
    };

    /** @brief Stable canonical identity independent of planning completion order and runtime addresses. */
    struct GraphVariantIdentity {
        GraphVariantKind kind = GraphVariantKind::Original;
        uint32_t ordinal = 0;

        friend constexpr bool operator==(GraphVariantIdentity, GraphVariantIdentity) noexcept = default;
        friend constexpr auto operator<=>(GraphVariantIdentity, GraphVariantIdentity) noexcept = default;
    };

    /** @brief One owning logical graph variant paired with its exact planning-time program catalog. */
    struct GraphPortfolioVariant {
        GraphVariantIdentity identity{};
        ExecutionGraph graph;
        ProgramCatalogSnapshot programs;
    };

    /**
     * @brief Request-local bounded logical portfolio retaining the authoritative original and optional fused variant.
     * @details Storage is intentionally hidden so later device-specific transforms can extend the bounded set without
     * changing callers. Canonical iteration always visits the original first and then transform identities in order.
     */
    class GraphPortfolio final {
    public:
        /** @brief Current hard bound: authoritative original plus one canonical fusion transform. */
        static constexpr size_t kMaximumVariantCount = 2;

        /** @brief Build original and optional fused variants without allowing an optional transform to fail submission.
         */
        [[nodiscard]] static GraphPortfolio Create(ExecutionGraph graph, ProgramCatalogSnapshot programs,
                                                   GraphFusionPolicy fusion) noexcept;

        /** @brief Return the canonical number of retained variants. */
        [[nodiscard]] constexpr size_t VariantCount() const noexcept { return fused_.has_value() ? 2U : 1U; }

        /** @brief Borrow the canonical variant at @p index. @pre @p index is less than VariantCount(). */
        [[nodiscard]] const GraphPortfolioVariant& Variant(size_t index) const noexcept {
            assert(index < VariantCount());
            return index == 0 ? original_ : *fused_;
        }

        /** @brief Borrow the authoritative original graph and base catalog. */
        [[nodiscard]] const GraphPortfolioVariant& Original() const noexcept { return original_; }

        /** @brief Return the deterministic result of attempting the optional fusion transform. */
        [[nodiscard]] GraphOptimizationReport Optimization() const noexcept { return optimization_; }

    private:
        GraphPortfolio(GraphPortfolioVariant original, GraphOptimizationReport optimization) noexcept
            : original_(std::move(original)), optimization_(optimization) {}

        GraphPortfolioVariant original_;
        std::optional<GraphPortfolioVariant> fused_{};
        GraphOptimizationReport optimization_{};
    };

    /** @brief One successfully planned variant awaiting objective ordering and exact physical admission. */
    struct GraphPortfolioPlanCandidate {
        GraphVariantIdentity variant{};
        GraphPlanSelection selection{};
    };

    /** @brief Compare candidates by planner objective and then stable variant identity. */
    [[nodiscard]] constexpr bool BetterGraphPortfolioCandidate(const GraphPortfolioPlanCandidate& left,
                                                               const GraphPortfolioPlanCandidate& right) noexcept {
        if (left.selection.predictedMakespanNanoseconds != right.selection.predictedMakespanNanoseconds) {
            return left.selection.predictedMakespanNanoseconds < right.selection.predictedMakespanNanoseconds;
        }
        return left.variant < right.variant;
    }

    static_assert(std::is_trivially_copyable_v<GraphVariantIdentity>);
    static_assert(std::is_nothrow_move_constructible_v<GraphPortfolio>);
    static_assert(std::is_nothrow_move_constructible_v<GraphPortfolioPlanCandidate>);

} // namespace Sora::Compute

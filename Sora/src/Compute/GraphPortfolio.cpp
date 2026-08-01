/**
 * @file GraphPortfolio.cpp
 * @brief Bounded logical graph portfolio construction.
 * @ingroup Compute
 */
#include <Sora/Compute/Runtime/GraphPortfolio.h>

#include <utility>

namespace Sora::Compute {

    GraphPortfolio GraphPortfolio::Create(ExecutionGraph graph, ProgramCatalogSnapshot programs,
                                          GraphFusionPolicy fusion) noexcept {
        GraphPortfolio portfolio{GraphPortfolioVariant{.identity = {GraphVariantKind::Original, 0},
                                                       .graph = std::move(graph),
                                                       .programs = std::move(programs)},
                                 {}};
        if (!fusion.Valid()) {
            portfolio.optimization_ = {
                .status = GraphOptimizationStatus::Failed,
                .issue = GraphOptimizationIssue::InvalidPolicy,
            };
            return portfolio;
        }
        if (!fusion.enabled) {
            portfolio.optimization_.status = GraphOptimizationStatus::Disabled;
            return portfolio;
        }

        auto optimized = OptimizeExecutionGraph(portfolio.original_.graph, portfolio.original_.programs, fusion);
        portfolio.optimization_ = optimized.report;
        if (optimized.report.status == GraphOptimizationStatus::Fused) {
            portfolio.fused_.emplace(GraphPortfolioVariant{
                .identity = {GraphVariantKind::Fused, 0},
                .graph = std::move(optimized.graph),
                .programs = std::move(optimized.programs),
            });
        }
        return portfolio;
    }

} // namespace Sora::Compute

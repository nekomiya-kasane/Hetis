/**
 * @file PhysicalGraphMaterializer.h
 * @brief Transactional conversion of validated physical plans into executable
 * graphs.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/CompilerService.h>
#include <Sora/Compute/PipelineService.h>
#include <Sora/Compute/Runtime/ExecutableGraph.h>
#include <Sora/Compute/Runtime/PhysicalGraphPlan.h>

#include <cstdint>
#include <expected>

namespace Sora::Compute {

    /** @brief Stable failure stage while materializing a side-effect-free physical
     * plan. */
    enum class PhysicalGraphMaterializationErrorCode : uint8_t {
        InvalidPlan,
        DeviceInvalidated,
        MissingPhysicalResource,
        AllocationFailed,
        AliasImportFailed,
        CompilationFailed,
        PipelineResolutionFailed,
        BindingFailed,
        BatchConstructionFailed,
        TransferConstructionFailed,
        CompletionConstructionFailed,
        GraphConstructionFailed,
        OutOfMemory,
    };

    /** @brief Non-owning artifact resolvers required by physical graph
     * materialization. */
    struct ArtifactResolutionServices {
        CompilerService& compiler;
        PipelineService& pipeline;
        KernelPerformanceObserver kernelPerformance{};
    };

    /** @brief Materialization failure with stable location and backend-specific
     * diagnostic fields. */
    struct PhysicalGraphMaterializationError {
        PhysicalGraphMaterializationErrorCode code = PhysicalGraphMaterializationErrorCode::InvalidPlan;
        uint32_t segment = kInvalidPhysicalPlanIndex;
        uint32_t subject = kInvalidPhysicalPlanIndex;
        uint32_t cause = 0;
        uint64_t detail = 0;

        friend constexpr bool operator==(PhysicalGraphMaterializationError,
                                         PhysicalGraphMaterializationError) noexcept = default;
    };

    /**
     * @brief Materialize every allocation, command, dependency, and completion
     * action transactionally.
     * @details No queue or transfer work is submitted. Backend allocations,
     * coherent imports, and compilation may occur; every allocation or coherent
     * import created by this invocation is released in reverse creation order if
     * any later step fails. Compiler and pipeline cache entries retain lifetimes
     * independent of the returned graph.
     */
    [[nodiscard]] std::expected<ExecutableGraph, PhysicalGraphMaterializationError>
    MaterializePhysicalGraph(const PhysicalGraphPlan& plan, const PhysicalResourceCatalog& resources,
                             ArtifactResolutionServices services) noexcept;

} // namespace Sora::Compute

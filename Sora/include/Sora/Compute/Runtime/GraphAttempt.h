/**
 * @file GraphAttempt.h
 * @brief Immutable execution-side input for one fresh recovering submission attempt.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Runtime/ExecutableGraph.h>
#include <Sora/Compute/Runtime/RuntimeEpoch.h>

#include <cstdint>
#include <memory>
#include <span>

namespace Sora::Compute {

    /**
     * @brief Execution-side lease and observations used to build one fresh physical graph attempt.
     * @details Higher-level planners receive a restricted GraphPlanningContext instead of this epoch-owning value.
     */
    struct GraphAttemptContext {
        std::shared_ptr<const RuntimeEpoch> epoch{};
        std::span<const RuntimeDeviceBinding> devices{};
        std::span<const DeviceSnapshot> snapshots{};
        const GraphSubmissionError* previousSubmission = nullptr;
        uint32_t attempt = 0;
    };

} // namespace Sora::Compute

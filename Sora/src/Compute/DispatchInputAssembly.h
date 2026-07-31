/**
 * @file DispatchInputAssembly.h
 * @brief Shared input-assembly feasibility algebra for runtime planners.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Runtime/DispatchCandidateEnumeration.h>
#include <Sora/Compute/Runtime/PhysicalResourceInventory.h>

#include <cstdint>
#include <ranges>

namespace Sora::Compute::Detail {

    /** @brief Strongest physical operation required to join one projection's exact input routes. */
    enum class InputAssemblyRequirement : uint8_t {
        None,
        SignalOnly,
        Copy,
    };

    /** @brief Classify the strongest physical join operation required by one exact residency projection. */
    [[nodiscard]] inline InputAssemblyRequirement
    ClassifyInputAssembly(const DispatchResidencyProjection& projection) noexcept {
        InputAssemblyRequirement requirement = InputAssemblyRequirement::None;
        for (const DispatchMemoryBinding& binding : projection.bindings) {
            if (binding.access != AccessMode::Read) {
                continue;
            }
            const BufferId buffer = binding.buffer.Describe().id;
            const auto routeCount = std::ranges::count(projection.inputs, buffer, &GraphResidencyRoute::buffer);
            if (routeCount <= 1) {
                continue;
            }
            requirement = InputAssemblyRequirement::SignalOnly;
            if (std::ranges::any_of(projection.inputs, [&](const GraphResidencyRoute& route) {
                    return route.buffer == buffer &&
                           (route.hops.empty() || route.hops.back().kind == TransferKind::CoherentAlias);
                })) {
                return InputAssemblyRequirement::Copy;
            }
        }
        return requirement;
    }

    /** @brief Return whether @p resources can execute every input assembly required by @p projection. */
    [[nodiscard]] inline bool CanAssembleDispatchInputs(const DispatchResidencyProjection& projection,
                                                        const PhysicalResourceInventory& resources) noexcept {
        const InputAssemblyRequirement requirement = ClassifyInputAssembly(projection);
        if (requirement == InputAssemblyRequirement::None) {
            return true;
        }
        if (!projection.destination) {
            return false;
        }
        return std::ranges::any_of(resources.Queues(), [&](const PhysicalQueueDescriptor& queue) {
            return queue.service == projection.service && queue.backend == projection.destination->location.backend &&
                   queue.device == projection.device &&
                   (requirement != InputAssemblyRequirement::Copy ||
                    queue.capabilities.Supports(BackendCommandKind::Copy));
        });
    }

} // namespace Sora::Compute::Detail

/**
 * @file CalibratedTransferTopology.cpp
 * @brief Payload-specific transfer topology projection implementation.
 * @ingroup Compute
 */

#include <Sora/Compute/Runtime/CalibratedTransferTopology.h>

#include <new>

namespace Sora::Compute {

    std::expected<CalibratedTransferTopology, CalibratedTransferTopologyError>
    ProjectCalibratedTransferTopology(const PhysicalResourceInventory& resources,
                                      const PerformanceSnapshot& performance, uint64_t bytes) noexcept {
        if (bytes == 0) {
            return std::unexpected(CalibratedTransferTopologyError::InvalidRequest);
        }
        try {
            std::vector<TransferLink> links;
            links.reserve(resources.Transfers().size());
            for (const PhysicalTransferDescriptor& descriptor : resources.Transfers()) {
                if (!descriptor.Valid()) {
                    return std::unexpected(CalibratedTransferTopologyError::InvalidDescriptor);
                }
                const TransferCalibrationKey key{.service = descriptor.service,
                                                 .engine = descriptor.engine,
                                                 .source = descriptor.source,
                                                 .destination = descriptor.destination};
                const auto estimate = performance.Estimate(key, bytes);
                if (!estimate) {
                    continue;
                }
                links.push_back({.sourceService = descriptor.sourceService,
                                 .service = descriptor.service,
                                 .engine = descriptor.engine,
                                 .source = descriptor.source,
                                 .destination = descriptor.destination,
                                 .kind = descriptor.kind,
                                 .bytesPerSecond = estimate->bytesPerSecond,
                                 .latencyNanoseconds = estimate->latencyNanoseconds});
            }
            return CalibratedTransferTopology{bytes, std::move(links)};
        } catch (const std::bad_alloc&) {
            return std::unexpected(CalibratedTransferTopologyError::OutOfMemory);
        }
    }

} // namespace Sora::Compute

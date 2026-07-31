/**
 * @file CalibratedTransferTopology.h
 * @brief Immutable payload-specific transfer topology projected from physical identities and measured calibration.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Runtime/PerformanceModel.h>
#include <Sora/Compute/Runtime/PhysicalResourceInventory.h>

#include <cstdint>
#include <expected>
#include <span>
#include <utility>
#include <vector>

namespace Sora::Compute {

    /** @brief Stable failure while joining topology identities with immutable transfer calibration. */
    enum class CalibratedTransferTopologyError : uint8_t {
        InvalidRequest,
        InvalidDescriptor,
        OutOfMemory,
    };

    /** @brief Pure directed links whose costs were calibrated for one payload magnitude. */
    class CalibratedTransferTopology final {
    public:
        /** @brief Return the payload magnitude used to choose every calibration bucket. */
        [[nodiscard]] uint64_t Bytes() const noexcept { return bytes_; }

        /** @brief Return measured directed links; uncalibrated physical edges are absent. */
        [[nodiscard]] std::span<const TransferLink> Links() const noexcept { return links_; }

    private:
        friend std::expected<CalibratedTransferTopology, CalibratedTransferTopologyError>
        ProjectCalibratedTransferTopology(const PhysicalResourceInventory&, const PerformanceSnapshot&,
                                          uint64_t) noexcept;

        CalibratedTransferTopology(uint64_t bytes, std::vector<TransferLink> links) noexcept
            : bytes_(bytes), links_(std::move(links)) {}

        uint64_t bytes_ = 0;
        std::vector<TransferLink> links_{};
    };

    /** @brief Project physical transfer descriptors into measured links for exactly @p bytes. */
    [[nodiscard]] std::expected<CalibratedTransferTopology, CalibratedTransferTopologyError>
    ProjectCalibratedTransferTopology(const PhysicalResourceInventory& resources,
                                      const PerformanceSnapshot& performance, uint64_t bytes) noexcept;

} // namespace Sora::Compute

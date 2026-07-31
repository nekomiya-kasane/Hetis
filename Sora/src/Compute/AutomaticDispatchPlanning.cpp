#include <Sora/Compute/Runtime/AutomaticDispatchPlanning.h>

#include <Sora/Compute/Cost.h>

#include "AutomaticDispatchPlanningInternal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Sora::Compute {

    namespace {

        struct EligibleCandidate {
            size_t originalIndex = 0;
            uint64_t fixedNanoseconds = 0;
        };

        struct DeviceCapacity {
            EligibleCandidate candidate{};
            uint64_t axisUnits = 0;
        };

        struct DeviceCapacitySet {
            std::array<DeviceCapacity, kMaximumAutomaticPartitionDevices> devices{};
            size_t size = 0;
            uint64_t totalAxisUnits = 0;
        };

        struct PartitionSeed {
            std::vector<size_t> placements{};
            std::vector<uint64_t> extents{};
        };

        struct PartitionSearchState {
            std::vector<size_t> placements{};
            std::vector<uint64_t> extents{};
            PartitionedDispatchExecution execution{};
        };

        struct AxisPartitionSearch {
            uint8_t axis = 0;
            uint64_t axisUnits = 0;
            uint64_t workItemsPerAxisUnit = 0;
            size_t maximumDevices = 0;
            PartitionSearchState state{};
            std::optional<PartitionSearchState> deviceProposalState{};
            size_t deviceOrdinal = 0;
            size_t boundaryOrdinal = 0;
            size_t deviceBoundaryOrdinal = 0;
            bool deviceExhausted = false;
            bool boundaryExhausted = false;
            bool deviceTurn = true;

            [[nodiscard]] bool Exhausted() const noexcept { return deviceExhausted && boundaryExhausted; }
        };

        [[nodiscard]] constexpr bool StableCandidateLess(const PlacementCandidate& left,
                                                         const PlacementCandidate& right) noexcept {
            if (left.device.id != right.device.id) {
                return left.device.id < right.device.id;
            }
            if (left.service != right.service) {
                return left.service < right.service;
            }
            return left.queue < right.queue;
        }

        [[nodiscard]] constexpr uint64_t FixedCost(const WorkloadDesc& workload,
                                                   const PlacementCandidate& candidate) noexcept {
            WorkloadDesc unit = workload;
            unit.workItemCount = 1;
            const CostBreakdown cost = Detail::Estimate(unit, candidate);
            const uint64_t compute = Detail::RateCost(1, candidate.workItemsPerSecond);
            return cost.totalNanoseconds >= compute ? cost.totalNanoseconds - compute : 0;
        }

        [[nodiscard]] constexpr bool Dominates(const EligibleCandidate& left, const EligibleCandidate& right,
                                               std::span<const PlacementCandidate> candidates) noexcept {
            const PlacementCandidate& leftCandidate = candidates[left.originalIndex];
            const PlacementCandidate& rightCandidate = candidates[right.originalIndex];
            return left.fixedNanoseconds <= right.fixedNanoseconds &&
                   leftCandidate.workItemsPerSecond >= rightCandidate.workItemsPerSecond &&
                   (left.fixedNanoseconds < right.fixedNanoseconds ||
                    leftCandidate.workItemsPerSecond > rightCandidate.workItemsPerSecond ||
                    StableCandidateLess(leftCandidate, rightCandidate));
        }

        [[nodiscard]] bool EligibleLess(const EligibleCandidate& left, const EligibleCandidate& right,
                                        std::span<const PlacementCandidate> candidates) noexcept {
            const PlacementCandidate& leftCandidate = candidates[left.originalIndex];
            const PlacementCandidate& rightCandidate = candidates[right.originalIndex];
            if (StableCandidateLess(leftCandidate, rightCandidate)) {
                return true;
            }
            if (StableCandidateLess(rightCandidate, leftCandidate)) {
                return false;
            }
            return left.originalIndex < right.originalIndex;
        }

        [[nodiscard]] bool CapacityBetter(const DeviceCapacity& left, const DeviceCapacity& right,
                                          std::span<const PlacementCandidate> candidates) noexcept {
            return left.axisUnits != right.axisUnits ? left.axisUnits > right.axisUnits
                                                     : EligibleLess(left.candidate, right.candidate, candidates);
        }

        [[nodiscard]] std::vector<EligibleCandidate>
        CollectEligibleCandidates(const WorkloadDesc& workload, std::span<const PlacementCandidate> candidates,
                                  const PlacementPolicy& policy, bool requireTransferFree) {
            std::vector<EligibleCandidate> eligible;
            eligible.reserve(candidates.size());
            for (size_t index = 0; index < candidates.size(); ++index) {
                const PlacementCandidate& candidate = candidates[index];
                const auto execution = PlanExecution(workload, {&candidate, 1}, policy);
                if (!execution ||
                    (requireTransferFree && candidate.transfer &&
                     (candidate.transfer->bytes != 0 || candidate.transfer->nanoseconds != 0)) ||
                    execution->cost.totalNanoseconds == std::numeric_limits<uint64_t>::max()) {
                    continue;
                }
                const EligibleCandidate item{.originalIndex = index,
                                             .fixedNanoseconds = FixedCost(workload, candidate)};
                const bool dominated = std::ranges::any_of(eligible, [&](const EligibleCandidate& existing) {
                    return candidates[existing.originalIndex].device.id == candidate.device.id &&
                           Dominates(existing, item, candidates);
                });
                if (dominated) {
                    continue;
                }
                std::erase_if(eligible, [&](const EligibleCandidate& existing) {
                    return candidates[existing.originalIndex].device.id == candidate.device.id &&
                           Dominates(item, existing, candidates);
                });
                eligible.push_back(item);
            }
            std::ranges::sort(eligible, [&](const EligibleCandidate& left, const EligibleCandidate& right) {
                return EligibleLess(left, right, candidates);
            });
            return eligible;
        }

        [[nodiscard]] constexpr uint64_t AxisUnitWorkItems(const DispatchDomain& domain, uint8_t axis) noexcept {
            return domain.workItemCount / domain.extents[axis];
        }

        [[nodiscard]] constexpr bool AxisEnabled(uint8_t mask, uint8_t axis) noexcept {
            return (mask & static_cast<uint8_t>(uint8_t{1} << axis)) != 0;
        }

        [[nodiscard]] constexpr uint8_t CandidateAxisMask(const DispatchDomain& domain, PartitionContract contract,
                                                          const AutomaticPartitionPolicy& policy) noexcept {
            if (!policy.enabled || contract.kind != PartitionKind::Elementwise) {
                return 0;
            }
            uint8_t mask = static_cast<uint8_t>(contract.splittableAxisMask & policy.allowedAxisMask);
            for (uint8_t axis = 0; axis < domain.rank; ++axis) {
                if (domain.extents[axis] < 2) {
                    mask &= static_cast<uint8_t>(~(uint8_t{1} << axis));
                }
            }
            return mask;
        }

        [[nodiscard]] constexpr uint64_t CapacityAt(uint64_t nanoseconds, uint64_t fixedNanoseconds,
                                                    uint64_t workItemsPerSecond, uint64_t workItemsPerAxisUnit,
                                                    uint64_t maximumUnits) noexcept {
            if (nanoseconds <= fixedNanoseconds || workItemsPerSecond == 0 || workItemsPerAxisUnit == 0) {
                return 0;
            }
            constexpr uint64_t nanosecondsPerSecond = 1'000'000'000;
            const __uint128_t availableWork =
                static_cast<__uint128_t>(nanoseconds - fixedNanoseconds) * workItemsPerSecond;
            uint64_t lower = 0;
            uint64_t upper = maximumUnits;
            while (lower < upper) {
                const uint64_t middle = lower + (upper - lower) / 2 + 1;
                const __uint128_t requiredWork =
                    static_cast<__uint128_t>(middle) * workItemsPerAxisUnit * nanosecondsPerSecond;
                if (requiredWork <= availableWork) {
                    lower = middle;
                } else {
                    upper = middle - 1;
                }
            }
            return lower;
        }

        [[nodiscard]] bool FeasibleAt(uint64_t nanoseconds, std::span<const EligibleCandidate> selected,
                                      std::span<const PlacementCandidate> candidates, uint64_t workItemsPerAxisUnit,
                                      uint64_t axisUnits) noexcept {
            uint64_t total = 0;
            for (const EligibleCandidate& item : selected) {
                const uint64_t capacity =
                    CapacityAt(nanoseconds, item.fixedNanoseconds, candidates[item.originalIndex].workItemsPerSecond,
                               workItemsPerAxisUnit, axisUnits);
                if (capacity == 0) {
                    return false;
                }
                total = Detail::SaturatingAdd(total, capacity);
            }
            return total >= axisUnits;
        }

        void InsertDeviceCapacity(DeviceCapacitySet& selected, DeviceCapacity choice, size_t maximumDevices,
                                  std::span<const PlacementCandidate> candidates) noexcept {
            size_t position = 0;
            while (position < selected.size && CapacityBetter(selected.devices[position], choice, candidates)) {
                ++position;
            }
            if (position >= maximumDevices) {
                return;
            }
            const size_t newSize = std::min(selected.size + 1, maximumDevices);
            for (size_t index = newSize - 1; index > position; --index) {
                selected.devices[index] = selected.devices[index - 1];
            }
            selected.devices[position] = choice;
            selected.size = newSize;
        }

        [[nodiscard]] DeviceCapacitySet SelectDeviceCapacities(uint64_t nanoseconds,
                                                               std::span<const EligibleCandidate> eligible,
                                                               std::span<const PlacementCandidate> candidates,
                                                               uint64_t workItemsPerAxisUnit, uint64_t axisUnits,
                                                               size_t maximumDevices) noexcept {
            DeviceCapacitySet selected;
            size_t begin = 0;
            while (begin < eligible.size()) {
                const DeviceId device = candidates[eligible[begin].originalIndex].device.id;
                DeviceCapacity best{};
                size_t end = begin;
                for (; end < eligible.size() && candidates[eligible[end].originalIndex].device.id == device; ++end) {
                    const EligibleCandidate candidate = eligible[end];
                    const uint64_t capacity = CapacityAt(nanoseconds, candidate.fixedNanoseconds,
                                                         candidates[candidate.originalIndex].workItemsPerSecond,
                                                         workItemsPerAxisUnit, axisUnits);
                    const DeviceCapacity choice{.candidate = candidate, .axisUnits = capacity};
                    if (capacity != 0 && (best.axisUnits == 0 || CapacityBetter(choice, best, candidates))) {
                        best = choice;
                    }
                }
                if (best.axisUnits != 0) {
                    InsertDeviceCapacity(selected, best, maximumDevices, candidates);
                }
                begin = end;
            }
            for (size_t index = 0; index < selected.size; ++index) {
                selected.totalAxisUnits =
                    Detail::SaturatingAdd(selected.totalAxisUnits, selected.devices[index].axisUnits);
            }
            return selected;
        }

        [[nodiscard]] bool GloballyFeasibleAt(uint64_t nanoseconds, std::span<const EligibleCandidate> eligible,
                                              std::span<const PlacementCandidate> candidates,
                                              uint64_t workItemsPerAxisUnit, uint64_t axisUnits,
                                              size_t maximumDevices) noexcept {
            const DeviceCapacitySet selected = SelectDeviceCapacities(nanoseconds, eligible, candidates,
                                                                      workItemsPerAxisUnit, axisUnits, maximumDevices);
            return selected.size >= 2 && selected.totalAxisUnits >= axisUnits;
        }

        [[nodiscard]] std::optional<uint64_t> GlobalMinimumParallelTime(std::span<const EligibleCandidate> eligible,
                                                                        std::span<const PlacementCandidate> candidates,
                                                                        uint64_t workItemsPerAxisUnit,
                                                                        uint64_t axisUnits,
                                                                        size_t maximumDevices) noexcept {
            uint64_t high = 0;
            const uint64_t totalWorkItems = axisUnits * workItemsPerAxisUnit;
            for (const EligibleCandidate& item : eligible) {
                const PlacementCandidate& candidate = candidates[item.originalIndex];
                high = std::max(high,
                                Detail::SaturatingAdd(item.fixedNanoseconds,
                                                      Detail::RateCost(totalWorkItems, candidate.workItemsPerSecond)));
            }
            if (!GloballyFeasibleAt(high, eligible, candidates, workItemsPerAxisUnit, axisUnits, maximumDevices)) {
                return std::nullopt;
            }
            uint64_t low = 0;
            while (low < high) {
                const uint64_t middle = low + (high - low) / 2;
                if (GloballyFeasibleAt(middle, eligible, candidates, workItemsPerAxisUnit, axisUnits, maximumDevices)) {
                    high = middle;
                } else {
                    low = middle + 1;
                }
            }
            return low;
        }

        [[nodiscard]] std::vector<EligibleCandidate>
        SelectExactCandidates(uint64_t nanoseconds, std::span<const EligibleCandidate> eligible,
                              std::span<const PlacementCandidate> candidates, uint64_t workItemsPerAxisUnit,
                              uint64_t axisUnits, size_t maximumDevices) {
            const DeviceCapacitySet capacities = SelectDeviceCapacities(
                nanoseconds, eligible, candidates, workItemsPerAxisUnit, axisUnits, maximumDevices);
            uint64_t covered = 0;
            size_t selectedCount = 0;
            while (selectedCount < capacities.size && (selectedCount < 2 || covered < axisUnits)) {
                covered = Detail::SaturatingAdd(covered, capacities.devices[selectedCount].axisUnits);
                ++selectedCount;
            }
            std::vector<EligibleCandidate> selected;
            selected.reserve(selectedCount);
            for (size_t index = 0; index < selectedCount; ++index) {
                selected.push_back(capacities.devices[index].candidate);
            }
            return selected;
        }

        [[nodiscard]] std::optional<uint64_t> MinimumParallelTime(std::span<const EligibleCandidate> selected,
                                                                  std::span<const PlacementCandidate> candidates,
                                                                  uint64_t workItemsPerAxisUnit,
                                                                  uint64_t axisUnits) noexcept {
            uint64_t high = 0;
            const uint64_t totalWorkItems = axisUnits * workItemsPerAxisUnit;
            for (const EligibleCandidate& item : selected) {
                const PlacementCandidate& candidate = candidates[item.originalIndex];
                high = std::max(high,
                                Detail::SaturatingAdd(item.fixedNanoseconds,
                                                      Detail::RateCost(totalWorkItems, candidate.workItemsPerSecond)));
            }
            if (!FeasibleAt(high, selected, candidates, workItemsPerAxisUnit, axisUnits)) {
                return std::nullopt;
            }
            uint64_t low = 0;
            while (low < high) {
                const uint64_t middle = low + (high - low) / 2;
                if (FeasibleAt(middle, selected, candidates, workItemsPerAxisUnit, axisUnits)) {
                    high = middle;
                } else {
                    low = middle + 1;
                }
            }
            return low;
        }

        [[nodiscard]] std::optional<std::vector<uint64_t>>
        IntegerCoverExtents(uint64_t parallelTime, std::span<const EligibleCandidate> selected,
                            std::span<const PlacementCandidate> candidates, uint64_t workItemsPerAxisUnit,
                            uint64_t axisUnits) {
            if (selected.size() < 2 || selected.size() > axisUnits) {
                return std::nullopt;
            }
            std::vector<uint64_t> extents(selected.size(), 1);
            uint64_t remaining = axisUnits - selected.size();
            for (size_t index = 0; index < selected.size() && remaining != 0; ++index) {
                const EligibleCandidate item = selected[index];
                const uint64_t capacity =
                    CapacityAt(parallelTime, item.fixedNanoseconds, candidates[item.originalIndex].workItemsPerSecond,
                               workItemsPerAxisUnit, axisUnits);
                if (capacity == 0) {
                    return std::nullopt;
                }
                const uint64_t additional = std::min(remaining, capacity - 1);
                extents[index] += additional;
                remaining -= additional;
            }
            return remaining == 0 ? std::optional<std::vector<uint64_t>>{std::move(extents)} : std::nullopt;
        }

        [[nodiscard]] std::optional<PartitionSeed> SeedGlobalDeviceSet(std::span<const EligibleCandidate> eligible,
                                                                       std::span<const PlacementCandidate> candidates,
                                                                       uint64_t workItemsPerAxisUnit,
                                                                       uint64_t axisUnits, size_t maximumDevices) {
            const auto parallelTime =
                GlobalMinimumParallelTime(eligible, candidates, workItemsPerAxisUnit, axisUnits, maximumDevices);
            if (!parallelTime) {
                return std::nullopt;
            }
            std::vector<EligibleCandidate> selected = SelectExactCandidates(
                *parallelTime, eligible, candidates, workItemsPerAxisUnit, axisUnits, maximumDevices);
            std::ranges::sort(selected, [&](const EligibleCandidate& left, const EligibleCandidate& right) {
                return EligibleLess(left, right, candidates);
            });
            auto extents = IntegerCoverExtents(*parallelTime, selected, candidates, workItemsPerAxisUnit, axisUnits);
            if (!extents) {
                return std::nullopt;
            }
            PartitionSeed result{.extents = std::move(*extents)};
            result.placements.reserve(selected.size());
            for (const EligibleCandidate item : selected) {
                result.placements.push_back(item.originalIndex);
            }
            return result;
        }

        [[nodiscard]] std::optional<uint64_t> ExactDeviceSetMinimumTime(std::span<const EligibleCandidate> eligible,
                                                                        std::span<const PlacementCandidate> candidates,
                                                                        uint64_t workItemsPerAxisUnit,
                                                                        uint64_t axisUnits,
                                                                        size_t deviceCount) noexcept {
            uint64_t high = 0;
            const uint64_t totalWorkItems = axisUnits * workItemsPerAxisUnit;
            for (const EligibleCandidate& item : eligible) {
                const PlacementCandidate& candidate = candidates[item.originalIndex];
                high = std::max(high,
                                Detail::SaturatingAdd(item.fixedNanoseconds,
                                                      Detail::RateCost(totalWorkItems, candidate.workItemsPerSecond)));
            }
            const auto feasible = [&](uint64_t nanoseconds) noexcept {
                const DeviceCapacitySet selected = SelectDeviceCapacities(nanoseconds, eligible, candidates,
                                                                          workItemsPerAxisUnit, axisUnits, deviceCount);
                return selected.size == deviceCount && selected.totalAxisUnits >= axisUnits;
            };
            if (!feasible(high)) {
                return std::nullopt;
            }
            uint64_t low = 0;
            while (low < high) {
                const uint64_t middle = low + (high - low) / 2;
                if (feasible(middle)) {
                    high = middle;
                } else {
                    low = middle + 1;
                }
            }
            return low;
        }

        [[nodiscard]] std::optional<PartitionSeed> SeedExactDeviceSet(std::span<const DeviceId> devices,
                                                                      std::span<const EligibleCandidate> eligible,
                                                                      std::span<const PlacementCandidate> candidates,
                                                                      uint64_t workItemsPerAxisUnit,
                                                                      uint64_t axisUnits) {
            if (devices.size() < 2 || devices.size() > axisUnits) {
                return std::nullopt;
            }
            std::vector<EligibleCandidate> constrained;
            constrained.reserve(eligible.size());
            for (const EligibleCandidate item : eligible) {
                if (std::ranges::binary_search(devices, candidates[item.originalIndex].device.id)) {
                    constrained.push_back(item);
                }
            }
            const auto parallelTime =
                ExactDeviceSetMinimumTime(constrained, candidates, workItemsPerAxisUnit, axisUnits, devices.size());
            if (!parallelTime) {
                return std::nullopt;
            }
            const DeviceCapacitySet capacities = SelectDeviceCapacities(
                *parallelTime, constrained, candidates, workItemsPerAxisUnit, axisUnits, devices.size());
            if (capacities.size != devices.size()) {
                return std::nullopt;
            }
            std::vector<EligibleCandidate> selected;
            selected.reserve(capacities.size);
            for (size_t index = 0; index < capacities.size; ++index) {
                selected.push_back(capacities.devices[index].candidate);
            }
            std::ranges::sort(selected, [&](const EligibleCandidate& left, const EligibleCandidate& right) {
                return EligibleLess(left, right, candidates);
            });
            auto extents = IntegerCoverExtents(*parallelTime, selected, candidates, workItemsPerAxisUnit, axisUnits);
            if (!extents) {
                return std::nullopt;
            }
            PartitionSeed result{.extents = std::move(*extents)};
            result.placements.reserve(selected.size());
            for (const EligibleCandidate item : selected) {
                result.placements.push_back(item.originalIndex);
            }
            return result;
        }

        [[nodiscard]] std::vector<DeviceId> AvailableDevices(std::span<const EligibleCandidate> eligible,
                                                             std::span<const PlacementCandidate> candidates) {
            std::vector<DeviceId> devices;
            devices.reserve(eligible.size());
            for (const EligibleCandidate item : eligible) {
                const DeviceId device = candidates[item.originalIndex].device.id;
                if (devices.empty() || devices.back() != device) {
                    devices.push_back(device);
                }
            }
            return devices;
        }

        [[nodiscard]] std::vector<DeviceId> SelectedDevices(std::span<const size_t> placements,
                                                            std::span<const PlacementCandidate> candidates) {
            std::vector<DeviceId> devices;
            devices.reserve(placements.size());
            for (const size_t placement : placements) {
                const DeviceId device = candidates[placement].device.id;
                if (devices.empty() || devices.back() != device) {
                    devices.push_back(device);
                }
            }
            return devices;
        }

        [[nodiscard]] std::optional<std::vector<DeviceId>> DeviceSetProposal(std::span<const DeviceId> selected,
                                                                             std::span<const DeviceId> available,
                                                                             size_t maximumDevices, size_t ordinal) {
            size_t current = 0;
            const auto emit = [&](std::vector<DeviceId> proposal) -> std::optional<std::vector<DeviceId>> {
                if (current++ != ordinal) {
                    return std::nullopt;
                }
                std::ranges::sort(proposal);
                return proposal;
            };
            for (size_t removed = 0; removed < selected.size(); ++removed) {
                for (const DeviceId added : available) {
                    if (std::ranges::binary_search(selected, added)) {
                        continue;
                    }
                    std::vector<DeviceId> proposal{selected.begin(), selected.end()};
                    proposal[removed] = added;
                    if (auto result = emit(std::move(proposal))) {
                        return result;
                    }
                }
            }
            if (selected.size() < maximumDevices) {
                for (const DeviceId added : available) {
                    if (std::ranges::binary_search(selected, added)) {
                        continue;
                    }
                    std::vector<DeviceId> proposal{selected.begin(), selected.end()};
                    proposal.push_back(added);
                    if (auto result = emit(std::move(proposal))) {
                        return result;
                    }
                }
            }
            if (selected.size() > 2) {
                for (size_t removed = 0; removed < selected.size(); ++removed) {
                    std::vector<DeviceId> proposal{selected.begin(), selected.end()};
                    proposal.erase(proposal.begin() + static_cast<std::ptrdiff_t>(removed));
                    if (auto result = emit(std::move(proposal))) {
                        return result;
                    }
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::vector<uint64_t>> BoundaryProposal(std::span<const uint64_t> extents,
                                                                            uint64_t axisUnits, size_t ordinal) {
            size_t current = 0;
            for (uint64_t step = std::bit_floor(axisUnits / 2); step != 0; step /= 2) {
                for (size_t boundary = 0; boundary + 1 < extents.size(); ++boundary) {
                    for (const bool moveRight : {false, true}) {
                        const size_t source = moveRight ? boundary : boundary + 1;
                        const size_t target = moveRight ? boundary + 1 : boundary;
                        if (extents[source] <= step) {
                            continue;
                        }
                        if (current++ != ordinal) {
                            continue;
                        }
                        std::vector<uint64_t> proposal{extents.begin(), extents.end()};
                        proposal[source] -= step;
                        proposal[target] += step;
                        return proposal;
                    }
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] WorkloadDesc ShardWorkload(const WorkloadDesc& workload, uint64_t workItemCount) noexcept {
            WorkloadDesc shard = workload;
            shard.workItemCount = workItemCount;
            return shard;
        }

        [[nodiscard]] std::expected<std::optional<PartitionedDispatchExecution>, AutomaticDispatchPlanningError>
        EvaluateSelection(const DispatchDomain& domain, PartitionContract contract, const WorkloadDesc& workload,
                          std::span<const PlacementCandidate> candidates, std::vector<EligibleCandidate> selected,
                          uint8_t axis, const AutomaticPartitionPolicy& policy) noexcept {
            if (selected.size() < 2 || selected.size() > domain.extents[axis]) {
                return std::optional<PartitionedDispatchExecution>{};
            }
            std::ranges::sort(selected, [&](const EligibleCandidate& left, const EligibleCandidate& right) {
                return EligibleLess(left, right, candidates);
            });
            const uint64_t axisUnits = domain.extents[axis];
            const uint64_t workItemsPerAxisUnit = AxisUnitWorkItems(domain, axis);
            const auto parallelTime = MinimumParallelTime(selected, candidates, workItemsPerAxisUnit, axisUnits);
            if (!parallelTime) {
                return std::optional<PartitionedDispatchExecution>{};
            }

            try {
                auto shardExtents =
                    IntegerCoverExtents(*parallelTime, selected, candidates, workItemsPerAxisUnit, axisUnits);
                if (!shardExtents) {
                    return std::optional<PartitionedDispatchExecution>{};
                }
                auto partition = ElementwisePartitionPlan::Create(domain, contract, axis, *shardExtents);
                if (!partition) {
                    return std::unexpected(AutomaticDispatchPlanningError::InvalidRequest);
                }

                PartitionedDispatchExecution result{.axis = axis,
                                                    .partitionOverheadNanoseconds = policy.partitionOverheadNanoseconds,
                                                    .publicationNanoseconds = policy.publicationNanoseconds};
                result.shards.reserve(selected.size());
                for (size_t index = 0; index < selected.size(); ++index) {
                    const EligibleCandidate item = selected[index];
                    const PlacementCandidate& candidate = candidates[item.originalIndex];
                    const WorkloadDesc shard = ShardWorkload(workload, partition->Shards()[index].workItemCount);
                    auto execution =
                        PlanExecution(shard, std::span<const PlacementCandidate>{&candidate, 1}, policy.placement);
                    if (!execution) {
                        return std::optional<PartitionedDispatchExecution>{};
                    }
                    execution->candidateIndex = item.originalIndex;
                    result.parallelNanoseconds = std::max(result.parallelNanoseconds, execution->cost.totalNanoseconds);
                    result.shards.push_back({partition->Shards()[index], std::move(*execution)});
                }
                result.totalNanoseconds =
                    Detail::SaturatingAdd(result.parallelNanoseconds, result.partitionOverheadNanoseconds);
                result.totalNanoseconds = Detail::SaturatingAdd(result.totalNanoseconds, result.publicationNanoseconds);
                return std::optional<PartitionedDispatchExecution>{std::move(result)};
            } catch (const std::bad_alloc&) {
                return std::unexpected(AutomaticDispatchPlanningError::OutOfMemory);
            }
        }

        [[nodiscard]] std::expected<bool, AutomaticDispatchPlanningError>
        ScheduleResidentPartition(PartitionedDispatchExecution& candidate) {
            try {
                std::vector<RouteTransferBatch> batches;
                batches.reserve(candidate.shards.size());
                for (uint32_t index = 0; index < candidate.shards.size(); ++index) {
                    const PartitionShardExecution& shard = candidate.shards[index];
                    batches.push_back({.operation = index,
                                       .dispatchNanoseconds = shard.execution.cost.totalNanoseconds,
                                       .routes = shard.residency.inputs});
                }
                auto transferSchedule = ScheduleRouteTransfers(batches);
                if (!transferSchedule) {
                    return transferSchedule.error() == RouteTransferSchedulingError::OutOfMemory
                               ? std::unexpected(AutomaticDispatchPlanningError::OutOfMemory)
                               : std::expected<bool, AutomaticDispatchPlanningError>{false};
                }
                candidate.transfers = std::move(transferSchedule->transfers);
                candidate.transferReadyNanoseconds = transferSchedule->makespanNanoseconds;
                for (uint32_t index = 0; index < candidate.shards.size(); ++index) {
                    PartitionShardExecution& shard = candidate.shards[index];
                    shard.routeReadyNanoseconds = std::move(transferSchedule->routeReadyNanoseconds[index]);
                    shard.inputReadyNanoseconds = transferSchedule->readyNanoseconds[index];
                    shard.startNanoseconds = shard.inputReadyNanoseconds;
                    shard.finishNanoseconds =
                        Detail::SaturatingAdd(shard.startNanoseconds, shard.execution.cost.totalNanoseconds);
                    candidate.parallelNanoseconds = std::max(candidate.parallelNanoseconds, shard.finishNanoseconds);
                }
                candidate.totalNanoseconds =
                    Detail::SaturatingAdd(candidate.parallelNanoseconds, candidate.partitionOverheadNanoseconds);
                candidate.totalNanoseconds =
                    Detail::SaturatingAdd(candidate.totalNanoseconds, candidate.publicationNanoseconds);
                return candidate.totalNanoseconds != std::numeric_limits<uint64_t>::max();
            } catch (const std::bad_alloc&) {
                return std::unexpected(AutomaticDispatchPlanningError::OutOfMemory);
            }
        }

        [[nodiscard]] std::expected<std::optional<PartitionedDispatchExecution>, AutomaticDispatchPlanningError>
        EvaluateExactPartition(const KernelInvocation& invocation, const DispatchCandidateSet& candidates,
                               const GraphPlanningServices& services, const GraphResidencyState& residency,
                               std::span<const PlacementCandidate> computeCandidates,
                               const AutomaticPartitionPolicy& policy, uint8_t axis,
                               std::span<const size_t> placementIndices, std::span<const uint64_t> extents,
                               uint32_t& evaluations, Detail::AutomaticPartitionSchedulerRef scheduler) {
            ++evaluations;
            auto cover = ElementwisePartitionPlan::Create(invocation.Domain(), invocation.Partition(), axis, extents);
            if (!cover || cover->Shards().size() != placementIndices.size()) {
                return std::optional<PartitionedDispatchExecution>{};
            }
            PartitionedDispatchExecution candidate{.axis = axis,
                                                   .partitionEvaluations = evaluations,
                                                   .partitionOverheadNanoseconds = policy.partitionOverheadNanoseconds,
                                                   .publicationNanoseconds = policy.publicationNanoseconds};
            candidate.shards.reserve(cover->Shards().size());
            for (size_t index = 0; index < cover->Shards().size(); ++index) {
                const size_t placementIndex = placementIndices[index];
                if (placementIndex >= candidates.placements.size()) {
                    return std::unexpected(AutomaticDispatchPlanningError::InvalidRequest);
                }
                const DispatchPlacementCandidate& selected = candidates.placements[placementIndex];
                const DispatchResidencyProjection& destination = candidates.residencies[selected.residencyProjection];
                auto projection = ProjectDispatchSliceResidency(invocation, candidates.node, cover->Shards()[index],
                                                                destination, services, residency);
                if (!projection) {
                    if (projection.error().code == DispatchCandidateEnumerationErrorCode::OutOfMemory) {
                        return std::unexpected(AutomaticDispatchPlanningError::OutOfMemory);
                    }
                    return std::optional<PartitionedDispatchExecution>{};
                }
                const WorkloadDesc shardWorkload =
                    ShardWorkload(candidates.workload, cover->Shards()[index].workItemCount);
                const PlacementCandidate& computeCandidate = computeCandidates[placementIndex];
                auto execution = PlanExecution(shardWorkload, {&computeCandidate, 1}, policy.placement);
                if (!execution) {
                    return std::optional<PartitionedDispatchExecution>{};
                }
                execution->candidateIndex = placementIndex;
                candidate.shards.push_back(
                    {.slice = cover->Shards()[index], .execution = *execution, .residency = std::move(*projection)});
            }
            auto scheduled = scheduler(candidate);
            if (!scheduled) {
                return std::unexpected(scheduled.error());
            }
            if (!*scheduled) {
                return std::optional<PartitionedDispatchExecution>{};
            }
            return std::optional<PartitionedDispatchExecution>{std::move(candidate)};
        }

        template<typename Evaluate>
        [[nodiscard]] std::expected<bool, AutomaticDispatchPlanningError>
        AdvanceAxisSearch(AxisPartitionSearch& search, std::span<const EligibleCandidate> eligible,
                          std::span<const PlacementCandidate> candidates, std::span<const DeviceId> availableDevices,
                          Evaluate&& evaluate) {
            bool consumedEvaluation = false;
            bool improved = false;
            for (uint32_t attempt = 0; attempt < 2 && !consumedEvaluation; ++attempt) {
                const bool chooseDevice = search.deviceTurn;
                search.deviceTurn = !search.deviceTurn;
                if (chooseDevice && !search.deviceExhausted) {
                    while (!consumedEvaluation) {
                        if (search.deviceProposalState) {
                            auto proposal = BoundaryProposal(search.deviceProposalState->extents, search.axisUnits,
                                                             search.deviceBoundaryOrdinal++);
                            if (!proposal) {
                                search.deviceProposalState.reset();
                                search.deviceBoundaryOrdinal = 0;
                                continue;
                            }
                            auto evaluated = evaluate(search.axis, search.deviceProposalState->placements, *proposal);
                            if (!evaluated) {
                                return std::unexpected(evaluated.error());
                            }
                            consumedEvaluation = true;
                            if (*evaluated && (*evaluated)->totalNanoseconds <
                                                  search.deviceProposalState->execution.totalNanoseconds) {
                                search.deviceProposalState->extents = std::move(*proposal);
                                search.deviceProposalState->execution = std::move(**evaluated);
                                search.deviceBoundaryOrdinal = 0;
                            }
                            if (search.deviceProposalState->execution.totalNanoseconds <
                                search.state.execution.totalNanoseconds) {
                                search.state = std::move(*search.deviceProposalState);
                                search.deviceProposalState.reset();
                                improved = true;
                            }
                            continue;
                        }
                        const std::vector<DeviceId> selectedDevices =
                            SelectedDevices(search.state.placements, candidates);
                        auto devices = DeviceSetProposal(selectedDevices, availableDevices, search.maximumDevices,
                                                         search.deviceOrdinal++);
                        if (!devices) {
                            search.deviceExhausted = true;
                            break;
                        }
                        auto proposal = SeedExactDeviceSet(*devices, eligible, candidates, search.workItemsPerAxisUnit,
                                                           search.axisUnits);
                        if (!proposal) {
                            continue;
                        }
                        auto evaluated = evaluate(search.axis, proposal->placements, proposal->extents);
                        if (!evaluated) {
                            return std::unexpected(evaluated.error());
                        }
                        consumedEvaluation = true;
                        if (*evaluated) {
                            search.deviceProposalState =
                                PartitionSearchState{.placements = std::move(proposal->placements),
                                                     .extents = std::move(proposal->extents),
                                                     .execution = std::move(**evaluated)};
                            if (search.deviceProposalState->execution.totalNanoseconds <
                                search.state.execution.totalNanoseconds) {
                                search.state = std::move(*search.deviceProposalState);
                                search.deviceProposalState.reset();
                                improved = true;
                            }
                        }
                    }
                } else if (!chooseDevice && !search.boundaryExhausted) {
                    auto proposal = BoundaryProposal(search.state.extents, search.axisUnits, search.boundaryOrdinal++);
                    if (!proposal) {
                        search.boundaryExhausted = true;
                        continue;
                    }
                    auto evaluated = evaluate(search.axis, search.state.placements, *proposal);
                    if (!evaluated) {
                        return std::unexpected(evaluated.error());
                    }
                    consumedEvaluation = true;
                    if (*evaluated && (*evaluated)->totalNanoseconds < search.state.execution.totalNanoseconds) {
                        search.state.extents = std::move(*proposal);
                        search.state.execution = std::move(**evaluated);
                        improved = true;
                    }
                }
            }
            if (improved) {
                search.deviceOrdinal = 0;
                search.boundaryOrdinal = 0;
                search.deviceBoundaryOrdinal = 0;
                search.deviceProposalState.reset();
                search.deviceExhausted = false;
                search.boundaryExhausted = false;
            }
            return consumedEvaluation;
        }

        [[nodiscard]] constexpr bool StrictlyImproves(uint64_t baseline, uint64_t partitioned,
                                                      uint32_t minimumImprovementPermille) noexcept {
            const uint64_t retainedPermille = 1000 - minimumImprovementPermille;
            const uint64_t floor = (baseline / 1000) * retainedPermille + (baseline % 1000) * retainedPermille / 1000;
            const bool exact = (baseline % 1000) * retainedPermille % 1000 == 0;
            return exact ? partitioned < floor : partitioned <= floor;
        }

        [[nodiscard]] constexpr bool SearchCanAmortize(const WorkloadDesc& workload, uint64_t baselineNanoseconds,
                                                       const AutomaticPartitionPolicy& policy) noexcept {
            return policy.minimumAmortizedBenefitNanoseconds == 0 ||
                   static_cast<__uint128_t>(baselineNanoseconds) * workload.expectedReuseCount >=
                       policy.minimumAmortizedBenefitNanoseconds;
        }

        [[nodiscard]] constexpr bool BetterPartition(const PartitionedDispatchExecution& left,
                                                     const PartitionedDispatchExecution& right) noexcept {
            return left.totalNanoseconds < right.totalNanoseconds ||
                   (left.totalNanoseconds == right.totalNanoseconds && left.axis < right.axis);
        }

    } // namespace

    std::expected<AutomaticDispatchExecution, AutomaticDispatchPlanningError>
    PlanAutomaticDispatch(const DispatchDomain& domain, PartitionContract contract, const WorkloadDesc& workload,
                          std::span<const PlacementCandidate> candidates,
                          const AutomaticPartitionPolicy& policy) noexcept {
        const IndexDomain indexDomain{.rank = domain.rank};
        if (!domain.Valid() || !contract.Valid(indexDomain) || !policy.Valid() ||
            !ValidSemanticHash(workload.kernelHash) || workload.workItemCount != domain.workItemCount ||
            candidates.empty()) {
            return std::unexpected(AutomaticDispatchPlanningError::InvalidRequest);
        }
        auto baseline = PlanExecution(workload, candidates, policy.placement);
        if (!baseline) {
            return std::unexpected(baseline.error() == PlanningError::NoCandidate
                                       ? AutomaticDispatchPlanningError::NoSingleDeviceCandidate
                                       : AutomaticDispatchPlanningError::InvalidRequest);
        }
        AutomaticDispatchExecution result{.singleDeviceBaseline = *baseline};
        if (!SearchCanAmortize(workload, result.singleDeviceBaseline.cost.totalNanoseconds, policy)) {
            return result;
        }
        const uint8_t axisMask = CandidateAxisMask(domain, contract, policy);
        if (axisMask == 0) {
            return result;
        }

        try {
            std::vector<EligibleCandidate> eligible =
                CollectEligibleCandidates(workload, candidates, policy.placement, true);
            if (eligible.size() < 2) {
                return result;
            }

            std::optional<PartitionedDispatchExecution> best;
            for (uint8_t axis = 0; axis < domain.rank; ++axis) {
                if (!AxisEnabled(axisMask, axis)) {
                    continue;
                }
                const uint64_t axisUnits = domain.extents[axis];
                const uint64_t workItemsPerAxisUnit = AxisUnitWorkItems(domain, axis);
                const size_t maximumDevices = std::min<size_t>({policy.maximumDevices, eligible.size(), axisUnits});
                const auto minimum =
                    GlobalMinimumParallelTime(eligible, candidates, workItemsPerAxisUnit, axisUnits, maximumDevices);
                if (!minimum) {
                    continue;
                }
                auto selected = SelectExactCandidates(*minimum, eligible, candidates, workItemsPerAxisUnit, axisUnits,
                                                      maximumDevices);
                auto partitioned =
                    EvaluateSelection(domain, contract, workload, candidates, std::move(selected), axis, policy);
                if (!partitioned) {
                    return std::unexpected(partitioned.error());
                }
                if (*partitioned && (!best || BetterPartition(**partitioned, *best))) {
                    best = std::move(**partitioned);
                }
            }
            if (best && StrictlyImproves(result.singleDeviceBaseline.cost.totalNanoseconds, best->totalNanoseconds,
                                         policy.minimumImprovementPermille)) {
                result.partitioned = std::move(*best);
            }
            return result;
        } catch (const std::bad_alloc&) {
            return std::unexpected(AutomaticDispatchPlanningError::OutOfMemory);
        }
    }

    std::expected<Detail::AutomaticPartitionSearchResult, AutomaticDispatchPlanningError>
    Detail::SearchAutomaticDispatchPartitions(const KernelInvocation& invocation,
                                              const DispatchCandidateSet& candidates,
                                              const GraphPlanningServices& services,
                                              const GraphResidencyState& residency,
                                              const AutomaticPartitionPolicy& policy,
                                              AutomaticPartitionSchedulerRef scheduler) noexcept {
        const DispatchDomain& domain = invocation.Domain();
        const PartitionContract contract = invocation.Partition();
        const IndexDomain indexDomain{.rank = domain.rank};
        if (!domain.Valid() || !contract.Valid(indexDomain) || !policy.Valid() ||
            !ValidSemanticHash(candidates.workload.kernelHash) ||
            candidates.workload.workItemCount != domain.workItemCount ||
            candidates.workload.kernelHash != invocation.ProgramHash() || !candidates.node.Valid() ||
            candidates.placements.empty()) {
            return std::unexpected(AutomaticDispatchPlanningError::InvalidRequest);
        }
        try {
            std::vector<PlacementCandidate> placements;
            placements.reserve(candidates.placements.size());
            for (const DispatchPlacementCandidate& candidate : candidates.placements) {
                if (candidate.residencyProjection >= candidates.residencies.size()) {
                    return std::unexpected(AutomaticDispatchPlanningError::InvalidRequest);
                }
                placements.push_back(candidate.placement);
            }
            auto baseline = PlanExecution(candidates.workload, placements, policy.placement);
            if (!baseline) {
                return std::unexpected(baseline.error() == PlanningError::NoCandidate
                                           ? AutomaticDispatchPlanningError::NoSingleDeviceCandidate
                                           : AutomaticDispatchPlanningError::InvalidRequest);
            }
            AutomaticPartitionSearchResult result{.singleDeviceBaseline = *baseline};
            if (!SearchCanAmortize(candidates.workload, result.singleDeviceBaseline.cost.totalNanoseconds, policy)) {
                return result;
            }
            const uint8_t axisMask = CandidateAxisMask(domain, contract, policy);
            if (axisMask == 0) {
                return result;
            }

            std::vector<PlacementCandidate> computeCandidates = placements;
            for (PlacementCandidate& candidate : computeCandidates) {
                candidate.transfer = CandidateTransferCost{};
            }
            std::vector<EligibleCandidate> eligible =
                CollectEligibleCandidates(candidates.workload, computeCandidates, policy.placement, false);
            const std::vector<DeviceId> availableDevices = AvailableDevices(eligible, computeCandidates);
            if (availableDevices.size() < 2) {
                return result;
            }
            if (policy.maximumPartitionEvaluations < static_cast<uint32_t>(std::popcount(axisMask))) {
                return std::unexpected(AutomaticDispatchPlanningError::InsufficientSearchBudget);
            }

            uint32_t evaluations = 0;
            const auto evaluate = [&](uint8_t axis, std::span<const size_t> placementIndices,
                                      std::span<const uint64_t> extents)
                -> std::expected<std::optional<PartitionedDispatchExecution>, AutomaticDispatchPlanningError> {
                return EvaluateExactPartition(invocation, candidates, services, residency, computeCandidates, policy,
                                              axis, placementIndices, extents, evaluations, scheduler);
            };

            std::vector<AxisPartitionSearch> searches;
            searches.reserve(std::popcount(axisMask));
            for (uint8_t axis = 0; axis < domain.rank; ++axis) {
                if (!AxisEnabled(axisMask, axis)) {
                    continue;
                }
                const uint64_t axisUnits = domain.extents[axis];
                const uint64_t workItemsPerAxisUnit = AxisUnitWorkItems(domain, axis);
                const size_t maximumDevices =
                    std::min<size_t>({policy.maximumDevices, availableDevices.size(), axisUnits});
                auto seed =
                    SeedGlobalDeviceSet(eligible, computeCandidates, workItemsPerAxisUnit, axisUnits, maximumDevices);
                if (!seed) {
                    continue;
                }
                auto initial = evaluate(axis, seed->placements, seed->extents);
                if (!initial) {
                    return std::unexpected(initial.error());
                }
                if (!*initial) {
                    continue;
                }
                searches.push_back({.axis = axis,
                                    .axisUnits = axisUnits,
                                    .workItemsPerAxisUnit = workItemsPerAxisUnit,
                                    .maximumDevices = maximumDevices,
                                    .state = {.placements = std::move(seed->placements),
                                              .extents = std::move(seed->extents),
                                              .execution = std::move(**initial)}});
            }
            if (searches.empty()) {
                result.partitionEvaluations = evaluations;
                return result;
            }

            bool consumedEvaluation = true;
            while (evaluations < policy.maximumPartitionEvaluations && consumedEvaluation) {
                consumedEvaluation = false;
                for (AxisPartitionSearch& search : searches) {
                    if (evaluations >= policy.maximumPartitionEvaluations || search.Exhausted()) {
                        continue;
                    }
                    auto advanced = AdvanceAxisSearch(search, eligible, computeCandidates, availableDevices, evaluate);
                    if (!advanced) {
                        return std::unexpected(advanced.error());
                    }
                    consumedEvaluation = consumedEvaluation || *advanced;
                }
            }

            auto best = std::ranges::min_element(
                searches, [](const AxisPartitionSearch& left, const AxisPartitionSearch& right) {
                    return BetterPartition(left.state.execution, right.state.execution);
                });
            result.partitionEvaluations = evaluations;
            best->state.execution.partitionEvaluations = evaluations;
            result.bestPartition = std::move(best->state.execution);
            return result;
        } catch (const std::bad_alloc&) {
            return std::unexpected(AutomaticDispatchPlanningError::OutOfMemory);
        }
    }

    std::expected<AutomaticDispatchExecution, AutomaticDispatchPlanningError>
    PlanAutomaticDispatch(const KernelInvocation& invocation, const DispatchCandidateSet& candidates,
                          const GraphPlanningServices& services, const GraphResidencyState& residency,
                          const AutomaticPartitionPolicy& policy) noexcept {
        auto scheduler =
            [](PartitionedDispatchExecution& candidate) -> std::expected<bool, AutomaticDispatchPlanningError> {
            return ScheduleResidentPartition(candidate);
        };
        auto searched = Detail::SearchAutomaticDispatchPartitions(invocation, candidates, services, residency, policy,
                                                                  Detail::AutomaticPartitionSchedulerRef{scheduler});
        if (!searched) {
            return std::unexpected(searched.error());
        }
        AutomaticDispatchExecution result{.singleDeviceBaseline = searched->singleDeviceBaseline};
        if (searched->bestPartition &&
            StrictlyImproves(result.singleDeviceBaseline.cost.totalNanoseconds,
                             searched->bestPartition->totalNanoseconds, policy.minimumImprovementPermille)) {
            result.partitioned = std::move(searched->bestPartition);
        }
        return result;
    }

} // namespace Sora::Compute

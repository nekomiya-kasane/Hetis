#include <Sora/Compute/Runtime/AutomaticDispatchPlanning.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace {

    static_assert(Sora::Compute::AutomaticPartitionPolicy{}.enabled);
    static_assert(Sora::Compute::AutomaticPartitionPolicy{.maximumDevices = 2}.enabled);

    constexpr Sora::Compute::SemanticHash kKernel{7, 9};

    [[nodiscard]] Sora::Compute::PlacementCandidate
    Candidate(uint64_t identity, uint64_t workItemsPerSecond, uint64_t launchNanoseconds = 1'000,
              std::optional<Sora::Compute::CandidateTransferCost> transfer = std::nullopt) {
        const Sora::Compute::DeviceId device{identity, 1};
        return {.service = {identity, 2},
                .device = {.id = device,
                           .backend = Sora::Compute::BackendKind::Vulkan,
                           .kind = Sora::Compute::DeviceKind::DiscreteGpu,
                           .totalMemoryBytes = uint64_t{16} << 30U,
                           .maximumAllocationBytes = uint64_t{12} << 30U},
                .target = {.device = device, .backend = Sora::Compute::BackendKind::Vulkan},
                .queue = {identity, 3},
                .snapshot = {.id = device,
                             .state = Sora::Compute::DeviceState::Available,
                             .generation = 1,
                             .availableMemoryBytes = uint64_t{12} << 30U},
                .executable = {.mode = Sora::Compute::ArtifactResolutionMode::Live},
                .workItemsPerSecond = workItemsPerSecond,
                .transfer = transfer,
                .launchNanoseconds = launchNanoseconds,
                .deterministic = true};
    }

    [[nodiscard]] constexpr Sora::Compute::WorkloadDesc Workload(uint64_t workItems) noexcept {
        return {.kernelHash = kKernel,
                .resourceDemand = {.floatingPointOperations = 2, .bytesRead = 8, .bytesWritten = 4},
                .workItemCount = workItems,
                .requiredDeviceMemoryBytes = 1U << 20U,
                .largestAllocationBytes = 1U << 20U};
    }

} // namespace

TEST_CASE("Automatic dispatch planning selects a profitable strict multi-device cover",
          "[Sora][Compute][Planner][Partition]") {
    constexpr Sora::Compute::DispatchDomain domain{.rank = 1, .extents = {1'000'000}, .workItemCount = 1'000'000};
    const std::array candidates{Candidate(1, 1'000'000'000), Candidate(2, 1'000'000'000)};
    const auto plan = Sora::Compute::PlanAutomaticDispatch(
        domain, Sora::Compute::PartitionContract::Elementwise(1), Workload(domain.workItemCount), candidates,
        {.maximumDevices = 2, .partitionOverheadNanoseconds = 5'000, .publicationNanoseconds = 1'000});

    REQUIRE(plan.has_value());
    REQUIRE(plan->UsesPartitioning());
    REQUIRE(plan->partitioned->shards.size() == 2);
    REQUIRE(plan->partitioned->shards[0].slice.extents[0] == 500'000);
    REQUIRE(plan->partitioned->shards[1].slice.extents[0] == 500'000);
    REQUIRE(plan->partitioned->shards[0].slice.origin[0] == 0);
    REQUIRE(plan->partitioned->shards[1].slice.origin[0] == 500'000);
    REQUIRE(plan->partitioned->shards[0].execution.device != plan->partitioned->shards[1].execution.device);
    REQUIRE(plan->SelectedNanoseconds() < plan->singleDeviceBaseline.cost.totalNanoseconds);
}

TEST_CASE("Automatic dispatch planning weights integer shards by calibrated throughput",
          "[Sora][Compute][Planner][Partition][Cost]") {
    constexpr Sora::Compute::DispatchDomain domain{.rank = 1, .extents = {1'024}, .workItemCount = 1'024};
    const std::array candidates{Candidate(1, 3'000'000'000, 0), Candidate(2, 1'000'000'000, 0)};
    const auto plan = Sora::Compute::PlanAutomaticDispatch(domain, Sora::Compute::PartitionContract::Elementwise(1),
                                                           Workload(domain.workItemCount), candidates,
                                                           {.maximumDevices = 2, .minimumImprovementPermille = 0});

    REQUIRE(plan.has_value());
    REQUIRE(plan->UsesPartitioning());
    REQUIRE(plan->partitioned->shards[0].slice.extents[0] > plan->partitioned->shards[1].slice.extents[0]);
    REQUIRE(plan->partitioned->shards[0].slice.extents[0] + plan->partitioned->shards[1].slice.extents[0] ==
            domain.extents[0]);
}

TEST_CASE("Automatic dispatch planning keeps each device's non-dominated queue frontier",
          "[Sora][Compute][Planner][Partition][Pareto]") {
    constexpr Sora::Compute::DispatchDomain domain{.rank = 1, .extents = {1'000'000}, .workItemCount = 1'000'000};
    const std::array candidates{
        Candidate(1, 10'000'000'000, 180'000),
        Candidate(1, 1'000'000'000, 0),
        Candidate(2, 5'000'000'000, 0),
    };
    const auto plan = Sora::Compute::PlanAutomaticDispatch(domain, Sora::Compute::PartitionContract::Elementwise(1),
                                                           Workload(domain.workItemCount), candidates,
                                                           {.maximumDevices = 2, .minimumImprovementPermille = 0});

    REQUIRE(plan.has_value());
    REQUIRE(plan->UsesPartitioning());
    REQUIRE(plan->partitioned->shards.size() == 2);
    REQUIRE(plan->partitioned->shards[0].execution.device == candidates[1].device.id);
    REQUIRE(plan->partitioned->shards[0].execution.candidateIndex == 1);
}

TEST_CASE("Automatic dispatch planning finds a non-nested globally optimal device subset",
          "[Sora][Compute][Planner][Partition][Exact]") {
    constexpr Sora::Compute::DispatchDomain domain{.rank = 1, .extents = {1'000}, .workItemCount = 1'000};
    const std::array candidates{
        Candidate(1, 1'000'000'000, 0),
        Candidate(2, 10'000'000'000, 400),
        Candidate(3, 1'000'000'000, 0),
        Candidate(4, 1'000'000'000, 0),
    };
    const auto plan = Sora::Compute::PlanAutomaticDispatch(domain, Sora::Compute::PartitionContract::Elementwise(1),
                                                           Workload(domain.workItemCount), candidates,
                                                           {.maximumDevices = 3, .minimumImprovementPermille = 0});

    REQUIRE(plan.has_value());
    REQUIRE(plan->UsesPartitioning());
    REQUIRE(plan->partitioned->shards.size() == 3);
    REQUIRE(plan->partitioned->parallelNanoseconds == 334);
    REQUIRE(plan->partitioned->shards[0].execution.device == candidates[0].device.id);
    REQUIRE(plan->partitioned->shards[1].execution.device == candidates[2].device.id);
    REQUIRE(plan->partitioned->shards[2].execution.device == candidates[3].device.id);
}

TEST_CASE("Automatic dispatch planning preserves the single-device fallback boundary",
          "[Sora][Compute][Planner][Partition][Fallback]") {
    constexpr Sora::Compute::DispatchDomain domain{.rank = 1, .extents = {1'000'000}, .workItemCount = 1'000'000};

    SECTION("exact shard transfer projection is required before partitioning") {
        constexpr Sora::Compute::CandidateTransferCost transfer{1U << 20U, 20'000};
        const std::array candidates{Candidate(1, 1'000'000'000, 1'000, transfer),
                                    Candidate(2, 1'000'000'000, 1'000, transfer)};
        const auto plan = Sora::Compute::PlanAutomaticDispatch(domain, Sora::Compute::PartitionContract::Elementwise(1),
                                                               Workload(domain.workItemCount), candidates);
        REQUIRE(plan.has_value());
        REQUIRE_FALSE(plan->UsesPartitioning());
    }

    SECTION("partition and publication overhead must clear the crossover margin") {
        const std::array candidates{Candidate(1, 1'000'000'000), Candidate(2, 1'000'000'000)};
        const auto plan = Sora::Compute::PlanAutomaticDispatch(
            domain, Sora::Compute::PartitionContract::Elementwise(1), Workload(domain.workItemCount), candidates,
            {.maximumDevices = 2, .partitionOverheadNanoseconds = 2'000'000});
        REQUIRE(plan.has_value());
        REQUIRE_FALSE(plan->UsesPartitioning());
    }

    SECTION("an indivisible semantic contract cannot be overridden by runtime device count") {
        const std::array candidates{Candidate(1, 1'000'000'000), Candidate(2, 1'000'000'000)};
        const auto plan = Sora::Compute::PlanAutomaticDispatch(domain, Sora::Compute::PartitionContract::Indivisible(),
                                                               Workload(domain.workItemCount), candidates);
        REQUIRE(plan.has_value());
        REQUIRE_FALSE(plan->UsesPartitioning());
    }
}

TEST_CASE("Automatic dispatch planning selects the best contract-permitted axis",
          "[Sora][Compute][Planner][Partition][Axis]") {
    constexpr Sora::Compute::DispatchDomain domain{.rank = 3, .extents = {1, 3, 1'000}, .workItemCount = 3'000};
    const std::array candidates{Candidate(1, 1'000'000'000, 0), Candidate(2, 1'000'000'000, 0)};

    const auto automatic = Sora::Compute::PlanAutomaticDispatch(
        domain, Sora::Compute::PartitionContract::Elementwise(domain.rank), Workload(domain.workItemCount), candidates,
        {.maximumDevices = 2, .minimumImprovementPermille = 0});
    REQUIRE(automatic.has_value());
    REQUIRE(automatic->UsesPartitioning());
    REQUIRE(automatic->partitioned->axis == 2);
    REQUIRE(automatic->partitioned->shards[0].slice.extents[2] == 500);
    REQUIRE(automatic->partitioned->shards[1].slice.extents[2] == 500);

    const auto constrained = Sora::Compute::PlanAutomaticDispatch(
        domain, Sora::Compute::PartitionContract::Elementwise(domain.rank), Workload(domain.workItemCount), candidates,
        {.allowedAxisMask = uint8_t{1} << 1U, .maximumDevices = 2, .minimumImprovementPermille = 0});
    REQUIRE(constrained.has_value());
    REQUIRE(constrained->UsesPartitioning());
    REQUIRE(constrained->partitioned->axis == 1);
    REQUIRE(constrained->partitioned->shards[0].slice.extents[1] == 2);
    REQUIRE(constrained->partitioned->shards[1].slice.extents[1] == 1);
}

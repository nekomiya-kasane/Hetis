/**
 * @file DispatchPartitionBenchmark.cpp
 * @brief Control-plane baseline for strict dispatch covers and shard-local physical binding lowering.
 */

#include <BenchmarkHarness.h>
#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>
#include <Sora/Compute/Runtime/AutomaticDispatchPlanning.h>
#include <Sora/Compute/Runtime/PhysicalBinding.h>
#include <Sora/Compute/Runtime/RouteTransferScheduling.h>

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <print>
#include <utility>
#include <vector>

namespace {

    constexpr size_t kSamples = 9;
    constexpr auto kMinimumSampleTime = std::chrono::milliseconds(50);
    constexpr uint64_t kElementCount = uint64_t{1} << 20;
    constexpr uint32_t kShardCount = 16;
    constexpr Sora::Compute::ValueType kFloat{Sora::Compute::ScalarKind::Float32, 1};
    constexpr Sora::Compute::MemoryLocation kLocation{
        .domain = {1, 1}, .device = {2, 2}, .backend = Sora::Compute::BackendKind::Cpu};
    constexpr Sora::Compute::MemoryLocation kTransferDestination{
        .domain = {3, 3}, .device = {4, 4}, .backend = Sora::Compute::BackendKind::Cpu};

    struct BackendState {};
    struct CodeOwner {};
    struct NativeAllocation {};

    void DestroyBackend(void* state) noexcept {
        delete static_cast<BackendState*>(state);
    }
    void DestroyAllocation(void* state) noexcept {
        delete static_cast<NativeAllocation*>(state);
    }

    [[nodiscard]] const Sora::Compute::BackendService& BenchmarkService() noexcept {
        static const Sora::Compute::BackendService service = [] {
            auto state = std::make_unique<BackendState>();
            auto code = Sora::Compute::BackendCodeLease::Retain(std::make_shared<CodeOwner>());
            auto adopted = Sora::Compute::BackendService::Adopt(
                {.id = {1, 1}, .state = state.get(), .destroy = DestroyBackend}, std::move(code));
            if (!adopted) {
                std::terminate();
            }
            static_cast<void>(state.release());
            return std::move(*adopted);
        }();
        return service;
    }

    consteval auto MakeSaxpyProgram() {
        Sora::Compute::KernelBuilder<3, 4> scalar;
        const auto alpha = scalar.AddValueParameter(kFloat);
        const auto x = scalar.AddValueParameter(kFloat);
        const auto y = scalar.AddValueParameter(kFloat);
        const auto value = scalar.Ternary(Sora::Compute::OpCode::Fma, alpha, x, y);
        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<4, 3, 4> program{domain, std::move(scalar).Finish(value),
                                                             Sora::Compute::PartitionContract::Elementwise(1)};
        const auto alphaArgument = program.AddValueArgument(kFloat);
        const auto xArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, 1,
                                                         Sora::Compute::LayoutKind::Contiguous, 64, {1});
        const auto yArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, 1,
                                                         Sora::Compute::LayoutKind::Contiguous, 64, {2});
        const auto outputArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Write, kFloat, 1,
                                                              Sora::Compute::LayoutKind::Contiguous, 64, {3});
        program.BindArgument(alphaArgument);
        program.BindArgument(xArgument);
        program.BindArgument(yArgument);
        return std::move(program).Finish(outputArgument);
    }

    constexpr auto kSaxpy = MakeSaxpyProgram();

    class PartitionFixture final {
    public:
        PartitionFixture() noexcept {
            try {
                Initialize();
            } catch (const std::bad_alloc&) {
                failed_ = true;
            }
        }

        void ConstructBalancedCover() noexcept {
            const auto plan = Sora::Compute::ElementwisePartitionPlan::Balanced(invocation_.Domain(), kSaxpy.partition,
                                                                                0, kShardCount);
            ConsumePlan(plan);
        }

        void ValidateExplicitCover() noexcept {
            const auto plan = Sora::Compute::ElementwisePartitionPlan::Create(invocation_.Domain(), kSaxpy.partition, 0,
                                                                              shardExtents_);
            ConsumePlan(plan);
        }

        void LowerWholeDomain() noexcept {
            const auto bindings = Sora::Compute::MaterializeKernelBindings(invocation_, sources_);
            ConsumeBindings(bindings);
        }

        void LowerAllShards() noexcept {
            for (const Sora::Compute::DispatchSlice& shard : partition_->Shards()) {
                const auto bindings = Sora::Compute::MaterializeKernelBindings(invocation_, sources_, shard);
                ConsumeBindings(bindings);
            }
        }

        void PlanSingleDevice() noexcept {
            const auto plan = Sora::Compute::PlanExecution(workload_, candidates_);
            if (!plan) {
                failed_ = true;
                return;
            }
            checksum_ += plan->cost.totalNanoseconds + plan->candidateIndex;
        }

        void PlanAutomaticPartition() noexcept {
            const auto plan = Sora::Compute::PlanAutomaticDispatch(invocation_.Domain(), kSaxpy.partition, workload_,
                                                                   candidates_, {.maximumDevices = 2});
            if (!plan || !plan->UsesPartitioning()) {
                failed_ = true;
                return;
            }
            checksum_ += plan->SelectedNanoseconds() + plan->partitioned->shards.size();
        }

        void ScheduleContendedTransfers() noexcept {
            const auto schedule = Sora::Compute::ScheduleRouteTransfers(transferBatches_);
            if (!schedule) {
                failed_ = true;
                return;
            }
            checksum_ += schedule->makespanNanoseconds + schedule->transfers.size();
        }

        [[nodiscard]] bool Failed() const noexcept { return failed_; }
        [[nodiscard]] uint64_t Checksum() const noexcept { return checksum_; }

    private:
        using PartitionResult =
            std::expected<Sora::Compute::ElementwisePartitionPlan, Sora::Compute::DispatchPartitionError>;
        using BindingResult = std::expected<Sora::Compute::PhysicalKernelBindings, Sora::Compute::PhysicalBindingError>;

        [[nodiscard]] static Sora::Compute::Buffer MakeBuffer(Sora::Compute::BufferId id) noexcept {
            auto buffer = Sora::Compute::Buffer::Create({.id = id,
                                                         .elementType = kFloat,
                                                         .layout = Sora::Compute::LayoutKind::Contiguous,
                                                         .rank = 1,
                                                         .sizeBytes = kElementCount * sizeof(float),
                                                         .alignment = 64});
            return buffer ? std::move(*buffer) : Sora::Compute::Buffer{};
        }

        [[nodiscard]] static Sora::Compute::BufferView MakeView(Sora::Compute::Buffer buffer) noexcept {
            constexpr std::array extents{kElementCount};
            auto view = Sora::Compute::BufferView::Contiguous(std::move(buffer), extents);
            return view ? std::move(*view) : Sora::Compute::BufferView{};
        }

        [[nodiscard]] static Sora::Compute::Allocation MakeAllocation(Sora::Compute::PhysicalMemoryId id) {
            auto native = std::make_unique<NativeAllocation>();
            auto allocation = Sora::Compute::Allocation::Adopt(
                BenchmarkService(), {.location = kLocation,
                                     .memory = id,
                                     .properties = Sora::Compute::MemoryProperty::DeviceLocal,
                                     .sizeBytes = kElementCount * sizeof(float),
                                     .alignment = 64,
                                     .state = native.release(),
                                     .destroy = DestroyAllocation});
            return allocation ? std::move(*allocation) : Sora::Compute::Allocation{};
        }

        void Initialize() {
            auto x = MakeBuffer({1, 1});
            auto y = MakeBuffer({1, 2});
            auto output = MakeBuffer({1, 3});
            const auto alphaBytes = std::bit_cast<std::array<std::byte, sizeof(float)>>(2.0F);
            auto alpha = Sora::Compute::ScalarArgument::Create(kFloat, alphaBytes);
            if (!x || !y || !output || !alpha) {
                failed_ = true;
                return;
            }
            std::vector<Sora::Compute::KernelArgumentBinding> arguments;
            arguments.reserve(4);
            arguments.emplace_back(std::move(*alpha));
            arguments.emplace_back(MakeView(x));
            arguments.emplace_back(MakeView(y));
            arguments.emplace_back(MakeView(output));
            constexpr std::array extents{kElementCount};
            auto invocation = Sora::Compute::BindKernelProgram(kSaxpy, extents, std::move(arguments));
            if (!invocation) {
                failed_ = true;
                return;
            }
            invocation_ = std::move(*invocation);

            const auto whole = Sora::Compute::ByteRegion::Make(0, kElementCount * sizeof(float));
            auto xAllocation = MakeAllocation({1, 1});
            auto yAllocation = MakeAllocation({2, 2});
            auto outputAllocation = MakeAllocation({3, 3});
            if (!whole || !xAllocation || !yAllocation || !outputAllocation) {
                failed_ = true;
                return;
            }
            auto pending = Sora::Compute::PendingReplica::Create(outputAllocation, *whole);
            if (!pending) {
                failed_ = true;
                return;
            }
            const Sora::Compute::ResidentReplica xReplica{
                .allocation = std::move(xAllocation), .bufferRegion = *whole, .version = 1};
            sources_[0] = xReplica;
            sources_[1] = Sora::Compute::ResidentReplica{
                .allocation = std::move(yAllocation), .bufferRegion = *whole, .version = 1};
            sources_[2] = std::move(*pending);

            const auto half = Sora::Compute::ByteRegion::Make(0, whole->Size() / 2);
            if (!half) {
                failed_ = true;
                return;
            }
            const Sora::Compute::TransferLink link{.sourceService = BenchmarkService().Id(),
                                                   .service = BenchmarkService().Id(),
                                                   .engine = {5, 5},
                                                   .source = kLocation,
                                                   .destination = kTransferDestination,
                                                   .kind = Sora::Compute::TransferKind::Copy,
                                                   .bytesPerSecond = 16'000'000'000,
                                                   .latencyNanoseconds = 100,
                                                   .maximumTransferBytes = whole->Size()};
            const Sora::Compute::GraphResidencyRoute route{.buffer = x.Describe().id,
                                                           .source = xReplica,
                                                           .destinationService = BenchmarkService().Id(),
                                                           .destination = kTransferDestination,
                                                           .region = *half,
                                                           .version = 1,
                                                           .estimatedNanoseconds =
                                                               Sora::Compute::Detail::TransferCost(link, half->Size()),
                                                           .hops = {link}};
            transferRoutes_[0] = {route};
            transferRoutes_[1] = {route};
            transferBatches_[0] = {.operation = 0, .dispatchNanoseconds = 1'000, .routes = transferRoutes_[0]};
            transferBatches_[1] = {.operation = 1, .dispatchNanoseconds = 1'000, .routes = transferRoutes_[1]};

            shardExtents_.fill(kElementCount / kShardCount);
            auto partition = Sora::Compute::ElementwisePartitionPlan::Create(invocation_.Domain(), kSaxpy.partition, 0,
                                                                             shardExtents_);
            if (!partition) {
                failed_ = true;
                return;
            }
            partition_.emplace(std::move(*partition));

            workload_ = {.kernelHash = kSaxpy.Hash(),
                         .resourceDemand = kSaxpy.ResourceDemand(),
                         .workItemCount = kElementCount,
                         .requiredDeviceMemoryBytes = kElementCount * sizeof(float) * 3,
                         .largestAllocationBytes = kElementCount * sizeof(float)};
            for (uint64_t index = 0; index < candidates_.size(); ++index) {
                const Sora::Compute::DeviceId device{index + 1, 1};
                candidates_[index] = {.service = {index + 1, 2},
                                      .device = {.id = device,
                                                 .backend = Sora::Compute::BackendKind::Vulkan,
                                                 .kind = Sora::Compute::DeviceKind::DiscreteGpu,
                                                 .totalMemoryBytes = uint64_t{16} << 30U,
                                                 .maximumAllocationBytes = uint64_t{12} << 30U},
                                      .target = {.device = device, .backend = Sora::Compute::BackendKind::Vulkan},
                                      .queue = {index + 1, 3},
                                      .snapshot = {.id = device,
                                                   .state = Sora::Compute::DeviceState::Available,
                                                   .generation = 1,
                                                   .availableMemoryBytes = uint64_t{12} << 30U},
                                      .executable = {.mode = Sora::Compute::ArtifactResolutionMode::Live},
                                      .workItemsPerSecond = 1'000'000'000,
                                      .launchNanoseconds = 1'000,
                                      .deterministic = true};
            }
        }

        void ConsumePlan(const PartitionResult& result) noexcept {
            if (!result) {
                failed_ = true;
                return;
            }
            checksum_ += result->Shards().size() + result->Shards().back().workItemCount;
        }

        void ConsumeBindings(const BindingResult& result) noexcept {
            if (!result) {
                failed_ = true;
                return;
            }
            checksum_ += result->buffers.size() + result->scalarData.size() + result->buffers.back().offset;
        }

        Sora::Compute::KernelInvocation invocation_{};
        std::array<Sora::Compute::PhysicalBufferSource, 3> sources_{};
        std::array<uint64_t, kShardCount> shardExtents_{};
        std::optional<Sora::Compute::ElementwisePartitionPlan> partition_{};
        Sora::Compute::WorkloadDesc workload_{};
        std::array<Sora::Compute::PlacementCandidate, 2> candidates_{};
        std::array<std::array<Sora::Compute::GraphResidencyRoute, 1>, 2> transferRoutes_{};
        std::array<Sora::Compute::RouteTransferBatch, 2> transferBatches_{};
        uint64_t checksum_ = 0;
        bool failed_ = false;
    };

} // namespace

int main() {
    PartitionFixture fixture;
    if (fixture.Failed()) {
        return 2;
    }
    auto balanced = [&fixture]() noexcept { fixture.ConstructBalancedCover(); };
    auto explicitCover = [&fixture]() noexcept { fixture.ValidateExplicitCover(); };
    auto whole = [&fixture]() noexcept { fixture.LowerWholeDomain(); };
    auto shards = [&fixture]() noexcept { fixture.LowerAllShards(); };
    auto singlePlan = [&fixture]() noexcept { fixture.PlanSingleDevice(); };
    auto automaticPlan = [&fixture]() noexcept { fixture.PlanAutomaticPartition(); };
    auto transferSchedule = [&fixture]() noexcept { fixture.ScheduleContendedTransfers(); };
    const std::array cases{Sora::Benchmark::MakeCase("balanced_cover", balanced),
                           Sora::Benchmark::MakeCase("explicit_cover", explicitCover),
                           Sora::Benchmark::MakeCase("whole_binding", whole),
                           Sora::Benchmark::MakeCase("shard_bindings", shards),
                           Sora::Benchmark::MakeCase("single_plan", singlePlan),
                           Sora::Benchmark::MakeCase("automatic_plan", automaticPlan),
                           Sora::Benchmark::MakeCase("contended_transfer_schedule", transferSchedule)};
    const auto timing = Sora::Benchmark::MeasureInterleavedIndependent<kSamples>(cases, 1, kMinimumSampleTime);
    if (fixture.Failed() || fixture.Checksum() == 0 || fixture.Checksum() == std::numeric_limits<uint64_t>::max()) {
        return 3;
    }

    std::println("metric,value,unit");
    std::println("partition_balanced_cover,{:.6f},ns/cover", timing.MedianTime(0));
    std::println("partition_explicit_cover_validation,{:.6f},ns/cover", timing.MedianTime(1));
    std::println("partition_whole_binding_lowering,{:.6f},ns/domain", timing.MedianTime(2));
    std::println("partition_{}_shard_binding_lowering,{:.6f},ns/domain", kShardCount, timing.MedianTime(3));
    std::println("partition_shard_binding_ratio,{:.6f},ratio", timing.PairedRatio(3, 2));
    std::println("partition_per_shard_normalized_ratio,{:.6f},ratio",
                 timing.PairedRatio(3, 2) / static_cast<double>(kShardCount));
    std::println("partition_single_device_planning,{:.6f},ns/decision", timing.MedianTime(4));
    std::println("partition_automatic_two_device_planning,{:.6f},ns/decision", timing.MedianTime(5));
    std::println("partition_automatic_planning_ratio,{:.6f},ratio", timing.PairedRatio(5, 4));
    std::println("partition_two_route_contended_transfer_schedule,{:.6f},ns/schedule", timing.MedianTime(6));
    std::println("checksum,{},value", fixture.Checksum());
}

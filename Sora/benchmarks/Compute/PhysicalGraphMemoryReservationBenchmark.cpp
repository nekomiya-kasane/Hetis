/**
 * @file PhysicalGraphMemoryReservationBenchmark.cpp
 * @brief Whole-graph physical-memory reservation control-plane baseline.
 * @ingroup Compute
 */

#include <BenchmarkHarness.h>
#include <Sora/Compute/Runtime/PhysicalGraphMaterializer.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <memory>
#include <new>
#include <print>
#include <utility>
#include <vector>

namespace {

    constexpr size_t kSamples = 9;
    constexpr auto kMinimumSampleTime = std::chrono::milliseconds(50);
    constexpr uint64_t kAllocationBytes = 64;
    constexpr Sora::Compute::BackendServiceId kService{1, 1};
    constexpr Sora::Compute::DeviceId kDevice{2, 1};
    constexpr Sora::Compute::QueueId kQueue{3, 1};
    constexpr Sora::Compute::MemoryLocation kLocation{
        .domain = {4, 1}, .device = kDevice, .backend = Sora::Compute::BackendKind::Cpu};
    constexpr Sora::Compute::ValueType kFloat{Sora::Compute::ScalarKind::Float32, 1};

    struct BackendState {
        uint64_t nextMemory = 1;
        uint64_t allocations = 0;
        uint64_t releases = 0;
    };

    struct AllocationToken {
        BackendState* owner = nullptr;
    };

    void NoopDestroy(void*) noexcept {}

    void DestroyAllocation(void* opaque) noexcept {
        std::unique_ptr<AllocationToken> token{static_cast<AllocationToken*>(opaque)};
        ++token->owner->releases;
    }

    void ObserveDevice(void*, Sora::Compute::DeviceSnapshot* snapshot) noexcept {
        *snapshot = {.id = kDevice,
                     .state = Sora::Compute::DeviceState::Available,
                     .generation = 1,
                     .availableMemoryBytes = uint64_t{1} << 30U};
    }

    void RejectSubmit(void*, const Sora::Compute::BackendSubmitRequest*,
                      Sora::Compute::BackendSubmitResult* result) noexcept {
        result->error = {Sora::Compute::SubmissionErrorCode::SubmissionFailed, 0};
    }

    void Allocate(void* opaque, const Sora::Compute::BackendAllocationRequest* request,
                  Sora::Compute::BackendAllocationResult* result) noexcept {
        auto& state = *static_cast<BackendState*>(opaque);
        auto token = std::unique_ptr<AllocationToken>{new (std::nothrow) AllocationToken{&state}};
        if (!token) {
            result->error = {Sora::Compute::MemoryAllocationErrorCode::OutOfMemory, 0};
            return;
        }
        ++state.allocations;
        result->allocation = {.location = request->location,
                              .memory = {state.nextMemory++, 1},
                              .properties = request->requiredProperties | Sora::Compute::MemoryProperty::HostVisible,
                              .sizeBytes = request->sizeBytes,
                              .alignment = request->alignment,
                              .state = token.release(),
                              .destroy = DestroyAllocation};
        result->error = {};
    }

    template<typename T, typename Error>
    [[nodiscard]] T Take(std::expected<T, Error> value) {
        if (!value) {
            std::terminate();
        }
        return std::move(*value);
    }

    class ReservationBenchmarkFixture final {
    public:
        ReservationBenchmarkFixture()
            : service_(MakeService()),
              catalog_(MakeCatalog()),
              one_(MakePlan(1)),
              eight_(MakePlan(8)),
              thirtyTwo_(MakePlan(32)) {}

        void CopyOne() noexcept { CopyPlan(one_); }
        void ReserveOne() noexcept { ReservePlan(one_); }
        void CopyEight() noexcept { CopyPlan(eight_); }
        void ReserveEight() noexcept { ReservePlan(eight_); }
        void CopyThirtyTwo() noexcept { CopyPlan(thirtyTwo_); }
        void ReserveThirtyTwo() noexcept { ReservePlan(thirtyTwo_); }

        [[nodiscard]] bool Failed() const noexcept { return failed_; }
        [[nodiscard]] uint64_t Checksum() const noexcept { return checksum_; }
        [[nodiscard]] uint64_t Allocations() const noexcept { return state_.allocations; }
        [[nodiscard]] uint64_t Releases() const noexcept { return state_.releases; }

    private:
        [[nodiscard]] Sora::Compute::BackendService MakeService() {
            auto code = Sora::Compute::BackendCodeLease::Retain(std::make_shared<uint32_t>());
            return Take(Sora::Compute::BackendService::Adopt({.id = kService, .state = &state_, .destroy = NoopDestroy},
                                                             std::move(code)));
        }

        [[nodiscard]] Sora::Compute::PhysicalResourceCatalog MakeCatalog() {
            auto observer =
                Take(Sora::Compute::DeviceObserver::Create(service_, {.device = kDevice,
                                                                      .backend = Sora::Compute::BackendKind::Cpu,
                                                                      .state = &state_,
                                                                      .observe = ObserveDevice}));
            auto queue =
                Take(Sora::Compute::Queue::Adopt(service_, {.id = kQueue,
                                                            .device = kDevice,
                                                            .backend = Sora::Compute::BackendKind::Cpu,
                                                            .capabilities = Sora::Compute::QueueCapability::Fill,
                                                            .state = &state_,
                                                            .destroy = NoopDestroy,
                                                            .submit = RejectSubmit}));
            auto allocator = Take(Sora::Compute::MemoryAllocator::Create(
                service_, {.location = kLocation,
                           .supportedProperties =
                               Sora::Compute::MemoryProperty::HostVisible | Sora::Compute::MemoryProperty::DeviceLocal,
                           .state = &state_,
                           .allocate = Allocate}));
            return Take(Sora::Compute::PhysicalResourceCatalog::Create({.deviceObservers = {std::move(observer)},
                                                                        .queues = {std::move(queue)},
                                                                        .allocators = {std::move(allocator)}}));
        }

        [[nodiscard]] Sora::Compute::PhysicalGraphPlan MakePlan(size_t allocationCount) {
            const auto whole = Take(Sora::Compute::ByteRegion::Make(0, kAllocationBytes));
            constexpr std::array patternBytes{std::byte{0x5A}};
            const auto pattern = Take(Sora::Compute::FillPattern::Create(patternBytes));
            Sora::Compute::PhysicalGraphPlanStorage storage;
            storage.allocations.reserve(allocationCount);
            storage.segments.reserve(allocationCount);
            for (uint32_t index = 0; index < allocationCount; ++index) {
                auto buffer = Take(Sora::Compute::Buffer::Create({.id = {10, index + 1},
                                                                  .elementType = kFloat,
                                                                  .layout = Sora::Compute::LayoutKind::Contiguous,
                                                                  .rank = 1,
                                                                  .sizeBytes = kAllocationBytes,
                                                                  .alignment = 64}));
                auto slice = Take(Sora::Compute::BufferSlice::Create(buffer, whole));
                auto fill = Take(Sora::Compute::FillOperation::Create(std::move(slice), pattern));
                storage.allocations.push_back(
                    {.id = {index},
                     .buffer = buffer,
                     .service = kService,
                     .location = kLocation,
                     .bufferRegion = whole,
                     .version = 1,
                     .materialization =
                         Sora::Compute::PlannedAllocate{
                             .requiredProperties = Sora::Compute::MemoryProperty::DeviceLocal, .alignment = 64},
                     .publication =
                         Sora::Compute::PlannedPublication{.kind = Sora::Compute::PlannedPublicationKind::WholeWrite,
                                                           .expectedVersion = 0,
                                                           .producer = {index}}});
                storage.segments.emplace_back(
                    Sora::Compute::PlannedFill{.logicalNode = {index},
                                               .queue = {.service = kService,
                                                         .backend = Sora::Compute::BackendKind::Cpu,
                                                         .device = kDevice,
                                                         .queue = kQueue},
                                               .operation = std::move(fill),
                                               .destination = {index}});
            }
            return Take(Sora::Compute::PhysicalGraphPlan::Create(std::move(storage), catalog_));
        }

        void CopyPlan(const Sora::Compute::PhysicalGraphPlan& source) noexcept {
            try {
                auto copy = source;
                checksum_ += copy.Allocations().size();
            } catch (const std::bad_alloc&) {
                failed_ = true;
            }
        }

        void ReservePlan(const Sora::Compute::PhysicalGraphPlan& source) noexcept {
            try {
                auto plan = source;
                auto reservation = Sora::Compute::ReservePhysicalGraphMemory(std::move(plan), catalog_);
                if (!reservation) {
                    failed_ = true;
                    return;
                }
                checksum_ += reservation->Allocations().size();
            } catch (const std::bad_alloc&) {
                failed_ = true;
            }
        }

        BackendState state_{};
        Sora::Compute::BackendService service_{};
        Sora::Compute::PhysicalResourceCatalog catalog_;
        Sora::Compute::PhysicalGraphPlan one_;
        Sora::Compute::PhysicalGraphPlan eight_;
        Sora::Compute::PhysicalGraphPlan thirtyTwo_;
        uint64_t checksum_ = 0;
        bool failed_ = false;
    };

} // namespace

int main() {
    ReservationBenchmarkFixture fixture;
    fixture.ReserveOne();
    fixture.ReserveEight();
    fixture.ReserveThirtyTwo();
    if (fixture.Failed() || fixture.Allocations() != fixture.Releases()) {
        return 2;
    }

    auto copyOne = [&]() noexcept { fixture.CopyOne(); };
    auto reserveOne = [&]() noexcept { fixture.ReserveOne(); };
    auto copyEight = [&]() noexcept { fixture.CopyEight(); };
    auto reserveEight = [&]() noexcept { fixture.ReserveEight(); };
    auto copyThirtyTwo = [&]() noexcept { fixture.CopyThirtyTwo(); };
    auto reserveThirtyTwo = [&]() noexcept { fixture.ReserveThirtyTwo(); };
    const std::array cases{Sora::Benchmark::MakeCase("plan_copy_1", copyOne),
                           Sora::Benchmark::MakeCase("plan_copy_reserve_release_1", reserveOne),
                           Sora::Benchmark::MakeCase("plan_copy_8", copyEight),
                           Sora::Benchmark::MakeCase("plan_copy_reserve_release_8", reserveEight),
                           Sora::Benchmark::MakeCase("plan_copy_32", copyThirtyTwo),
                           Sora::Benchmark::MakeCase("plan_copy_reserve_release_32", reserveThirtyTwo)};
    const auto timing = Sora::Benchmark::MeasureInterleavedIndependent<kSamples>(cases, 1, kMinimumSampleTime);
    if (fixture.Failed() || fixture.Checksum() == 0 || fixture.Allocations() != fixture.Releases()) {
        return 3;
    }

    const std::array copyTimes{timing.MedianTime(0), timing.MedianTime(2), timing.MedianTime(4)};
    const std::array reserveTimes{timing.MedianTime(1), timing.MedianTime(3), timing.MedianTime(5)};
    constexpr std::array<size_t, 3> counts{1, 8, 32};
    std::println("metric,value,unit");
    for (size_t index = 0; index < counts.size(); ++index) {
        std::println("plan_copy_{},{:.6f},ns/graph", counts[index], copyTimes[index]);
        std::println("plan_copy_reserve_release_{},{:.6f},ns/graph", counts[index], reserveTimes[index]);
        std::println("reservation_increment_{},{:.6f},ns/graph", counts[index],
                     std::max(0.0, reserveTimes[index] - copyTimes[index]));
        std::println("reservation_increment_per_allocation_{},{:.6f},ns/allocation", counts[index],
                     std::max(0.0, reserveTimes[index] - copyTimes[index]) / counts[index]);
    }
    for (size_t index = 0; index < cases.size(); ++index) {
        std::println("{}_repetitions,{},count", cases[index].name, timing.repetitions[index]);
    }
    std::println("allocation_callbacks,{},count", fixture.Allocations());
    std::println("release_callbacks,{},count", fixture.Releases());
    std::println("checksum,{},value", fixture.Checksum());
    return 0;
}

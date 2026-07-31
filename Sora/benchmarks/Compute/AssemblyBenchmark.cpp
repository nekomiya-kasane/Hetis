/**
 * @file AssemblyBenchmark.cpp
 * @brief Control-plane baselines for local, mixed, and direct-filled dispatch-input assembly.
 * @ingroup Compute
 */

#include <BenchmarkHarness.h>
#include <Sora/Compute/Runtime/ExecutableBatch.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <memory>
#include <print>
#include <utility>
#include <vector>

namespace {

    constexpr size_t kSamples = 9;
    constexpr auto kMinimumSampleTime = std::chrono::milliseconds(50);
    constexpr Sora::Compute::DeviceId kDevice{2, 1};
    constexpr Sora::Compute::MemoryLocation kLocation{
        .domain = {3, 1}, .device = kDevice, .backend = Sora::Compute::BackendKind::Cpu};
    constexpr uint64_t kPartBytes = 4096;
    constexpr uint64_t kAssemblyBytes = kPartBytes * 2;

    struct BackendState {};
    struct CodeOwner {};
    struct EventToken {};
    struct AllocationToken {};

    struct QueueState {
        std::atomic<uint64_t> submissions{0};
        std::atomic<uint64_t> commands{0};
        std::atomic<uint64_t> waits{0};
        std::atomic<uint32_t> lastCommandCount{0};
        std::atomic<uint32_t> lastWaitCount{0};
    };

    void DestroyBackend(void* state) noexcept {
        delete static_cast<BackendState*>(state);
    }
    void DestroyQueue(void* state) noexcept {
        delete static_cast<QueueState*>(state);
    }
    void NoopDestroy(void*) noexcept {}

    void ObserveCompleted(void*, Sora::Compute::BackendEventObservation* observation) noexcept {
        *observation = {.status = Sora::Compute::EventStatus::Completed};
    }

    void WaitCompleted(void*, uint64_t, Sora::Compute::BackendEventObservation* observation) noexcept {
        *observation = {.status = Sora::Compute::EventStatus::Completed};
    }

    void Submit(void* opaque, const Sora::Compute::BackendSubmitRequest* request,
                Sora::Compute::BackendSubmitResult* result) noexcept {
        auto& state = *static_cast<QueueState*>(opaque);
        state.submissions.fetch_add(1, std::memory_order_relaxed);
        state.commands.fetch_add(request->commandCount, std::memory_order_relaxed);
        state.waits.fetch_add(request->waitCount, std::memory_order_relaxed);
        state.lastCommandCount.store(request->commandCount, std::memory_order_relaxed);
        state.lastWaitCount.store(request->waitCount, std::memory_order_relaxed);
        result->event = {.device = kDevice,
                         .backend = Sora::Compute::BackendKind::Cpu,
                         .state = opaque,
                         .destroy = NoopDestroy,
                         .query = ObserveCompleted,
                         .wait = WaitCompleted};
        result->error = {};
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

    template<typename T, typename Error>
    [[nodiscard]] T Take(std::expected<T, Error> value) {
        if (!value) {
            std::terminate();
        }
        return std::move(*value);
    }

    enum class AssemblyCase : uint8_t {
        Local,
        Mixed,
        AllPrefilled,
    };

    class AssemblyFixture final {
    public:
        explicit AssemblyFixture(AssemblyCase assemblyCase)
            : assemblyCase_(assemblyCase),
              queue_(MakeQueue()),
              sourceA_(MakeAllocation(sourceAToken_, {10, 1}, kPartBytes)),
              sourceB_(MakeAllocation(sourceBToken_, {10, 2}, kPartBytes)),
              destination_(MakeAllocation(destinationToken_, {10, 3}, kAssemblyBytes)),
              remoteA_(MakeEvent(remoteAToken_)),
              remoteB_(MakeEvent(remoteBToken_)) {}

        void Invoke() noexcept {
            try {
                Sora::Compute::ExecutableBatchStorage storage{.queue = queue_};
                switch (assemblyCase_) {
                    case AssemblyCase::Local:
                        storage.commands = {Copy(sourceA_, destination_, 0), Copy(sourceB_, destination_, kPartBytes)};
                        storage.allocations = {sourceA_, sourceB_, destination_};
                        break;
                    case AssemblyCase::Mixed:
                        storage.commands = {Copy(sourceA_, destination_, 0)};
                        storage.allocations = {sourceA_, destination_};
                        storage.waits = {remoteB_};
                        break;
                    case AssemblyCase::AllPrefilled:
                        storage.allocations = {destination_};
                        storage.waits = {remoteA_, remoteB_};
                        storage.signalOnly = true;
                        break;
                }
                auto batch = Sora::Compute::ExecutableBatch::Create(std::move(storage));
                if (!batch) {
                    failed_ = true;
                    return;
                }
                auto submission = std::move(*batch).Submit();
                if (!submission || submission->Query().status != Sora::Compute::EventStatus::Completed) {
                    failed_ = true;
                    return;
                }
                checksum_.fetch_add(1, std::memory_order_relaxed);
            } catch (...) {
                failed_ = true;
            }
        }

        [[nodiscard]] bool Validate(uint32_t commandCount, uint32_t waitCount) noexcept {
            Invoke();
            return !failed_ && state_->lastCommandCount.load(std::memory_order_relaxed) == commandCount &&
                   state_->lastWaitCount.load(std::memory_order_relaxed) == waitCount;
        }

        [[nodiscard]] uint64_t Checksum() const noexcept {
            return checksum_.load(std::memory_order_relaxed) + state_->submissions.load(std::memory_order_relaxed) +
                   state_->commands.load(std::memory_order_relaxed) + state_->waits.load(std::memory_order_relaxed);
        }

    private:
        [[nodiscard]] Sora::Compute::Queue MakeQueue() {
            auto state = std::make_unique<QueueState>();
            state_ = state.get();
            return Take(
                Sora::Compute::Queue::Adopt(BenchmarkService(), {.id = {4, 1},
                                                                 .device = kDevice,
                                                                 .backend = Sora::Compute::BackendKind::Cpu,
                                                                 .capabilities = Sora::Compute::QueueCapability::Copy,
                                                                 .state = state.release(),
                                                                 .destroy = DestroyQueue,
                                                                 .submit = Submit}));
        }

        [[nodiscard]] static Sora::Compute::Allocation
        MakeAllocation(AllocationToken& token, Sora::Compute::PhysicalMemoryId memory, uint64_t sizeBytes) {
            return Take(Sora::Compute::Allocation::Adopt(BenchmarkService(),
                                                         {.location = kLocation,
                                                          .memory = memory,
                                                          .properties = Sora::Compute::MemoryProperty::HostVisible,
                                                          .sizeBytes = sizeBytes,
                                                          .alignment = 64,
                                                          .state = &token,
                                                          .destroy = NoopDestroy}));
        }

        [[nodiscard]] static Sora::Compute::Event MakeEvent(EventToken& token) {
            return Take(Sora::Compute::Event::Adopt(BenchmarkService(), {.device = kDevice,
                                                                         .backend = Sora::Compute::BackendKind::Cpu,
                                                                         .state = &token,
                                                                         .destroy = NoopDestroy,
                                                                         .query = ObserveCompleted,
                                                                         .wait = WaitCompleted}));
        }

        [[nodiscard]] static Sora::Compute::BackendCommand Copy(const Sora::Compute::Allocation& source,
                                                                const Sora::Compute::Allocation& destination,
                                                                uint64_t destinationOffset) noexcept {
            return {.kind = Sora::Compute::BackendCommandKind::Copy,
                    .source = source.View(),
                    .target = destination.View(),
                    .targetOffset = destinationOffset,
                    .sizeBytes = kPartBytes};
        }

        AssemblyCase assemblyCase_;
        QueueState* state_ = nullptr;
        AllocationToken sourceAToken_{};
        AllocationToken sourceBToken_{};
        AllocationToken destinationToken_{};
        EventToken remoteAToken_{};
        EventToken remoteBToken_{};
        Sora::Compute::Queue queue_{};
        Sora::Compute::Allocation sourceA_{};
        Sora::Compute::Allocation sourceB_{};
        Sora::Compute::Allocation destination_{};
        Sora::Compute::Event remoteA_{};
        Sora::Compute::Event remoteB_{};
        std::atomic<uint64_t> checksum_{0};
        bool failed_ = false;
    };

} // namespace

int main() {
    AssemblyFixture local{AssemblyCase::Local};
    AssemblyFixture mixed{AssemblyCase::Mixed};
    AssemblyFixture allPrefilled{AssemblyCase::AllPrefilled};
    if (!local.Validate(2, 0) || !mixed.Validate(1, 1) || !allPrefilled.Validate(0, 2)) {
        return 2;
    }

    auto localCase = [&]() noexcept { local.Invoke(); };
    auto mixedCase = [&]() noexcept { mixed.Invoke(); };
    auto allPrefilledCase = [&]() noexcept { allPrefilled.Invoke(); };
    const std::array cases{Sora::Benchmark::MakeCase("local_or_legacy_two_copy", localCase),
                           Sora::Benchmark::MakeCase("mixed_prefill_and_copy", mixedCase),
                           Sora::Benchmark::MakeCase("all_prefilled_signal_only", allPrefilledCase)};
    const auto timing = Sora::Benchmark::MeasureInterleavedIndependent<kSamples>(cases, 1, kMinimumSampleTime);
    const uint64_t checksum = local.Checksum() + mixed.Checksum() + allPrefilled.Checksum();
    if (checksum == 0) {
        return 3;
    }

    std::println("metric,value,unit");
    std::println("local_or_legacy_two_copy,{:.6f},ns/assembly", timing.MedianTime(0));
    std::println("mixed_prefill_and_copy,{:.6f},ns/assembly", timing.MedianTime(1));
    std::println("all_prefilled_signal_only,{:.6f},ns/assembly", timing.MedianTime(2));
    std::println("all_prefilled_over_legacy_two_copy,{:.6f},ratio", timing.PairedRatio(2, 0));
    std::println("local_retained_allocations,3,count");
    std::println("local_assembly_commands,2,count");
    std::println("local_predecessor_waits,0,count");
    std::println("mixed_retained_allocations,2,count");
    std::println("mixed_assembly_commands,1,count");
    std::println("mixed_predecessor_waits,1,count");
    std::println("direct_fill_retained_allocations,1,count");
    std::println("direct_fill_assembly_commands,0,count");
    std::println("direct_fill_predecessor_waits,2,count");
    std::println("legacy_total_allocations,3,count");
    std::println("direct_fill_total_allocations,1,count");
    std::println("legacy_total_physical_submissions,3,count");
    std::println("direct_fill_total_physical_submissions,3,count");
    std::println("legacy_final_assembly_queue_submits,1,count");
    std::println("direct_fill_final_assembly_queue_submits,1,count");
    std::println("local_repetitions,{},count", timing.repetitions[0]);
    std::println("mixed_repetitions,{},count", timing.repetitions[1]);
    std::println("all_prefilled_repetitions,{},count", timing.repetitions[2]);
    std::println("checksum,{},value", checksum);
    return 0;
}

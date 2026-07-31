/**
 * @file HostTransferBridgeBenchmark.cpp
 * @brief Inter-service host-bridge throughput and facade-overhead baselines.
 */

#include <BenchmarkHarness.h>
#include <Sora/Compute/Backend/CpuBackend.h>
#include <Sora/Compute/Runtime/HostTransferBridge.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <print>
#include <span>
#include <utility>
#include <vector>

namespace {

    using namespace std::chrono_literals;
    using namespace Sora::Compute;

    constexpr size_t kSampleCount = 7;
    constexpr auto kMinimumSampleTime = 40ms;
    constexpr uint64_t kSmallBytes = 4U << 10U;
    constexpr uint64_t kLargeBytes = 16U << 20U;

    struct BufferPair {
        Allocation source;
        Allocation destination;
        std::span<const std::byte> sourceBytes;
        std::span<std::byte> destinationBytes;
    };

    class Fixture final {
    public:
        [[nodiscard]] static std::expected<Fixture, uint64_t> Create() noexcept {
            auto source = CpuBackend::Create({.workerCount = 1});
            auto destination = CpuBackend::Create({.workerCount = 1});
            if (!source || !destination) {
                return std::unexpected(1);
            }
            const std::array accessors{source->HostAccessor(), destination->HostAccessor()};
            auto bridges = CreateHostTransferBridges(accessors);
            if (!bridges) {
                return std::unexpected(2);
            }
            const auto found = std::ranges::find_if(*bridges, [&](const TransferEngine& bridge) {
                return bridge.SourceService() == source->Observer().Service() &&
                       bridge.Service() == destination->Observer().Service();
            });
            if (found == bridges->end()) {
                return std::unexpected(3);
            }
            auto small = MakeBuffers(*source, *destination, kSmallBytes);
            auto large = MakeBuffers(*source, *destination, kLargeBytes);
            if (!small || !large) {
                return std::unexpected(4);
            }
            return Fixture{std::move(*source), std::move(*destination), *found, std::move(*small), std::move(*large)};
        }

        void DirectSmall() noexcept { Direct(small_); }
        void BridgeSmall() noexcept {
            const std::array regions{TransferRegion{.sizeBytes = kSmallBytes}};
            Bridge(small_, regions);
        }
        void BridgeSmallScatter4() noexcept {
            constexpr uint64_t chunkBytes = kSmallBytes / 4;
            const std::array regions{
                TransferRegion{.sizeBytes = chunkBytes},
                TransferRegion{.sourceOffset = chunkBytes, .targetOffset = chunkBytes, .sizeBytes = chunkBytes},
                TransferRegion{.sourceOffset = chunkBytes * 2, .targetOffset = chunkBytes * 2, .sizeBytes = chunkBytes},
                TransferRegion{
                    .sourceOffset = chunkBytes * 3, .targetOffset = chunkBytes * 3, .sizeBytes = chunkBytes}};
            Bridge(small_, regions);
        }
        void DirectLarge() noexcept { Direct(large_); }
        void BridgeLarge() noexcept {
            const std::array regions{TransferRegion{.sizeBytes = kLargeBytes}};
            Bridge(large_, regions);
        }

        [[nodiscard]] bool Failed() const noexcept { return failed_; }
        [[nodiscard]] uint64_t Checksum() const noexcept { return checksum_; }

    private:
        Fixture(CpuBackend source, CpuBackend destination, TransferEngine bridge, BufferPair small,
                BufferPair large) noexcept
            : source_(std::move(source)),
              destination_(std::move(destination)),
              bridge_(std::move(bridge)),
              small_(std::move(small)),
              large_(std::move(large)) {}

        [[nodiscard]] static BackendAllocationRequest Request(const CpuBackend& backend, uint64_t bytes) noexcept {
            return {.location = backend.HostLocation(),
                    .requiredProperties = Detail::kCoherentHostMemoryProperties,
                    .preferredProperties = Detail::kCoherentHostMemoryProperties,
                    .sizeBytes = bytes,
                    .alignment = 64};
        }

        [[nodiscard]] static std::expected<BufferPair, uint64_t>
        MakeBuffers(CpuBackend& sourceBackend, CpuBackend& destinationBackend, uint64_t bytes) noexcept {
            auto source = sourceBackend.HostAllocator().Allocate(Request(sourceBackend, bytes));
            auto destination = destinationBackend.HostAllocator().Allocate(Request(destinationBackend, bytes));
            if (!source || !destination) {
                return std::unexpected(1);
            }
            auto sourceBytes = sourceBackend.HostAccessor().Write(*source, 0, bytes);
            auto destinationBytes = destinationBackend.HostAccessor().Write(*destination, 0, bytes);
            if (!sourceBytes || !destinationBytes) {
                return std::unexpected(2);
            }
            for (size_t index = 0; index < sourceBytes->size(); ++index) {
                (*sourceBytes)[index] = std::byte{static_cast<uint8_t>(index * 17U + 1U)};
            }
            return BufferPair{std::move(*source), std::move(*destination),
                              std::span<const std::byte>{sourceBytes->data(), sourceBytes->size()}, *destinationBytes};
        }

        void Direct(BufferPair& buffers) noexcept {
            std::memcpy(buffers.destinationBytes.data(), buffers.sourceBytes.data(), buffers.sourceBytes.size());
            std::atomic_signal_fence(std::memory_order_seq_cst);
            checksum_ +=
                std::to_integer<uint8_t>(buffers.destinationBytes[checksum_ % buffers.destinationBytes.size()]);
        }

        void Bridge(BufferPair& buffers, std::span<const TransferRegion> regions) noexcept {
            auto submission = bridge_.Submit(buffers.source, buffers.destination, regions);
            if (!submission || submission->WaitUntilTerminal().status != EventStatus::Completed) {
                failed_ = true;
                return;
            }
            checksum_ +=
                std::to_integer<uint8_t>(buffers.destinationBytes[checksum_ % buffers.destinationBytes.size()]);
        }

        CpuBackend source_;
        CpuBackend destination_;
        TransferEngine bridge_;
        BufferPair small_;
        BufferPair large_;
        uint64_t checksum_ = 0;
        bool failed_ = false;
    };

} // namespace

int main() {
    auto fixture = Fixture::Create();
    if (!fixture) {
        std::println(stderr, "host bridge fixture construction failed: {}", fixture.error());
        return 2;
    }
    auto directSmall = [&fixture]() noexcept { fixture->DirectSmall(); };
    auto bridgeSmall = [&fixture]() noexcept { fixture->BridgeSmall(); };
    auto bridgeSmallScatter4 = [&fixture]() noexcept { fixture->BridgeSmallScatter4(); };
    auto directLarge = [&fixture]() noexcept { fixture->DirectLarge(); };
    auto bridgeLarge = [&fixture]() noexcept { fixture->BridgeLarge(); };
    const std::array cases{
        Sora::Benchmark::MakeCase("direct_4k", directSmall),
        Sora::Benchmark::MakeCase("bridge_4k", bridgeSmall),
        Sora::Benchmark::MakeCase("bridge_4k_scatter_4", bridgeSmallScatter4),
        Sora::Benchmark::MakeCase("direct_16m", directLarge),
        Sora::Benchmark::MakeCase("bridge_16m", bridgeLarge),
    };
    const auto timing = Sora::Benchmark::MeasureInterleavedIndependent<kSampleCount>(cases, 1, kMinimumSampleTime);
    if (fixture->Failed()) {
        return 2;
    }

    const auto gibPerSecond = [](double nanoseconds, uint64_t bytes) {
        return static_cast<double>(bytes) / nanoseconds * 1.0e9 / static_cast<double>(uint64_t{1} << 30U);
    };
    std::println("family,payload,metric,value,unit");
    std::println("host_bridge,4096,direct,{:.6f},ns/transfer", timing.MedianTime(0));
    std::println("host_bridge,4096,facade,{:.6f},ns/transfer", timing.MedianTime(1));
    std::println("host_bridge,4096,facade_over_direct,{:.6f},ratio", timing.PairedRatio(1, 0));
    std::println("host_bridge,4096,facade_scatter_4,{:.6f},ns/transfer", timing.MedianTime(2));
    std::println("host_bridge,4096,scatter_4_over_single,{:.6f},ratio", timing.PairedRatio(2, 1));
    std::println("host_bridge,16777216,direct,{:.6f},GiB/s", gibPerSecond(timing.MedianTime(3), kLargeBytes));
    std::println("host_bridge,16777216,facade,{:.6f},GiB/s", gibPerSecond(timing.MedianTime(4), kLargeBytes));
    std::println("host_bridge,16777216,facade_over_direct,{:.6f},ratio", timing.PairedRatio(4, 3));
    std::println("checksum,{},bridge_copy,{},value", kLargeBytes, fixture->Checksum());
}

/**
 * @file ArtifactStoreBenchmark.cpp
 * @brief Persistent artifact lookup, publication, and bounded-maintenance baseline.
 */

#include <BenchmarkHarness.h>
#include <Sora/Compute/ArtifactStore.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <print>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace {

    constexpr size_t kSamples = 7;
    constexpr auto kMinimumSampleTime = std::chrono::milliseconds(20);
    constexpr std::array<std::byte, 64> kPayload{};

    constexpr Sora::Compute::TargetFingerprint kTarget{
        .device = {0xCA11, 0xAB1E},
        .backend = Sora::Compute::BackendKind::Cpu,
        .hostArchitecture = Sora::CpuArchitecture::X64,
        .operatingSystem = Sora::OperatingSystem::Windows,
        .binaryFormat = Sora::BinaryFormat::PE,
        .byteOrder = Sora::ByteOrder::Little,
        .abi = Sora::AbiFamily::MSVC,
        .identityStability = Sora::Compute::TargetIdentityStability::MachineStable,
        .addressBits = 64,
        .architectureId = 0x7838365F3634ULL,
        .fixedVectorBits = 256,
        .compilerIdentity = {0xC0, 0xC1},
        .driverIdentity = {0xD0, 0xD1},
    };

    [[nodiscard]] constexpr Sora::Compute::ArtifactKey Key(uint64_t identity) noexcept {
        return Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
            .lowered = {0x10, 0x11},
            .target = kTarget,
            .specialization = {.shape = {0x20, 0x21}, .layout = {0x30, 0x31}, .tuning = {identity, 0x41}},
        });
    }

    class TemporaryDirectory final {
    public:
        explicit TemporaryDirectory(std::string_view name) {
            static std::atomic<uint64_t> sequence = 0;
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path_ = std::filesystem::temp_directory_path() /
                    std::format("sora-artifact-store-benchmark-{}-{}-{}", name, stamp,
                                sequence.fetch_add(1, std::memory_order_relaxed));
        }

        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }

        TemporaryDirectory(const TemporaryDirectory&) = delete;
        TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

        [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

    private:
        std::filesystem::path path_;
    };

    template<size_t EntryCount, uint64_t ReconciliationInterval = 256>
    class ArtifactStoreFixture final {
    public:
        explicit ArtifactStoreFixture(std::string_view name) : directory_(name) {
            auto seed = Sora::Compute::ArtifactStore::Create({
                .root = directory_.Path(),
                .maximumPayloadBytes = 4096,
                .maximumStorageBytes = uint64_t{64} << 20U,
                .maximumEntryCount = 4096,
                .reconciliationPublicationInterval = 256,
                .durableWrites = false,
            });
            if (!seed) {
                failed_ = true;
                return;
            }
            for (size_t index = 0; index < EntryCount; ++index) {
                if (!seed->Store(Key(index + 1), kPayload)) {
                    failed_ = true;
                    return;
                }
            }
            if constexpr (ReconciliationInterval == 256) {
                store_.emplace(std::move(*seed));
            } else {
                auto measured = Sora::Compute::ArtifactStore::Create({
                    .root = directory_.Path(),
                    .maximumPayloadBytes = 4096,
                    .maximumStorageBytes = uint64_t{64} << 20U,
                    .maximumEntryCount = 4096,
                    .reconciliationPublicationInterval = ReconciliationInterval,
                    .durableWrites = false,
                });
                if (!measured) {
                    failed_ = true;
                    return;
                }
                store_.emplace(std::move(*measured));
            }
        }

        void Maintain() noexcept {
            const auto result = store_->Maintain();
            if (!result) {
                failed_ = true;
                return;
            }
            checksum_ += result->retainedEntries + result->retainedBytes;
        }

        void Load() noexcept {
            const auto result = store_->Load(Key(EntryCount));
            if (!result) {
                failed_ = true;
                return;
            }
            checksum_ += result->size();
        }

        void Store() noexcept {
            if (!store_->Store(Key(EntryCount), kPayload)) {
                failed_ = true;
                return;
            }
            checksum_ += kPayload.size();
        }

        [[nodiscard]] bool Failed() const noexcept { return failed_; }
        [[nodiscard]] uint64_t Checksum() const noexcept { return checksum_; }

    private:
        TemporaryDirectory directory_;
        std::optional<Sora::Compute::ArtifactStore> store_;
        uint64_t checksum_ = 0;
        bool failed_ = false;
    };

    template<size_t SampleCount>
    [[nodiscard]] std::optional<double> MeasureContendedHandoff(Sora::Compute::ArtifactStore& leaderStore,
                                                                Sora::Compute::ArtifactStore& followerStore) {
        std::array<double, SampleCount> samples{};
        for (double& sample : samples) {
            auto leader = leaderStore.AcquireMaterializationLease(Key(0xFEED));
            if (!leader) {
                return std::nullopt;
            }

            std::atomic<bool> started = false;
            std::optional<std::expected<Sora::Compute::ArtifactMaterializationLease, Sora::Compute::ArtifactStoreError>>
                follower;
            std::chrono::steady_clock::time_point acquiredAt{};
            std::jthread waiter{[&] {
                started.store(true, std::memory_order_release);
                follower = followerStore.AcquireMaterializationLease(Key(0xFEED));
                acquiredAt = std::chrono::steady_clock::now();
            }};
            while (!started.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{3});
            const auto releasedAt = std::chrono::steady_clock::now();
            leader = std::unexpected(Sora::Compute::ArtifactStoreError{});
            waiter.join();
            if (!follower || !*follower || !follower->value().ObservedContention()) {
                return std::nullopt;
            }
            sample = std::chrono::duration<double, std::nano>{acquiredAt - releasedAt}.count();
        }
        std::ranges::sort(samples);
        return samples[SampleCount / 2];
    }

} // namespace

int main() {
    ArtifactStoreFixture<64> small{"64"};
    ArtifactStoreFixture<256> large{"256"};
    ArtifactStoreFixture<256, 1> reconcileEveryPublication{"256-eager"};
    if (small.Failed() || large.Failed() || reconcileEveryPublication.Failed()) {
        return 2;
    }

    TemporaryDirectory leaseDirectory{"lease"};
    const Sora::Compute::ArtifactStoreConfig leaseConfig{
        .root = leaseDirectory.Path(),
        .maximumPayloadBytes = 4096,
        .materializationLeaseTimeout = std::chrono::seconds{2},
        .leasePollInterval = std::chrono::milliseconds{1},
        .durableWrites = false,
    };
    auto leaseStore = Sora::Compute::ArtifactStore::Create(leaseConfig);
    auto followerStore = Sora::Compute::ArtifactStore::Create(leaseConfig);
    if (!leaseStore || !followerStore) {
        return 2;
    }

    auto maintain64 = [&small]() noexcept { small.Maintain(); };
    auto maintain256 = [&large]() noexcept { large.Maintain(); };
    auto load256 = [&large]() noexcept { large.Load(); };
    auto store256 = [&large]() noexcept { large.Store(); };
    auto store256Eager = [&reconcileEveryPublication]() noexcept { reconcileEveryPublication.Store(); };
    uint64_t leaseChecksum = 0;
    bool leaseFailed = false;
    auto acquireLease = [&]() noexcept {
        auto lease = leaseStore->AcquireMaterializationLease(Key(0xFEED));
        if (!lease) {
            leaseFailed = true;
            return;
        }
        ++leaseChecksum;
    };
    const std::array cases{Sora::Benchmark::MakeCase("maintain_64", maintain64),
                           Sora::Benchmark::MakeCase("maintain_256", maintain256),
                           Sora::Benchmark::MakeCase("load_256", load256),
                           Sora::Benchmark::MakeCase("store_256", store256),
                           Sora::Benchmark::MakeCase("store_256_eager", store256Eager),
                           Sora::Benchmark::MakeCase("materialization_lease", acquireLease)};
    const auto timing = Sora::Benchmark::MeasureInterleavedIndependent<kSamples>(cases, 1, kMinimumSampleTime);
    const auto contendedHandoff = MeasureContendedHandoff<kSamples>(*leaseStore, *followerStore);
    const uint64_t checksum =
        small.Checksum() + large.Checksum() + reconcileEveryPublication.Checksum() + leaseChecksum;
    if (small.Failed() || large.Failed() || reconcileEveryPublication.Failed() || leaseFailed || !contendedHandoff ||
        checksum == 0 || checksum == std::numeric_limits<uint64_t>::max()) {
        return 3;
    }

    std::println("metric,value,unit");
    std::println("artifact_store_maintain_64,{:.6f},ns/operation", timing.MedianTime(0));
    std::println("artifact_store_maintain_256,{:.6f},ns/operation", timing.MedianTime(1));
    std::println("artifact_store_maintain_256_over_64,{:.6f},ratio", timing.PairedRatio(1, 0));
    std::println("artifact_store_warm_load_256,{:.6f},ns/operation", timing.MedianTime(2));
    std::println("artifact_store_publication_256,{:.6f},ns/operation", timing.MedianTime(3));
    std::println("artifact_store_eager_reconciliation_publication_256,{:.6f},ns/operation", timing.MedianTime(4));
    std::println("artifact_store_eager_over_amortized_publication_256,{:.6f},ratio", timing.PairedRatio(4, 3));
    std::println("artifact_store_materialization_lease_uncontended,{:.6f},ns/operation", timing.MedianTime(5));
    std::println("artifact_store_materialization_lease_contended_handoff,{:.6f},ns/handoff", *contendedHandoff);
    std::println("maintain_64_repetitions,{},count", timing.repetitions[0]);
    std::println("maintain_256_repetitions,{},count", timing.repetitions[1]);
    std::println("load_256_repetitions,{},count", timing.repetitions[2]);
    std::println("store_256_repetitions,{},count", timing.repetitions[3]);
    std::println("store_256_eager_repetitions,{},count", timing.repetitions[4]);
    std::println("materialization_lease_repetitions,{},count", timing.repetitions[5]);
    std::println("checksum,{},value", checksum);
}

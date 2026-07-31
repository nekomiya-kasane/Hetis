#include <Sora/Compute/ArtifactStore.h>
#include <Sora/Core/PAL/File.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace {

    class TemporaryDirectory final {
    public:
        TemporaryDirectory() {
            static std::atomic<uint64_t> sequence = 0;
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path_ =
                std::filesystem::temp_directory_path() /
                std::format("sora-artifact-store-test-{}-{}", stamp, sequence.fetch_add(1, std::memory_order_relaxed));
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

    constexpr Sora::Compute::TargetFingerprint kPersistentTarget{
        .device = {0xCA11, 0xAB1E},
        .backend = Sora::Compute::BackendKind::Cpu,
        .hostArchitecture = Sora::CpuArchitecture::X64,
        .operatingSystem = Sora::OperatingSystem::Windows,
        .binaryFormat = Sora::BinaryFormat::PE,
        .byteOrder = Sora::ByteOrder::Little,
        .abi = Sora::AbiFamily::MSVC,
        .identityStability = Sora::Compute::TargetIdentityStability::MachineStable,
        .addressBits = 64,
        .features = Sora::Compute::DeviceFeature::FusedMultiplyAdd,
        .architectureId = 0x7838365F3634ULL,
        .fixedVectorBits = 256,
        .compilerIdentity = {0xC0, 0xC1},
        .driverIdentity = {0xD0, 0xD1},
    };

    constexpr uint64_t kArtifactStoreV1FixedHeaderSize = 92;

    [[nodiscard]] constexpr Sora::Compute::ArtifactKey ExecutableKey(uint64_t tuning) {
        return Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
            .lowered = {0x10, 0x11},
            .target = kPersistentTarget,
            .specialization = {.shape = {0x20, 0x21}, .layout = {0x30, 0x31}, .tuning = {tuning, 0x41}},
        });
    }

    constexpr Sora::Compute::ArtifactKey kExecutableKey = ExecutableKey(0x40);

    constexpr std::array kPayload{std::byte{0x53}, std::byte{0x4F}, std::byte{0x52}, std::byte{0x41},
                                  std::byte{0x2D}, std::byte{0x4A}, std::byte{0x49}, std::byte{0x54}};

    Sora::Compute::ArtifactStore MakeStore(const std::filesystem::path& root, uint64_t maximumPayload = 4096,
                                           uint64_t maximumStorage = uint64_t{1} << 20U, uint64_t maximumEntries = 64,
                                           uint64_t reconciliationPublications = 256,
                                           uint64_t reconciliationBytes = uint64_t{64} << 20U) {
        auto store = Sora::Compute::ArtifactStore::Create({
            .root = root,
            .maximumPayloadBytes = maximumPayload,
            .maximumStorageBytes = maximumStorage,
            .maximumEntryCount = maximumEntries,
            .reconciliationPublicationInterval = reconciliationPublications,
            .reconciliationByteInterval = reconciliationBytes,
            .durableWrites = false,
        });
        REQUIRE(store.has_value());
        return std::move(*store);
    }

    void WriteByte(const std::filesystem::path& path, uint64_t offset, std::byte value) {
        auto file =
            Sora::PAL::File::Open(path, {
                                            .access = Sora::PAL::FileAccess::Read | Sora::PAL::FileAccess::Write,
                                            .creation = Sora::PAL::FileCreation::OpenExisting,
                                        });
        REQUIRE(file.has_value());
        REQUIRE(file->WriteAllAt(std::span{&value, 1}, offset).has_value());
    }

} // namespace

TEST_CASE("ArtifactStore atomically round-trips one exact persistent artifact", "[Sora][Compute][Artifact][Store]") {
    STATIC_REQUIRE(kExecutableKey.Valid());
    STATIC_REQUIRE(kPersistentTarget.Valid());
    STATIC_REQUIRE(kExecutableKey.PersistentCacheable());
    STATIC_REQUIRE(Sora::Compute::DigestArtifactBytes(kPayload).Valid());

    TemporaryDirectory temporary;
    auto store = MakeStore(temporary.Path());
    REQUIRE(store.Store(kExecutableKey, kPayload).has_value());

    auto loaded = store.Load(kExecutableKey);
    REQUIRE(loaded.has_value());
    REQUIRE(std::ranges::equal(*loaded, kPayload));

    auto path = store.EntryPath(kExecutableKey);
    REQUIRE(path.has_value());
    REQUIRE(std::filesystem::is_regular_file(*path));
    REQUIRE(path->extension() == ".sora-artifact");
    for (const auto& entry : std::filesystem::directory_iterator(path->parent_path())) {
        REQUIRE(entry.path().extension() == ".sora-artifact");
    }
}

TEST_CASE("ArtifactStore distinguishes misses, capability gates, and payload limits",
          "[Sora][Compute][Artifact][Store][Validation]") {
    TemporaryDirectory temporary;
    auto store = MakeStore(temporary.Path(), kPayload.size());

    auto missing = store.Load(kExecutableKey);
    REQUIRE_FALSE(missing.has_value());
    REQUIRE(missing.error().code == Sora::Compute::ArtifactStoreErrorCode::NotFound);

    auto processTarget = kPersistentTarget;
    processTarget.identityStability = Sora::Compute::TargetIdentityStability::ProcessStable;
    const auto processKey = Sora::Compute::MakeArtifactKey(
        Sora::Compute::ExecutableKey{.lowered = {0x10, 0x11}, .target = processTarget, .specialization = {}});
    auto nonPersistent = store.Store(processKey, kPayload);
    REQUIRE_FALSE(nonPersistent.has_value());
    REQUIRE(nonPersistent.error().code == Sora::Compute::ArtifactStoreErrorCode::NonPersistentIdentity);

    std::array<std::byte, kPayload.size() + 1> oversized{};
    auto tooLarge = store.Store(kExecutableKey, oversized);
    REQUIRE_FALSE(tooLarge.has_value());
    REQUIRE(tooLarge.error().code == Sora::Compute::ArtifactStoreErrorCode::PayloadTooLarge);

    TemporaryDirectory capacityTemporary;
    auto capacityStore = MakeStore(capacityTemporary.Path(), 4096, 1, 1);
    auto noCapacity = capacityStore.Store(kExecutableKey, kPayload);
    REQUIRE_FALSE(noCapacity.has_value());
    REQUIRE(noCapacity.error().code == Sora::Compute::ArtifactStoreErrorCode::CapacityExceeded);
}

TEST_CASE("ArtifactStore enforces total byte and entry budgets outside the lookup path",
          "[Sora][Compute][Artifact][Store][Maintenance]") {
    SECTION("entry count") {
        TemporaryDirectory temporary;
        auto store = MakeStore(temporary.Path(), 4096, uint64_t{1} << 20U, 2);
        constexpr auto oldest = ExecutableKey(1);
        constexpr auto middle = ExecutableKey(2);
        constexpr auto newest = ExecutableKey(3);
        REQUIRE(store.Store(oldest, kPayload).has_value());
        auto oldestPath = store.EntryPath(oldest);
        REQUIRE(oldestPath.has_value());
        std::filesystem::last_write_time(*oldestPath,
                                         std::filesystem::file_time_type::clock::now() - std::chrono::hours{2});
        REQUIRE(store.Store(middle, kPayload).has_value());
        auto middlePath = store.EntryPath(middle);
        REQUIRE(middlePath.has_value());
        std::filesystem::last_write_time(*middlePath,
                                         std::filesystem::file_time_type::clock::now() - std::chrono::hours{1});
        REQUIRE(store.Store(newest, kPayload).has_value());

        auto evicted = store.Load(oldest);
        REQUIRE_FALSE(evicted.has_value());
        REQUIRE(evicted.error().code == Sora::Compute::ArtifactStoreErrorCode::NotFound);
        REQUIRE(store.Load(middle).has_value());
        REQUIRE(store.Load(newest).has_value());
        auto maintained = store.Maintain();
        REQUIRE(maintained.has_value());
        REQUIRE(maintained->retainedEntries == 2);
        REQUIRE(maintained->retainedBytes <= (uint64_t{1} << 20U));
    }

    SECTION("total bytes") {
        TemporaryDirectory temporary;
        auto initial = MakeStore(temporary.Path());
        constexpr auto oldest = ExecutableKey(11);
        constexpr auto middle = ExecutableKey(12);
        constexpr auto newest = ExecutableKey(13);
        REQUIRE(initial.Store(oldest, kPayload).has_value());
        auto oldestPath = initial.EntryPath(oldest);
        REQUIRE(oldestPath.has_value());
        const uint64_t entryBytes = std::filesystem::file_size(*oldestPath);
        std::filesystem::last_write_time(*oldestPath,
                                         std::filesystem::file_time_type::clock::now() - std::chrono::hours{2});

        auto bounded = MakeStore(temporary.Path(), 4096, entryBytes * 2, 64);
        REQUIRE(bounded.Store(middle, kPayload).has_value());
        auto middlePath = bounded.EntryPath(middle);
        REQUIRE(middlePath.has_value());
        std::filesystem::last_write_time(*middlePath,
                                         std::filesystem::file_time_type::clock::now() - std::chrono::hours{1});
        REQUIRE(bounded.Store(newest, kPayload).has_value());

        REQUIRE_FALSE(bounded.Load(oldest).has_value());
        REQUIRE(bounded.Load(middle).has_value());
        REQUIRE(bounded.Load(newest).has_value());
        auto maintained = bounded.Maintain();
        REQUIRE(maintained.has_value());
        REQUIRE(maintained->retainedBytes <= entryBytes * 2);
        REQUIRE(maintained->retainedEntries == 2);
    }

    SECTION("periodic reconciliation observes external publications") {
        TemporaryDirectory temporary;
        auto bounded = MakeStore(temporary.Path(), 4096, uint64_t{1} << 20U, 2, 2);
        auto external = MakeStore(temporary.Path(), 4096, uint64_t{1} << 20U, 64);
        REQUIRE(external.Store(ExecutableKey(21), kPayload).has_value());
        REQUIRE(external.Store(ExecutableKey(22), kPayload).has_value());
        REQUIRE(external.Store(ExecutableKey(23), kPayload).has_value());

        REQUIRE(bounded.Store(ExecutableKey(24), kPayload).has_value());
        REQUIRE(bounded.Store(ExecutableKey(25), kPayload).has_value());
        auto maintained = bounded.Maintain();
        REQUIRE(maintained.has_value());
        REQUIRE(maintained->retainedEntries == 2);
        REQUIRE(maintained->retainedBytes <= (uint64_t{1} << 20U));
    }
}

TEST_CASE("ArtifactStore rejects truncated and checksum-corrupted entries",
          "[Sora][Compute][Artifact][Store][Integrity]") {
    SECTION("truncated payload") {
        TemporaryDirectory temporary;
        auto store = MakeStore(temporary.Path());
        REQUIRE(store.Store(kExecutableKey, kPayload).has_value());
        auto path = store.EntryPath(kExecutableKey);
        REQUIRE(path.has_value());
        auto file = Sora::PAL::File::Open(*path, {
                                                     .access = Sora::PAL::FileAccess::Write,
                                                     .creation = Sora::PAL::FileCreation::OpenExisting,
                                                 });
        REQUIRE(file.has_value());
        auto size = file->Size();
        REQUIRE(size.has_value());
        REQUIRE(file->Resize(*size - 1).has_value());
        REQUIRE(file->Close().has_value());
        auto loaded = store.Load(kExecutableKey);
        REQUIRE_FALSE(loaded.has_value());
        REQUIRE(loaded.error().code == Sora::Compute::ArtifactStoreErrorCode::Corrupted);
    }

    SECTION("payload checksum") {
        TemporaryDirectory temporary;
        auto store = MakeStore(temporary.Path());
        REQUIRE(store.Store(kExecutableKey, kPayload).has_value());
        auto path = store.EntryPath(kExecutableKey);
        REQUIRE(path.has_value());
        auto file = Sora::PAL::File::Open(*path);
        REQUIRE(file.has_value());
        auto size = file->Size();
        REQUIRE(size.has_value());
        REQUIRE(file->Close().has_value());
        WriteByte(*path, *size - 1, std::byte{0xFF});
        auto loaded = store.Load(kExecutableKey);
        REQUIRE_FALSE(loaded.has_value());
        REQUIRE(loaded.error().code == Sora::Compute::ArtifactStoreErrorCode::Corrupted);
    }

    SECTION("exact key words") {
        TemporaryDirectory temporary;
        auto store = MakeStore(temporary.Path());
        REQUIRE(store.Store(kExecutableKey, kPayload).has_value());
        auto path = store.EntryPath(kExecutableKey);
        REQUIRE(path.has_value());
        WriteByte(*path, kArtifactStoreV1FixedHeaderSize, std::byte{0xFF});
        auto loaded = store.Load(kExecutableKey);
        REQUIRE_FALSE(loaded.has_value());
        REQUIRE(loaded.error().code == Sora::Compute::ArtifactStoreErrorCode::Corrupted);
    }
}

TEST_CASE("ArtifactStore rejects unsupported wire versions", "[Sora][Compute][Artifact][Store][Version]") {
    TemporaryDirectory temporary;
    auto store = MakeStore(temporary.Path());
    REQUIRE(store.Store(kExecutableKey, kPayload).has_value());
    auto path = store.EntryPath(kExecutableKey);
    REQUIRE(path.has_value());
    WriteByte(*path, 4, std::byte{0x02});
    auto loaded = store.Load(kExecutableKey);
    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(loaded.error().code == Sora::Compute::ArtifactStoreErrorCode::UnsupportedVersion);
}

TEST_CASE("ArtifactStore concurrent identical writers publish one complete entry",
          "[Sora][Compute][Artifact][Store][Concurrent]") {
    TemporaryDirectory temporary;
    auto store = MakeStore(temporary.Path());
    constexpr size_t writerCount = 8;
    std::array<std::jthread, writerCount> writers;
    std::array<bool, writerCount> succeeded{};
    std::atomic<bool> start = false;
    for (size_t index = 0; index < writers.size(); ++index) {
        writers[index] = std::jthread{[&, index] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            succeeded[index] = store.Store(kExecutableKey, kPayload).has_value();
        }};
    }
    start.store(true, std::memory_order_release);
    for (auto& writer : writers) {
        writer.join();
    }
    REQUIRE(std::ranges::all_of(succeeded, std::identity{}));
    auto loaded = store.Load(kExecutableKey);
    REQUIRE(loaded.has_value());
    REQUIRE(std::ranges::equal(*loaded, kPayload));
}

TEST_CASE("ArtifactStore target upgrades map to distinct content addresses",
          "[Sora][Compute][Artifact][Store][Invalidation]") {
    TemporaryDirectory temporary;
    auto store = MakeStore(temporary.Path());
    auto upgradedTarget = kPersistentTarget;
    ++upgradedTarget.compilerIdentity.low;
    const auto upgraded = Sora::Compute::MakeArtifactKey(Sora::Compute::ExecutableKey{
        .lowered = {0x10, 0x11},
        .target = upgradedTarget,
        .specialization = {.shape = {0x20, 0x21}, .layout = {0x30, 0x31}, .tuning = {0x40, 0x41}},
    });
    auto oldPath = store.EntryPath(kExecutableKey);
    auto upgradedPath = store.EntryPath(upgraded);
    REQUIRE(oldPath.has_value());
    REQUIRE(upgradedPath.has_value());
    REQUIRE(*oldPath != *upgradedPath);
}

TEST_CASE("ArtifactStore bounds materialization locks and reports lease timeout",
          "[Sora][Compute][Artifact][Store][Lease]") {
    TemporaryDirectory temporary;
    const Sora::Compute::ArtifactStoreConfig config{
        .root = temporary.Path(),
        .maximumPayloadBytes = 4096,
        .materializationLeaseStripes = 4,
        .materializationLeaseTimeout = std::chrono::milliseconds{20},
        .leasePollInterval = std::chrono::milliseconds{1},
        .durableWrites = false,
    };
    auto firstStore = Sora::Compute::ArtifactStore::Create(config);
    auto secondStore = Sora::Compute::ArtifactStore::Create(config);
    REQUIRE(firstStore.has_value());
    REQUIRE(secondStore.has_value());

    auto first = firstStore->AcquireMaterializationLease(kExecutableKey);
    REQUIRE(first.has_value());
    REQUIRE_FALSE(first->ObservedContention());
    auto timedOut = secondStore->AcquireMaterializationLease(kExecutableKey);
    REQUIRE_FALSE(timedOut.has_value());
    REQUIRE(timedOut.error().code == Sora::Compute::ArtifactStoreErrorCode::LeaseTimeout);

    first = std::unexpected(Sora::Compute::ArtifactStoreError{});
    auto released = secondStore->AcquireMaterializationLease(kExecutableKey);
    REQUIRE(released.has_value());
    released = std::unexpected(Sora::Compute::ArtifactStoreError{});

    for (uint64_t identity = 1; identity <= 32; ++identity) {
        auto lease = firstStore->AcquireMaterializationLease(ExecutableKey(identity));
        REQUIRE(lease.has_value());
    }
    const auto lockDirectory = temporary.Path() / ".coordination" / "materialization";
    const auto lockCount = static_cast<size_t>(std::ranges::distance(std::filesystem::directory_iterator{lockDirectory},
                                                                     std::filesystem::directory_iterator{}));
    REQUIRE(lockCount <= config.materializationLeaseStripes);
}

TEST_CASE("ArtifactStore defers automatic maintenance while explicit maintenance reports contention",
          "[Sora][Compute][Artifact][Store][Maintenance][Lease]") {
    TemporaryDirectory temporary;
    auto store = Sora::Compute::ArtifactStore::Create({
        .root = temporary.Path(),
        .maximumPayloadBytes = 4096,
        .reconciliationPublicationInterval = 1,
        .durableWrites = false,
    });
    REQUIRE(store.has_value());
    auto held = Sora::PAL::InterprocessFileLock::TryAcquire(temporary.Path() / ".coordination" / "maintenance.lock");
    REQUIRE(held.has_value());
    REQUIRE(held->has_value());

    auto contended = store->Maintain();
    REQUIRE_FALSE(contended.has_value());
    REQUIRE(contended.error().code == Sora::Compute::ArtifactStoreErrorCode::LeaseContended);
    REQUIRE(store->Store(kExecutableKey, kPayload).has_value());

    held->reset();
    REQUIRE(store->Maintain().has_value());
}

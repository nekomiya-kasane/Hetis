/**
 * @file ArtifactStore.cpp
 * @brief Validated content-addressed artifact persistence implementation.
 * @ingroup Compute
 */

#include <Sora/Compute/ArtifactStore.h>

#include <Sora/Core/PAL/AtomicFile.h>
#include <Sora/Core/PAL/File.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <format>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

namespace Sora::Compute {

    namespace {

        inline constexpr uint32_t kArtifactStoreMagic = 0x54524153U;
        inline constexpr uint16_t kArtifactStoreMajorVersion = 1;
        inline constexpr uint16_t kArtifactStoreMinorVersion = 0;
        inline constexpr uint32_t kArtifactDigestWireSize = 4 * sizeof(uint64_t);
        inline constexpr uint32_t kArtifactStoreFixedHeaderSize = 3 * sizeof(uint32_t) + 3 * sizeof(uint16_t) +
                                                                  2 * sizeof(uint8_t) + sizeof(uint64_t) +
                                                                  2 * kArtifactDigestWireSize;
        static_assert(kArtifactStoreFixedHeaderSize == 92);
        inline constexpr uint32_t kArtifactStoreFlags = 0;
        inline constexpr size_t kMaximumArtifactKeyWords = 32;
        inline constexpr uint32_t kMaximumMaterializationLeaseStripes = uint32_t{1} << 16U;
        inline constexpr std::string_view kCoordinationDirectory = ".coordination";
        inline constexpr std::string_view kMaterializationDirectory = "materialization";
        inline constexpr std::string_view kMaintenanceLockFile = "maintenance.lock";

        struct StoredEntry {
            std::filesystem::path path;
            std::filesystem::file_time_type publicationTime{};
            uint64_t sizeBytes = 0;
            bool protectedEntry = false;
        };

        [[nodiscard]] constexpr uint64_t SaturatingAdd(uint64_t lhs, uint64_t rhs) noexcept {
            return rhs > std::numeric_limits<uint64_t>::max() - lhs ? std::numeric_limits<uint64_t>::max() : lhs + rhs;
        }

        [[nodiscard]] bool Missing(const std::error_code& error) noexcept {
            return error == std::errc::no_such_file_or_directory;
        }

        template<std::unsigned_integral T>
        void AppendLittleEndian(std::vector<std::byte>& bytes, T value) {
            for (size_t byte = 0; byte < sizeof(T); ++byte) {
                bytes.push_back(static_cast<std::byte>(static_cast<uint8_t>(value >> (byte * 8U))));
            }
        }

        void AppendDigest(std::vector<std::byte>& bytes, ArtifactDigest digest) {
            for (const uint64_t word : digest.words) {
                AppendLittleEndian(bytes, word);
            }
        }

        class HeaderReader final {
        public:
            explicit constexpr HeaderReader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

            template<std::unsigned_integral T>
            [[nodiscard]] constexpr bool Read(T& value) noexcept {
                if (sizeof(T) > bytes_.size() - offset_) {
                    return false;
                }
                value = 0;
                for (size_t byte = 0; byte < sizeof(T); ++byte) {
                    value |= static_cast<T>(std::to_integer<uint8_t>(bytes_[offset_++])) << (byte * 8U);
                }
                return true;
            }

            [[nodiscard]] constexpr bool Read(ArtifactDigest& digest) noexcept {
                for (uint64_t& word : digest.words) {
                    if (!Read(word)) {
                        return false;
                    }
                }
                return true;
            }

        private:
            std::span<const std::byte> bytes_;
            size_t offset_ = 0;
        };

        struct ArtifactStoreHeader {
            uint32_t magic = 0;
            uint16_t major = 0;
            uint16_t minor = 0;
            uint32_t headerSize = 0;
            uint32_t flags = 0;
            uint8_t kind = 0;
            uint8_t keyWordCount = 0;
            uint16_t reserved = 0;
            uint64_t payloadSize = 0;
            ArtifactDigest keyDigest{};
            ArtifactDigest payloadDigest{};
        };

        [[nodiscard]] constexpr bool ValidArtifactKindValue(uint8_t value) noexcept {
            return value <= std::to_underlying(ArtifactKind::DevicePipeline);
        }

        [[nodiscard]] ArtifactDigest DigestArtifactKey(const ArtifactKey& key) noexcept {
            std::array<std::byte, 2 + kMaximumArtifactKeyWords * sizeof(uint64_t)> canonical{};
            size_t offset = 0;
            canonical[offset++] = static_cast<std::byte>(std::to_underlying(key.Kind()));
            canonical[offset++] = static_cast<std::byte>(key.Words().size());
            for (uint64_t word : key.Words()) {
                for (size_t byte = 0; byte < sizeof(word); ++byte) {
                    canonical[offset++] = static_cast<std::byte>(static_cast<uint8_t>(word >> (byte * 8U)));
                }
            }
            return DigestArtifactBytes(std::span{canonical}.first(offset));
        }

        [[nodiscard]] constexpr std::string_view ArtifactKindDirectory(ArtifactKind kind) noexcept {
            switch (kind) {
                case ArtifactKind::PortableLowering:
                    return "lowering";
                case ArtifactKind::DeviceExecutable:
                    return "executable";
                case ArtifactKind::DevicePipeline:
                    return "pipeline";
            }
            return "invalid";
        }

        [[nodiscard]] std::array<char, 64> DigestHex(ArtifactDigest digest) noexcept {
            constexpr std::string_view digits = "0123456789abcdef";
            std::array<char, 64> result{};
            size_t output = 0;
            for (uint64_t word : digest.words) {
                for (size_t nibble = 0; nibble < 16; ++nibble) {
                    const size_t shift = (15 - nibble) * 4;
                    result[output++] = digits[static_cast<size_t>((word >> shift) & 0xFU)];
                }
            }
            return result;
        }

        [[nodiscard]] ArtifactStoreError MapPalError(ErrorCode error) noexcept {
            if (error == ErrorCode::ResourceNotFound) {
                return {ArtifactStoreErrorCode::NotFound, std::to_underlying(error)};
            }
            if (error == ErrorCode::OutOfMemory) {
                return {ArtifactStoreErrorCode::OutOfMemory, std::to_underlying(error)};
            }
            return {ArtifactStoreErrorCode::IoError, std::to_underlying(error)};
        }

        [[nodiscard]] std::expected<void, ArtifactStoreError> ValidatePersistentKey(const ArtifactKey& key) noexcept {
            if (!key.Valid() || key.Words().size() > kMaximumArtifactKeyWords) {
                return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::InvalidKey, key.Words().size()});
            }
            if (!key.PersistentCacheable()) {
                return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::NonPersistentIdentity, 0});
            }
            return {};
        }

        [[nodiscard]] std::expected<ArtifactStoreHeader, ArtifactStoreError>
        DecodeHeader(std::span<const std::byte> bytes) noexcept {
            HeaderReader reader{bytes};
            ArtifactStoreHeader header;
            if (!reader.Read(header.magic) || !reader.Read(header.major) || !reader.Read(header.minor) ||
                !reader.Read(header.headerSize) || !reader.Read(header.flags) || !reader.Read(header.kind) ||
                !reader.Read(header.keyWordCount) || !reader.Read(header.reserved) ||
                !reader.Read(header.payloadSize) || !reader.Read(header.keyDigest) ||
                !reader.Read(header.payloadDigest)) {
                return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::Corrupted, 0});
            }
            if (header.magic != kArtifactStoreMagic) {
                return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::Corrupted, 0});
            }
            if (header.major != kArtifactStoreMajorVersion || header.minor > kArtifactStoreMinorVersion) {
                return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::UnsupportedVersion, header.major});
            }
            if (header.flags != kArtifactStoreFlags || header.reserved != 0 || !ValidArtifactKindValue(header.kind) ||
                header.keyWordCount == 0 || header.keyWordCount > kMaximumArtifactKeyWords ||
                header.headerSize != kArtifactStoreFixedHeaderSize + header.keyWordCount * sizeof(uint64_t)) {
                return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::Corrupted, header.headerSize});
            }
            return header;
        }

        void AppendHeader(std::vector<std::byte>& bytes, const ArtifactKey& key, uint64_t payloadSize,
                          ArtifactDigest keyDigest, ArtifactDigest payloadDigest) {
            AppendLittleEndian(bytes, kArtifactStoreMagic);
            AppendLittleEndian(bytes, kArtifactStoreMajorVersion);
            AppendLittleEndian(bytes, kArtifactStoreMinorVersion);
            AppendLittleEndian(bytes, kArtifactStoreFixedHeaderSize +
                                          static_cast<uint32_t>(key.Words().size() * sizeof(uint64_t)));
            AppendLittleEndian(bytes, kArtifactStoreFlags);
            AppendLittleEndian(bytes, std::to_underlying(key.Kind()));
            AppendLittleEndian(bytes, static_cast<uint8_t>(key.Words().size()));
            AppendLittleEndian(bytes, uint16_t{0});
            AppendLittleEndian(bytes, payloadSize);
            AppendDigest(bytes, keyDigest);
            AppendDigest(bytes, payloadDigest);
            for (uint64_t word : key.Words()) {
                AppendLittleEndian(bytes, word);
            }
        }

    } // namespace

    std::expected<ArtifactStore, ArtifactStoreError> ArtifactStore::Create(ArtifactStoreConfig config) noexcept {
        if (config.root.empty() || config.maximumPayloadBytes == 0 ||
            config.maximumPayloadBytes > std::numeric_limits<size_t>::max() || config.maximumStorageBytes == 0 ||
            config.maximumEntryCount == 0 || config.reconciliationPublicationInterval == 0 ||
            config.reconciliationByteInterval == 0 || config.materializationLeaseStripes == 0 ||
            config.materializationLeaseStripes > kMaximumMaterializationLeaseStripes ||
            !std::has_single_bit(config.materializationLeaseStripes) ||
            config.materializationLeaseTimeout.count() <= 0 || config.leasePollInterval.count() <= 0 ||
            config.leasePollInterval > config.materializationLeaseTimeout) {
            return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::InvalidConfiguration, 0});
        }
        try {
            std::error_code error;
            std::filesystem::create_directories(config.root, error);
            if (error || !std::filesystem::is_directory(config.root, error) || error) {
                return std::unexpected(
                    ArtifactStoreError{ArtifactStoreErrorCode::IoError, static_cast<uint64_t>(error.value())});
            }
            std::filesystem::create_directories(config.root / kCoordinationDirectory / kMaterializationDirectory,
                                                error);
            if (error) {
                return std::unexpected(
                    ArtifactStoreError{ArtifactStoreErrorCode::IoError, static_cast<uint64_t>(error.value())});
            }
            ArtifactStore store{std::move(config), std::make_unique<MaintenanceState>()};
            const std::scoped_lock lock{store.maintenance_->mutex};
            auto maintenanceLease = store.TryAcquireMaintenanceLease();
            if (!maintenanceLease) {
                return std::unexpected(maintenanceLease.error());
            }
            if (*maintenanceLease) {
                if (auto maintained = store.MaintainLocked(nullptr); !maintained) {
                    return std::unexpected(maintained.error());
                }
            } else {
                store.maintenance_->publicationsSinceReconciliation = store.config_.reconciliationPublicationInterval;
                store.maintenance_->bytesSinceReconciliation = store.config_.reconciliationByteInterval;
            }
            return store;
        } catch (const std::bad_alloc&) {
            return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::OutOfMemory, 0});
        } catch (...) {
            return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::IoError, 0});
        }
    }

    std::expected<std::filesystem::path, ArtifactStoreError>
    ArtifactStore::EntryPath(const ArtifactKey& key) const noexcept {
        if (auto valid = ValidatePersistentKey(key); !valid) {
            return std::unexpected(valid.error());
        }
        try {
            const auto characters = DigestHex(DigestArtifactKey(key));
            const std::string hex{characters.data(), characters.size()};
            return config_.root / "v1" / ArtifactKindDirectory(key.Kind()) / hex.substr(0, 2) /
                   (hex + ".sora-artifact");
        } catch (const std::bad_alloc&) {
            return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::OutOfMemory, 0});
        } catch (...) {
            return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::IoError, 0});
        }
    }

    std::expected<std::filesystem::path, ArtifactStoreError>
    ArtifactStore::MaterializationLeasePath(const ArtifactKey& key) const noexcept {
        if (auto valid = ValidatePersistentKey(key); !valid) {
            return std::unexpected(valid.error());
        }
        try {
            const ArtifactDigest digest = DigestArtifactKey(key);
            const uint64_t mixed = digest.words[0] ^ std::rotl(digest.words[1], 13) ^ std::rotl(digest.words[2], 29) ^
                                   std::rotl(digest.words[3], 47);
            const uint32_t stripe = static_cast<uint32_t>(mixed) & (config_.materializationLeaseStripes - 1U);
            return config_.root / kCoordinationDirectory / kMaterializationDirectory /
                   std::format("stripe-{:08x}.lock", stripe);
        } catch (const std::bad_alloc&) {
            return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::OutOfMemory, 0});
        } catch (...) {
            return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::IoError, 0});
        }
    }

    std::expected<ArtifactMaterializationLease, ArtifactStoreError>
    ArtifactStore::AcquireMaterializationLease(const ArtifactKey& key) const noexcept {
        auto path = MaterializationLeasePath(key);
        if (!path) {
            return std::unexpected(path.error());
        }
        try {
            const auto start = std::chrono::steady_clock::now();
            const auto timeout =
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(config_.materializationLeaseTimeout);
            const auto pollInterval =
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(config_.leasePollInterval);
            bool observedContention = false;
            for (;;) {
                auto acquired = PAL::InterprocessFileLock::TryAcquire(*path);
                if (!acquired) {
                    return std::unexpected(MapPalError(acquired.error()));
                }
                if (*acquired) {
                    return ArtifactMaterializationLease{std::move(**acquired), observedContention};
                }
                observedContention = true;
                const auto elapsed = std::chrono::steady_clock::now() - start;
                if (elapsed >= timeout) {
                    return std::unexpected(ArtifactStoreError{
                        ArtifactStoreErrorCode::LeaseTimeout,
                        static_cast<uint64_t>(config_.materializationLeaseTimeout.count()),
                    });
                }
                std::this_thread::sleep_for(std::min(pollInterval, timeout - elapsed));
            }
        } catch (const std::bad_alloc&) {
            return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::OutOfMemory, 0});
        } catch (...) {
            return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::IoError, 0});
        }
    }

    std::expected<std::optional<PAL::InterprocessFileLock>, ArtifactStoreError>
    ArtifactStore::TryAcquireMaintenanceLease() const noexcept {
        try {
            auto acquired =
                PAL::InterprocessFileLock::TryAcquire(config_.root / kCoordinationDirectory / kMaintenanceLockFile);
            if (!acquired) {
                return std::unexpected(MapPalError(acquired.error()));
            }
            return std::move(*acquired);
        } catch (const std::bad_alloc&) {
            return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::OutOfMemory, 0});
        } catch (...) {
            return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::IoError, 0});
        }
    }

    std::expected<void, ArtifactStoreError> ArtifactStore::Store(const ArtifactKey& key,
                                                                 std::span<const std::byte> payload) const noexcept {
        if (payload.empty()) {
            return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::InvalidKey, 0});
        }
        if (payload.size() > config_.maximumPayloadBytes) {
            return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::PayloadTooLarge, payload.size()});
        }
        const uint64_t headerSize = kArtifactStoreFixedHeaderSize + key.Words().size() * sizeof(uint64_t);
        if (headerSize > config_.maximumStorageBytes || payload.size() > config_.maximumStorageBytes - headerSize) {
            return std::unexpected(
                ArtifactStoreError{ArtifactStoreErrorCode::CapacityExceeded, headerSize + payload.size()});
        }
        const uint64_t publishedSize = headerSize + payload.size();
        auto path = EntryPath(key);
        if (!path) {
            return std::unexpected(path.error());
        }
        try {
            std::optional<uint64_t> previousSize;
            std::error_code sizeError;
            const uintmax_t nativePreviousSize = std::filesystem::file_size(*path, sizeError);
            if (!sizeError) {
                if (nativePreviousSize > std::numeric_limits<uint64_t>::max()) {
                    return std::unexpected(
                        ArtifactStoreError{ArtifactStoreErrorCode::IoError, std::numeric_limits<uint64_t>::max()});
                }
                previousSize = static_cast<uint64_t>(nativePreviousSize);
            } else if (Missing(sizeError)) {
                sizeError.clear();
            } else {
                return std::unexpected(
                    ArtifactStoreError{ArtifactStoreErrorCode::IoError, static_cast<uint64_t>(sizeError.value())});
            }

            std::error_code directoryError;
            std::filesystem::create_directories(path->parent_path(), directoryError);
            if (directoryError) {
                return std::unexpected(
                    ArtifactStoreError{ArtifactStoreErrorCode::IoError, static_cast<uint64_t>(directoryError.value())});
            }

            std::vector<std::byte> header;
            header.reserve(kArtifactStoreFixedHeaderSize + key.Words().size() * sizeof(uint64_t));
            AppendHeader(header, key, payload.size(), DigestArtifactKey(key), DigestArtifactBytes(payload));
            auto writer = PAL::AtomicFileWriter::Create(*path, {.durable = config_.durableWrites});
            if (!writer) {
                return std::unexpected(MapPalError(writer.error()));
            }
            if (auto written = writer->Output().WriteAllAt(header, 0); !written) {
                return std::unexpected(MapPalError(written.error()));
            }
            if (auto written = writer->Output().WriteAllAt(payload, header.size()); !written) {
                return std::unexpected(MapPalError(written.error()));
            }
            ErrorCode commitError = ErrorCode::IoError;
            constexpr uint32_t maximumPublishAttempts = 8;
            for (uint32_t attempt = 0; attempt < maximumPublishAttempts; ++attempt) {
                if (auto committed = writer->Commit(); committed) {
                    if (auto accounted = AccountPublication(*path, previousSize, publishedSize); !accounted) {
                        return std::unexpected(accounted.error());
                    }
                    return {};
                } else {
                    commitError = committed.error();
                }
                // Another process or thread may have won the same content-addressed publication race.
                if (auto existing = Load(key); existing) {
                    if (auto accounted = AccountPublication(*path, previousSize, publishedSize); !accounted) {
                        return std::unexpected(accounted.error());
                    }
                    return {};
                }
                std::this_thread::yield();
            }
            return std::unexpected(MapPalError(commitError));
        } catch (const std::bad_alloc&) {
            return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::OutOfMemory, 0});
        } catch (...) {
            return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::IoError, 0});
        }
    }

    std::expected<ArtifactStoreMaintenance, ArtifactStoreError> ArtifactStore::Maintain() const noexcept {
        try {
            const std::scoped_lock lock{maintenance_->mutex};
            auto maintenanceLease = TryAcquireMaintenanceLease();
            if (!maintenanceLease) {
                return std::unexpected(maintenanceLease.error());
            }
            if (!*maintenanceLease) {
                return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::LeaseContended, 0});
            }
            return MaintainLocked(nullptr);
        } catch (const std::bad_alloc&) {
            return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::OutOfMemory, 0});
        } catch (...) {
            return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::IoError, 0});
        }
    }

    std::expected<ArtifactStoreMaintenance, ArtifactStoreError>
    ArtifactStore::MaintainLocked(const std::filesystem::path* protectedEntry) const noexcept {
        try {
            ArtifactStoreMaintenance result;
            std::vector<StoredEntry> entries;
            const std::filesystem::path versionRoot = config_.root / "v1";
            std::error_code error;
            if (!std::filesystem::exists(versionRoot, error)) {
                if (error) {
                    return std::unexpected(
                        ArtifactStoreError{ArtifactStoreErrorCode::IoError, static_cast<uint64_t>(error.value())});
                }
                maintenance_->estimatedEntries = 0;
                maintenance_->estimatedBytes = 0;
                maintenance_->publicationsSinceReconciliation = 0;
                maintenance_->bytesSinceReconciliation = 0;
                return result;
            }

            std::filesystem::recursive_directory_iterator iterator{versionRoot, error};
            const std::filesystem::recursive_directory_iterator end;
            if (error) {
                return std::unexpected(
                    ArtifactStoreError{ArtifactStoreErrorCode::IoError, static_cast<uint64_t>(error.value())});
            }
            while (iterator != end) {
                const std::filesystem::directory_entry& entry = *iterator;
                const bool regular = entry.is_regular_file(error);
                if (error) {
                    if (Missing(error)) {
                        error.clear();
                        iterator.increment(error);
                        if (error) {
                            return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::IoError,
                                                                      static_cast<uint64_t>(error.value())});
                        }
                        continue;
                    }
                    return std::unexpected(
                        ArtifactStoreError{ArtifactStoreErrorCode::IoError, static_cast<uint64_t>(error.value())});
                }
                if (regular && entry.path().extension() == ".sora-artifact") {
                    const uintmax_t nativeSize = entry.file_size(error);
                    if (error) {
                        if (Missing(error)) {
                            error.clear();
                            iterator.increment(error);
                            if (error) {
                                return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::IoError,
                                                                          static_cast<uint64_t>(error.value())});
                            }
                            continue;
                        }
                        return std::unexpected(
                            ArtifactStoreError{ArtifactStoreErrorCode::IoError, static_cast<uint64_t>(error.value())});
                    }
                    if (nativeSize > std::numeric_limits<uint64_t>::max()) {
                        return std::unexpected(
                            ArtifactStoreError{ArtifactStoreErrorCode::IoError, std::numeric_limits<uint64_t>::max()});
                    }
                    const auto publicationTime = entry.last_write_time(error);
                    if (error) {
                        if (Missing(error)) {
                            error.clear();
                            iterator.increment(error);
                            if (error) {
                                return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::IoError,
                                                                          static_cast<uint64_t>(error.value())});
                            }
                            continue;
                        }
                        return std::unexpected(
                            ArtifactStoreError{ArtifactStoreErrorCode::IoError, static_cast<uint64_t>(error.value())});
                    }
                    const uint64_t sizeBytes = static_cast<uint64_t>(nativeSize);
                    result.scannedEntries = SaturatingAdd(result.scannedEntries, 1);
                    result.scannedBytes = SaturatingAdd(result.scannedBytes, sizeBytes);
                    entries.push_back(
                        StoredEntry{.path = entry.path(),
                                    .publicationTime = publicationTime,
                                    .sizeBytes = sizeBytes,
                                    .protectedEntry = protectedEntry != nullptr && entry.path() == *protectedEntry});
                }
                iterator.increment(error);
                if (error) {
                    return std::unexpected(
                        ArtifactStoreError{ArtifactStoreErrorCode::IoError, static_cast<uint64_t>(error.value())});
                }
            }

            std::ranges::sort(entries, [](const StoredEntry& lhs, const StoredEntry& rhs) {
                if (lhs.protectedEntry != rhs.protectedEntry) {
                    return !lhs.protectedEntry;
                }
                if (lhs.publicationTime != rhs.publicationTime) {
                    return lhs.publicationTime < rhs.publicationTime;
                }
                return lhs.path < rhs.path;
            });

            uint64_t retainedEntries = result.scannedEntries;
            uint64_t retainedBytes = result.scannedBytes;
            for (const StoredEntry& entry : entries) {
                if (retainedEntries <= config_.maximumEntryCount && retainedBytes <= config_.maximumStorageBytes) {
                    break;
                }
                if (entry.protectedEntry) {
                    return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::CapacityExceeded, retainedBytes});
                }
                const bool removed = std::filesystem::remove(entry.path, error);
                if (error) {
                    return std::unexpected(
                        ArtifactStoreError{ArtifactStoreErrorCode::IoError, static_cast<uint64_t>(error.value())});
                }
                if (!removed) {
                    --retainedEntries;
                    retainedBytes -= entry.sizeBytes;
                    continue;
                }
                --retainedEntries;
                retainedBytes -= entry.sizeBytes;
                result.removedEntries = SaturatingAdd(result.removedEntries, 1);
                result.removedBytes = SaturatingAdd(result.removedBytes, entry.sizeBytes);
            }
            result.retainedEntries = retainedEntries;
            result.retainedBytes = retainedBytes;
            maintenance_->estimatedEntries = retainedEntries;
            maintenance_->estimatedBytes = retainedBytes;
            maintenance_->publicationsSinceReconciliation = 0;
            maintenance_->bytesSinceReconciliation = 0;
            return result;
        } catch (const std::bad_alloc&) {
            return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::OutOfMemory, 0});
        } catch (...) {
            return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::IoError, 0});
        }
    }

    std::expected<void, ArtifactStoreError> ArtifactStore::AccountPublication(const std::filesystem::path& path,
                                                                              std::optional<uint64_t> previousSize,
                                                                              uint64_t publishedSize) const noexcept {
        try {
            const std::scoped_lock lock{maintenance_->mutex};
            if (previousSize) {
                maintenance_->estimatedBytes =
                    *previousSize > maintenance_->estimatedBytes ? 0 : maintenance_->estimatedBytes - *previousSize;
            } else {
                maintenance_->estimatedEntries = SaturatingAdd(maintenance_->estimatedEntries, 1);
            }
            maintenance_->estimatedBytes = SaturatingAdd(maintenance_->estimatedBytes, publishedSize);
            maintenance_->publicationsSinceReconciliation =
                SaturatingAdd(maintenance_->publicationsSinceReconciliation, 1);
            maintenance_->bytesSinceReconciliation =
                SaturatingAdd(maintenance_->bytesSinceReconciliation, publishedSize);

            const bool overBudget = maintenance_->estimatedEntries > config_.maximumEntryCount ||
                                    maintenance_->estimatedBytes > config_.maximumStorageBytes;
            const bool periodicReconciliation =
                maintenance_->publicationsSinceReconciliation >= config_.reconciliationPublicationInterval ||
                maintenance_->bytesSinceReconciliation >= config_.reconciliationByteInterval;
            if (overBudget || periodicReconciliation) {
                auto maintenanceLease = TryAcquireMaintenanceLease();
                if (!maintenanceLease) {
                    return std::unexpected(maintenanceLease.error());
                }
                if (!*maintenanceLease) {
                    return {};
                }
                auto maintained = MaintainLocked(std::addressof(path));
                if (!maintained) {
                    return std::unexpected(maintained.error());
                }
            }
            return {};
        } catch (const std::bad_alloc&) {
            return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::OutOfMemory, 0});
        } catch (...) {
            return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::IoError, 0});
        }
    }

    std::expected<std::vector<std::byte>, ArtifactStoreError>
    ArtifactStore::Load(const ArtifactKey& key) const noexcept {
        auto path = EntryPath(key);
        if (!path) {
            return std::unexpected(path.error());
        }
        try {
            auto opened = PAL::File::Open(*path, {.access = PAL::FileAccess::Read,
                                                  .flags = PAL::FileOpenFlag::Positional | PAL::FileOpenFlag::Random});
            if (!opened) {
                return std::unexpected(MapPalError(opened.error()));
            }
            auto fileSize = opened->Size();
            if (!fileSize) {
                return std::unexpected(MapPalError(fileSize.error()));
            }
            if (*fileSize < kArtifactStoreFixedHeaderSize) {
                return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::Corrupted, *fileSize});
            }

            std::array<std::byte, kArtifactStoreFixedHeaderSize> headerBytes{};
            if (auto read = opened->ReadAllAt(headerBytes, 0); !read) {
                return std::unexpected(MapPalError(read.error()));
            }
            auto decoded = DecodeHeader(headerBytes);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            const ArtifactStoreHeader& header = *decoded;
            if (header.kind != std::to_underlying(key.Kind()) || header.keyWordCount != key.Words().size() ||
                header.keyDigest != DigestArtifactKey(key)) {
                return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::Corrupted, header.kind});
            }
            if (header.payloadSize == 0 || header.payloadSize > config_.maximumPayloadBytes ||
                header.headerSize > std::numeric_limits<uint64_t>::max() - header.payloadSize ||
                *fileSize != header.headerSize + header.payloadSize) {
                return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::Corrupted, header.payloadSize});
            }

            std::array<std::byte, kMaximumArtifactKeyWords * sizeof(uint64_t)> keyBytes{};
            const size_t activeKeyBytes = header.keyWordCount * sizeof(uint64_t);
            if (auto read = opened->ReadAllAt(std::span{keyBytes}.first(activeKeyBytes), kArtifactStoreFixedHeaderSize);
                !read) {
                return std::unexpected(MapPalError(read.error()));
            }
            HeaderReader keyReader{std::span{keyBytes}.first(activeKeyBytes)};
            for (uint64_t expected : key.Words()) {
                uint64_t stored = 0;
                if (!keyReader.Read(stored) || stored != expected) {
                    return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::Corrupted, activeKeyBytes});
                }
            }

            std::vector<std::byte> payload(static_cast<size_t>(header.payloadSize));
            if (auto read = opened->ReadAllAt(payload, header.headerSize); !read) {
                return std::unexpected(MapPalError(read.error()));
            }
            if (DigestArtifactBytes(payload) != header.payloadDigest) {
                return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::Corrupted, header.headerSize});
            }
            return payload;
        } catch (const std::bad_alloc&) {
            return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::OutOfMemory, 0});
        } catch (...) {
            return std::unexpected(ArtifactStoreError{ArtifactStoreErrorCode::IoError, 0});
        }
    }

} // namespace Sora::Compute

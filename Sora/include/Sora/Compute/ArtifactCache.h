/**
 * @file ArtifactCache.h
 * @brief Thread-safe bounded cache for service-scoped live backend artifacts.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Artifact.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Sora::Compute {

    /** @brief In-process identity of one semantic artifact materialized inside one backend service. */
    struct LiveArtifactKey {
        ArtifactKey artifact{};
        BackendServiceId service{};

        /** @brief Return whether both the semantic artifact and runtime service identities are complete. */
        [[nodiscard]] constexpr bool Valid() const noexcept { return artifact.Valid() && service.Valid(); }

        friend constexpr bool operator==(const LiveArtifactKey&, const LiveArtifactKey&) noexcept = default;
    };

    /** @brief Hash functor for service-scoped live artifact lookup. */
    struct LiveArtifactKeyHash {
        [[nodiscard]] constexpr size_t operator()(const LiveArtifactKey& key) const noexcept {
            uint64_t state = ArtifactKeyHash{}(key.artifact);
            const auto mix = [&state](uint64_t value) constexpr noexcept {
                state ^= value + 0x9E3779B97F4A7C15ULL + (state << 6U) + (state >> 2U);
            };
            mix(key.service.low);
            mix(key.service.high);
            return static_cast<size_t>(state);
        }
    };

    /** @brief Immutable exact-key projection of live artifact availability for one planning invocation. */
    class ArtifactAvailabilitySnapshot final {
    public:
        /** @brief Return whether one exact service-scoped artifact was live when this snapshot was captured. */
        [[nodiscard]] bool Contains(const LiveArtifactKey& key) const noexcept;

        /** @brief Classify one exact key without acquiring the cache mutex or retaining an artifact lease. */
        /** @brief Return deterministic exact keys for diagnostics and bulk planning. */
        [[nodiscard]] std::span<const LiveArtifactKey> Keys() const noexcept { return keys_; }

    private:
        friend class ArtifactCache;

        explicit ArtifactAvailabilitySnapshot(std::vector<LiveArtifactKey> keys) noexcept : keys_(std::move(keys)) {}

        std::vector<LiveArtifactKey> keys_{};
    };

    /** @brief Recoverable in-memory artifact-cache failure. */
    enum class ArtifactCacheError : uint8_t {
        InvalidArtifact,
        InvalidService,
        RetiredService,
        OutOfMemory,
    };

    /** @brief Bounded strong-reference cache with deterministic least-recently-used eviction. */
    class ArtifactCache final {
        struct Entry {
            Artifact artifact;
            uint64_t lastUse = 0;
        };

    public:
        /** @brief Construct a cache retaining at most @p capacity live artifacts. */
        explicit ArtifactCache(size_t capacity) noexcept : capacity_(capacity) {}

        ArtifactCache(const ArtifactCache&) = delete;
        ArtifactCache& operator=(const ArtifactCache&) = delete;

        /** @brief Find @p key and update its recency. */
        [[nodiscard]] std::optional<Artifact> Find(const LiveArtifactKey& key) noexcept;

        /** @brief Insert or replace one artifact; zero-capacity caches accept but do not retain it. */
        [[nodiscard]] std::expected<void, ArtifactCacheError> Insert(Artifact artifact) noexcept;

        /** @brief Remove one key and release the cache's ownership. */
        [[nodiscard]] bool Erase(const LiveArtifactKey& key) noexcept;

        /** @brief Permanently reject @p service and remove all of its artifacts outside the cache lock. */
        [[nodiscard]] std::expected<size_t, ArtifactCacheError> InvalidateService(BackendServiceId service) noexcept;

        /** @brief Release every cached artifact. */
        void Clear() noexcept;

        /** @brief Return the number of retained artifacts. */
        [[nodiscard]] size_t Size() const noexcept;

        /** @brief Return the configured maximum number of retained artifacts. */
        [[nodiscard]] size_t Capacity() const noexcept { return capacity_; }

        /** @brief Freeze exact live-key availability without exposing LRU state, artifact leases, or cache authority.
         */
        [[nodiscard]] std::expected<ArtifactAvailabilitySnapshot, ArtifactCacheError>
        SnapshotAvailability() const noexcept;

    private:
        [[nodiscard]] Artifact EvictOneLocked() noexcept;

        using EntryMap = std::unordered_map<LiveArtifactKey, Entry, LiveArtifactKeyHash>;

        mutable std::mutex mutex_;
        EntryMap entries_;
        std::vector<BackendServiceId> retiredServices_;
        size_t capacity_ = 0;
        uint64_t clock_ = 0;
    };

} // namespace Sora::Compute

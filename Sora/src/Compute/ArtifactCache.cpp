/**
 * @file ArtifactCache.cpp
 * @brief Thread-safe bounded cache implementation for live compute artifacts.
 * @ingroup Compute
 */

#include <Sora/Compute/ArtifactCache.h>

#include <algorithm>
#include <new>
#include <ranges>

namespace Sora::Compute {

    namespace {

        struct LiveArtifactKeyLess {
            [[nodiscard]] constexpr bool operator()(const LiveArtifactKey& left,
                                                    const LiveArtifactKey& right) const noexcept {
                if (left.service != right.service) {
                    return left.service < right.service;
                }
                if (left.artifact.Kind() != right.artifact.Kind()) {
                    return left.artifact.Kind() < right.artifact.Kind();
                }
                return std::ranges::lexicographical_compare(left.artifact.Words(), right.artifact.Words());
            }
        };

    } // namespace

    bool ArtifactAvailabilitySnapshot::Contains(const LiveArtifactKey& key) const noexcept {
        if (!key.Valid()) {
            return false;
        }
        const auto found = std::ranges::lower_bound(keys_, key, LiveArtifactKeyLess{});
        return found != keys_.end() && *found == key;
    }

    std::optional<Artifact> ArtifactCache::Find(const LiveArtifactKey& key) noexcept {
        if (!key.Valid()) {
            return std::nullopt;
        }
        std::scoped_lock lock{mutex_};
        const auto found = entries_.find(key);
        if (found == entries_.end()) {
            return std::nullopt;
        }
        found->second.lastUse = ++clock_;
        return found->second.artifact;
    }

    std::expected<void, ArtifactCacheError> ArtifactCache::Insert(Artifact artifact) noexcept {
        Artifact retired;
        if (!artifact) {
            return std::unexpected(ArtifactCacheError::InvalidArtifact);
        }
        try {
            std::scoped_lock lock{mutex_};
            const LiveArtifactKey key{artifact.Key(), artifact.View().service};
            if (std::ranges::binary_search(retiredServices_, key.service)) {
                return std::unexpected(ArtifactCacheError::RetiredService);
            }
            if (capacity_ == 0) {
                return {};
            }
            auto found = entries_.find(key);
            if (found != entries_.end()) {
                retired = std::move(found->second.artifact);
                found->second = Entry{.artifact = std::move(artifact), .lastUse = ++clock_};
                return {};
            }
            entries_.emplace(key, Entry{.artifact = std::move(artifact), .lastUse = ++clock_});
            if (entries_.size() > capacity_) {
                retired = EvictOneLocked();
            }
            return {};
        } catch (const std::bad_alloc&) {
            return std::unexpected(ArtifactCacheError::OutOfMemory);
        }
    }

    bool ArtifactCache::Erase(const LiveArtifactKey& key) noexcept {
        Artifact retired;
        {
            std::scoped_lock lock{mutex_};
            const auto found = entries_.find(key);
            if (found == entries_.end()) {
                return false;
            }
            retired = std::move(found->second.artifact);
            entries_.erase(found);
        }
        return true;
    }

    std::expected<size_t, ArtifactCacheError> ArtifactCache::InvalidateService(BackendServiceId service) noexcept {
        if (!service.Valid()) {
            return std::unexpected(ArtifactCacheError::InvalidService);
        }
        std::vector<Artifact> retired;
        try {
            {
                std::scoped_lock lock{mutex_};
                const size_t count = static_cast<size_t>(std::ranges::count_if(
                    entries_, [service](const auto& entry) { return entry.first.service == service; }));
                retired.reserve(count);
                const auto position = std::ranges::lower_bound(retiredServices_, service);
                if (position == retiredServices_.end() || *position != service) {
                    retiredServices_.insert(position, service);
                }
                for (auto& [key, entry] : entries_) {
                    if (key.service == service) {
                        retired.push_back(std::move(entry.artifact));
                    }
                }
                std::erase_if(entries_, [service](const auto& entry) { return entry.first.service == service; });
            }
            return retired.size();
        } catch (const std::bad_alloc&) {
            return std::unexpected(ArtifactCacheError::OutOfMemory);
        }
    }

    void ArtifactCache::Clear() noexcept {
        EntryMap retired;
        {
            std::scoped_lock lock{mutex_};
            entries_.swap(retired);
        }
    }

    size_t ArtifactCache::Size() const noexcept {
        std::scoped_lock lock{mutex_};
        return entries_.size();
    }

    std::expected<ArtifactAvailabilitySnapshot, ArtifactCacheError>
    ArtifactCache::SnapshotAvailability() const noexcept {
        try {
            std::vector<LiveArtifactKey> keys;
            {
                std::scoped_lock lock{mutex_};
                keys.reserve(entries_.size());
                for (const auto& item : entries_) {
                    keys.push_back(item.first);
                }
            }
            std::ranges::sort(keys, LiveArtifactKeyLess{});
            return ArtifactAvailabilitySnapshot{std::move(keys)};
        } catch (const std::bad_alloc&) {
            return std::unexpected(ArtifactCacheError::OutOfMemory);
        }
    }

    Artifact ArtifactCache::EvictOneLocked() noexcept {
        const auto oldest =
            std::ranges::min_element(entries_, {}, [](const auto& entry) { return entry.second.lastUse; });
        if (oldest == entries_.end()) {
            return {};
        }
        Artifact retired = std::move(oldest->second.artifact);
        entries_.erase(oldest);
        return retired;
    }

} // namespace Sora::Compute

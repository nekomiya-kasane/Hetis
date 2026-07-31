/**
 * @file RuntimeInvalidationRegistry.cpp
 * @brief Linearizable runtime resource registration and backend-service
 * retirement.
 * @ingroup Compute
 */

#include <Sora/Compute/Runtime/RuntimeInvalidationRegistry.h>

#include <algorithm>
#include <functional>
#include <new>

namespace Sora::Compute {

    namespace {

        [[nodiscard]] constexpr RuntimeInvalidationError
        Failure(RuntimeInvalidationErrorCode code, BackendServiceId service = {}, BufferId buffer = {}) noexcept {
            return {.code = code, .service = service, .buffer = buffer};
        }

        [[nodiscard]] constexpr RuntimeInvalidationError MapArtifactFailure(ArtifactCacheError error,
                                                                            BackendServiceId service) noexcept {
            return Failure(error == ArtifactCacheError::OutOfMemory
                               ? RuntimeInvalidationErrorCode::OutOfMemory
                               : RuntimeInvalidationErrorCode::ArtifactCacheFailure,
                           service);
        }

        [[nodiscard]] constexpr RuntimeInvalidationError MapBufferFailure(MemoryError error, BackendServiceId service,
                                                                          BufferId buffer) noexcept {
            return Failure(error == MemoryError::OutOfMemory ? RuntimeInvalidationErrorCode::OutOfMemory
                                                             : RuntimeInvalidationErrorCode::BufferFailure,
                           service, buffer);
        }

    } // namespace

    std::expected<bool, RuntimeInvalidationError> RuntimeInvalidationRegistry::Register(ArtifactCache& cache) noexcept {
        std::vector<BackendServiceId> retiredServices;
        bool inserted = false;
        try {
            std::scoped_lock lock{mutex_};
            retiredServices = retiredServices_;
            const auto position = std::ranges::lower_bound(caches_, &cache, std::less<ArtifactCache*>{});
            if (position == caches_.end() || *position != &cache) {
                caches_.insert(position, &cache);
                inserted = true;
            }
        } catch (const std::bad_alloc&) {
            return std::unexpected(Failure(RuntimeInvalidationErrorCode::OutOfMemory));
        }

        for (BackendServiceId service : retiredServices) {
            if (auto invalidated = cache.InvalidateService(service); !invalidated) {
                return std::unexpected(MapArtifactFailure(invalidated.error(), service));
            }
        }
        return inserted;
    }

    std::expected<bool, RuntimeInvalidationError> RuntimeInvalidationRegistry::Register(const Buffer& buffer) noexcept {
        if (!buffer) {
            return std::unexpected(Failure(RuntimeInvalidationErrorCode::InvalidBuffer));
        }
        const WeakBuffer weak = buffer.Weak();
        std::vector<BackendServiceId> retiredServices;
        bool inserted = false;
        try {
            std::scoped_lock lock{mutex_};
            retiredServices = retiredServices_;
            const auto position = std::ranges::lower_bound(
                buffers_, weak.Identity(), {}, [](const WeakBuffer& candidate) { return candidate.Identity(); });
            if (position != buffers_.end() && position->Identity() == weak.Identity()) {
                if (!position->SharesOwnership(weak)) {
                    return std::unexpected(
                        Failure(RuntimeInvalidationErrorCode::ConflictingBufferIdentity, {}, weak.Identity()));
                }
            } else {
                buffers_.insert(position, weak);
                inserted = true;
            }
        } catch (const std::bad_alloc&) {
            return std::unexpected(Failure(RuntimeInvalidationErrorCode::OutOfMemory, {}, weak.Identity()));
        }

        for (BackendServiceId service : retiredServices) {
            if (auto invalidated = buffer.InvalidateService(service); !invalidated) {
                return std::unexpected(MapBufferFailure(invalidated.error(), service, weak.Identity()));
            }
        }
        return inserted;
    }

    std::expected<RuntimeInvalidationResult, RuntimeInvalidationError>
    RuntimeInvalidationRegistry::InvalidateService(BackendServiceId service) noexcept {
        if (!service.Valid()) {
            return std::unexpected(Failure(RuntimeInvalidationErrorCode::InvalidService));
        }

        RuntimeInvalidationResult result{};
        std::vector<ArtifactCache*> caches;
        std::vector<Buffer> buffers;
        try {
            std::scoped_lock lock{mutex_};
            caches = caches_;
            buffers.reserve(buffers_.size());
            for (const WeakBuffer& weak : buffers_) {
                if (Buffer buffer = weak.Lock()) {
                    buffers.push_back(std::move(buffer));
                } else {
                    ++result.expiredBuffersPruned;
                }
            }
            std::erase_if(buffers_, [](const WeakBuffer& weak) { return weak.Expired(); });

            const auto position = std::ranges::lower_bound(retiredServices_, service);
            if (position == retiredServices_.end() || *position != service) {
                retiredServices_.insert(position, service);
                result.newlyRetired = true;
            }
        } catch (const std::bad_alloc&) {
            return std::unexpected(Failure(RuntimeInvalidationErrorCode::OutOfMemory, service));
        }

        result.cachesVisited = caches.size();
        result.buffersVisited = buffers.size();
        for (ArtifactCache* cache : caches) {
            auto invalidated = cache->InvalidateService(service);
            if (!invalidated) {
                return std::unexpected(MapArtifactFailure(invalidated.error(), service));
            }
            result.artifactsRetired += *invalidated;
        }
        for (const Buffer& buffer : buffers) {
            auto invalidated = buffer.InvalidateService(service);
            if (!invalidated) {
                return std::unexpected(MapBufferFailure(invalidated.error(), service, buffer.Describe().id));
            }
            result.replicasRetired += *invalidated;
        }
        return result;
    }

    bool RuntimeInvalidationRegistry::IsServiceRetired(BackendServiceId service) const noexcept {
        std::scoped_lock lock{mutex_};
        return std::ranges::binary_search(retiredServices_, service);
    }

    size_t RuntimeInvalidationRegistry::CacheCount() const noexcept {
        std::scoped_lock lock{mutex_};
        return caches_.size();
    }

    size_t RuntimeInvalidationRegistry::BufferCount() const noexcept {
        std::scoped_lock lock{mutex_};
        return buffers_.size();
    }

} // namespace Sora::Compute

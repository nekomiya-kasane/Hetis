/**
 * @file RuntimeInvalidationRegistry.h
 * @brief Linearizable service retirement across runtime artifact caches and
 * logical buffers.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/ArtifactCache.h>
#include <Sora/Compute/Memory/Buffer.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <vector>

namespace Sora::Compute {

    /** @brief Stable failure while registering resources or retiring one backend
     * service. */
    enum class RuntimeInvalidationErrorCode : uint8_t {
        InvalidService,
        InvalidBuffer,
        ConflictingBufferIdentity,
        ArtifactCacheFailure,
        BufferFailure,
        OutOfMemory,
    };

    /** @brief Invalidation failure with the affected service and logical-buffer
     * identity. */
    struct RuntimeInvalidationError {
        RuntimeInvalidationErrorCode code = RuntimeInvalidationErrorCode::InvalidService;
        BackendServiceId service{};
        BufferId buffer{};

        friend constexpr bool operator==(RuntimeInvalidationError, RuntimeInvalidationError) noexcept = default;
    };

    /** @brief Diagnostic counts from one complete service-retirement pass. */
    struct RuntimeInvalidationResult {
        bool newlyRetired = false;
        size_t cachesVisited = 0;
        size_t artifactsRetired = 0;
        size_t buffersVisited = 0;
        size_t replicasRetired = 0;
        size_t expiredBuffersPruned = 0;

        friend constexpr bool operator==(RuntimeInvalidationResult, RuntimeInvalidationResult) noexcept = default;
    };

    /**
     * @brief Process-local retirement registry spanning all live runtime caches and
     * logical buffers.
     * @details Cache references are non-owning and must outlive the registry.
     * Buffers are observed weakly. Register and InvalidateService are linearizable,
     * while backend destruction always occurs after releasing the registry mutex.
     */
    class RuntimeInvalidationRegistry final {
    public:
        RuntimeInvalidationRegistry() = default;
        RuntimeInvalidationRegistry(const RuntimeInvalidationRegistry&) = delete;
        RuntimeInvalidationRegistry& operator=(const RuntimeInvalidationRegistry&) = delete;

        /** @brief Register one stable artifact cache and apply every service
         * retirement already published. */
        [[nodiscard]] std::expected<bool, RuntimeInvalidationError> Register(ArtifactCache& cache) noexcept;

        /** @brief Register one logical buffer without extending its lifetime. */
        [[nodiscard]] std::expected<bool, RuntimeInvalidationError> Register(const Buffer& buffer) noexcept;

        /** @brief Publish permanent service retirement and repeat a complete
         * resource-cleanup pass. */
        [[nodiscard]] std::expected<RuntimeInvalidationResult, RuntimeInvalidationError>
        InvalidateService(BackendServiceId service) noexcept;

        /** @brief Return whether @p service has been permanently retired. */
        [[nodiscard]] bool IsServiceRetired(BackendServiceId service) const noexcept;

        /** @brief Return the number of stable cache references. */
        [[nodiscard]] size_t CacheCount() const noexcept;

        /** @brief Return the number of retained weak buffer identities, including
         * not-yet-pruned expired entries. */
        [[nodiscard]] size_t BufferCount() const noexcept;

    private:
        mutable std::mutex mutex_;
        std::vector<ArtifactCache*> caches_;
        std::vector<WeakBuffer> buffers_;
        std::vector<BackendServiceId> retiredServices_;
    };

} // namespace Sora::Compute

/**
 * @file Transfer.h
 * @brief Backend-independent transfer-engine identities and directed memory topology links.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Memory/Memory.h>

#include <cstdint>
#include <limits>
#include <type_traits>

namespace Sora::Compute {

    /** @brief Stable identity of a runtime transfer materializer registered by a backend or bridge. */
    struct TransferEngineId {
        uint64_t low = 0;
        uint64_t high = 0;

        /** @brief Return whether this identity denotes a transfer engine. */
        [[nodiscard]] constexpr bool Valid() const noexcept { return low != 0 || high != 0; }

        friend constexpr bool operator==(TransferEngineId, TransferEngineId) noexcept = default;
        friend constexpr auto operator<=>(TransferEngineId, TransferEngineId) noexcept = default;
    };

    /** @brief Semantic mechanism used to materialize one topology edge. */
    enum class TransferKind : uint8_t {
        Copy,
        CoherentAlias,
        HostBridge,
        Remote,
    };

    /** @brief Return whether @p kind is a supported transfer mechanism. */
    [[nodiscard]] constexpr bool ValidTransferKind(TransferKind kind) noexcept {
        return kind == TransferKind::Copy || kind == TransferKind::CoherentAlias || kind == TransferKind::HostBridge ||
               kind == TransferKind::Remote;
    }

    /** @brief Directed runtime topology edge with a measured transfer cost model. */
    struct TransferLink {
        BackendServiceId sourceService{};
        BackendServiceId service{};
        TransferEngineId engine{};
        MemoryLocation source{};
        MemoryLocation destination{};
        TransferKind kind = TransferKind::Copy;
        uint64_t bytesPerSecond = 0;
        uint64_t latencyNanoseconds = 0;
        uint64_t maximumTransferBytes = std::numeric_limits<uint64_t>::max();

        /** @brief Return whether this link has internally consistent topology and cost semantics. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            const bool validRate = kind == TransferKind::CoherentAlias || bytesPerSecond != 0;
            const bool validAlias = kind != TransferKind::CoherentAlias || source.domain == destination.domain;
            const bool serviceTransition = sourceService != service || source != destination;
            return sourceService.Valid() && service.Valid() && engine.Valid() && source.Valid() &&
                   destination.Valid() && serviceTransition && ValidTransferKind(kind) && validRate && validAlias &&
                   maximumTransferBytes != 0;
        }

        friend constexpr bool operator==(TransferLink, TransferLink) noexcept = default;
    };

    static_assert(std::is_trivially_copyable_v<TransferEngineId>);
    static_assert(std::is_trivially_copyable_v<TransferLink>);

} // namespace Sora::Compute

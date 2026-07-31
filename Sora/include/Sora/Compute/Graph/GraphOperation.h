/**
 * @file GraphOperation.h
 * @brief Owning logical payloads for runtime execution-graph nodes.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Invocation/KernelInvocation.h>
#include <Sora/Compute/Memory/BufferSlice.h>
#include <Sora/Compute/Memory/BufferView.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <concepts>
#include <cstdint>
#include <expected>
#include <limits>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace Sora::Compute {

    inline constexpr size_t kMaximumFillPatternSize = 16;

    /** @brief Stable reason a logical graph operation could not be formed. */
    enum class GraphOperationError : uint8_t {
        InvalidBuffer,
        SizeMismatch,
        OverlappingRegions,
        InvalidPattern,
        InvalidOperation,
        InvalidResource,
        CapacityExceeded,
        OutOfMemory,
    };

    /** @brief Canonical fixed-capacity byte pattern for one fill operation. */
    struct FillPattern {
        std::array<std::byte, kMaximumFillPatternSize> storage{};
        uint8_t size = 0;

        /** @brief Validate and copy a power-of-two pattern of at most sixteen bytes. */
        [[nodiscard]] static constexpr std::expected<FillPattern, GraphOperationError>
        Create(std::span<const std::byte> bytes) noexcept {
            if (bytes.empty() || bytes.size() > kMaximumFillPatternSize || !std::has_single_bit(bytes.size())) {
                return std::unexpected(GraphOperationError::InvalidPattern);
            }
            FillPattern pattern{.size = static_cast<uint8_t>(bytes.size())};
            for (size_t index = 0; index < bytes.size(); ++index) {
                pattern.storage[index] = bytes[index];
            }
            return pattern;
        }

        /** @brief Return active canonical pattern bytes. */
        [[nodiscard]] constexpr std::span<const std::byte> Bytes() const noexcept { return {storage.data(), size}; }

        friend constexpr bool operator==(FillPattern, FillPattern) noexcept = default;
    };

    /** @brief Owning logical byte copy between two non-overlapping slices. */
    class CopyOperation {
    public:
        /** @brief Validate equal sizes and retain both logical buffer leases. */
        [[nodiscard]] static std::expected<CopyOperation, GraphOperationError>
        Create(BufferSlice source, BufferSlice destination) noexcept {
            if (!source || !destination) {
                return std::unexpected(GraphOperationError::InvalidBuffer);
            }
            if (source.Region().Size() != destination.Region().Size()) {
                return std::unexpected(GraphOperationError::SizeMismatch);
            }
            const BufferId sourceId = source.BufferDescriptor().id;
            const BufferId destinationId = destination.BufferDescriptor().id;
            if (sourceId == destinationId && source.Region().Intersects(destination.Region())) {
                return std::unexpected(GraphOperationError::OverlappingRegions);
            }
            return CopyOperation{std::move(source), std::move(destination), sourceId != destinationId};
        }

        /** @brief Return source and destination effects in canonical order. */
        [[nodiscard]] std::span<const ResourceAccess> ResourceAccesses() const noexcept { return accesses_; }

        /** @brief Return the retained source slice. */
        [[nodiscard]] const BufferSlice& Source() const noexcept { return source_; }

        /** @brief Return the retained destination slice. */
        [[nodiscard]] const BufferSlice& Destination() const noexcept { return destination_; }

        /** @brief Return the number of bytes copied. */
        [[nodiscard]] uint64_t SizeBytes() const noexcept { return source_.Region().Size(); }

        /** @brief Return whether selected physical allocations must be checked for overlap. */
        [[nodiscard]] bool RequiresPhysicalOverlapCheck() const noexcept { return physicalOverlapCheck_; }

    private:
        CopyOperation(BufferSlice source, BufferSlice destination, bool physicalOverlapCheck) noexcept
            : source_(std::move(source)),
              destination_(std::move(destination)),
              accesses_{{{source_.BufferDescriptor().id, source_.Region(), AccessMode::Read},
                         {destination_.BufferDescriptor().id, destination_.Region(), AccessMode::Write}}},
              physicalOverlapCheck_(physicalOverlapCheck) {}

        BufferSlice source_{};
        BufferSlice destination_{};
        std::array<ResourceAccess, 2> accesses_{};
        bool physicalOverlapCheck_ = false;
    };

    /** @brief Owning logical fill of one contiguous slice with a canonical byte pattern. */
    class FillOperation {
    public:
        /** @brief Validate pattern alignment and retain the destination buffer lease. */
        [[nodiscard]] static std::expected<FillOperation, GraphOperationError> Create(BufferSlice destination,
                                                                                      FillPattern pattern) noexcept {
            if (!destination) {
                return std::unexpected(GraphOperationError::InvalidBuffer);
            }
            if (pattern.size == 0 || destination.Region().Offset() % pattern.size != 0 ||
                destination.Region().Size() % pattern.size != 0) {
                return std::unexpected(GraphOperationError::InvalidPattern);
            }
            return FillOperation{std::move(destination), pattern};
        }

        /** @brief Return the destination write effect. */
        [[nodiscard]] std::span<const ResourceAccess> ResourceAccesses() const noexcept { return accesses_; }

        /** @brief Return the retained destination slice. */
        [[nodiscard]] const BufferSlice& Destination() const noexcept { return destination_; }

        /** @brief Return the canonical repeated fill pattern. */
        [[nodiscard]] FillPattern Pattern() const noexcept { return pattern_; }

        /** @brief Return the number of bytes filled. */
        [[nodiscard]] uint64_t SizeBytes() const noexcept { return destination_.Region().Size(); }

    private:
        FillOperation(BufferSlice destination, FillPattern pattern) noexcept
            : destination_(std::move(destination)),
              accesses_{{{destination_.BufferDescriptor().id, destination_.Region(), AccessMode::Write}}},
              pattern_(pattern) {}

        BufferSlice destination_{};
        std::array<ResourceAccess, 1> accesses_{};
        FillPattern pattern_{};
    };

    /** @brief Portability boundary of a registered external graph operation. */
    enum class ExternalPortability : uint8_t {
        ProcessLocal,
        BackendPortable,
        RemotePortable,
    };

    /** @brief One affine buffer binding and declared effect of an external operation. */
    struct ExternalResourceBinding {
        BufferView view{};
        AccessMode access = AccessMode::Read;
    };

    /** @brief Owning payload for an explicitly registered non-canonical operation. */
    class ExternalOperation {
    public:
        /** @brief Validate identity and resource bindings, then derive logical effects. */
        [[nodiscard]] static std::expected<ExternalOperation, GraphOperationError>
        Create(SemanticHash operation, ExternalPortability portability,
               std::vector<ExternalResourceBinding> resources) noexcept {
            const bool validPortability = portability == ExternalPortability::ProcessLocal ||
                                          portability == ExternalPortability::BackendPortable ||
                                          portability == ExternalPortability::RemotePortable;
            if ((operation.low == 0 && operation.high == 0) || !validPortability) {
                return std::unexpected(GraphOperationError::InvalidOperation);
            }
            if (resources.size() > std::numeric_limits<uint32_t>::max()) {
                return std::unexpected(GraphOperationError::CapacityExceeded);
            }
            try {
                std::vector<ResourceAccess> accesses;
                std::vector<AffineByteFootprint> byteFootprints;
                accesses.reserve(resources.size());
                byteFootprints.reserve(resources.size());
                for (const ExternalResourceBinding& resource : resources) {
                    if (!resource.view || !ValidAccessMode(resource.access)) {
                        return std::unexpected(GraphOperationError::InvalidResource);
                    }
                    const AffineByteFootprint footprint = resource.view.Footprint();
                    ByteFootprintId footprintId{};
                    if (!footprint.IsContiguous()) {
                        const auto existing = std::ranges::find(byteFootprints, footprint);
                        if (existing == byteFootprints.end()) {
                            footprintId.index = static_cast<uint32_t>(byteFootprints.size());
                            byteFootprints.push_back(footprint);
                        } else {
                            footprintId.index = static_cast<uint32_t>(existing - byteFootprints.begin());
                        }
                    }
                    accesses.push_back(
                        {resource.view.BufferDescriptor().id, resource.view.Region(), resource.access, footprintId});
                }
                return ExternalOperation{operation, portability, std::move(resources), std::move(accesses),
                                         std::move(byteFootprints)};
            } catch (const std::bad_alloc&) {
                return std::unexpected(GraphOperationError::OutOfMemory);
            }
        }

        /** @brief Return the registered stable operation identity. */
        [[nodiscard]] SemanticHash Operation() const noexcept { return operation_; }

        /** @brief Return whether this operation may cross backend or process boundaries. */
        [[nodiscard]] ExternalPortability Portability() const noexcept { return portability_; }

        /** @brief Return owning external resource bindings. */
        [[nodiscard]] std::span<const ExternalResourceBinding> Resources() const noexcept { return resources_; }

        /** @brief Return logical effects derived from the resource bindings. */
        [[nodiscard]] std::span<const ResourceAccess> ResourceAccesses() const noexcept { return accesses_; }

        /** @brief Return operation-local non-contiguous footprints referenced by ResourceAccesses(). */
        [[nodiscard]] std::span<const AffineByteFootprint> ByteFootprints() const noexcept { return byteFootprints_; }

    private:
        ExternalOperation(SemanticHash operation, ExternalPortability portability,
                          std::vector<ExternalResourceBinding> resources, std::vector<ResourceAccess> accesses,
                          std::vector<AffineByteFootprint> byteFootprints) noexcept
            : operation_(operation),
              portability_(portability),
              resources_(std::move(resources)),
              accesses_(std::move(accesses)),
              byteFootprints_(std::move(byteFootprints)) {}

        SemanticHash operation_{};
        ExternalPortability portability_ = ExternalPortability::ProcessLocal;
        std::vector<ExternalResourceBinding> resources_{};
        std::vector<ResourceAccess> accesses_{};
        std::vector<AffineByteFootprint> byteFootprints_{};
    };

    /** @brief Owning process-local payload of one runtime graph node. */
    using GraphOperation = std::variant<KernelInvocation, CopyOperation, FillOperation, ExternalOperation>;

    /** @brief Derive structural node metadata from an owning logical operation. */
    [[nodiscard]] inline GraphNodeSpec DescribeGraphOperation(const GraphOperation& operation) noexcept {
        return std::visit(
            []<typename Operation>(const Operation& value) -> GraphNodeSpec {
                if constexpr (std::same_as<Operation, KernelInvocation>) {
                    return {GraphNodeKind::Dispatch, value.ProgramHash(), value.Domain().workItemCount};
                } else if constexpr (std::same_as<Operation, CopyOperation>) {
                    return {GraphNodeKind::Copy, {}, value.SizeBytes()};
                } else if constexpr (std::same_as<Operation, FillOperation>) {
                    return {GraphNodeKind::Fill, {}, value.SizeBytes()};
                } else {
                    return {GraphNodeKind::External, value.Operation(), 0};
                }
            },
            operation);
    }

    /** @brief Return effects owned and derived by @p operation. */
    [[nodiscard]] inline std::span<const ResourceAccess>
    GraphOperationAccesses(const GraphOperation& operation) noexcept {
        return std::visit([](const auto& value) { return value.ResourceAccesses(); }, operation);
    }

    /** @brief Return operation-local affine footprints referenced by GraphOperationAccesses(). */
    [[nodiscard]] inline std::span<const AffineByteFootprint>
    GraphOperationFootprints(const GraphOperation& operation) noexcept {
        return std::visit(
            []<typename Operation>(const Operation& value) -> std::span<const AffineByteFootprint> {
                if constexpr (std::same_as<Operation, KernelInvocation> || std::same_as<Operation, ExternalOperation>) {
                    return value.ByteFootprints();
                } else {
                    return {};
                }
            },
            operation);
    }

    static_assert(std::is_trivially_copyable_v<FillPattern>);
    static_assert(std::is_nothrow_move_constructible_v<GraphOperation>);

} // namespace Sora::Compute

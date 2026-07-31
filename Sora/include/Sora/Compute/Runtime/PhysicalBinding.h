/**
 * @file PhysicalBinding.h
 * @brief Physical replica selection and complete backend binding materialization for kernel invocations.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Backend/Queue.h>
#include <Sora/Compute/Invocation/DispatchPartition.h>
#include <Sora/Compute/Invocation/KernelInvocation.h>
#include <Sora/Compute/Memory/ResidencyTable.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <new>
#include <numeric>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace Sora::Compute {

    /** @brief Writable physical region not published as a ready logical replica until graph completion. */
    class PendingReplica {
    public:
        PendingReplica() noexcept = default;

        /** @brief Validate a logical-region mapping into one retained physical allocation. */
        [[nodiscard]] static std::expected<PendingReplica, MemoryError>
        Create(Allocation allocation, ByteRegion bufferRegion, uint64_t allocationOffset = 0) noexcept {
            if (!allocation || bufferRegion.Size() == 0 ||
                !allocation.View().Covers(allocationOffset, bufferRegion.Size())) {
                return std::unexpected(MemoryError::InvalidAllocation);
            }
            return PendingReplica{std::move(allocation), bufferRegion, allocationOffset};
        }

        /** @brief Return whether this pending mapping retains writable storage. */
        [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(allocation_); }

        /** @brief Return the retained physical allocation. */
        [[nodiscard]] const Allocation& Storage() const noexcept { return allocation_; }

        /** @brief Return the logical byte interval mapped by this target. */
        [[nodiscard]] ByteRegion BufferRegion() const noexcept { return bufferRegion_; }

        /** @brief Resolve a contained logical interval to its physical allocation offset. */
        [[nodiscard]] constexpr std::optional<uint64_t> ResolveAllocationOffset(ByteRegion requested) const noexcept {
            if (!bufferRegion_.Contains(requested)) {
                return std::nullopt;
            }
            return allocationOffset_ + requested.Offset() - bufferRegion_.Offset();
        }

    private:
        PendingReplica(Allocation allocation, ByteRegion bufferRegion, uint64_t allocationOffset) noexcept
            : allocation_(std::move(allocation)), bufferRegion_(bufferRegion), allocationOffset_(allocationOffset) {}

        Allocation allocation_{};
        ByteRegion bufferRegion_{};
        uint64_t allocationOffset_ = 0;
    };

    /** @brief Physical storage selected for one buffer argument in signature order. */
    using PhysicalBufferSource = std::variant<ResidentReplica, PendingReplica>;

    /** @brief Stable physical-binding failure before backend submission. */
    enum class PhysicalBindingErrorCode : uint8_t {
        InvalidInvocation,
        SourceCountMismatch,
        SourceKindMismatch,
        RegionNotResident,
        UnrepresentableSlice,
        InsufficientAlignment,
        AliasContractViolation,
        CapacityExceeded,
        OutOfMemory,
    };

    /** @brief Physical-binding failure with primary and optional related argument indices. */
    struct PhysicalBindingError {
        PhysicalBindingErrorCode code = PhysicalBindingErrorCode::InvalidInvocation;
        uint32_t argument = kInvalidKernelArgumentIndex;
        uint32_t related = kInvalidKernelArgumentIndex;

        friend constexpr bool operator==(PhysicalBindingError, PhysicalBindingError) noexcept = default;
    };

    /** @brief Complete immutable-by-convention payload consumed by one physical dispatch command. */
    struct PhysicalKernelBindings {
        std::vector<BackendBinding> buffers{};
        std::vector<std::byte> scalarData{};
        std::vector<BackendScalarBinding> scalars{};
    };

    namespace Detail {

        struct ResolvedPhysicalSource {
            Allocation allocation{};
            uint64_t offset = 0;
        };

        [[nodiscard]] inline std::expected<ResolvedPhysicalSource, PhysicalBindingError>
        ResolvePhysicalSource(const PhysicalBufferSource& source, AccessMode access, ByteCoverageView coverage,
                              uint32_t argument) noexcept {
            if (const auto* pending = std::get_if<PendingReplica>(&source)) {
                if (!*pending) {
                    return std::unexpected(
                        PhysicalBindingError{PhysicalBindingErrorCode::SourceKindMismatch, argument});
                }
                const auto offset = pending->ResolveAllocationOffset(coverage.BoundingRegion());
                if (!offset) {
                    return std::unexpected(PhysicalBindingError{PhysicalBindingErrorCode::RegionNotResident, argument});
                }
                return ResolvedPhysicalSource{pending->Storage(), *offset};
            }
            if (access != AccessMode::Read) {
                return std::unexpected(PhysicalBindingError{PhysicalBindingErrorCode::SourceKindMismatch, argument});
            }
            const auto* resident = std::get_if<ResidentReplica>(&source);
            if (resident == nullptr || resident->version == 0 || !resident->Covers(resident->version, coverage)) {
                return std::unexpected(PhysicalBindingError{PhysicalBindingErrorCode::RegionNotResident, argument});
            }
            const auto offset = resident->ResolveAllocationOffset(coverage);
            if (!offset) {
                return std::unexpected(PhysicalBindingError{PhysicalBindingErrorCode::RegionNotResident, argument});
            }
            return ResolvedPhysicalSource{resident->allocation, *offset};
        }

        [[nodiscard]] inline const BackendBinding* FindPhysicalBinding(std::span<const BackendBinding> bindings,
                                                                       KernelArgumentId argument) noexcept {
            for (const BackendBinding& binding : bindings) {
                if (binding.binding == argument.index) {
                    return &binding;
                }
            }
            return nullptr;
        }

        [[nodiscard]] inline std::expected<void, PhysicalBindingError>
        ValidatePhysicalAliases(const KernelInvocation& invocation, std::span<const BackendBinding> bindings) noexcept {
            for (const PhysicalAliasRequirement requirement : invocation.PhysicalAliasRequirements()) {
                const BackendBinding* first = FindPhysicalBinding(bindings, requirement.first);
                const BackendBinding* second = FindPhysicalBinding(bindings, requirement.second);
                if (first == nullptr || second == nullptr) {
                    return std::unexpected(PhysicalBindingError{PhysicalBindingErrorCode::InvalidInvocation,
                                                                requirement.first.index, requirement.second.index});
                }
                const auto firstRange = first->allocation.PhysicalRange(first->offset, first->size);
                const auto secondRange = second->allocation.PhysicalRange(second->offset, second->size);
                if (!firstRange || !secondRange) {
                    return std::unexpected(PhysicalBindingError{PhysicalBindingErrorCode::RegionNotResident,
                                                                requirement.first.index, requirement.second.index});
                }
                if (firstRange->Intersects(*secondRange)) {
                    return std::unexpected(PhysicalBindingError{PhysicalBindingErrorCode::AliasContractViolation,
                                                                requirement.first.index, requirement.second.index});
                }
            }
            return {};
        }

    } // namespace Detail

    /**
     * @brief Materialize scalar and affine buffer bindings from explicitly selected physical storage.
     * @details @p bufferSources is ordered by buffer arguments only, preserving their relative signature order.
     * Published ResidentReplica values are read-only here; graph-local PendingReplica values support read, write, and
     * read-write access after plan-level producer validation. Deferred noalias obligations are proven physically.
     */
    [[nodiscard]] inline std::expected<PhysicalKernelBindings, PhysicalBindingError>
    MaterializeKernelBindings(const KernelInvocation& invocation, std::span<const PhysicalBufferSource> bufferSources,
                              const DispatchSlice& slice) noexcept {
        const auto signature = invocation.Signature();
        const auto arguments = invocation.Bindings();
        if (signature.size() != arguments.size() || signature.empty() || !slice.ValidWithin(invocation.Domain())) {
            return std::unexpected(PhysicalBindingError{PhysicalBindingErrorCode::InvalidInvocation});
        }
        size_t expectedBufferCount = 0;
        for (const KernelArgumentDesc& argument : signature) {
            expectedBufferCount += argument.kind == ParameterKind::Buffer;
        }
        if (bufferSources.size() != expectedBufferCount) {
            return std::unexpected(PhysicalBindingError{PhysicalBindingErrorCode::SourceCountMismatch});
        }

        try {
            PhysicalKernelBindings result;
            result.buffers.reserve(expectedBufferCount);
            result.scalars.reserve(signature.size() - expectedBufferCount);
            size_t bufferIndex = 0;
            for (uint32_t argumentIndex = 0; argumentIndex < signature.size(); ++argumentIndex) {
                const KernelArgumentDesc& expected = signature[argumentIndex];
                if (expected.kind == ParameterKind::Value) {
                    const auto* scalar = std::get_if<ScalarArgument>(&arguments[argumentIndex]);
                    if (scalar == nullptr || scalar->Bytes().size() > UINT32_MAX ||
                        result.scalarData.size() > std::numeric_limits<uint64_t>::max() - scalar->Bytes().size()) {
                        return std::unexpected(
                            PhysicalBindingError{PhysicalBindingErrorCode::InvalidInvocation, argumentIndex});
                    }
                    const uint64_t offset = result.scalarData.size();
                    result.scalarData.insert(result.scalarData.end(), scalar->Bytes().begin(), scalar->Bytes().end());
                    result.scalars.push_back({.dataOffset = offset,
                                              .size = static_cast<uint32_t>(scalar->Bytes().size()),
                                              .binding = argumentIndex,
                                              .type = scalar->Type()});
                    continue;
                }

                const auto* view = std::get_if<BufferView>(&arguments[argumentIndex]);
                if (view == nullptr || !*view || bufferIndex >= bufferSources.size()) {
                    return std::unexpected(
                        PhysicalBindingError{PhysicalBindingErrorCode::InvalidInvocation, argumentIndex});
                }
                const std::span<const uint64_t> origin{slice.origin.data(), slice.rank};
                const std::span<const uint64_t> extents{slice.extents.data(), slice.rank};
                auto shardView = view->SubView(origin, extents);
                if (!shardView || !LayoutSatisfies(shardView->Describe().layout, expected.layout)) {
                    return std::unexpected(
                        PhysicalBindingError{PhysicalBindingErrorCode::UnrepresentableSlice, argumentIndex});
                }
                const AffineByteFootprint footprint = shardView->Footprint();
                auto source = Detail::ResolvePhysicalSource(bufferSources[bufferIndex++], expected.access,
                                                            ByteCoverageView::Affine(footprint), argumentIndex);
                if (!source) {
                    return std::unexpected(source.error());
                }
                uint64_t effectiveAlignment = source->allocation.Alignment();
                if (source->offset != 0) {
                    effectiveAlignment = std::gcd(effectiveAlignment, source->offset);
                }
                if (effectiveAlignment < expected.minimumAlignment) {
                    return std::unexpected(
                        PhysicalBindingError{PhysicalBindingErrorCode::InsufficientAlignment, argumentIndex});
                }

                const BufferViewDesc& affine = shardView->Describe();
                BackendBinding binding{.allocation = source->allocation.View(),
                                       .offset = source->offset,
                                       .size = shardView->Region().Size(),
                                       .binding = argumentIndex,
                                       .access = expected.access,
                                       .kind = BackendBindingKind::AffineElements,
                                       .elementType = expected.elementType,
                                       .layout = affine.layout,
                                       .rank = affine.rank,
                                       .componentStrideBytes = affine.componentStrideBytes};
                for (uint8_t axis = 0; axis < kMaximumBufferRank; ++axis) {
                    binding.extents[axis] = affine.extents[axis];
                    binding.byteStrides[axis] = affine.byteStrides[axis];
                }
                result.buffers.push_back(binding);
            }
            if (auto aliases = Detail::ValidatePhysicalAliases(invocation, result.buffers); !aliases) {
                return std::unexpected(aliases.error());
            }
            return result;
        } catch (const std::bad_alloc&) {
            return std::unexpected(PhysicalBindingError{PhysicalBindingErrorCode::OutOfMemory});
        }
    }

    /** @brief Materialize the unique whole-domain binding set. */
    [[nodiscard]] inline std::expected<PhysicalKernelBindings, PhysicalBindingError>
    MaterializeKernelBindings(const KernelInvocation& invocation,
                              std::span<const PhysicalBufferSource> bufferSources) noexcept {
        return MaterializeKernelBindings(invocation, bufferSources, DispatchSlice::Whole(invocation.Domain()));
    }

} // namespace Sora::Compute

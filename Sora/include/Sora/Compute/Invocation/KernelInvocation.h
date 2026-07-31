/**
 * @file KernelInvocation.h
 * @brief Validated runtime bindings for one canonical kernel program.
 * @ingroup Compute
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <Sora/Compute/Graph/GraphModel.h>
#include <Sora/Compute/KernelProgramValidation.h>
#include <Sora/Compute/Memory/BufferView.h>

namespace Sora::Compute {

    namespace Detail {
        struct KernelInvocationBinding;
    }

    /** @brief Fully resolved non-empty index domain of one dispatch. */
    struct DispatchDomain {
        uint8_t rank = 0;
        std::array<uint64_t, kMaximumIndexRank> extents{};
        uint64_t workItemCount = 0;

        /** @brief Return whether active extents form the exact non-empty work-item count. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            if (rank == 0 || rank > kMaximumIndexRank || workItemCount == 0) {
                return false;
            }
            uint64_t count = 1;
            for (uint8_t axis = 0; axis < kMaximumIndexRank; ++axis) {
                if (axis >= rank) {
                    if (extents[axis] != 0) {
                        return false;
                    }
                    continue;
                }
                if (extents[axis] == 0 || count > std::numeric_limits<uint64_t>::max() / extents[axis]) {
                    return false;
                }
                count *= extents[axis];
            }
            return count == workItemCount;
        }

        friend constexpr bool operator==(DispatchDomain, DispatchDomain) noexcept = default;
    };

    /** @brief Stable reason a scalar argument could not be formed. */
    enum class ScalarArgumentError : uint8_t {
        InvalidValue,
        OutOfMemory,
    };

    /** @brief Owning canonical bytes for one runtime scalar or short-vector argument. */
    class ScalarArgument {
    public:
        /** @brief Validate and copy one canonical little-endian value. */
        [[nodiscard]] static std::expected<ScalarArgument, ScalarArgumentError>
        Create(ValueType type, std::span<const std::byte> bytes) noexcept {
            if (!type.Valid() || bytes.size() != type.ByteSize()) {
                return std::unexpected(ScalarArgumentError::InvalidValue);
            }
            if (type.scalar == ScalarKind::Bool) {
                for (const std::byte value : bytes) {
                    if (value != std::byte{0} && value != std::byte{1}) {
                        return std::unexpected(ScalarArgumentError::InvalidValue);
                    }
                }
            }
            try {
                return ScalarArgument{type, std::vector<std::byte>{bytes.begin(), bytes.end()}};
            } catch (const std::bad_alloc&) {
                return std::unexpected(ScalarArgumentError::OutOfMemory);
            }
        }

        /** @brief Return the declared portable value type. */
        [[nodiscard]] ValueType Type() const noexcept { return type_; }

        /** @brief Return the canonical value bytes. */
        [[nodiscard]] std::span<const std::byte> Bytes() const noexcept { return bytes_; }

    private:
        ScalarArgument(ValueType type, std::vector<std::byte> bytes) noexcept : type_(type), bytes_(std::move(bytes)) {}

        ValueType type_{};
        std::vector<std::byte> bytes_{};
    };

    /** @brief One runtime argument before it is checked against a program signature. */
    using KernelArgumentBinding = std::variant<ScalarArgument, BufferView>;

    /** @brief Pair whose distinct logical buffers require a final physical allocation overlap check. */
    struct PhysicalAliasRequirement {
        KernelArgumentId first{};
        KernelArgumentId second{};

        friend constexpr bool operator==(PhysicalAliasRequirement, PhysicalAliasRequirement) noexcept = default;
    };

    /** @brief Stable reason a runtime binding set could not instantiate a kernel program. */
    enum class KernelInvocationErrorCode : uint8_t {
        InvalidProgram,
        InvalidDomain,
        ArgumentCountMismatch,
        BindingKindMismatch,
        TypeMismatch,
        RankMismatch,
        LayoutMismatch,
        AlignmentMismatch,
        ExtentMismatch,
        AliasContractViolation,
        OutOfMemory,
    };

    /** @brief Invocation failure with the primary and optional related argument indices. */
    struct KernelInvocationError {
        KernelInvocationErrorCode code = KernelInvocationErrorCode::InvalidProgram;
        uint32_t argument = kInvalidKernelArgumentIndex;
        uint32_t related = kInvalidKernelArgumentIndex;

        friend constexpr bool operator==(KernelInvocationError, KernelInvocationError) noexcept = default;
    };

    /** @brief Immutable-by-convention runtime invocation safe to insert into a logical execution graph. */
    class KernelInvocation {
    public:
        /** @brief Return the canonical program identity. */
        [[nodiscard]] SemanticHash ProgramHash() const noexcept { return programHash_; }

        /** @brief Return the concrete dispatch domain. */
        [[nodiscard]] const DispatchDomain& Domain() const noexcept { return domain_; }

        /** @brief Return the program-declared proof obligation for semantics-preserving partitioning. */
        [[nodiscard]] PartitionContract Partition() const noexcept { return partition_; }

        /** @brief Return scalar-width capabilities required by the validated program. */
        [[nodiscard]] DeviceFeatureMask RequiredDeviceFeatures() const noexcept { return requiredDeviceFeatures_; }

        /** @brief Return device-independent demand refined by the actual runtime binding layouts. */
        [[nodiscard]] KernelResourceDemand ResourceDemand() const noexcept { return resourceDemand_; }

        /** @brief Return the immutable portable signature retained for physical binding and compilation. */
        [[nodiscard]] std::span<const KernelArgumentDesc> Signature() const noexcept { return signature_; }

        /** @brief Return argument bindings in signature order. */
        [[nodiscard]] std::span<const KernelArgumentBinding> Bindings() const noexcept { return bindings_; }

        /** @brief Return concrete logical effects derived from the program and bound views. */
        [[nodiscard]] std::span<const ResourceAccess> ResourceAccesses() const noexcept { return accesses_; }

        /** @brief Return operation-local non-contiguous footprints referenced by ResourceAccesses(). */
        [[nodiscard]] std::span<const AffineByteFootprint> ByteFootprints() const noexcept { return byteFootprints_; }

        /** @brief Return noalias proof obligations deferred until physical allocations are selected. */
        [[nodiscard]] std::span<const PhysicalAliasRequirement> PhysicalAliasRequirements() const noexcept {
            return physicalAliasRequirements_;
        }

    private:
        friend struct Detail::KernelInvocationBinding;

        friend std::expected<KernelInvocation, KernelInvocationError>
            BindKernelProgram(KernelProgramView, SemanticHash, std::span<const uint64_t>,
                              std::vector<KernelArgumentBinding>) noexcept;

        template<size_t ArgumentCapacity, size_t ScalarParameterCapacity, size_t InstructionCapacity>
        friend std::expected<KernelInvocation, KernelInvocationError>
        BindKernelProgram(const KernelProgram<ArgumentCapacity, ScalarParameterCapacity, InstructionCapacity>&,
                          std::span<const uint64_t>, std::vector<KernelArgumentBinding>) noexcept;

        SemanticHash programHash_{};
        DispatchDomain domain_{};
        PartitionContract partition_{};
        DeviceFeatureMask requiredDeviceFeatures_{};
        KernelResourceDemand resourceDemand_{};
        std::vector<KernelArgumentDesc> signature_{};
        std::vector<KernelArgumentBinding> bindings_{};
        std::vector<ResourceAccess> accesses_{};
        std::vector<AffineByteFootprint> byteFootprints_{};
        std::vector<PhysicalAliasRequirement> physicalAliasRequirements_{};
    };

    namespace Detail {

        [[nodiscard]] constexpr DeviceFeatureMask RequiredScalarFeatures(ScalarKind scalar) noexcept {
            switch (scalar) {
                case ScalarKind::Bool:
                case ScalarKind::Int8:
                case ScalarKind::UInt8:
                    return DeviceFeature::Int8;
                case ScalarKind::Int16:
                case ScalarKind::UInt16:
                    return DeviceFeature::Int16;
                case ScalarKind::Int64:
                case ScalarKind::UInt64:
                    return DeviceFeature::Int64;
                case ScalarKind::Float16:
                    return DeviceFeature::Float16;
                case ScalarKind::Float64:
                    return DeviceFeature::Float64;
                case ScalarKind::Int32:
                case ScalarKind::UInt32:
                case ScalarKind::Float32:
                case ScalarKind::Invalid:
                    return {};
            }
            return {};
        }

        [[nodiscard]] constexpr DeviceFeatureMask RequiredDeviceFeatures(KernelProgramView program) noexcept {
            DeviceFeatureMask required{};
            for (const KernelArgumentDesc& argument : program.arguments) {
                required |= RequiredScalarFeatures(argument.elementType.scalar);
            }
            for (const Instruction& instruction : program.scalar.instructions) {
                required |= RequiredScalarFeatures(instruction.resultType.scalar);
            }
            return required;
        }

        [[nodiscard]] constexpr std::expected<DispatchDomain, KernelInvocationError>
        ResolveDispatchDomain(IndexDomain programDomain, std::span<const uint64_t> extents) noexcept {
            if (!programDomain.Valid() || extents.size() != programDomain.rank) {
                return std::unexpected(KernelInvocationError{KernelInvocationErrorCode::InvalidDomain});
            }
            DispatchDomain resolved{.rank = programDomain.rank, .workItemCount = 1};
            for (uint8_t axis = 0; axis < programDomain.rank; ++axis) {
                if (extents[axis] == 0 ||
                    (programDomain.staticExtents[axis] != 0 && programDomain.staticExtents[axis] != extents[axis]) ||
                    resolved.workItemCount > std::numeric_limits<uint64_t>::max() / extents[axis]) {
                    return std::unexpected(KernelInvocationError{KernelInvocationErrorCode::InvalidDomain});
                }
                resolved.extents[axis] = extents[axis];
                resolved.workItemCount *= extents[axis];
            }
            return resolved;
        }

        struct KernelInvocationBinding final {
            [[nodiscard]] static std::expected<KernelInvocation, KernelInvocationError>
            Bind(KernelProgramView program, SemanticHash programHash, std::span<const uint64_t> extents,
                 std::vector<KernelArgumentBinding> bindings) noexcept {
                auto domain = ResolveDispatchDomain(program.domain, extents);
                if (!domain) {
                    return std::unexpected(domain.error());
                }
                if (bindings.size() != program.arguments.size()) {
                    return std::unexpected(KernelInvocationError{KernelInvocationErrorCode::ArgumentCountMismatch});
                }

                try {
                    KernelInvocation invocation;
                    invocation.programHash_ = programHash;
                    invocation.domain_ = *domain;
                    invocation.partition_ = program.partition;
                    invocation.requiredDeviceFeatures_ = RequiredDeviceFeatures(program);
                    invocation.resourceDemand_ = ResourceDemandOf(program);
                    invocation.signature_.assign(program.arguments.begin(), program.arguments.end());
                    invocation.bindings_ = std::move(bindings);
                    invocation.accesses_.reserve(program.arguments.size());
                    invocation.byteFootprints_.reserve(program.arguments.size());

                    for (uint32_t index = 0; index < program.arguments.size(); ++index) {
                        const KernelArgumentDesc& expected = program.arguments[index];
                        const KernelArgumentBinding& binding = invocation.bindings_[index];
                        if (expected.kind == ParameterKind::Value) {
                            const auto* scalar = std::get_if<ScalarArgument>(&binding);
                            if (scalar == nullptr) {
                                return std::unexpected(
                                    KernelInvocationError{KernelInvocationErrorCode::BindingKindMismatch, index});
                            }
                            if (scalar->Type() != expected.elementType ||
                                scalar->Bytes().size() != expected.elementType.ByteSize()) {
                                return std::unexpected(
                                    KernelInvocationError{KernelInvocationErrorCode::TypeMismatch, index});
                            }
                            continue;
                        }

                        const auto* view = std::get_if<BufferView>(&binding);
                        if (view == nullptr || !*view) {
                            return std::unexpected(
                                KernelInvocationError{KernelInvocationErrorCode::BindingKindMismatch, index});
                        }
                        const BufferDesc buffer = view->BufferDescriptor();
                        const BufferViewDesc& actual = view->Describe();
                        if (buffer.elementType != expected.elementType) {
                            return std::unexpected(
                                KernelInvocationError{KernelInvocationErrorCode::TypeMismatch, index});
                        }
                        if (actual.rank != expected.rank) {
                            return std::unexpected(
                                KernelInvocationError{KernelInvocationErrorCode::RankMismatch, index});
                        }
                        if (!LayoutSatisfies(actual.layout, expected.layout)) {
                            return std::unexpected(
                                KernelInvocationError{KernelInvocationErrorCode::LayoutMismatch, index});
                        }
                        if (actual.layout == LayoutKind::Strided) {
                            invocation.resourceDemand_.AddStridedMemory(expected.access,
                                                                        expected.elementType.ByteSize());
                        }
                        if (view->BaseAlignment() < expected.minimumAlignment) {
                            return std::unexpected(
                                KernelInvocationError{KernelInvocationErrorCode::AlignmentMismatch, index});
                        }
                        for (uint8_t axis = 0; axis < actual.rank; ++axis) {
                            if (actual.extents[axis] != domain->extents[axis]) {
                                return std::unexpected(
                                    KernelInvocationError{KernelInvocationErrorCode::ExtentMismatch, index, axis});
                            }
                        }
                    }

                    for (uint32_t index = 0; index < program.arguments.size(); ++index) {
                        const KernelArgumentDesc& argument = program.arguments[index];
                        if (argument.kind != ParameterKind::Buffer) {
                            continue;
                        }
                        const auto& view = std::get<BufferView>(invocation.bindings_[index]);
                        const AffineByteFootprint footprint = view.Footprint();
                        ByteFootprintId footprintId{};
                        if (!footprint.IsContiguous()) {
                            const auto existing = std::ranges::find(invocation.byteFootprints_, footprint);
                            if (existing == invocation.byteFootprints_.end()) {
                                footprintId.index = static_cast<uint32_t>(invocation.byteFootprints_.size());
                                invocation.byteFootprints_.push_back(footprint);
                            } else {
                                footprintId.index =
                                    static_cast<uint32_t>(existing - invocation.byteFootprints_.begin());
                            }
                        }
                        invocation.accesses_.push_back({
                            .buffer = view.BufferDescriptor().id,
                            .region = view.Region(),
                            .mode = argument.access,
                            .footprint = footprintId,
                        });
                    }

                    for (uint32_t first = 0; first < program.arguments.size(); ++first) {
                        if (program.arguments[first].kind != ParameterKind::Buffer) {
                            continue;
                        }
                        for (uint32_t second = first + 1; second < program.arguments.size(); ++second) {
                            const KernelArgumentDesc& firstDesc = program.arguments[first];
                            const KernelArgumentDesc& secondDesc = program.arguments[second];
                            if (secondDesc.kind != ParameterKind::Buffer || !firstDesc.aliasClass.Known() ||
                                !secondDesc.aliasClass.Known() || firstDesc.aliasClass == secondDesc.aliasClass) {
                                continue;
                            }
                            const auto& firstView = std::get<BufferView>(invocation.bindings_[first]);
                            const auto& secondView = std::get<BufferView>(invocation.bindings_[second]);
                            const BufferId firstBuffer = firstView.BufferDescriptor().id;
                            const BufferId secondBuffer = secondView.BufferDescriptor().id;
                            if (firstBuffer == secondBuffer) {
                                if (firstView.Region().Intersects(secondView.Region())) {
                                    return std::unexpected(KernelInvocationError{
                                        KernelInvocationErrorCode::AliasContractViolation, first, second});
                                }
                            } else {
                                invocation.physicalAliasRequirements_.push_back({{first}, {second}});
                            }
                        }
                    }
                    return invocation;
                } catch (const std::bad_alloc&) {
                    return std::unexpected(KernelInvocationError{KernelInvocationErrorCode::OutOfMemory});
                }
            }
        };

    } // namespace Detail

    /** @brief Bind one validated representation-independent program view and its canonical identity. */
    [[nodiscard]] inline std::expected<KernelInvocation, KernelInvocationError>
    BindKernelProgram(KernelProgramView program, SemanticHash programHash, std::span<const uint64_t> extents,
                      std::vector<KernelArgumentBinding> bindings) noexcept {
        try {
            std::vector<uint8_t> useCounts(program.arguments.size());
            if (!ValidateKernelProgram(program, useCounts)) {
                return std::unexpected(KernelInvocationError{KernelInvocationErrorCode::InvalidProgram});
            }
            return Detail::KernelInvocationBinding::Bind(program, programHash, extents, std::move(bindings));
        } catch (const std::bad_alloc&) {
            return std::unexpected(KernelInvocationError{KernelInvocationErrorCode::OutOfMemory});
        }
    }

    /**
     * @brief Validate runtime bindings, instantiate effects, and retain every logical buffer lease.
     * @details Distinct logical buffers can still share a backend allocation. Every returned physical alias
     * requirement must therefore be proven disjoint by the physical graph materializer before submission.
     */
    template<size_t ArgumentCapacity, size_t ScalarParameterCapacity, size_t InstructionCapacity>
    [[nodiscard]] std::expected<KernelInvocation, KernelInvocationError>
    BindKernelProgram(const KernelProgram<ArgumentCapacity, ScalarParameterCapacity, InstructionCapacity>& program,
                      std::span<const uint64_t> extents, std::vector<KernelArgumentBinding> bindings) noexcept {
        if (!ValidateKernelProgram(program)) {
            return std::unexpected(KernelInvocationError{KernelInvocationErrorCode::InvalidProgram});
        }
        return Detail::KernelInvocationBinding::Bind(program.View(), program.Hash(), extents, std::move(bindings));
    }

    static_assert(std::is_trivially_copyable_v<DispatchDomain>);
    static_assert(std::is_trivially_copyable_v<PhysicalAliasRequirement>);

} // namespace Sora::Compute

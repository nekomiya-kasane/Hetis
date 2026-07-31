/**
 * @file KernelProgram.h
 * @brief Canonical elementwise dispatch program built around pure scalar Kernel IR.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/KernelIR.h>
#include <Sora/Compute/KernelResourceDemand.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace Sora::Compute {

    inline constexpr uint32_t kKernelProgramMagic = 0x47525053U; // "SPRG" in little-endian byte order.
    inline constexpr uint16_t kKernelProgramMajorVersion = 1;
    inline constexpr uint16_t kKernelProgramMinorVersion = 0;
    inline constexpr uint32_t kInvalidKernelArgumentIndex = std::numeric_limits<uint32_t>::max();
    inline constexpr uint8_t kMaximumIndexRank = 3;

    /** @brief Strong identity of one argument in a kernel-program signature. */
    struct KernelArgumentId {
        uint32_t index = kInvalidKernelArgumentIndex;

        /** @brief Return whether this identity denotes a signature argument. */
        [[nodiscard]] constexpr bool Valid() const noexcept { return index != kInvalidKernelArgumentIndex; }

        friend constexpr bool operator==(KernelArgumentId, KernelArgumentId) noexcept = default;
    };

    /** @brief Alias class used to prove that differently classified buffer arguments cannot overlap. */
    struct AliasClassId {
        uint32_t value = 0;

        /** @brief Return whether this class carries a non-conservative alias promise. */
        [[nodiscard]] constexpr bool Known() const noexcept { return value != 0; }

        friend constexpr bool operator==(AliasClassId, AliasClassId) noexcept = default;
    };

    /** @brief One runtime value or ranked buffer in a kernel-program signature. */
    struct KernelArgumentDesc {
        ParameterKind kind = ParameterKind::Value;
        AccessMode access = AccessMode::Read;
        ValueType elementType{};
        uint8_t rank = 0;
        LayoutKind layout = LayoutKind::Scalar;
        uint32_t minimumAlignment = 1;
        AliasClassId aliasClass{};

        /** @brief Return whether two buffer arguments may overlap under their declared alias classes. */
        [[nodiscard]] constexpr bool MayAlias(const KernelArgumentDesc& other) const noexcept {
            return kind == ParameterKind::Buffer && other.kind == ParameterKind::Buffer &&
                   (!aliasClass.Known() || !other.aliasClass.Known() || aliasClass == other.aliasClass);
        }

        friend constexpr bool operator==(KernelArgumentDesc, KernelArgumentDesc) noexcept = default;
    };

    /** @brief Structured execution pattern whose semantics are understood by every backend. */
    enum class KernelPattern : uint8_t {
        Elementwise,
    };

    /** @brief One-to-three-dimensional logical index domain; zero extent denotes a runtime extent. */
    struct IndexDomain {
        uint8_t rank = 0;
        std::array<uint64_t, kMaximumIndexRank> staticExtents{};

        /** @brief Return whether rank and inactive axes form a canonical domain. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            if (rank == 0 || rank > kMaximumIndexRank) {
                return false;
            }
            for (uint8_t axis = rank; axis < staticExtents.size(); ++axis) {
                if (staticExtents[axis] != 0) {
                    return false;
                }
            }
            return true;
        }

        friend constexpr bool operator==(IndexDomain, IndexDomain) noexcept = default;
    };

    /** @brief Origin of one pure scalar-module input at a logical work item. */
    enum class ScalarInputKind : uint8_t {
        Argument,
        LogicalIndex,
    };

    /** @brief Mapping from one scalar-module parameter to a signature argument or logical axis. */
    struct ScalarInputBinding {
        ScalarInputKind kind = ScalarInputKind::Argument;
        uint32_t sourceIndex = kInvalidKernelArgumentIndex;

        friend constexpr bool operator==(ScalarInputBinding, ScalarInputBinding) noexcept = default;
    };

    /** @brief Granularity of a derived logical memory effect. */
    enum class EffectGranularity : uint8_t {
        PerElement,
    };

    /** @brief One derived buffer effect in signature order. */
    struct KernelEffect {
        KernelArgumentId argument{};
        AccessMode access = AccessMode::Read;
        EffectGranularity granularity = EffectGranularity::PerElement;

        friend constexpr bool operator==(KernelEffect, KernelEffect) noexcept = default;
    };

    /** @brief Fixed-capacity effect summary whose active length is part of its value. */
    template<size_t Capacity>
    struct StaticEffectSummary {
        std::array<KernelEffect, Capacity> storage{};
        size_t size = 0;

        /** @brief Return the active effects without copying. */
        [[nodiscard]] constexpr std::span<const KernelEffect> Effects() const noexcept {
            return {storage.data(), size < Capacity ? size : Capacity};
        }
    };

    namespace Detail {

        [[nodiscard]] constexpr bool SpecialFunctionOperation(OpCode opcode) noexcept {
            switch (opcode) {
                case OpCode::Div:
                case OpCode::Inv:
                case OpCode::Sin:
                case OpCode::Cos:
                case OpCode::Tan:
                case OpCode::Asin:
                case OpCode::Acos:
                case OpCode::Atan:
                case OpCode::Atan2:
                case OpCode::Exp:
                case OpCode::Log:
                case OpCode::Sqrt:
                case OpCode::Pow:
                    return true;
                case OpCode::Input:
                case OpCode::Constant:
                case OpCode::Add:
                case OpCode::Sub:
                case OpCode::Mul:
                case OpCode::Neg:
                case OpCode::Square:
                case OpCode::Abs:
                case OpCode::Fma:
                case OpCode::Mfs:
                case OpCode::Nms:
                case OpCode::Nma:
                case OpCode::Lerp:
                case OpCode::Clamp:
                case OpCode::Saturate:
                case OpCode::Sign:
                    return false;
            }
            return false;
        }

        [[nodiscard]] constexpr uint64_t OperationMultiplicity(OpCode opcode) noexcept {
            switch (opcode) {
                case OpCode::Input:
                case OpCode::Constant:
                    return 0;
                case OpCode::Fma:
                case OpCode::Mfs:
                case OpCode::Nms:
                case OpCode::Nma:
                case OpCode::Clamp:
                case OpCode::Saturate:
                    return 2;
                case OpCode::Lerp:
                    return 3;
                case OpCode::Add:
                case OpCode::Sub:
                case OpCode::Mul:
                case OpCode::Div:
                case OpCode::Neg:
                case OpCode::Inv:
                case OpCode::Square:
                case OpCode::Abs:
                case OpCode::Sin:
                case OpCode::Cos:
                case OpCode::Tan:
                case OpCode::Asin:
                case OpCode::Acos:
                case OpCode::Atan:
                case OpCode::Atan2:
                case OpCode::Exp:
                case OpCode::Log:
                case OpCode::Sqrt:
                case OpCode::Pow:
                case OpCode::Sign:
                    return 1;
            }
            return 0;
        }

        constexpr void AccumulateInstructionDemand(KernelResourceDemand& demand,
                                                   const Instruction& instruction) noexcept {
            const uint64_t lanes = instruction.resultType.lanes;
            const uint64_t operations = SaturatingDemandMultiply(OperationMultiplicity(instruction.opcode), lanes);
            if (operations == 0) {
                return;
            }
            if (instruction.resultType.scalar == ScalarKind::Float64) {
                demand.float64Operations = SaturatingDemandAdd(demand.float64Operations, operations);
            } else if (instruction.resultType.scalar == ScalarKind::Float16 ||
                       instruction.resultType.scalar == ScalarKind::Float32) {
                demand.floatingPointOperations = SaturatingDemandAdd(demand.floatingPointOperations, operations);
            } else {
                demand.integerOperations = SaturatingDemandAdd(demand.integerOperations, operations);
            }
            if (SpecialFunctionOperation(instruction.opcode)) {
                demand.specialFunctionOperations = SaturatingDemandAdd(demand.specialFunctionOperations, lanes);
            }
        }

    } // namespace Detail

    /** @brief Whether and along which axes a dispatch may be partitioned without changing semantics. */
    enum class PartitionKind : uint8_t {
        Indivisible,
        Elementwise,
    };

    /** @brief Explicit proof obligation consumed by multi-device partition planning. */
    struct PartitionContract {
        PartitionKind kind = PartitionKind::Indivisible;
        uint8_t splittableAxisMask = 0;

        /** @brief Construct a contract that forbids automatic partitioning. */
        [[nodiscard]] static constexpr PartitionContract Indivisible() noexcept { return {}; }

        /** @brief Construct an elementwise contract over every active axis of @p rank. */
        [[nodiscard]] static constexpr PartitionContract Elementwise(uint8_t rank) noexcept {
            return {.kind = PartitionKind::Elementwise,
                    .splittableAxisMask =
                        rank <= kMaximumIndexRank ? static_cast<uint8_t>((uint8_t{1} << rank) - 1U) : uint8_t{0}};
        }

        /** @brief Return whether the contract references only active axes and matches its kind. */
        [[nodiscard]] constexpr bool Valid(IndexDomain domain) const noexcept {
            if (!domain.Valid()) {
                return false;
            }
            const uint8_t activeMask = static_cast<uint8_t>((uint8_t{1} << domain.rank) - 1U);
            if ((splittableAxisMask & static_cast<uint8_t>(~activeMask)) != 0) {
                return false;
            }
            if (kind == PartitionKind::Indivisible) {
                return splittableAxisMask == 0;
            }
            return kind == PartitionKind::Elementwise && splittableAxisMask != 0;
        }

        friend constexpr bool operator==(PartitionContract, PartitionContract) noexcept = default;
    };

    /** @brief Borrowed representation-independent view of one canonical elementwise kernel program. */
    struct KernelProgramView {
        KernelPattern pattern = KernelPattern::Elementwise;
        IndexDomain domain{};
        PartitionContract partition{};
        KernelModuleView scalar{};
        std::span<const KernelArgumentDesc> arguments{};
        std::span<const ScalarInputBinding> inputs{};
        KernelArgumentId output{};
    };

    /** @brief Derive canonical per-work-item compute and logical memory demand from any program representation. */
    [[nodiscard]] constexpr KernelResourceDemand ResourceDemandOf(KernelProgramView program) noexcept {
        KernelResourceDemand demand{};
        for (const KernelArgumentDesc& argument : program.arguments) {
            if (argument.kind != ParameterKind::Buffer) {
                continue;
            }
            const uint64_t bytes = argument.elementType.ByteSize();
            if (argument.access != AccessMode::Write) {
                demand.bytesRead = Detail::SaturatingDemandAdd(demand.bytesRead, bytes);
            }
            if (argument.access != AccessMode::Read) {
                demand.bytesWritten = Detail::SaturatingDemandAdd(demand.bytesWritten, bytes);
            }
        }
        for (const Instruction& instruction : program.scalar.instructions) {
            Detail::AccumulateInstructionDemand(demand, instruction);
        }
        return demand;
    }

    /** @brief Canonical fixed-capacity program for one elementwise device dispatch. */
    template<size_t ArgumentCapacity, size_t ScalarParameterCapacity, size_t InstructionCapacity>
    struct KernelProgram {
        static_assert(ArgumentCapacity <= std::numeric_limits<uint32_t>::max());
        static_assert(ScalarParameterCapacity <= std::numeric_limits<uint32_t>::max());
        static_assert(InstructionCapacity <= std::numeric_limits<uint32_t>::max());

        using ScalarModule = KernelModule<ScalarParameterCapacity, InstructionCapacity>;

        static constexpr size_t kHeaderWireSize = 52;
        static constexpr size_t kArgumentWireSize = 16;
        static constexpr size_t kInputBindingWireSize = 8;
        static constexpr size_t kWireCapacity = kHeaderWireSize + ArgumentCapacity * kArgumentWireSize +
                                                ScalarParameterCapacity * kInputBindingWireSize +
                                                ScalarModule::kWireCapacity;
        static_assert(ScalarModule::kWireCapacity <= std::numeric_limits<uint32_t>::max());

        KernelPattern pattern = KernelPattern::Elementwise;
        IndexDomain domain{};
        PartitionContract partition{};
        ScalarModule scalar{};
        std::array<KernelArgumentDesc, ArgumentCapacity> arguments{};
        std::array<ScalarInputBinding, ScalarParameterCapacity> inputs{};
        uint32_t argumentCount = 0;
        uint32_t inputCount = 0;
        KernelArgumentId output{};

        /** @brief Return the active signature arguments. */
        [[nodiscard]] constexpr std::span<const KernelArgumentDesc> Arguments() const noexcept {
            return {arguments.data(), argumentCount < ArgumentCapacity ? argumentCount : ArgumentCapacity};
        }

        /** @brief Return scalar-input mappings in scalar-parameter order. */
        [[nodiscard]] constexpr std::span<const ScalarInputBinding> Inputs() const noexcept {
            return {inputs.data(), inputCount < ScalarParameterCapacity ? inputCount : ScalarParameterCapacity};
        }

        /** @brief Borrow this program through the representation-independent canonical-program interface. */
        [[nodiscard]] constexpr KernelProgramView View() const noexcept {
            return {.pattern = pattern,
                    .domain = domain,
                    .partition = partition,
                    .scalar = scalar.View(),
                    .arguments = Arguments(),
                    .inputs = Inputs(),
                    .output = output};
        }

        /** @brief Derive one per-element effect for every buffer argument without redundant stored metadata. */
        [[nodiscard]] constexpr StaticEffectSummary<ArgumentCapacity> EffectSummary() const noexcept {
            StaticEffectSummary<ArgumentCapacity> summary{};
            for (uint32_t index = 0; index < argumentCount && index < ArgumentCapacity; ++index) {
                if (arguments[index].kind == ParameterKind::Buffer) {
                    summary.storage[summary.size++] = {
                        .argument = {index},
                        .access = arguments[index].access,
                        .granularity = EffectGranularity::PerElement,
                    };
                }
            }
            return summary;
        }

        /** @brief Derive canonical per-work-item compute and logical memory demand. */
        [[nodiscard]] constexpr KernelResourceDemand ResourceDemand() const noexcept {
            return ResourceDemandOf(View());
        }

        /** @brief Serialize the active program and nested scalar module into canonical little-endian bytes. */
        [[nodiscard]] constexpr StaticBytes<kWireCapacity> Serialize() const noexcept {
            StaticBytes<kWireCapacity> bytes{};
            const auto scalarBytes = scalar.Serialize();
            Detail::AppendLittleEndian(bytes, kKernelProgramMagic);
            Detail::AppendLittleEndian(bytes, kKernelProgramMajorVersion);
            Detail::AppendLittleEndian(bytes, kKernelProgramMinorVersion);
            Detail::AppendLittleEndian(bytes, pattern);
            Detail::AppendLittleEndian(bytes, domain.rank);
            Detail::AppendLittleEndian(bytes, partition.kind);
            Detail::AppendLittleEndian(bytes, partition.splittableAxisMask);
            Detail::AppendLittleEndian(bytes, argumentCount);
            Detail::AppendLittleEndian(bytes, inputCount);
            Detail::AppendLittleEndian(bytes, output.index);
            Detail::AppendLittleEndian(bytes, static_cast<uint32_t>(scalarBytes.size));
            for (const uint64_t extent : domain.staticExtents) {
                Detail::AppendLittleEndian(bytes, extent);
            }

            for (const KernelArgumentDesc& argument : Arguments()) {
                Detail::AppendLittleEndian(bytes, argument.kind);
                Detail::AppendLittleEndian(bytes, argument.access);
                Detail::AppendLittleEndian(bytes, argument.elementType.scalar);
                Detail::AppendLittleEndian(bytes, argument.elementType.lanes);
                Detail::AppendLittleEndian(bytes, argument.rank);
                Detail::AppendLittleEndian(bytes, argument.layout);
                Detail::AppendLittleEndian(bytes, uint16_t{0});
                Detail::AppendLittleEndian(bytes, argument.minimumAlignment);
                Detail::AppendLittleEndian(bytes, argument.aliasClass.value);
            }
            for (const ScalarInputBinding& input : Inputs()) {
                Detail::AppendLittleEndian(bytes, input.kind);
                Detail::AppendLittleEndian(bytes, uint8_t{0});
                Detail::AppendLittleEndian(bytes, uint16_t{0});
                Detail::AppendLittleEndian(bytes, input.sourceIndex);
            }
            for (const std::byte byte : scalarBytes.Bytes()) {
                bytes.storage[bytes.size++] = byte;
            }
            return bytes;
        }

        /** @brief Return the semantic identity of the complete dispatch program. */
        [[nodiscard]] constexpr SemanticHash Hash() const noexcept {
            const auto bytes = Serialize();
            return Detail::HashBytes(bytes.Bytes());
        }
    };

    static_assert(std::is_trivially_copyable_v<KernelArgumentId>);
    static_assert(std::is_trivially_copyable_v<AliasClassId>);
    static_assert(std::is_trivially_copyable_v<KernelArgumentDesc>);
    static_assert(std::is_trivially_copyable_v<IndexDomain>);
    static_assert(std::is_trivially_copyable_v<ScalarInputBinding>);
    static_assert(std::is_trivially_copyable_v<KernelEffect>);
    static_assert(std::is_trivially_copyable_v<PartitionContract>);

} // namespace Sora::Compute

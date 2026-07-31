/**
 * @file KernelIR.h
 * @brief Canonical, endian-stable scalar kernel intermediate representation.
 * @ingroup Compute
 */
#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace Sora::Compute {

    inline constexpr uint32_t kKernelIrMagic = 0x52494B53U; // "SKIR" in little-endian byte order.
    inline constexpr uint16_t kKernelIrMajorVersion = 1;
    inline constexpr uint16_t kKernelIrMinorVersion = 0;
    inline constexpr uint32_t kInvalidValueIndex = std::numeric_limits<uint32_t>::max();

    /** @brief Scalar representation understood by every portable backend. */
    enum class ScalarKind : uint8_t {
        Invalid,
        Bool,
        Int8,
        Int16,
        Int32,
        Int64,
        UInt8,
        UInt16,
        UInt32,
        UInt64,
        Float16,
        Float32,
        Float64,
    };

    /** @brief Return whether @p kind is a defined portable scalar kind. */
    [[nodiscard]] constexpr bool ValidScalarKind(ScalarKind kind) noexcept {
        return kind >= ScalarKind::Bool && kind <= ScalarKind::Float64;
    }

    /** @brief Return the canonical in-memory width of @p kind, or zero for an invalid kind. */
    [[nodiscard]] constexpr uint8_t ScalarByteSize(ScalarKind kind) noexcept {
        switch (kind) {
            case ScalarKind::Bool:
            case ScalarKind::Int8:
            case ScalarKind::UInt8:
                return 1;
            case ScalarKind::Int16:
            case ScalarKind::UInt16:
            case ScalarKind::Float16:
                return 2;
            case ScalarKind::Int32:
            case ScalarKind::UInt32:
            case ScalarKind::Float32:
                return 4;
            case ScalarKind::Int64:
            case ScalarKind::UInt64:
            case ScalarKind::Float64:
                return 8;
            case ScalarKind::Invalid:
                return 0;
        }
        return 0;
    }

    /** @brief Logical role of one kernel parameter. */
    enum class ParameterKind : uint8_t {
        Value,
        Buffer,
    };

    /** @brief Return whether @p kind is a defined parameter kind. */
    [[nodiscard]] constexpr bool ValidParameterKind(ParameterKind kind) noexcept {
        return kind == ParameterKind::Value || kind == ParameterKind::Buffer;
    }

    /** @brief Memory effect permitted through one parameter. */
    enum class AccessMode : uint8_t {
        Read,
        Write,
        ReadWrite,
    };

    /** @brief Return whether @p mode is a defined memory access mode. */
    [[nodiscard]] constexpr bool ValidAccessMode(AccessMode mode) noexcept {
        return mode == AccessMode::Read || mode == AccessMode::Write || mode == AccessMode::ReadWrite;
    }

    /** @brief Logical ordering of a ranked buffer parameter. */
    enum class LayoutKind : uint8_t {
        Scalar,
        Contiguous,
        Strided,
        StructureOfArrays,
    };

    /** @brief Return whether @p layout is a defined logical layout kind. */
    [[nodiscard]] constexpr bool ValidLayoutKind(LayoutKind layout) noexcept {
        return layout == LayoutKind::Scalar || layout == LayoutKind::Contiguous || layout == LayoutKind::Strided ||
               layout == LayoutKind::StructureOfArrays;
    }

    /** @brief Return whether @p actual provides every addressing guarantee required by @p required. */
    [[nodiscard]] constexpr bool LayoutSatisfies(LayoutKind actual, LayoutKind required) noexcept {
        return ValidLayoutKind(actual) && ValidLayoutKind(required) &&
               (actual == required || (required == LayoutKind::Strided && actual != LayoutKind::Scalar));
    }

    /** @brief Floating-point transformations permitted while lowering a kernel. */
    enum class FloatingPointMode : uint8_t {
        Strict,
        Contract,
        Fast,
    };

    /** @brief Return whether @p mode is a defined floating-point mode. */
    [[nodiscard]] constexpr bool ValidFloatingPointMode(FloatingPointMode mode) noexcept {
        return mode == FloatingPointMode::Strict || mode == FloatingPointMode::Contract ||
               mode == FloatingPointMode::Fast;
    }

    /** @brief Ordering guarantee required of collective operations. */
    enum class ReductionMode : uint8_t {
        Deterministic,
        Reorderable,
    };

    /** @brief Return whether @p mode is a defined reduction mode. */
    [[nodiscard]] constexpr bool ValidReductionMode(ReductionMode mode) noexcept {
        return mode == ReductionMode::Deterministic || mode == ReductionMode::Reorderable;
    }

    /** @brief Backend-independent numerical contract participating in semantic identity. */
    struct NumericalPolicy {
        FloatingPointMode floatingPoint = FloatingPointMode::Strict;
        ReductionMode reduction = ReductionMode::Deterministic;

        friend constexpr bool operator==(NumericalPolicy, NumericalPolicy) noexcept = default;
    };

    /** @brief Portable scalar or short-vector value type. */
    struct ValueType {
        ScalarKind scalar = ScalarKind::Invalid;
        uint8_t lanes = 1;

        /** @brief Return whether the type has a known scalar representation and at least one lane. */
        [[nodiscard]] constexpr bool Valid() const noexcept { return ValidScalarKind(scalar) && lanes != 0; }

        /** @brief Return the canonical packed byte size, or zero when the type is invalid. */
        [[nodiscard]] constexpr uint32_t ByteSize() const noexcept {
            return static_cast<uint32_t>(ScalarByteSize(scalar)) * lanes;
        }

        friend constexpr bool operator==(ValueType, ValueType) noexcept = default;
    };

    /** @brief Strong SSA value identity; instruction index and value identity coincide in Kernel IR v1. */
    struct ValueId {
        uint32_t index = kInvalidValueIndex;

        /** @brief Return whether the identity denotes an instruction result. */
        [[nodiscard]] constexpr bool Valid() const noexcept { return index != kInvalidValueIndex; }

        friend constexpr bool operator==(ValueId, ValueId) noexcept = default;
    };

    /** @brief One value or buffer parameter in a kernel signature. */
    struct ParameterDesc {
        ParameterKind kind = ParameterKind::Value;
        AccessMode access = AccessMode::Read;
        ValueType elementType{};
        uint8_t rank = 0;
        LayoutKind layout = LayoutKind::Scalar;

        friend constexpr bool operator==(ParameterDesc, ParameterDesc) noexcept = default;
    };

    /** @brief Portable scalar instruction set shared by the interpreter and backend lowerers. */
    enum class OpCode : uint8_t {
        Input,
        Constant,
        Add,
        Sub,
        Mul,
        Div,
        Neg,
        Inv,
        Square,
        Abs,
        Sin,
        Cos,
        Tan,
        Asin,
        Acos,
        Atan,
        Atan2,
        Exp,
        Log,
        Sqrt,
        Pow,
        Fma,
        Mfs,
        Nms,
        Nma,
        Lerp,
        Clamp,
        Saturate,
        Sign,
    };

    /** @brief Return whether @p opcode is a defined scalar instruction. */
    [[nodiscard]] constexpr bool ValidOpCode(OpCode opcode) noexcept {
        return opcode >= OpCode::Input && opcode <= OpCode::Sign;
    }

    /** @brief Return the fixed operand count of @p opcode. */
    [[nodiscard]] constexpr uint8_t OperandCount(OpCode opcode) noexcept {
        switch (opcode) {
            case OpCode::Input:
            case OpCode::Constant:
                return 0;
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
            case OpCode::Exp:
            case OpCode::Log:
            case OpCode::Sqrt:
            case OpCode::Saturate:
            case OpCode::Sign:
                return 1;
            case OpCode::Add:
            case OpCode::Sub:
            case OpCode::Mul:
            case OpCode::Div:
            case OpCode::Atan2:
            case OpCode::Pow:
                return 2;
            case OpCode::Fma:
            case OpCode::Mfs:
            case OpCode::Nms:
            case OpCode::Nma:
            case OpCode::Lerp:
            case OpCode::Clamp:
                return 3;
        }
        return 0;
    }

    /** @brief One typed SSA instruction with at most three operands and one canonical immediate. */
    struct Instruction {
        OpCode opcode = OpCode::Constant;
        ValueType resultType{};
        uint8_t operandCount = 0;
        std::array<ValueId, 3> operands{};
        uint64_t immediate = 0;
        uint32_t flags = 0;

        friend constexpr bool operator==(const Instruction&, const Instruction&) noexcept = default;
    };

    /** @brief Return whether the immediate has the unique wire representation required by the opcode and type. */
    [[nodiscard]] constexpr bool ValidInstructionImmediate(const Instruction& instruction) noexcept {
        if (instruction.opcode == OpCode::Input) {
            return true;
        }
        if (instruction.opcode != OpCode::Constant) {
            return instruction.immediate == 0;
        }
        if (instruction.resultType.scalar == ScalarKind::Bool) {
            return instruction.immediate <= 1;
        }
        const uint8_t scalarBytes = ScalarByteSize(instruction.resultType.scalar);
        return scalarBytes != 0 && (scalarBytes == 8 || instruction.immediate >> (scalarBytes * 8U) == 0);
    }

    /** @brief Stable 128-bit semantic identity used to index portable and target-specific caches. */
    struct SemanticHash {
        uint64_t low = 0;
        uint64_t high = 0;

        friend constexpr bool operator==(SemanticHash, SemanticHash) noexcept = default;
    };

    /** @brief Fixed-capacity byte sequence whose active length is part of its value. */
    template<size_t Capacity>
    struct StaticBytes {
        std::array<std::byte, Capacity> storage{};
        size_t size = 0;

        /** @brief Return the active canonical byte sequence. */
        [[nodiscard]] constexpr std::span<const std::byte> Bytes() const noexcept {
            return {storage.data(), size < Capacity ? size : Capacity};
        }
    };

    namespace Detail {

        inline constexpr size_t kKernelIrHeaderWireSize = 24;
        inline constexpr size_t kParameterWireSize = 8;
        inline constexpr size_t kInstructionWireSize = 32;

        /** @brief Incremental, allocation-free implementation of the canonical semantic hash. */
        class SemanticHashBuilder final {
        public:
            /** @brief Append one canonical byte. */
            constexpr void Append(std::byte byte) noexcept {
                const uint64_t value = std::to_integer<uint8_t>(byte);
                low_ = (low_ ^ value) * kPrime;
                high_ = (high_ ^ (value + 0x9DU)) * kPrime;
            }

            /** @brief Append canonical bytes in their existing order. */
            constexpr void Append(std::span<const std::byte> bytes) noexcept {
                for (const std::byte byte : bytes) {
                    Append(byte);
                }
            }

            /** @brief Append one unsigned value in canonical little-endian order. */
            template<std::unsigned_integral T>
            constexpr void AppendLittleEndian(T value) noexcept {
                for (size_t index = 0; index < sizeof(T); ++index) {
                    Append(std::byte((value >> (index * 8U)) & T{0xFF}));
                }
            }

            /** @brief Append one enumeration through its canonical underlying representation. */
            template<typename E>
                requires std::is_enum_v<E>
            constexpr void AppendLittleEndian(E value) noexcept {
                using Representation = std::make_unsigned_t<std::underlying_type_t<E>>;
                AppendLittleEndian(static_cast<Representation>(value));
            }

            /** @brief Finish without consuming the builder. */
            [[nodiscard]] constexpr SemanticHash Finish() const noexcept { return {.low = low_, .high = high_}; }

        private:
            static constexpr uint64_t kPrime = 1099511628211ULL;
            uint64_t low_ = 14695981039346656037ULL;
            uint64_t high_ = 7809847782465536322ULL;
        };

        template<size_t Capacity, std::unsigned_integral T>
        constexpr void AppendLittleEndian(StaticBytes<Capacity>& bytes, T value) noexcept {
            for (size_t index = 0; index < sizeof(T); ++index) {
                bytes.storage[bytes.size++] = std::byte((value >> (index * 8U)) & T{0xFF});
            }
        }

        template<size_t Capacity, typename E>
            requires std::is_enum_v<E>
        constexpr void AppendLittleEndian(StaticBytes<Capacity>& bytes, E value) noexcept {
            AppendLittleEndian(bytes, static_cast<std::underlying_type_t<E>>(value));
        }

        [[nodiscard]] constexpr SemanticHash HashBytes(std::span<const std::byte> bytes) noexcept {
            SemanticHashBuilder builder;
            builder.Append(bytes);
            return builder.Finish();
        }

    } // namespace Detail

    /** @brief Borrowed representation-independent view of one validated-or-validatable scalar module. */
    struct KernelModuleView {
        NumericalPolicy numericalPolicy{};
        std::span<const ParameterDesc> parameters{};
        std::span<const Instruction> instructions{};
        ValueId result{};

        /** @brief Return the result type, or an invalid type when the result identity is outside this view. */
        [[nodiscard]] constexpr ValueType ResultType() const noexcept {
            return result.Valid() && result.index < instructions.size() ? instructions[result.index].resultType
                                                                        : ValueType{};
        }
    };

    /** @brief Exact-capacity, immutable-by-convention portable kernel module. */
    template<size_t ParameterCapacity, size_t InstructionCapacity>
    struct KernelModule {
        static_assert(ParameterCapacity <= std::numeric_limits<uint32_t>::max());
        static_assert(InstructionCapacity <= std::numeric_limits<uint32_t>::max());

        static constexpr size_t kParameterCapacity = ParameterCapacity;
        static constexpr size_t kInstructionCapacity = InstructionCapacity;
        static constexpr size_t kWireCapacity = Detail::kKernelIrHeaderWireSize +
                                                ParameterCapacity * Detail::kParameterWireSize +
                                                InstructionCapacity * Detail::kInstructionWireSize;

        NumericalPolicy numericalPolicy{};
        std::array<ParameterDesc, ParameterCapacity> parameters{};
        std::array<Instruction, InstructionCapacity> instructions{};
        uint32_t parameterCount = 0;
        uint32_t instructionCount = 0;
        ValueId result{};

        /** @brief Return the active parameter descriptors. */
        [[nodiscard]] constexpr std::span<const ParameterDesc> Parameters() const noexcept {
            return {parameters.data(), parameterCount < ParameterCapacity ? parameterCount : ParameterCapacity};
        }

        /** @brief Return the active SSA instructions. */
        [[nodiscard]] constexpr std::span<const Instruction> Instructions() const noexcept {
            return {instructions.data(),
                    instructionCount < InstructionCapacity ? instructionCount : InstructionCapacity};
        }

        /** @brief Borrow this module through the representation-independent scalar-IR interface. */
        [[nodiscard]] constexpr KernelModuleView View() const noexcept {
            return {.numericalPolicy = numericalPolicy,
                    .parameters = Parameters(),
                    .instructions = Instructions(),
                    .result = result};
        }

        /** @brief Serialize active fields to the canonical little-endian SKIR v1 wire representation. */
        [[nodiscard]] constexpr StaticBytes<kWireCapacity> Serialize() const noexcept {
            StaticBytes<kWireCapacity> bytes{};
            Detail::AppendLittleEndian(bytes, kKernelIrMagic);
            Detail::AppendLittleEndian(bytes, kKernelIrMajorVersion);
            Detail::AppendLittleEndian(bytes, kKernelIrMinorVersion);
            Detail::AppendLittleEndian(bytes, numericalPolicy.floatingPoint);
            Detail::AppendLittleEndian(bytes, numericalPolicy.reduction);
            Detail::AppendLittleEndian(bytes, uint16_t{0});
            Detail::AppendLittleEndian(bytes, parameterCount);
            Detail::AppendLittleEndian(bytes, instructionCount);
            Detail::AppendLittleEndian(bytes, result.index);

            for (const ParameterDesc& parameter : Parameters()) {
                Detail::AppendLittleEndian(bytes, parameter.kind);
                Detail::AppendLittleEndian(bytes, parameter.access);
                Detail::AppendLittleEndian(bytes, parameter.elementType.scalar);
                Detail::AppendLittleEndian(bytes, parameter.elementType.lanes);
                Detail::AppendLittleEndian(bytes, parameter.rank);
                Detail::AppendLittleEndian(bytes, parameter.layout);
                Detail::AppendLittleEndian(bytes, uint16_t{0});
            }

            for (const Instruction& instruction : Instructions()) {
                Detail::AppendLittleEndian(bytes, instruction.opcode);
                Detail::AppendLittleEndian(bytes, instruction.resultType.scalar);
                Detail::AppendLittleEndian(bytes, instruction.resultType.lanes);
                Detail::AppendLittleEndian(bytes, instruction.operandCount);
                for (const ValueId operand : instruction.operands) {
                    Detail::AppendLittleEndian(bytes, operand.index);
                }
                Detail::AppendLittleEndian(bytes, instruction.immediate);
                Detail::AppendLittleEndian(bytes, instruction.flags);
                Detail::AppendLittleEndian(bytes, uint32_t{0});
            }
            return bytes;
        }

        /** @brief Return the semantic identity of the canonical active wire representation. */
        [[nodiscard]] constexpr SemanticHash Hash() const noexcept {
            const auto bytes = Serialize();
            return Detail::HashBytes(bytes.Bytes());
        }
    };

    static_assert(std::is_trivially_copyable_v<ValueType>);
    static_assert(std::is_trivially_copyable_v<ParameterDesc>);
    static_assert(std::is_trivially_copyable_v<Instruction>);
    static_assert(std::is_standard_layout_v<Instruction>);

} // namespace Sora::Compute

/**
 * @file SlangKernelLowering.cpp
 * @brief KernelProgram-v1 to self-contained Slang compute-module lowering.
 * @ingroup Compute
 */
#include "SlangKernelLowering.h"

#include <Sora/Compute/Invocation/DispatchGeometry.h>

#include <Sora/Compute/KernelProgramValidation.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <new>
#include <string_view>

namespace Sora::Compute::Detail {

    namespace {

        [[nodiscard]] constexpr bool IsFloating(ScalarKind scalar) noexcept {
            return scalar == ScalarKind::Float16 || scalar == ScalarKind::Float32 || scalar == ScalarKind::Float64;
        }

        [[nodiscard]] constexpr bool SupportedScalar(ScalarKind scalar, DeviceFeatureMask features) noexcept {
            switch (scalar) {
                case ScalarKind::Bool:
                case ScalarKind::Int8:
                case ScalarKind::UInt8:
                    return features.Contains(DeviceFeature::Int8);
                case ScalarKind::Int16:
                case ScalarKind::UInt16:
                    return features.Contains(DeviceFeature::Int16);
                case ScalarKind::Int32:
                case ScalarKind::UInt32:
                    return true;
                case ScalarKind::Int64:
                case ScalarKind::UInt64:
                    return features.Contains(DeviceFeature::Int64);
                case ScalarKind::Float16:
                    return features.Contains(DeviceFeature::Float16);
                case ScalarKind::Float32:
                    return true;
                case ScalarKind::Float64:
                    return features.Contains(DeviceFeature::Float64);
                case ScalarKind::Invalid:
                    return false;
            }
            return false;
        }

        [[nodiscard]] constexpr uint32_t AlignUp(uint32_t value, uint32_t alignment) noexcept {
            return (value + alignment - 1U) & ~(alignment - 1U);
        }

        [[nodiscard]] constexpr char AxisName(uint32_t axis) noexcept {
            return axis == 0 ? 'x' : axis == 1 ? 'y' : 'z';
        }

        [[nodiscard]] std::string_view ScalarTypeName(ScalarKind scalar) noexcept {
            switch (scalar) {
                case ScalarKind::Bool:
                    return "bool";
                case ScalarKind::Int8:
                    return "int8_t";
                case ScalarKind::Int16:
                    return "int16_t";
                case ScalarKind::Int32:
                    return "int";
                case ScalarKind::Int64:
                    return "int64_t";
                case ScalarKind::UInt8:
                    return "uint8_t";
                case ScalarKind::UInt16:
                    return "uint16_t";
                case ScalarKind::UInt32:
                    return "uint";
                case ScalarKind::UInt64:
                    return "uint64_t";
                case ScalarKind::Float16:
                    return "half";
                case ScalarKind::Float32:
                    return "float";
                case ScalarKind::Float64:
                    return "double";
                case ScalarKind::Invalid:
                    return {};
            }
            return {};
        }

        void AppendUnsigned(std::string& text, uint64_t value) {
            char buffer[32];
            const auto converted = std::to_chars(std::begin(buffer), std::end(buffer), value);
            text.append(buffer, converted.ptr);
        }

        void AppendHex(std::string& text, uint64_t value, uint32_t digits) {
            static constexpr char kHex[] = "0123456789abcdef";
            text.append("0x");
            for (uint32_t shift = digits * 4U; shift != 0; shift -= 4U) {
                text.push_back(kHex[(value >> (shift - 4U)) & 0xFU]);
            }
        }

        void AppendValueType(std::string& text, ValueType type) {
            if (type.lanes == 1) {
                text.append(ScalarTypeName(type.scalar));
                return;
            }
            text.append("vector<");
            text.append(ScalarTypeName(type.scalar));
            text.append(", ");
            AppendUnsigned(text, type.lanes);
            text.push_back('>');
        }

        void AppendValueName(std::string& text, uint32_t value) {
            text.push_back('v');
            AppendUnsigned(text, value);
        }

        [[nodiscard]] VulkanDispatchAbi BuildDispatchAbi(const VulkanKernelProgram& program) noexcept {
            VulkanDispatchAbi abi{};
            abi.argumentMetadataOffsets.fill(kInvalidVulkanMetadataOffset);
            abi.metadataBinding = program.argumentCount;
            abi.localSize = CanonicalDispatchLocalSize(program.domain.rank);
            if (abi.localSize[0] == 0) {
                return {};
            }

            uint32_t offset = kVulkanMetadataHeaderSize;
            for (uint32_t argument = 0; argument < program.argumentCount; ++argument) {
                if (program.arguments[argument].kind == ParameterKind::Buffer) {
                    abi.argumentMetadataOffsets[argument] = offset;
                    offset += kVulkanBufferMetadataSize;
                    ++abi.storageBufferCount;
                }
            }
            ++abi.storageBufferCount; // Provider-private metadata buffer.
            for (uint32_t argument = 0; argument < program.argumentCount; ++argument) {
                const KernelArgumentDesc& desc = program.arguments[argument];
                if (desc.kind == ParameterKind::Value) {
                    const uint32_t alignment = std::max<uint32_t>(4, ScalarByteSize(desc.elementType.scalar));
                    offset = AlignUp(offset, alignment);
                    abi.argumentMetadataOffsets[argument] = offset;
                    offset += desc.elementType.ByteSize();
                }
            }
            abi.metadataSize = AlignUp(offset, 16);
            return abi;
        }

        class SourceEmitter final {
        public:
            SourceEmitter(const VulkanKernelProgram& program, VulkanDispatchAbi abi) : program_(program), abi_(abi) {
                text_.reserve(4096 + program.scalar.instructionCount * 96);
            }

            [[nodiscard]] SlangKernelSource Emit() {
                EmitResources();
                EmitOffsetFunctions();
                EmitEntryPoint();
                return {std::move(text_), abi_};
            }

        private:
            void EmitResources() {
                for (uint32_t argument = 0; argument < program_.argumentCount; ++argument) {
                    const KernelArgumentDesc& desc = program_.arguments[argument];
                    if (desc.kind != ParameterKind::Buffer) {
                        continue;
                    }
                    text_.append("[[vk::binding(");
                    AppendUnsigned(text_, argument);
                    text_.append(", 0)]] ");
                    text_.append(desc.access == AccessMode::Read ? "ByteAddressBuffer " : "RWByteAddressBuffer ");
                    AppendBufferName(argument);
                    text_.append(";\n");
                }
                text_.append("[[vk::binding(");
                AppendUnsigned(text_, abi_.metadataBinding);
                text_.append(", 0)]] ByteAddressBuffer sora_metadata;\n\n");
            }

            void EmitOffsetFunctions() {
                for (uint32_t argument = 0; argument < program_.argumentCount; ++argument) {
                    const KernelArgumentDesc& desc = program_.arguments[argument];
                    if (desc.kind != ParameterKind::Buffer) {
                        continue;
                    }
                    const uint32_t metadata = abi_.argumentMetadataOffsets[argument];
                    text_.append("uint sora_offset_");
                    AppendUnsigned(text_, argument);
                    text_.append("(uint3 threadId, uint component)\n{\n    uint baseOffset = "
                                 "sora_metadata.Load(");
                    AppendUnsigned(text_, metadata);
                    text_.append(");\n    uint layout = sora_metadata.Load(");
                    AppendUnsigned(text_, metadata + 4);
                    text_.append(");\n    uint componentStride = sora_metadata.Load(");
                    AppendUnsigned(text_, metadata + 8);
                    text_.append(");\n    uint linear = threadId.x;\n");
                    for (uint32_t axis = 1; axis < program_.domain.rank; ++axis) {
                        text_.append("    linear = linear * sora_metadata.Load(");
                        AppendUnsigned(text_, axis * 4U);
                        text_.append(") + threadId.");
                        text_.push_back(AxisName(axis));
                        text_.append(";\n");
                    }
                    text_.append("    if (layout == ");
                    AppendUnsigned(text_, std::to_underlying(LayoutKind::Contiguous));
                    text_.append(")\n        return baseOffset + linear * ");
                    AppendUnsigned(text_, desc.elementType.ByteSize());
                    text_.append(" + component * ");
                    AppendUnsigned(text_, ScalarByteSize(desc.elementType.scalar));
                    text_.append(";\n    if (layout == ");
                    AppendUnsigned(text_, std::to_underlying(LayoutKind::StructureOfArrays));
                    text_.append(")\n        return baseOffset + linear * ");
                    AppendUnsigned(text_, ScalarByteSize(desc.elementType.scalar));
                    text_.append(" + component * componentStride;\n    return baseOffset + "
                                 "component * componentStride");
                    for (uint32_t axis = 0; axis < program_.domain.rank; ++axis) {
                        text_.append(" + threadId.");
                        text_.push_back(AxisName(axis));
                        text_.append(" * sora_metadata.Load(");
                        AppendUnsigned(text_, metadata + 16U + axis * 4U);
                        text_.append(")");
                    }
                    text_.append(";\n}\n\n");
                }
            }

            void EmitEntryPoint() {
                text_.append("[shader(\"compute\")]\n[numthreads(");
                AppendUnsigned(text_, abi_.localSize[0]);
                text_.append(", ");
                AppendUnsigned(text_, abi_.localSize[1]);
                text_.append(", ");
                AppendUnsigned(text_, abi_.localSize[2]);
                text_.append(")]\nvoid ");
                text_.append(kVulkanEntryPoint);
                text_.append("(uint3 threadId : SV_DispatchThreadID)\n{\n");
                for (uint32_t axis = 0; axis < program_.domain.rank; ++axis) {
                    text_.append("    if (threadId.");
                    text_.push_back(AxisName(axis));
                    text_.append(" >= sora_metadata.Load(");
                    AppendUnsigned(text_, axis * 4U);
                    text_.append(")) return;\n");
                }
                for (uint32_t instruction = 0; instruction < program_.scalar.instructionCount; ++instruction) {
                    EmitInstruction(instruction);
                }
                EmitStore();
                text_.append("}\n");
            }

            void EmitInstruction(uint32_t index) {
                const Instruction& instruction = program_.scalar.instructions[index];
                text_.append("    ");
                if (program_.scalar.numericalPolicy.floatingPoint == FloatingPointMode::Strict &&
                    IsFloating(instruction.resultType.scalar) && instruction.opcode != OpCode::Input &&
                    instruction.opcode != OpCode::Constant) {
                    text_.append("precise ");
                }
                AppendValueType(text_, instruction.resultType);
                text_.push_back(' ');
                AppendValueName(text_, index);
                text_.append(" = ");
                if (instruction.opcode == OpCode::Input) {
                    EmitInput(static_cast<uint32_t>(instruction.immediate), instruction.resultType);
                } else if (instruction.opcode == OpCode::Constant) {
                    EmitConstant(instruction.resultType, instruction.immediate);
                } else {
                    EmitExpression(instruction);
                }
                text_.append(";\n");
            }

            void EmitInput(uint32_t parameter, ValueType type) {
                const ScalarInputBinding source = program_.inputs[parameter];
                if (source.kind == ScalarInputKind::LogicalIndex) {
                    AppendValueType(text_, type);
                    text_.append("(threadId.");
                    text_.push_back(AxisName(source.sourceIndex));
                    text_.append(" + sora_metadata.Load(");
                    AppendUnsigned(text_, 16U + source.sourceIndex * 4U);
                    text_.append("))");
                    return;
                }
                const KernelArgumentDesc& argument = program_.arguments[source.sourceIndex];
                if (argument.kind == ParameterKind::Value) {
                    text_.append("sora_metadata.Load<");
                    if (type.scalar == ScalarKind::Bool) {
                        text_.append("uint8_t");
                    } else {
                        AppendValueType(text_, type);
                    }
                    text_.append(">(");
                    AppendUnsigned(text_, abi_.argumentMetadataOffsets[source.sourceIndex]);
                    text_.push_back(')');
                    if (type.scalar == ScalarKind::Bool) {
                        text_.append(" != uint8_t(0)");
                    }
                    return;
                }
                if (type.lanes != 1) {
                    AppendValueType(text_, type);
                    text_.push_back('(');
                }
                for (uint32_t component = 0; component < type.lanes; ++component) {
                    if (component != 0) {
                        text_.append(", ");
                    }
                    AppendBufferName(source.sourceIndex);
                    text_.append(".Load<");
                    text_.append(type.scalar == ScalarKind::Bool ? std::string_view{"uint8_t"}
                                                                 : ScalarTypeName(type.scalar));
                    text_.append(">(sora_offset_");
                    AppendUnsigned(text_, source.sourceIndex);
                    text_.append("(threadId, ");
                    AppendUnsigned(text_, component);
                    text_.append("))");
                    if (type.scalar == ScalarKind::Bool) {
                        text_.append(" != uint8_t(0)");
                    }
                }
                if (type.lanes != 1) {
                    text_.push_back(')');
                }
            }

            void EmitConstant(ValueType type, uint64_t immediate) {
                if (type.lanes != 1) {
                    AppendValueType(text_, type);
                    text_.push_back('(');
                }
                switch (type.scalar) {
                    case ScalarKind::Bool:
                        text_.append((immediate & 1U) != 0 ? "true" : "false");
                        break;
                    case ScalarKind::Float16:
                        text_.append("bitCast<half>(uint16_t(");
                        AppendHex(text_, immediate & 0xFFFFU, 4);
                        text_.append("u))");
                        break;
                    case ScalarKind::Float32:
                        text_.append("asfloat(");
                        AppendHex(text_, immediate & 0xFFFFFFFFU, 8);
                        text_.append("u)");
                        break;
                    case ScalarKind::Float64:
                        text_.append("asdouble(");
                        AppendHex(text_, immediate & 0xFFFFFFFFU, 8);
                        text_.append("u, ");
                        AppendHex(text_, immediate >> 32U, 8);
                        text_.append("u)");
                        break;
                    default:
                        AppendValueType(text_, ValueType{type.scalar, 1});
                        text_.push_back('(');
                        AppendHex(text_, immediate, 16);
                        text_.append("ull)");
                        break;
                }
                if (type.lanes != 1) {
                    text_.push_back(')');
                }
            }

            void EmitExpression(const Instruction& instruction) {
                const auto operand = [this, &instruction](uint32_t index) {
                    AppendValueName(text_, instruction.operands[index].index);
                };
                switch (instruction.opcode) {
                    case OpCode::Add:
                    case OpCode::Sub:
                    case OpCode::Mul:
                    case OpCode::Div:
                        operand(0);
                        text_.push_back(' ');
                        text_.push_back(instruction.opcode == OpCode::Add   ? '+'
                                        : instruction.opcode == OpCode::Sub ? '-'
                                        : instruction.opcode == OpCode::Mul ? '*'
                                                                            : '/');
                        text_.push_back(' ');
                        operand(1);
                        return;
                    case OpCode::Neg:
                        text_.push_back('-');
                        operand(0);
                        return;
                    case OpCode::Inv:
                        AppendValueType(text_, instruction.resultType);
                        text_.append("(1.0) / ");
                        operand(0);
                        return;
                    case OpCode::Square:
                        operand(0);
                        text_.append(" * ");
                        operand(0);
                        return;
                    case OpCode::Mfs:
                    case OpCode::Nms:
                    case OpCode::Nma:
                        text_.append("fma(");
                        if (instruction.opcode != OpCode::Mfs) {
                            text_.push_back('-');
                        }
                        operand(0);
                        text_.append(", ");
                        operand(1);
                        text_.append(", ");
                        if (instruction.opcode != OpCode::Nma) {
                            text_.push_back('-');
                        }
                        operand(2);
                        text_.push_back(')');
                        return;
                    default:
                        break;
                }
                text_.append(FunctionName(instruction.opcode));
                text_.push_back('(');
                for (uint32_t index = 0; index < instruction.operandCount; ++index) {
                    if (index != 0) {
                        text_.append(", ");
                    }
                    operand(index);
                }
                text_.push_back(')');
            }

            [[nodiscard]] static std::string_view FunctionName(OpCode opcode) noexcept {
                switch (opcode) {
                    case OpCode::Abs:
                        return "abs";
                    case OpCode::Sin:
                        return "sin";
                    case OpCode::Cos:
                        return "cos";
                    case OpCode::Tan:
                        return "tan";
                    case OpCode::Asin:
                        return "asin";
                    case OpCode::Acos:
                        return "acos";
                    case OpCode::Atan:
                        return "atan";
                    case OpCode::Atan2:
                        return "atan2";
                    case OpCode::Exp:
                        return "exp";
                    case OpCode::Log:
                        return "log";
                    case OpCode::Sqrt:
                        return "sqrt";
                    case OpCode::Pow:
                        return "pow";
                    case OpCode::Fma:
                        return "fma";
                    case OpCode::Lerp:
                        return "lerp";
                    case OpCode::Clamp:
                        return "clamp";
                    case OpCode::Saturate:
                        return "saturate";
                    case OpCode::Sign:
                        return "sign";
                    default:
                        return {};
                }
            }

            void EmitStore() {
                const uint32_t output = program_.output.index;
                const ValueType type = program_.arguments[output].elementType;
                for (uint32_t component = 0; component < type.lanes; ++component) {
                    text_.append("    ");
                    AppendBufferName(output);
                    text_.append(".Store<");
                    text_.append(type.scalar == ScalarKind::Bool ? std::string_view{"uint8_t"}
                                                                 : ScalarTypeName(type.scalar));
                    text_.append(">(sora_offset_");
                    AppendUnsigned(text_, output);
                    text_.append("(threadId, ");
                    AppendUnsigned(text_, component);
                    text_.append("), ");
                    if (type.scalar == ScalarKind::Bool) {
                        text_.push_back('(');
                    }
                    AppendValueName(text_, program_.scalar.result.index);
                    if (type.lanes != 1) {
                        text_.push_back('[');
                        AppendUnsigned(text_, component);
                        text_.push_back(']');
                    }
                    if (type.scalar == ScalarKind::Bool) {
                        text_.append(" ? uint8_t(1) : uint8_t(0))");
                    }
                    text_.append(");\n");
                }
            }

            void AppendBufferName(uint32_t argument) {
                text_.append("sora_buffer_");
                AppendUnsigned(text_, argument);
            }

            const VulkanKernelProgram& program_;
            VulkanDispatchAbi abi_{};
            std::string text_{};
        };

    } // namespace

    bool SupportsVulkanProgram(const VulkanKernelProgram& program, DeviceFeatureMask features) noexcept {
        if (!ValidateKernelProgram(program)) {
            return false;
        }
        for (const KernelArgumentDesc& argument : program.Arguments()) {
            if (!SupportedScalar(argument.elementType.scalar, features) ||
                argument.elementType.lanes > kVulkanMaximumValueLanes) {
                return false;
            }
        }
        for (const Instruction& instruction : program.scalar.Instructions()) {
            if (!SupportedScalar(instruction.resultType.scalar, features) ||
                instruction.resultType.lanes > kVulkanMaximumValueLanes ||
                ((instruction.opcode != OpCode::Input && instruction.opcode != OpCode::Constant) &&
                 !IsFloating(instruction.resultType.scalar))) {
                return false;
            }
        }
        return true;
    }

    std::expected<SlangKernelSource, CompileError> LowerKernelProgramToSlang(const VulkanKernelProgram& program,
                                                                             DeviceFeatureMask features) noexcept {
        if (!SupportsVulkanProgram(program, features)) {
            return std::unexpected(CompileError{CompileErrorCode::UnsupportedIr, 0});
        }
        try {
            const VulkanDispatchAbi abi = BuildDispatchAbi(program);
            if (abi.metadataSize == 0) {
                return std::unexpected(CompileError{CompileErrorCode::UnsupportedIr, 0});
            }
            return SourceEmitter{program, abi}.Emit();
        } catch (const std::bad_alloc&) {
            return std::unexpected(CompileError{CompileErrorCode::OutOfMemory, 0});
        } catch (...) {
            return std::unexpected(CompileError{CompileErrorCode::CompilationFailed, 0});
        }
    }

} // namespace Sora::Compute::Detail

/**
 * @file LlvmKernelLowering.cpp
 * @brief KernelProgram-v1 to scalar-loop LLVM IR lowering.
 * @ingroup Compute
 */

#include "LlvmKernelLowering.h"

#include <Sora/Compute/KernelProgramValidation.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace Sora::Compute::Detail {

    namespace {

        constexpr size_t kMaximumValueLanes = 4;

        void AppendUnsigned(std::string& output, uint64_t value) {
            std::array<char, std::numeric_limits<uint64_t>::digits10 + 2> buffer{};
            const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
            if (error != std::errc{}) {
                std::unreachable();
            }
            output.append(buffer.data(), end);
        }

        void AppendSigned(std::string& output, int64_t value) {
            std::array<char, std::numeric_limits<int64_t>::digits10 + 3> buffer{};
            const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
            if (error != std::errc{}) {
                std::unreachable();
            }
            output.append(buffer.data(), end);
        }

        [[nodiscard]] std::string IndexedName(std::string_view prefix, uint64_t first, uint64_t second) {
            std::string result;
            result.reserve(prefix.size() + 24);
            result.append(prefix);
            AppendUnsigned(result, first);
            result.push_back('_');
            AppendUnsigned(result, second);
            return result;
        }

        [[nodiscard]] std::string IndexedName(std::string_view prefix, uint64_t first) {
            std::string result;
            result.reserve(prefix.size() + 16);
            result.append(prefix);
            AppendUnsigned(result, first);
            return result;
        }

        [[nodiscard]] std::string QualifiedIndex(std::string_view base, std::string_view qualifier, uint64_t index) {
            std::string result;
            result.reserve(base.size() + qualifier.size() + 18);
            result.append(base);
            result.push_back('_');
            result.append(qualifier);
            result.push_back('_');
            AppendUnsigned(result, index);
            return result;
        }

        [[nodiscard]] constexpr std::string_view IrType(ScalarKind scalar) noexcept {
            switch (scalar) {
                case ScalarKind::Bool:
                case ScalarKind::Int8:
                case ScalarKind::UInt8:
                    return "i8";
                case ScalarKind::Int16:
                case ScalarKind::UInt16:
                    return "i16";
                case ScalarKind::Int32:
                case ScalarKind::UInt32:
                    return "i32";
                case ScalarKind::Int64:
                case ScalarKind::UInt64:
                    return "i64";
                case ScalarKind::Float32:
                    return "float";
                case ScalarKind::Float64:
                    return "double";
                case ScalarKind::Invalid:
                case ScalarKind::Float16:
                    return {};
            }
            return {};
        }

        [[nodiscard]] constexpr bool SupportedScalar(ScalarKind scalar) noexcept {
            return !IrType(scalar).empty();
        }

        [[nodiscard]] constexpr std::string_view FastMath(FloatingPointMode mode) noexcept {
            switch (mode) {
                case FloatingPointMode::Strict:
                    return {};
                case FloatingPointMode::Contract:
                    return "contract ";
                case FloatingPointMode::Fast:
                    return "fast ";
            }
            return {};
        }

        [[nodiscard]] constexpr bool UsesScalarExternalMathCall(OpCode opcode) noexcept {
            switch (opcode) {
                case OpCode::Tan:
                case OpCode::Asin:
                case OpCode::Acos:
                case OpCode::Atan:
                case OpCode::Atan2:
                    return true;
                default:
                    return false;
            }
        }

        [[nodiscard]] bool CanRequestLoopVectorization(const LlvmCpuProgram& program) noexcept {
            for (const KernelArgumentDesc& argument : program.Arguments()) {
                if (argument.kind == ParameterKind::Buffer && argument.layout == LayoutKind::Strided) {
                    return false;
                }
            }
            for (const Instruction& instruction : program.scalar.Instructions()) {
                if (UsesScalarExternalMathCall(instruction.opcode)) {
                    return false;
                }
            }
            return true;
        }

        class IrEmitter final {
        public:
            explicit IrEmitter(const LlvmCpuProgram& value, LlvmFunctionSanitizerAbi sanitizerAbi)
                : program_(value),
                  sanitizerAbi_(sanitizerAbi),
                  requestLoopVectorization_(CanRequestLoopVectorization(value)) {
                output_.reserve(32768);
                for (auto& lanes : values_) {
                    for (auto& lane : lanes) {
                        lane.reserve(24);
                    }
                }
            }

            [[nodiscard]] std::string Emit() {
                EmitPreamble();
                EmitEntry();
                EmitLoop();
                EmitEpilogue();
                return std::move(output_);
            }

        private:
            void Text(std::string_view value) { output_.append(value); }
            void Number(uint64_t value) { AppendUnsigned(output_, value); }
            void Signed32(uint32_t value) {
                constexpr int64_t modulus = int64_t{1} << 32;
                const int64_t signedValue = value <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max())
                                                ? static_cast<int64_t>(value)
                                                : static_cast<int64_t>(value) - modulus;
                AppendSigned(output_, signedValue);
            }

            void Line(std::string_view value = {}) {
                output_.append(value);
                output_.push_back('\n');
            }

            [[nodiscard]] std::string ValueName(uint32_t instruction, uint8_t lane) const {
                return IndexedName("%v", instruction, lane);
            }

            [[nodiscard]] std::string ArgumentName(uint32_t argument, uint8_t lane) const {
                return IndexedName("%a", argument, lane);
            }

            [[nodiscard]] std::string BufferName(uint32_t argument, std::string_view suffix) const {
                std::string result = IndexedName("%b", argument);
                result.push_back('_');
                result.append(suffix);
                return result;
            }

            void EmitPreamble() {
                Line("; Sora KernelProgram v1 native CPU lowering");
                Line("source_filename = \"Sora.KernelProgram\"");
                Line();
                Line("declare float @llvm.fabs.f32(float)");
                Line("declare double @llvm.fabs.f64(double)");
                Line("declare float @llvm.sin.f32(float)");
                Line("declare double @llvm.sin.f64(double)");
                Line("declare float @llvm.cos.f32(float)");
                Line("declare double @llvm.cos.f64(double)");
                Line("declare float @llvm.exp.f32(float)");
                Line("declare double @llvm.exp.f64(double)");
                Line("declare float @llvm.log.f32(float)");
                Line("declare double @llvm.log.f64(double)");
                Line("declare float @llvm.sqrt.f32(float)");
                Line("declare double @llvm.sqrt.f64(double)");
                Line("declare float @llvm.pow.f32(float, float)");
                Line("declare double @llvm.pow.f64(double, double)");
                Line("declare float @llvm.fma.f32(float, float, float)");
                Line("declare double @llvm.fma.f64(double, double, double)");
                Line("declare float @tanf(float)");
                Line("declare double @tan(double)");
                Line("declare float @asinf(float)");
                Line("declare double @asin(double)");
                Line("declare float @acosf(float)");
                Line("declare double @acos(double)");
                Line("declare float @atanf(float)");
                Line("declare double @atan(double)");
                Line("declare float @atan2f(float, float)");
                Line("declare double @atan2(double, double)");
                Line();
                Text("define void @sora_cpu_kernel_entry(ptr nocapture readnone %state, ptr nocapture readonly %inv) "
                     "nounwind");
                if (sanitizerAbi_.Enabled()) {
                    Text(" !func_sanitize !");
                    Number(requestLoopVectorization_ ? 3 : 2);
                }
                Line(" {");
            }

            void EmitPointerLoad(std::string_view result, std::string_view base, size_t offset) {
                Text("  ");
                Text(result);
                Text("_addr = getelementptr i8, ptr ");
                Text(base);
                Text(", i64 ");
                Number(offset);
                Line();
                Text("  ");
                Text(result);
                Text(" = load ptr, ptr ");
                Text(result);
                Line("_addr, align 1");
            }

            void EmitIntegerLoad(std::string_view result, std::string_view base, size_t offset) {
                Text("  ");
                Text(result);
                Text("_addr = getelementptr i8, ptr ");
                Text(base);
                Text(", i64 ");
                Number(offset);
                Line();
                Text("  ");
                Text(result);
                Text(" = load i64, ptr ");
                Text(result);
                Line("_addr, align 1");
            }

            void EmitEntry() {
                Line("entry:");
                EmitPointerLoad("%buffers", "%inv", offsetof(CpuCodeInvocation, buffers));
                EmitPointerLoad("%scalars", "%inv", offsetof(CpuCodeInvocation, scalars));
                EmitIntegerLoad("%begin", "%inv", offsetof(CpuCodeInvocation, begin));
                EmitIntegerLoad("%end", "%inv", offsetof(CpuCodeInvocation, end));
                for (uint8_t axis = 0; axis < program_.domain.rank; ++axis) {
                    const std::string origin = IndexedName("%origin", axis);
                    const std::string extent = IndexedName("%extent", axis);
                    EmitIntegerLoad(origin, "%inv", offsetof(CpuCodeInvocation, origin) + axis * sizeof(uint64_t));
                    EmitIntegerLoad(extent, "%inv", offsetof(CpuCodeInvocation, extents) + axis * sizeof(uint64_t));
                }
                for (uint32_t argument = 0; argument < program_.argumentCount; ++argument) {
                    const KernelArgumentDesc& descriptor = program_.arguments[argument];
                    if (descriptor.kind == ParameterKind::Buffer) {
                        EmitBufferEntry(argument, descriptor);
                    } else {
                        EmitScalarEntry(argument, descriptor.elementType);
                    }
                }
                Line("  %empty = icmp uge i64 %begin, %end");
                Line("  br i1 %empty, label %exit, label %loop");
                Line();
            }

            void EmitBufferEntry(uint32_t argument, const KernelArgumentDesc& descriptor) {
                const std::string base = BufferName(argument, "base");
                Text("  ");
                Text(base);
                Text(" = getelementptr i8, ptr %buffers, i64 ");
                Number(argument * sizeof(CpuCodeBuffer));
                Line();
                EmitPointerLoad(BufferName(argument, "data"), base, offsetof(CpuCodeBuffer, data));
                if (descriptor.layout == LayoutKind::StructureOfArrays || descriptor.layout == LayoutKind::Strided) {
                    EmitIntegerLoad(BufferName(argument, "component_stride"), base,
                                    offsetof(CpuCodeBuffer, componentStrideBytes));
                }
                if (descriptor.layout == LayoutKind::Strided) {
                    for (uint8_t axis = 0; axis < descriptor.rank; ++axis) {
                        EmitIntegerLoad(BufferName(argument, IndexedName("stride", axis)), base,
                                        offsetof(CpuCodeBuffer, byteStrides) + axis * sizeof(uint64_t));
                    }
                }
            }

            void EmitScalarEntry(uint32_t argument, ValueType type) {
                const std::string base = IndexedName("%sbase", argument);
                Text("  ");
                Text(base);
                Text(" = getelementptr i8, ptr %scalars, i64 ");
                Number(argument * sizeof(CpuCodeScalar));
                Line();
                const std::string data = IndexedName("%sdata", argument);
                EmitPointerLoad(data, base, offsetof(CpuCodeScalar, data));
                for (uint8_t lane = 0; lane < type.lanes; ++lane) {
                    const std::string pointer = IndexedName("%sap", argument, lane);
                    Text("  ");
                    Text(pointer);
                    Text(" = getelementptr i8, ptr ");
                    Text(data);
                    Text(", i64 ");
                    Number(lane * ScalarByteSize(type.scalar));
                    Line();
                    const std::string value = ArgumentName(argument, lane);
                    Text("  ");
                    Text(value);
                    Text(" = load ");
                    Text(IrType(type.scalar));
                    Text(", ptr ");
                    Text(pointer);
                    Line(", align 1");
                }
            }

            void EmitLoop() {
                Line("loop:");
                Line("  %index = phi i64 [ %begin, %entry ], [ %next, %loop_latch ]");
                for (uint32_t instruction = 0; instruction < program_.scalar.instructionCount; ++instruction) {
                    EmitInstruction(instruction, program_.scalar.instructions[instruction]);
                }
                EmitStoreResult();
                Line("  br label %loop_latch");
                Line();
                Line("loop_latch:");
                Line("  %next = add nuw i64 %index, 1");
                Line("  %done = icmp uge i64 %next, %end");
                Line("  br i1 %done, label %exit, label %loop, !llvm.loop !0");
                Line();
            }

            void EmitInstruction(uint32_t index, const Instruction& instruction) {
                for (uint8_t lane = 0; lane < instruction.resultType.lanes; ++lane) {
                    const std::string result = ValueName(index, lane);
                    values_[index][lane] = result;
                    if (instruction.opcode == OpCode::Input) {
                        EmitInput(index, lane, instruction);
                    } else if (instruction.opcode == OpCode::Constant) {
                        EmitConstant(result, instruction.resultType.scalar, instruction.immediate);
                    } else {
                        EmitOperation(index, lane, instruction);
                    }
                }
            }

            void EmitInput(uint32_t index, uint8_t lane, const Instruction& instruction) {
                const ScalarInputBinding& input = program_.inputs[instruction.immediate];
                if (input.kind == ScalarInputKind::LogicalIndex) {
                    EmitLogicalIndex(values_[index][lane], input.sourceIndex);
                    return;
                }
                const KernelArgumentDesc& source = program_.arguments[input.sourceIndex];
                if (source.kind == ParameterKind::Value) {
                    values_[index][lane] = ArgumentName(input.sourceIndex, lane);
                    return;
                }
                const std::string pointer =
                    EmitElementPointer(input.sourceIndex, lane, "%index", IndexedName("in", index, lane));
                Text("  ");
                Text(values_[index][lane]);
                Text(" = load ");
                Text(IrType(instruction.resultType.scalar));
                Text(", ptr ");
                Text(pointer);
                Line(", align 1");
            }

            void EmitLogicalIndex(std::string_view result, uint32_t requestedAxis) {
                std::string remainder = "%index";
                for (uint8_t axis = program_.domain.rank; axis-- > 0;) {
                    const std::string coordinate = QualifiedIndex(result, "coord", axis);
                    Text("  ");
                    Text(coordinate);
                    Text(" = urem i64 ");
                    Text(remainder);
                    Text(", %extent");
                    Number(axis);
                    Line();
                    if (axis == requestedAxis) {
                        Text("  ");
                        Text(result);
                        Text(" = add i64 ");
                        Text(coordinate);
                        Text(", %origin");
                        Number(axis);
                        Line();
                        return;
                    }
                    const std::string quotient = QualifiedIndex(result, "quotient", axis);
                    Text("  ");
                    Text(quotient);
                    Text(" = udiv i64 ");
                    Text(remainder);
                    Text(", %extent");
                    Number(axis);
                    Line();
                    remainder = quotient;
                }
            }

            void EmitConstant(std::string_view result, ScalarKind scalar, uint64_t immediate) {
                const uint8_t bytes = ScalarByteSize(scalar);
                const uint64_t bits =
                    bytes == sizeof(uint64_t) ? immediate : immediate & ((uint64_t{1} << (bytes * 8U)) - uint64_t{1});
                Text("  ");
                Text(result);
                if (scalar == ScalarKind::Float32 || scalar == ScalarKind::Float64) {
                    Text(" = bitcast i");
                } else {
                    Text(" = add i");
                }
                Number(bytes * 8U);
                Text(" ");
                Number(bits);
                if (scalar == ScalarKind::Float32 || scalar == ScalarKind::Float64) {
                    Text(" to ");
                    Text(IrType(scalar));
                } else {
                    Text(", 0");
                }
                Line();
            }

            [[nodiscard]] std::string Operand(const Instruction& instruction, uint8_t operand, uint8_t lane) const {
                return values_[instruction.operands[operand].index][lane];
            }

            void EmitOperation(uint32_t index, uint8_t lane, const Instruction& instruction) {
                const std::string result = values_[index][lane];
                const std::string first = Operand(instruction, 0, lane);
                const std::string second = instruction.operandCount > 1 ? Operand(instruction, 1, lane) : std::string{};
                const std::string third = instruction.operandCount > 2 ? Operand(instruction, 2, lane) : std::string{};
                const std::string_view type = IrType(instruction.resultType.scalar);
                const std::string_view fast = FastMath(program_.scalar.numericalPolicy.floatingPoint);
                switch (instruction.opcode) {
                    case OpCode::Add:
                        return EmitBinary(result, "fadd", fast, type, first, second);
                    case OpCode::Sub:
                        return EmitBinary(result, "fsub", fast, type, first, second);
                    case OpCode::Mul:
                        return EmitBinary(result, "fmul", fast, type, first, second);
                    case OpCode::Div:
                        return EmitBinary(result, "fdiv", fast, type, first, second);
                    case OpCode::Neg:
                        return EmitUnary(result, "fneg", fast, type, first);
                    case OpCode::Inv:
                        return EmitBinary(result, "fdiv", fast, type, "1.000000e+00", first);
                    case OpCode::Square:
                        return EmitBinary(result, "fmul", fast, type, first, first);
                    case OpCode::Abs:
                        return EmitIntrinsic(result, "fabs", fast, type, first);
                    case OpCode::Sin:
                        return EmitIntrinsic(result, "sin", fast, type, first);
                    case OpCode::Cos:
                        return EmitIntrinsic(result, "cos", fast, type, first);
                    case OpCode::Tan:
                        return EmitExternal(result, "tan", fast, type, first);
                    case OpCode::Asin:
                        return EmitExternal(result, "asin", fast, type, first);
                    case OpCode::Acos:
                        return EmitExternal(result, "acos", fast, type, first);
                    case OpCode::Atan:
                        return EmitExternal(result, "atan", fast, type, first);
                    case OpCode::Atan2:
                        return EmitExternal(result, "atan2", fast, type, first, second);
                    case OpCode::Exp:
                        return EmitIntrinsic(result, "exp", fast, type, first);
                    case OpCode::Log:
                        return EmitIntrinsic(result, "log", fast, type, first);
                    case OpCode::Sqrt:
                        return EmitIntrinsic(result, "sqrt", fast, type, first);
                    case OpCode::Pow:
                        return EmitIntrinsic(result, "pow", fast, type, first, second);
                    case OpCode::Fma:
                        return EmitIntrinsic(result, "fma", fast, type, first, second, third);
                    case OpCode::Mfs:
                        return EmitSignedFma(result, fast, type, first, second, third, false, true);
                    case OpCode::Nms:
                        return EmitSignedFma(result, fast, type, first, second, third, true, true);
                    case OpCode::Nma:
                        return EmitSignedFma(result, fast, type, first, second, third, true, false);
                    case OpCode::Lerp:
                        return EmitLerp(result, fast, type, first, second, third);
                    case OpCode::Clamp:
                        return EmitClamp(result, type, first, second, third);
                    case OpCode::Saturate:
                        return EmitClamp(result, type, first, "0.000000e+00", "1.000000e+00");
                    case OpCode::Sign:
                        return EmitSign(result, type, first);
                    case OpCode::Input:
                    case OpCode::Constant:
                        return;
                }
            }

            void EmitUnary(std::string_view result, std::string_view opcode, std::string_view flags,
                           std::string_view type, std::string_view value) {
                Text("  ");
                Text(result);
                Text(" = ");
                Text(opcode);
                Text(" ");
                Text(flags);
                Text(type);
                Text(" ");
                Text(value);
                Line();
            }

            void EmitBinary(std::string_view result, std::string_view opcode, std::string_view flags,
                            std::string_view type, std::string_view first, std::string_view second) {
                Text("  ");
                Text(result);
                Text(" = ");
                Text(opcode);
                Text(" ");
                Text(flags);
                Text(type);
                Text(" ");
                Text(first);
                Text(", ");
                Text(second);
                Line();
            }

            void EmitCall(std::string_view result, std::string_view function, std::string_view flags,
                          std::string_view type, std::string_view first, std::string_view second = {},
                          std::string_view third = {}) {
                Text("  ");
                Text(result);
                Text(" = call ");
                Text(flags);
                Text(type);
                Text(" @");
                Text(function);
                Text("(");
                Text(type);
                Text(" ");
                Text(first);
                if (!second.empty()) {
                    Text(", ");
                    Text(type);
                    Text(" ");
                    Text(second);
                }
                if (!third.empty()) {
                    Text(", ");
                    Text(type);
                    Text(" ");
                    Text(third);
                }
                Line(")");
            }

            void EmitIntrinsic(std::string_view result, std::string_view operation, std::string_view flags,
                               std::string_view type, std::string_view first, std::string_view second = {},
                               std::string_view third = {}) {
                std::string name{"llvm."};
                name.append(operation);
                name.append(type == "float" ? ".f32" : ".f64");
                EmitCall(result, name, flags, type, first, second, third);
            }

            void EmitExternal(std::string_view result, std::string_view operation, std::string_view flags,
                              std::string_view type, std::string_view first, std::string_view second = {}) {
                std::string name{operation};
                if (type == "float") {
                    name.push_back('f');
                }
                EmitCall(result, name, flags, type, first, second);
            }

            void EmitSignedFma(std::string_view result, std::string_view flags, std::string_view type,
                               std::string_view first, std::string_view second, std::string_view third,
                               bool negateFirst, bool negateThird) {
                std::string firstValue{first};
                std::string thirdValue{third};
                if (negateFirst) {
                    firstValue = std::string{result} + "_neg_first";
                    EmitUnary(firstValue, "fneg", flags, type, first);
                }
                if (negateThird) {
                    thirdValue = std::string{result} + "_neg_third";
                    EmitUnary(thirdValue, "fneg", flags, type, third);
                }
                EmitIntrinsic(result, "fma", flags, type, firstValue, second, thirdValue);
            }

            void EmitSelect(std::string_view result, std::string_view condition, std::string_view type,
                            std::string_view whenTrue, std::string_view whenFalse) {
                Text("  ");
                Text(result);
                Text(" = select i1 ");
                Text(condition);
                Text(", ");
                Text(type);
                Text(" ");
                Text(whenTrue);
                Text(", ");
                Text(type);
                Text(" ");
                Text(whenFalse);
                Line();
            }

            void EmitCompare(std::string_view result, std::string_view predicate, std::string_view type,
                             std::string_view first, std::string_view second) {
                Text("  ");
                Text(result);
                Text(" = fcmp ");
                Text(predicate);
                Text(" ");
                Text(type);
                Text(" ");
                Text(first);
                Text(", ");
                Text(second);
                Line();
            }

            void EmitClamp(std::string_view result, std::string_view type, std::string_view value, std::string_view low,
                           std::string_view high) {
                const std::string below = std::string{result} + "_below";
                const std::string lower = std::string{result} + "_lower";
                const std::string above = std::string{result} + "_above";
                EmitCompare(below, "olt", type, value, low);
                EmitSelect(lower, below, type, low, value);
                EmitCompare(above, "olt", type, high, lower);
                EmitSelect(result, above, type, high, lower);
            }

            void EmitSign(std::string_view result, std::string_view type, std::string_view value) {
                const std::string positive = std::string{result} + "_positive";
                const std::string negative = std::string{result} + "_negative";
                const std::string positiveInt = std::string{result} + "_positive_i";
                const std::string negativeInt = std::string{result} + "_negative_i";
                const std::string difference = std::string{result} + "_difference";
                EmitCompare(positive, "olt", type, "0.000000e+00", value);
                EmitCompare(negative, "olt", type, value, "0.000000e+00");
                Text("  ");
                Text(positiveInt);
                Text(" = zext i1 ");
                Text(positive);
                Line(" to i32");
                Text("  ");
                Text(negativeInt);
                Text(" = zext i1 ");
                Text(negative);
                Line(" to i32");
                Text("  ");
                Text(difference);
                Text(" = sub i32 ");
                Text(positiveInt);
                Text(", ");
                Text(negativeInt);
                Line();
                Text("  ");
                Text(result);
                Text(" = sitofp i32 ");
                Text(difference);
                Text(" to ");
                Text(type);
                Line();
            }

            void EmitLerp(std::string_view result, std::string_view flags, std::string_view type,
                          std::string_view first, std::string_view second, std::string_view third) {
                const std::string prefix{result};
                EmitCompare(prefix + "_a_le_zero", "ole", type, first, "0.000000e+00");
                EmitCompare(prefix + "_b_ge_zero", "oge", type, second, "0.000000e+00");
                EmitCompare(prefix + "_a_ge_zero", "oge", type, first, "0.000000e+00");
                EmitCompare(prefix + "_b_le_zero", "ole", type, second, "0.000000e+00");
                Line("  " + prefix + "_opposite0 = and i1 " + prefix + "_a_le_zero, " + prefix + "_b_ge_zero");
                Line("  " + prefix + "_opposite1 = and i1 " + prefix + "_a_ge_zero, " + prefix + "_b_le_zero");
                Line("  " + prefix + "_opposite = or i1 " + prefix + "_opposite0, " + prefix + "_opposite1");
                EmitBinary(prefix + "_tb", "fmul", flags, type, third, second);
                EmitBinary(prefix + "_one_minus_t", "fsub", flags, type, "1.000000e+00", third);
                EmitBinary(prefix + "_wa", "fmul", flags, type, prefix + "_one_minus_t", first);
                EmitBinary(prefix + "_cross", "fadd", flags, type, prefix + "_tb", prefix + "_wa");
                EmitCompare(prefix + "_t_is_one", "oeq", type, third, "1.000000e+00");
                EmitBinary(prefix + "_delta", "fsub", flags, type, second, first);
                EmitBinary(prefix + "_scaled", "fmul", flags, type, third, prefix + "_delta");
                EmitBinary(prefix + "_x", "fadd", flags, type, first, prefix + "_scaled");
                EmitCompare(prefix + "_t_gt_one", "ogt", type, third, "1.000000e+00");
                EmitCompare(prefix + "_b_gt_a", "ogt", type, second, first);
                Line("  " + prefix + "_direction = icmp eq i1 " + prefix + "_t_gt_one, " + prefix + "_b_gt_a");
                EmitCompare(prefix + "_b_lt_x", "olt", type, second, prefix + "_x");
                EmitSelect(prefix + "_maximum", prefix + "_b_lt_x", type, prefix + "_x", second);
                EmitCompare(prefix + "_x_lt_b", "olt", type, prefix + "_x", second);
                EmitSelect(prefix + "_minimum", prefix + "_x_lt_b", type, prefix + "_x", second);
                EmitSelect(prefix + "_bounded", prefix + "_direction", type, prefix + "_maximum", prefix + "_minimum");
                EmitSelect(prefix + "_same_sign", prefix + "_t_is_one", type, second, prefix + "_bounded");
                EmitSelect(result, prefix + "_opposite", type, prefix + "_cross", prefix + "_same_sign");
            }

            [[nodiscard]] std::string EmitElementPointer(uint32_t argument, uint8_t component, std::string_view linear,
                                                         std::string suffix) {
                const KernelArgumentDesc& descriptor = program_.arguments[argument];
                const uint64_t scalarBytes = ScalarByteSize(descriptor.elementType.scalar);
                const std::string offset = "%offset_" + suffix;
                if (descriptor.layout == LayoutKind::Contiguous) {
                    Text("  ");
                    Text(offset);
                    Text(" = mul i64 ");
                    Text(linear);
                    Text(", ");
                    Number(descriptor.elementType.ByteSize());
                    Line();
                    if (component != 0) {
                        const std::string componentOffset = offset + "_component";
                        Text("  ");
                        Text(componentOffset);
                        Text(" = add i64 ");
                        Text(offset);
                        Text(", ");
                        Number(component * scalarBytes);
                        Line();
                        return EmitAddress(argument, componentOffset, suffix);
                    }
                    return EmitAddress(argument, offset, suffix);
                }
                if (descriptor.layout == LayoutKind::StructureOfArrays) {
                    Text("  ");
                    Text(offset);
                    Text(" = mul i64 ");
                    Text(linear);
                    Text(", ");
                    Number(scalarBytes);
                    Line();
                    if (component == 0) {
                        return EmitAddress(argument, offset, suffix);
                    }
                    const std::string planeOffset = offset + "_plane";
                    Text("  ");
                    Text(planeOffset);
                    Text(" = mul i64 ");
                    Text(BufferName(argument, "component_stride"));
                    Text(", ");
                    Number(component);
                    Line();
                    const std::string total = offset + "_total";
                    Text("  ");
                    Text(total);
                    Text(" = add i64 ");
                    Text(offset);
                    Text(", ");
                    Text(planeOffset);
                    Line();
                    return EmitAddress(argument, total, suffix);
                }
                return EmitStridedPointer(argument, component, linear, std::move(suffix));
            }

            [[nodiscard]] std::string EmitStridedPointer(uint32_t argument, uint8_t component, std::string_view linear,
                                                         std::string suffix) {
                const KernelArgumentDesc& descriptor = program_.arguments[argument];
                std::string remainder{linear};
                std::string accumulated;
                for (uint8_t axis = descriptor.rank; axis-- > 0;) {
                    const std::string coordinate = QualifiedIndex("%coord_" + suffix, "axis", axis);
                    Text("  ");
                    Text(coordinate);
                    Text(" = urem i64 ");
                    Text(remainder);
                    Text(", %extent");
                    Number(axis);
                    Line();
                    const std::string contribution = QualifiedIndex("%contribution_" + suffix, "axis", axis);
                    Text("  ");
                    Text(contribution);
                    Text(" = mul i64 ");
                    Text(coordinate);
                    Text(", ");
                    Text(BufferName(argument, IndexedName("stride", axis)));
                    Line();
                    if (accumulated.empty()) {
                        accumulated = contribution;
                    } else {
                        const std::string sum = QualifiedIndex("%sum_" + suffix, "axis", axis);
                        Text("  ");
                        Text(sum);
                        Text(" = add i64 ");
                        Text(accumulated);
                        Text(", ");
                        Text(contribution);
                        Line();
                        accumulated = sum;
                    }
                    if (axis != 0) {
                        const std::string quotient = QualifiedIndex("%quotient_" + suffix, "axis", axis);
                        Text("  ");
                        Text(quotient);
                        Text(" = udiv i64 ");
                        Text(remainder);
                        Text(", %extent");
                        Number(axis);
                        Line();
                        remainder = quotient;
                    }
                }
                if (component != 0) {
                    const std::string componentOffset = "%component_" + suffix;
                    Text("  ");
                    Text(componentOffset);
                    Text(" = mul i64 ");
                    Text(BufferName(argument, "component_stride"));
                    Text(", ");
                    Number(component);
                    Line();
                    const std::string total = "%total_" + suffix;
                    Text("  ");
                    Text(total);
                    Text(" = add i64 ");
                    Text(accumulated);
                    Text(", ");
                    Text(componentOffset);
                    Line();
                    accumulated = total;
                }
                return EmitAddress(argument, accumulated, suffix);
            }

            [[nodiscard]] std::string EmitAddress(uint32_t argument, std::string_view offset, std::string_view suffix) {
                const std::string pointer = "%pointer_" + std::string{suffix};
                Text("  ");
                Text(pointer);
                Text(" = getelementptr i8, ptr ");
                Text(BufferName(argument, "data"));
                Text(", i64 ");
                Text(offset);
                Line();
                return pointer;
            }

            void EmitStoreResult() {
                const Instruction& result = program_.scalar.instructions[program_.scalar.result.index];
                for (uint8_t lane = 0; lane < result.resultType.lanes; ++lane) {
                    const std::string pointer =
                        EmitElementPointer(program_.output.index, lane, "%index", IndexedName("out", lane));
                    Text("  store ");
                    Text(IrType(result.resultType.scalar));
                    Text(" ");
                    Text(values_[program_.scalar.result.index][lane]);
                    Text(", ptr ");
                    Text(pointer);
                    Line(", align 1");
                }
            }

            void EmitEpilogue() {
                Line("exit:");
                Line("  ret void");
                Line("}");
                Line();
                Line(requestLoopVectorization_ ? "!0 = distinct !{!0, !1, !2}" : "!0 = distinct !{!0, !1}");
                Line("!1 = !{!\"llvm.loop.mustprogress\"}");
                if (requestLoopVectorization_) {
                    Line("!2 = !{!\"llvm.loop.vectorize.enable\", i1 true}");
                }
                if (sanitizerAbi_.Enabled()) {
                    Text(requestLoopVectorization_ ? "!3 = !{i32 " : "!2 = !{i32 ");
                    Signed32(sanitizerAbi_.signature);
                    Text(", i32 ");
                    Signed32(sanitizerAbi_.typeHash);
                    Line("}");
                }
            }

            const LlvmCpuProgram& program_;
            LlvmFunctionSanitizerAbi sanitizerAbi_{};
            bool requestLoopVectorization_ = false;
            std::string output_;
            std::array<std::array<std::string, kMaximumValueLanes>, kCpuMaximumInstructions> values_{};
        };

        [[nodiscard]] bool SupportedProgram(const LlvmCpuProgram& program) noexcept {
            if (!ValidateKernelProgram(program)) {
                return false;
            }
            for (const KernelArgumentDesc& argument : program.Arguments()) {
                if (!SupportedScalar(argument.elementType.scalar) || argument.elementType.lanes > kMaximumValueLanes) {
                    return false;
                }
            }
            for (const Instruction& instruction : program.scalar.Instructions()) {
                if (!SupportedScalar(instruction.resultType.scalar) ||
                    instruction.resultType.lanes > kMaximumValueLanes ||
                    ((instruction.opcode != OpCode::Input && instruction.opcode != OpCode::Constant) &&
                     instruction.resultType.scalar != ScalarKind::Float32 &&
                     instruction.resultType.scalar != ScalarKind::Float64)) {
                    return false;
                }
            }
            return true;
        }

    } // namespace

    std::expected<std::string, CompileError>
    LowerKernelProgramToLlvmIr(const LlvmCpuProgram& program, LlvmFunctionSanitizerAbi sanitizerAbi) noexcept {
        if (!SupportedProgram(program)) {
            return std::unexpected(CompileError{CompileErrorCode::UnsupportedIr, 0});
        }
        try {
            return IrEmitter{program, sanitizerAbi}.Emit();
        } catch (const std::bad_alloc&) {
            return std::unexpected(CompileError{CompileErrorCode::OutOfMemory, 0});
        } catch (...) {
            return std::unexpected(CompileError{CompileErrorCode::CompilationFailed, 0});
        }
    }

} // namespace Sora::Compute::Detail

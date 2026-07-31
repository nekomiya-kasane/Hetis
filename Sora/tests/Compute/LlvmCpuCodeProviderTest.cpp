#include <Sora/Compute/Backend/LlvmCpuCodeProvider.h>
#include <Sora/Compute/ElementwiseKernel.h>
#include <Sora/Compute/KernelBuilder.h>
#include <Sora/Compute/KernelProgramBuilder.h>
#include <Sora/Core/PAL/CpuInfo.h>
#include <Sora/Math/PortableCompile.h>
#include <Sora/Platform.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <barrier>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace {

    constexpr Sora::Compute::ValueType kFloat{Sora::Compute::ScalarKind::Float32, 1};
    constexpr Sora::Compute::ValueType kDouble{Sora::Compute::ScalarKind::Float64, 1};
    constexpr Sora::Compute::ValueType kInt32{Sora::Compute::ScalarKind::Int32, 1};
    constexpr Sora::Compute::ValueType kVec3{Sora::Compute::ScalarKind::Float32, 3};
    constexpr Sora::Compute::ValueType kUInt64{Sora::Compute::ScalarKind::UInt64, 1};

    struct Vec3Pod {
        float x;
        float y;
        float z;
    };

    struct DoubleSlot {
        double value = 0.0;
        uint64_t canary = 0;
    };

    static_assert(sizeof(Vec3Pod) == sizeof(float) * 3);
    static_assert(sizeof(DoubleSlot) == sizeof(double) * 2);

    consteval auto MakeSaxpyProgram() {
        Sora::Compute::KernelBuilder<3, 4> scalar;
        const auto alpha = scalar.AddValueParameter(kFloat);
        const auto x = scalar.AddValueParameter(kFloat);
        const auto y = scalar.AddValueParameter(kFloat);
        auto module = std::move(scalar).Finish(scalar.Ternary(Sora::Compute::OpCode::Fma, alpha, x, y));

        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<4, 3, 4> program{
            domain, module, Sora::Compute::PartitionContract::Elementwise(domain.rank)};
        const auto alphaArgument = program.AddValueArgument(kFloat);
        const auto xArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, domain.rank,
                                                         Sora::Compute::LayoutKind::Contiguous, 64, {1});
        const auto yArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kFloat, domain.rank,
                                                         Sora::Compute::LayoutKind::Contiguous, 64, {2});
        const auto output = program.AddBufferArgument(Sora::Compute::AccessMode::Write, kFloat, domain.rank,
                                                      Sora::Compute::LayoutKind::Contiguous, 64, {3});
        program.BindArgument(alphaArgument);
        program.BindArgument(xArgument);
        program.BindArgument(yArgument);
        return std::move(program).Finish(output);
    }

    consteval auto MakeVec3AddProgram(Sora::Compute::LayoutKind layout) {
        constexpr auto definition = Sora::Math::CompilePortable<float, 2>(
            [](auto first, auto second) { return Sora::Math::Add(first, second); });
        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        return Sora::Compute::LiftElementwise<3>(definition, domain,
                                                 {.layout = layout, .minimumAlignment = 64, .aliasClass = {3}},
                                                 Sora::Compute::ElementwiseInputSpec::Buffer(layout, 64, {1}),
                                                 Sora::Compute::ElementwiseInputSpec::Buffer(layout, 64, {2}));
    }

    consteval auto MakeIndexProgram() {
        Sora::Compute::KernelBuilder<1, 1> scalar;
        const auto index = scalar.AddValueParameter(kUInt64);
        auto module = std::move(scalar).Finish(index);

        constexpr Sora::Compute::IndexDomain domain{.rank = 2};
        Sora::Compute::KernelProgramBuilder<1, 1, 1> program{
            domain, module, Sora::Compute::PartitionContract::Elementwise(domain.rank)};
        const auto output = program.AddBufferArgument(Sora::Compute::AccessMode::Write, kUInt64, domain.rank,
                                                      Sora::Compute::LayoutKind::Contiguous, 8, {1});
        program.BindLogicalIndex(1);
        return std::move(program).Finish(output);
    }

    consteval auto MakeDoubleMathProgram() {
        Sora::Compute::KernelBuilder<2, 4> scalar;
        const auto x = scalar.AddValueParameter(kDouble);
        const auto y = scalar.AddValueParameter(kDouble);
        const auto tangent = scalar.Unary(Sora::Compute::OpCode::Tan, x);
        auto module = std::move(scalar).Finish(scalar.Binary(Sora::Compute::OpCode::Atan2, tangent, y));

        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<3, 2, 4> program{
            domain, module, Sora::Compute::PartitionContract::Elementwise(domain.rank)};
        const auto xArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kDouble, domain.rank,
                                                         Sora::Compute::LayoutKind::Contiguous, 64, {1});
        const auto yArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kDouble, domain.rank,
                                                         Sora::Compute::LayoutKind::Contiguous, 64, {2});
        const auto output = program.AddBufferArgument(Sora::Compute::AccessMode::Write, kDouble, domain.rank,
                                                      Sora::Compute::LayoutKind::Contiguous, 64, {3});
        program.BindArgument(xArgument);
        program.BindArgument(yArgument);
        return std::move(program).Finish(output);
    }

    consteval auto MakeStridedDoubleProgram() {
        Sora::Compute::KernelBuilder<1, 3> scalar;
        const auto x = scalar.AddValueParameter(kDouble);
        const auto offset = scalar.Constant(1.25);
        auto module = std::move(scalar).Finish(scalar.Binary(Sora::Compute::OpCode::Add, x, offset));

        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<2, 1, 3> program{
            domain, module, Sora::Compute::PartitionContract::Elementwise(domain.rank)};
        const auto xArgument = program.AddBufferArgument(Sora::Compute::AccessMode::Read, kDouble, domain.rank,
                                                         Sora::Compute::LayoutKind::Strided, 64, {1});
        const auto output = program.AddBufferArgument(Sora::Compute::AccessMode::Write, kDouble, domain.rank,
                                                      Sora::Compute::LayoutKind::Strided, 64, {2});
        program.BindArgument(xArgument);
        return std::move(program).Finish(output);
    }

    consteval auto MakeIntegerConstantProgram() {
        Sora::Compute::KernelBuilder<0, 1> scalar;
        auto module = std::move(scalar).Finish(scalar.Constant(int32_t{-1'234'567}));

        constexpr Sora::Compute::IndexDomain domain{.rank = 1};
        Sora::Compute::KernelProgramBuilder<1, 0, 1> program{
            domain, module, Sora::Compute::PartitionContract::Elementwise(domain.rank)};
        const auto output = program.AddBufferArgument(Sora::Compute::AccessMode::Write, kInt32, domain.rank,
                                                      Sora::Compute::LayoutKind::Contiguous, 64, {1});
        return std::move(program).Finish(output);
    }

    constexpr auto kSaxpy = MakeSaxpyProgram();
    constexpr auto kVec3AosAdd = MakeVec3AddProgram(Sora::Compute::LayoutKind::Contiguous);
    constexpr auto kVec3SoaAdd = MakeVec3AddProgram(Sora::Compute::LayoutKind::StructureOfArrays);
    constexpr auto kIndex = MakeIndexProgram();
    constexpr auto kDoubleMath = MakeDoubleMathProgram();
    constexpr auto kStridedDouble = MakeStridedDoubleProgram();
    constexpr auto kIntegerConstant = MakeIntegerConstantProgram();

    [[nodiscard]] Sora::Compute::TargetIdentityStability HostIdentityStability(const Sora::PAL::CpuInfo& cpu) noexcept {
        if (cpu.architecture == Sora::CpuArchitecture::X86 || cpu.architecture == Sora::CpuArchitecture::X64) {
            return cpu.x86.vendor.front() != '\0' ? Sora::Compute::TargetIdentityStability::MachineStable
                                                  : Sora::Compute::TargetIdentityStability::ProcessStable;
        }
        return Sora::Compute::TargetIdentityStability::ProcessStable;
    }

    struct ProviderFixture {
        Sora::Compute::CpuCodeProvider provider;
        Sora::Compute::TargetFingerprint target;
    };

    ProviderFixture MakeProvider() {
#if defined(_WIN32)
        const std::array searchPaths{std::filesystem::path{LR"(C:\Program Files\LLVM\bin)"}};
        auto created = Sora::Compute::CreateLlvmCpuCodeProvider(
            {.searchPaths = std::span<const std::filesystem::path>{searchPaths}});
#else
        auto created = Sora::Compute::CreateLlvmCpuCodeProvider();
#endif
        const Sora::Compute::CompileError error = created ? Sora::Compute::CompileError{} : created.error();
        INFO("LLVM provider error code=" << static_cast<uint32_t>(error.code) << ", detail=" << error.detail);
        REQUIRE(created.has_value());
        Sora::Compute::CpuCodeProvider provider = std::move(*created);
        const Sora::PAL::CpuInfo cpu = Sora::PAL::DetectHostCpuInfo();
        const Sora::Compute::TargetFingerprint target{
            .device = {0x534F52415F4C4C56ULL, 0x4D5F544553545F31ULL},
            .backend = Sora::Compute::BackendKind::Cpu,
            .hostArchitecture = cpu.architecture,
            .operatingSystem = Sora::kOperatingSystem,
            .binaryFormat = Sora::kBinaryFormat,
            .byteOrder = cpu.byteOrder,
            .abi = Sora::kAbiFamily,
            .identityStability = HostIdentityStability(cpu),
            .addressBits = cpu.addressBits,
            .architectureId = cpu.ArchitectureId(),
            .fixedVectorBits = cpu.MaximumFixedVectorBits(),
            .scalableVectorBits = cpu.currentScalableVectorBits,
            .compilerIdentity = provider.CompilerIdentity(),
            .driverIdentity = provider.DriverIdentity(),
        };
        return {.provider = std::move(provider), .target = target};
    }

    template<size_t Arguments, size_t Parameters, size_t Instructions>
    Sora::Compute::CpuCodeArtifact
    Compile(ProviderFixture& fixture,
            const Sora::Compute::KernelProgram<Arguments, Parameters, Instructions>& program) {
        const auto bytes = program.Serialize();
        const Sora::Compute::CpuCodeCompileRequest request{
            .programData = bytes.Bytes().data(),
            .programSize = bytes.Bytes().size(),
            .programHash = program.Hash(),
            .target = fixture.target,
        };
        auto compiled = fixture.provider.Compile(request);
        const Sora::Compute::CompileError error = compiled ? Sora::Compute::CompileError{} : compiled.error();
        INFO("LLVM compile error code=" << static_cast<uint32_t>(error.code) << ", detail=" << error.detail);
        REQUIRE(compiled.has_value());
        return std::move(*compiled);
    }

    template<typename T>
    [[nodiscard]] std::byte* Bytes(T* values) noexcept {
        return reinterpret_cast<std::byte*>(values);
    }

    template<typename T>
    [[nodiscard]] const std::byte* Bytes(const T* values) noexcept {
        return reinterpret_cast<const std::byte*>(values);
    }

} // namespace

TEST_CASE("LLVM CPU provider JITs SAXPY and validates persistent artifact envelopes",
          "[Sora][Compute][LLVM][JIT][Persistence]") {
    constexpr uint64_t count = 257;
    constexpr float alpha = 2.25F;
    auto fixture = MakeProvider();
    const auto programBytes = kSaxpy.Serialize();
    const Sora::Compute::CpuCodeCompileRequest request{
        .programData = programBytes.Bytes().data(),
        .programSize = programBytes.Bytes().size(),
        .programHash = kSaxpy.Hash(),
        .target = fixture.target,
    };
    auto compiled = fixture.provider.Compile(request);
    REQUIRE(compiled.has_value());
    auto duplicate = fixture.provider.Compile(request);
    REQUIRE(duplicate.has_value());
    auto exported = fixture.provider.Export(*compiled);
    REQUIRE(exported.has_value());
    REQUIRE(exported->size() > programBytes.Bytes().size());
    auto imported = fixture.provider.Import(request, *exported);
    REQUIRE(imported.has_value());

    std::vector<float> x(count);
    std::vector<float> y(count);
    std::vector<float> output(count);
    for (uint64_t index = 0; index < count; ++index) {
        x[index] = static_cast<float>(index) * 0.5F;
        y[index] = 900.0F - static_cast<float>(index) * 0.25F;
    }
    std::array<Sora::Compute::CpuCodeBuffer, 4> buffers{};
    buffers[1] = {.data = Bytes(x.data()),
                  .sizeBytes = x.size() * sizeof(float),
                  .access = Sora::Compute::AccessMode::Read,
                  .elementType = kFloat,
                  .layout = Sora::Compute::LayoutKind::Contiguous,
                  .rank = 1,
                  .extents = {count},
                  .byteStrides = {sizeof(float)},
                  .componentStrideBytes = sizeof(float)};
    buffers[2] = buffers[1];
    buffers[2].data = Bytes(y.data());
    buffers[3] = buffers[1];
    buffers[3].data = Bytes(output.data());
    buffers[3].access = Sora::Compute::AccessMode::Write;
    std::array<Sora::Compute::CpuCodeScalar, 4> scalars{};
    scalars[0] = {.data = Bytes(&alpha), .sizeBytes = sizeof(alpha), .type = kFloat};
    const Sora::Compute::CpuCodeInvocation invocation{
        .buffers = buffers.data(),
        .scalars = scalars.data(),
        .argumentCount = buffers.size(),
        .rank = 1,
        .extents = {count},
        .begin = 0,
        .end = count,
    };
    Sora::Compute::CpuCodeInvocation single = invocation;
    single.end = 1;
    imported->Execute(single);
    REQUIRE(output.front() == std::fma(alpha, x.front(), y.front()));
    output.front() = 0.0F;
    imported->Execute(invocation);
    for (uint64_t index = 0; index < count; ++index) {
        REQUIRE(output[index] == std::fma(alpha, x[index], y[index]));
    }

    auto secondProvider = MakeProvider();
    auto crossProvider = secondProvider.provider.Import(request, *exported);
    REQUIRE(crossProvider.has_value());

    Sora::Compute::CpuCodeCompileRequest wrongKeyRequest = request;
    wrongKeyRequest.specialization.shape = {0x57524F4E475F4B45ULL, 0x595F544553545F31ULL};
    auto wrongKey = fixture.provider.Import(wrongKeyRequest, *exported);
    REQUIRE_FALSE(wrongKey.has_value());
    REQUIRE(wrongKey.error().code == Sora::Compute::CompileErrorCode::InvalidArtifact);

    auto truncated =
        fixture.provider.Import(request, std::span<const std::byte>{*exported}.first(exported->size() - 1));
    REQUIRE_FALSE(truncated.has_value());
    REQUIRE(truncated.error().code == Sora::Compute::CompileErrorCode::InvalidArtifact);

    exported->back() ^= std::byte{0x01};
    auto corrupted = fixture.provider.Import(request, *exported);
    REQUIRE_FALSE(corrupted.has_value());
    REQUIRE(corrupted.error().code == Sora::Compute::CompileErrorCode::InvalidArtifact);
}

TEST_CASE("LLVM CPU provider lowers Vec3 AoS and SoA through the same artifact interface",
          "[Sora][Compute][LLVM][Layout]") {
    constexpr uint64_t count = 31;
    auto fixture = MakeProvider();

    SECTION("AoS") {
        auto artifact = Compile(fixture, kVec3AosAdd);
        std::vector<Vec3Pod> x(count);
        std::vector<Vec3Pod> y(count);
        std::vector<Vec3Pod> output(count);
        for (uint64_t index = 0; index < count; ++index) {
            x[index] = {static_cast<float>(index), static_cast<float>(index + 1), static_cast<float>(index + 2)};
            y[index] = {10.0F, 20.0F, 30.0F};
        }
        std::array<Sora::Compute::CpuCodeBuffer, 3> buffers{};
        buffers[0] = {.data = Bytes(x.data()),
                      .sizeBytes = x.size() * sizeof(Vec3Pod),
                      .access = Sora::Compute::AccessMode::Read,
                      .elementType = kVec3,
                      .layout = Sora::Compute::LayoutKind::Contiguous,
                      .rank = 1,
                      .extents = {count},
                      .byteStrides = {sizeof(Vec3Pod)},
                      .componentStrideBytes = sizeof(float)};
        buffers[1] = buffers[0];
        buffers[1].data = Bytes(y.data());
        buffers[2] = buffers[0];
        buffers[2].data = Bytes(output.data());
        buffers[2].access = Sora::Compute::AccessMode::Write;
        const Sora::Compute::CpuCodeInvocation invocation{
            .buffers = buffers.data(), .argumentCount = buffers.size(), .rank = 1, .extents = {count}, .end = count};
        artifact.Execute(invocation);
        for (uint64_t index = 0; index < count; ++index) {
            REQUIRE(output[index].x == x[index].x + y[index].x);
            REQUIRE(output[index].y == x[index].y + y[index].y);
            REQUIRE(output[index].z == x[index].z + y[index].z);
        }
    }

    SECTION("SoA") {
        auto artifact = Compile(fixture, kVec3SoaAdd);
        std::vector<float> x(count * 3);
        std::vector<float> y(count * 3);
        std::vector<float> output(count * 3);
        for (uint64_t lane = 0; lane < 3; ++lane) {
            for (uint64_t index = 0; index < count; ++index) {
                x[lane * count + index] = static_cast<float>(lane * 100 + index);
                y[lane * count + index] = static_cast<float>(lane + 1);
            }
        }
        std::array<Sora::Compute::CpuCodeBuffer, 3> buffers{};
        buffers[0] = {.data = Bytes(x.data()),
                      .sizeBytes = x.size() * sizeof(float),
                      .access = Sora::Compute::AccessMode::Read,
                      .elementType = kVec3,
                      .layout = Sora::Compute::LayoutKind::StructureOfArrays,
                      .rank = 1,
                      .extents = {count},
                      .byteStrides = {sizeof(float)},
                      .componentStrideBytes = count * sizeof(float)};
        buffers[1] = buffers[0];
        buffers[1].data = Bytes(y.data());
        buffers[2] = buffers[0];
        buffers[2].data = Bytes(output.data());
        buffers[2].access = Sora::Compute::AccessMode::Write;
        const Sora::Compute::CpuCodeInvocation invocation{
            .buffers = buffers.data(), .argumentCount = buffers.size(), .rank = 1, .extents = {count}, .end = count};
        artifact.Execute(invocation);
        for (uint64_t lane = 0; lane < 3; ++lane) {
            for (uint64_t index = 0; index < count; ++index) {
                REQUIRE(output[lane * count + index] == x[lane * count + index] + y[lane * count + index]);
            }
        }
    }
}

TEST_CASE("LLVM CPU provider lowers two-dimensional logical indices", "[Sora][Compute][LLVM][Index]") {
    constexpr uint64_t rows = 7;
    constexpr uint64_t columns = 11;
    constexpr uint64_t count = rows * columns;
    auto fixture = MakeProvider();
    auto artifact = Compile(fixture, kIndex);
    std::vector<uint64_t> output(count);
    std::array<Sora::Compute::CpuCodeBuffer, 1> buffers{};
    buffers[0] = {.data = Bytes(output.data()),
                  .sizeBytes = output.size() * sizeof(uint64_t),
                  .access = Sora::Compute::AccessMode::Write,
                  .elementType = kUInt64,
                  .layout = Sora::Compute::LayoutKind::Contiguous,
                  .rank = 2,
                  .extents = {rows, columns},
                  .byteStrides = {columns * sizeof(uint64_t), sizeof(uint64_t)},
                  .componentStrideBytes = sizeof(uint64_t)};
    const Sora::Compute::CpuCodeInvocation invocation{
        .buffers = buffers.data(),
        .argumentCount = buffers.size(),
        .rank = 2,
        .extents = {rows, columns},
        .end = count,
    };
    artifact.Execute(invocation);
    for (uint64_t index = 0; index < count; ++index) {
        REQUIRE(output[index] == index % columns);
    }
}

TEST_CASE("LLVM CPU provider lowers Float64 math, strided buffers, and integer constants",
          "[Sora][Compute][LLVM][Types][Layout][Math]") {
    constexpr uint64_t count = 23;
    auto fixture = MakeProvider();

    SECTION("Float64 external math symbols") {
        auto artifact = Compile(fixture, kDoubleMath);
        std::vector<double> x(count);
        std::vector<double> y(count);
        std::vector<double> output(count);
        for (uint64_t index = 0; index < count; ++index) {
            x[index] = 0.01 * static_cast<double>(index + 1);
            y[index] = 0.5 + 0.02 * static_cast<double>(index);
        }
        std::array<Sora::Compute::CpuCodeBuffer, 3> buffers{};
        buffers[0] = {.data = Bytes(x.data()),
                      .sizeBytes = x.size() * sizeof(double),
                      .access = Sora::Compute::AccessMode::Read,
                      .elementType = kDouble,
                      .layout = Sora::Compute::LayoutKind::Contiguous,
                      .rank = 1,
                      .extents = {count},
                      .byteStrides = {sizeof(double)},
                      .componentStrideBytes = sizeof(double)};
        buffers[1] = buffers[0];
        buffers[1].data = Bytes(y.data());
        buffers[2] = buffers[0];
        buffers[2].data = Bytes(output.data());
        buffers[2].access = Sora::Compute::AccessMode::Write;
        const Sora::Compute::CpuCodeInvocation invocation{
            .buffers = buffers.data(), .argumentCount = buffers.size(), .rank = 1, .extents = {count}, .end = count};
        artifact.Execute(invocation);
        for (uint64_t index = 0; index < count; ++index) {
            const double expected = std::atan2(std::tan(x[index]), y[index]);
            REQUIRE(std::abs(output[index] - expected) <= 1.0e-13);
        }
    }

    SECTION("Float64 strided constant add") {
        constexpr uint64_t canary = 0xC0DEC0DEC0DEC0DEULL;
        auto artifact = Compile(fixture, kStridedDouble);
        std::vector<DoubleSlot> x(count);
        std::vector<DoubleSlot> output(count);
        for (uint64_t index = 0; index < count; ++index) {
            x[index] = {.value = static_cast<double>(index) * 0.5, .canary = canary};
            output[index].canary = canary;
        }
        std::array<Sora::Compute::CpuCodeBuffer, 2> buffers{};
        buffers[0] = {.data = Bytes(x.data()),
                      .sizeBytes = x.size() * sizeof(DoubleSlot),
                      .access = Sora::Compute::AccessMode::Read,
                      .elementType = kDouble,
                      .layout = Sora::Compute::LayoutKind::Strided,
                      .rank = 1,
                      .extents = {count},
                      .byteStrides = {sizeof(DoubleSlot)},
                      .componentStrideBytes = sizeof(double)};
        buffers[1] = buffers[0];
        buffers[1].data = Bytes(output.data());
        buffers[1].access = Sora::Compute::AccessMode::Write;
        const Sora::Compute::CpuCodeInvocation invocation{
            .buffers = buffers.data(), .argumentCount = buffers.size(), .rank = 1, .extents = {count}, .end = count};
        artifact.Execute(invocation);
        for (uint64_t index = 0; index < count; ++index) {
            REQUIRE(output[index].value == x[index].value + 1.25);
            REQUIRE(output[index].canary == canary);
        }
    }

    SECTION("signed integer constant") {
        auto artifact = Compile(fixture, kIntegerConstant);
        std::vector<int32_t> output(count);
        std::array<Sora::Compute::CpuCodeBuffer, 1> buffers{};
        buffers[0] = {.data = Bytes(output.data()),
                      .sizeBytes = output.size() * sizeof(int32_t),
                      .access = Sora::Compute::AccessMode::Write,
                      .elementType = kInt32,
                      .layout = Sora::Compute::LayoutKind::Contiguous,
                      .rank = 1,
                      .extents = {count},
                      .byteStrides = {sizeof(int32_t)},
                      .componentStrideBytes = sizeof(int32_t)};
        const Sora::Compute::CpuCodeInvocation invocation{
            .buffers = buffers.data(), .argumentCount = buffers.size(), .rank = 1, .extents = {count}, .end = count};
        artifact.Execute(invocation);
        for (int32_t value : output) {
            REQUIRE(value == -1'234'567);
        }
    }
}

TEST_CASE("LLVM CPU provider serializes same-key materialization and preserves code leases",
          "[Sora][Compute][LLVM][Concurrency][Lifetime]") {
    constexpr size_t workerCount = 8;
    constexpr size_t rounds = 8;
    auto fixture = MakeProvider();
    const auto programBytes = kSaxpy.Serialize();
    const Sora::Compute::CpuCodeCompileRequest request{
        .programData = programBytes.Bytes().data(),
        .programSize = programBytes.Bytes().size(),
        .programHash = kSaxpy.Hash(),
        .target = fixture.target,
    };
    auto seed = fixture.provider.Compile(request);
    REQUIRE(seed.has_value());
    auto exported = fixture.provider.Export(*seed);
    REQUIRE(exported.has_value());
    seed = Sora::Compute::CpuCodeArtifact{};

    std::barrier start{static_cast<std::ptrdiff_t>(workerCount)};
    std::array<bool, workerCount> succeeded{};
    succeeded.fill(true);
    std::array<std::jthread, workerCount> workers{};
    for (size_t worker = 0; worker < workerCount; ++worker) {
        workers[worker] = std::jthread([&, worker] {
            start.arrive_and_wait();
            for (size_t round = 0; round < rounds; ++round) {
                auto artifact = ((worker + round) & 1U) == 0 ? fixture.provider.Compile(request)
                                                             : fixture.provider.Import(request, *exported);
                if (!artifact) {
                    succeeded[worker] = false;
                    return;
                }
            }
        });
    }
    for (std::jthread& worker : workers) {
        worker.join();
    }
    for (bool result : succeeded) {
        REQUIRE(result);
    }

    auto survivor = fixture.provider.Import(request, *exported);
    REQUIRE(survivor.has_value());
    fixture.provider = {};
    constexpr float alpha = 2.0F;
    std::array<float, 1> x{3.0F};
    std::array<float, 1> y{4.0F};
    std::array<float, 1> output{};
    std::array<Sora::Compute::CpuCodeBuffer, 4> buffers{};
    buffers[1] = {.data = Bytes(x.data()),
                  .sizeBytes = sizeof(x),
                  .access = Sora::Compute::AccessMode::Read,
                  .elementType = kFloat,
                  .layout = Sora::Compute::LayoutKind::Contiguous,
                  .rank = 1,
                  .extents = {1},
                  .byteStrides = {sizeof(float)},
                  .componentStrideBytes = sizeof(float)};
    buffers[2] = buffers[1];
    buffers[2].data = Bytes(y.data());
    buffers[3] = buffers[1];
    buffers[3].data = Bytes(output.data());
    buffers[3].access = Sora::Compute::AccessMode::Write;
    std::array<Sora::Compute::CpuCodeScalar, 4> scalars{};
    scalars[0] = {.data = Bytes(&alpha), .sizeBytes = sizeof(alpha), .type = kFloat};
    const Sora::Compute::CpuCodeInvocation invocation{.buffers = buffers.data(),
                                                      .scalars = scalars.data(),
                                                      .argumentCount = buffers.size(),
                                                      .rank = 1,
                                                      .extents = {1},
                                                      .end = 1};
    survivor->Execute(invocation);
    REQUIRE(output[0] == std::fma(alpha, x[0], y[0]));
}

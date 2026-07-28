#include <Sora/Core/PAL/CpuInfo.h>

#include <catch2/catch_test_macros.hpp>

#include <climits>
#include <cstdint>

namespace {

    using Sora::PAL::CpuFeature;
    using Sora::PAL::CpuInfo;

    constexpr CpuInfo kAvx2Cpu{.features = CpuFeature::X86Sse2 | CpuFeature::X86Avx | CpuFeature::X86Avx2};
    constexpr CpuInfo kSveCpu{.features = CpuFeature::ArmSve, .currentScalableVectorBits = 256};

    static_assert(kAvx2Cpu.MaximumFixedVectorBits() == 256);
    static_assert(!kAvx2Cpu.SupportsFixedVectorBits(0));
    static_assert(kAvx2Cpu.SupportsFixedVectorBits(128));
    static_assert(kAvx2Cpu.Supports(CpuFeature::X86Avx | CpuFeature::X86Avx2));
    static_assert(kSveCpu.MaximumFixedVectorBits() == 0);
    static_assert(kSveCpu.SupportsScalableVectors());
    static_assert(Sora::IsValidFlagSet(Sora::PAL::kCompileTargetCpuFeatures));

} // namespace

TEST_CASE("CPU discovery reports a stable dispatch-safe architecture identity", "[Sora][PAL][CpuInfo]") {
    const CpuInfo first = Sora::PAL::DetectHostCpuInfo();
    const CpuInfo second = Sora::PAL::DetectHostCpuInfo();

    REQUIRE(first == second);
    REQUIRE(first.ArchitectureId() != uint64_t{0});
    REQUIRE(first.architecture == Sora::kCpuArchitecture);
    REQUIRE(first.byteOrder == Sora::kByteOrder);
    REQUIRE(first.addressBits == sizeof(void*) * CHAR_BIT);
    REQUIRE(first.logicalProcessorCount >= 1);
    REQUIRE(Sora::IsValidFlagSet(first.features));
    REQUIRE(first.Supports(Sora::PAL::kCompileTargetCpuFeatures));

    CpuInfo schedulingVariant = first;
    ++schedulingVariant.logicalProcessorCount;
    schedulingVariant.currentScalableVectorBits ^= uint32_t{128};
    REQUIRE(schedulingVariant.ArchitectureId() == first.ArchitectureId());

    CpuInfo featureVariant = first;
    featureVariant.features ^= CpuFeature::WasmRelaxedSimd;
    REQUIRE(featureVariant.ArchitectureId() != first.ArchitectureId());

    if (first.currentScalableVectorBits != 0) {
        REQUIRE(first.SupportsScalableVectors());
        REQUIRE(first.currentScalableVectorBits >= 128);
        REQUIRE(first.currentScalableVectorBits % 128 == 0);
    }
    if (first.Supports(CpuFeature::X86Sse2)) {
        REQUIRE(first.SupportsFixedVectorBits(128));
    }
    if (first.Supports(CpuFeature::X86Avx)) {
        REQUIRE(first.SupportsFixedVectorBits(256));
    }
    if (first.Supports(CpuFeature::X86Avx2)) {
        REQUIRE(first.Supports(CpuFeature::X86Avx));
    }
    if (first.Supports(CpuFeature::X86Avx512F)) {
        REQUIRE(first.Supports(CpuFeature::X86Avx));
        REQUIRE(first.SupportsFixedVectorBits(512));
    }
    if (first.Supports(CpuFeature::Fma) &&
        (first.architecture == Sora::CpuArchitecture::X86 || first.architecture == Sora::CpuArchitecture::X64)) {
        REQUIRE(first.Supports(CpuFeature::X86Avx));
    }
    if (first.Supports(CpuFeature::ArmNeon)) {
        REQUIRE(first.Supports(CpuFeature::Fma));
        REQUIRE(first.SupportsFixedVectorBits(128));
    }
    if (first.Supports(CpuFeature::ArmSve2)) {
        REQUIRE(first.Supports(CpuFeature::ArmSve));
    }
    if (first.Supports(CpuFeature::PowerPcVsx)) {
        REQUIRE(first.Supports(CpuFeature::PowerPcAltivec));
    }
    if (first.Supports(CpuFeature::LoongArchLasx)) {
        REQUIRE(first.Supports(CpuFeature::LoongArchLsx));
    }
    if (first.Supports(CpuFeature::WasmRelaxedSimd)) {
        REQUIRE(first.Supports(CpuFeature::WasmSimd128));
    }
    if (first.architecture == Sora::CpuArchitecture::X86 || first.architecture == Sora::CpuArchitecture::X64) {
        REQUIRE(first.x86.vendor.back() == '\0');
    }
}

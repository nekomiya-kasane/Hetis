/**
 * @file CpuInfo.cpp
 * @brief Process-usable host CPU capability discovery.
 * @ingroup Core
 */

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <thread>

#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86))
#    include <intrin.h>
#elif (defined(__x86_64__) || defined(__i386__)) && (defined(__clang__) || defined(__GNUC__))
#    include <cpuid.h>
#endif

#if defined(_WIN32) && (defined(__aarch64__) || defined(_M_ARM64))
#    define WIN32_LEAN_AND_MEAN
#    define NOMINMAX
#    include <Windows.h>
#endif

#if defined(__APPLE__) && defined(__aarch64__)
#    include <sys/sysctl.h>
#endif

#if defined(__linux__) && (defined(__aarch64__) || defined(__riscv))
#    include <asm/hwcap.h>
#    include <sys/auxv.h>
#endif

#if defined(__linux__) && defined(__aarch64__)
#    include <sys/prctl.h>
#endif

#if defined(__linux__) && defined(__riscv) && __has_include(<asm/hwprobe.h>)
#    include <asm/hwprobe.h>
#    include <sys/syscall.h>
#    include <unistd.h>
#    define SORA_HAS_RISCV_HWPROBE 1
#endif

#include <Sora/Core/PAL/CpuInfo.h>

namespace Sora::PAL {

    namespace {

        struct CpuRegisters {
            uint32_t eax = 0;
            uint32_t ebx = 0;
            uint32_t ecx = 0;
            uint32_t edx = 0;
        };

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
        [[nodiscard]] CpuRegisters ReadCpuid(uint32_t leaf, uint32_t subleaf = 0) noexcept {
#    if defined(_WIN32)
            int registers[4]{};
            __cpuidex(registers, static_cast<int>(leaf), static_cast<int>(subleaf));
            return {.eax = static_cast<uint32_t>(registers[0]),
                    .ebx = static_cast<uint32_t>(registers[1]),
                    .ecx = static_cast<uint32_t>(registers[2]),
                    .edx = static_cast<uint32_t>(registers[3])};
#    else
            CpuRegisters registers{};
            __cpuid_count(leaf, subleaf, registers.eax, registers.ebx, registers.ecx, registers.edx);
            return registers;
#    endif
        }

        [[nodiscard]] uint64_t ReadXcr0() noexcept {
#    if defined(_WIN32)
            return _xgetbv(0);
#    else
            uint32_t low = 0;
            uint32_t high = 0;
            __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
            return (static_cast<uint64_t>(high) << 32U) | low;
#    endif
        }

        void DetectX86(CpuInfo& info) noexcept {
            info.features = CpuFeature::None;
            const CpuRegisters leaf0 = ReadCpuid(0);
            const uint32_t maximumLeaf = leaf0.eax;
            std::memcpy(info.x86.vendor.data(), &leaf0.ebx, sizeof(leaf0.ebx));
            std::memcpy(info.x86.vendor.data() + 4, &leaf0.edx, sizeof(leaf0.edx));
            std::memcpy(info.x86.vendor.data() + 8, &leaf0.ecx, sizeof(leaf0.ecx));
            if (maximumLeaf < 1) {
                return;
            }

            const CpuRegisters leaf1 = ReadCpuid(1);
            const uint32_t baseFamily = (leaf1.eax >> 8U) & 0xFU;
            const uint32_t baseModel = (leaf1.eax >> 4U) & 0xFU;
            const uint32_t extendedFamily = (leaf1.eax >> 20U) & 0xFFU;
            const uint32_t extendedModel = (leaf1.eax >> 16U) & 0xFU;
            info.x86.family = baseFamily == 0xFU ? baseFamily + extendedFamily : baseFamily;
            info.x86.model = baseFamily == 0x6U || baseFamily == 0xFU ? baseModel | (extendedModel << 4U) : baseModel;
            info.x86.stepping = leaf1.eax & 0xFU;

            if ((leaf1.edx & (uint32_t{1} << 26U)) != 0) {
                info.features |= CpuFeature::X86Sse2;
            }

            const bool osXsave = (leaf1.ecx & (uint32_t{1} << 27U)) != 0;
            const bool hardwareAvx = (leaf1.ecx & (uint32_t{1} << 28U)) != 0;
            const uint64_t xcr0 = osXsave ? ReadXcr0() : 0;
            const bool avxState = hardwareAvx && (xcr0 & uint64_t{0x6}) == uint64_t{0x6};
            if (!avxState) {
                return;
            }

            info.features |= CpuFeature::X86Avx;
            if ((leaf1.ecx & (uint32_t{1} << 12U)) != 0) {
                info.features |= CpuFeature::Fma;
            }
            if (maximumLeaf < 7) {
                return;
            }

            const CpuRegisters leaf7 = ReadCpuid(7);
            if ((leaf7.ebx & (uint32_t{1} << 5U)) != 0) {
                info.features |= CpuFeature::X86Avx2;
            }
            constexpr uint64_t kAvx512StateMask = 0xE6;
            const bool avx512State = (xcr0 & kAvx512StateMask) == kAvx512StateMask;
            if (avx512State && (leaf7.ebx & (uint32_t{1} << 16U)) != 0) {
                info.features |= CpuFeature::X86Avx512F;
            }
        }
#endif

#if defined(__linux__) && defined(__aarch64__)
        [[nodiscard]] uint32_t CurrentSveVectorBits() noexcept {
#    if defined(PR_SVE_GET_VL) && defined(PR_SVE_VL_LEN_MASK)
            const int configuration = prctl(PR_SVE_GET_VL);
            if (configuration >= 0) {
                return static_cast<uint32_t>(configuration & PR_SVE_VL_LEN_MASK) * CHAR_BIT;
            }
#    endif
            return 0;
        }
#endif

#if defined(__APPLE__) && defined(__aarch64__)
        [[nodiscard]] bool AppleCpuFeature(const char* name) noexcept {
            int enabled = 0;
            size_t size = sizeof(enabled);
            return sysctlbyname(name, &enabled, &size, nullptr, 0) == 0 && size == sizeof(enabled) && enabled != 0;
        }
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
        void DetectArm64(CpuInfo& info) noexcept {
            bool neon = false;
            bool fma = false;
            bool sve = false;
            bool sve2 = false;
#    if defined(__linux__)
            const unsigned long capabilities = getauxval(AT_HWCAP);
#        if defined(HWCAP_ASIMD)
            neon = (capabilities & HWCAP_ASIMD) != 0;
#        endif
#        if defined(HWCAP_FP)
            fma = (capabilities & HWCAP_FP) != 0;
#        endif
#        if defined(HWCAP_SVE)
            sve = (capabilities & HWCAP_SVE) != 0;
#        endif
#        if defined(AT_HWCAP2) && defined(HWCAP2_SVE2)
            sve2 = (getauxval(AT_HWCAP2) & HWCAP2_SVE2) != 0;
#        endif
#    elif defined(_WIN32)
#        if defined(PF_ARM_NEON_INSTRUCTIONS_AVAILABLE)
            neon = IsProcessorFeaturePresent(PF_ARM_NEON_INSTRUCTIONS_AVAILABLE) != FALSE;
#        endif
            fma = neon;
#    elif defined(__APPLE__)
            neon = true;
            fma = true;
            sve = AppleCpuFeature("hw.optional.arm.FEAT_SVE");
            sve2 = AppleCpuFeature("hw.optional.arm.FEAT_SVE2");
#    else
            constexpr CpuFeature compiled = kCompileTargetCpuFeatures;
            neon = HasAny(compiled, CpuFeature::ArmNeon);
            fma = HasAny(compiled, CpuFeature::Fma);
            sve = HasAny(compiled, CpuFeature::ArmSve);
            sve2 = HasAny(compiled, CpuFeature::ArmSve2);
#    endif
            if (neon) {
                info.features |= CpuFeature::ArmNeon;
            }
            if (fma || neon) {
                info.features |= CpuFeature::Fma;
            }
            if (sve) {
                info.features |= CpuFeature::ArmSve;
#    if defined(__linux__)
                info.currentScalableVectorBits = CurrentSveVectorBits();
#    endif
            }
            if (sve2) {
                info.features |= CpuFeature::ArmSve | CpuFeature::ArmSve2;
            }
        }
#endif

#if defined(__riscv)
        void DetectRiscV(CpuInfo& info) noexcept {
            info.features |= kCompileTargetCpuFeatures;
#    if defined(SORA_HAS_RISCV_HWPROBE) && defined(__NR_riscv_hwprobe) && defined(RISCV_HWPROBE_KEY_IMA_EXT_0)
            riscv_hwprobe extensions{.key = RISCV_HWPROBE_KEY_IMA_EXT_0};
            if (syscall(__NR_riscv_hwprobe, &extensions, size_t{1}, size_t{0}, nullptr, uint32_t{0}) == 0 &&
                extensions.key >= 0) {
#        if defined(RISCV_HWPROBE_IMA_FD)
                if ((extensions.value & RISCV_HWPROBE_IMA_FD) != 0) {
                    info.features |= CpuFeature::Fma;
                }
#        endif
#        if defined(RISCV_HWPROBE_IMA_V)
                if ((extensions.value & RISCV_HWPROBE_IMA_V) != 0) {
                    info.features |= CpuFeature::RiscVVector;
                }
#        endif
#        if defined(RISCV_HWPROBE_EXT_ZVE32X)
                if ((extensions.value & RISCV_HWPROBE_EXT_ZVE32X) != 0) {
                    info.features |= CpuFeature::RiscVVector;
                }
#        endif
            }
#    endif
        }
#endif

    } // namespace

    CpuInfo DetectHostCpuInfo() noexcept {
        CpuInfo info{.logicalProcessorCount = std::max(std::thread::hardware_concurrency(), uint32_t{1})};

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
        DetectX86(info);
#elif defined(__aarch64__) || defined(_M_ARM64)
        DetectArm64(info);
#elif defined(__riscv)
        DetectRiscV(info);
#else
        info.features = kCompileTargetCpuFeatures;
#endif
        return info;
    }

} // namespace Sora::PAL

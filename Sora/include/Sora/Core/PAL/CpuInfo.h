/**
 * @file CpuInfo.h
 * @brief Process-usable host CPU capabilities and stable code-generation
 * fingerprints.
 * @ingroup Core
 */
#pragma once

#include "Sora/Core/Flags.h"
#include "Sora/Core/Hash.h"
#include "Sora/Platform.h"

#include <array>
#include <climits>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace Sora::PAL {

    /** @brief Instruction capabilities that the current process may execute safely.
     */
    enum class CpuFeature : uint64_t {
        None = 0,
        Fma = uint64_t{1} << 0,
        X86Sse2 = uint64_t{1} << 1,
        X86Avx = uint64_t{1} << 2,
        X86Avx2 = uint64_t{1} << 3,
        X86Avx512F = uint64_t{1} << 4,
        ArmNeon = uint64_t{1} << 5,
        ArmSve = uint64_t{1} << 6,
        ArmSve2 = uint64_t{1} << 7,
        RiscVVector = uint64_t{1} << 8,
        PowerPcAltivec = uint64_t{1} << 9,
        PowerPcVsx = uint64_t{1} << 10,
        LoongArchLsx = uint64_t{1} << 11,
        LoongArchLasx = uint64_t{1} << 12,
        S390Vector = uint64_t{1} << 13,
        WasmSimd128 = uint64_t{1} << 14,
        WasmRelaxedSimd = uint64_t{1} << 15,
    };

    static_assert(Concept::BitfieldEnum<CpuFeature>);

    /**
     * @brief Return ISA features guaranteed by the translation target.
     *
     * @details These are compile-time facts, not a runtime host probe. They are
     * authoritative for WebAssembly modules, whose target features are fixed at
     * validation/instantiation time, and conservative fallbacks for targets without
     * a runtime probing ABI.
     */
    inline constexpr CpuFeature kCompileTargetCpuFeatures = [] consteval {
        CpuFeature features = CpuFeature::None;
#if defined(__FMA__)
        features |= CpuFeature::Fma;
#endif
#if defined(__SSE2__) || defined(_M_X64)
        features |= CpuFeature::X86Sse2;
#endif
#if defined(__AVX__)
        features |= CpuFeature::X86Avx;
#endif
#if defined(__AVX2__)
        features |= CpuFeature::X86Avx | CpuFeature::X86Avx2;
#endif
#if defined(__AVX512F__)
        features |= CpuFeature::X86Avx | CpuFeature::X86Avx512F;
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
        features |= CpuFeature::ArmNeon;
#endif
#if defined(__ARM_FEATURE_FMA) || defined(__aarch64__) || defined(_M_ARM64)
        features |= CpuFeature::Fma;
#endif
#if defined(__ARM_FEATURE_SVE)
        features |= CpuFeature::ArmSve;
#endif
#if defined(__ARM_FEATURE_SVE2)
        features |= CpuFeature::ArmSve | CpuFeature::ArmSve2;
#endif
#if defined(__riscv_flen)
        features |= CpuFeature::Fma;
#endif
#if defined(__riscv_vector)
        features |= CpuFeature::RiscVVector;
#endif
#if defined(__ALTIVEC__)
        features |= CpuFeature::PowerPcAltivec;
#endif
#if defined(__VSX__)
        features |= CpuFeature::PowerPcAltivec | CpuFeature::PowerPcVsx;
#endif
#if defined(__loongarch_sx)
        features |= CpuFeature::LoongArchLsx;
#endif
#if defined(__loongarch_asx)
        features |= CpuFeature::LoongArchLsx | CpuFeature::LoongArchLasx;
#endif
#if defined(__VEC__) && defined(__s390x__)
        features |= CpuFeature::S390Vector;
#endif
#if defined(__wasm_simd128__)
        features |= CpuFeature::WasmSimd128;
#endif
#if defined(__wasm_relaxed_simd__)
        features |= CpuFeature::WasmSimd128 | CpuFeature::WasmRelaxedSimd;
#endif
        return features;
    }();

    /** @brief x86 CPUID identity. Zero values denote unavailable or non-x86
     * identity data. */
    struct X86CpuIdentity {
        std::array<char, 13> vendor{};
        uint32_t family = 0;
        uint32_t model = 0;
        uint32_t stepping = 0;

        friend constexpr bool operator==(const X86CpuIdentity&, const X86CpuIdentity&) noexcept = default;
    };

    /**
     * @brief Immutable host capabilities visible to the current process and calling
     * thread.
     *
     * @details @ref features includes operating-system execution-state gates. @ref
     * currentScalableVectorBits is thread-local state on architectures such as Arm
     * SVE and is deliberately excluded from @ref ArchitectureId.
     */
    struct CpuInfo {
        CpuArchitecture architecture = kCpuArchitecture;
        ByteOrder byteOrder = kByteOrder;
        uint8_t addressBits = static_cast<uint8_t>(sizeof(void*) * CHAR_BIT);
        CpuFeature features = CpuFeature::None;
        uint32_t logicalProcessorCount = 1;
        uint32_t currentScalableVectorBits = 0;
        X86CpuIdentity x86{};

        /** @brief Fingerprint schema version; increment whenever the canonical byte
         * sequence changes. */
        inline static constexpr uint32_t kArchitectureIdSchemaVersion = 2;

        /** @brief Return whether every capability in @p required is executable by the
         * current process. */
        [[nodiscard]] constexpr bool Supports(CpuFeature required) const noexcept { return HasAll(features, required); }

        /** @brief Return the widest fixed-size SIMD register supported by the
         * executable feature set. */
        [[nodiscard]] constexpr uint16_t MaximumFixedVectorBits() const noexcept {
            if (HasAny(features, CpuFeature::X86Avx512F)) {
                return 512;
            }
            if (HasAny(features, CpuFeature::X86Avx | CpuFeature::X86Avx2 | CpuFeature::LoongArchLasx)) {
                return 256;
            }
            constexpr CpuFeature kFixed128 = CpuFeature::X86Sse2 | CpuFeature::ArmNeon | CpuFeature::PowerPcAltivec |
                                             CpuFeature::PowerPcVsx | CpuFeature::LoongArchLsx |
                                             CpuFeature::S390Vector | CpuFeature::WasmSimd128;
            return HasAny(features, kFixed128) ? uint16_t{128} : uint16_t{0};
        }

        /** @brief Return whether fixed-width SIMD code requiring @p bits can execute.
         */
        [[nodiscard]] constexpr bool SupportsFixedVectorBits(uint16_t bits) const noexcept {
            return bits != 0 && bits <= MaximumFixedVectorBits();
        }

        /** @brief Return whether the ISA exposes a scalable-vector execution model.
         */
        [[nodiscard]] constexpr bool SupportsScalableVectors() const noexcept {
            return HasAny(features, CpuFeature::ArmSve | CpuFeature::RiscVVector);
        }

        /**
         * @brief Build a stable JIT/cache key from architecture, ABI,
         * microarchitecture, and executable ISA state.
         *
         * @details The canonical stream is little-endian and excludes processor count
         * and per-thread scalable-vector length. It therefore remains stable across
         * affinity changes, hot-plug events, and SVE vector-length changes.
         */
        [[nodiscard]] constexpr uint64_t ArchitectureId() const noexcept {
            auto hash = Hashing::Fnv1a64State::Seed();
            const auto append = [&hash]<std::unsigned_integral T>(T value) constexpr {
                uint64_t canonical = value;
                for (size_t byte = 0; byte < sizeof(T); ++byte) {
                    hash.FeedByte(static_cast<std::byte>(static_cast<uint8_t>(canonical)));
                    canonical >>= CHAR_BIT;
                }
            };

            append(kArchitectureIdSchemaVersion);
            append(std::to_underlying(architecture));
            append(std::to_underlying(byteOrder));
            append(addressBits);
            append(std::to_underlying(features));
            for (const char character : x86.vendor) {
                append(static_cast<uint8_t>(character));
            }
            append(x86.family);
            append(x86.model);
            append(x86.stepping);
            return hash.Finalize();
        }

        friend constexpr bool operator==(const CpuInfo&, const CpuInfo&) noexcept = default;
    };

    /** @brief Discover CPU capabilities executable by the current process and
     * calling thread. */
    [[nodiscard]] CpuInfo DetectHostCpuInfo() noexcept;

} // namespace Sora::PAL

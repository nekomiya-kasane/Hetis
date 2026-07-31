/**
 * @file Artifact.h
 * @brief Target identity and shared ownership for backend compute artifacts.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Backend/Abi.h>
#include <Sora/Compute/Backend/Service.h>
#include <Sora/Compute/Device.h>
#include <Sora/Platform.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace Sora::Compute {

    /** @brief Stage represented by a cached backend artifact. */
    enum class ArtifactKind : uint8_t {
        PortableLowering,
        DeviceExecutable,
        DevicePipeline,
    };

    /** @brief Return whether @p kind denotes one defined artifact stage. */
    [[nodiscard]] constexpr bool ValidArtifactKind(ArtifactKind kind) noexcept {
        return kind == ArtifactKind::PortableLowering || kind == ArtifactKind::DeviceExecutable ||
               kind == ArtifactKind::DevicePipeline;
    }

    /** @brief Actual or forecast path by which one exact artifact becomes a live backend handle. */
    enum class ArtifactResolutionPath : uint8_t {
        Live,
        PackageImport,
        PersistentImport,
        PersistentProduction,
        TransientProduction,
    };

    inline constexpr size_t kArtifactResolutionPathCount = 5;

    /** @brief Return whether @p path denotes one complete artifact-resolution path. */
    [[nodiscard]] constexpr bool ValidArtifactResolutionPath(ArtifactResolutionPath path) noexcept {
        return path >= ArtifactResolutionPath::Live && path <= ArtifactResolutionPath::TransientProduction;
    }

    /** @brief Return whether @p path imports backend-private bytes. */
    [[nodiscard]] constexpr bool IsArtifactImportPath(ArtifactResolutionPath path) noexcept {
        return path == ArtifactResolutionPath::PackageImport || path == ArtifactResolutionPath::PersistentImport;
    }

    /** @brief Return whether @p path invokes a stage-specific producer. */
    [[nodiscard]] constexpr bool IsArtifactProductionPath(ArtifactResolutionPath path) noexcept {
        return path == ArtifactResolutionPath::PersistentProduction ||
               path == ArtifactResolutionPath::TransientProduction;
    }

    /** @brief Lifetime over which backend discovery proves a target identity safe
     * for artifact reuse. */
    enum class TargetIdentityStability : uint8_t {
        Ephemeral,
        ProcessStable,
        MachineStable,
    };

    /** @brief Target dimensions on which compiler-produced executable bytes depend. */
    enum class ExecutableTargetDomain : uint8_t {
        BackendPortable = 0,
        DeviceArchitecture = 1,
        HostPlatform = 2,
        TargetNative = 3,
    };

    /** @brief Return whether @p domain denotes one supported executable compatibility domain. */
    [[nodiscard]] constexpr bool ValidExecutableTargetDomain(ExecutableTargetDomain domain) noexcept {
        return domain >= ExecutableTargetDomain::BackendPortable && domain <= ExecutableTargetDomain::TargetNative;
    }

    /** @brief Return whether executable bytes depend on device architecture and feature facts. */
    [[nodiscard]] constexpr bool UsesDeviceArchitecture(ExecutableTargetDomain domain) noexcept {
        return (std::to_underlying(domain) & std::to_underlying(ExecutableTargetDomain::DeviceArchitecture)) != 0;
    }

    /** @brief Return whether executable bytes depend on host object-format and ABI facts. */
    [[nodiscard]] constexpr bool UsesHostPlatform(ExecutableTargetDomain domain) noexcept {
        return (std::to_underlying(domain) & std::to_underlying(ExecutableTargetDomain::HostPlatform)) != 0;
    }

    inline constexpr SemanticHash kComputeRuntimeIdentity{0x534F52415F52554EULL, 0x54494D455F303031ULL};

    /** @brief Return whether @p value denotes a nonzero semantic identity. */
    [[nodiscard]] constexpr bool ValidSemanticHash(SemanticHash value) noexcept {
        return value.low != 0 || value.high != 0;
    }

    /** @brief Immutable composed compilation target projected into stage-specific compatibility identities. */
    struct TargetFingerprint {
        DeviceId device{};
        BackendKind backend = BackendKind::Cpu;
        CpuArchitecture hostArchitecture = CpuArchitecture::Unknown;
        OperatingSystem operatingSystem = OperatingSystem::Unknown;
        BinaryFormat binaryFormat = BinaryFormat::Unknown;
        ByteOrder byteOrder = ByteOrder::Unknown;
        AbiFamily abi = AbiFamily::Unknown;
        TargetIdentityStability identityStability = TargetIdentityStability::Ephemeral;
        ExecutableTargetDomain executableDomain = ExecutableTargetDomain::TargetNative;
        uint8_t addressBits = 0;
        DeviceFeatureMask features{};
        uint64_t architectureId = 0;
        uint32_t fixedVectorBits = 0;
        uint32_t scalableVectorBits = 0;
        uint32_t backendAbiVersion = kComputeBackendAbiVersion;
        SemanticHash runtimeIdentity = kComputeRuntimeIdentity;
        SemanticHash compilerIdentity{};
        SemanticHash driverIdentity{};

        /** @brief Return whether all executable-compatibility facts form one complete
         * runtime target. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            const bool validHostArchitecture =
                hostArchitecture >= CpuArchitecture::X86 && hostArchitecture <= CpuArchitecture::Wasm64;
            const bool validOperatingSystem =
                operatingSystem >= OperatingSystem::Windows && operatingSystem <= OperatingSystem::Emscripten;
            const bool validBinaryFormat = binaryFormat >= BinaryFormat::PE && binaryFormat <= BinaryFormat::Wasm;
            const bool validByteOrder =
                byteOrder == ByteOrder::Little || byteOrder == ByteOrder::Big || byteOrder == ByteOrder::Mixed;
            const bool validAbi = abi >= AbiFamily::MSVC && abi <= AbiFamily::WASI;
            const bool validStability = identityStability == TargetIdentityStability::Ephemeral ||
                                        identityStability == TargetIdentityStability::ProcessStable ||
                                        identityStability == TargetIdentityStability::MachineStable;
            return device.Valid() && ValidBackendKind(backend) && validHostArchitecture && validOperatingSystem &&
                   validBinaryFormat && validByteOrder && validAbi && validStability &&
                   ValidExecutableTargetDomain(executableDomain) && (addressBits == 32 || addressBits == 64) &&
                   architectureId != 0 && backendAbiVersion == kComputeBackendAbiVersion &&
                   ValidSemanticHash(runtimeIdentity) && ValidSemanticHash(compilerIdentity) &&
                   ValidSemanticHash(driverIdentity);
        }

        /** @brief Return whether this fingerprint is complete enough for persistent
         * native-artifact reuse. */
        [[nodiscard]] constexpr bool PersistentCacheable() const noexcept {
            const bool byteOrderKnown = byteOrder == ByteOrder::Little || byteOrder == ByteOrder::Big;
            return Valid() && identityStability == TargetIdentityStability::MachineStable && byteOrderKnown;
        }

        friend constexpr bool operator==(TargetFingerprint, TargetFingerprint) noexcept = default;
    };

    /** @brief Compiler-independent physical and runtime facts discovered for one execution target. */
    struct RuntimeTargetFacts {
        DeviceId device{};
        BackendKind backend = BackendKind::Cpu;
        CpuArchitecture hostArchitecture = CpuArchitecture::Unknown;
        OperatingSystem operatingSystem = OperatingSystem::Unknown;
        BinaryFormat binaryFormat = BinaryFormat::Unknown;
        ByteOrder byteOrder = ByteOrder::Unknown;
        AbiFamily abi = AbiFamily::Unknown;
        TargetIdentityStability identityStability = TargetIdentityStability::Ephemeral;
        uint8_t addressBits = 0;
        DeviceFeatureMask features{};
        uint64_t architectureId = 0;
        uint32_t fixedVectorBits = 0;
        uint32_t scalableVectorBits = 0;
        uint32_t backendAbiVersion = kComputeBackendAbiVersion;
        SemanticHash runtimeIdentity = kComputeRuntimeIdentity;
        SemanticHash driverIdentity{};

        /** @brief Return whether all compiler-independent runtime target facts are complete. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            const bool validHostArchitecture =
                hostArchitecture >= CpuArchitecture::X86 && hostArchitecture <= CpuArchitecture::Wasm64;
            const bool validOperatingSystem =
                operatingSystem >= OperatingSystem::Windows && operatingSystem <= OperatingSystem::Emscripten;
            const bool validBinaryFormat = binaryFormat >= BinaryFormat::PE && binaryFormat <= BinaryFormat::Wasm;
            const bool validByteOrder =
                byteOrder == ByteOrder::Little || byteOrder == ByteOrder::Big || byteOrder == ByteOrder::Mixed;
            const bool validAbi = abi >= AbiFamily::MSVC && abi <= AbiFamily::WASI;
            const bool validStability = identityStability == TargetIdentityStability::Ephemeral ||
                                        identityStability == TargetIdentityStability::ProcessStable ||
                                        identityStability == TargetIdentityStability::MachineStable;
            return device.Valid() && ValidBackendKind(backend) && validHostArchitecture && validOperatingSystem &&
                   validBinaryFormat && validByteOrder && validAbi && validStability &&
                   (addressBits == 32 || addressBits == 64) && architectureId != 0 &&
                   backendAbiVersion == kComputeBackendAbiVersion && ValidSemanticHash(runtimeIdentity) &&
                   ValidSemanticHash(driverIdentity);
        }

        friend constexpr bool operator==(RuntimeTargetFacts, RuntimeTargetFacts) noexcept = default;
    };

    /** @brief Forget compiler selection while preserving all discovered physical and runtime facts. */
    [[nodiscard]] constexpr RuntimeTargetFacts RuntimeTarget(const TargetFingerprint& target) noexcept {
        return {.device = target.device,
                .backend = target.backend,
                .hostArchitecture = target.hostArchitecture,
                .operatingSystem = target.operatingSystem,
                .binaryFormat = target.binaryFormat,
                .byteOrder = target.byteOrder,
                .abi = target.abi,
                .identityStability = target.identityStability,
                .addressBits = target.addressBits,
                .features = target.features,
                .architectureId = target.architectureId,
                .fixedVectorBits = target.fixedVectorBits,
                .scalableVectorBits = target.scalableVectorBits,
                .backendAbiVersion = target.backendAbiVersion,
                .runtimeIdentity = target.runtimeIdentity,
                .driverIdentity = target.driverIdentity};
    }

    /** @brief Compose discovered runtime facts with one exact compiler-output profile. */
    [[nodiscard]] constexpr TargetFingerprint ComposeTarget(const RuntimeTargetFacts& target,
                                                            SemanticHash compilerIdentity,
                                                            ExecutableTargetDomain executableDomain) noexcept {
        return {.device = target.device,
                .backend = target.backend,
                .hostArchitecture = target.hostArchitecture,
                .operatingSystem = target.operatingSystem,
                .binaryFormat = target.binaryFormat,
                .byteOrder = target.byteOrder,
                .abi = target.abi,
                .identityStability = target.identityStability,
                .executableDomain = executableDomain,
                .addressBits = target.addressBits,
                .features = target.features,
                .architectureId = target.architectureId,
                .fixedVectorBits = target.fixedVectorBits,
                .scalableVectorBits = target.scalableVectorBits,
                .backendAbiVersion = target.backendAbiVersion,
                .runtimeIdentity = target.runtimeIdentity,
                .compilerIdentity = compilerIdentity,
                .driverIdentity = target.driverIdentity};
    }

    /** @brief Canonical compatibility domain of compiler-produced executable bytes. */
    struct ExecutableTargetIdentity {
        BackendKind backend = BackendKind::Cpu;
        ExecutableTargetDomain executableDomain = ExecutableTargetDomain::TargetNative;
        TargetIdentityStability identityStability = TargetIdentityStability::Ephemeral;
        CpuArchitecture hostArchitecture = CpuArchitecture::Unknown;
        OperatingSystem operatingSystem = OperatingSystem::Unknown;
        BinaryFormat binaryFormat = BinaryFormat::Unknown;
        ByteOrder byteOrder = ByteOrder::Unknown;
        AbiFamily abi = AbiFamily::Unknown;
        uint8_t addressBits = 0;
        DeviceFeatureMask features{};
        uint64_t architectureId = 0;
        uint32_t fixedVectorBits = 0;
        uint32_t scalableVectorBits = 0;
        uint32_t backendAbiVersion = kComputeBackendAbiVersion;
        SemanticHash runtimeIdentity = kComputeRuntimeIdentity;
        SemanticHash compilerIdentity{};

        constexpr ExecutableTargetIdentity() noexcept = default;

        /** @brief Project runtime target facts onto executable-byte compatibility. */
        constexpr ExecutableTargetIdentity(const TargetFingerprint& target) noexcept
            : backend(target.backend),
              executableDomain(target.executableDomain),
              identityStability(target.identityStability),
              backendAbiVersion(target.backendAbiVersion),
              runtimeIdentity(target.runtimeIdentity),
              compilerIdentity(target.compilerIdentity) {
            if (UsesHostPlatform(executableDomain)) {
                hostArchitecture = target.hostArchitecture;
                operatingSystem = target.operatingSystem;
                binaryFormat = target.binaryFormat;
                byteOrder = target.byteOrder;
                abi = target.abi;
                addressBits = target.addressBits;
            }
            if (UsesDeviceArchitecture(executableDomain)) {
                features = target.features;
                architectureId = target.architectureId;
                fixedVectorBits = target.fixedVectorBits;
                scalableVectorBits = target.scalableVectorBits;
            }
        }

        /** @brief Return whether this value completely identifies one executable compatibility domain. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            const bool validStability = identityStability == TargetIdentityStability::Ephemeral ||
                                        identityStability == TargetIdentityStability::ProcessStable ||
                                        identityStability == TargetIdentityStability::MachineStable;
            const bool validCommon = ValidBackendKind(backend) && ValidExecutableTargetDomain(executableDomain) &&
                                     validStability && backendAbiVersion == kComputeBackendAbiVersion &&
                                     ValidSemanticHash(runtimeIdentity) && ValidSemanticHash(compilerIdentity);
            const bool validHostArchitecture =
                hostArchitecture >= CpuArchitecture::X86 && hostArchitecture <= CpuArchitecture::Wasm64;
            const bool validOperatingSystem =
                operatingSystem >= OperatingSystem::Windows && operatingSystem <= OperatingSystem::Emscripten;
            const bool validBinaryFormat = binaryFormat >= BinaryFormat::PE && binaryFormat <= BinaryFormat::Wasm;
            const bool validByteOrder =
                byteOrder == ByteOrder::Little || byteOrder == ByteOrder::Big || byteOrder == ByteOrder::Mixed;
            const bool validAbi = abi >= AbiFamily::MSVC && abi <= AbiFamily::WASI;
            const bool validHost = UsesHostPlatform(executableDomain)
                                       ? validHostArchitecture && validOperatingSystem && validBinaryFormat &&
                                             validByteOrder && validAbi && (addressBits == 32 || addressBits == 64)
                                       : hostArchitecture == CpuArchitecture::Unknown &&
                                             operatingSystem == OperatingSystem::Unknown &&
                                             binaryFormat == BinaryFormat::Unknown && byteOrder == ByteOrder::Unknown &&
                                             abi == AbiFamily::Unknown && addressBits == 0;
            const bool validDevice =
                UsesDeviceArchitecture(executableDomain)
                    ? architectureId != 0
                    : features.Bits() == 0 && architectureId == 0 && fixedVectorBits == 0 && scalableVectorBits == 0;
            return validCommon && validHost && validDevice;
        }

        /** @brief Return whether executable bytes may be reused across process lifetimes. */
        [[nodiscard]] constexpr bool PersistentCacheable() const noexcept {
            const bool byteOrderKnown = byteOrder == ByteOrder::Little || byteOrder == ByteOrder::Big;
            return Valid() && identityStability == TargetIdentityStability::MachineStable &&
                   (!UsesHostPlatform(executableDomain) || byteOrderKnown);
        }

        friend constexpr bool operator==(ExecutableTargetIdentity, ExecutableTargetIdentity) noexcept = default;
    };

    /** @brief Canonical compatibility domain of driver-created pipeline-cache bytes. */
    struct PipelineTargetIdentity {
        DeviceId device{};
        BackendKind backend = BackendKind::Cpu;
        TargetIdentityStability identityStability = TargetIdentityStability::Ephemeral;
        uint64_t architectureId = 0;
        uint32_t backendAbiVersion = kComputeBackendAbiVersion;
        SemanticHash runtimeIdentity = kComputeRuntimeIdentity;
        SemanticHash driverIdentity{};

        constexpr PipelineTargetIdentity() noexcept = default;

        /** @brief Project runtime target facts onto exact driver pipeline compatibility. */
        constexpr PipelineTargetIdentity(const TargetFingerprint& target) noexcept
            : device(target.device),
              backend(target.backend),
              identityStability(target.identityStability),
              architectureId(target.architectureId),
              backendAbiVersion(target.backendAbiVersion),
              runtimeIdentity(target.runtimeIdentity),
              driverIdentity(target.driverIdentity) {}

        /** @brief Return whether this value completely identifies one driver compatibility domain. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            const bool validStability = identityStability == TargetIdentityStability::Ephemeral ||
                                        identityStability == TargetIdentityStability::ProcessStable ||
                                        identityStability == TargetIdentityStability::MachineStable;
            return device.Valid() && ValidBackendKind(backend) && validStability && architectureId != 0 &&
                   backendAbiVersion == kComputeBackendAbiVersion && ValidSemanticHash(runtimeIdentity) &&
                   ValidSemanticHash(driverIdentity);
        }

        /** @brief Return whether pipeline-cache bytes may be reused across process lifetimes. */
        [[nodiscard]] constexpr bool PersistentCacheable() const noexcept {
            return Valid() && identityStability == TargetIdentityStability::MachineStable;
        }

        friend constexpr bool operator==(PipelineTargetIdentity, PipelineTargetIdentity) noexcept = default;
    };

    /** @brief Project one complete runtime target onto executable-byte compatibility. */
    [[nodiscard]] constexpr ExecutableTargetIdentity ExecutableTarget(const TargetFingerprint& target) noexcept {
        return ExecutableTargetIdentity{target};
    }

    /** @brief Project one complete runtime target onto driver pipeline-cache compatibility. */
    [[nodiscard]] constexpr PipelineTargetIdentity PipelineTarget(const TargetFingerprint& target) noexcept {
        return PipelineTargetIdentity{target};
    }

    /** @brief Runtime-known shape, layout, and tuning values participating in
     * artifact identity. */
    struct SpecializationKey {
        SemanticHash shape{};
        SemanticHash layout{};
        SemanticHash tuning{};

        friend constexpr bool operator==(SpecializationKey, SpecializationKey) noexcept = default;
    };

    /** @brief Exact identity of device-independent backend lowering. */
    struct LoweringKey {
        SemanticHash module{};
        SemanticHash lowerer{};
        SemanticHash options{};
        uint64_t flags = 0;

        friend constexpr bool operator==(LoweringKey, LoweringKey) noexcept = default;
    };

    /** @brief Exact identity of one target-specific executable artifact. */
    struct ExecutableKey {
        SemanticHash lowered{};
        ExecutableTargetIdentity target{};
        SpecializationKey specialization{};

        friend constexpr bool operator==(ExecutableKey, ExecutableKey) noexcept = default;
    };

    /** @brief Exact identity of one driver-created launch pipeline. */
    struct PipelineKey {
        SemanticHash executable{};
        PipelineTargetIdentity target{};
        SemanticHash builder{};
        SemanticHash state{};
        SemanticHash bindingLayout{};

        friend constexpr bool operator==(PipelineKey, PipelineKey) noexcept = default;
    };

    /** @brief Exact canonical identity shared by caches without conflating
     * stage-specific field meanings. */
    class ArtifactKey final {
    public:
        constexpr ArtifactKey() noexcept = default;

        /** @brief Return whether this key was built from a complete stage-specific
         * identity. */
        [[nodiscard]] constexpr bool Valid() const noexcept { return wordCount_ != 0; }

        /** @brief Return whether this exact identity may be reused across process
         * lifetimes. */
        [[nodiscard]] constexpr bool PersistentCacheable() const noexcept { return persistentCacheable_; }

        /** @brief Return the artifact stage represented by this key. */
        [[nodiscard]] constexpr ArtifactKind Kind() const noexcept { return kind_; }

        /** @brief Return the target backend, or CPU for a device-independent
         * lowering. */
        [[nodiscard]] constexpr BackendKind Backend() const noexcept { return backend_; }

        /** @brief Return the lowerer, compiler, or pipeline-builder identity that
         * produced this stage. */
        [[nodiscard]] constexpr SemanticHash ProducerIdentity() const noexcept { return producerIdentity_; }

        /** @brief Return the executable compatibility domain when this is an executable key. */
        [[nodiscard]] constexpr std::optional<ExecutableTargetDomain> ExecutableDomain() const noexcept {
            if (kind_ != ArtifactKind::DeviceExecutable || wordCount_ < 4) {
                return std::nullopt;
            }
            return static_cast<ExecutableTargetDomain>(words_[3]);
        }

        /** @brief Return the program or preceding artifact identity consumed by this
         * stage. */
        [[nodiscard]] constexpr SemanticHash InputIdentity() const noexcept {
            return wordCount_ >= 2 ? SemanticHash{words_[0], words_[1]} : SemanticHash{};
        }

        /** @brief Return the exact canonical words used for equality, hashing, and
         * disk serialization. */
        [[nodiscard]] constexpr std::span<const uint64_t> Words() const noexcept { return {words_.data(), wordCount_}; }

        friend constexpr bool operator==(const ArtifactKey& left, const ArtifactKey& right) noexcept {
            return left.kind_ == right.kind_ && left.backend_ == right.backend_ &&
                   left.producerIdentity_ == right.producerIdentity_ &&
                   left.persistentCacheable_ == right.persistentCacheable_ && left.wordCount_ == right.wordCount_ &&
                   std::ranges::equal(left.Words(), right.Words());
        }

    private:
        friend constexpr ArtifactKey MakeArtifactKey(const LoweringKey&) noexcept;
        friend constexpr ArtifactKey MakeArtifactKey(const ExecutableKey&) noexcept;
        friend constexpr ArtifactKey MakeArtifactKey(const PipelineKey&) noexcept;

        constexpr void Append(uint64_t value) noexcept { words_[wordCount_++] = value; }
        constexpr void Append(SemanticHash value) noexcept {
            Append(value.low);
            Append(value.high);
        }
        constexpr void Append(DeviceId value) noexcept {
            Append(value.low);
            Append(value.high);
        }
        constexpr void Append(const ExecutableTargetIdentity& target) noexcept {
            Append(std::to_underlying(target.backend));
            Append(std::to_underlying(target.executableDomain));
            Append(target.architectureId);
            Append(target.features.Bits());
            Append(target.fixedVectorBits);
            Append(target.scalableVectorBits);
            Append(target.compilerIdentity);
            Append(target.runtimeIdentity);
            Append(target.backendAbiVersion);
            Append(std::to_underlying(target.identityStability));
            Append(std::to_underlying(target.hostArchitecture));
            Append(std::to_underlying(target.operatingSystem));
            Append(std::to_underlying(target.binaryFormat));
            Append(std::to_underlying(target.byteOrder));
            Append(std::to_underlying(target.abi));
            Append(target.addressBits);
        }
        constexpr void Append(const PipelineTargetIdentity& target) noexcept {
            Append(target.device);
            Append(std::to_underlying(target.backend));
            Append(std::to_underlying(target.identityStability));
            Append(target.architectureId);
            Append(target.backendAbiVersion);
            Append(target.runtimeIdentity);
            Append(target.driverIdentity);
        }

        std::array<uint64_t, 32> words_{};
        SemanticHash producerIdentity_{};
        ArtifactKind kind_ = ArtifactKind::PortableLowering;
        BackendKind backend_ = BackendKind::Cpu;
        uint8_t wordCount_ = 0;
        bool persistentCacheable_ = false;
    };

    /** @brief One successful cold-path timing observation for an exact artifact key. */
    struct ArtifactResolutionObservation {
        ArtifactKey key{};
        ArtifactResolutionPath path = ArtifactResolutionPath::TransientProduction;
        uint64_t nanoseconds = 0;
    };

    /** @brief Non-owning noexcept sink used only after successful cold artifact resolution. */
    struct ArtifactResolutionObserver {
        void* context = nullptr;
        void (*observe)(void*, const ArtifactResolutionObservation&) noexcept = nullptr;

        /** @brief Publish @p observation when a sink is bound. */
        void Observe(const ArtifactResolutionObservation& observation) const noexcept {
            if (observe != nullptr) {
                observe(context, observation);
            }
        }

        [[nodiscard]] constexpr explicit operator bool() const noexcept { return observe != nullptr; }
    };

    /** @brief Build a portable-lowering artifact key without target- or
     * driver-specific contamination. */
    [[nodiscard]] constexpr ArtifactKey MakeArtifactKey(const LoweringKey& key) noexcept {
        ArtifactKey result;
        if (!ValidSemanticHash(key.module) || !ValidSemanticHash(key.lowerer)) {
            return result;
        }
        result.kind_ = ArtifactKind::PortableLowering;
        result.producerIdentity_ = key.lowerer;
        result.Append(key.module);
        result.Append(key.lowerer);
        result.Append(key.options);
        result.persistentCacheable_ = ValidSemanticHash(key.module) && ValidSemanticHash(key.lowerer);
        result.Append(key.flags);
        return result;
    }

    /** @brief Build a target-executable key from the exact target and
     * specialization contract. */
    [[nodiscard]] constexpr ArtifactKey MakeArtifactKey(const ExecutableKey& key) noexcept {
        ArtifactKey result;
        if (!ValidSemanticHash(key.lowered) || !key.target.Valid()) {
            return result;
        }
        result.kind_ = ArtifactKind::DeviceExecutable;
        result.backend_ = key.target.backend;
        result.producerIdentity_ = key.target.compilerIdentity;
        result.persistentCacheable_ = key.target.PersistentCacheable() && ValidSemanticHash(key.lowered);
        result.Append(key.lowered);
        result.Append(key.target);
        result.Append(key.specialization.shape);
        result.Append(key.specialization.layout);
        result.Append(key.specialization.tuning);
        return result;
    }

    /** @brief Build a driver-pipeline key without queue load or other transient
     * runtime state. */
    [[nodiscard]] constexpr ArtifactKey MakeArtifactKey(const PipelineKey& key) noexcept {
        ArtifactKey result;
        if (!ValidSemanticHash(key.executable) || !key.target.Valid() || !ValidSemanticHash(key.builder) ||
            !ValidSemanticHash(key.state) || !ValidSemanticHash(key.bindingLayout)) {
            return result;
        }
        result.kind_ = ArtifactKind::DevicePipeline;
        result.backend_ = key.target.backend;
        result.producerIdentity_ = key.builder;
        result.persistentCacheable_ = key.target.PersistentCacheable() && ValidSemanticHash(key.executable) &&
                                      ValidSemanticHash(key.builder) && ValidSemanticHash(key.state) &&
                                      ValidSemanticHash(key.bindingLayout);
        result.Append(key.executable);
        result.Append(key.target);
        result.Append(key.builder);
        result.Append(key.state);
        result.Append(key.bindingLayout);
        return result;
    }

    /** @brief Total ordering for semantic identities used by immutable sorted
     * indexes. */
    struct SemanticHashLess {
        [[nodiscard]] constexpr bool operator()(SemanticHash left, SemanticHash right) const noexcept {
            return left.low < right.low || (left.low == right.low && left.high < right.high);
        }
    };

    /** @brief Total ordering for complete artifact keys used by canonical catalogs.
     */
    struct ArtifactKeyLess {
        [[nodiscard]] constexpr bool operator()(const ArtifactKey& left, const ArtifactKey& right) const noexcept {
            if (left.Kind() != right.Kind()) {
                return std::to_underlying(left.Kind()) < std::to_underlying(right.Kind());
            }
            return std::ranges::lexicographical_compare(left.Words(), right.Words());
        }
    };

    /** @brief Stable in-process hash functor for artifact lookup; equality remains
     * the collision authority. */
    struct ArtifactKeyHash {
        [[nodiscard]] constexpr size_t operator()(const ArtifactKey& key) const noexcept {
            uint64_t state = 0x9E3779B97F4A7C15ULL;
            const auto mix = [&state](uint64_t value) constexpr noexcept {
                value ^= value >> 30U;
                value *= 0xBF58476D1CE4E5B9ULL;
                value ^= value >> 27U;
                value *= 0x94D049BB133111EBULL;
                value ^= value >> 31U;
                state ^= value + 0x9E3779B97F4A7C15ULL + (state << 6U) + (state >> 2U);
            };
            mix(std::to_underlying(key.Kind()));
            for (uint64_t word : key.Words()) {
                mix(word);
            }
            return static_cast<size_t>(state);
        }
    };

    /** @brief Derive a stable semantic identity from every canonical word of @p
     * key. */
    [[nodiscard]] constexpr SemanticHash ArtifactIdentity(const ArtifactKey& key) noexcept {
        if (!key.Valid()) {
            return {};
        }
        uint64_t low = 0x243F6A8885A308D3ULL ^ std::to_underlying(key.Kind());
        uint64_t high = 0x13198A2E03707344ULL ^ (uint64_t{std::to_underlying(key.Kind())} << 32U);
        for (const uint64_t word : key.Words()) {
            low ^= word + 0x9E3779B97F4A7C15ULL + (low << 6U) + (low >> 2U);
            low ^= low >> 30U;
            low *= 0xBF58476D1CE4E5B9ULL;
            high ^= word + 0xD1B54A32D192ED03ULL + (high << 7U) + (high >> 3U);
            high ^= high >> 27U;
            high *= 0x94D049BB133111EBULL;
        }
        low ^= high >> 29U;
        high ^= low >> 31U;
        return {low, high};
    }

    using ArtifactDestroy = void (*)(void* state) noexcept;

    /** @brief ABI payload transferring ownership of one backend artifact to the
     * runtime. */
    struct ArtifactPayload {
        ArtifactKey key{};
        DeviceId device{};
        void* state = nullptr;
        ArtifactDestroy destroy = nullptr;
    };

    /** @brief Non-owning view used by queues and backend executors. */
    struct ArtifactView {
        void* state = nullptr;
        BackendServiceId service{};
        DeviceId device{};
        SemanticHash identity{};
        ArtifactKind kind = ArtifactKind::PortableLowering;
        BackendKind backend = BackendKind::Cpu;

        /** @brief Return whether the view denotes a live backend artifact. */
        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            const bool validDevice = kind == ArtifactKind::PortableLowering || device.Valid();
            return state != nullptr && service.Valid() && validDevice && ValidSemanticHash(identity) &&
                   ValidArtifactKind(kind) && ValidBackendKind(backend);
        }

        friend constexpr bool operator==(ArtifactView, ArtifactView) noexcept = default;
    };

    /** @brief Failure while adopting an ABI payload into runtime ownership. */
    enum class ArtifactError : uint8_t {
        InvalidPayload,
        KeyMismatch,
        OutOfMemory,
    };

    namespace Detail {

        inline void ReleaseArtifactPayload(ArtifactPayload payload) noexcept {
            if (payload.state != nullptr && payload.destroy != nullptr) {
                payload.destroy(payload.state);
            }
        }

    } // namespace Detail

    /** @brief Shared RAII ownership for a backend artifact cached and submitted by
     * multiple graphs. */
    class Artifact {
        struct Control {
            Control(ArtifactPayload value, BackendService serviceOwner) noexcept
                : payload(value), service(std::move(serviceOwner)) {}
            ~Control() noexcept { payload.destroy(payload.state); }

            ArtifactPayload payload;
            BackendService service;
        };

    public:
        Artifact() noexcept = default;

        /** @brief Adopt @p payload and arrange exactly-once destruction. */
        [[nodiscard]] static std::expected<Artifact, ArtifactError> Adopt(BackendService service,
                                                                          ArtifactPayload payload) noexcept {
            const bool validDevice =
                payload.key.Kind() == ArtifactKind::PortableLowering ? !payload.device.Valid() : payload.device.Valid();
            if (!service || !payload.key.Valid() || !validDevice || payload.state == nullptr ||
                payload.destroy == nullptr) {
                Detail::ReleaseArtifactPayload(payload);
                return std::unexpected(ArtifactError::InvalidPayload);
            }
            try {
                return Artifact{std::make_shared<Control>(payload, std::move(service))};
            } catch (const std::bad_alloc&) {
                Detail::ReleaseArtifactPayload(payload);
                return std::unexpected(ArtifactError::OutOfMemory);
            }
        }

        /** @brief Adopt only when @p payload carries the exact requested key. */
        [[nodiscard]] static std::expected<Artifact, ArtifactError>
        Adopt(BackendService service, ArtifactPayload payload, const ArtifactKey& expected) noexcept {
            if (payload.key != expected) {
                Detail::ReleaseArtifactPayload(payload);
                return std::unexpected(ArtifactError::KeyMismatch);
            }
            return Adopt(std::move(service), payload);
        }

        /** @brief Return whether this handle owns a live artifact. */
        [[nodiscard]] explicit operator bool() const noexcept { return control_ != nullptr; }

        /** @brief Return a non-owning backend submission view. */
        [[nodiscard]] ArtifactView View() const noexcept {
            return control_ != nullptr ? ArtifactView{.state = control_->payload.state,
                                                      .service = control_->service.Id(),
                                                      .device = control_->payload.device,
                                                      .identity = ArtifactIdentity(control_->payload.key),
                                                      .kind = control_->payload.key.Kind(),
                                                      .backend = control_->payload.key.Backend()}
                                       : ArtifactView{};
        }

        /** @brief Return the complete cache key. @pre This handle is non-empty. */
        [[nodiscard]] const ArtifactKey& Key() const noexcept {
            assert(control_ != nullptr);
            return control_->payload.key;
        }

        /** @brief Return the number of shared owners, primarily for cache
         * diagnostics. */
        [[nodiscard]] long UseCount() const noexcept { return control_.use_count(); }

    private:
        explicit Artifact(std::shared_ptr<Control> control) noexcept : control_(std::move(control)) {}

        std::shared_ptr<Control> control_{};
    };

    static_assert(std::is_trivially_copyable_v<TargetFingerprint>);
    static_assert(std::is_trivially_copyable_v<ExecutableTargetIdentity>);
    static_assert(std::is_trivially_copyable_v<PipelineTargetIdentity>);
    static_assert(std::is_trivially_copyable_v<SpecializationKey>);
    static_assert(std::is_trivially_copyable_v<LoweringKey>);
    static_assert(std::is_trivially_copyable_v<ExecutableKey>);
    static_assert(std::is_trivially_copyable_v<PipelineKey>);
    static_assert(std::is_trivially_copyable_v<ArtifactKey>);
    static_assert(std::is_trivially_copyable_v<ArtifactPayload>);
    static_assert(std::is_trivially_copyable_v<RuntimeTargetFacts>);
    static_assert(std::is_standard_layout_v<ArtifactPayload>);

} // namespace Sora::Compute

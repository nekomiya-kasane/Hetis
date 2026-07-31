/**
 * @file Backend.h
 * @brief Versioned compiler callback boundary for portable compute backends.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Artifact.h>
#include <Sora/Compute/Backend/ArtifactIo.h>
#include <Sora/Compute/Backend/Service.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Compute {

    /** @brief Portable compiler failure independent of a concrete backend SDK. */
    enum class CompileErrorCode : uint8_t {
        None,
        InvalidRequest,
        InvalidApi,
        BackendUnavailable,
        UnsupportedTarget,
        UnsupportedIr,
        CompilationFailed,
        InvalidArtifact,
        ArtifactImportFailed,
        ArtifactExportFailed,
        OutOfMemory,
    };

    /** @brief Compiler error with an opaque backend-specific diagnostic identity. */
    struct CompileError {
        CompileErrorCode code = CompileErrorCode::None;
        uint64_t detail = 0;

        friend constexpr bool operator==(CompileError, CompileError) noexcept = default;
    };

    /** @brief Versioned, allocation-free request passed to one backend compiler callback. */
    struct BackendCompileRequest {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(BackendCompileRequest);
        const std::byte* moduleData = nullptr;
        uint64_t moduleSize = 0;
        SemanticHash moduleHash{};
        TargetFingerprint target{};
        SpecializationKey specialization{};
        ArtifactKind artifactKind = ArtifactKind::DeviceExecutable;

        /** @brief Return the canonical module bytes without copying. */
        [[nodiscard]] constexpr std::span<const std::byte> ModuleBytes() const noexcept {
            if (moduleSize > std::numeric_limits<size_t>::max()) {
                return {};
            }
            return {moduleData, static_cast<size_t>(moduleSize)};
        }

        /** @brief Return the artifact identity required from a successful compiler. */
        [[nodiscard]] constexpr ArtifactKey ExpectedKey() const noexcept {
            switch (artifactKind) {
                case ArtifactKind::PortableLowering:
                    return MakeArtifactKey(LoweringKey{.module = moduleHash, .lowerer = target.compilerIdentity});
                case ArtifactKind::DeviceExecutable:
                    return MakeArtifactKey(ExecutableKey{
                        .lowered = moduleHash, .target = ExecutableTarget(target), .specialization = specialization});
                case ArtifactKind::DevicePipeline:
                    return {};
            }
            return {};
        }
    };

    /** @brief Runtime-allocated callback result; a non-null payload denotes success. */
    struct BackendCompileResult {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(BackendCompileResult);
        ArtifactPayload payload{};
        CompileError error{CompileErrorCode::CompilationFailed, 0};
    };

    /** @brief Versioned request for reconstructing one backend artifact from validated persistent bytes. */
    struct BackendArtifactImportRequest {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(BackendArtifactImportRequest);
        const BackendCompileRequest* compileRequest = nullptr;
        const std::byte* artifactData = nullptr;
        uint64_t artifactSize = 0;

        /** @brief Return the backend-private artifact bytes without copying. */
        [[nodiscard]] constexpr std::span<const std::byte> ArtifactBytes() const noexcept {
            if (artifactSize > std::numeric_limits<size_t>::max()) {
                return {};
            }
            return {artifactData, static_cast<size_t>(artifactSize)};
        }
    };

    /** @brief Runtime-allocated export result; a complete byte payload denotes success. */
    struct BackendArtifactExportResult {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(BackendArtifactExportResult);
        BackendArtifactBytes payload{};
        CompileError error{CompileErrorCode::ArtifactExportFailed, 0};
    };

    using BackendCompile = void (*)(void* state, const BackendCompileRequest* request,
                                    BackendCompileResult* result) noexcept;
    using BackendImportArtifact = void (*)(void* state, const BackendArtifactImportRequest* request,
                                           BackendCompileResult* result) noexcept;
    using BackendExportArtifact = void (*)(void* state, const BackendArtifactExportRequest* request,
                                           BackendArtifactExportResult* result) noexcept;

    /** @brief ABI-version-5 POD callback table for one compiler implementation. */
    struct BackendCompilerApiV5 {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(BackendCompilerApiV5);
        SemanticHash identity{};
        BackendKind backend = BackendKind::Cpu;
        ExecutableTargetDomain executableDomain = ExecutableTargetDomain::TargetNative;
        void* state = nullptr;
        BackendCompile compile = nullptr;
        BackendImportArtifact importArtifact = nullptr;
        BackendExportArtifact exportArtifact = nullptr;
    };

    namespace Detail {

        [[nodiscard]] constexpr bool ValidCompileErrorCode(CompileErrorCode code) noexcept {
            return code == CompileErrorCode::None || code == CompileErrorCode::InvalidApi ||
                   code == CompileErrorCode::InvalidRequest || code == CompileErrorCode::BackendUnavailable ||
                   code == CompileErrorCode::UnsupportedTarget || code == CompileErrorCode::UnsupportedIr ||
                   code == CompileErrorCode::CompilationFailed || code == CompileErrorCode::InvalidArtifact ||
                   code == CompileErrorCode::ArtifactImportFailed || code == CompileErrorCode::ArtifactExportFailed ||
                   code == CompileErrorCode::OutOfMemory;
        }

        [[nodiscard]] constexpr bool ValidBackendCompileErrorCode(CompileErrorCode code) noexcept {
            return ValidCompileErrorCode(code) && code != CompileErrorCode::InvalidApi;
        }

        [[nodiscard]] constexpr bool ValidCompilerIdentity(SemanticHash identity) noexcept {
            return identity.low != 0 || identity.high != 0;
        }

    } // namespace Detail

    /** @brief Validated compiler facade retaining backend state and executable code. */
    class BackendCompiler final {
    public:
        /** @brief Validate and retain one immutable compiler callback table. */
        [[nodiscard]] static std::expected<BackendCompiler, CompileError> Create(BackendService service,
                                                                                 BackendCompilerApiV5 api) noexcept {
            const bool valid =
                service && ValidBackendAbi(api.abiVersion, api.structSize, sizeof(BackendCompilerApiV5)) &&
                Detail::ValidCompilerIdentity(api.identity) && ValidBackendKind(api.backend) &&
                ValidExecutableTargetDomain(api.executableDomain) && api.state != nullptr && api.compile != nullptr &&
                ((api.importArtifact == nullptr && api.exportArtifact == nullptr) ||
                 (api.importArtifact != nullptr && api.exportArtifact != nullptr));
            if (!valid) {
                return std::unexpected(CompileError{CompileErrorCode::InvalidApi, 0});
            }
            return BackendCompiler{std::move(service), api};
        }

        /** @brief Return the semantic compiler identity participating in artifact keys. */
        [[nodiscard]] constexpr SemanticHash Identity() const noexcept { return api_.identity; }

        /** @brief Return the physical backend targeted by this compiler. */
        [[nodiscard]] constexpr BackendKind Backend() const noexcept { return api_.backend; }

        /** @brief Return the target dimensions participating in compiler-produced executable keys. */
        [[nodiscard]] constexpr ExecutableTargetDomain ExecutableDomain() const noexcept {
            return api_.executableDomain;
        }

        /** @brief Return the runtime service context owning compiler-produced native handles. */
        [[nodiscard]] BackendServiceId Service() const noexcept { return service_.Id(); }

        /** @brief Return whether this compiler can reconstruct and export persistent artifacts. */
        [[nodiscard]] constexpr bool SupportsPersistentArtifacts() const noexcept {
            return api_.importArtifact != nullptr;
        }

        /** @brief Return whether @p request is complete and belongs to this compiler identity. */
        [[nodiscard]] bool Accepts(const BackendCompileRequest& request) const noexcept {
            return ValidRequest(request, request.ExpectedKey());
        }

        /** @brief Validate @p request, compile it, and transactionally adopt the returned artifact. */
        [[nodiscard]] std::expected<Artifact, CompileError>
        Compile(const BackendCompileRequest& request) const noexcept {
            const ArtifactKey expectedKey = request.ExpectedKey();
            if (!ValidRequest(request, expectedKey)) {
                return std::unexpected(CompileError{CompileErrorCode::InvalidRequest, 0});
            }

            BackendCompileResult result{};
            api_.compile(api_.state, &request, &result);
            const DeviceId expectedDevice =
                expectedKey.Kind() == ArtifactKind::PortableLowering ? DeviceId{} : request.target.device;
            return AdoptResult(result, expectedKey, expectedDevice);
        }

        /** @brief Reconstruct an artifact from backend-private bytes under the exact @p request identity. */
        [[nodiscard]] std::expected<Artifact, CompileError> Import(const BackendCompileRequest& request,
                                                                   std::span<const std::byte> bytes) const noexcept {
            const ArtifactKey expectedKey = request.ExpectedKey();
            if (!SupportsPersistentArtifacts() || !ValidRequest(request, expectedKey) || bytes.empty()) {
                return std::unexpected(CompileError{CompileErrorCode::InvalidRequest, 0});
            }
            BackendArtifactImportRequest importRequest{
                .compileRequest = &request,
                .artifactData = bytes.data(),
                .artifactSize = bytes.size(),
            };
            BackendCompileResult result{};
            api_.importArtifact(api_.state, &importRequest, &result);
            const DeviceId expectedDevice =
                expectedKey.Kind() == ArtifactKind::PortableLowering ? DeviceId{} : request.target.device;
            return AdoptResult(result, expectedKey, expectedDevice);
        }

        /** @brief Copy one live artifact into backend-private reconstructible bytes. */
        [[nodiscard]] std::expected<std::vector<std::byte>, CompileError>
        Export(const Artifact& artifact) const noexcept {
            const ArtifactView view = artifact.View();
            if (!SupportsPersistentArtifacts() || !artifact) {
                return std::unexpected(CompileError{CompileErrorCode::InvalidRequest, 0});
            }
            const ArtifactKey& key = artifact.Key();
            const bool validStage = key.Kind() == ArtifactKind::PortableLowering ||
                                    (key.Kind() == ArtifactKind::DeviceExecutable && key.Backend() == api_.backend &&
                                     key.ExecutableDomain() == api_.executableDomain);
            if (!validStage || key.ProducerIdentity() != api_.identity || view.service != service_.Id()) {
                return std::unexpected(CompileError{CompileErrorCode::InvalidRequest, 0});
            }
            const BackendArtifactExportRequest request{.key = artifact.Key(), .artifact = view};
            BackendArtifactExportResult result{};
            api_.exportArtifact(api_.state, &request, &result);
            if (!ValidBackendAbi(result.abiVersion, result.structSize, sizeof(BackendArtifactExportResult)) ||
                !Detail::ValidBackendCompileErrorCode(result.error.code)) {
                Detail::ReleaseArtifactBytes(result.payload);
                return std::unexpected(CompileError{CompileErrorCode::InvalidArtifact, 0});
            }
            if (result.error.code != CompileErrorCode::None) {
                const CompileError error = result.error;
                Detail::ReleaseArtifactBytes(result.payload);
                return std::unexpected(error);
            }
            if (result.payload.data == nullptr || result.payload.size == 0 || result.payload.state == nullptr ||
                result.payload.destroy == nullptr || result.payload.size > std::numeric_limits<size_t>::max()) {
                Detail::ReleaseArtifactBytes(result.payload);
                return std::unexpected(CompileError{CompileErrorCode::InvalidArtifact, 0});
            }
            try {
                std::vector<std::byte> bytes(result.payload.Bytes().begin(), result.payload.Bytes().end());
                Detail::ReleaseArtifactBytes(result.payload);
                return bytes;
            } catch (const std::bad_alloc&) {
                Detail::ReleaseArtifactBytes(result.payload);
                return std::unexpected(CompileError{CompileErrorCode::OutOfMemory, 0});
            } catch (...) {
                Detail::ReleaseArtifactBytes(result.payload);
                return std::unexpected(CompileError{CompileErrorCode::InvalidArtifact, 0});
            }
        }

    private:
        [[nodiscard]] bool ValidRequest(const BackendCompileRequest& request,
                                        const ArtifactKey& expectedKey) const noexcept {
            return request.abiVersion == kComputeBackendAbiVersion &&
                   request.structSize >= sizeof(BackendCompileRequest) && request.moduleData != nullptr &&
                   request.moduleSize != 0 && request.moduleSize <= std::numeric_limits<size_t>::max() &&
                   request.target.backend == api_.backend && request.target.compilerIdentity == api_.identity &&
                   request.target.executableDomain == api_.executableDomain &&
                   Detail::HashBytes(request.ModuleBytes()) == request.moduleHash && expectedKey.Valid();
        }

        [[nodiscard]] std::expected<Artifact, CompileError> AdoptResult(BackendCompileResult& result,
                                                                        const ArtifactKey& expectedKey,
                                                                        DeviceId expectedDevice) const noexcept {
            if (!ValidBackendAbi(result.abiVersion, result.structSize, sizeof(BackendCompileResult)) ||
                !Detail::ValidBackendCompileErrorCode(result.error.code)) {
                Detail::ReleaseArtifactPayload(result.payload);
                return std::unexpected(CompileError{CompileErrorCode::InvalidArtifact, 0});
            }
            if (result.payload.state == nullptr) {
                return std::unexpected(result.error.code == CompileErrorCode::None
                                           ? CompileError{CompileErrorCode::CompilationFailed, 0}
                                           : result.error);
            }
            if (result.error.code != CompileErrorCode::None) {
                const uint64_t detail = result.error.detail;
                Detail::ReleaseArtifactPayload(result.payload);
                return std::unexpected(CompileError{CompileErrorCode::InvalidArtifact, detail});
            }
            if (result.payload.device != expectedDevice) {
                Detail::ReleaseArtifactPayload(result.payload);
                return std::unexpected(CompileError{CompileErrorCode::InvalidArtifact, 0});
            }

            auto artifact = Artifact::Adopt(service_, result.payload, expectedKey);
            if (!artifact) {
                const CompileErrorCode code = artifact.error() == ArtifactError::OutOfMemory
                                                  ? CompileErrorCode::OutOfMemory
                                                  : CompileErrorCode::InvalidArtifact;
                return std::unexpected(CompileError{code, static_cast<uint64_t>(artifact.error())});
            }
            return std::move(*artifact);
        }

        BackendCompiler(BackendService service, BackendCompilerApiV5 api) noexcept
            : service_(std::move(service)), api_(api) {}

        BackendService service_{};
        BackendCompilerApiV5 api_{};
    };

    static_assert(std::is_trivially_copyable_v<BackendCompileRequest>);
    static_assert(std::is_same_v<decltype(BackendCompileRequest::moduleSize), uint64_t>);
    static_assert(std::is_trivially_copyable_v<BackendCompileResult>);
    static_assert(std::is_trivially_copyable_v<BackendArtifactImportRequest>);
    static_assert(std::is_trivially_copyable_v<BackendArtifactExportResult>);
    static_assert(std::is_trivially_copyable_v<BackendCompilerApiV5>);
    static_assert(std::is_standard_layout_v<BackendCompileRequest>);
    static_assert(std::is_standard_layout_v<BackendArtifactImportRequest>);
    static_assert(std::is_standard_layout_v<BackendCompilerApiV5>);

} // namespace Sora::Compute

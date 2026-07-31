/**
 * @file Pipeline.h
 * @brief Versioned creation boundary for driver-owned compute pipelines.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Backend/ArtifactIo.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Compute {

    /** @brief Semantic state required to create one launch pipeline from an executable artifact. */
    struct PipelineStateKey {
        SemanticHash state{};
        SemanticHash bindingLayout{};

        /** @brief Return whether both pipeline compatibility domains have stable identities. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return ValidSemanticHash(state) && ValidSemanticHash(bindingLayout);
        }

        friend constexpr bool operator==(PipelineStateKey, PipelineStateKey) noexcept = default;
    };

    /** @brief Portable pipeline creation failure independent of a concrete driver API. */
    enum class PipelineErrorCode : uint8_t {
        None,
        InvalidRequest,
        InvalidApi,
        BackendUnavailable,
        UnsupportedExecutable,
        CreationFailed,
        InvalidArtifact,
        ArtifactImportFailed,
        ArtifactExportFailed,
        OutOfMemory,
    };

    /** @brief Pipeline error with an opaque backend-specific diagnostic identity. */
    struct PipelineError {
        PipelineErrorCode code = PipelineErrorCode::None;
        uint64_t detail = 0;

        friend constexpr bool operator==(PipelineError, PipelineError) noexcept = default;
    };

    /** @brief Versioned, allocation-free request passed to one pipeline callback. */
    struct BackendPipelineRequest {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(BackendPipelineRequest);
        ArtifactView executable{};
        PipelineKey pipeline{};

        /** @brief Return the exact artifact identity required from a successful callback. */
        [[nodiscard]] constexpr ArtifactKey ExpectedKey() const noexcept { return MakeArtifactKey(pipeline); }
    };

    /** @brief Runtime-allocated pipeline callback result; a non-null payload denotes success. */
    struct BackendPipelineResult {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(BackendPipelineResult);
        ArtifactPayload payload{};
        PipelineError error{PipelineErrorCode::CreationFailed, 0};
    };

    /** @brief Versioned request for reconstructing one pipeline from validated persistent bytes. */
    struct BackendPipelineImportRequest {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(BackendPipelineImportRequest);
        const BackendPipelineRequest* pipelineRequest = nullptr;
        const std::byte* artifactData = nullptr;
        uint64_t artifactSize = 0;

        /** @brief Return the backend-private pipeline bytes without copying. */
        [[nodiscard]] constexpr std::span<const std::byte> ArtifactBytes() const noexcept {
            if (artifactSize > std::numeric_limits<size_t>::max()) {
                return {};
            }
            return {artifactData, static_cast<size_t>(artifactSize)};
        }
    };

    /** @brief Runtime-allocated export result; a complete byte payload denotes success. */
    struct BackendPipelineExportResult {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(BackendPipelineExportResult);
        BackendArtifactBytes payload{};
        PipelineError error{PipelineErrorCode::ArtifactExportFailed, 0};
    };

    using BackendCreatePipeline = void (*)(void* state, const BackendPipelineRequest* request,
                                           BackendPipelineResult* result) noexcept;
    using BackendImportPipeline = void (*)(void* state, const BackendPipelineImportRequest* request,
                                           BackendPipelineResult* result) noexcept;
    using BackendExportPipeline = void (*)(void* state, const BackendArtifactExportRequest* request,
                                           BackendPipelineExportResult* result) noexcept;

    /** @brief ABI-version-1 POD callback table for one driver pipeline implementation. */
    struct BackendPipelineApiV1 {
        uint32_t abiVersion = kComputeBackendAbiVersion;
        uint32_t structSize = sizeof(BackendPipelineApiV1);
        SemanticHash identity{};
        void* state = nullptr;
        BackendCreatePipeline create = nullptr;
        BackendImportPipeline importPipeline = nullptr;
        BackendExportPipeline exportPipeline = nullptr;
    };

    /** @brief One planner-visible factory and semantic state combination supported by a physical device. */
    struct PipelineConfiguration {
        BackendServiceId service{};
        SemanticHash factory{};
        SemanticHash state{};
        DeviceId device{};
        BackendKind backend = BackendKind::Cpu;

        /** @brief Return whether every identity required to select this configuration is complete. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return service.Valid() && ValidSemanticHash(factory) && ValidSemanticHash(state) && device.Valid() &&
                   ValidBackendKind(backend);
        }

        friend constexpr bool operator==(PipelineConfiguration, PipelineConfiguration) noexcept = default;
    };

    namespace Detail {

        [[nodiscard]] constexpr bool ValidPipelineErrorCode(PipelineErrorCode code) noexcept {
            return code == PipelineErrorCode::None || code == PipelineErrorCode::InvalidRequest ||
                   code == PipelineErrorCode::InvalidApi || code == PipelineErrorCode::BackendUnavailable ||
                   code == PipelineErrorCode::UnsupportedExecutable || code == PipelineErrorCode::CreationFailed ||
                   code == PipelineErrorCode::InvalidArtifact || code == PipelineErrorCode::ArtifactImportFailed ||
                   code == PipelineErrorCode::ArtifactExportFailed || code == PipelineErrorCode::OutOfMemory;
        }

        [[nodiscard]] constexpr bool ValidBackendPipelineErrorCode(PipelineErrorCode code) noexcept {
            return ValidPipelineErrorCode(code) && code != PipelineErrorCode::InvalidApi;
        }

    } // namespace Detail

    /** @brief Validated pipeline facade retaining backend state and executable callback code. */
    class BackendPipelineFactory final {
    public:
        /** @brief Validate and retain one immutable pipeline callback table. */
        [[nodiscard]] static std::expected<BackendPipelineFactory, PipelineError>
        Create(BackendService service, TargetFingerprint target, BackendPipelineApiV1 api) noexcept {
            const bool valid =
                service && ValidBackendAbi(api.abiVersion, api.structSize, sizeof(BackendPipelineApiV1)) &&
                target.Valid() && ValidSemanticHash(api.identity) && api.state != nullptr && api.create != nullptr &&
                ((api.importPipeline == nullptr && api.exportPipeline == nullptr) ||
                 (api.importPipeline != nullptr && api.exportPipeline != nullptr));
            if (!valid) {
                return std::unexpected(PipelineError{PipelineErrorCode::InvalidApi, 0});
            }
            return BackendPipelineFactory{std::move(service), target, api};
        }

        /** @brief Return the pipeline implementation identity participating in artifact keys. */
        [[nodiscard]] constexpr SemanticHash Identity() const noexcept { return api_.identity; }

        /** @brief Return the physical backend targeted by this factory. */
        [[nodiscard]] constexpr BackendKind Backend() const noexcept { return target_.backend; }

        /** @brief Return the physical device whose driver context creates these pipelines. */
        [[nodiscard]] constexpr DeviceId Device() const noexcept { return target_.device; }

        /** @brief Return the complete runtime target bound to this driver context. */
        [[nodiscard]] constexpr const TargetFingerprint& Target() const noexcept { return target_; }

        /** @brief Return the runtime service context owning factory-produced native handles. */
        [[nodiscard]] BackendServiceId Service() const noexcept { return service_.Id(); }

        /** @brief Return whether this factory can reconstruct and export persistent driver pipelines. */
        [[nodiscard]] constexpr bool SupportsPersistentArtifacts() const noexcept {
            return api_.importPipeline != nullptr;
        }

        /** @brief Bind one semantic pipeline state to this factory for planner discovery. */
        [[nodiscard]] PipelineConfiguration Configuration(SemanticHash state) const noexcept {
            return {
                .service = Service(), .factory = Identity(), .state = state, .device = Device(), .backend = Backend()};
        }

        /** @brief Return the exact key this factory would create from @p executable and @p state. */
        [[nodiscard]] ArtifactKey ExpectedKey(const Artifact& executable, PipelineStateKey state) const noexcept {
            const BackendPipelineRequest request = MakeRequest(executable, state);
            return ValidRequest(request) ? request.ExpectedKey() : ArtifactKey{};
        }

        /** @brief Return whether @p executable and @p state form a complete request for this factory. */
        [[nodiscard]] bool Accepts(const Artifact& executable, PipelineStateKey state) const noexcept {
            return ValidRequest(MakeRequest(executable, state));
        }

        /** @brief Create a driver pipeline while @p executable remains strongly owned by the caller. */
        [[nodiscard]] std::expected<Artifact, PipelineError> CreatePipeline(const Artifact& executable,
                                                                            PipelineStateKey state) const noexcept {
            const BackendPipelineRequest request = MakeRequest(executable, state);
            const ArtifactKey expectedKey = request.ExpectedKey();
            if (!ValidRequest(request)) {
                return std::unexpected(PipelineError{PipelineErrorCode::InvalidRequest, 0});
            }
            BackendPipelineResult result{};
            api_.create(api_.state, &request, &result);
            return AdoptResult(result, expectedKey);
        }

        /** @brief Reconstruct a driver pipeline from backend-private bytes under the exact request identity. */
        [[nodiscard]] std::expected<Artifact, PipelineError> Import(const Artifact& executable, PipelineStateKey state,
                                                                    std::span<const std::byte> bytes) const noexcept {
            const BackendPipelineRequest request = MakeRequest(executable, state);
            const ArtifactKey expectedKey = request.ExpectedKey();
            if (!SupportsPersistentArtifacts() || !ValidRequest(request) || bytes.empty()) {
                return std::unexpected(PipelineError{PipelineErrorCode::InvalidRequest, 0});
            }
            const BackendPipelineImportRequest importRequest{
                .pipelineRequest = &request,
                .artifactData = bytes.data(),
                .artifactSize = bytes.size(),
            };
            BackendPipelineResult result{};
            api_.importPipeline(api_.state, &importRequest, &result);
            return AdoptResult(result, expectedKey);
        }

        /** @brief Copy one live driver pipeline into backend-private reconstructible bytes. */
        [[nodiscard]] std::expected<std::vector<std::byte>, PipelineError>
        Export(const Artifact& artifact) const noexcept {
            const ArtifactView view = artifact.View();
            if (!SupportsPersistentArtifacts() || !artifact || artifact.Key().Kind() != ArtifactKind::DevicePipeline ||
                artifact.Key().Backend() != Backend() || view.device != Device() ||
                artifact.Key().ProducerIdentity() != api_.identity || view.service != service_.Id()) {
                return std::unexpected(PipelineError{PipelineErrorCode::InvalidRequest, 0});
            }
            const BackendArtifactExportRequest request{.key = artifact.Key(), .artifact = view};
            BackendPipelineExportResult result{};
            api_.exportPipeline(api_.state, &request, &result);
            if (!ValidBackendAbi(result.abiVersion, result.structSize, sizeof(BackendPipelineExportResult)) ||
                !Detail::ValidBackendPipelineErrorCode(result.error.code)) {
                Detail::ReleaseArtifactBytes(result.payload);
                return std::unexpected(PipelineError{PipelineErrorCode::InvalidArtifact, 0});
            }
            if (result.error.code != PipelineErrorCode::None) {
                const PipelineError error = result.error;
                Detail::ReleaseArtifactBytes(result.payload);
                return std::unexpected(error);
            }
            if (result.payload.data == nullptr || result.payload.size == 0 || result.payload.state == nullptr ||
                result.payload.destroy == nullptr || result.payload.size > std::numeric_limits<size_t>::max()) {
                Detail::ReleaseArtifactBytes(result.payload);
                return std::unexpected(PipelineError{PipelineErrorCode::InvalidArtifact, 0});
            }
            try {
                std::vector<std::byte> bytes(result.payload.Bytes().begin(), result.payload.Bytes().end());
                Detail::ReleaseArtifactBytes(result.payload);
                return bytes;
            } catch (const std::bad_alloc&) {
                Detail::ReleaseArtifactBytes(result.payload);
                return std::unexpected(PipelineError{PipelineErrorCode::OutOfMemory, 0});
            } catch (...) {
                Detail::ReleaseArtifactBytes(result.payload);
                return std::unexpected(PipelineError{PipelineErrorCode::InvalidArtifact, 0});
            }
        }

    private:
        [[nodiscard]] BackendPipelineRequest MakeRequest(const Artifact& executable,
                                                         PipelineStateKey state) const noexcept {
            if (!executable) {
                return {};
            }
            const ArtifactKey& executableKey = executable.Key();
            return {
                .executable = executable.View(),
                .pipeline = {.executable = ArtifactIdentity(executableKey),
                             .target = PipelineTarget(target_),
                             .builder = api_.identity,
                             .state = state.state,
                             .bindingLayout = state.bindingLayout},
            };
        }

        [[nodiscard]] bool ValidRequest(const BackendPipelineRequest& request) const noexcept {
            return request.abiVersion == kComputeBackendAbiVersion &&
                   request.structSize >= sizeof(BackendPipelineRequest) && request.executable &&
                   request.executable.kind == ArtifactKind::DeviceExecutable &&
                   request.executable.backend == Backend() && request.executable.device == Device() &&
                   request.executable.service == service_.Id() &&
                   request.executable.device == request.pipeline.target.device &&
                   request.pipeline.target == PipelineTarget(target_) && request.pipeline.builder == api_.identity &&
                   request.pipeline.executable == request.executable.identity &&
                   ValidSemanticHash(request.pipeline.state) && ValidSemanticHash(request.pipeline.bindingLayout) &&
                   request.ExpectedKey().Valid();
        }

        [[nodiscard]] std::expected<Artifact, PipelineError>
        AdoptResult(BackendPipelineResult& result, const ArtifactKey& expectedKey) const noexcept {
            if (!ValidBackendAbi(result.abiVersion, result.structSize, sizeof(BackendPipelineResult)) ||
                !Detail::ValidBackendPipelineErrorCode(result.error.code)) {
                Detail::ReleaseArtifactPayload(result.payload);
                return std::unexpected(PipelineError{PipelineErrorCode::InvalidArtifact, 0});
            }
            if (result.payload.state == nullptr) {
                return std::unexpected(result.error.code == PipelineErrorCode::None
                                           ? PipelineError{PipelineErrorCode::CreationFailed, 0}
                                           : result.error);
            }
            if (result.error.code != PipelineErrorCode::None) {
                const uint64_t detail = result.error.detail;
                Detail::ReleaseArtifactPayload(result.payload);
                return std::unexpected(PipelineError{PipelineErrorCode::InvalidArtifact, detail});
            }
            if (result.payload.device != Device()) {
                Detail::ReleaseArtifactPayload(result.payload);
                return std::unexpected(PipelineError{PipelineErrorCode::InvalidArtifact, 0});
            }
            auto artifact = Artifact::Adopt(service_, result.payload, expectedKey);
            if (!artifact) {
                const PipelineErrorCode code = artifact.error() == ArtifactError::OutOfMemory
                                                   ? PipelineErrorCode::OutOfMemory
                                                   : PipelineErrorCode::InvalidArtifact;
                return std::unexpected(PipelineError{code, static_cast<uint64_t>(artifact.error())});
            }
            return std::move(*artifact);
        }

        BackendPipelineFactory(BackendService service, TargetFingerprint target, BackendPipelineApiV1 api) noexcept
            : service_(std::move(service)), target_(target), api_(api) {}

        BackendService service_{};
        TargetFingerprint target_{};
        BackendPipelineApiV1 api_{};
    };

    static_assert(std::is_trivially_copyable_v<PipelineStateKey>);
    static_assert(std::is_trivially_copyable_v<PipelineConfiguration>);
    static_assert(std::is_trivially_copyable_v<PipelineError>);
    static_assert(std::is_trivially_copyable_v<BackendPipelineRequest>);
    static_assert(std::is_trivially_copyable_v<BackendPipelineResult>);
    static_assert(std::is_trivially_copyable_v<BackendPipelineImportRequest>);
    static_assert(std::is_trivially_copyable_v<BackendPipelineExportResult>);
    static_assert(std::is_trivially_copyable_v<BackendPipelineApiV1>);
    static_assert(std::is_standard_layout_v<BackendPipelineRequest>);
    static_assert(std::is_standard_layout_v<BackendPipelineImportRequest>);
    static_assert(std::is_standard_layout_v<BackendPipelineApiV1>);

} // namespace Sora::Compute

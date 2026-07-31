/**
 * @file CpuCodeProvider.h
 * @brief Versioned native-code provider boundary for the built-in CPU backend.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Backend.h>
#include <Sora/Compute/Backend/Queue.h>
#include <Sora/Compute/KernelProgram.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace Sora::Compute {

    inline constexpr uint32_t kCpuCodeProviderAbiVersion = 2;

    /** @brief Return whether a CPU code-provider ABI structure exposes the complete version-1 prefix. */
    [[nodiscard]] constexpr bool ValidCpuCodeProviderAbi(uint32_t version, uint32_t size,
                                                         size_t requiredSize) noexcept {
        return version == kCpuCodeProviderAbiVersion && size >= requiredSize;
    }

    /** @brief One validated affine buffer view indexed by its KernelProgram argument number. */
    struct CpuCodeBuffer {
        std::byte* data = nullptr;
        uint64_t sizeBytes = 0;
        AccessMode access = AccessMode::Read;
        ValueType elementType{};
        LayoutKind layout = LayoutKind::Scalar;
        uint8_t rank = 0;
        uint64_t extents[kMaximumBufferRank]{};
        uint64_t byteStrides[kMaximumBufferRank]{};
        uint64_t componentStrideBytes = 0;
    };

    /** @brief One immutable scalar view indexed by its KernelProgram argument number. */
    struct CpuCodeScalar {
        const std::byte* data = nullptr;
        uint32_t sizeBytes = 0;
        ValueType type{};
    };

    /** @brief Allocation-free native range invocation built once from a validated backend dispatch. */
    struct CpuCodeInvocation {
        uint32_t abiVersion = kCpuCodeProviderAbiVersion;
        uint32_t structSize = sizeof(CpuCodeInvocation);
        const CpuCodeBuffer* buffers = nullptr;
        const CpuCodeScalar* scalars = nullptr;
        uint32_t argumentCount = 0;
        uint8_t rank = 0;
        uint64_t origin[kMaximumIndexRank]{};
        uint64_t extents[kMaximumIndexRank]{};
        uint64_t begin = 0;
        uint64_t end = 0;
    };

    /** @brief Canonical program and exact native target passed to provider compilation. */
    struct CpuCodeCompileRequest {
        uint32_t abiVersion = kCpuCodeProviderAbiVersion;
        uint32_t structSize = sizeof(CpuCodeCompileRequest);
        const std::byte* programData = nullptr;
        uint64_t programSize = 0;
        SemanticHash programHash{};
        TargetFingerprint target{};
        SpecializationKey specialization{};

        /** @brief Return canonical KernelProgram bytes without copying. */
        [[nodiscard]] constexpr std::span<const std::byte> ProgramBytes() const noexcept {
            if (programSize > std::numeric_limits<size_t>::max()) {
                return {};
            }
            return {programData, static_cast<size_t>(programSize)};
        }
    };

    /** @brief Native object bytes imported under one exact canonical program and target identity. */
    struct CpuCodeImportRequest {
        uint32_t abiVersion = kCpuCodeProviderAbiVersion;
        uint32_t structSize = sizeof(CpuCodeImportRequest);
        const CpuCodeCompileRequest* compileRequest = nullptr;
        const std::byte* objectData = nullptr;
        uint64_t objectSize = 0;

        /** @brief Return backend-private native object bytes without copying. */
        [[nodiscard]] constexpr std::span<const std::byte> ObjectBytes() const noexcept {
            if (objectSize > std::numeric_limits<size_t>::max()) {
                return {};
            }
            return {objectData, static_cast<size_t>(objectSize)};
        }
    };

    /** @brief Non-owning provider-private artifact state accepted only by its creating provider. */
    struct CpuCodeExportRequest {
        uint32_t abiVersion = kCpuCodeProviderAbiVersion;
        uint32_t structSize = sizeof(CpuCodeExportRequest);
        void* artifactState = nullptr;
    };

    using CpuCodeArtifactDestroy = void (*)(void* state) noexcept;
    using CpuCodeEntry = void (*)(void* state, const CpuCodeInvocation* invocation) noexcept;

    /** @brief Ownership and range entrypoint returned for one native CPU artifact. */
    struct CpuCodeArtifactPayload {
        void* state = nullptr;
        CpuCodeArtifactDestroy destroy = nullptr;
        CpuCodeEntry execute = nullptr;
    };

    /** @brief Provider-allocated compilation result transferred to the CPU backend. */
    struct CpuCodeArtifactResult {
        uint32_t abiVersion = kCpuCodeProviderAbiVersion;
        uint32_t structSize = sizeof(CpuCodeArtifactResult);
        CpuCodeArtifactPayload payload{};
        CompileError error{CompileErrorCode::CompilationFailed, 0};
    };

    using CpuCodeArtifactBytesDestroy = void (*)(void* state) noexcept;

    /** @brief Provider-private ownership transfer for reconstructible native object bytes. */
    struct CpuCodeArtifactBytes {
        const std::byte* data = nullptr;
        uint64_t size = 0;
        void* state = nullptr;
        CpuCodeArtifactBytesDestroy destroy = nullptr;

        /** @brief Return the exported native object bytes without copying. */
        [[nodiscard]] constexpr std::span<const std::byte> Bytes() const noexcept {
            if (size > std::numeric_limits<size_t>::max()) {
                return {};
            }
            return {data, static_cast<size_t>(size)};
        }
    };

    /** @brief Provider-allocated native object bytes transferred to the CPU backend. */
    struct CpuCodeExportResult {
        uint32_t abiVersion = kCpuCodeProviderAbiVersion;
        uint32_t structSize = sizeof(CpuCodeExportResult);
        CpuCodeArtifactBytes payload{};
        CompileError error{CompileErrorCode::ArtifactExportFailed, 0};
    };

    using CpuCodeProviderDestroy = void (*)(void* state) noexcept;
    using CpuCodeCompile = void (*)(void* state, const CpuCodeCompileRequest* request,
                                    CpuCodeArtifactResult* result) noexcept;
    using CpuCodeImport = void (*)(void* state, const CpuCodeImportRequest* request,
                                   CpuCodeArtifactResult* result) noexcept;
    using CpuCodeExport = void (*)(void* state, const CpuCodeExportRequest* request,
                                   CpuCodeExportResult* result) noexcept;

    /** @brief ABI-version-1 callback table for an optional native CPU compiler such as LLVM ORC. */
    struct CpuCodeProviderApiV1 {
        uint32_t abiVersion = kCpuCodeProviderAbiVersion;
        uint32_t structSize = sizeof(CpuCodeProviderApiV1);
        SemanticHash compilerIdentity{};
        SemanticHash driverIdentity{};
        void* state = nullptr;
        CpuCodeProviderDestroy destroy = nullptr;
        CpuCodeCompile compile = nullptr;
        CpuCodeImport importArtifact = nullptr;
        CpuCodeExport exportArtifact = nullptr;
    };

    namespace Detail {

        inline void ReleaseCpuCodeArtifact(CpuCodeArtifactPayload payload) noexcept {
            if (payload.state != nullptr && payload.destroy != nullptr) {
                payload.destroy(payload.state);
            }
        }

        inline void ReleaseCpuCodeArtifactBytes(CpuCodeArtifactBytes payload) noexcept {
            if (payload.state != nullptr && payload.destroy != nullptr) {
                payload.destroy(payload.state);
            }
        }

        struct CpuCodeProviderControl {
            CpuCodeProviderControl(CpuCodeProviderApiV1 value, BackendCodeLease codeOwner) noexcept
                : api(value), code(std::move(codeOwner)) {}

            ~CpuCodeProviderControl() noexcept { api.destroy(api.state); }

            CpuCodeProviderApiV1 api;
            BackendCodeLease code;
        };

    } // namespace Detail

    class CpuCodeProvider;

    /** @brief Move-only native artifact retaining its provider code until destruction. */
    class CpuCodeArtifact final {
    public:
        CpuCodeArtifact() noexcept = default;
        CpuCodeArtifact(const CpuCodeArtifact&) = delete;
        CpuCodeArtifact& operator=(const CpuCodeArtifact&) = delete;

        CpuCodeArtifact(CpuCodeArtifact&& other) noexcept
            : provider_(std::move(other.provider_)), payload_(std::exchange(other.payload_, {})) {}

        CpuCodeArtifact& operator=(CpuCodeArtifact&& other) noexcept {
            if (this != &other) {
                Reset();
                provider_ = std::move(other.provider_);
                payload_ = std::exchange(other.payload_, {});
            }
            return *this;
        }

        ~CpuCodeArtifact() noexcept { Reset(); }

        /** @brief Return whether this artifact owns native state and one executable range entrypoint. */
        [[nodiscard]] explicit operator bool() const noexcept {
            return provider_ != nullptr && payload_.state != nullptr && payload_.destroy != nullptr &&
                   payload_.execute != nullptr;
        }

        /** @brief Enter generated code exactly once for the already validated range. */
        void Execute(const CpuCodeInvocation& invocation) const noexcept {
            assert(static_cast<bool>(*this));
            payload_.execute(payload_.state, &invocation);
        }

    private:
        friend class CpuCodeProvider;

        CpuCodeArtifact(std::shared_ptr<Detail::CpuCodeProviderControl> provider,
                        CpuCodeArtifactPayload payload) noexcept
            : provider_(std::move(provider)), payload_(payload) {}

        void Reset() noexcept {
            Detail::ReleaseCpuCodeArtifact(payload_);
            payload_ = {};
            provider_.reset();
        }

        std::shared_ptr<Detail::CpuCodeProviderControl> provider_{};
        CpuCodeArtifactPayload payload_{};
    };

    /** @brief Validated native-code provider facade with explicit state and executable-code ownership. */
    class CpuCodeProvider final {
    public:
        CpuCodeProvider() noexcept = default;
        CpuCodeProvider(const CpuCodeProvider&) = delete;
        CpuCodeProvider& operator=(const CpuCodeProvider&) = delete;
        CpuCodeProvider(CpuCodeProvider&&) noexcept = default;
        CpuCodeProvider& operator=(CpuCodeProvider&&) noexcept = default;

        /** @brief Adopt one provider state while retaining every callback and artifact destructor implementation. */
        [[nodiscard]] static std::expected<CpuCodeProvider, CompileError> Adopt(CpuCodeProviderApiV1 api,
                                                                                BackendCodeLease code) noexcept {
            const bool valid = ValidCpuCodeProviderAbi(api.abiVersion, api.structSize, sizeof(CpuCodeProviderApiV1)) &&
                               ValidSemanticHash(api.compilerIdentity) && ValidSemanticHash(api.driverIdentity) &&
                               api.state != nullptr && api.destroy != nullptr && api.compile != nullptr &&
                               api.importArtifact != nullptr && api.exportArtifact != nullptr && code;
            if (!valid) {
                if (api.state != nullptr && api.destroy != nullptr) {
                    api.destroy(api.state);
                }
                return std::unexpected(CompileError{CompileErrorCode::InvalidApi, 0});
            }
            try {
                return CpuCodeProvider{std::make_shared<Detail::CpuCodeProviderControl>(api, std::move(code))};
            } catch (const std::bad_alloc&) {
                api.destroy(api.state);
                return std::unexpected(CompileError{CompileErrorCode::OutOfMemory, 0});
            }
        }

        /** @brief Return whether this facade owns one complete provider. */
        [[nodiscard]] explicit operator bool() const noexcept { return control_ != nullptr; }

        /** @brief Return the semantic compiler identity participating in executable keys. */
        [[nodiscard]] SemanticHash CompilerIdentity() const noexcept {
            assert(control_ != nullptr);
            return control_->api.compilerIdentity;
        }

        /** @brief Return the native runtime identity invalidating incompatible persisted objects. */
        [[nodiscard]] SemanticHash DriverIdentity() const noexcept {
            assert(control_ != nullptr);
            return control_->api.driverIdentity;
        }

        /** @brief Compile one canonical KernelProgram into target-native executable code. */
        [[nodiscard]] std::expected<CpuCodeArtifact, CompileError>
        Compile(const CpuCodeCompileRequest& request) const noexcept {
            if (!ValidRequest(request)) {
                return std::unexpected(CompileError{CompileErrorCode::InvalidRequest, 0});
            }
            CpuCodeArtifactResult result{};
            control_->api.compile(control_->api.state, &request, &result);
            return AdoptResult(result);
        }

        /** @brief Reconstruct target-native executable code from provider-private object bytes. */
        [[nodiscard]] std::expected<CpuCodeArtifact, CompileError>
        Import(const CpuCodeCompileRequest& compileRequest, std::span<const std::byte> objectBytes) const noexcept {
            if (!ValidRequest(compileRequest) || objectBytes.empty()) {
                return std::unexpected(CompileError{CompileErrorCode::InvalidRequest, 0});
            }
            const CpuCodeImportRequest request{
                .compileRequest = &compileRequest, .objectData = objectBytes.data(), .objectSize = objectBytes.size()};
            CpuCodeArtifactResult result{};
            control_->api.importArtifact(control_->api.state, &request, &result);
            return AdoptResult(result);
        }

        /** @brief Copy a native artifact into provider-private reconstructible object bytes. */
        [[nodiscard]] std::expected<std::vector<std::byte>, CompileError>
        Export(const CpuCodeArtifact& artifact) const noexcept {
            if (control_ == nullptr || !artifact || artifact.provider_.get() != control_.get()) {
                return std::unexpected(CompileError{CompileErrorCode::InvalidRequest, 0});
            }
            const CpuCodeExportRequest request{.artifactState = artifact.payload_.state};
            CpuCodeExportResult result{};
            control_->api.exportArtifact(control_->api.state, &request, &result);
            if (!ValidCpuCodeProviderAbi(result.abiVersion, result.structSize, sizeof(CpuCodeExportResult)) ||
                !Detail::ValidBackendCompileErrorCode(result.error.code)) {
                Detail::ReleaseCpuCodeArtifactBytes(result.payload);
                return std::unexpected(CompileError{CompileErrorCode::InvalidArtifact, 0});
            }
            if (result.error.code != CompileErrorCode::None) {
                const CompileError error = result.error;
                Detail::ReleaseCpuCodeArtifactBytes(result.payload);
                return std::unexpected(error);
            }
            if (result.payload.data == nullptr || result.payload.size == 0 || result.payload.state == nullptr ||
                result.payload.destroy == nullptr || result.payload.size > std::numeric_limits<size_t>::max()) {
                Detail::ReleaseCpuCodeArtifactBytes(result.payload);
                return std::unexpected(CompileError{CompileErrorCode::InvalidArtifact, 0});
            }
            try {
                std::vector<std::byte> bytes(result.payload.Bytes().begin(), result.payload.Bytes().end());
                Detail::ReleaseCpuCodeArtifactBytes(result.payload);
                return bytes;
            } catch (const std::bad_alloc&) {
                Detail::ReleaseCpuCodeArtifactBytes(result.payload);
                return std::unexpected(CompileError{CompileErrorCode::OutOfMemory, 0});
            } catch (...) {
                Detail::ReleaseCpuCodeArtifactBytes(result.payload);
                return std::unexpected(CompileError{CompileErrorCode::InvalidArtifact, 0});
            }
        }

    private:
        explicit CpuCodeProvider(std::shared_ptr<Detail::CpuCodeProviderControl> control) noexcept
            : control_(std::move(control)) {}

        [[nodiscard]] bool ValidRequest(const CpuCodeCompileRequest& request) const noexcept {
            return control_ != nullptr &&
                   ValidCpuCodeProviderAbi(request.abiVersion, request.structSize, sizeof(CpuCodeCompileRequest)) &&
                   request.programData != nullptr && request.programSize != 0 &&
                   request.programSize <= std::numeric_limits<size_t>::max() && request.target.device.Valid() &&
                   request.target.backend == BackendKind::Cpu &&
                   request.target.compilerIdentity == control_->api.compilerIdentity &&
                   request.target.driverIdentity == control_->api.driverIdentity &&
                   Detail::HashBytes(request.ProgramBytes()) == request.programHash;
        }

        [[nodiscard]] std::expected<CpuCodeArtifact, CompileError>
        AdoptResult(CpuCodeArtifactResult& result) const noexcept {
            if (!ValidCpuCodeProviderAbi(result.abiVersion, result.structSize, sizeof(CpuCodeArtifactResult)) ||
                !Detail::ValidBackendCompileErrorCode(result.error.code)) {
                Detail::ReleaseCpuCodeArtifact(result.payload);
                return std::unexpected(CompileError{CompileErrorCode::InvalidArtifact, 0});
            }
            if (result.error.code != CompileErrorCode::None) {
                const CompileError error = result.error;
                Detail::ReleaseCpuCodeArtifact(result.payload);
                return std::unexpected(error);
            }
            if (result.payload.state == nullptr || result.payload.destroy == nullptr ||
                result.payload.execute == nullptr) {
                Detail::ReleaseCpuCodeArtifact(result.payload);
                return std::unexpected(CompileError{CompileErrorCode::InvalidArtifact, 0});
            }
            return CpuCodeArtifact{control_, std::exchange(result.payload, {})};
        }

        std::shared_ptr<Detail::CpuCodeProviderControl> control_{};
    };

    static_assert(std::is_trivially_copyable_v<CpuCodeBuffer>);
    static_assert(std::is_trivially_copyable_v<CpuCodeScalar>);
    static_assert(std::is_trivially_copyable_v<CpuCodeInvocation>);
    static_assert(std::is_trivially_copyable_v<CpuCodeCompileRequest>);
    static_assert(std::is_trivially_copyable_v<CpuCodeImportRequest>);
    static_assert(std::is_trivially_copyable_v<CpuCodeExportRequest>);
    static_assert(std::is_trivially_copyable_v<CpuCodeArtifactPayload>);
    static_assert(std::is_trivially_copyable_v<CpuCodeArtifactResult>);
    static_assert(std::is_trivially_copyable_v<CpuCodeArtifactBytes>);
    static_assert(std::is_trivially_copyable_v<CpuCodeExportResult>);
    static_assert(std::is_trivially_copyable_v<CpuCodeProviderApiV1>);
    static_assert(std::is_standard_layout_v<CpuCodeInvocation>);
    static_assert(std::is_standard_layout_v<CpuCodeCompileRequest>);
    static_assert(std::is_standard_layout_v<CpuCodeArtifactBytes>);
    static_assert(std::is_standard_layout_v<CpuCodeProviderApiV1>);

} // namespace Sora::Compute

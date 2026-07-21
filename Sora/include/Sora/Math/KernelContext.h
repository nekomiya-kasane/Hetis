/**
 * @file KernelContext.h
 * @brief Runtime ISA selection and portable JIT-provider boundary for batched mathematical kernels.
 * @ingroup Math
 */
#pragma once

#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <type_traits>
#include <utility>

namespace Sora::Math {

    inline constexpr uint32_t kKernelAbiVersion = 1;

    /** @brief Runtime CPU capabilities relevant to precompiled and JIT-generated mathematical kernels. */
    enum class KernelFeature : uint64_t {
        Scalar = uint64_t{1} << 0,
        Simd128 = uint64_t{1} << 1,
        Simd256 = uint64_t{1} << 2,
        Simd512 = uint64_t{1} << 3,
        Fma = uint64_t{1} << 4,
        X86Sse2 = uint64_t{1} << 5,
        X86Avx2 = uint64_t{1} << 6,
        X86Avx512F = uint64_t{1} << 7,
        ArmNeon = uint64_t{1} << 8,
        ArmSve = uint64_t{1} << 9,
    };

    /** @brief Compact capability set with constexpr subset operations. */
    class KernelFeatureMask {
    public:
        constexpr KernelFeatureMask() noexcept = default;
        constexpr KernelFeatureMask(KernelFeature feature) noexcept : bits_(std::to_underlying(feature)) {}
        constexpr explicit KernelFeatureMask(uint64_t bits) noexcept : bits_(bits) {}

        /** @brief Return the raw stable bit representation. */
        [[nodiscard]] constexpr uint64_t Bits() const noexcept { return bits_; }

        /** @brief Return whether every bit in @p required is present. */
        [[nodiscard]] constexpr bool Contains(KernelFeatureMask required) const noexcept {
            return (bits_ & required.bits_) == required.bits_;
        }

        /** @brief Return the number of represented capabilities. */
        [[nodiscard]] constexpr int Count() const noexcept { return std::popcount(bits_); }

        /** @brief Return whether the set is empty. */
        [[nodiscard]] constexpr bool Empty() const noexcept { return bits_ == 0; }

        friend constexpr bool operator==(KernelFeatureMask, KernelFeatureMask) noexcept = default;

        friend constexpr KernelFeatureMask operator|(KernelFeatureMask left, KernelFeatureMask right) noexcept {
            return KernelFeatureMask(left.bits_ | right.bits_);
        }

        friend constexpr KernelFeatureMask operator&(KernelFeatureMask left, KernelFeatureMask right) noexcept {
            return KernelFeatureMask(left.bits_ & right.bits_);
        }

        constexpr KernelFeatureMask& operator|=(KernelFeatureMask right) noexcept {
            bits_ |= right.bits_;
            return *this;
        }

    private:
        uint64_t bits_ = 0;
    };

    /** @brief Build a feature set from individual feature enumerators. */
    template<typename... Features>
        requires(sizeof...(Features) > 0 && (std::same_as<Features, KernelFeature> && ...))
    [[nodiscard]] constexpr KernelFeatureMask KernelFeatures(Features... features) noexcept {
        return (KernelFeatureMask(features) | ...);
    }

    /** @brief Runtime compilation policy for one kernel resolution. */
    enum class JitMode : uint8_t {
        Disabled,
        Prefer,
        Require,
    };

    /** @brief Runtime policy independent of any concrete compiler backend. */
    struct KernelPolicy {
        JitMode jit = JitMode::Prefer;
        bool deterministic = false;
        size_t minimumJitWorkItems = 4096;
        uint32_t threadBudget = 0;
    };

    /** @brief Scalar element representation crossing the runtime kernel ABI. */
    enum class KernelScalarFormat : uint8_t {
        Unknown,
        Float16,
        Float32,
        Float64,
        Int8,
        Int16,
        Int32,
        Int64,
        UInt8,
        UInt16,
        UInt32,
        UInt64,
    };

    /** @brief Address domain of a runtime kernel buffer. */
    enum class KernelMemorySpace : uint8_t {
        Host,
        Unified,
        Device,
    };

    /** @brief Read-only input buffer for a kernel invocation. */
    struct KernelInput {
        const void* data = nullptr;
        size_t elementCount = 0;
        ptrdiff_t byteStride = 0;
        KernelScalarFormat format = KernelScalarFormat::Unknown;
        KernelMemorySpace memorySpace = KernelMemorySpace::Host;
    };

    /** @brief Writable output buffer for a kernel invocation. */
    struct KernelOutput {
        void* data = nullptr;
        size_t elementCount = 0;
        ptrdiff_t byteStride = 0;
        KernelScalarFormat format = KernelScalarFormat::Unknown;
        KernelMemorySpace memorySpace = KernelMemorySpace::Host;
    };

    /** @brief Allocation-free invocation view consumed by a resolved kernel entry point. */
    struct KernelInvocation {
        const KernelInput* inputs = nullptr;
        size_t inputCount = 0;
        KernelOutput* outputs = nullptr;
        size_t outputCount = 0;
        size_t workItemOffset = 0;
        size_t workItemCount = 0;
        void* scratch = nullptr;
        size_t scratchBytes = 0;

        /** @brief Return input buffers as a non-owning span. */
        [[nodiscard]] constexpr std::span<const KernelInput> Inputs() const noexcept { return {inputs, inputCount}; }

        /** @brief Return output buffers as a non-owning span. */
        [[nodiscard]] constexpr std::span<KernelOutput> Outputs() const noexcept { return {outputs, outputCount}; }
    };

    /** @brief Portable or provider-specific intermediate representation offered to a JIT backend. */
    enum class KernelIrKind : uint8_t {
        None,
        SoraKernelIr,
        SpirV,
        LlvmBitcode,
        WebAssembly,
    };

    /**
     * @brief Versioned, allocation-free request describing a kernel family and optional runtime IR.
     * @details @c familyId and @c semanticHash are stable application-defined identifiers. Providers should cache
     * generated code by the semantic hash, IR bytes, policy, and selected feature mask.
     */
    struct KernelRequest {
        uint32_t abiVersion = kKernelAbiVersion;
        uint32_t structSize = sizeof(KernelRequest);
        uint64_t familyId = 0;
        uint64_t semanticHash = 0;
        KernelIrKind irKind = KernelIrKind::None;
        const std::byte* irData = nullptr;
        size_t irSize = 0;
        size_t workItemCount = 0;
        KernelFeatureMask requiredFeatures{};
        KernelFeatureMask preferredFeatures{};
    };

    /** @brief Origin of a resolved executable. */
    enum class KernelBackendKind : uint8_t {
        Scalar,
        Precompiled,
        Jit,
    };

    using KernelEntry = void (*)(void* state, const KernelInvocation& invocation) noexcept;
    using KernelDestroy = void (*)(void* state) noexcept;

    /** @brief One statically compiled implementation eligible for runtime feature selection. */
    struct KernelCandidate {
        KernelEntry entry = nullptr;
        void* state = nullptr;
        KernelFeatureMask requiredFeatures{};
        KernelBackendKind backend = KernelBackendKind::Precompiled;
        size_t minimumWorkItems = 0;
        uint32_t priority = 0;
        bool deterministic = true;
    };

    /** @brief Typed resolution and provider failure. */
    enum class KernelErrorCode : uint8_t {
        InvalidRequest,
        RequiredFeatureUnavailable,
        NoCompatibleKernel,
        JitUnavailable,
        JitCompilationFailed,
        JitProducedInvalidExecutable,
    };

    /** @brief Trivially copyable kernel error with a provider-defined detail value. */
    struct KernelError {
        KernelErrorCode code = KernelErrorCode::InvalidRequest;
        uint64_t detail = 0;

        friend constexpr bool operator==(KernelError, KernelError) noexcept = default;
    };

    /** @brief ABI result returned by a non-owning JIT provider reference. */
    struct KernelCompileResult {
        KernelEntry entry = nullptr;
        void* state = nullptr;
        KernelDestroy destroy = nullptr;
        KernelFeatureMask requiredFeatures{};
        KernelError error{KernelErrorCode::JitCompilationFailed, 0};
        bool deterministic = false;
    };

    using JitCompile = KernelCompileResult (*)(void* providerState, const KernelRequest& request,
                                               KernelFeatureMask hostFeatures, const KernelPolicy& policy) noexcept;

    /** @brief Non-owning callback table for a concrete runtime compiler and its code cache. */
    struct JitProviderRef {
        void* state = nullptr;
        JitCompile compile = nullptr;
        uint32_t abiVersion = kKernelAbiVersion;

        /** @brief Return whether this provider can be called by the current ABI. */
        [[nodiscard]] constexpr bool Valid() const noexcept {
            return compile != nullptr && abiVersion == kKernelAbiVersion;
        }
    };

    static_assert(std::is_trivially_copyable_v<KernelFeatureMask>);
    static_assert(std::is_trivially_copyable_v<KernelInput>);
    static_assert(std::is_trivially_copyable_v<KernelOutput>);
    static_assert(std::is_trivially_copyable_v<KernelInvocation>);
    static_assert(std::is_trivially_copyable_v<KernelRequest>);
    static_assert(std::is_trivially_copyable_v<KernelCandidate>);
    static_assert(std::is_trivially_copyable_v<KernelCompileResult>);
    static_assert(std::is_trivially_copyable_v<JitProviderRef>);

    static_assert(std::is_standard_layout_v<KernelFeatureMask>);
    static_assert(std::is_standard_layout_v<KernelInvocation>);
    static_assert(std::is_standard_layout_v<KernelRequest>);
    static_assert(std::is_standard_layout_v<KernelCompileResult>);
    static_assert(std::is_standard_layout_v<JitProviderRef>);

    /** @brief Move-only RAII handle to either a static or JIT-generated kernel entry point. */
    class KernelExecutable {
    public:
        constexpr KernelExecutable() noexcept = default;
        KernelExecutable(const KernelExecutable&) = delete;
        KernelExecutable& operator=(const KernelExecutable&) = delete;

        constexpr KernelExecutable(KernelExecutable&& other) noexcept
            : entry_(std::exchange(other.entry_, nullptr)),
              state_(std::exchange(other.state_, nullptr)),
              destroy_(std::exchange(other.destroy_, nullptr)),
              requiredFeatures_(other.requiredFeatures_),
              backend_(other.backend_) {}

        constexpr KernelExecutable& operator=(KernelExecutable&& other) noexcept {
            if (this != &other) {
                Reset();
                entry_ = std::exchange(other.entry_, nullptr);
                state_ = std::exchange(other.state_, nullptr);
                destroy_ = std::exchange(other.destroy_, nullptr);
                requiredFeatures_ = other.requiredFeatures_;
                backend_ = other.backend_;
            }
            return *this;
        }

        constexpr ~KernelExecutable() { Reset(); }

        /** @brief Return whether this handle contains a callable entry point. */
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return entry_ != nullptr; }

        /** @brief Invoke the already-resolved kernel without further backend selection. */
        constexpr void operator()(const KernelInvocation& invocation) const noexcept {
            assert(entry_ != nullptr);
            entry_(state_, invocation);
        }

        /** @brief Return the executable origin. */
        [[nodiscard]] constexpr KernelBackendKind Backend() const noexcept { return backend_; }

        /** @brief Return features required by this executable. */
        [[nodiscard]] constexpr KernelFeatureMask RequiredFeatures() const noexcept { return requiredFeatures_; }

    private:
        friend class KernelContext;

        constexpr KernelExecutable(KernelEntry entry, void* state, KernelDestroy destroy,
                                   KernelFeatureMask requiredFeatures, KernelBackendKind backend) noexcept
            : entry_(entry), state_(state), destroy_(destroy), requiredFeatures_(requiredFeatures), backend_(backend) {}

        constexpr void Reset() noexcept {
            if (destroy_ != nullptr) {
                destroy_(state_);
            }
            entry_ = nullptr;
            state_ = nullptr;
            destroy_ = nullptr;
        }

        KernelEntry entry_ = nullptr;
        void* state_ = nullptr;
        KernelDestroy destroy_ = nullptr;
        KernelFeatureMask requiredFeatures_{};
        KernelBackendKind backend_ = KernelBackendKind::Scalar;
    };

    /** @brief Detect host ISA capabilities without assuming the current translation unit target. */
    [[nodiscard]] KernelFeatureMask DetectHostKernelFeatures() noexcept;

    /**
     * @brief Resolve precompiled or runtime-compiled implementations once per kernel family or batch.
     * @details Resolution never occurs per lane or element. The JIT provider is non-owning and is responsible for
     * compilation caching, executable-memory policy, and backend-specific lifetime management.
     */
    class KernelContext {
    public:
        /** @brief Construct from detected host capabilities and an optional JIT provider. */
        explicit KernelContext(JitProviderRef provider = {}) noexcept
            : KernelContext(DetectHostKernelFeatures(), provider) {}

        /** @brief Construct from an explicit capability set, primarily for schedulers and tests. */
        constexpr KernelContext(KernelFeatureMask features, JitProviderRef provider = {}) noexcept
            : features_(features), provider_(provider) {}

        /** @brief Return the capabilities available to candidate and JIT selection. */
        [[nodiscard]] constexpr KernelFeatureMask Features() const noexcept { return features_; }

        /** @brief Resolve one executable according to request, candidates, and policy. */
        [[nodiscard]] std::expected<KernelExecutable, KernelError>
        Resolve(const KernelRequest& request, std::span<const KernelCandidate> candidates,
                const KernelPolicy& policy = {}) const noexcept;

    private:
        KernelFeatureMask features_;
        JitProviderRef provider_;
    };

} // namespace Sora::Math

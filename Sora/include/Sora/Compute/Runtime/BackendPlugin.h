/**
 * @file BackendPlugin.h
 * @brief Dynamic backend plugin ownership and transactional runtime discovery adapter.
 * @ingroup Compute
 */
#pragma once

#include <Sora/Compute/Runtime/BackendPluginAbi.h>
#include <Sora/Compute/Runtime/RuntimeDiscovery.h>
#include <Sora/Core/PAL/Module.h>

#include <cstdint>
#include <expected>
#include <memory>

namespace Sora::Compute {

    /** @brief Failure while loading or adopting one backend plugin root. */
    enum class BackendPluginLoadErrorCode : uint8_t {
        InvalidModule,
        MissingNegotiationExport,
        InvalidNegotiationResult,
        IncompatibleAbi,
        InvalidPluginApi,
        OutOfMemory,
    };

    /** @brief Plugin loading failure preserving a well-formed plugin diagnostic when available. */
    struct BackendPluginLoadError {
        BackendPluginLoadErrorCode code = BackendPluginLoadErrorCode::InvalidModule;
        BackendPluginCallResult plugin{};

        friend constexpr bool operator==(BackendPluginLoadError, BackendPluginLoadError) noexcept = default;
    };

    /** @brief Host-adapter diagnostics encoded in RuntimeProviderError for failed plugin discovery. */
    enum class BackendPluginProviderErrorCode : uint64_t {
        InvalidEnumerationResult = 1,
        IncompleteEnumeration,
        InvalidBundle,
        InvalidService,
        InvalidObserver,
        InvalidQueue,
        InvalidAllocator,
        InvalidHostMemoryAccessor,
        InvalidTransferEngine,
        InvalidCompiler,
        InvalidPipelineFactory,
        CapacityExceeded,
        OutOfMemory,
        SynchronizationFailed,
    };

    inline constexpr uint64_t kBackendPluginProviderErrorDomain = 0x534F52415F504C47ULL;

    /** @brief Owning, callable runtime provider produced from one negotiated plugin module. */
    class RuntimeBackendPlugin final {
        struct Control;

    public:
        /** @brief Validate and adopt a negotiated root while retaining its executable code. */
        [[nodiscard]] static std::expected<RuntimeBackendPlugin, BackendPluginLoadError>
        Adopt(BackendPluginApiV1 api, BackendCodeLease code) noexcept;

        /** @brief Return the stable provider identity exported by this plugin. */
        [[nodiscard]] RuntimeProviderId Provider() const noexcept;

        /** @brief Enumerate one complete snapshot and project it into an owning discovery contribution. */
        [[nodiscard]] std::expected<RuntimeDiscoveryContribution, RuntimeDiscoveryError> operator()() const noexcept;

    private:
        explicit RuntimeBackendPlugin(std::shared_ptr<Control> control) noexcept : control_(std::move(control)) {}

        std::shared_ptr<Control> control_;
    };

    /** @brief Resolve, negotiate, and retain one backend plugin from an already loaded module. */
    [[nodiscard]] std::expected<RuntimeBackendPlugin, BackendPluginLoadError>
    LoadRuntimeBackendPlugin(PAL::ModulePtr module) noexcept;

    static_assert(RuntimeDiscoveryProvider<RuntimeBackendPlugin>);

} // namespace Sora::Compute

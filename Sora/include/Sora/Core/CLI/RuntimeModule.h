/**
 * @file RuntimeModule.h
 * @brief C-compatible runtime CLI module ABI and validated import into command fragments.
 * @ingroup Core
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <Sora/Core/CLI/Fragment.h>
#include <Sora/Core/FixedString.h>

namespace Sora::CLI {

    inline constexpr std::uint32_t kRuntimeModuleMagic = 0x494C4353;
    inline constexpr std::uint16_t kRuntimeModuleAbiMajor = 1;
    inline constexpr std::uint16_t kRuntimeModuleAbiMinor = 0;
    inline constexpr std::size_t kMaxRuntimeModuleCommands = 1024;
    inline constexpr std::string_view kRuntimeModuleEntryName = "SoraCliRuntimeModule";

    /** @brief C-compatible non-owning UTF-8 string view used by the runtime module ABI. */
    struct RuntimeStringView {
        const char* data = nullptr; /**< First byte, or null only when @ref size is zero. */
        std::uint32_t size = 0;     /**< Number of bytes, excluding any terminator. */
    };

    /** @brief Convert a bounded C++ string view into its runtime ABI representation. */
    [[nodiscard]] constexpr RuntimeStringView RuntimeString(std::string_view text) noexcept {
        return {.data = text.data(), .size = static_cast<std::uint32_t>(text.size())};
    }

    /** @brief C-compatible command-line argument view accepted by runtime module thunks. */
    struct RuntimeArgvView {
        int argc = 0;                                      /**< Raw argument count when @ref argv is used. */
        const char* const* argv = nullptr;                  /**< Raw NUL-terminated arguments. */
        const RuntimeStringView* tokens = nullptr; /**< Length-delimited tokens when raw argv is unavailable. */
        std::uint32_t tokenCount = 0;                       /**< Number of entries in @ref tokens. */
        RuntimeStringView commandPrefix{};                  /**< Host path preceding the module command tree. */
    };

    /** @brief C-compatible command metadata exported by a runtime module. */
    struct RuntimeCommandDescriptor {
        RuntimeStringView name{};  /**< Top-level command path segment. */
        RuntimeStringView about{}; /**< Human-readable command summary. */
    };

    /** @brief Self-describing header placed first in every runtime module descriptor. */
    struct RuntimeAbiHeader {
        std::uint32_t magic = kRuntimeModuleMagic;                  /**< Runtime descriptor magic. */
        std::uint16_t major = kRuntimeModuleAbiMajor;               /**< Breaking ABI generation. */
        std::uint16_t minor = kRuntimeModuleAbiMinor;               /**< Backward-compatible ABI revision. */
        std::uint32_t headerSize = sizeof(RuntimeAbiHeader);        /**< Size known by the producer. */
        std::uint32_t descriptorSize = 0;                           /**< Size of the complete descriptor. */
        std::uint64_t requiredCapabilities = 0;                     /**< Capabilities required from the host. */
        std::uint64_t optionalCapabilities = 0;                     /**< Capabilities used when available. */
    };

    /** @brief Fixed-width completion category transported across the runtime module ABI. */
    enum class RuntimeInvocationStatus : std::uint32_t {
        Completed,
        InvalidArguments,
        Failed,
    };

    /** @brief C-compatible result returned by a runtime module invocation thunk. */
    struct RuntimeInvocationResult {
        RuntimeInvocationStatus status = RuntimeInvocationStatus::Completed; /**< Completion category. */
        std::int32_t exitCode = 0;                                      /**< Process-style result code. */
        RuntimeStringView diagnostic{};                                 /**< Provider-owned text copied by the host. */
    };

    using RuntimeInvokeFn = RuntimeInvocationResult (*)(const void* state, const RuntimeArgvView* argv) noexcept;

    /** @brief C-compatible module descriptor returned from @ref kRuntimeModuleEntryName. */
    struct RuntimeModuleDescriptor {
        RuntimeAbiHeader header{};                       /**< Version, size, and capability contract. */
        RuntimeStringView provider{};                    /**< Stable module identity used by provenance. */
        const RuntimeCommandDescriptor* commands = nullptr; /**< Contiguous exported top-level commands. */
        std::uint32_t commandCount = 0;                  /**< Number of entries in @ref commands. */
        const void* state = nullptr;                     /**< Opaque state passed back to @ref invoke. */
        RuntimeInvokeFn invoke = nullptr;                /**< Noexcept parse and action vtable entry. */
    };

    using RuntimeModuleEntry = const RuntimeModuleDescriptor*() noexcept;

    /** @brief Failure category produced while validating an external runtime module descriptor. */
    enum class RuntimeAbiErrorKind : std::uint8_t {
        MissingDescriptor,
        InvalidMagic,
        IncompatibleVersion,
        InvalidSize,
        UnsupportedCapabilities,
        MissingOwner,
        InvalidProvider,
        InvalidCommands,
        InvalidInvocation,
    };

    /** @brief Structured runtime ABI import failure, separate from startup-link and parse errors. */
    struct RuntimeAbiError {
        RuntimeAbiErrorKind kind = RuntimeAbiErrorKind::MissingDescriptor; /**< Failure category. */
        std::string detail;                                                /**< Human-readable validation detail. */

        /** @brief Return a process exit code suitable for module import failures. */
        [[nodiscard]] constexpr int ExitCode() const noexcept { return 70; }

        /** @brief Format this import failure for a human operator. */
        [[nodiscard]] std::string Message() const { return "runtime CLI module ABI error: " + detail; }
    };

    namespace Detail {

        [[nodiscard]] inline bool ValidRuntimeString(RuntimeStringView text) noexcept {
            return text.size == 0 || text.data != nullptr;
        }

        [[nodiscard]] inline std::string CopyRuntimeString(RuntimeStringView text) {
            return text.size == 0 ? std::string{} : std::string{text.data, text.size};
        }

        [[nodiscard]] constexpr RuntimeInvocationStatus ToRuntimeStatus(InvocationStatus status) noexcept {
            switch (status) {
            case InvocationStatus::Completed:
                return RuntimeInvocationStatus::Completed;
            case InvocationStatus::InvalidArguments:
                return RuntimeInvocationStatus::InvalidArguments;
            case InvocationStatus::Failed:
                return RuntimeInvocationStatus::Failed;
            }
            return RuntimeInvocationStatus::Failed;
        }

        struct ImportedRuntimeState {
            struct CommandStorage {
                std::string name;
                std::string about;
            };

            std::string provider;
            std::vector<CommandStorage> commandStorage;
            std::vector<FragmentCommand> commands;
            const void* moduleState = nullptr;
            RuntimeInvokeFn invoke = nullptr;
            std::shared_ptr<const void> moduleOwner;

            [[nodiscard]] static InvocationResult Invoke(const void* state,
                                                         const FragmentInvocation& invocation) noexcept {
                const auto& imported = *static_cast<const ImportedRuntimeState*>(state);
                try {
                    std::vector<RuntimeStringView> tokenStorage;
                    RuntimeArgvView runtimeArgv{};
                    if (!invocation.argv.tokens.empty()) {
                        tokenStorage.reserve(invocation.argv.tokens.size());
                        for (std::string_view token : invocation.argv.tokens) {
                            tokenStorage.push_back(RuntimeString(token));
                        }
                        runtimeArgv.tokens = tokenStorage.data();
                        runtimeArgv.tokenCount = static_cast<std::uint32_t>(tokenStorage.size());
                    } else {
                        runtimeArgv.argc = invocation.argv.argc;
                        runtimeArgv.argv = invocation.argv.argv;
                    }
                    runtimeArgv.commandPrefix = RuntimeString(invocation.commandPrefix);

                    const RuntimeInvocationResult result = imported.invoke(imported.moduleState, &runtimeArgv);
                    if (!ValidRuntimeString(result.diagnostic)) {
                        return InvocationResult::Failed(1, "runtime CLI module returned an invalid diagnostic view");
                    }
                    InvocationStatus status = InvocationStatus::Failed;
                    switch (result.status) {
                    case RuntimeInvocationStatus::Completed:
                        status = InvocationStatus::Completed;
                        break;
                    case RuntimeInvocationStatus::InvalidArguments:
                        status = InvocationStatus::InvalidArguments;
                        break;
                    case RuntimeInvocationStatus::Failed:
                        status = InvocationStatus::Failed;
                        break;
                    default:
                        return InvocationResult::Failed(1, "runtime CLI module returned an invalid status value");
                    }
                    return {.status = status, .exitCode = result.exitCode,
                            .diagnostic = CopyRuntimeString(result.diagnostic)};
                } catch (const std::exception& error) {
                    return InvocationResult::Failed(1, error.what());
                } catch (...) {
                    return InvocationResult::Failed(1, "runtime CLI module import thunk raised an unknown exception");
                }
            }
        };

    } // namespace Detail

    /**
     * @brief Static producer-side runtime module image built from a reflected subprogram declaration.
     * @tparam Declaration Module-local declaration type.
     * @tparam CommandTree Static command tree declared by @p Declaration.
     */
    template<typename Declaration, typename CommandTree = CommandTreeOf<Declaration>>
    struct StaticRuntimeModule;

    template<typename Declaration, typename... Nodes>
    struct StaticRuntimeModule<Declaration, Commands<Nodes...>> {
        using CommandTreeType = Commands<Nodes...>;
        using SubprogramType = StaticSubprogram<Declaration, CommandTreeType>;

        FixedString<64> provider{}; /**< Provider identity embedded in the exported descriptor. */
        SubprogramType subprogram{}; /**< Typed parser and actions retained inside the producer module. */
        std::array<RuntimeCommandDescriptor, sizeof...(Nodes)> commands{}; /**< ABI command metadata. */

        /** @brief Build a descriptor view whose pointers refer to this static module image. */
        [[nodiscard]] RuntimeModuleDescriptor Descriptor() const& noexcept {
            return {.header = {.descriptorSize = sizeof(RuntimeModuleDescriptor)},
                    .provider = RuntimeString(provider.view()),
                    .commands = commands.data(),
                    .commandCount = static_cast<std::uint32_t>(commands.size()),
                    .state = std::addressof(subprogram),
                    .invoke = &Invoke};
        }

        RuntimeModuleDescriptor Descriptor() const&& = delete;

    private:
        [[nodiscard]] static RuntimeInvocationResult Invoke(const void* state,
                                                            const RuntimeArgvView* runtimeArgv) noexcept {
            thread_local std::string diagnostic;
            diagnostic.clear();
            try {
                if (runtimeArgv == nullptr || runtimeArgv->argc < 0 ||
                    (runtimeArgv->argc > 0 && runtimeArgv->argv == nullptr) ||
                    (runtimeArgv->tokenCount > 0 && runtimeArgv->tokens == nullptr) ||
                    !Detail::ValidRuntimeString(runtimeArgv->commandPrefix)) {
                    diagnostic = "runtime CLI module received an invalid argv view";
                    return {.status = RuntimeInvocationStatus::Failed,
                            .exitCode = 1,
                            .diagnostic = RuntimeString(diagnostic)};
                }

                std::vector<std::string_view> tokenStorage;
                ArgvView argv{};
                if (runtimeArgv->tokens != nullptr) {
                    tokenStorage.reserve(runtimeArgv->tokenCount);
                    for (std::uint32_t index = 0; index < runtimeArgv->tokenCount; ++index) {
                        const RuntimeStringView token = runtimeArgv->tokens[index];
                        if (!Detail::ValidRuntimeString(token)) {
                            diagnostic = "runtime CLI module received an invalid token view";
                            return {.status = RuntimeInvocationStatus::Failed,
                                    .exitCode = 1,
                                    .diagnostic = RuntimeString(diagnostic)};
                        }
                        tokenStorage.emplace_back(token.data, token.size);
                    }
                    argv.tokens = tokenStorage;
                } else {
                    argv.argc = runtimeArgv->argc;
                    argv.argv = runtimeArgv->argv;
                }

                const std::string_view commandPrefix =
                    runtimeArgv->commandPrefix.size == 0
                        ? std::string_view{}
                        : std::string_view{runtimeArgv->commandPrefix.data, runtimeArgv->commandPrefix.size};
                InvocationResult result = static_cast<const SubprogramType*>(state)->Execute(
                    FragmentInvocation{.argv = argv, .commandPrefix = commandPrefix});
                diagnostic = std::move(result.diagnostic);
                return {.status = Detail::ToRuntimeStatus(result.status),
                        .exitCode = result.exitCode,
                        .diagnostic = RuntimeString(diagnostic)};
            } catch (const std::exception& error) {
                diagnostic = error.what();
                return {.status = RuntimeInvocationStatus::Failed,
                        .exitCode = 1,
                        .diagnostic = RuntimeString(diagnostic)};
            } catch (...) {
                diagnostic = "runtime CLI module raised an unknown exception";
                return {.status = RuntimeInvocationStatus::Failed,
                        .exitCode = 1,
                        .diagnostic = RuntimeString(diagnostic)};
            }
        }
    };

    /**
     * @brief Compile a reflected subprogram declaration into a producer-side runtime module image.
     * @tparam Declaration Type declaring the module's command tree and schema customisation.
     * @param[in] provider Stable provider identity exposed through runtime ABI provenance.
     */
    template<Concept::ProgramRoot Declaration>
    consteval auto CompileRuntimeModule(std::string_view provider) {
        using CommandTree = CommandTreeOf<Declaration>;
        constexpr std::size_t commandCount = Detail::TopLevelCommandCountV<CommandTree>;
        if (provider.empty() || provider.size() > 64) {
            throw "Sora CLI runtime module provider names must contain between 1 and 64 bytes.";
        }

        auto subprogram = CompileSubprogram<Declaration>();
        std::array<RuntimeCommandDescriptor, commandCount> commands{};
        for (std::size_t index = 0; index < commandCount; ++index) {
            commands[index] = {.name = RuntimeString(subprogram.commands[index].name),
                               .about = RuntimeString(subprogram.commands[index].about)};
        }
        return StaticRuntimeModule<Declaration, CommandTree>{.provider = FixedString<64>{provider},
                                                             .subprogram = subprogram,
                                                             .commands = commands};
    }

    /**
     * @brief Validate and import an external runtime module as an owner-protected command fragment.
     * @param[in] descriptor Descriptor returned by the module's @ref kRuntimeModuleEntryName export.
     * @param[in] owner Lease keeping the dynamic module and all provider-owned pointers alive.
     * @return Same-ABI fragment registration ready for an explicit registry snapshot.
     */
    [[nodiscard]] inline auto ImportRuntimeModule(const RuntimeModuleDescriptor* descriptor,
                                                  std::shared_ptr<const void> owner)
        -> std::expected<FragmentRegistration, RuntimeAbiError> {
        auto fail = [](RuntimeAbiErrorKind kind, std::string detail) {
            return std::unexpected(RuntimeAbiError{.kind = kind, .detail = std::move(detail)});
        };

        if (descriptor == nullptr) {
            return fail(RuntimeAbiErrorKind::MissingDescriptor, "module entry returned null");
        }
        if (descriptor->header.magic != kRuntimeModuleMagic) {
            return fail(RuntimeAbiErrorKind::InvalidMagic, "descriptor magic does not identify a Sora CLI module");
        }
        if (descriptor->header.major != kRuntimeModuleAbiMajor || descriptor->header.minor != kRuntimeModuleAbiMinor) {
            return fail(RuntimeAbiErrorKind::IncompatibleVersion, "module ABI version is not supported by this host");
        }
        if (descriptor->header.headerSize != sizeof(RuntimeAbiHeader) ||
            descriptor->header.descriptorSize != sizeof(RuntimeModuleDescriptor)) {
            return fail(RuntimeAbiErrorKind::InvalidSize, "descriptor structure sizes do not match this ABI");
        }
        if (descriptor->header.requiredCapabilities != 0) {
            return fail(RuntimeAbiErrorKind::UnsupportedCapabilities,
                        "module requires runtime capabilities not implemented by this host");
        }
        if (!owner) {
            return fail(RuntimeAbiErrorKind::MissingOwner, "runtime modules require a non-empty owner lease");
        }
        if (!Detail::ValidRuntimeString(descriptor->provider) || descriptor->provider.size == 0 ||
            descriptor->provider.size > 64) {
            return fail(RuntimeAbiErrorKind::InvalidProvider, "provider identity is empty, invalid, or too long");
        }
        if (descriptor->commandCount == 0 || descriptor->commandCount > kMaxRuntimeModuleCommands ||
            descriptor->commands == nullptr) {
            return fail(RuntimeAbiErrorKind::InvalidCommands, "command table is empty, missing, or exceeds limits");
        }
        if (descriptor->state == nullptr || descriptor->invoke == nullptr) {
            return fail(RuntimeAbiErrorKind::InvalidInvocation, "module state or invocation vtable is missing");
        }

        auto state = std::make_shared<Detail::ImportedRuntimeState>();
        state->provider = Detail::CopyRuntimeString(descriptor->provider);
        state->commandStorage.reserve(descriptor->commandCount);
        state->moduleState = descriptor->state;
        state->invoke = descriptor->invoke;
        state->moduleOwner = std::move(owner);

        for (std::uint32_t index = 0; index < descriptor->commandCount; ++index) {
            const RuntimeCommandDescriptor& command = descriptor->commands[index];
            if (!Detail::ValidRuntimeString(command.name) || !Detail::ValidRuntimeString(command.about) ||
                command.name.size == 0 || command.name.size > 64 || command.about.size > 256) {
                return fail(RuntimeAbiErrorKind::InvalidCommands,
                            "a command name or description is invalid or exceeds schema limits");
            }
            state->commandStorage.push_back({.name = Detail::CopyRuntimeString(command.name),
                                             .about = Detail::CopyRuntimeString(command.about)});
        }

        state->commands.reserve(state->commandStorage.size());
        for (const Detail::ImportedRuntimeState::CommandStorage& command : state->commandStorage) {
            state->commands.push_back({.name = command.name, .about = command.about});
        }

        CommandFragment fragment{.provider = state->provider,
                                 .commands = state->commands,
                                 .state = state.get(),
                                 .invoke = &Detail::ImportedRuntimeState::Invoke,
                                 .source = CommandSource::RuntimeModule};
        return FragmentRegistration{.fragment = fragment, .owner = std::move(state)};
    }

    static_assert(std::is_standard_layout_v<RuntimeStringView> && std::is_trivially_copyable_v<RuntimeStringView>);
    static_assert(std::is_standard_layout_v<RuntimeArgvView> && std::is_trivially_copyable_v<RuntimeArgvView>);
    static_assert(std::is_standard_layout_v<RuntimeCommandDescriptor> &&
                  std::is_trivially_copyable_v<RuntimeCommandDescriptor>);
    static_assert(std::is_standard_layout_v<RuntimeAbiHeader> && std::is_trivially_copyable_v<RuntimeAbiHeader>);
    static_assert(std::is_same_v<std::underlying_type_t<RuntimeInvocationStatus>, std::uint32_t>);
    static_assert(std::is_standard_layout_v<RuntimeInvocationResult> &&
                  std::is_trivially_copyable_v<RuntimeInvocationResult>);
    static_assert(std::is_standard_layout_v<RuntimeModuleDescriptor> &&
                  std::is_trivially_copyable_v<RuntimeModuleDescriptor>);

} // namespace Sora::CLI

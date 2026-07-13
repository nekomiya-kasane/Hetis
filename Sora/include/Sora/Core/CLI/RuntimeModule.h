/**
 * @file RuntimeModule.h
 * @brief C-compatible runtime CLI module ABI and validated import into mounted command fragments.
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
    inline constexpr std::uint16_t kRuntimeModuleAbiMajor = 2;
    inline constexpr std::uint16_t kRuntimeModuleAbiMinor = 0;
    inline constexpr std::size_t kMaxRuntimeModuleCommands = 1024;
    inline constexpr std::size_t kMaxRuntimeModuleOptions = 4096;
    inline constexpr std::size_t kMaxRuntimeModuleOperands = 4096;
    inline constexpr std::size_t kMaxRuntimeInvocationTokens = 1u << 20;
    inline constexpr std::size_t kMaxRuntimeDiagnosticBytes = 1u << 20;
    inline constexpr std::string_view kRuntimeModuleEntryName = "SoraCliRuntimeModule";

    /** @brief C-compatible non-owning UTF-8 string view used by the runtime module ABI. */
    struct RuntimeStringView {
        const char* data = nullptr;
        std::uint64_t size = 0;
    };

    /** @brief Convert a bounded C++ string view into its runtime ABI representation. */
    [[nodiscard]] constexpr RuntimeStringView RuntimeString(std::string_view text) noexcept {
        return {.data = text.data(), .size = text.size()};
    }

    /** @brief C-compatible command-line argument view accepted by runtime module thunks. */
    struct RuntimeArgvView {
        int argc = 0;
        const char* const* argv = nullptr;
        const RuntimeStringView* tokens = nullptr;
        std::uint64_t tokenCount = 0;
        RuntimeStringView commandPrefix{};
    };

    /** @brief C-compatible command-tree node metadata exported by a runtime module. */
    struct RuntimeCommandDescriptor {
        std::uint32_t commandId = kInvalidCommandId;
        std::uint32_t parentCommandId = kInvalidCommandId;
        RuntimeStringView name{};
        RuntimeStringView about{};
        std::uint32_t optionBegin = 0;
        std::uint32_t optionCount = 0;
        std::uint32_t operandBegin = 0;
        std::uint32_t operandCount = 0;
    };

    /** @brief C-compatible option metadata used by host-side linked recognition and help. */
    struct RuntimeOptionDescriptor {
        RuntimeStringView longName{};
        RuntimeStringView valueName{};
        RuntimeStringView about{};
        char shortName = '\0';
        std::uint8_t kind = static_cast<std::uint8_t>(OptionKind::Switch);
        std::uint8_t cardinality = static_cast<std::uint8_t>(ValueCardinality::None);
        std::uint8_t required = 0;
        std::uint8_t global = 0;
        std::uint8_t overridesGlobal = 0;
        std::uint8_t reserved[3]{};
    };

    /** @brief C-compatible positional operand metadata used by linked recognition and help. */
    struct RuntimeOperandDescriptor {
        RuntimeStringView name{};
        RuntimeStringView about{};
        std::uint8_t cardinality = static_cast<std::uint8_t>(ValueCardinality::One);
        std::uint8_t reserved[7]{};
    };

    /** @brief Self-describing header placed first in every runtime module descriptor. */
    struct RuntimeAbiHeader {
        std::uint32_t magic = kRuntimeModuleMagic;
        std::uint16_t major = kRuntimeModuleAbiMajor;
        std::uint16_t minor = kRuntimeModuleAbiMinor;
        std::uint32_t headerSize = sizeof(RuntimeAbiHeader);
        std::uint32_t descriptorSize = 0;
        std::uint64_t requiredCapabilities = 0;
        std::uint64_t optionalCapabilities = 0;
    };

    /** @brief Fixed-width completion category transported across the runtime module ABI. */
    enum class RuntimeInvocationStatus : std::uint32_t {
        Completed,
        InvalidArguments,
        Failed,
    };

    /** @brief C-compatible result returned by a runtime module invocation thunk. */
    struct RuntimeInvocationResult {
        RuntimeInvocationStatus status = RuntimeInvocationStatus::Completed;
        std::int32_t exitCode = 0;
        RuntimeStringView diagnostic{};
    };

    using RuntimeInvokeFn = RuntimeInvocationResult (*)(const void* state, const RuntimeArgvView* argv) noexcept;
    using RuntimeValidateValueFn = bool (*)(const void* state, std::uint32_t descriptorId,
                                            RuntimeStringView value) noexcept;

    /** @brief C-compatible complete module descriptor returned from @ref kRuntimeModuleEntryName. */
    struct RuntimeModuleDescriptor {
        RuntimeAbiHeader header{};
        RuntimeStringView provider{};
        const RuntimeCommandDescriptor* commands = nullptr;
        std::uint32_t commandCount = 0;
        const RuntimeOptionDescriptor* options = nullptr;
        std::uint32_t optionCount = 0;
        const RuntimeOperandDescriptor* operands = nullptr;
        std::uint32_t operandCount = 0;
        const void* state = nullptr;
        RuntimeValidateValueFn validateValue = nullptr;
        RuntimeInvokeFn invoke = nullptr;
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
        RuntimeAbiErrorKind kind = RuntimeAbiErrorKind::MissingDescriptor;
        std::string detail;

        /** @brief Return a process exit code suitable for module import failures. */
        [[nodiscard]] constexpr int ExitCode() const noexcept { return 70; }

        /** @brief Format this import failure for a human operator. */
        [[nodiscard]] std::string Message() const { return "runtime CLI module ABI error: " + detail; }
    };

    namespace Detail {

        [[nodiscard]] inline bool ValidRuntimeString(RuntimeStringView text) noexcept {
            return text.size <= std::numeric_limits<std::size_t>::max() && (text.size == 0 || text.data != nullptr);
        }

        [[nodiscard]] inline std::string CopyRuntimeString(RuntimeStringView text) {
            return text.size == 0 ? std::string{} : std::string{text.data, static_cast<std::size_t>(text.size)};
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
                std::uint32_t commandId = kInvalidCommandId;
                std::uint32_t parentCommandId = kInvalidCommandId;
                std::string name;
                std::string about;
                std::uint32_t optionBegin = 0;
                std::uint32_t optionCount = 0;
                std::uint32_t operandBegin = 0;
                std::uint32_t operandCount = 0;
            };

            struct OptionStorage {
                std::string longName;
                std::string valueName;
                std::string about;
                char shortName = '\0';
                OptionKind kind = OptionKind::Switch;
                ValueCardinality cardinality = ValueCardinality::None;
                bool required = false;
                bool global = false;
                bool overridesGlobal = false;
            };

            struct OperandStorage {
                std::string name;
                std::string about;
                ValueCardinality cardinality = ValueCardinality::One;
            };

            std::string provider;
            std::vector<CommandStorage> commands;
            std::vector<OptionStorage> options;
            std::vector<OperandStorage> operands;
            const void* moduleState = nullptr;
            RuntimeInvokeFn invoke = nullptr;
            RuntimeValidateValueFn validateValue = nullptr;
            std::shared_ptr<const void> moduleOwner;

            [[nodiscard]] static bool Describe(const void* state, FragmentDescription& output) noexcept {
                try {
                    const auto& self = *static_cast<const ImportedRuntimeState*>(state);
                    output.commands.clear();
                    output.options.clear();
                    output.operands.clear();
                    output.commands.reserve(self.commands.size());
                    output.options.reserve(self.options.size());
                    output.operands.reserve(self.operands.size());
                    for (const CommandStorage& command : self.commands) {
                        output.commands.push_back({.commandId = command.commandId,
                                                   .parentCommandId = command.parentCommandId,
                                                   .name = command.name,
                                                   .about = command.about,
                                                   .optionBegin = command.optionBegin,
                                                   .optionCount = command.optionCount,
                                                   .operandBegin = command.operandBegin,
                                                   .operandCount = command.operandCount});
                    }
                    for (std::size_t index = 0; index < self.options.size(); ++index) {
                        const OptionStorage& option = self.options[index];
                        output.options.push_back({.descriptorId = static_cast<std::uint32_t>(index),
                                                  .longName = option.longName,
                                                  .valueName = option.valueName,
                                                  .about = option.about,
                                                  .shortName = option.shortName,
                                                  .kind = option.kind,
                                                  .cardinality = option.cardinality,
                                                  .required = option.required,
                                                  .global = option.global,
                                                  .overridesGlobal = option.overridesGlobal});
                    }
                    for (const OperandStorage& operand : self.operands) {
                        output.operands.push_back(
                            {.name = operand.name, .about = operand.about, .cardinality = operand.cardinality});
                    }
                    return true;
                } catch (...) {
                    output = {};
                    return false;
                }
            }

            [[nodiscard]] static bool ValidateValue(const void* state, std::uint32_t descriptorId,
                                                    std::string_view value) noexcept {
                const auto& self = *static_cast<const ImportedRuntimeState*>(state);
                return descriptorId < self.options.size() &&
                       self.validateValue(self.moduleState, descriptorId, RuntimeString(value));
            }

            [[nodiscard]] static InvocationResult Invoke(const void* state,
                                                         const FragmentInvocation& invocation) noexcept {
                const auto& self = *static_cast<const ImportedRuntimeState*>(state);
                std::vector<RuntimeStringView> tokens;
                try {
                    tokens.reserve(invocation.argv.Size());
                    for (std::size_t index = 0; index < invocation.argv.Size(); ++index) {
                        tokens.push_back(RuntimeString(invocation.argv[index]));
                    }
                } catch (...) {
                    return InvocationResult::Failed(1, "failed to prepare runtime CLI invocation tokens");
                }

                const RuntimeArgvView runtimeArgv{.tokens = tokens.data(),
                                                  .tokenCount = tokens.size(),
                                                  .commandPrefix = RuntimeString(invocation.commandPrefix)};
                const RuntimeInvocationResult result = self.invoke(self.moduleState, &runtimeArgv);
                if (!ValidRuntimeString(result.diagnostic) || result.diagnostic.size > kMaxRuntimeDiagnosticBytes) {
                    return InvocationResult::Failed(1, "runtime CLI module returned an invalid diagnostic view");
                }

                std::string diagnostic;
                try {
                    diagnostic = CopyRuntimeString(result.diagnostic);
                } catch (...) {
                    return InvocationResult::Failed(1, "failed to copy runtime CLI module diagnostics");
                }
                switch (result.status) {
                    case RuntimeInvocationStatus::Completed:
                        return InvocationResult::Completed(result.exitCode);
                    case RuntimeInvocationStatus::InvalidArguments:
                        return InvocationResult::InvalidArguments(result.exitCode, std::move(diagnostic));
                    case RuntimeInvocationStatus::Failed:
                        return InvocationResult::Failed(result.exitCode, std::move(diagnostic));
                }
                return InvocationResult::Failed(1, "runtime CLI module returned an invalid invocation status");
            }
        };

    } // namespace Detail

    /** @brief Producer-side static runtime module image with complete command-tree metadata. */
    template<typename Declaration, typename CommandTree, std::size_t CommandCount, std::size_t OptionCount,
             std::size_t OperandCount>
    struct StaticRuntimeModule {
        using SubprogramType = StaticSubprogram<Declaration, CommandTree>;

        FixedString<64> provider{};
        SubprogramType subprogram{};
        std::array<RuntimeCommandDescriptor, CommandCount> commands{};
        std::array<RuntimeOptionDescriptor, OptionCount> options{};
        std::array<RuntimeOperandDescriptor, OperandCount> operands{};

        /** @brief Return the process-lifetime ABI descriptor exported by the producer DLL. */
        [[nodiscard]] RuntimeModuleDescriptor Descriptor() const& noexcept {
            return {.header = {.descriptorSize = sizeof(RuntimeModuleDescriptor)},
                    .provider = RuntimeString(provider.view()),
                    .commands = commands.data(),
                    .commandCount = static_cast<std::uint32_t>(commands.size()),
                    .options = options.data(),
                    .optionCount = static_cast<std::uint32_t>(options.size()),
                    .operands = operands.data(),
                    .operandCount = static_cast<std::uint32_t>(operands.size()),
                    .state = std::addressof(subprogram),
                    .validateValue = &ValidateValue,
                    .invoke = &Invoke};
        }

        RuntimeModuleDescriptor Descriptor() const&& = delete;

    private:
        [[nodiscard]] static bool ValidateValue(const void* state, std::uint32_t descriptorId,
                                                RuntimeStringView value) noexcept {
            if (!Detail::ValidRuntimeString(value)) {
                return false;
            }
            const auto& schema = static_cast<const SubprogramType*>(state)->program.schema;
            if (descriptorId >= schema.options.size()) {
                return false;
            }
            const OptionDesc& option = schema.options[descriptorId];
            const std::string_view text = value.size == 0
                                              ? std::string_view{}
                                              : std::string_view{value.data, static_cast<std::size_t>(value.size)};
            return option.validateValue != nullptr && option.validateValue(text);
        }

        [[nodiscard]] static RuntimeInvocationResult Invoke(const void* state,
                                                            const RuntimeArgvView* runtimeArgv) noexcept {
            static thread_local std::string diagnostic;
            diagnostic.clear();
            try {
                if (runtimeArgv == nullptr || runtimeArgv->tokenCount > kMaxRuntimeInvocationTokens ||
                    (runtimeArgv->tokenCount != 0 && runtimeArgv->tokens == nullptr) ||
                    !Detail::ValidRuntimeString(runtimeArgv->commandPrefix)) {
                    diagnostic = "runtime CLI module received an invalid argument view";
                    return {.status = RuntimeInvocationStatus::Failed,
                            .exitCode = 1,
                            .diagnostic = RuntimeString(diagnostic)};
                }

                std::vector<std::string_view> tokenStorage;
                tokenStorage.reserve(runtimeArgv->tokenCount);
                for (std::uint64_t index = 0; index < runtimeArgv->tokenCount; ++index) {
                    const RuntimeStringView token = runtimeArgv->tokens[index];
                    if (!Detail::ValidRuntimeString(token)) {
                        diagnostic = "runtime CLI module received an invalid token view";
                        return {.status = RuntimeInvocationStatus::Failed,
                                .exitCode = 1,
                                .diagnostic = RuntimeString(diagnostic)};
                    }
                    tokenStorage.push_back(token.size == 0 ? std::string_view{}
                                                           : std::string_view{token.data, token.size});
                }

                const std::string_view prefix =
                    runtimeArgv->commandPrefix.size == 0
                        ? std::string_view{}
                        : std::string_view{runtimeArgv->commandPrefix.data, runtimeArgv->commandPrefix.size};
                InvocationResult result = static_cast<const SubprogramType*>(state)->Execute(
                    FragmentInvocation{.argv = ArgvView{.tokens = tokenStorage}, .commandPrefix = prefix});
                diagnostic = std::move(result.diagnostic);
                return {.status = Detail::ToRuntimeStatus(result.status),
                        .exitCode = result.exitCode,
                        .diagnostic = RuntimeString(diagnostic)};
            } catch (const std::exception& error) {
                diagnostic = error.what();
                return {
                    .status = RuntimeInvocationStatus::Failed, .exitCode = 1, .diagnostic = RuntimeString(diagnostic)};
            } catch (...) {
                diagnostic = "runtime CLI module raised an unknown exception";
                return {
                    .status = RuntimeInvocationStatus::Failed, .exitCode = 1, .diagnostic = RuntimeString(diagnostic)};
            }
        }
    };

    /** @brief Compile a reflected subprogram declaration into a producer-side runtime module image. */
    template<Concept::ProgramRoot Declaration>
    consteval auto CompileRuntimeModule(std::string_view provider) {
        using CommandTree = CommandTreeOf<Declaration>;
        if (!Detail::IsCliName(provider) || provider.size() > 64) {
            throw "Sora CLI runtime module provider names must contain between 1 and 64 bytes.";
        }

        constexpr auto subprogram = CompileSubprogram<Declaration>();
        constexpr std::size_t commandCount = subprogram.program.schema.commands.size();
        constexpr std::size_t optionCount = subprogram.program.schema.options.size();
        constexpr std::size_t operandCount = subprogram.program.schema.operands.size();
        StaticRuntimeModule<Declaration, CommandTree, commandCount, optionCount, operandCount> result{
            .provider = FixedString<64>{provider}, .subprogram = subprogram};

        for (std::size_t index = 0; index < commandCount; ++index) {
            const CommandDesc& command = subprogram.program.schema.commands[index];
            result.commands[index] = {.commandId = command.commandId,
                                      .parentCommandId = command.parentCommandId,
                                      .name = RuntimeString(subprogram.program.schema.NameText(command.name)),
                                      .about = RuntimeString(subprogram.program.schema.NameText(command.about)),
                                      .optionBegin = command.optionBegin,
                                      .optionCount = command.optionCount,
                                      .operandBegin = command.operandBegin,
                                      .operandCount = command.operandCount};
        }
        for (std::size_t index = 0; index < optionCount; ++index) {
            const OptionDesc& option = subprogram.program.schema.options[index];
            result.options[index] = {.longName = RuntimeString(subprogram.program.schema.NameText(option.longName)),
                                     .valueName = RuntimeString(subprogram.program.schema.NameText(option.valueName)),
                                     .about = RuntimeString(subprogram.program.schema.NameText(option.about)),
                                     .shortName = option.shortName,
                                     .kind = static_cast<std::uint8_t>(option.kind),
                                     .cardinality = static_cast<std::uint8_t>(option.cardinality),
                                     .required = option.required,
                                     .global = option.global,
                                     .overridesGlobal = option.overridesGlobal};
        }
        for (std::size_t index = 0; index < operandCount; ++index) {
            const OperandDesc& operand = subprogram.program.schema.operands[index];
            result.operands[index] = {.name = RuntimeString(subprogram.program.schema.NameText(operand.name)),
                                      .about = RuntimeString(subprogram.program.schema.NameText(operand.about)),
                                      .cardinality = static_cast<std::uint8_t>(operand.cardinality)};
        }
        return result;
    }

    /** @brief Validate and import an external runtime module as an owner-protected command fragment. */
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
            descriptor->provider.size > 64 ||
            !Detail::IsCliName(std::string_view{descriptor->provider.data, descriptor->provider.size})) {
            return fail(RuntimeAbiErrorKind::InvalidProvider, "provider identity is empty, invalid, or too long");
        }
        if (descriptor->commandCount == 0 || descriptor->commandCount > kMaxRuntimeModuleCommands ||
            descriptor->commands == nullptr || descriptor->optionCount > kMaxRuntimeModuleOptions ||
            (descriptor->optionCount != 0 && descriptor->options == nullptr) ||
            descriptor->operandCount > kMaxRuntimeModuleOperands ||
            (descriptor->operandCount != 0 && descriptor->operands == nullptr)) {
            return fail(RuntimeAbiErrorKind::InvalidCommands, "descriptor tables are missing or exceed ABI limits");
        }
        if (descriptor->state == nullptr || descriptor->validateValue == nullptr || descriptor->invoke == nullptr) {
            return fail(RuntimeAbiErrorKind::InvalidInvocation, "module state or invocation vtable is missing");
        }

        auto state = std::make_shared<Detail::ImportedRuntimeState>();
        state->provider = Detail::CopyRuntimeString(descriptor->provider);
        state->moduleState = descriptor->state;
        state->invoke = descriptor->invoke;
        state->validateValue = descriptor->validateValue;
        state->moduleOwner = std::move(owner);
        state->commands.reserve(descriptor->commandCount);
        state->options.reserve(descriptor->optionCount);
        state->operands.reserve(descriptor->operandCount);

        for (std::uint32_t index = 0; index < descriptor->commandCount; ++index) {
            const RuntimeCommandDescriptor& command = descriptor->commands[index];
            if (!Detail::ValidRuntimeString(command.name) || !Detail::ValidRuntimeString(command.about) ||
                command.about.size > 256 ||
                (index != 0 && (command.name.size == 0 || command.name.size > 64 ||
                                !Detail::IsCliName(std::string_view{command.name.data, command.name.size}))) ||
                command.optionBegin > descriptor->optionCount ||
                command.optionCount > descriptor->optionCount - command.optionBegin ||
                command.operandBegin > descriptor->operandCount ||
                command.operandCount > descriptor->operandCount - command.operandBegin) {
                return fail(RuntimeAbiErrorKind::InvalidCommands, "a command descriptor is invalid");
            }
            state->commands.push_back({.commandId = command.commandId,
                                       .parentCommandId = command.parentCommandId,
                                       .name = Detail::CopyRuntimeString(command.name),
                                       .about = Detail::CopyRuntimeString(command.about),
                                       .optionBegin = command.optionBegin,
                                       .optionCount = command.optionCount,
                                       .operandBegin = command.operandBegin,
                                       .operandCount = command.operandCount});
        }
        for (std::uint32_t index = 0; index < descriptor->optionCount; ++index) {
            const RuntimeOptionDescriptor& option = descriptor->options[index];
            if (!Detail::ValidRuntimeString(option.longName) || !Detail::ValidRuntimeString(option.valueName) ||
                !Detail::ValidRuntimeString(option.about)) {
                return fail(RuntimeAbiErrorKind::InvalidCommands, "an option string view is invalid");
            }
            const auto kind = static_cast<OptionKind>(option.kind);
            const auto cardinality = static_cast<ValueCardinality>(option.cardinality);
            const bool validKind =
                (kind == OptionKind::Switch && cardinality == ValueCardinality::None && option.valueName.size == 0) ||
                (kind == OptionKind::Parameter && cardinality == ValueCardinality::One) ||
                (kind == OptionKind::Help && cardinality == ValueCardinality::None &&
                 std::string_view{option.longName.data, static_cast<std::size_t>(option.longName.size)} ==
                     kHelpOptionName &&
                 option.shortName == kHelpOptionShortName && option.valueName.size == 0 && option.required == 0 &&
                 option.overridesGlobal == 0);
            if (option.longName.size == 0 || option.longName.size > 64 || option.valueName.size > 32 ||
                option.about.size > 256 ||
                !Detail::IsCliName(std::string_view{option.longName.data, option.longName.size}) ||
                !Detail::IsShortOptionName(option.shortName) ||
                option.kind > static_cast<std::uint8_t>(OptionKind::Help) ||
                option.cardinality > static_cast<std::uint8_t>(ValueCardinality::ZeroOrMore) || option.required > 1 ||
                option.global != 0 || option.overridesGlobal > 1 || !validKind) {
                return fail(RuntimeAbiErrorKind::InvalidCommands, "an option descriptor is invalid");
            }
            state->options.push_back({.longName = Detail::CopyRuntimeString(option.longName),
                                      .valueName = Detail::CopyRuntimeString(option.valueName),
                                      .about = Detail::CopyRuntimeString(option.about),
                                      .shortName = option.shortName,
                                      .kind = static_cast<OptionKind>(option.kind),
                                      .cardinality = static_cast<ValueCardinality>(option.cardinality),
                                      .required = option.required != 0,
                                      .global = option.global != 0,
                                      .overridesGlobal = option.overridesGlobal != 0});
        }
        for (std::uint32_t index = 0; index < descriptor->operandCount; ++index) {
            const RuntimeOperandDescriptor& operand = descriptor->operands[index];
            if (!Detail::ValidRuntimeString(operand.name) || !Detail::ValidRuntimeString(operand.about) ||
                operand.name.size == 0 || operand.name.size > 64 || operand.about.size > 256 ||
                !Detail::IsCliName(std::string_view{operand.name.data, operand.name.size}) ||
                operand.cardinality == static_cast<std::uint8_t>(ValueCardinality::None) ||
                operand.cardinality > static_cast<std::uint8_t>(ValueCardinality::ZeroOrMore)) {
                return fail(RuntimeAbiErrorKind::InvalidCommands, "an operand descriptor is invalid");
            }
            state->operands.push_back({.name = Detail::CopyRuntimeString(operand.name),
                                       .about = Detail::CopyRuntimeString(operand.about),
                                       .cardinality = static_cast<ValueCardinality>(operand.cardinality)});
        }

        CommandFragment fragment{.provider = state->provider,
                                 .state = state.get(),
                                 .describe = &Detail::ImportedRuntimeState::Describe,
                                 .validateValue = &Detail::ImportedRuntimeState::ValidateValue,
                                 .invoke = &Detail::ImportedRuntimeState::Invoke,
                                 .source = CommandSource::RuntimeModule};
        return FragmentRegistration{.fragment = fragment, .owner = std::move(state)};
    }

    static_assert(std::is_standard_layout_v<RuntimeStringView> && std::is_trivially_copyable_v<RuntimeStringView>);
    static_assert(std::is_standard_layout_v<RuntimeArgvView> && std::is_trivially_copyable_v<RuntimeArgvView>);
    static_assert(std::is_standard_layout_v<RuntimeCommandDescriptor> &&
                  std::is_trivially_copyable_v<RuntimeCommandDescriptor>);
    static_assert(std::is_standard_layout_v<RuntimeOptionDescriptor> &&
                  std::is_trivially_copyable_v<RuntimeOptionDescriptor>);
    static_assert(std::is_standard_layout_v<RuntimeOperandDescriptor> &&
                  std::is_trivially_copyable_v<RuntimeOperandDescriptor>);
    static_assert(std::is_standard_layout_v<RuntimeAbiHeader> && std::is_trivially_copyable_v<RuntimeAbiHeader>);
    static_assert(std::is_same_v<std::underlying_type_t<RuntimeInvocationStatus>, std::uint32_t>);
    static_assert(std::is_standard_layout_v<RuntimeInvocationResult> &&
                  std::is_trivially_copyable_v<RuntimeInvocationResult>);
    static_assert(std::is_standard_layout_v<RuntimeModuleDescriptor> &&
                  std::is_trivially_copyable_v<RuntimeModuleDescriptor>);

} // namespace Sora::CLI

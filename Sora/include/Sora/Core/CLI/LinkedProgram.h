/**
 * @file LinkedProgram.h
 * @brief Immutable startup-linked command graphs with arbitrary-scope fragment mounting.
 * @ingroup Core
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Sora/Core/CLI/Fragment.h>

namespace Sora::CLI {

    /** @brief Failure category returned when an immutable linked program cannot execute an argument stream. */
    enum class LinkedProgramErrorKind : std::uint8_t {
        MissingCommand,
        UnknownCommand,
        UnknownOption,
        MissingValue,
        UnexpectedValue,
        TooManyOperands,
        MissingRequiredOption,
        MissingRequiredOperand,
        InvalidArguments,
        InvocationFailed,
    };

    /** @brief Structured execution failure for an already linked program. */
    struct LinkedProgramError {
        LinkedProgramErrorKind kind = LinkedProgramErrorKind::UnknownCommand;
        int exitCode = 2;
        std::string command;
        std::string diagnostic;

        /** @brief Return the suggested process result code. */
        [[nodiscard]] constexpr int ExitCode() const noexcept { return exitCode; }

        /** @brief Format this execution failure for a human user. */
        [[nodiscard]] std::string Message() const {
            if (!diagnostic.empty()) {
                return diagnostic;
            }
            switch (kind) {
                case LinkedProgramErrorKind::MissingCommand:
                    return "missing command";
                case LinkedProgramErrorKind::UnknownCommand:
                    return std::format("unknown command '{}'", command);
                case LinkedProgramErrorKind::UnknownOption:
                    return std::format("unknown option '{}'", command);
                case LinkedProgramErrorKind::MissingValue:
                    return std::format("missing value for '{}'", command);
                case LinkedProgramErrorKind::UnexpectedValue:
                    return std::format("unexpected value for '{}'", command);
                case LinkedProgramErrorKind::TooManyOperands:
                    return std::format("too many operands near '{}'", command);
                case LinkedProgramErrorKind::MissingRequiredOption:
                    return std::format("missing required option '{}'", command);
                case LinkedProgramErrorKind::MissingRequiredOperand:
                    return std::format("missing required operand '{}'", command);
                case LinkedProgramErrorKind::InvalidArguments:
                    return std::format("invalid arguments for '{}'", command);
                case LinkedProgramErrorKind::InvocationFailed:
                    return std::format("command '{}' failed before completion", command);
            }
            return "CLI execution failed";
        }
    };

    namespace Detail {

        [[nodiscard]] inline std::string_view CommandSourceName(CommandSource source) noexcept {
            switch (source) {
                case CommandSource::Program:
                    return "program";
                case CommandSource::StartupFragment:
                    return "startup";
                case CommandSource::RuntimeModule:
                    return "runtime";
            }
            return "unknown";
        }

        struct LinkedOption {
            std::string longName;
            std::string valueName;
            std::string about;
            char shortName = '\0';
            OptionKind kind = OptionKind::Switch;
            ValueCardinality cardinality = ValueCardinality::None;
            size_t presenceId = 0;
            std::uint32_t descriptorId = 0;
            bool required = false;
            bool global = false;
            bool overridesGlobal = false;
            ValidateValueFn directValidator = nullptr;
            FragmentValidateValueFn fragmentValidator = nullptr;
            const void* validationState = nullptr;
        };

        struct LinkedOperand {
            std::string name;
            std::string about;
            ValueCardinality cardinality = ValueCardinality::One;
            size_t presenceId = 0;
        };

        struct LinkedCommandNode {
            std::string name;
            std::string about;
            std::string provider;
            CommandSource source = CommandSource::Program;
            std::uint32_t parent = 0;
            std::uint32_t routeId = 0;
            std::vector<std::uint32_t> children;
            std::vector<LinkedOption> options;
            std::vector<LinkedOperand> operands;
            const void* state = nullptr;
            FragmentInvokeFn invoke = nullptr;
            std::shared_ptr<const void> owner;
        };

        struct LinkedTokenEvent {
            std::uint32_t routeId = 0;
            std::string token;
        };

        struct LinkedParseResult {
            std::uint32_t command = 0;
            std::uint32_t helpCommand = kInvalidCommandId;
            std::vector<LinkedTokenEvent> events;
        };

        struct PendingFragment {
            const FragmentRegistration* registration = nullptr;
            FragmentDescription description;
            std::uint32_t routeId = 0;
            bool linked = false;
        };

        [[nodiscard]] inline std::string JoinPath(std::span<const std::string> path) {
            std::string result;
            for (const std::string& segment : path) {
                if (!result.empty()) {
                    result += ' ';
                }
                result += segment;
            }
            return result;
        }

        [[nodiscard]] inline const LinkedOption* FindLongOption(const std::vector<LinkedCommandNode>& nodes,
                                                                std::uint32_t command, Policy policy,
                                                                std::string_view name) noexcept {
            for (const LinkedOption& option : nodes[command].options) {
                if (option.longName == name) {
                    return std::addressof(option);
                }
            }
            if (command != 0 && Sora::HasFlag(policy, Policy::GlobalOptionsAnywhere)) {
                for (const LinkedOption& option : nodes[0].options) {
                    if (option.global && option.longName == name) {
                        return std::addressof(option);
                    }
                }
            }
            return nullptr;
        }

        [[nodiscard]] inline const LinkedOption* FindShortOption(const std::vector<LinkedCommandNode>& nodes,
                                                                 std::uint32_t command, Policy policy,
                                                                 char name) noexcept {
            for (const LinkedOption& option : nodes[command].options) {
                if (option.shortName == name) {
                    return std::addressof(option);
                }
            }
            if (command != 0 && Sora::HasFlag(policy, Policy::GlobalOptionsAnywhere)) {
                for (const LinkedOption& option : nodes[0].options) {
                    if (option.global && option.shortName == name) {
                        return std::addressof(option);
                    }
                }
            }
            return nullptr;
        }

        [[nodiscard]] inline std::uint32_t FindChild(const std::vector<LinkedCommandNode>& nodes, std::uint32_t parent,
                                                     std::string_view name) noexcept {
            for (std::uint32_t child : nodes[parent].children) {
                if (nodes[child].name == name) {
                    return child;
                }
            }
            return kInvalidCommandId;
        }

        [[nodiscard]] inline std::string OptionLabel(const LinkedOption& option) {
            std::string result;
            if (option.shortName != '\0') {
                result = std::format("-{}, --{}", option.shortName, option.longName);
            } else {
                result = "--" + option.longName;
            }
            if (option.kind == OptionKind::Parameter) {
                result += ' ';
                result += option.valueName.empty() ? "value" : option.valueName;
            }
            return result;
        }

        [[nodiscard]] inline std::string OperandLabel(const LinkedOperand& operand) {
            switch (operand.cardinality) {
                case ValueCardinality::One:
                    return '<' + operand.name + '>';
                case ValueCardinality::OptionalOne:
                    return "[<" + operand.name + ">]";
                case ValueCardinality::OneOrMore:
                    return '<' + operand.name + ">...";
                case ValueCardinality::ZeroOrMore:
                    return "[<" + operand.name + ">...]";
                case ValueCardinality::None:
                    return {};
            }
            return {};
        }

    } // namespace Detail

    /** @brief Immutable command graph linked from a static root and mounted provider fragments. */
    template<typename Root, typename CommandTree = CommandTreeOf<Root>>
    class LinkedProgram {
    public:
        using ProgramType = Program<Root, CommandTree>;
        using RunResult = std::expected<int, LinkedProgramError>;

        LinkedProgram(const ProgramType& root, std::string programName, Policy policy,
                      std::vector<Detail::LinkedCommandNode> nodes, size_t presenceCount)
            : root_(std::addressof(root)),
              programName_(std::move(programName)),
              policy_(policy),
              nodes_(std::move(nodes)),
              presenceCount_(presenceCount) {}

        /** @brief Return the root program display name. */
        [[nodiscard]] std::string_view ProgramName() const noexcept { return programName_; }

        /** @brief Return the number of user-visible commands in the linked graph. */
        [[nodiscard]] size_t CommandCount() const noexcept { return nodes_.empty() ? 0 : nodes_.size() - 1; }

        /** @brief Return true when @p path identifies one linked command. */
        [[nodiscard]] bool ContainsCommand(std::string_view path) const noexcept {
            std::uint32_t current = 0;
            while (!path.empty()) {
                const size_t separator = path.find(' ');
                const std::string_view segment = path.substr(0, separator);
                current = Detail::FindChild(nodes_, current, segment);
                if (current == kInvalidCommandId) {
                    return false;
                }
                path = separator == std::string_view::npos ? std::string_view{} : path.substr(separator + 1);
            }
            return current != 0;
        }

        /** @brief Parse, route, and dispatch @p argv through the immutable linked graph. */
        [[nodiscard]] RunResult Run(ArgvView argv) const {
            auto parsed = Parse(argv);
            if (!parsed) {
                return std::unexpected(std::move(parsed.error()));
            }
            if (parsed->helpCommand != kInvalidCommandId) {
                PrintHelp(parsed->helpCommand);
                return 0;
            }

            const Detail::LinkedCommandNode& selected = nodes_[parsed->command];
            if (selected.routeId == 0) {
                try {
                    auto rootResult = root_->Parse(argv);
                    if (!rootResult) {
                        return std::unexpected(
                            LinkedProgramError{.kind = LinkedProgramErrorKind::InvalidArguments,
                                               .exitCode = rootResult.error().ExitCode(),
                                               .command = selected.name,
                                               .diagnostic = root_->FormatError(rootResult.error())});
                    }
                    return root_->Dispatch(*rootResult);
                } catch (const std::exception& error) {
                    return std::unexpected(LinkedProgramError{.kind = LinkedProgramErrorKind::InvocationFailed,
                                                              .exitCode = 1,
                                                              .command = selected.name,
                                                              .diagnostic = error.what()});
                } catch (...) {
                    return std::unexpected(LinkedProgramError{.kind = LinkedProgramErrorKind::InvocationFailed,
                                                              .exitCode = 1,
                                                              .command = selected.name,
                                                              .diagnostic = "CLI action raised an unknown exception"});
                }
            }

            std::vector<std::string_view> localTokens;
            localTokens.reserve(parsed->events.size());
            for (const Detail::LinkedTokenEvent& event : parsed->events) {
                if (event.routeId == selected.routeId) {
                    localTokens.push_back(event.token);
                }
            }

            const std::string prefix = FragmentPrefix(parsed->command, selected.routeId);
            InvocationResult invocation = selected.invoke(
                selected.state, FragmentInvocation{.argv = ArgvView{.tokens = localTokens}, .commandPrefix = prefix});
            if (invocation.status == InvocationStatus::Completed) {
                return invocation.exitCode;
            }
            return std::unexpected(LinkedProgramError{.kind = invocation.status == InvocationStatus::InvalidArguments
                                                                  ? LinkedProgramErrorKind::InvalidArguments
                                                                  : LinkedProgramErrorKind::InvocationFailed,
                                                      .exitCode = invocation.exitCode,
                                                      .command = selected.name,
                                                      .diagnostic = std::move(invocation.diagnostic)});
        }

        /** @brief Render help for @p command from the same graph used by parsing and dispatch. */
        [[nodiscard]] std::string FormatHelp(std::uint32_t command = 0, HelpRenderOptions options = {}) const {
            const Detail::LinkedCommandNode& node = nodes_[command];
            Detail::HelpDocument document{.usage = CommandPath(command)};
            if (!node.options.empty()) {
                document.usage += " [options]";
            }
            if (!node.children.empty()) {
                document.usage += " <command>";
            }
            for (const Detail::LinkedOperand& operand : node.operands) {
                document.usage += ' ';
                document.usage += Detail::OperandLabel(operand);
            }

            Detail::HelpSection localOptions{.title = command == 0 ? "Options" : "Local options"};
            for (const Detail::LinkedOption& option : node.options) {
                localOptions.entries.push_back({.label = Detail::OptionLabel(option), .description = option.about});
            }
            if (!localOptions.entries.empty()) {
                document.sections.push_back(std::move(localOptions));
            }

            if (command != 0) {
                Detail::HelpSection globals{.title = "Global options"};
                for (const Detail::LinkedOption& option : nodes_[0].options) {
                    const bool overridden = std::ranges::any_of(node.options, [&](const Detail::LinkedOption& local) {
                        return local.overridesGlobal && local.longName == option.longName;
                    });
                    if (option.global && option.kind != OptionKind::Help && !overridden) {
                        globals.entries.push_back({.label = Detail::OptionLabel(option), .description = option.about});
                    }
                }
                if (!globals.entries.empty()) {
                    document.sections.push_back(std::move(globals));
                }
            }

            if (!node.operands.empty()) {
                Detail::HelpSection operands{.title = "Operands"};
                for (const Detail::LinkedOperand& operand : node.operands) {
                    operands.entries.push_back({.label = Detail::OperandLabel(operand), .description = operand.about});
                }
                document.sections.push_back(std::move(operands));
            }

            if (!node.children.empty()) {
                Detail::HelpSection commands{.title = "Subcommands"};
                for (std::uint32_t child : node.children) {
                    const Detail::LinkedCommandNode& childNode = nodes_[child];
                    commands.entries.push_back(
                        {.label = childNode.name,
                         .description = childNode.about,
                         .annotation =
                             std::format("{}:{}", Detail::CommandSourceName(childNode.source), childNode.provider)});
                }
                document.sections.push_back(std::move(commands));
            }
            return Detail::RenderHelpDocument(document, options);
        }

        /** @brief Write help for @p command using terminal-aware tapioca styling. */
        void PrintHelp(std::uint32_t command = 0, HelpRenderOptions options = {}) const {
            std::FILE* output = options.output == nullptr ? stdout : options.output;
            tapioca::pal::write_file(output, FormatHelp(command, options));
            tapioca::pal::flush_file(output);
        }

    private:
        [[nodiscard]] std::expected<Detail::LinkedParseResult, LinkedProgramError> Parse(ArgvView argv) const {
            Detail::LinkedParseResult result;
            result.events.reserve(argv.Size());
            std::vector<bool> presence(presenceCount_);
            std::vector<std::uint32_t> operandCounts(presenceCount_);
            std::vector<std::uint32_t> path{0};

            std::uint32_t current = 0;
            std::uint32_t operandIndex = 0;
            bool afterDelimiter = false;

            auto addOption = [&](const Detail::LinkedOption& option, std::optional<std::string_view> value) {
                const std::uint32_t route = option.global ? 0 : nodes_[current].routeId;
                result.events.push_back({.routeId = route, .token = "--" + option.longName});
                if (value.has_value()) {
                    result.events.push_back({.routeId = route, .token = std::string{*value}});
                }
                presence[option.presenceId] = true;
            };

            for (size_t index = 0; index < argv.Size(); ++index) {
                const std::string_view token = argv[index];
                if (!afterDelimiter && token == "--") {
                    afterDelimiter = true;
                    result.events.push_back({.routeId = nodes_[current].routeId, .token = "--"});
                    continue;
                }

                if (!afterDelimiter && token.starts_with("--") && token.size() > 2) {
                    const std::string_view body = token.substr(2);
                    const size_t equals = body.find('=');
                    const std::string_view name = equals == std::string_view::npos ? body : body.substr(0, equals);
                    const std::optional<std::string_view> attached =
                        equals == std::string_view::npos ? std::nullopt
                                                         : std::optional<std::string_view>{body.substr(equals + 1)};
                    const Detail::LinkedOption* option = Detail::FindLongOption(nodes_, current, policy_, name);
                    if (option == nullptr) {
                        return std::unexpected(LinkedProgramError{.kind = LinkedProgramErrorKind::UnknownOption,
                                                                  .command = std::string{token}});
                    }
                    if (option->kind == OptionKind::Help) {
                        if (attached.has_value()) {
                            return std::unexpected(LinkedProgramError{.kind = LinkedProgramErrorKind::UnexpectedValue,
                                                                      .command = option->longName});
                        }
                        result.command = current;
                        result.helpCommand = current;
                        return result;
                    }
                    if (option->kind == OptionKind::Switch) {
                        if (attached.has_value()) {
                            return std::unexpected(LinkedProgramError{.kind = LinkedProgramErrorKind::UnexpectedValue,
                                                                      .command = option->longName});
                        }
                        addOption(*option, std::nullopt);
                    } else {
                        std::string_view value;
                        if (attached.has_value()) {
                            value = *attached;
                        } else if (index + 1 < argv.Size()) {
                            value = argv[++index];
                        } else {
                            return std::unexpected(LinkedProgramError{.kind = LinkedProgramErrorKind::MissingValue,
                                                                      .command = option->longName});
                        }
                        if (!ValidateOptionValue(*option, value)) {
                            return std::unexpected(LinkedProgramError{
                                .kind = LinkedProgramErrorKind::InvalidArguments,
                                .command = option->longName,
                                .diagnostic = std::format("invalid value '{}' for '{}'", value, option->longName)});
                        }
                        addOption(*option, value);
                    }
                    continue;
                }

                if (!afterDelimiter && token.starts_with('-') && token.size() > 1) {
                    for (size_t shortIndex = 1; shortIndex < token.size(); ++shortIndex) {
                        const Detail::LinkedOption* option =
                            Detail::FindShortOption(nodes_, current, policy_, token[shortIndex]);
                        if (option == nullptr) {
                            return std::unexpected(
                                LinkedProgramError{.kind = LinkedProgramErrorKind::UnknownOption,
                                                   .command = std::format("-{}", token[shortIndex])});
                        }
                        if (option->kind == OptionKind::Help) {
                            result.command = current;
                            result.helpCommand = current;
                            return result;
                        }
                        if (option->kind == OptionKind::Switch) {
                            addOption(*option, std::nullopt);
                            continue;
                        }

                        std::string_view value;
                        if (shortIndex + 1 < token.size()) {
                            value = token.substr(shortIndex + 1);
                            shortIndex = token.size();
                        } else if (index + 1 < argv.Size()) {
                            value = argv[++index];
                        } else {
                            return std::unexpected(LinkedProgramError{.kind = LinkedProgramErrorKind::MissingValue,
                                                                      .command = option->longName});
                        }
                        if (!ValidateOptionValue(*option, value)) {
                            return std::unexpected(LinkedProgramError{
                                .kind = LinkedProgramErrorKind::InvalidArguments,
                                .command = option->longName,
                                .diagnostic = std::format("invalid value '{}' for '{}'", value, option->longName)});
                        }
                        addOption(*option, value);
                        break;
                    }
                    continue;
                }

                if (!afterDelimiter) {
                    const std::uint32_t child = Detail::FindChild(nodes_, current, token);
                    if (child != kInvalidCommandId) {
                        current = child;
                        result.command = current;
                        operandIndex = 0;
                        path.push_back(current);
                        result.events.push_back({.routeId = nodes_[current].routeId, .token = nodes_[current].name});
                        continue;
                    }
                }

                if (operandIndex >= nodes_[current].operands.size()) {
                    const auto kind = current == 0 && !nodes_[0].children.empty()
                                          ? LinkedProgramErrorKind::UnknownCommand
                                          : LinkedProgramErrorKind::TooManyOperands;
                    return std::unexpected(LinkedProgramError{.kind = kind, .command = std::string{token}});
                }
                const Detail::LinkedOperand& operand = nodes_[current].operands[operandIndex];
                result.events.push_back({.routeId = nodes_[current].routeId, .token = std::string{token}});
                presence[operand.presenceId] = true;
                ++operandCounts[operand.presenceId];
                if (operand.cardinality == ValueCardinality::One ||
                    operand.cardinality == ValueCardinality::OptionalOne) {
                    ++operandIndex;
                }
            }

            if (!nodes_[current].children.empty()) {
                return std::unexpected(LinkedProgramError{.kind = LinkedProgramErrorKind::MissingCommand,
                                                          .command = nodes_[current].name});
            }
            for (std::uint32_t command : path) {
                for (const Detail::LinkedOption& option : nodes_[command].options) {
                    if (option.required && !presence[option.presenceId]) {
                        return std::unexpected(LinkedProgramError{.kind = LinkedProgramErrorKind::MissingRequiredOption,
                                                                  .command = option.longName});
                    }
                }
                for (const Detail::LinkedOperand& operand : nodes_[command].operands) {
                    const bool required = operand.cardinality == ValueCardinality::One ||
                                          operand.cardinality == ValueCardinality::OneOrMore;
                    if (required && operandCounts[operand.presenceId] == 0) {
                        return std::unexpected(LinkedProgramError{
                            .kind = LinkedProgramErrorKind::MissingRequiredOperand, .command = operand.name});
                    }
                }
            }
            if (current == 0) {
                return std::unexpected(LinkedProgramError{.kind = LinkedProgramErrorKind::MissingCommand});
            }
            return result;
        }

        [[nodiscard]] std::string CommandPath(std::uint32_t command) const {
            std::vector<std::string_view> reverse;
            for (std::uint32_t current = command; current != 0; current = nodes_[current].parent) {
                reverse.push_back(nodes_[current].name);
            }
            std::string path = programName_;
            for (auto iterator = reverse.rbegin(); iterator != reverse.rend(); ++iterator) {
                path += ' ';
                path += *iterator;
            }
            return path;
        }

        [[nodiscard]] static bool ValidateOptionValue(const Detail::LinkedOption& option,
                                                      std::string_view value) noexcept {
            if (option.directValidator != nullptr) {
                return option.directValidator(value);
            }
            return option.fragmentValidator != nullptr &&
                   option.fragmentValidator(option.validationState, option.descriptorId, value);
        }

        [[nodiscard]] std::string FragmentPrefix(std::uint32_t command, std::uint32_t routeId) const {
            std::uint32_t first = command;
            while (nodes_[first].parent != 0 && nodes_[nodes_[first].parent].routeId == routeId) {
                first = nodes_[first].parent;
            }
            return CommandPath(nodes_[first].parent);
        }

        const ProgramType* root_ = nullptr;
        std::string programName_;
        Policy policy_ = Policy::None;
        std::vector<Detail::LinkedCommandNode> nodes_;
        size_t presenceCount_ = 0;
    };

    namespace Detail {

        [[nodiscard]] inline std::uint32_t ResolvePath(const std::vector<LinkedCommandNode>& nodes,
                                                       std::span<const std::string> path) noexcept {
            std::uint32_t current = 0;
            for (const std::string& segment : path) {
                current = FindChild(nodes, current, segment);
                if (current == kInvalidCommandId) {
                    return current;
                }
            }
            return current;
        }

        [[nodiscard]] inline std::optional<LinkError> ValidateDescription(const FragmentRegistration& registration,
                                                                          const FragmentDescription& description) {
            const CommandFragment& fragment = registration.fragment;
            if (description.commands.empty()) {
                return LinkError{.kind = LinkErrorKind::InvalidFragment,
                                 .provider = std::string{fragment.provider},
                                 .detail = "command tree is empty"};
            }
            if (description.commands.size() == 1 || description.commands[0].operandCount != 0) {
                return LinkError{.kind = LinkErrorKind::InvalidFragment,
                                 .provider = std::string{fragment.provider},
                                 .detail = "fragment root must be hidden and export at least one command"};
            }
            for (std::uint32_t index = 0; index < description.commands[0].optionCount; ++index) {
                if (description.options[description.commands[0].optionBegin + index].kind != OptionKind::Help) {
                    return LinkError{.kind = LinkErrorKind::InvalidFragment,
                                     .provider = std::string{fragment.provider},
                                     .detail = "fragment root cannot own user-visible options"};
                }
            }
            for (size_t index = 0; index < description.commands.size(); ++index) {
                const FragmentCommand& command = description.commands[index];
                if (command.commandId != index ||
                    (index == 0 ? command.parentCommandId != kInvalidCommandId
                                : command.parentCommandId >= command.commandId) ||
                    command.optionBegin > description.options.size() ||
                    command.optionCount > description.options.size() - command.optionBegin ||
                    command.operandBegin > description.operands.size() ||
                    command.operandCount > description.operands.size() - command.operandBegin) {
                    return LinkError{.kind = LinkErrorKind::InvalidFragment,
                                     .provider = std::string{fragment.provider},
                                     .detail = "command ids, parent ids, or descriptor slices are invalid"};
                }
                if (index != 0 &&
                    (!IsCliName(command.name) || command.name.size() > 64 || command.about.size() > 256)) {
                    return LinkError{.kind = LinkErrorKind::InvalidFragment,
                                     .provider = std::string{fragment.provider},
                                     .command = std::string{command.name},
                                     .detail = "command name or description violates schema limits"};
                }
            }
            for (const FragmentOption& option : description.options) {
                const bool validKind =
                    (option.kind == OptionKind::Switch && option.cardinality == ValueCardinality::None &&
                     option.valueName.empty()) ||
                    (option.kind == OptionKind::Parameter && option.cardinality == ValueCardinality::One) ||
                    (option.kind == OptionKind::Help && option.cardinality == ValueCardinality::None &&
                     option.longName == kHelpOptionName && option.shortName == kHelpOptionShortName &&
                     option.valueName.empty() && !option.required && !option.overridesGlobal);
                if (!IsCliName(option.longName) || option.longName.size() > 64 || option.valueName.size() > 32 ||
                    option.about.size() > 256 || !IsShortOptionName(option.shortName) || option.global ||
                    (option.kind == OptionKind::Parameter && option.valueName.size() != 0 &&
                     !IsCliName(option.valueName)) ||
                    option.descriptorId >= description.options.size() || !validKind) {
                    return LinkError{.kind = LinkErrorKind::InvalidFragment,
                                     .provider = std::string{fragment.provider},
                                     .detail = "option metadata is invalid for a mounted fragment"};
                }
            }
            for (const FragmentOperand& operand : description.operands) {
                if (!IsCliName(operand.name) || operand.name.size() > 64 || operand.about.size() > 256 ||
                    operand.cardinality == ValueCardinality::None) {
                    return LinkError{.kind = LinkErrorKind::InvalidFragment,
                                     .provider = std::string{fragment.provider},
                                     .detail = "operand metadata is invalid for a mounted fragment"};
                }
            }
            for (std::string_view segment : registration.mountPath) {
                if (!IsCliName(segment) || segment.size() > 64) {
                    return LinkError{.kind = LinkErrorKind::InvalidMount,
                                     .provider = std::string{fragment.provider},
                                     .command = JoinPath(registration.mountPath),
                                     .detail = "mount path contains an invalid command segment"};
                }
            }
            return std::nullopt;
        }

    } // namespace Detail

    /** @brief Seal a static root and mounted fragment snapshot into one immutable linked command graph. */
    template<typename Root, typename CommandTree>
    [[nodiscard]] auto LinkAtStartup(const Program<Root, CommandTree>& root, const FragmentRegistrySnapshot& snapshot,
                                     std::string_view rootProvider = "program")
        -> std::expected<LinkedProgram<Root, CommandTree>, LinkError> {
        if (!Detail::IsCliName(rootProvider) || rootProvider.size() > 64 || root.schema.commands.empty()) {
            return std::unexpected(LinkError{.kind = LinkErrorKind::InvalidProgram,
                                             .detail = "provider identity or root schema is invalid"});
        }

        std::vector<Detail::LinkedCommandNode> nodes(root.schema.commands.size());
        size_t presenceCount = 0;
        for (const CommandDesc& command : root.schema.commands) {
            auto& node = nodes[command.commandId];
            node.name = std::string{root.schema.NameText(command.name)};
            node.about = std::string{root.schema.NameText(command.about)};
            node.provider = std::string{rootProvider};
            node.source = CommandSource::Program;
            node.parent = command.commandId == 0 ? 0 : command.parentCommandId;
            for (std::uint32_t index = 0; index < command.optionCount; ++index) {
                const OptionDesc& option = root.schema.options[command.optionBegin + index];
                node.options.push_back({.longName = std::string{root.schema.NameText(option.longName)},
                                        .valueName = std::string{root.schema.NameText(option.valueName)},
                                        .about = std::string{root.schema.NameText(option.about)},
                                        .shortName = option.shortName,
                                        .kind = option.kind,
                                        .cardinality = option.cardinality,
                                        .presenceId = presenceCount++,
                                        .descriptorId = command.optionBegin + index,
                                        .required = option.required,
                                        .global = option.global,
                                        .overridesGlobal = option.overridesGlobal,
                                        .directValidator = option.validateValue});
            }
            for (std::uint32_t index = 0; index < command.operandCount; ++index) {
                const OperandDesc& operand = root.schema.operands[command.operandBegin + index];
                node.operands.push_back({.name = std::string{root.schema.NameText(operand.name)},
                                         .about = std::string{root.schema.NameText(operand.about)},
                                         .cardinality = operand.cardinality,
                                         .presenceId = presenceCount++});
            }
        }
        for (std::uint32_t command = 1; command < nodes.size(); ++command) {
            nodes[nodes[command].parent].children.push_back(command);
        }

        std::vector<Detail::PendingFragment> pending;
        pending.reserve(snapshot.Fragments().size());
        std::uint32_t nextRoute = 1;
        for (const FragmentRegistration& registration : snapshot.Fragments()) {
            const CommandFragment& fragment = registration.fragment;
            const bool validSource =
                fragment.source == CommandSource::StartupFragment || fragment.source == CommandSource::RuntimeModule;
            if (fragment.format != kCommandFragmentFormat || !Detail::IsCliName(fragment.provider) ||
                fragment.provider.size() > 64 || fragment.state == nullptr || fragment.describe == nullptr ||
                fragment.validateValue == nullptr || fragment.invoke == nullptr || !validSource ||
                (fragment.source == CommandSource::RuntimeModule && !registration.owner)) {
                return std::unexpected(
                    LinkError{.kind = LinkErrorKind::InvalidFragment,
                              .provider = std::string{fragment.provider},
                              .detail = "metadata, owner lease, state, or provider thunks are invalid"});
            }
            Detail::PendingFragment item{.registration = std::addressof(registration), .routeId = nextRoute++};
            if (!fragment.describe(fragment.state, item.description)) {
                return std::unexpected(LinkError{.kind = LinkErrorKind::InvalidFragment,
                                                 .provider = std::string{fragment.provider},
                                                 .detail = "provider failed to describe its sealed command tree"});
            }
            if (auto error = Detail::ValidateDescription(registration, item.description); error.has_value()) {
                return std::unexpected(std::move(*error));
            }
            pending.push_back(std::move(item));
        }

        size_t remaining = pending.size();
        while (remaining != 0) {
            bool progress = false;
            for (Detail::PendingFragment& item : pending) {
                if (item.linked) {
                    continue;
                }
                const FragmentRegistration& registration = *item.registration;
                const std::uint32_t mount = Detail::ResolvePath(nodes, registration.mountPath);
                if (mount == kInvalidCommandId) {
                    continue;
                }
                if (item.description.commands.size() - 1 > kMaxLinkedCommands - (nodes.size() - 1)) {
                    return std::unexpected(LinkError{.kind = LinkErrorKind::ResourceLimit});
                }

                std::vector<std::uint32_t> rewrite(item.description.commands.size(), kInvalidCommandId);
                rewrite[0] = mount;
                for (size_t localId = 1; localId < item.description.commands.size(); ++localId) {
                    const FragmentCommand& command = item.description.commands[localId];
                    const std::uint32_t parent = rewrite[command.parentCommandId];
                    if (parent == kInvalidCommandId) {
                        return std::unexpected(LinkError{.kind = LinkErrorKind::InvalidFragment,
                                                         .provider = std::string{registration.fragment.provider},
                                                         .detail = "command tree is not parent-before-child ordered"});
                    }
                    if (const std::uint32_t conflict = Detail::FindChild(nodes, parent, command.name);
                        conflict != kInvalidCommandId) {
                        return std::unexpected(
                            LinkError{.kind = LinkErrorKind::DuplicateCommand,
                                      .provider = nodes[conflict].provider,
                                      .command = std::string{command.name},
                                      .conflictingProvider = std::string{registration.fragment.provider}});
                    }

                    const std::uint32_t linkedId = static_cast<std::uint32_t>(nodes.size());
                    rewrite[localId] = linkedId;
                    Detail::LinkedCommandNode node{.name = std::string{command.name},
                                                   .about = std::string{command.about},
                                                   .provider = std::string{registration.fragment.provider},
                                                   .source = registration.fragment.source,
                                                   .parent = parent,
                                                   .routeId = item.routeId,
                                                   .state = registration.fragment.state,
                                                   .invoke = registration.fragment.invoke,
                                                   .owner = registration.owner};
                    for (std::uint32_t index = 0; index < command.optionCount; ++index) {
                        const FragmentOption& option = item.description.options[command.optionBegin + index];
                        bool matchedGlobal = false;
                        for (const Detail::LinkedOption& global : nodes[0].options) {
                            if (!global.global ||
                                (option.longName != global.longName &&
                                 (option.shortName == '\0' || option.shortName != global.shortName))) {
                                continue;
                            }
                            if (!option.overridesGlobal || option.longName != global.longName ||
                                option.shortName != global.shortName || option.kind != global.kind ||
                                option.cardinality != global.cardinality) {
                                return std::unexpected(LinkError{
                                    .kind = LinkErrorKind::InvalidFragment,
                                    .provider = std::string{registration.fragment.provider},
                                    .command = std::string{command.name},
                                    .detail = "local/global option collision lacks an exact Override declaration"});
                            }
                            matchedGlobal = true;
                        }
                        if (option.overridesGlobal && !matchedGlobal) {
                            return std::unexpected(
                                LinkError{.kind = LinkErrorKind::InvalidFragment,
                                          .provider = std::string{registration.fragment.provider},
                                          .command = std::string{command.name},
                                          .detail = "Override option does not match a root-global option"});
                        }
                        node.options.push_back({.longName = std::string{option.longName},
                                                .valueName = std::string{option.valueName},
                                                .about = std::string{option.about},
                                                .shortName = option.shortName,
                                                .kind = option.kind,
                                                .cardinality = option.cardinality,
                                                .presenceId = presenceCount++,
                                                .descriptorId = option.descriptorId,
                                                .required = option.required,
                                                .global = false,
                                                .overridesGlobal = option.overridesGlobal,
                                                .fragmentValidator = registration.fragment.validateValue,
                                                .validationState = registration.fragment.state});
                    }
                    for (std::uint32_t index = 0; index < command.operandCount; ++index) {
                        const FragmentOperand& operand = item.description.operands[command.operandBegin + index];
                        node.operands.push_back({.name = std::string{operand.name},
                                                 .about = std::string{operand.about},
                                                 .cardinality = operand.cardinality,
                                                 .presenceId = presenceCount++});
                    }
                    nodes.push_back(std::move(node));
                    nodes[parent].children.push_back(linkedId);
                }
                item.linked = true;
                --remaining;
                progress = true;
            }
            if (!progress) {
                for (const Detail::PendingFragment& item : pending) {
                    if (!item.linked) {
                        return std::unexpected(
                            LinkError{.kind = LinkErrorKind::InvalidMount,
                                      .provider = std::string{item.registration->fragment.provider},
                                      .command = Detail::JoinPath(item.registration->mountPath),
                                      .detail = "target command scope does not exist in the assembled graph"});
                    }
                }
            }
        }

        for (Detail::LinkedCommandNode& node : nodes) {
            std::ranges::sort(node.children, {},
                              [&](std::uint32_t child) -> const std::string& { return nodes[child].name; });
        }
        return LinkedProgram<Root, CommandTree>{root, std::string{root.schema.NameText(root.schema.programName)},
                                                root.schema.policy, std::move(nodes), presenceCount};
    }

    template<typename Root, typename CommandTree>
    void LinkAtStartup(Program<Root, CommandTree>&&, const FragmentRegistrySnapshot&,
                       std::string_view = "program") = delete;

} // namespace Sora::CLI

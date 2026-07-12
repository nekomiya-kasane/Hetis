#pragma once

/**
 * @file Program.h
 * @brief Program-level compile, parse, and dispatch entry points for Sora command-line programs.
 * @ingroup Core
 */

#include <array>
#include <concepts>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include <Sora/Core/CLI/Descriptions.h>
#include <Sora/Core/CLI/Help.h>
#include <Sora/Core/CLI/Parser.h>
#include <Sora/Core/CLI/Schema.h>

namespace Sora::CLI {

    namespace Detail {

        /** @brief Empty dispatch context used by @ref Program::Dispatch overloads without an explicit context. */
        struct EmptyContext {};

        /** @brief Invoke @p command with @p context when supported, otherwise fall back to nullary invocation. */
        template<typename Command, typename Context>
        [[nodiscard]] int DispatchCommand(const Command& command, Context& context) {
            if constexpr (std::same_as<std::remove_cvref_t<Command>, std::monostate>) {
                return 0;
            } else if constexpr (requires { { command(context) } -> std::convertible_to<int>; }) {
                return command(context);
            } else if constexpr (requires { command(context); }) {
                command(context);
                return 0;
            } else if constexpr (requires { { command() } -> std::convertible_to<int>; }) {
                return command();
            } else if constexpr (requires { command(); }) {
                command();
                return 0;
            } else {
                return 0;
            }
        }

    } // namespace Detail

    /** @brief Sealed static command-line program. */
    template<typename Root, typename CommandTree = CommandTreeOf<Root>>
    struct Program {
        using RootType = Root;
        using CommandTreeType = CommandTree;
        using CommandVariantType = CommandVariantOf<CommandTree>;
        using ResultType = ParseResult<Root, CommandVariantType>;
        using ExpectedResult = ParseExpected<Root, CommandVariantType>;

        NormalizedSchema schema = {}; /**< Sealed descriptor image. */

        /** @brief Parse @p argv into a typed root object and selected command object. */
        [[nodiscard]] ExpectedResult Parse(ArgvView argv) const {
            ResultType result{};
            std::array<bool, 1024> presence{};
            std::array<std::uint32_t, 1024> operandCounts{};

            CommandId current = 0;
            std::uint32_t operandIndex = 0;
            bool afterDelimiter = false;

            for (std::size_t i = 0; i < argv.Size(); ++i) {
                const std::string_view token = argv[i];
                const SourceRef source{.tokenIndex = static_cast<std::uint32_t>(i)};

                if (!afterDelimiter && token == "--") {
                    afterDelimiter = true;
                    continue;
                }

                if (!afterDelimiter && token.starts_with("--") && token.size() > 2) {
                    const std::string_view body = token.substr(2);
                    const std::size_t eq = body.find('=');
                    const std::string_view name = eq == std::string_view::npos ? body : body.substr(0, eq);
                    std::optional<std::string_view> attached;
                    if (eq != std::string_view::npos) {
                        attached = body.substr(eq + 1);
                    }
                    const OptionDesc* option = FindLongOption(current, name);
                    if (option == nullptr) {
                        return std::unexpected(ParseError{.kind = ParseErrorKind::UnknownOption,
                                                          .source = source,
                                                          .commandId = current,
                                                          .token = token});
                    }

                    if (option->kind == OptionKind::Help) {
                        if (attached.has_value()) {
                            return std::unexpected(ParseError{.kind = ParseErrorKind::UnexpectedValue,
                                                              .source = source,
                                                              .commandId = current,
                                                              .descriptorName = option->longName,
                                                              .token = token});
                        }
                        result.helpCommandId = current;
                        return result;
                    }

                    if (option->kind == OptionKind::Switch) {
                        if (attached.has_value()) {
                            return std::unexpected(ParseError{.kind = ParseErrorKind::UnexpectedValue,
                                                              .source = source,
                                                              .commandId = current,
                                                              .descriptorName = option->longName,
                                                              .token = token});
                        }
                        if (!BindSwitch(result, *option)) {
                            return InvalidValue(source, current, *option, token);
                        }
                    } else {
                        std::string_view value{};
                        if (attached.has_value()) {
                            value = *attached;
                        } else {
                            if (i + 1 >= argv.Size()) {
                                return std::unexpected(ParseError{.kind = ParseErrorKind::MissingValue,
                                                                  .source = source,
                                                                  .commandId = current,
                                                                  .descriptorName = option->longName,
                                                                  .token = token});
                            }
                            value = argv[++i];
                        }
                        if (!BindValue(result, *option, value)) {
                            return InvalidValue(source, current, *option, value);
                        }
                    }
                    presence[option->presenceBit] = true;
                    continue;
                }

                if (!afterDelimiter && token.starts_with("-") && token.size() > 1) {
                    for (std::size_t j = 1; j < token.size(); ++j) {
                        const char shortName = token[j];
                        const OptionDesc* option = FindShortOption(current, shortName);
                        if (option == nullptr) {
                            return std::unexpected(ParseError{.kind = ParseErrorKind::UnknownOption,
                                                              .source = source,
                                                              .commandId = current,
                                                              .token = token,
                                                              .shortName = shortName});
                        }

                        if (option->kind == OptionKind::Help) {
                            result.helpCommandId = current;
                            return result;
                        }

                        if (option->kind == OptionKind::Switch) {
                            if (!BindSwitch(result, *option)) {
                                return InvalidValue(source, current, *option, token);
                            }
                            presence[option->presenceBit] = true;
                            continue;
                        }

                        std::string_view value{};
                        if (j + 1 < token.size()) {
                            value = token.substr(j + 1);
                            j = token.size();
                        } else {
                            if (i + 1 >= argv.Size()) {
                                return std::unexpected(ParseError{.kind = ParseErrorKind::MissingValue,
                                                                  .source = source,
                                                                  .commandId = current,
                                                                  .descriptorName = option->longName,
                                                                  .token = token});
                            }
                            value = argv[++i];
                        }
                        if (!BindValue(result, *option, value)) {
                            return InvalidValue(source, current, *option, value);
                        }
                        presence[option->presenceBit] = true;
                        break;
                    }
                    continue;
                }

                if (!afterDelimiter) {
                    if (const CommandId child = FindChild(current, token); child != kInvalidCommandId) {
                        current = child;
                        result.commandId = current;
                        operandIndex = 0;
                        if (!EmplaceCommand(result, current)) {
                            return std::unexpected(ParseError{.kind = ParseErrorKind::UnknownCommand,
                                                              .source = source,
                                                              .commandId = current,
                                                              .token = token});
                        }
                        continue;
                    }
                }

                if (!BindOperand(result, current, operandIndex, token, presence, operandCounts)) {
                    const ParseErrorKind kind =
                        current == 0 && schema.commands[0].childCount != 0 ? ParseErrorKind::UnknownCommand
                                                                            : ParseErrorKind::TooManyOperands;
                    return std::unexpected(
                        ParseError{.kind = kind, .source = source, .commandId = current, .token = token});
                }
            }

            if (current == 0 && schema.commands[0].childCount != 0) {
                return std::unexpected(ParseError{.kind = ParseErrorKind::MissingCommand, .commandId = 0});
            }

            if (auto error = CheckRequired(schema.commands[0], presence, operandCounts); error.has_value()) {
                return std::unexpected(*error);
            }
            if (current != 0) {
                if (auto error = CheckRequired(schema.commands[current], presence, operandCounts); error.has_value()) {
                    return std::unexpected(*error);
                }
            }

            return result;
        }

        /** @brief Dispatch the selected command with an explicit context object. */
        template<typename Context>
        [[nodiscard]] int Dispatch(const ResultType& result, Context& context) const {
            if (result.HelpRequested()) {
                PrintHelp(result);
                return 0;
            }
            return std::visit([&](const auto& command) { return Detail::DispatchCommand(command, context); },
                              result.command);
        }

        /** @brief Dispatch the selected command without an explicit context object. */
        [[nodiscard]] int Dispatch(const ResultType& result) const {
            Detail::EmptyContext context{};
            return Dispatch(result, context);
        }

        /** @brief Render help for command scope @p commandId from the sealed schema. */
        [[nodiscard]] std::string FormatHelp(CommandId commandId = 0, HelpRenderOptions options = {}) const {
            return Sora::CLI::FormatHelp(schema, commandId, options);
        }

        /** @brief Render help for the scope selected by @p result, or the program root when no help was requested. */
        [[nodiscard]] std::string FormatHelp(const ResultType& result, HelpRenderOptions options = {}) const {
            const CommandId commandId = result.HelpRequested() ? result.helpCommandId : 0;
            return Sora::CLI::FormatHelp(schema, commandId, options);
        }

        /** @brief Write help for command scope @p commandId using terminal-aware tapioca styling. */
        void PrintHelp(CommandId commandId = 0, HelpRenderOptions options = {}) const {
            Sora::CLI::PrintHelp(schema, commandId, options);
        }

        /** @brief Write help for the scope selected by @p result using terminal-aware tapioca styling. */
        void PrintHelp(const ResultType& result, HelpRenderOptions options = {}) const {
            const CommandId commandId = result.HelpRequested() ? result.helpCommandId : 0;
            Sora::CLI::PrintHelp(schema, commandId, options);
        }

        /** @brief Return a compact human-readable error string for diagnostics and tests. */
        [[nodiscard]] std::string FormatError(const ParseError& error) const {
            const std::string_view token = error.token;
            switch (error.kind) {
            case ParseErrorKind::MissingCommand:
                return "missing command";
            case ParseErrorKind::UnknownCommand:
                return std::format("unknown command '{}'", token);
            case ParseErrorKind::UnknownOption:
                return error.shortName == '\0' ? std::format("unknown option '{}'", token)
                                               : std::format("unknown short option '-{}'", error.shortName);
            case ParseErrorKind::MissingValue:
                return std::format("missing value for '{}'", NameText(error.descriptorName));
            case ParseErrorKind::UnexpectedValue:
                return std::format("unexpected value for '{}'", NameText(error.descriptorName));
            case ParseErrorKind::InvalidValue:
                return std::format("invalid value '{}' for '{}'", token, NameText(error.descriptorName));
            case ParseErrorKind::TooManyOperands:
                return std::format("too many operands near '{}'", token);
            case ParseErrorKind::MissingRequiredOption:
                return std::format("missing required option '{}'", NameText(error.descriptorName));
            case ParseErrorKind::MissingRequiredOperand:
                return std::format("missing required operand '{}'", NameText(error.descriptorName));
            }
            return "parse error";
        }

    private:
        [[nodiscard]] std::string_view NameText(NameId id) const noexcept { return schema.NameText(id); }

        [[nodiscard]] CommandId FindChild(CommandId parent, std::string_view name) const noexcept {
            const CommandDesc& command = schema.commands[parent];
            for (std::uint32_t i = 0; i < command.childCount; ++i) {
                const CommandEdge& edge = schema.edges[command.childBegin + i];
                if (NameText(edge.name) == name) {
                    return edge.childCommandId;
                }
            }
            return kInvalidCommandId;
        }

        [[nodiscard]] const OptionDesc* FindLongOption(CommandId commandId, std::string_view name) const noexcept {
            if (const OptionDesc* option = FindLongOptionIn(commandId, name, false); option != nullptr) {
                return option;
            }
            if (commandId != 0 && Sora::HasFlag(schema.policy, Policy::GlobalOptionsAnywhere)) {
                return FindLongOptionIn(0, name, true);
            }
            return nullptr;
        }

        [[nodiscard]] const OptionDesc* FindShortOption(CommandId commandId, char name) const noexcept {
            if (const OptionDesc* option = FindShortOptionIn(commandId, name, false); option != nullptr) {
                return option;
            }
            if (commandId != 0 && Sora::HasFlag(schema.policy, Policy::GlobalOptionsAnywhere)) {
                return FindShortOptionIn(0, name, true);
            }
            return nullptr;
        }

        [[nodiscard]] const OptionDesc* FindLongOptionIn(CommandId commandId, std::string_view name,
                                                         bool requireGlobal) const noexcept {
            const CommandDesc& command = schema.commands[commandId];
            for (std::uint32_t i = 0; i < command.optionCount; ++i) {
                const OptionDesc& option = schema.options[command.optionBegin + i];
                if ((!requireGlobal || option.global) && NameText(option.longName) == name) {
                    return &option;
                }
            }
            return nullptr;
        }

        [[nodiscard]] const OptionDesc* FindShortOptionIn(CommandId commandId, char name,
                                                          bool requireGlobal) const noexcept {
            const CommandDesc& command = schema.commands[commandId];
            for (std::uint32_t i = 0; i < command.optionCount; ++i) {
                const OptionDesc& option = schema.options[command.optionBegin + i];
                if ((!requireGlobal || option.global) && option.shortName == name) {
                    return &option;
                }
            }
            return nullptr;
        }

        [[nodiscard]] bool BindSwitch(ResultType& result, const OptionDesc& option) const noexcept {
            void* object = ObjectFor(result, option.ownerCommandId);
            return object != nullptr && option.bindSwitch != nullptr && option.bindSwitch(object);
        }

        [[nodiscard]] bool BindValue(ResultType& result, const OptionDesc& option,
                                     std::string_view value) const noexcept {
            void* object = ObjectFor(result, option.ownerCommandId);
            return object != nullptr && option.bindValue != nullptr && option.bindValue(object, value);
        }

        [[nodiscard]] bool BindOperand(ResultType& result, CommandId commandId, std::uint32_t& operandIndex,
                                       std::string_view token, std::array<bool, 1024>& presence,
                                       std::array<std::uint32_t, 1024>& operandCounts) const noexcept {
            const CommandDesc& command = schema.commands[commandId];
            if (operandIndex >= command.operandCount) {
                return false;
            }

            const OperandDesc& operand = schema.operands[command.operandBegin + operandIndex];
            void* object = ObjectFor(result, operand.ownerCommandId);
            if (object == nullptr || operand.bindValue == nullptr || !operand.bindValue(object, token)) {
                return false;
            }

            presence[operand.presenceBit] = true;
            ++operandCounts[operand.presenceBit];
            if (operand.cardinality == ValueCardinality::One || operand.cardinality == ValueCardinality::OptionalOne) {
                ++operandIndex;
            }
            return true;
        }

        [[nodiscard]] ExpectedResult InvalidValue(SourceRef source, CommandId commandId, const OptionDesc& option,
                                                  std::string_view token) const {
            return std::unexpected(ParseError{.kind = ParseErrorKind::InvalidValue,
                                              .source = source,
                                              .commandId = commandId,
                                              .descriptorName = option.longName,
                                              .token = token});
        }

        [[nodiscard]] std::optional<ParseError> CheckRequired(
            const CommandDesc& command, const std::array<bool, 1024>& presence,
            const std::array<std::uint32_t, 1024>& operandCounts) const noexcept {
            for (std::uint32_t i = 0; i < command.optionCount; ++i) {
                const OptionDesc& option = schema.options[command.optionBegin + i];
                if (option.required && !presence[option.presenceBit]) {
                    return ParseError{.kind = ParseErrorKind::MissingRequiredOption,
                                      .commandId = command.commandId,
                                      .descriptorName = option.longName};
                }
            }
            for (std::uint32_t i = 0; i < command.operandCount; ++i) {
                const OperandDesc& operand = schema.operands[command.operandBegin + i];
                const bool required =
                    operand.cardinality == ValueCardinality::One || operand.cardinality == ValueCardinality::OneOrMore;
                if (required && operandCounts[operand.presenceBit] == 0) {
                    return ParseError{.kind = ParseErrorKind::MissingRequiredOperand,
                                      .commandId = command.commandId,
                                      .descriptorName = operand.name};
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] bool EmplaceCommand(ResultType& result, CommandId commandId) const {
            const VariantId variantId = schema.commands[commandId].variantId;
            return EmplaceVariant(result.command, variantId);
        }

        [[nodiscard]] void* ObjectFor(ResultType& result, CommandId commandId) const noexcept {
            if (commandId == 0) {
                return &result.root;
            }
            if (result.commandId != commandId) {
                return nullptr;
            }
            return VariantObject(result.command, schema.commands[commandId].variantId);
        }

        template<typename... Ts>
        [[nodiscard]] static bool EmplaceVariant(std::variant<std::monostate, Ts...>& variant, VariantId variantId) {
            bool matched = false;
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                auto emplaceOne = [&]<std::size_t Index> {
                    if (variantId == Index) {
                        variant.template emplace<Index + 1>();
                        matched = true;
                    }
                };
                (emplaceOne.template operator()<I>(), ...);
            }(std::make_index_sequence<sizeof...(Ts)>{});
            return matched;
        }

        template<typename... Ts>
        [[nodiscard]] static void* VariantObject(std::variant<std::monostate, Ts...>& variant,
                                                 VariantId variantId) noexcept {
            void* result = nullptr;
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                auto selectOne = [&]<std::size_t Index> {
                    if (variantId == Index) {
                        result = static_cast<void*>(&std::get<Index + 1>(variant));
                    }
                };
                (selectOne.template operator()<I>(), ...);
            }(std::make_index_sequence<sizeof...(Ts)>{});
            return result;
        }
    };

    template<Concept::ProgramRoot Root>
    consteval auto Compile() -> Program<Root> {
        SchemaBuilder<Root> builder;
        if constexpr (Concept::HasBuildSchema<Root>) {
            Root::BuildSchema(builder);
        } else if constexpr (Concept::HasDescribe<Root>) {
            Root::Describe(builder);
        }
        return Program<Root>{.schema = builder.Seal()};
    }

    template<Concept::ProgramRoot Root, typename SchemaFactory>
    consteval auto Compile(SchemaFactory factory) -> Program<Root> {
        SchemaBuilder<Root> builder;
        factory(builder);
        return Program<Root>{.schema = builder.Seal()};
    }

} // namespace Sora::CLI

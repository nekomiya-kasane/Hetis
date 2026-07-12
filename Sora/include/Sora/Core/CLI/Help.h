/**
 * @file Help.h
 * @brief Schema-derived plain and tapioca-styled command-line help rendering.
 * @ingroup Core
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Sora/Core/CLI/Schema.h>
#include <Sora/Core/ToStyledString.h>

#include <tapioca/pal.h>
#include <tapioca/terminal.h>

namespace Sora::CLI {

    /** @brief Policy controlling ANSI color emission in generated help text. */
    enum class HelpColorPolicy : std::uint8_t {
        Auto,   /**< Use color only for a capable terminal and when @c NO_COLOR is absent. */
        Always, /**< Force tapioca styling with modern terminal capabilities. */
        Never,  /**< Produce deterministic plain text without ANSI sequences. */
    };

    /** @brief Runtime options controlling help formatting and terminal capability detection. */
    struct HelpRenderOptions {
        HelpColorPolicy color = HelpColorPolicy::Auto; /**< Color-selection policy. */
        std::string_view commandPrefix{};              /**< Optional linked-program path replacing the schema root. */
        std::FILE* output = stdout;                    /**< Target stream used for TTY detection and printing. */
        std::uint32_t width = 0;                       /**< Output width, or zero to detect a suitable value. */
    };

    namespace Detail {

        namespace Styled = Sora::$::Serialization;

        struct HelpEntry {
            std::string label;
            std::string description;
            std::string annotation;
            bool required = false;
        };

        struct HelpSection {
            std::string title;
            std::vector<HelpEntry> entries;
        };

        struct HelpDocument {
            std::string usage;
            std::string about;
            std::vector<HelpSection> sections;
        };

        struct ResolvedHelpRenderOptions {
            bool color = false;
            std::uint32_t width = 100;
            tapioca::terminal_caps caps = tapioca::terminal_caps::legacy_win_cmd();
        };

        [[nodiscard]] inline ResolvedHelpRenderOptions ResolveHelpRenderOptions(HelpRenderOptions options) noexcept {
            ResolvedHelpRenderOptions resolved{};
            std::FILE* output = options.output == nullptr ? stdout : options.output;
            const bool tty = tapioca::terminal::is_tty(output);

            if (options.color == HelpColorPolicy::Always) {
                resolved.color = true;
                resolved.caps = tapioca::terminal_caps::modern();
            } else if (options.color == HelpColorPolicy::Auto && tty && std::getenv("NO_COLOR") == nullptr) {
                resolved.caps = tapioca::terminal_caps::detect();
                resolved.color = resolved.caps.vt_sequences && resolved.caps.max_colors != tapioca::color_depth::none;
            }

            if (options.width != 0) {
                resolved.width = options.width;
            } else if (tty) {
                const std::uint32_t detected = tapioca::terminal::get_size().width;
                resolved.width = detected == 0 ? 100 : detected;
            }
            resolved.width = std::clamp(resolved.width, 40u, 160u);
            return resolved;
        }

        inline void AppendWords(Styled::StyledStringBuilder& builder, Styled::StyledRole role, std::string_view text,
                                std::size_t firstColumn, std::size_t continuationColumn, std::size_t width) {
            std::size_t column = firstColumn;
            std::size_t position = 0;
            bool firstWord = true;
            while (position < text.size()) {
                while (position < text.size() && text[position] == ' ') {
                    ++position;
                }
                if (position == text.size()) {
                    break;
                }

                const std::size_t end = text.find(' ', position);
                const std::string_view word = text.substr(position, end == std::string_view::npos
                                                                        ? text.size() - position
                                                                        : end - position);
                const std::size_t separator = firstWord ? 0 : 1;
                if (!firstWord && column + separator + word.size() > width) {
                    builder.Raw("\n");
                    builder.Raw(std::string(continuationColumn, ' '));
                    column = continuationColumn;
                    firstWord = true;
                }
                if (!firstWord) {
                    builder.Raw(" ");
                    ++column;
                }
                builder.Text(role, word);
                column += word.size();
                firstWord = false;
                position = end == std::string_view::npos ? text.size() : end + 1;
            }
        }

        [[nodiscard]] inline std::string RenderHelpDocument(const HelpDocument& document, HelpRenderOptions options) {
            const ResolvedHelpRenderOptions resolved = ResolveHelpRenderOptions(options);
            Styled::StyledStringBuilder builder{{.color = resolved.color,
                                                 .escapePolicy = Styled::StyledEscapePolicy::TerminalSafe,
                                                 .caps = resolved.caps}};

            builder.Text(Styled::StyledRole::EnumName, "Usage");
            builder.Raw(Styled::StyledRole::Punctuation, ": ");
            builder.Text(Styled::StyledRole::TypeName, document.usage);
            builder.Raw("\n");

            if (!document.about.empty()) {
                builder.Raw("\n");
                AppendWords(builder, Styled::StyledRole::Plain, document.about, 0, 0, resolved.width);
                builder.Raw("\n");
            }

            for (const HelpSection& section : document.sections) {
                builder.Raw("\n");
                builder.Text(Styled::StyledRole::TypeName, section.title);
                builder.Raw(Styled::StyledRole::Punctuation, ":\n");

                std::size_t labelWidth = 0;
                for (const HelpEntry& entry : section.entries) {
                    labelWidth = std::max(labelWidth, entry.label.size());
                }
                labelWidth = std::min(labelWidth, std::min<std::size_t>(32, resolved.width / 2));
                const std::size_t descriptionColumn = 4 + labelWidth;

                for (const HelpEntry& entry : section.entries) {
                    builder.Raw("  ");
                    builder.Text(Styled::StyledRole::FieldName, entry.label);
                    const bool hasDetail = !entry.description.empty() || entry.required || !entry.annotation.empty();
                    if (!hasDetail) {
                        builder.Raw("\n");
                        continue;
                    }
                    std::size_t column = 2 + entry.label.size();
                    if (entry.label.size() > labelWidth) {
                        builder.Raw("\n");
                        builder.Raw(std::string(descriptionColumn, ' '));
                        column = descriptionColumn;
                    } else {
                        const std::size_t padding = descriptionColumn - column;
                        builder.Raw(std::string(padding, ' '));
                        column += padding;
                    }

                    AppendWords(builder, Styled::StyledRole::Plain, entry.description, column, descriptionColumn,
                                resolved.width);
                    if (entry.required) {
                        builder.Text(Styled::StyledRole::Error, " (required)");
                    }
                    if (!entry.annotation.empty()) {
                        builder.Text(Styled::StyledRole::Null, " [");
                        builder.Text(Styled::StyledRole::Null, entry.annotation);
                        builder.Text(Styled::StyledRole::Null, "]");
                    }
                    builder.Raw("\n");
                }
            }
            return std::move(builder).Finish();
        }

        [[nodiscard]] inline std::string OptionLabel(const NormalizedSchema& schema, const OptionDesc& option) {
            std::string label;
            if (option.shortName != '\0') {
                label = std::string{"-"} + option.shortName + ", ";
            } else {
                label = "    ";
            }
            label += "--";
            label += schema.NameText(option.longName);
            if (option.kind == OptionKind::Parameter) {
                label += " <";
                const std::string_view valueName = schema.NameText(option.valueName);
                label += valueName.empty() ? "value" : valueName;
                label += ">";
            }
            return label;
        }

        [[nodiscard]] inline std::string OperandLabel(const NormalizedSchema& schema, const OperandDesc& operand) {
            const std::string name{schema.NameText(operand.name)};
            switch (operand.cardinality) {
            case ValueCardinality::One:
                return "<" + name + ">";
            case ValueCardinality::OptionalOne:
                return "[" + name + "]";
            case ValueCardinality::OneOrMore:
                return "<" + name + ">...";
            case ValueCardinality::ZeroOrMore:
                return "[" + name + "]...";
            case ValueCardinality::None:
                return name;
            }
            return name;
        }

        [[nodiscard]] inline std::string CommandPath(const NormalizedSchema& schema, CommandId commandId,
                                                     std::string_view commandPrefix) {
            std::vector<std::string_view> segments;
            for (CommandId current = commandId; current != 0;) {
                const CommandDesc& command = schema.commands[current];
                segments.push_back(schema.NameText(command.name));
                current = command.parentCommandId;
            }
            std::ranges::reverse(segments);

            std::string path{commandPrefix.empty() ? schema.NameText(schema.programName) : commandPrefix};
            for (std::string_view segment : segments) {
                path += " ";
                path += segment;
            }
            return path;
        }

        [[nodiscard]] inline HelpDocument BuildHelpDocument(const NormalizedSchema& schema, CommandId commandId,
                                                            std::string_view commandPrefix) {
            const CommandDesc& command = schema.commands[commandId];
            HelpDocument document{.usage = CommandPath(schema, commandId, commandPrefix),
                                  .about = std::string{schema.NameText(command.about)}};
            document.usage += " [options]";
            for (std::uint32_t index = 0; index < command.operandCount; ++index) {
                document.usage += " ";
                document.usage += OperandLabel(schema, schema.operands[command.operandBegin + index]);
            }
            if (command.childCount != 0) {
                document.usage += " <command>";
            }

            HelpSection options{.title = "Options"};
            for (std::uint32_t index = 0; index < command.optionCount; ++index) {
                const OptionDesc& option = schema.options[command.optionBegin + index];
                options.entries.push_back({.label = OptionLabel(schema, option),
                                           .description = std::string{schema.NameText(option.about)},
                                           .required = option.required});
            }
            document.sections.push_back(std::move(options));

            if (commandId != 0) {
                HelpSection globals{.title = "Global options"};
                const CommandDesc& root = schema.commands[0];
                for (std::uint32_t index = 0; index < root.optionCount; ++index) {
                    const OptionDesc& option = schema.options[root.optionBegin + index];
                    if (option.global) {
                        globals.entries.push_back({.label = OptionLabel(schema, option),
                                                   .description = std::string{schema.NameText(option.about)},
                                                   .required = option.required});
                    }
                }
                if (!globals.entries.empty()) {
                    document.sections.push_back(std::move(globals));
                }
            }

            if (command.operandCount != 0) {
                HelpSection operands{.title = "Operands"};
                for (std::uint32_t index = 0; index < command.operandCount; ++index) {
                    const OperandDesc& operand = schema.operands[command.operandBegin + index];
                    const bool required = operand.cardinality == ValueCardinality::One ||
                                          operand.cardinality == ValueCardinality::OneOrMore;
                    operands.entries.push_back({.label = OperandLabel(schema, operand),
                                                .description = std::string{schema.NameText(operand.about)},
                                                .required = required});
                }
                document.sections.push_back(std::move(operands));
            }

            if (command.childCount != 0) {
                HelpSection subcommands{.title = "Subcommands"};
                for (std::uint32_t index = 0; index < command.childCount; ++index) {
                    const CommandEdge& edge = schema.edges[command.childBegin + index];
                    const CommandDesc& child = schema.commands[edge.childCommandId];
                    subcommands.entries.push_back({.label = std::string{schema.NameText(edge.name)},
                                                   .description = std::string{schema.NameText(child.about)}});
                }
                document.sections.push_back(std::move(subcommands));
            }
            return document;
        }

    } // namespace Detail

    /**
     * @brief Render help for @p commandId directly from a sealed schema image.
     * @param[in] schema Sealed schema providing command, option, operand, and documentation metadata.
     * @param[in] commandId Command scope to describe, where zero denotes the program root.
     * @param[in] options Terminal color and width policy.
     * @return Plain or ANSI-styled help text.
     */
    [[nodiscard]] inline std::string FormatHelp(const NormalizedSchema& schema, CommandId commandId,
                                                HelpRenderOptions options = {}) {
        if (commandId >= schema.commands.size()) {
            commandId = 0;
        }
        return Detail::RenderHelpDocument(Detail::BuildHelpDocument(schema, commandId, options.commandPrefix), options);
    }

    /**
     * @brief Render and write help for @p commandId to the configured output stream.
     * @param[in] schema Sealed schema to describe.
     * @param[in] commandId Command scope to describe.
     * @param[in] options Terminal color, width, and output stream policy.
     */
    inline void PrintHelp(const NormalizedSchema& schema, CommandId commandId, HelpRenderOptions options = {}) {
        std::FILE* output = options.output == nullptr ? stdout : options.output;
        tapioca::pal::write_file(output, FormatHelp(schema, commandId, options));
        tapioca::pal::flush_file(output);
    }

} // namespace Sora::CLI

#pragma once

/**
 * @file Descriptions.h
 * @brief Static descriptor atoms for sealed command-line schemas.
 * @ingroup Core
 */

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

#include <Sora/Core/FixedString.h>
#include <Sora/Core/Flags.h>
#include <Sora/Core/StringUtils.h>

namespace Sora::CLI {

    using NameId = std::uint32_t;
    using CommandId = std::uint32_t;
    using CommandTypeId = std::uint32_t;

    inline constexpr size_t kMaxSchemaNames = 1024;
    inline constexpr size_t kMaxSchemaCommands = 1024;
    inline constexpr size_t kMaxSchemaOptions = 1024;
    inline constexpr size_t kMaxSchemaOperands = 1024;
    inline constexpr size_t kMaxSchemaEdges = 1024;
    inline constexpr size_t kMaxSchemaPresences = kMaxSchemaOptions + kMaxSchemaOperands;

    inline constexpr NameId kInvalidNameId = std::numeric_limits<NameId>::max();
    inline constexpr CommandId kInvalidCommandId = std::numeric_limits<CommandId>::max();
    inline constexpr CommandTypeId kInvalidCommandTypeId = std::numeric_limits<CommandTypeId>::max();
    inline constexpr std::uint32_t kInvalidPresenceBit = std::numeric_limits<std::uint32_t>::max();

    enum class OptionKind : uint8_t {
        Switch,
        Parameter,
        Help,
    };

    inline constexpr std::string_view kHelpOptionName = "help";
    inline constexpr std::string_view kHelpOptionAbout = "Show help for this command.";
    inline constexpr char kHelpOptionShortName = 'h';
    inline constexpr std::string_view kHelpOptionLongToken = "--help";
    inline constexpr std::string_view kHelpOptionShortToken = "-h";
    inline constexpr std::string_view kHelpOptionAssignmentPrefix = "--help=";

    namespace Detail {

        /** @brief Return true when @p text is a valid command path segment, option name, or metavariable name. */
        [[nodiscard]] constexpr bool IsCliName(std::string_view text) noexcept {
            if (text.empty() || (!Sora::Ascii::IsAlpha(text.front()) && !Sora::Ascii::IsDigit(text.front())) ||
                (!Sora::Ascii::IsAlpha(text.back()) && !Sora::Ascii::IsDigit(text.back()))) {
                return false;
            }
            for (char c : text) {
                if (!Sora::Ascii::IsAlpha(c) && !Sora::Ascii::IsDigit(c) && c != '-' && c != '.') {
                    return false;
                }
            }
            return true;
        }

        /** @brief Return true when @p name can be used as a one-character short option. */
        [[nodiscard]] constexpr bool IsShortOptionName(char name) noexcept {
            return name == '\0' || Sora::Ascii::IsAlpha(name) || Sora::Ascii::IsDigit(name);
        }

    } // namespace Detail

    enum class ValueCardinality : uint8_t {
        None,
        One,
        OptionalOne,
        OneOrMore,
        ZeroOrMore,
    };

    enum class Policy : uint8_t {
        None = 0,
        GlobalOptionsAnywhere = 1ull << 0,
    };

    /** @brief Annotation for an option that consumes one value token, such as @c --message text. */
    struct Parameter {
        FixedString<64> name{};      /**< Long option name without leading dashes. */
        char shortName = '\0';       /**< Optional one-character short option name. */
        FixedString<32> valueName{}; /**< Human-readable metavariable name used by help renderers. */
        FixedString<256> about{};    /**< Human-readable help text. */
        bool required = false;       /**< Whether the option must appear after fallback resolution. */
    };

    /** @brief Annotation for an option that does not consume a value token, such as @c --verbose. */
    struct Switch {
        FixedString<64> name{};   /**< Long option name without leading dashes. */
        char shortName = '\0';    /**< Optional one-character short option name. */
        FixedString<256> about{}; /**< Human-readable help text. */
    };

    /** @brief Annotation for a positional token consumed by declaration order. */
    struct Operand {
        FixedString<64> name{};                               /**< Operand name used by diagnostics and help. */
        ValueCardinality cardinality = ValueCardinality::One; /**< Positional arity. */
        FixedString<256> about{};                             /**< Human-readable help text. */
    };

    /** @brief Annotation marking a root option as visible from subcommands. */
    struct Global {
        constexpr bool operator==(const Global&) const = default;
    };

    /** @brief Annotation explicitly replacing a visible root-global option in one local command scope. */
    struct Override {
        constexpr bool operator==(const Override&) const = default;
    };

    /** @brief Optional annotation overriding the default lower-kebab command name of a command type. */
    struct CommandName {
        FixedString<64> name{};   /**< Command path segment. */
        FixedString<256> about{}; /**< Human-readable help text. */
    };

    /** @brief One interned string in a sealed schema image. */
    struct NameEntry {
        const char* data = nullptr;
        std::uint32_t size = 0;

        /** @brief Project the static pointer/length pair as a standard string view. */
        [[nodiscard]] constexpr std::string_view Text() const noexcept {
            return size == 0 ? std::string_view{} : std::string_view{data, size};
        }
    };

    using BindValueFn = bool (*)(void* object, std::string_view value) noexcept;
    using BindSwitchFn = bool (*)(void* object) noexcept;
    using ValidateValueFn = bool (*)(std::string_view value) noexcept;

    /** @brief Leaf option descriptor stored in the sealed schema. */
    struct OptionDesc {
        NameId longName = kInvalidNameId;
        NameId valueName = kInvalidNameId;
        NameId about = kInvalidNameId;
        char shortName = '\0';
        OptionKind kind = OptionKind::Switch;
        ValueCardinality cardinality = ValueCardinality::None;
        CommandId ownerCommandId = kInvalidCommandId;
        std::uint32_t presenceBit = kInvalidPresenceBit;
        bool required = false;
        bool global = false;
        bool overridesGlobal = false;
        BindValueFn bindValue = nullptr;
        BindSwitchFn bindSwitch = nullptr;
        ValidateValueFn validateValue = nullptr;
    };

    /** @brief Positional operand descriptor stored in the sealed schema. */
    struct OperandDesc {
        NameId name = kInvalidNameId;
        NameId about = kInvalidNameId;
        ValueCardinality cardinality = ValueCardinality::One;
        CommandId ownerCommandId = kInvalidCommandId;
        std::uint32_t presenceBit = kInvalidPresenceBit;
        BindValueFn bindValue = nullptr;
    };

    /** @brief Static edge from one command trie node to a child node. */
    struct CommandEdge {
        CommandId parentCommandId = kInvalidCommandId;
        NameId name = kInvalidNameId;
        CommandId childCommandId = kInvalidCommandId;
    };

    /** @brief Command descriptor stored in the sealed schema. */
    struct CommandDesc {
        NameId name = kInvalidNameId;
        NameId about = kInvalidNameId;
        CommandId commandId = kInvalidCommandId;
        CommandId parentCommandId = kInvalidCommandId;
        CommandTypeId typeId = kInvalidCommandTypeId;
        std::uint32_t depth = 0; /**< One-based command depth; the synthetic root uses zero. */
        std::uint32_t optionBegin = 0;
        std::uint32_t optionCount = 0;
        std::uint32_t operandBegin = 0;
        std::uint32_t operandCount = 0;
        std::uint32_t childBegin = 0;
        std::uint32_t childCount = 0;
    };

    /** @brief Position of a token in the caller-provided argument stream. */
    struct SourceRef {
        uint64_t tokenIndex = 0;
    };

    enum class ParseErrorKind : uint8_t {
        MissingCommand,
        UnknownCommand,
        UnknownOption,
        MissingValue,
        UnexpectedValue,
        InvalidValue,
        TooManyOperands,
        MissingRequiredOption,
        MissingRequiredOperand,
    };

    /** @brief Structured parse failure; formatting is intentionally outside the hot parser path. */
    struct ParseError {
        ParseErrorKind kind = ParseErrorKind::UnknownCommand;
        SourceRef source{};
        CommandId commandId = kInvalidCommandId;
        NameId descriptorName = kInvalidNameId;
        std::string_view token{};
        char shortName = '\0';

        /** @brief Conventional process exit code for command-line usage errors. */
        [[nodiscard]] constexpr int ExitCode() const noexcept { return 2; }
    };

    namespace $ {

        using Parameter = Sora::CLI::Parameter;
        using Switch = Sora::CLI::Switch;
        using Operand = Sora::CLI::Operand;
        using Global = Sora::CLI::Global;
        using Override = Sora::CLI::Override;
        using CommandName = Sora::CLI::CommandName;

    } // namespace $

    static_assert(std::is_trivially_copyable_v<NameEntry> && std::is_default_constructible_v<NameEntry>);
    static_assert(std::is_trivially_copyable_v<OptionDesc> && std::is_default_constructible_v<OptionDesc>);
    static_assert(std::is_trivially_copyable_v<OperandDesc> && std::is_default_constructible_v<OperandDesc>);
    static_assert(std::is_trivially_copyable_v<CommandEdge> && std::is_default_constructible_v<CommandEdge>);
    static_assert(std::is_trivially_copyable_v<CommandDesc> && std::is_default_constructible_v<CommandDesc>);
    static_assert(std::is_trivially_copyable_v<SourceRef> && std::is_default_constructible_v<SourceRef>);

} // namespace Sora::CLI

#pragma once

/**
 * @file Descriptions.h
 * @brief Static descriptor atoms for sealed command-line schemas.
 * @ingroup Core
 */

#include <cstdint>
#include <span>
#include <type_traits>

#include <Sora/Core/Flags.h>
#include <Sora/Core/Traits/EnumTraits.h>

namespace Sora::CLI {

    using NameId = std::uint32_t;
    using CommandId = std::uint32_t;

    inline constexpr NameId kInvalidNameId = std::numeric_limits<NameId>::max();
    inline constexpr CommandId kInvalidCommandId = std::numeric_limits<CommandId>::max();

    enum class OptionKind : uint8_t {
        Switch,
        Parameter,
    };

    enum class ValueCardinality : uint8_t {
        None,
        One,
        OptionalOne,
        OneOrMore,
        ZeroOrMore,
    };

    enum class SourceKind : uint8_t {
        Argv,
        ResponseFile,
        Environment,
        Config,
        DefaultValue,
    };

    enum class Policy : uint64_t {
        None = 0,
        GnuStyle = 1ull << 0,
        PosixStrict = 1ull << 1,
        AllowResponseFile = 1ull << 2,
        AllowAbbreviation = 1ull << 3,
        GlobalOptionsBeforeSubcommand = 1ull << 4,
        GlobalOptionsAnywhere = 1ull << 5,
        AllowInterspersedOperands = 1ull << 6,
        Utf8 = 1ull << 7,
    };

    using BindFn = void (*)(void* object, void const* value) noexcept;
    using ParseValueFn = bool (*)(void* output, char const* first, char const* last) noexcept;
    using ActionAdapterFn = int (*)(void const* command, void* context) noexcept;

    /** @brief Leaf option descriptor stored in the sealed schema. */
    struct OptionDesc {
        NameId longName = kInvalidNameId;
        char shortName = '\0';
        OptionKind kind = OptionKind::Switch;
        ValueCardinality cardinality = ValueCardinality::None;
        CommandId ownerCommandId = kInvalidCommandId;
        std::uint32_t fieldId = 0;
        std::uint32_t groupId = 0;
        std::uint32_t presenceBit = 0;
        BindFn bind = nullptr;
        ParseValueFn parseValue = nullptr;
    };

    /** @brief Positional operand descriptor stored in the sealed schema. */
    struct OperandDesc {
        NameId name = kInvalidNameId;
        ValueCardinality cardinality = ValueCardinality::One;
        CommandId ownerCommandId = kInvalidCommandId;
        std::uint32_t fieldId = 0;
        std::uint32_t presenceBit = 0;
        BindFn bind = nullptr;
        ParseValueFn parseValue = nullptr;
    };

    /** @brief Static edge from one command trie node to a child node. */
    struct CommandEdge {
        NameId name = kInvalidNameId;
        CommandId childCommandId = kInvalidCommandId;
    };

    /** @brief Command descriptor stored in the sealed schema. */
    struct CommandDesc {
        NameId name = kInvalidNameId;
        CommandId commandId = kInvalidCommandId;
        std::span<OptionDesc const> localOptions = {};
        std::span<OperandDesc const> operands = {};
        std::span<CommandEdge const> children = {};
        ActionAdapterFn action = nullptr;
    };

    /** @brief Structured provenance for a parsed token or fallback value. */
    struct SourceRef {
        SourceKind kind = SourceKind::Argv;
        uint32_t tokenIndex = 0;
        uint32_t fileId = 0;
        uint32_t line = 0;
        uint32_t column = 0;
        NameId key = kInvalidNameId;
    };

    static_assert(std::is_trivially_copyable_v<OptionDesc> && std::is_default_constructible_v<OptionDesc>);
    static_assert(std::is_trivially_copyable_v<OperandDesc> && std::is_default_constructible_v<OperandDesc>);
    static_assert(std::is_trivially_copyable_v<CommandEdge> && std::is_default_constructible_v<CommandEdge>);
    static_assert(std::is_trivially_copyable_v<CommandDesc> && std::is_default_constructible_v<CommandDesc>);
    static_assert(std::is_trivially_copyable_v<SourceRef> && std::is_default_constructible_v<SourceRef>);

} // namespace Sora::CLI

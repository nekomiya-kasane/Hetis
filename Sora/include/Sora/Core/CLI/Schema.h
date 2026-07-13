#pragma once

/**
 * @file Schema.h
 * @brief Compile-time schema normalisation for Sora command-line programs.
 * @ingroup Core
 */

#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <meta>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include <Sora/Core/ADT/FixedCapacityVector.h>
#include <Sora/Core/CLI/Descriptions.h>
#include <Sora/Core/StringUtils.h>
#include <Sora/Core/Traits/AnnotationTraits.h>
#include <Sora/Core/Traits/EnumTraits.h>
#include <Sora/Core/Traits/ScopeTraits.h>
#include <Sora/Core/Traits/TypeTraits.h>

namespace Sora::CLI {

    template<typename... Nodes>
    struct Commands;

    namespace Detail {

        template<typename T>
        [[nodiscard]] consteval auto DeclaredCommandsOf() {
            if constexpr (requires { typename T::Commands; }) {
                return std::type_identity<typename T::Commands>{};
            } else {
                return std::type_identity<Commands<>>{};
            }
        }

        template<typename T>
        using DeclaredCommandsType = typename decltype(DeclaredCommandsOf<T>())::type;

        template<typename T>
        concept HasDeclaredCommands = requires { typename T::Commands; };

    } // namespace Detail

    /**
     * @brief Type-level command-tree node.
     * @tparam T Typed command object constructed after successful parsing.
     * @tparam Children Static child commands visible below @p T.
     */
    template<typename T, typename Children = Detail::DeclaredCommandsType<T>>
    struct Command {
        using Type = T;
        using ChildrenType = Children;
    };

    /** @brief Type-level list of sibling command nodes. */
    template<typename... Nodes>
    struct Commands {
        static constexpr size_t Count = sizeof...(Nodes);
    };

    template<typename Root>
    class SchemaBuilder;

    /** @brief Canonical sealed descriptor image produced by consteval schema lowering. */
    struct NormalizedSchema {
        NameId programName = kInvalidNameId;     /**< Program display name. */
        Policy policy = Policy::None;            /**< Global parser policy. */
        std::uint32_t presenceCount = 0;         /**< Number of presence bits used by the schema. */
        std::span<const NameEntry> names{};      /**< Interned static-storage strings. */
        std::span<const CommandDesc> commands{}; /**< Command descriptors in depth-first order. */
        std::span<const OptionDesc> options{};   /**< Option descriptors grouped by owning command. */
        std::span<const OperandDesc> operands{}; /**< Operand descriptors grouped by owning command. */
        std::span<const CommandEdge> edges{};    /**< Parent-to-child command edges. */

        /** @brief Return interned text for @p id, or an empty view when @p id is invalid. */
        [[nodiscard]] constexpr std::string_view NameText(NameId id) const noexcept {
            if (id < names.size()) {
                return names[id].Text();
            }
            return {};
        }
    };

    namespace Detail {

        /** @brief Mutable consteval storage used only while lowering a schema into static descriptor arrays. */
        struct SchemaStorage {
            NameId programName = kInvalidNameId;
            Policy policy = Policy::None;
            std::uint32_t presenceCount = 0;
            FixedCapacityVector<NameEntry, kMaxSchemaNames> names{};
            FixedCapacityVector<CommandDesc, kMaxSchemaCommands> commands{};
            FixedCapacityVector<OptionDesc, kMaxSchemaOptions> options{};
            FixedCapacityVector<OperandDesc, kMaxSchemaOperands> operands{};
            FixedCapacityVector<CommandEdge, kMaxSchemaEdges> edges{};
        };

        template<typename T>
        struct IsCommands : std::false_type {};

        template<typename... Nodes>
        struct IsCommands<Commands<Nodes...>> : std::true_type {};

        template<typename T>
        inline constexpr bool IsCommandsV = IsCommands<std::remove_cvref_t<T>>::value;

        template<typename T>
        concept CommandList = IsCommandsV<T>;

        template<typename T>
        concept StringField = std::same_as<T, std::string_view> || std::same_as<T, std::string>;

        template<typename T>
        concept CustomField = requires(T& value, std::string_view text) {
            { ParseValue(value, text) } noexcept -> std::same_as<bool>;
        };

        template<typename T>
        concept ScalarField = CustomField<T> || StringField<T> || (std::integral<T> && !std::same_as<T, bool>) ||
                              std::floating_point<T> || std::is_enum_v<T> || std::same_as<T, bool>;

        template<typename T>
        concept VectorField = requires(T value, typename T::value_type item) {
            typename T::value_type;
            value.push_back(std::move(item));
        } && ScalarField<typename T::value_type>;

        template<typename T>
        concept ParseableField = ScalarField<T> || VectorField<T>;

        template<typename T>
        concept SwitchField = std::same_as<T, bool> || (std::integral<T> && !std::same_as<T, bool>);

        template<std::size_t N>
        [[nodiscard]] consteval FixedString<N> FixedText(std::string_view text, const char* error) {
            if (text.size() > N) {
                throw error;
            }
            return FixedString<N>{text};
        }

        [[nodiscard]] consteval FixedString<64> KebabIdentifier(std::string_view source) {
            return FixedText<64>(Sora::Ascii::ToLowerKebab(source), "Sora CLI identifier exceeds 64 bytes.");
        }

        [[nodiscard]] consteval NameId Intern(SchemaStorage& schema, std::string_view text) {
            if (text.empty()) {
                return kInvalidNameId;
            }

            for (std::size_t index = 0; index < schema.names.size(); ++index) {
                if (schema.names[index].Text() == text) {
                    return static_cast<NameId>(index);
                }
            }

            const auto id = static_cast<NameId>(schema.names.size());
            const char* stable = std::define_static_string(text);
            schema.names.push_back(NameEntry{.data = stable, .size = static_cast<std::uint32_t>(text.size())});
            return id;
        }

        template<typename T>
        [[nodiscard]] consteval std::optional<FixedString<64>> TypeCommandName() {
            auto annotations = std::meta::annotations_of(^^T, ^^CommandName);
            if (annotations.size() > 1) {
                throw "Sora CLI command type can carry at most one command_name annotation.";
            }
            if (annotations.empty()) {
                return std::nullopt;
            }

            const CommandName annotation = std::meta::extract<CommandName>(annotations.front());
            if (!Detail::IsCliName(annotation.name.view())) {
                throw "Sora CLI command_name annotation must contain one valid command path segment.";
            }
            return annotation.name;
        }

        template<typename T>
        [[nodiscard]] consteval FixedString<64> DefaultCommandName() {
            if (auto annotated = TypeCommandName<T>(); annotated.has_value()) {
                return *annotated;
            }
            return KebabIdentifier(std::meta::identifier_of(^^T));
        }

        template<std::meta::info Member>
        [[nodiscard]] consteval FixedString<64> DefaultFieldName() {
            return KebabIdentifier(std::meta::identifier_of(Member));
        }

        template<typename T>
        [[nodiscard]] constexpr bool ParseScalar(T& out, std::string_view text) noexcept {
            if constexpr (CustomField<T>) {
                return ParseValue(out, text);
            } else if constexpr (std::same_as<T, std::string_view>) {
                out = text;
                return true;
            } else if constexpr (std::same_as<T, std::string>) {
                try {
                    out.assign(text);
                    return true;
                } catch (...) {
                    return false;
                }
            } else if constexpr (std::same_as<T, bool>) {
                if (text == "1" || Sora::Ascii::EqualsIgnoreCase(text, "true") ||
                    Sora::Ascii::EqualsIgnoreCase(text, "yes") || Sora::Ascii::EqualsIgnoreCase(text, "on")) {
                    out = true;
                    return true;
                }
                if (text == "0" || Sora::Ascii::EqualsIgnoreCase(text, "false") ||
                    Sora::Ascii::EqualsIgnoreCase(text, "no") || Sora::Ascii::EqualsIgnoreCase(text, "off")) {
                    out = false;
                    return true;
                }
                return false;
            } else if constexpr (std::integral<T>) {
                T value{};
                const char* first = text.data();
                const char* last = text.data() + text.size();
                const auto [ptr, ec] = std::from_chars(first, last, value);
                if (ec != std::errc{} || ptr != last) {
                    return false;
                }
                out = value;
                return true;
            } else if constexpr (std::floating_point<T>) {
                T value{};
                const char* first = text.data();
                const char* last = text.data() + text.size();
                const auto [ptr, ec] = std::from_chars(first, last, value);
                if (ec != std::errc{} || ptr != last) {
                    return false;
                }
                out = value;
                return true;
            } else if constexpr (std::is_enum_v<T>) {
                if (auto value = Sora::Meta::EnumCast<T>(text); value.has_value()) {
                    out = *value;
                    return true;
                }
                using Raw = std::underlying_type_t<T>;
                Raw raw{};
                if (!ParseScalar(raw, text)) {
                    return false;
                }
                out = static_cast<T>(raw);
                return true;
            } else {
                static_assert(Sora::kDependentFalse<T>, "Unsupported Sora CLI scalar field type.");
            }
        }

        template<typename T>
        [[nodiscard]] constexpr bool ParseField(T& out, std::string_view text) noexcept {
            if constexpr (ScalarField<T>) {
                return ParseScalar(out, text);
            } else if constexpr (VectorField<T>) {
                using Element = typename T::value_type;
                Element element{};
                if (!ParseScalar(element, text)) {
                    return false;
                }
                try {
                    out.push_back(std::move(element));
                    return true;
                } catch (...) {
                    return false;
                }
            } else {
                static_assert(Sora::kDependentFalse<T>, "Unsupported Sora CLI field type.");
            }
        }

        template<std::meta::info Member>
        [[nodiscard]] consteval std::size_t MemberBindingDiscriminator() {
            constexpr auto owner = Sora::Meta::ParentScopeOf(Member);
            std::size_t ordinal = 0;
            for (auto member : std::meta::nonstatic_data_members_of(owner, std::meta::access_context::unchecked())) {
                if (member == Member) {
                    return static_cast<std::size_t>(std::meta::offset_of(Member).total_bits()) * 131u + ordinal;
                }
                ++ordinal;
            }
            throw "Sora CLI reflected member does not belong to its parent type.";
        }

        template<std::meta::info Member>
        using MemberBindingToken = std::integral_constant<std::size_t, MemberBindingDiscriminator<Member>()>;

        template<typename CommandType, std::meta::info Member, typename = MemberBindingToken<Member>>
        [[nodiscard]] bool BindValue(void* object, std::string_view value) noexcept {
            using RawFieldType = typename [:std::meta::type_of(Member):];
            using FieldType = std::remove_cvref_t<RawFieldType>;
            auto& typed = *static_cast<CommandType*>(object);
            auto& field = typed.[:Member:];
            return ParseField<FieldType>(field, value);
        }

        template<typename CommandType, std::meta::info Member, typename = MemberBindingToken<Member>>
        [[nodiscard]] bool ValidateValue(std::string_view value) noexcept {
            using RawFieldType = typename [:std::meta::type_of(Member):];
            using FieldType = std::remove_cvref_t<RawFieldType>;
            CommandType object{};
            return ParseField<FieldType>(object.[:Member:], value);
        }

        template<typename CommandType, std::meta::info Member, typename = MemberBindingToken<Member>>
        [[nodiscard]] bool BindSwitch(void* object) noexcept {
            using RawFieldType = typename [:std::meta::type_of(Member):];
            using FieldType = std::remove_cvref_t<RawFieldType>;
            auto& typed = *static_cast<CommandType*>(object);
            auto& field = typed.[:Member:];
            if constexpr (std::same_as<FieldType, bool>) {
                field = true;
                return true;
            } else if constexpr (std::integral<FieldType>) {
                ++field;
                return true;
            } else {
                return false;
            }
        }

        struct CommandOverride {
            std::meta::info type{};
            FixedString<64> name{};
            FixedString<256> about{};
            bool hasName = false;
        };

        struct RequiredOverride {
            std::meta::info member{};
        };

        template<typename Root>
        [[nodiscard]] consteval FixedString<64> ProgramNameOf(const SchemaBuilder<Root>& builder);

        template<typename Root>
        [[nodiscard]] consteval std::optional<CommandOverride> CommandOverrideOf(const SchemaBuilder<Root>& builder,
                                                                                 std::meta::info type);

        template<typename Root>
        [[nodiscard]] consteval bool IsRequiredOverride(const SchemaBuilder<Root>& builder, std::meta::info member);

        template<typename CommandsList>
        struct CommandTypes;

        template<typename CommandsList>
        struct CommandDepth;

        template<typename... Nodes>
        struct CommandTypes<Commands<Nodes...>> {
            using Type = Sora::Traits::Concat<typename CommandTypes<Nodes>::Type...>;
        };

        template<typename... Nodes>
        struct CommandDepth<Commands<Nodes...>> {
            static constexpr std::size_t value = [] {
                std::size_t depth = 0;
                ((depth = depth < CommandDepth<Nodes>::value ? CommandDepth<Nodes>::value : depth), ...);
                return depth;
            }();
        };

        template<typename T, typename Children>
        struct CommandTypes<Command<T, Children>> {
            using Type = Sora::Traits::PushFront<T, typename CommandTypes<Children>::Type>;
        };

        template<typename T, typename Children>
        struct CommandDepth<Command<T, Children>>
            : std::integral_constant<std::size_t, 1 + CommandDepth<Children>::value> {};

        template<>
        struct CommandTypes<Commands<>> {
            using Type = Sora::Traits::TypeList<>;
        };

        template<>
        struct CommandDepth<Commands<>> : std::integral_constant<std::size_t, 0> {};

        template<typename List>
        struct VariantOf;

        template<typename... Ts>
        struct VariantOf<Sora::Traits::TypeList<Ts...>> {
            using Type = std::variant<std::monostate, Ts...>;
        };

        template<typename T>
        consteval void ValidateCommandList() {
            static_assert(CommandList<T>, "Sora CLI command tree must be Sora::CLI::Commands<...>.");
        }

        template<typename T>
        consteval void ValidateCommandNode() {
            static_assert(std::is_default_constructible_v<T>,
                          "Sora CLI command objects must be default-constructible for parse finalisation.");
        }

        template<typename CommandType, std::meta::info Member>
        consteval void ValidateOptionField(OptionKind kind) {
            using RawFieldType = typename [:std::meta::type_of(Member):];
            using FieldType = std::remove_cvref_t<RawFieldType>;
            if constexpr (!ParseableField<FieldType>) {
                throw "Sora CLI option field type is not parseable.";
            }
            if (kind == OptionKind::Switch) {
                if constexpr (!SwitchField<FieldType>) {
                    throw "Sora CLI switch_ fields must be bool or an integral counter.";
                }
            }
        }

        template<typename CommandType, typename Root>
        consteval void AppendFields(SchemaStorage& schema, const SchemaBuilder<Root>& builder, CommandId owner) {
            auto& command = schema.commands[owner];
            command.optionBegin = static_cast<std::uint32_t>(schema.options.size());
            command.operandBegin = static_cast<std::uint32_t>(schema.operands.size());
            schema.options.push_back(OptionDesc{.longName = Intern(schema, kHelpOptionName),
                                                .about = Intern(schema, kHelpOptionAbout),
                                                .shortName = kHelpOptionShortName,
                                                .kind = OptionKind::Help,
                                                .cardinality = ValueCardinality::None,
                                                .ownerCommandId = owner});

            template for (constexpr std::meta::info member : Sora::Traits::DataMembers<CommandType>) {
                const auto option = Sora::$::GetSingleOptional<Parameter, Switch>(member);
                const auto positional = Sora::$::GetSingleOptional<Operand>(member);
                if (option.has_value() && positional.has_value()) {
                    throw "Sora CLI field cannot be both an option and an operand.";
                }
                const bool global = Sora::$::Has<Global>(member);
                const bool overridesGlobal = Sora::$::Has<Override>(member);
                if (global && (!option.has_value() || owner != 0)) {
                    throw "Sora CLI Global annotation is valid only on root-scope options.";
                }
                if (overridesGlobal && (!option.has_value() || owner == 0)) {
                    throw "Sora CLI Override annotation is valid only on local options.";
                }
                if (!option.has_value() && !positional.has_value()) {
                    continue;
                }

                const auto presenceBit = schema.presenceCount++;
                if (option.has_value()) {
                    OptionDesc desc{.ownerCommandId = owner,
                                    .presenceBit = presenceBit,
                                    .required = IsRequiredOverride(builder, member),
                                    .global = global,
                                    .overridesGlobal = overridesGlobal};

                    if (std::holds_alternative<Parameter>(*option)) {
                        const auto annotation = std::get<Parameter>(*option);
                        const auto longName = annotation.name.empty() ? DefaultFieldName<member>() : annotation.name;
                        if (!Detail::IsCliName(longName.view()) ||
                            (!annotation.valueName.empty() && !Detail::IsCliName(annotation.valueName.view())) ||
                            !Detail::IsShortOptionName(annotation.shortName)) {
                            throw "Sora CLI parameter names must be valid long, short, and metavariable names.";
                        }
                        desc.kind = OptionKind::Parameter;
                        desc.cardinality = ValueCardinality::One;
                        desc.longName = Intern(schema, longName.view());
                        desc.valueName = Intern(schema, annotation.valueName.view());
                        desc.about = Intern(schema, annotation.about.view());
                        desc.shortName = annotation.shortName;
                        desc.required = desc.required || annotation.required;
                        ValidateOptionField<CommandType, member>(OptionKind::Parameter);
                        desc.bindValue = &BindValue<CommandType, member>;
                        desc.validateValue = &ValidateValue<CommandType, member>;
                    } else {
                        const auto annotation = std::get<Switch>(*option);
                        const auto longName = annotation.name.empty() ? DefaultFieldName<member>() : annotation.name;
                        if (!Detail::IsCliName(longName.view()) || !Detail::IsShortOptionName(annotation.shortName)) {
                            throw "Sora CLI switch names must be valid long and short option names.";
                        }
                        desc.kind = OptionKind::Switch;
                        desc.cardinality = ValueCardinality::None;
                        desc.longName = Intern(schema, longName.view());
                        desc.about = Intern(schema, annotation.about.view());
                        desc.shortName = annotation.shortName;
                        ValidateOptionField<CommandType, member>(OptionKind::Switch);
                        desc.bindSwitch = &BindSwitch<CommandType, member>;
                    }

                    for (std::size_t i = command.optionBegin; i < schema.options.size(); ++i) {
                        const OptionDesc& existing = schema.options[i];
                        if (existing.longName == desc.longName) {
                            if (existing.kind == OptionKind::Help) {
                                throw "Sora CLI reserves --help for built-in command help.";
                            }
                            throw "Duplicate Sora CLI option long name in one command scope.";
                        }
                        if (desc.shortName != '\0' && existing.shortName == desc.shortName) {
                            if (existing.kind == OptionKind::Help) {
                                throw "Sora CLI reserves -h for built-in command help.";
                            }
                            throw "Duplicate Sora CLI option short name in one command scope.";
                        }
                    }
                    schema.options.push_back(desc);
                } else {
                    const auto annotation = *positional;
                    const auto operandName = annotation.name.empty() ? DefaultFieldName<member>() : annotation.name;
                    if (!Detail::IsCliName(operandName.view())) {
                        throw "Sora CLI operand names must be valid metavariable names.";
                    }
                    using RawFieldType = typename [:std::meta::type_of(member):];
                    using FieldType = std::remove_cvref_t<RawFieldType>;
                    if constexpr (!ParseableField<FieldType>) {
                        throw "Sora CLI operand field type is not parseable.";
                    }
                    if constexpr (!VectorField<FieldType>) {
                        if (annotation.cardinality == ValueCardinality::ZeroOrMore ||
                            annotation.cardinality == ValueCardinality::OneOrMore) {
                            throw "Repeated Sora CLI operands must bind to a push_back-capable field.";
                        }
                    }

                    schema.operands.push_back(OperandDesc{.name = Intern(schema, operandName.view()),
                                                          .about = Intern(schema, annotation.about.view()),
                                                          .cardinality = annotation.cardinality,
                                                          .ownerCommandId = owner,
                                                          .presenceBit = presenceBit,
                                                          .bindValue = &BindValue<CommandType, member>});
                }
            }

            command.optionCount = static_cast<std::uint32_t>(schema.options.size() - command.optionBegin);
            command.operandCount = static_cast<std::uint32_t>(schema.operands.size() - command.operandBegin);

            bool sawOptional = false;
            for (std::size_t i = command.operandBegin; i < schema.operands.size(); ++i) {
                const auto cardinality = schema.operands[i].cardinality;
                if (cardinality == ValueCardinality::ZeroOrMore || cardinality == ValueCardinality::OneOrMore) {
                    if (i + 1 != schema.operands.size()) {
                        throw "Variadic Sora CLI operand must be the last operand in its command scope.";
                    }
                }
                if (cardinality == ValueCardinality::OptionalOne || cardinality == ValueCardinality::ZeroOrMore) {
                    sawOptional = true;
                }
                if (sawOptional && cardinality == ValueCardinality::One) {
                    throw "Required Sora CLI operand cannot follow an optional operand.";
                }
            }
        }

        template<typename Node, typename Root>
        consteval CommandId ReserveCommand(SchemaStorage& schema, const SchemaBuilder<Root>& builder, CommandId parent,
                                           CommandTypeId typeId) {
            using CommandType = typename Node::Type;
            ValidateCommandNode<CommandType>();

            FixedString<64> name = DefaultCommandName<CommandType>();
            FixedString<256> about{};
            if (auto override = CommandOverrideOf(builder, ^^CommandType); override.has_value()) {
                if (override->hasName) {
                    name = override->name;
                }
                about = override->about;
            }
            if (!Detail::IsCliName(name.view())) {
                throw "Sora CLI command names must be valid path segments.";
            }

            const auto id = static_cast<CommandId>(schema.commands.size());
            schema.commands.push_back(CommandDesc{.name = Intern(schema, name.view()),
                                                  .about = Intern(schema, about.view()),
                                                  .commandId = id,
                                                  .parentCommandId = parent,
                                                  .typeId = typeId,
                                                  .depth = schema.commands[parent].depth + 1});
            return id;
        }

        template<typename Node, typename Root>
        consteval void FillCommand(SchemaStorage& schema, const SchemaBuilder<Root>& builder, CommandId id,
                                   CommandTypeId& nextType);

        template<typename... Nodes, typename Root>
        consteval void AppendChildren(SchemaStorage& schema, const SchemaBuilder<Root>& builder, CommandId parent,
                                      CommandTypeId& nextType, Commands<Nodes...>) {
            constexpr std::size_t count = sizeof...(Nodes);
            auto& parentDesc = schema.commands[parent];
            parentDesc.childBegin = static_cast<std::uint32_t>(schema.edges.size());

            std::array<CommandId, count> childIds{};
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                ((childIds[I] = ReserveCommand<Nodes>(schema, builder, parent, nextType++),
                  schema.edges.push_back(CommandEdge{.parentCommandId = parent,
                                                     .name = schema.commands[childIds[I]].name,
                                                     .childCommandId = childIds[I]})),
                 ...);
            }(std::make_index_sequence<count>{});

            parentDesc.childCount = static_cast<std::uint32_t>(count);
            for (std::size_t i = parentDesc.childBegin; i < schema.edges.size(); ++i) {
                for (std::size_t j = i + 1; j < schema.edges.size(); ++j) {
                    if (schema.edges[i].parentCommandId == parent && schema.edges[j].parentCommandId == parent &&
                        schema.edges[i].name == schema.edges[j].name) {
                        throw "Duplicate Sora CLI child command name in one command scope.";
                    }
                }
            }

            [&]<std::size_t... I>(std::index_sequence<I...>) {
                (FillCommand<Nodes>(schema, builder, childIds[I], nextType), ...);
            }(std::make_index_sequence<count>{});
        }

        template<typename Node, typename Root>
        consteval void FillCommand(SchemaStorage& schema, const SchemaBuilder<Root>& builder, CommandId id,
                                   CommandTypeId& nextType) {
            using CommandType = typename Node::Type;
            using Children = typename Node::ChildrenType;
            AppendFields<CommandType>(schema, builder, id);
            AppendChildren(schema, builder, id, nextType, Children{});
        }

        template<typename CommandsList, typename Root>
        consteval void ValidateBuilderOverrides(const SchemaBuilder<Root>& builder) {
            using CommandTypes = typename Detail::CommandTypes<CommandsList>::Type;
            const auto commandTypes = Sora::Meta::TypeListTypesOf(std::meta::dealias(^^CommandTypes));

            for (const CommandOverride& override : builder.CommandOverrides()) {
                bool found = false;
                for (std::meta::info type : commandTypes) {
                    found = found || std::meta::dealias(type) == std::meta::dealias(override.type);
                }
                if (!found) {
                    throw "Sora CLI command override targets a type outside the command tree.";
                }
            }

            for (const RequiredOverride& required : builder.RequiredOverrides()) {
                if (!std::meta::is_nonstatic_data_member(required.member)) {
                    throw "Sora CLI required override must target a non-static data member.";
                }
                const auto owner = std::meta::dealias(Sora::Meta::ParentScopeOf(required.member));
                bool foundOwner = owner == std::meta::dealias(^^Root);
                for (std::meta::info type : commandTypes) {
                    foundOwner = foundOwner || owner == std::meta::dealias(type);
                }
                const bool hasBinding = !std::meta::annotations_of(required.member, ^^Parameter).empty() ||
                                        !std::meta::annotations_of(required.member, ^^Switch).empty() ||
                                        !std::meta::annotations_of(required.member, ^^Operand).empty();
                if (!foundOwner || !hasBinding) {
                    throw "Sora CLI required override targets a member without a CLI binding in this program.";
                }
            }
        }

        consteval void ValidateGlobalOptionConflicts(const SchemaStorage& schema, bool allowExternalOverrides) {
            const CommandDesc& root = schema.commands[0];
            for (std::size_t commandIndex = 1; commandIndex < schema.commands.size(); ++commandIndex) {
                const CommandDesc& command = schema.commands[commandIndex];
                for (std::uint32_t localIndex = 0; localIndex < command.optionCount; ++localIndex) {
                    const OptionDesc& local = schema.options[command.optionBegin + localIndex];
                    bool matchedOverride = false;
                    for (std::uint32_t rootIndex = 0; rootIndex < root.optionCount; ++rootIndex) {
                        const OptionDesc& global = schema.options[root.optionBegin + rootIndex];
                        if (!global.global) {
                            continue;
                        }
                        if (local.longName == global.longName ||
                            (local.shortName != '\0' && local.shortName == global.shortName)) {
                            if (!local.overridesGlobal || local.longName != global.longName ||
                                local.shortName != global.shortName || local.kind != global.kind ||
                                local.cardinality != global.cardinality) {
                                throw "Sora CLI local/global option collisions require an exact Override declaration.";
                            }
                            matchedOverride = true;
                        }
                    }
                    if (local.overridesGlobal && !matchedOverride && !allowExternalOverrides) {
                        throw "Sora CLI Override option does not match a visible root-global option.";
                    }
                }
            }
        }

        template<typename CommandsList, typename Root>
        [[nodiscard]] consteval NormalizedSchema SealSchema(const SchemaBuilder<Root>& builder) {
            ValidateCommandList<CommandsList>();
            ValidateBuilderOverrides<CommandsList>(builder);

            using CommandTypes = typename Detail::CommandTypes<CommandsList>::Type;
            static_assert(CommandTypes::size == Sora::Traits::Unique<CommandTypes>::size,
                          "Sora CLI command types must be unique within one static command tree.");

            SchemaStorage schema{};
            schema.policy = builder.PolicyValue();
            schema.programName = Intern(schema, ProgramNameOf(builder).view());
            schema.commands.push_back(CommandDesc{.name = schema.programName, .commandId = 0});
            AppendFields<Root>(schema, builder, 0);

            CommandTypeId nextType = 0;
            AppendChildren(schema, builder, 0, nextType, CommandsList{});
            ValidateGlobalOptionConflicts(schema, builder.AllowsExternalOptionOverrides());
            const auto names = std::define_static_array(schema.names);
            const auto commands = std::define_static_array(schema.commands);
            const auto options = std::define_static_array(schema.options);
            const auto operands = std::define_static_array(schema.operands);
            const auto edges = std::define_static_array(schema.edges);
            return NormalizedSchema{.programName = schema.programName,
                                    .policy = schema.policy,
                                    .presenceCount = schema.presenceCount,
                                    .names = names,
                                    .commands = commands,
                                    .options = options,
                                    .operands = operands,
                                    .edges = edges};
        }

    } // namespace Detail

    namespace Concept {

        template<typename T>
        concept HasBuildSchema = requires(SchemaBuilder<T>& builder) { T::BuildSchema(builder); };

        /** @brief Type that can be lowered into a sealed Sora command-line program. */
        template<typename T>
        concept ProgramRoot = Detail::HasDeclaredCommands<T> || HasBuildSchema<T>;

    } // namespace Concept

    /** @brief Consteval schema builder for program-level policy and metadata overrides. */
    template<typename Root>
    class SchemaBuilder {
        FixedString<64> programName_{};
        Policy policy_ = Policy::None;
        bool allowExternalOptionOverrides_ = false;
        FixedCapacityVector<Detail::CommandOverride, kMaxSchemaCommands> commandOverrides_{};
        FixedCapacityVector<Detail::RequiredOverride, kMaxSchemaPresences> requiredOverrides_{};

    public:
        /** @brief Set the program name shown by diagnostics and help. */
        consteval SchemaBuilder& Name(std::string_view name) {
            if (!Detail::IsCliName(name)) {
                throw "Sora CLI program names must be valid executable path segments.";
            }
            programName_ = Detail::FixedText<64>(name, "Sora CLI program name exceeds 64 bytes.");
            return *this;
        }

        /** @brief Set parser policy bits. */
        consteval SchemaBuilder& Policy(Sora::CLI::Policy value) noexcept {
            policy_ = value;
            return *this;
        }

        /** @brief Defer unmatched Override validation to startup linking against the host root globals. */
        consteval SchemaBuilder& AllowExternalOptionOverrides() noexcept {
            allowExternalOptionOverrides_ = true;
            return *this;
        }

        /** @brief Override the path segment and help text of command @p Cmd. */
        template<typename Cmd>
        consteval SchemaBuilder& Command(std::string_view name = {}, std::string_view about = {}) {
            if (!name.empty() && !Detail::IsCliName(name)) {
                throw "Sora CLI command override names must be valid path segments.";
            }
            Detail::CommandOverride override{
                .type = ^^Cmd,
                .name = name.empty() ? FixedString<64>{}
                                     : Detail::FixedText<64>(name, "Sora CLI command name exceeds 64 bytes."),
                .about = Detail::FixedText<256>(about, "Sora CLI command description exceeds 256 bytes."),
                .hasName = !name.empty()};
            for (Detail::CommandOverride& existing : commandOverrides_) {
                if (existing.type == override.type) {
                    existing = override;
                    return *this;
                }
            }
            commandOverrides_.push_back(override);
            return *this;
        }

        /** @brief Mark a reflected field as required after all fallback sources are considered. */
        consteval SchemaBuilder& Requires(std::meta::info member) {
            for (const Detail::RequiredOverride& required : requiredOverrides_) {
                if (required.member == member) {
                    return *this;
                }
            }
            requiredOverrides_.push_back(Detail::RequiredOverride{.member = member});
            return *this;
        }

        /** @brief Return the configured policy bits. */
        [[nodiscard]] consteval Sora::CLI::Policy PolicyValue() const noexcept { return policy_; }

        /** @brief Return whether startup linking must resolve otherwise unmatched Override annotations. */
        [[nodiscard]] consteval bool AllowsExternalOptionOverrides() const noexcept {
            return allowExternalOptionOverrides_;
        }

        /** @brief Return the configured program name, or an empty string when defaulted. */
        [[nodiscard]] consteval FixedString<64> ProgramNameOverride() const noexcept { return programName_; }

        /** @brief Return command overrides registered by @ref Command. */
        [[nodiscard]] consteval const auto& CommandOverrides() const noexcept { return commandOverrides_; }

        /** @brief Return required-field overrides registered by @ref Requires. */
        [[nodiscard]] consteval const auto& RequiredOverrides() const noexcept { return requiredOverrides_; }

        /** @brief Seal the schema using @p Root::Commands as the command grammar. */
        [[nodiscard]] consteval NormalizedSchema Seal() const {
            return Detail::SealSchema<Detail::DeclaredCommandsType<Root>>(*this);
        }
    };

    namespace Detail {

        template<typename Root>
        [[nodiscard]] consteval FixedString<64> ProgramNameOf(const SchemaBuilder<Root>& builder) {
            if (!builder.ProgramNameOverride().empty()) {
                return builder.ProgramNameOverride();
            }
            return DefaultCommandName<Root>();
        }

        template<typename Root>
        [[nodiscard]] consteval std::optional<CommandOverride> CommandOverrideOf(const SchemaBuilder<Root>& builder,
                                                                                 std::meta::info type) {
            for (const CommandOverride& override : builder.CommandOverrides()) {
                if (std::meta::dealias(override.type) == std::meta::dealias(type)) {
                    return override;
                }
            }
            return std::nullopt;
        }

        template<typename Root>
        [[nodiscard]] consteval bool IsRequiredOverride(const SchemaBuilder<Root>& builder, std::meta::info member) {
            for (const RequiredOverride& required : builder.RequiredOverrides()) {
                if (required.member == member) {
                    return true;
                }
            }
            return false;
        }

    } // namespace Detail

    template<typename Root>
    using CommandTreeOf = Detail::DeclaredCommandsType<Root>;

    template<typename CommandsList>
    using CommandTypeListOf = typename Detail::CommandTypes<CommandsList>::Type;

    template<typename CommandsList>
    using CommandVariantOf = typename Detail::VariantOf<CommandTypeListOf<CommandsList>>::Type;

    template<typename CommandsList>
    inline constexpr std::size_t CommandDepthOf = Detail::CommandDepth<CommandsList>::value;

} // namespace Sora::CLI

namespace Sora::Concept {

    inline namespace CLI {

        using Sora::CLI::Concept::ProgramRoot;

    } // namespace CLI

} // namespace Sora::Concept

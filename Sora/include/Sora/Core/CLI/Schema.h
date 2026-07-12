#pragma once

/**
 * @file Schema.h
 * @brief Compile-time schema normalisation for Sora command-line programs.
 * @ingroup Core
 */

#include <algorithm>
#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <meta>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

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

        template<typename T, typename = void>
        struct DeclaredCommands {
            using Type = Commands<>;
        };

        template<typename T, typename = void>
        struct HasDeclaredCommands : std::false_type {};

        template<typename T>
        struct DeclaredCommands<T, std::void_t<typename T::Commands>> {
            using Type = typename T::Commands;
        };

        template<typename T>
        struct HasDeclaredCommands<T, std::void_t<typename T::Commands>> : std::true_type {};

        template<typename T>
        inline constexpr bool HasDeclaredCommandsV = HasDeclaredCommands<T>::value;

    } // namespace Detail

    /**
     * @brief Type-level command-tree node.
     * @tparam T Typed command object constructed after successful parsing.
     * @tparam Children Static child commands visible below @p T.
     */
    template<typename T, typename Children = typename Detail::DeclaredCommands<T>::Type>
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
        NameId programName = kInvalidNameId;        /**< Program display name. */
        Policy policy = Policy::None;               /**< Global parser policy. */
        std::uint32_t presenceCount = 0;            /**< Number of presence bits used by the schema. */
        FixedCapacityVector<NameEntry> names = {};  /**< Interned static-storage strings. */
        FixedCapacityVector<CommandDesc> commands = {};
        FixedCapacityVector<OptionDesc> options = {};
        FixedCapacityVector<OperandDesc> operands = {};
        FixedCapacityVector<CommandEdge> edges = {};

        /** @brief Return interned text for @p id, or an empty view when @p id is invalid. */
        [[nodiscard]] constexpr std::string_view NameText(NameId id) const noexcept {
            for (const NameEntry& entry : names) {
                if (entry.id == id) {
                    return entry.text;
                }
            }
            return {};
        }
    };

    namespace Detail {

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
        concept ScalarField =
            StringField<T> || (std::integral<T> && !std::same_as<T, bool>) || std::floating_point<T> ||
            std::is_enum_v<T> || std::same_as<T, bool>;

        template<typename T>
        concept VectorField = requires(T value, typename T::value_type item) {
            typename T::value_type;
            value.push_back(std::move(item));
        };

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

        [[nodiscard]] consteval std::uint64_t NameHash(std::string_view text) noexcept {
            std::uint64_t hash = 14695981039346656037ull;
            for (char c : text) {
                hash ^= static_cast<unsigned char>(c);
                hash *= 1099511628211ull;
            }
            return hash;
        }

        [[nodiscard]] consteval NameId Intern(NormalizedSchema& schema, std::string_view text) {
            if (text.empty()) {
                return kInvalidNameId;
            }

            for (const NameEntry& entry : schema.names) {
                if (entry.text == text) {
                    return entry.id;
                }
            }

            const auto id = static_cast<NameId>(schema.names.size());
            const char* stable = std::define_static_string(text);
            schema.names.push_back(NameEntry{.id = id, .text = {stable, text.size()}, .hash = NameHash(text)});
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
            if (annotation.name.empty()) {
                throw "Sora CLI command_name annotation must not be empty.";
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
            if constexpr (std::same_as<T, std::string_view>) {
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

        template<typename CommandType>
        int ActionAdapter(void const* command, void* context) noexcept {
            const auto& typed = *static_cast<const CommandType*>(command);
            static_cast<void>(context);
            try {
                if constexpr (requires { { typed() } -> std::convertible_to<int>; }) {
                    return typed();
                } else if constexpr (requires { typed(); }) {
                    typed();
                    return 0;
                } else {
                    return 0;
                }
            } catch (...) {
                return 1;
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

        template<typename... Nodes>
        struct CommandTypes<Commands<Nodes...>> {
            using Type = Sora::Traits::Concat<typename CommandTypes<Nodes>::Type...>;
        };

        template<typename T, typename Children>
        struct CommandTypes<Command<T, Children>> {
            using Type = Sora::Traits::PushFront<T, typename CommandTypes<Children>::Type>;
        };

        template<>
        struct CommandTypes<Commands<>> {
            using Type = Sora::Traits::TypeList<>;
        };

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
        consteval void AppendFields(NormalizedSchema& schema, const SchemaBuilder<Root>& builder, CommandId owner) {
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
                if (!option.has_value() && !positional.has_value()) {
                    continue;
                }

                const auto presenceBit = schema.presenceCount++;
                if (option.has_value()) {
                    OptionDesc desc{.ownerCommandId = owner,
                                    .fieldId = presenceBit,
                                    .presenceBit = presenceBit,
                                    .required = IsRequiredOverride(builder, member),
                                    .global = Sora::$::Has<Global>(member)};

                    if (std::holds_alternative<Parameter>(*option)) {
                        const auto annotation = std::get<Parameter>(*option);
                        desc.kind = OptionKind::Parameter;
                        desc.cardinality = ValueCardinality::One;
                        desc.longName = Intern(schema, annotation.name.empty() ? DefaultFieldName<member>().view()
                                                                               : annotation.name.view());
                        desc.valueName = Intern(schema, annotation.valueName.view());
                        desc.about = Intern(schema, annotation.about.view());
                        desc.shortName = annotation.shortName;
                        desc.required = desc.required || annotation.required;
                        ValidateOptionField<CommandType, member>(OptionKind::Parameter);
                        desc.bindValue = &BindValue<CommandType, member>;
                    } else {
                        const auto annotation = std::get<Switch>(*option);
                        desc.kind = OptionKind::Switch;
                        desc.cardinality = ValueCardinality::None;
                        desc.longName = Intern(schema, annotation.name.empty() ? DefaultFieldName<member>().view()
                                                                               : annotation.name.view());
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

                    schema.operands.push_back(OperandDesc{.name = Intern(schema, annotation.name.empty()
                                                                                     ? DefaultFieldName<member>().view()
                                                                                     : annotation.name.view()),
                                                          .about = Intern(schema, annotation.about.view()),
                                                          .cardinality = annotation.cardinality,
                                                          .ownerCommandId = owner,
                                                          .fieldId = presenceBit,
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
        consteval CommandId ReserveCommand(NormalizedSchema& schema, const SchemaBuilder<Root>& builder,
                                           CommandId parent, VariantId variantId) {
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

            const auto id = static_cast<CommandId>(schema.commands.size());
            schema.commands.push_back(CommandDesc{.name = Intern(schema, name.view()),
                                                  .about = Intern(schema, about.view()),
                                                  .commandId = id,
                                                  .parentCommandId = parent,
                                                  .variantId = variantId,
                                                  .action = &ActionAdapter<CommandType>});
            return id;
        }

        template<typename Node, typename Root>
        consteval void FillCommand(NormalizedSchema& schema, const SchemaBuilder<Root>& builder, CommandId id,
                                   VariantId& nextVariant);

        template<typename... Nodes, typename Root>
        consteval void AppendChildren(NormalizedSchema& schema, const SchemaBuilder<Root>& builder, CommandId parent,
                                      VariantId& nextVariant, Commands<Nodes...>) {
            constexpr std::size_t count = sizeof...(Nodes);
            auto& parentDesc = schema.commands[parent];
            parentDesc.childBegin = static_cast<std::uint32_t>(schema.edges.size());

            std::array<CommandId, count> childIds{};
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                ((childIds[I] = ReserveCommand<Nodes>(schema, builder, parent, nextVariant++),
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
                (FillCommand<Nodes>(schema, builder, childIds[I], nextVariant), ...);
            }(std::make_index_sequence<count>{});
        }

        template<typename Node, typename Root>
        consteval void FillCommand(NormalizedSchema& schema, const SchemaBuilder<Root>& builder, CommandId id,
                                   VariantId& nextVariant) {
            using CommandType = typename Node::Type;
            using Children = typename Node::ChildrenType;
            AppendFields<CommandType>(schema, builder, id);
            AppendChildren(schema, builder, id, nextVariant, Children{});
        }

        template<typename CommandsList, typename Root>
        [[nodiscard]] consteval NormalizedSchema SealSchema(const SchemaBuilder<Root>& builder) {
            ValidateCommandList<CommandsList>();

            NormalizedSchema schema{};
            schema.policy = builder.PolicyValue();
            schema.programName = Intern(schema, ProgramNameOf(builder).view());
            schema.commands.push_back(CommandDesc{.name = schema.programName, .commandId = 0});
            AppendFields<Root>(schema, builder, 0);

            VariantId nextVariant = 0;
            AppendChildren(schema, builder, 0, nextVariant, CommandsList{});
            return schema;
        }

    } // namespace Detail

    namespace Concept {

        template<typename T>
        concept HasBuildSchema = requires(SchemaBuilder<T>& builder) { T::BuildSchema(builder); };

        template<typename T>
        concept HasDescribe = requires(SchemaBuilder<T>& builder) { T::Describe(builder); };

        /** @brief Type that can be lowered into a sealed Sora command-line program. */
        template<typename T>
        concept ProgramRoot = Detail::HasDeclaredCommandsV<T> || HasBuildSchema<T> || HasDescribe<T>;

    } // namespace Concept

    /** @brief Consteval schema builder for program-level policy and metadata overrides. */
    template<typename Root>
    class SchemaBuilder {
        FixedString<64> programName_{};
        Policy policy_ = Policy::None;
        FixedCapacityVector<Detail::CommandOverride> commandOverrides_{};
        FixedCapacityVector<Detail::RequiredOverride> requiredOverrides_{};

    public:
        /** @brief Set the program name shown by diagnostics and help. */
        consteval SchemaBuilder& Name(std::string_view name) {
            programName_ = Detail::FixedText<64>(name, "Sora CLI program name exceeds 64 bytes.");
            return *this;
        }

        /** @brief Set parser policy bits. */
        consteval SchemaBuilder& Policy(Sora::CLI::Policy value) noexcept {
            policy_ = value;
            return *this;
        }

        /** @brief Override the path segment and help text of command @p Cmd. */
        template<typename Cmd>
        consteval SchemaBuilder& Command(std::string_view name = {}, std::string_view about = {}) {
            Detail::CommandOverride override{.type = ^^Cmd,
                                             .name = name.empty() ? FixedString<64>{}
                                                                  : Detail::FixedText<64>(
                                                                        name,
                                                                        "Sora CLI command name exceeds 64 bytes."),
                                             .about = Detail::FixedText<256>(
                                                 about, "Sora CLI command description exceeds 256 bytes."),
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
            requiredOverrides_.push_back(Detail::RequiredOverride{.member = member});
            return *this;
        }

        /** @brief Return the configured policy bits. */
        [[nodiscard]] consteval Sora::CLI::Policy PolicyValue() const noexcept { return policy_; }

        /** @brief Return the configured program name, or an empty string when defaulted. */
        [[nodiscard]] consteval FixedString<64> ProgramNameOverride() const noexcept { return programName_; }

        /** @brief Return command overrides registered by @ref Command. */
        [[nodiscard]] consteval const auto& CommandOverrides() const noexcept { return commandOverrides_; }

        /** @brief Return required-field overrides registered by @ref Requires. */
        [[nodiscard]] consteval const auto& RequiredOverrides() const noexcept { return requiredOverrides_; }

        /** @brief Seal the schema using @p Root::Commands as the command grammar. */
        [[nodiscard]] consteval NormalizedSchema Seal() const {
            return Detail::SealSchema<typename Detail::DeclaredCommands<Root>::Type>(*this);
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
    using CommandTreeOf = typename Detail::DeclaredCommands<Root>::Type;

    template<typename CommandsList>
    using CommandTypeListOf = typename Detail::CommandTypes<CommandsList>::Type;

    template<typename CommandsList>
    using CommandVariantOf = typename Detail::VariantOf<CommandTypeListOf<CommandsList>>::Type;

    template<typename Root>
    using ProgramCommandVariant = CommandVariantOf<CommandTreeOf<Root>>;

} // namespace Sora::CLI

namespace Sora::Concept {

    inline namespace CLI {

        using Sora::CLI::Concept::ProgramRoot;

    } // namespace CLI

} // namespace Sora::Concept

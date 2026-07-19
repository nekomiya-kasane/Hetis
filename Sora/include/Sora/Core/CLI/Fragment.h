/**
 * @file Fragment.h
 * @brief Independently sealed CLI subprograms and explicit startup fragment registries.
 * @ingroup Core
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Sora/Core/CLI/Program.h>

namespace Sora::CLI {

    inline constexpr std::uint32_t kCommandFragmentFormat = 2;
    inline constexpr size_t kMaxLinkedCommands = 4096;

    /** @brief Origin of a command visible in a linked program. */
    enum class CommandSource : std::uint8_t {
        Program,
        StartupFragment,
        RuntimeModule,
    };

    /** @brief Outcome category returned by an erased command fragment. */
    enum class InvocationStatus : std::uint8_t {
        Completed,
        InvalidArguments,
        Failed,
    };

    /** @brief Result returned after an erased fragment parses and dispatches one invocation. */
    struct InvocationResult {
        InvocationStatus status = InvocationStatus::Completed; /**< Completion category. */
        int exitCode = 0;                                      /**< Process-style result code. */
        std::string diagnostic;                                /**< Cold-path diagnostic text. */

        /** @brief Construct a successfully dispatched result. */
        [[nodiscard]] static InvocationResult Completed(int exitCode = 0) {
            return {.status = InvocationStatus::Completed, .exitCode = exitCode};
        }

        /** @brief Construct a command-line rejection result. */
        [[nodiscard]] static InvocationResult InvalidArguments(int exitCode, std::string diagnostic) {
            return {.status = InvocationStatus::InvalidArguments,
                    .exitCode = exitCode,
                    .diagnostic = std::move(diagnostic)};
        }

        /** @brief Construct an action or module failure result. */
        [[nodiscard]] static InvocationResult Failed(int exitCode, std::string diagnostic) {
            return {.status = InvocationStatus::Failed, .exitCode = exitCode, .diagnostic = std::move(diagnostic)};
        }
    };

    /** @brief Host invocation metadata supplied to an erased command fragment. */
    struct FragmentInvocation {
        ArgvView argv{};                  /**< Fragment-local tokens beginning with its exported command. */
        std::string_view commandPrefix{}; /**< Host path preceding the fragment-local command tree. */
    };

    using FragmentInvokeFn = InvocationResult (*)(const void* state, const FragmentInvocation& invocation) noexcept;

    /** @brief Link-relevant command metadata copied from one provider-local sealed schema. */
    struct FragmentCommand {
        CommandId commandId = kInvalidCommandId;       /**< Provider-local command id. */
        CommandId parentCommandId = kInvalidCommandId; /**< Provider-local parent id. */
        std::string_view name{};                       /**< Provider-local path segment; empty for the root. */
        std::string_view about{};                      /**< Human-readable command summary. */
        std::uint32_t optionBegin = 0;                 /**< First provider-local option descriptor. */
        std::uint32_t optionCount = 0;                 /**< Number of local option descriptors. */
        std::uint32_t operandBegin = 0;                /**< First provider-local operand descriptor. */
        std::uint32_t operandCount = 0;                /**< Number of local operand descriptors. */
    };

    /** @brief Link-relevant option metadata independent of provider-owned field binders. */
    struct FragmentOption {
        std::uint32_t descriptorId = 0; /**< Provider-local option descriptor id used for validation. */
        std::string_view longName{};    /**< Long option name without leading dashes. */
        std::string_view valueName{};   /**< Metavariable rendered for value-taking options. */
        std::string_view about{};       /**< Human-readable option summary. */
        char shortName = '\0';          /**< Optional one-character short name. */
        OptionKind kind = OptionKind::Switch;
        ValueCardinality cardinality = ValueCardinality::None;
        bool required = false;
        bool global = false;
        bool overridesGlobal = false;
    };

    /** @brief Link-relevant positional operand metadata. */
    struct FragmentOperand {
        std::string_view name{};
        std::string_view about{};
        ValueCardinality cardinality = ValueCardinality::One;
    };

    /** @brief Host-owned projection of one provider's complete command tree. */
    struct FragmentDescription {
        std::vector<FragmentCommand> commands;
        std::vector<FragmentOption> options;
        std::vector<FragmentOperand> operands;
    };

    using FragmentDescribeFn = bool (*)(const void* state, FragmentDescription& output) noexcept;
    using FragmentValidateValueFn = bool (*)(const void* state, std::uint32_t descriptorId,
                                             std::string_view value) noexcept;

    /** @brief Same-ABI view of an independently sealed command subprogram. */
    struct CommandFragment {
        std::uint32_t format = kCommandFragmentFormat;         /**< Internal descriptor format. */
        std::string_view provider;                             /**< Stable provider identity. */
        const void* state = nullptr;                           /**< Opaque state passed to provider thunks. */
        FragmentDescribeFn describe = nullptr;                 /**< Complete command-tree projection thunk. */
        FragmentValidateValueFn validateValue = nullptr;       /**< Provider-local value validation thunk. */
        FragmentInvokeFn invoke = nullptr;                     /**< Provider-local parser and dispatch thunk. */
        CommandSource source = CommandSource::StartupFragment; /**< Provenance used by diagnostics and help. */
    };

    /** @brief Fragment, mount path, and optional lease protecting provider-owned storage and code. */
    struct FragmentRegistration {
        CommandFragment fragment{};
        std::vector<std::string> mountPath;  /**< Host command path whose child namespace receives the fragment. */
        std::shared_ptr<const void> owner{}; /**< Required owner lease for imported runtime modules. */
    };

    /** @brief Failure category produced while sealing a startup fragment snapshot. */
    enum class LinkErrorKind : std::uint8_t {
        InvalidProgram,
        InvalidFragment,
        InvalidMount,
        DuplicateCommand,
        ResourceLimit,
    };

    /** @brief Structured startup-link failure, separate from user command-line parse errors. */
    struct LinkError {
        LinkErrorKind kind = LinkErrorKind::InvalidFragment;
        std::string provider;
        std::string command;
        std::string conflictingProvider;
        std::string detail;

        /** @brief Return a process exit code suitable for startup assembly failures. */
        [[nodiscard]] constexpr int ExitCode() const noexcept { return 70; }

        /** @brief Format this startup-link failure for a human operator. */
        [[nodiscard]] std::string Message() const {
            switch (kind) {
                case LinkErrorKind::InvalidProgram:
                    return std::format("invalid root CLI program: {}", detail);
                case LinkErrorKind::InvalidFragment:
                    return std::format("invalid CLI fragment '{}': {}", provider, detail);
                case LinkErrorKind::InvalidMount:
                    return std::format("CLI fragment '{}' cannot mount at '{}': {}", provider, command, detail);
                case LinkErrorKind::DuplicateCommand:
                    return std::format("CLI command '{}' is provided by both '{}' and '{}'", command, provider,
                                       conflictingProvider);
                case LinkErrorKind::ResourceLimit:
                    return std::format("CLI startup link exceeded its command limit of {}", kMaxLinkedCommands);
            }
            return "CLI startup link failed";
        }
    };

    /** @brief Immutable value snapshot of explicitly registered fragments and their mount requests. */
    class FragmentRegistrySnapshot {
    public:
        /** @brief Return registrations in deterministic insertion order. */
        [[nodiscard]] std::span<const FragmentRegistration> Fragments() const noexcept { return fragments_; }

    private:
        friend class FragmentRegistry;

        explicit FragmentRegistrySnapshot(std::vector<FragmentRegistration> fragments)
            : fragments_(std::move(fragments)) {}

        std::vector<FragmentRegistration> fragments_;
    };

    /** @brief Explicit startup registry whose mutable state is never queried by the parse path. */
    class FragmentRegistry {
    public:
        /** @brief Add a process-lifetime same-ABI fragment at the program root. */
        void Add(CommandFragment fragment) { fragments_.push_back(FragmentRegistration{.fragment = fragment}); }

        /** @brief Add an owner-protected fragment using its existing mount path. */
        void Add(FragmentRegistration registration) { fragments_.push_back(std::move(registration)); }

        /** @brief Add a process-lifetime same-ABI fragment below @p mountPath. */
        void Add(CommandFragment fragment, std::initializer_list<std::string_view> mountPath) {
            Add(FragmentRegistration{.fragment = fragment}, mountPath);
        }

        /** @brief Add an owner-protected fragment below @p mountPath. */
        void Add(FragmentRegistration registration, std::initializer_list<std::string_view> mountPath) {
            registration.mountPath.clear();
            registration.mountPath.reserve(mountPath.size());
            for (std::string_view segment : mountPath) {
                registration.mountPath.emplace_back(segment);
            }
            fragments_.push_back(std::move(registration));
        }

        /** @brief Capture an immutable registry value for startup linking. */
        [[nodiscard]] FragmentRegistrySnapshot Snapshot() const { return FragmentRegistrySnapshot{fragments_}; }

    private:
        std::vector<FragmentRegistration> fragments_;
    };

    namespace Detail {

        template<typename CommandsList>
        struct TopLevelCommandCount;

        template<typename... Nodes>
        struct TopLevelCommandCount<Commands<Nodes...>> : std::integral_constant<size_t, sizeof...(Nodes)> {};

        template<typename CommandsList>
        inline constexpr size_t TopLevelCommandCountV = TopLevelCommandCount<CommandsList>::value;

        /** @brief Copy a normalized schema into binder-independent fragment metadata. */
        [[nodiscard]] inline bool DescribeSchema(const NormalizedSchema& schema, FragmentDescription& output) noexcept {
            try {
                output.commands.clear();
                output.options.clear();
                output.operands.clear();
                output.commands.reserve(schema.commands.size());
                output.options.reserve(schema.options.size());
                output.operands.reserve(schema.operands.size());

                for (const CommandDesc& command : schema.commands) {
                    output.commands.push_back({.commandId = command.commandId,
                                               .parentCommandId = command.parentCommandId,
                                               .name = schema.NameText(command.name),
                                               .about = schema.NameText(command.about),
                                               .optionBegin = command.optionBegin,
                                               .optionCount = command.optionCount,
                                               .operandBegin = command.operandBegin,
                                               .operandCount = command.operandCount});
                }
                for (size_t index = 0; index < schema.options.size(); ++index) {
                    const OptionDesc& option = schema.options[index];
                    output.options.push_back({.descriptorId = static_cast<std::uint32_t>(index),
                                              .longName = schema.NameText(option.longName),
                                              .valueName = schema.NameText(option.valueName),
                                              .about = schema.NameText(option.about),
                                              .shortName = option.shortName,
                                              .kind = option.kind,
                                              .cardinality = option.cardinality,
                                              .required = option.required,
                                              .global = option.global,
                                              .overridesGlobal = option.overridesGlobal});
                }
                for (const OperandDesc& operand : schema.operands) {
                    output.operands.push_back({.name = schema.NameText(operand.name),
                                               .about = schema.NameText(operand.about),
                                               .cardinality = operand.cardinality});
                }
                return true;
            } catch (...) {
                output = {};
                return false;
            }
        }

    } // namespace Detail

    /** @brief Independently sealed static command subprogram suitable for same-ABI startup linking. */
    template<typename Declaration, typename CommandTree = CommandTreeOf<Declaration>>
    struct StaticSubprogram;

    template<typename Declaration, typename... Nodes>
    struct StaticSubprogram<Declaration, Commands<Nodes...>> {
        using CommandTreeType = Commands<Nodes...>;
        using ProgramType = Program<Declaration, CommandTreeType>;

        ProgramType program{};

        /** @brief Parse and dispatch @p invocation entirely inside the declaring module. */
        [[nodiscard]] InvocationResult Execute(const FragmentInvocation& invocation) const noexcept {
            try {
                auto parsed = program.Parse(invocation.argv);
                if (!parsed) {
                    return InvocationResult::InvalidArguments(parsed.error().ExitCode(),
                                                              program.FormatError(parsed.error()));
                }
                if (parsed->HelpRequested()) {
                    program.PrintHelp(*parsed, {.commandPrefix = invocation.commandPrefix});
                    return InvocationResult::Completed();
                }
                return InvocationResult::Completed(program.Dispatch(*parsed));
            } catch (const std::exception& error) {
                return InvocationResult::Failed(1, error.what());
            } catch (...) {
                return InvocationResult::Failed(1, "CLI fragment action raised an unknown exception");
            }
        }

        /** @brief Project this subprogram as a same-ABI startup fragment owned by @p provider. */
        [[nodiscard]] CommandFragment Fragment(std::string_view provider) const& noexcept {
            return {.provider = provider,
                    .state = this,
                    .describe = &Describe,
                    .validateValue = &ValidateValue,
                    .invoke = &Invoke,
                    .source = CommandSource::StartupFragment};
        }

        CommandFragment Fragment(std::string_view) const&& = delete;

    private:
        [[nodiscard]] static bool Describe(const void* state, FragmentDescription& output) noexcept {
            return Detail::DescribeSchema(static_cast<const StaticSubprogram*>(state)->program.schema, output);
        }

        [[nodiscard]] static bool ValidateValue(const void* state, std::uint32_t descriptorId,
                                                std::string_view value) noexcept {
            const auto& schema = static_cast<const StaticSubprogram*>(state)->program.schema;
            if (descriptorId >= schema.options.size()) {
                return false;
            }
            const OptionDesc& option = schema.options[descriptorId];
            return option.validateValue != nullptr && option.validateValue(value);
        }

        [[nodiscard]] static InvocationResult Invoke(const void* state, const FragmentInvocation& invocation) noexcept {
            return static_cast<const StaticSubprogram*>(state)->Execute(invocation);
        }
    };

    /** @brief Compile a module-local command declaration into a sealed static subprogram. */
    template<Concept::ProgramRoot Declaration>
    consteval auto CompileSubprogram() {
        using CommandTree = CommandTreeOf<Declaration>;
        constexpr size_t commandCount = Detail::TopLevelCommandCountV<CommandTree>;

        SchemaBuilder<Declaration> builder;
        builder.AllowExternalOptionOverrides();
        if constexpr (Concept::HasBuildSchema<Declaration>) {
            Declaration::BuildSchema(builder);
        }
        auto program = Program<Declaration>{.schema = builder.Seal()};
        const CommandDesc& root = program.schema.commands[0];
        bool hasRootUserOption = false;
        for (std::uint32_t index = 0; index < root.optionCount; ++index) {
            hasRootUserOption =
                hasRootUserOption || program.schema.options[root.optionBegin + index].kind != OptionKind::Help;
        }
        if (hasRootUserOption || root.operandCount != 0) {
            throw "Sora CLI subprogram declarations cannot own root options or operands.";
        }
        if (root.childCount != commandCount) {
            throw "Sora CLI subprogram root metadata does not match its declared command tree.";
        }
        return StaticSubprogram<Declaration, CommandTree>{.program = program};
    }

} // namespace Sora::CLI

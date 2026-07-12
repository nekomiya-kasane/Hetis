/**
 * @file Fragment.h
 * @brief Compile-time subprograms, startup fragment registries, and immutable linked CLI programs.
 * @ingroup Core
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Sora/Core/CLI/Program.h>

namespace Sora::CLI {

    inline constexpr std::uint32_t kCommandFragmentFormat = 1;
    inline constexpr std::size_t kMaxLinkedCommands = 4096;

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
        ArgvView argv{};                    /**< Command tokens including the fragment's first path segment. */
        std::string_view commandPrefix{};   /**< Host path preceding the fragment-local command tree. */
    };

    using FragmentInvokeFn = InvocationResult (*)(const void* state, const FragmentInvocation& invocation) noexcept;

    /** @brief Public metadata for one top-level command exported by a fragment. */
    struct FragmentCommand {
        std::string_view name;  /**< Top-level command path segment. */
        std::string_view about; /**< Human-readable command summary. */
    };

    /** @brief Same-ABI view of an independently sealed command subprogram. */
    struct CommandFragment {
        std::uint32_t format = kCommandFragmentFormat;              /**< Internal descriptor format. */
        std::string_view provider;                                  /**< Module or component defining the commands. */
        std::span<const FragmentCommand> commands;                   /**< Top-level commands exposed by the fragment. */
        const void* state = nullptr;                                 /**< Opaque state passed back to @ref invoke. */
        FragmentInvokeFn invoke = nullptr;                      /**< Parser and dispatch thunk owned by the provider. */
        CommandSource source = CommandSource::StartupFragment;       /**< Provenance used by diagnostics and help. */
    };

    /** @brief Fragment plus an optional owner lease protecting its strings, state, and function pointers. */
    struct FragmentRegistration {
        CommandFragment fragment{};            /**< Fragment view registered for linking. */
        std::shared_ptr<const void> owner = {}; /**< Lifetime lease, required for imported runtime modules. */
    };

    /** @brief Failure category produced while sealing a startup fragment snapshot. */
    enum class LinkErrorKind : std::uint8_t {
        InvalidFragment,
        DuplicateCommand,
        ResourceLimit,
    };

    /** @brief Structured startup-link failure, separate from user command-line parse errors. */
    struct LinkError {
        LinkErrorKind kind = LinkErrorKind::InvalidFragment; /**< Failure category. */
        std::string provider;                                /**< Provider associated with the failure. */
        std::string command;                                 /**< Command associated with the failure. */
        std::string conflictingProvider;                     /**< Other provider in a name conflict. */
        std::string detail;                                  /**< Additional validation detail. */

        /** @brief Return a process exit code suitable for startup assembly failures. */
        [[nodiscard]] constexpr int ExitCode() const noexcept { return 70; }

        /** @brief Format this startup-link failure for a human operator. */
        [[nodiscard]] std::string Message() const {
            switch (kind) {
            case LinkErrorKind::InvalidFragment:
                return std::format("invalid CLI fragment '{}': {}", provider, detail);
            case LinkErrorKind::DuplicateCommand:
                return std::format("CLI command '{}' is provided by both '{}' and '{}'", command, provider,
                                   conflictingProvider);
            case LinkErrorKind::ResourceLimit:
                return std::format("CLI startup link exceeded its command limit of {}", kMaxLinkedCommands);
            }
            return "CLI startup link failed";
        }
    };

    /** @brief Immutable value snapshot of explicitly registered same-ABI fragments. */
    class FragmentRegistrySnapshot {
    public:
        /** @brief Return the registered fragments in deterministic insertion order. */
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
        /** @brief Add a same-ABI fragment whose provider storage has process lifetime. */
        void Add(CommandFragment fragment) { fragments_.push_back(FragmentRegistration{.fragment = fragment}); }

        /** @brief Add a fragment together with the lease protecting its provider-owned storage. */
        void Add(FragmentRegistration registration) { fragments_.push_back(std::move(registration)); }

        /** @brief Capture an immutable registry value for startup linking. */
        [[nodiscard]] FragmentRegistrySnapshot Snapshot() const { return FragmentRegistrySnapshot{fragments_}; }

    private:
        std::vector<FragmentRegistration> fragments_;
    };

    namespace Detail {

        template<typename CommandsList>
        struct TopLevelCommandCount;

        template<typename... Nodes>
        struct TopLevelCommandCount<Commands<Nodes...>> : std::integral_constant<std::size_t, sizeof...(Nodes)> {};

        template<typename CommandsList>
        inline constexpr std::size_t TopLevelCommandCountV = TopLevelCommandCount<CommandsList>::value;

        struct LinkedCommandRoute {
            std::string name;
            std::string about;
            std::string provider;
            CommandSource source = CommandSource::Program;
            const void* state = nullptr;
            FragmentInvokeFn invoke = nullptr;
            std::shared_ptr<const void> owner;
        };

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

    } // namespace Detail

    /**
     * @brief Independently sealed static command subprogram suitable for same-ABI startup linking.
     * @tparam Declaration Module-local declaration type owning a @ref Commands tree and schema customisation.
     * @tparam CommandTree Static command tree declared by @p Declaration.
     */
    template<typename Declaration, typename CommandTree = CommandTreeOf<Declaration>>
    struct StaticSubprogram;

    template<typename Declaration, typename... Nodes>
    struct StaticSubprogram<Declaration, Commands<Nodes...>> {
        using CommandTreeType = Commands<Nodes...>;
        using ProgramType = Program<Declaration, CommandTreeType>;

        ProgramType program{};                                      /**< Module-local parser and typed action table. */
        std::array<FragmentCommand, sizeof...(Nodes)> commands = {}; /**< Exported top-level command metadata. */

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
                    .commands = commands,
                    .state = this,
                    .invoke = &Invoke,
                    .source = CommandSource::StartupFragment};
        }

        CommandFragment Fragment(std::string_view) const&& = delete;

    private:
        [[nodiscard]] static InvocationResult Invoke(const void* state,
                                                     const FragmentInvocation& invocation) noexcept {
            return static_cast<const StaticSubprogram*>(state)->Execute(invocation);
        }
    };

    /**
     * @brief Compile a module-local command declaration into a sealed static subprogram.
     * @tparam Declaration Type declaring the fragment's command tree and optional schema builder callback.
     * @return Static subprogram whose root scope is an implementation detail and cannot own options or operands.
     */
    template<Concept::ProgramRoot Declaration>
    consteval auto CompileSubprogram() {
        using CommandTree = CommandTreeOf<Declaration>;
        constexpr std::size_t commandCount = Detail::TopLevelCommandCountV<CommandTree>;

        auto program = Compile<Declaration>();
        const CommandDesc& root = program.schema.commands[0];
        bool hasRootUserOption = false;
        for (std::uint32_t index = 0; index < root.optionCount; ++index) {
            hasRootUserOption = hasRootUserOption ||
                                program.schema.options[root.optionBegin + index].kind != OptionKind::Help;
        }
        if (hasRootUserOption || root.operandCount != 0) {
            throw "Sora CLI subprogram declarations cannot own root options or operands.";
        }
        if (root.childCount != commandCount) {
            throw "Sora CLI subprogram root metadata does not match its declared command tree.";
        }

        std::array<FragmentCommand, commandCount> commands{};
        for (std::size_t index = 0; index < commandCount; ++index) {
            const CommandEdge& edge = program.schema.edges[root.childBegin + index];
            const CommandDesc& command = program.schema.commands[edge.childCommandId];
            commands[index] = {.name = program.schema.NameText(edge.name),
                               .about = program.schema.NameText(command.about)};
        }
        return StaticSubprogram<Declaration, CommandTree>{.program = program, .commands = commands};
    }

    /** @brief Failure category returned when an immutable linked program cannot execute an argv stream. */
    enum class LinkedProgramErrorKind : std::uint8_t {
        MissingCommand,
        UnknownCommand,
        InvalidArguments,
        InvocationFailed,
    };

    /** @brief Structured execution failure for an already linked program. */
    struct LinkedProgramError {
        LinkedProgramErrorKind kind = LinkedProgramErrorKind::UnknownCommand; /**< Failure category. */
        int exitCode = 2;                                                     /**< Suggested process result. */
        std::string command;                                                  /**< Selected or rejected command. */
        std::string diagnostic;                                               /**< Human-readable detail. */

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
            case LinkedProgramErrorKind::InvalidArguments:
                return std::format("invalid arguments for '{}'", command);
            case LinkedProgramErrorKind::InvocationFailed:
                return std::format("command '{}' failed before completion", command);
            }
            return "CLI execution failed";
        }
    };

    /**
     * @brief Immutable root program linked with same-ABI and imported runtime command fragments.
     * @tparam Root Root program declaration type.
     * @tparam CommandTree Root program's static command tree.
     */
    template<typename Root, typename CommandTree = CommandTreeOf<Root>>
    class LinkedProgram {
    public:
        using ProgramType = Program<Root, CommandTree>;
        using RunResult = std::expected<int, LinkedProgramError>;

        /** @brief Construct from a static root program and a validated, sorted route snapshot. */
        LinkedProgram(const ProgramType& root, std::string programName, std::vector<Detail::LinkedCommandRoute> routes)
            : root_(std::addressof(root)), programName_(std::move(programName)), routes_(std::move(routes)) {}

        /** @brief Return the root program display name. */
        [[nodiscard]] std::string_view ProgramName() const noexcept { return programName_; }

        /** @brief Return the number of top-level commands in the linked grammar. */
        [[nodiscard]] std::size_t CommandCount() const noexcept { return routes_.size(); }

        /** @brief Return true when @p name identifies a linked top-level command. */
        [[nodiscard]] bool ContainsCommand(std::string_view name) const noexcept { return FindRoute(name) != nullptr; }

        /** @brief Parse and dispatch @p argv through the immutable linked command snapshot. */
        [[nodiscard]] RunResult Run(ArgvView argv) const {
            if (argv.Size() == 0) {
                return std::unexpected(LinkedProgramError{.kind = LinkedProgramErrorKind::MissingCommand});
            }

            const std::string_view commandName = argv[0];
            if (commandName == kHelpOptionLongToken || commandName == kHelpOptionShortToken) {
                PrintHelp();
                return 0;
            }
            if (commandName.starts_with(kHelpOptionAssignmentPrefix)) {
                return std::unexpected(LinkedProgramError{.kind = LinkedProgramErrorKind::InvalidArguments,
                                                          .command = "help",
                                                          .diagnostic = "unexpected value for 'help'"});
            }
            const Detail::LinkedCommandRoute* route = FindRoute(commandName);
            if (route == nullptr) {
                return std::unexpected(LinkedProgramError{.kind = LinkedProgramErrorKind::UnknownCommand,
                                                          .command = std::string{commandName}});
            }

            if (route->source == CommandSource::Program) {
                try {
                    auto parsed = root_->Parse(argv);
                    if (!parsed) {
                        return std::unexpected(LinkedProgramError{.kind = LinkedProgramErrorKind::InvalidArguments,
                                                                  .exitCode = parsed.error().ExitCode(),
                                                                  .command = route->name,
                                                                  .diagnostic = root_->FormatError(parsed.error())});
                    }
                    return root_->Dispatch(*parsed);
                } catch (const std::exception& error) {
                    return std::unexpected(LinkedProgramError{.kind = LinkedProgramErrorKind::InvocationFailed,
                                                              .exitCode = 1,
                                                              .command = route->name,
                                                              .diagnostic = error.what()});
                } catch (...) {
                    return std::unexpected(LinkedProgramError{.kind = LinkedProgramErrorKind::InvocationFailed,
                                                              .exitCode = 1,
                                                              .command = route->name,
                                                              .diagnostic = "CLI action raised an unknown exception"});
                }
            }

            InvocationResult invocation = route->invoke(
                route->state, FragmentInvocation{.argv = argv, .commandPrefix = programName_});
            if (invocation.status == InvocationStatus::Completed) {
                return invocation.exitCode;
            }
            return std::unexpected(LinkedProgramError{
                .kind = invocation.status == InvocationStatus::InvalidArguments
                            ? LinkedProgramErrorKind::InvalidArguments
                            : LinkedProgramErrorKind::InvocationFailed,
                .exitCode = invocation.exitCode,
                .command = route->name,
                .diagnostic = std::move(invocation.diagnostic)});
        }

        /** @brief Render root help from the same immutable route snapshot used by dispatch. */
        [[nodiscard]] std::string FormatHelp(HelpRenderOptions options = {}) const {
            Detail::HelpDocument document{.usage = programName_ + " [options] <command>"};
            Detail::HelpSection optionsSection{.title = "Options"};
            optionsSection.entries.push_back({.label = std::format("{}, {}", kHelpOptionShortToken,
                                                                   kHelpOptionLongToken),
                                              .description = std::string{kHelpOptionAbout}});
            document.sections.push_back(std::move(optionsSection));

            Detail::HelpSection commands{.title = "Subcommands"};
            for (const Detail::LinkedCommandRoute& route : routes_) {
                commands.entries.push_back({.label = route.name,
                                            .description = route.about,
                                            .annotation = std::format("{}:{}", Detail::CommandSourceName(route.source),
                                                                      route.provider)});
            }
            document.sections.push_back(std::move(commands));
            return Detail::RenderHelpDocument(document, options);
        }

        /** @brief Write root help using terminal-aware tapioca styling. */
        void PrintHelp(HelpRenderOptions options = {}) const {
            std::FILE* output = options.output == nullptr ? stdout : options.output;
            tapioca::pal::write_file(output, FormatHelp(options));
            tapioca::pal::flush_file(output);
        }

    private:
        [[nodiscard]] const Detail::LinkedCommandRoute* FindRoute(std::string_view name) const noexcept {
            auto route = std::lower_bound(routes_.begin(), routes_.end(), name,
                                          [](const Detail::LinkedCommandRoute& candidate, std::string_view value) {
                                              return candidate.name < value;
                                          });
            return route != routes_.end() && route->name == name ? std::addressof(*route) : nullptr;
        }

        const ProgramType* root_ = nullptr;
        std::string programName_;
        std::vector<Detail::LinkedCommandRoute> routes_;
    };

    /**
     * @brief Seal @p root and an explicit registry snapshot into one immutable startup-linked program.
     * @param[in] root Static root program. The object must outlive the returned linked program.
     * @param[in] snapshot Immutable fragment set captured before parsing begins.
     * @param[in] rootProvider Provenance label shown for commands defined by the executable.
     * @return Linked program, or a structured assembly error before any argv parsing occurs.
     */
    template<typename Root, typename CommandTree>
    [[nodiscard]] auto LinkAtStartup(const Program<Root, CommandTree>& root, const FragmentRegistrySnapshot& snapshot,
                                     std::string_view rootProvider = "program")
        -> std::expected<LinkedProgram<Root, CommandTree>, LinkError> {
        std::vector<Detail::LinkedCommandRoute> routes;
        routes.reserve(root.schema.commands[0].childCount + snapshot.Fragments().size());

        const CommandDesc& rootCommand = root.schema.commands[0];
        for (std::uint32_t index = 0; index < rootCommand.childCount; ++index) {
            const CommandEdge& edge = root.schema.edges[rootCommand.childBegin + index];
            const CommandDesc& command = root.schema.commands[edge.childCommandId];
            routes.push_back({.name = std::string{root.schema.NameText(edge.name)},
                              .about = std::string{root.schema.NameText(command.about)},
                              .provider = std::string{rootProvider},
                              .source = CommandSource::Program});
        }

        for (const FragmentRegistration& registration : snapshot.Fragments()) {
            const CommandFragment& fragment = registration.fragment;
            const bool validSource = fragment.source == CommandSource::StartupFragment ||
                                     fragment.source == CommandSource::RuntimeModule;
            if (fragment.format != kCommandFragmentFormat || fragment.provider.empty() || fragment.commands.empty() ||
                fragment.state == nullptr || fragment.invoke == nullptr || !validSource) {
                return std::unexpected(LinkError{.kind = LinkErrorKind::InvalidFragment,
                                                 .provider = std::string{fragment.provider},
                                                 .detail = "missing metadata, state, invocation thunk, or commands"});
            }

            for (const FragmentCommand& command : fragment.commands) {
                if (command.name.empty() || command.name.size() > 64 || command.about.size() > 256) {
                    return std::unexpected(LinkError{.kind = LinkErrorKind::InvalidFragment,
                                                     .provider = std::string{fragment.provider},
                                                     .command = std::string{command.name},
                                                     .detail = "command name or description violates schema limits"});
                }
                routes.push_back({.name = std::string{command.name},
                                  .about = std::string{command.about},
                                  .provider = std::string{fragment.provider},
                                  .source = fragment.source,
                                  .state = fragment.state,
                                  .invoke = fragment.invoke,
                                  .owner = registration.owner});
            }
        }

        if (routes.size() > kMaxLinkedCommands) {
            return std::unexpected(LinkError{.kind = LinkErrorKind::ResourceLimit});
        }

        std::ranges::sort(routes, {}, &Detail::LinkedCommandRoute::name);
        for (std::size_t index = 1; index < routes.size(); ++index) {
            if (routes[index - 1].name == routes[index].name) {
                return std::unexpected(LinkError{.kind = LinkErrorKind::DuplicateCommand,
                                                 .provider = routes[index - 1].provider,
                                                 .command = routes[index].name,
                                                 .conflictingProvider = routes[index].provider});
            }
        }

        std::string programName{root.schema.NameText(root.schema.programName)};
        return LinkedProgram<Root, CommandTree>{root, std::move(programName), std::move(routes)};
    }

    template<typename Root, typename CommandTree>
    void LinkAtStartup(Program<Root, CommandTree>&&, const FragmentRegistrySnapshot&,
                       std::string_view = "program") = delete;

} // namespace Sora::CLI

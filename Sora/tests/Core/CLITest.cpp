#include <Sora/Core/CLI.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <charconv>
#include <string_view>
#include <vector>

namespace {

    struct Port {
        int value = 0;
    };

    [[nodiscard]] bool FromString(Port& port, std::string_view text) noexcept {
        if (!text.starts_with("tcp:")) {
            return false;
        }
        int value = 0;
        const auto [end, error] = std::from_chars(text.data() + 4, text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size()) {
            return false;
        }
        port.value = value;
        return true;
    }

    struct TestContext {
        int base = 0;
    };

    struct Commit {
        [[=Sora::CLI::Parameter{.name = "message", .shortName = 'm', .valueName = "text", .required = true}]]
        std::string_view message{};

        [[=Sora::CLI::Switch{.name = "amend", .shortName = 'a'}]]
        bool amend = false;

        [[=Sora::CLI::Parameter{.name = "port"}]]
        Port port{};

        [[=Sora::CLI::Operand{.name = "paths", .cardinality = Sora::CLI::ValueCardinality::ZeroOrMore}]]
        std::vector<std::string_view> paths;

        [[nodiscard]] int operator()(const TestContext& context) const noexcept {
            return context.base + (amend ? 7 : 3);
        }
    };

    struct RemoteAdd {
        [[=Sora::CLI::Operand{.name = "name"}]]
        std::string_view name{};

        [[=Sora::CLI::Operand{.name = "url"}]]
        std::string_view url{};

        [[nodiscard]] int operator()() const noexcept { return name.empty() || url.empty() ? 1 : 0; }
    };

    struct Remote {
        [[=Sora::CLI::Parameter{.name = "endpoint", .required = true}]]
        std::string_view endpoint{};

        using Commands = Sora::CLI::Commands<Sora::CLI::Command<RemoteAdd>>;
    };

    struct TestProgram {
        [[=Sora::CLI::Switch{.name = "verbose", .shortName = 'v'}, =Sora::CLI::Global{}]]
        bool verbose = false;

        using Commands = Sora::CLI::Commands<Sora::CLI::Command<Commit>, Sora::CLI::Command<Remote>>;

        static consteval void BuildSchema(Sora::CLI::SchemaBuilder<TestProgram>& schema) {
            schema.Name("tool").Policy(Sora::CLI::Policy::GlobalOptionsAnywhere)
                .Command<Commit>("commit")
                .Command<Remote>("remote")
                .Command<RemoteAdd>("add");
        }
    };

    inline constexpr auto kProgram = Sora::CLI::Compile<TestProgram>();
    static_assert(decltype(kProgram)::kCommandDepth == 2);
    static_assert(sizeof(Sora::CLI::NormalizedSchema) <= 128);

    [[nodiscard]] Sora::CLI::ArgvView Tokens(std::span<const std::string_view> tokens) noexcept {
        return Sora::CLI::ArgvView{.tokens = tokens};
    }

} // namespace

TEST_CASE("CLI parses annotated options, short clusters, and variadic operands", "[Sora][Core][CLI]") {
    const std::array args{std::string_view{"-v"}, std::string_view{"commit"}, std::string_view{"-am"},
                          std::string_view{"hello"}, std::string_view{"--port"}, std::string_view{"tcp:443"},
                          std::string_view{"src"}, std::string_view{"include"}};

    auto parsed = kProgram.Parse(Tokens(args));

    REQUIRE(parsed.has_value());
    REQUIRE(parsed->root.verbose);
    REQUIRE(parsed->CommandObject<Commit>().amend);
    REQUIRE(parsed->CommandObject<Commit>().message == "hello");
    REQUIRE(parsed->CommandObject<Commit>().port.value == 443);
    REQUIRE(parsed->CommandObject<Commit>().paths == std::vector<std::string_view>{"src", "include"});

    TestContext context{.base = 10};
    REQUIRE(kProgram.Dispatch(*parsed, context) == 17);
}

TEST_CASE("CLI accepts global options after a subcommand when policy allows it", "[Sora][Core][CLI]") {
    const std::array args{std::string_view{"commit"}, std::string_view{"--message=hello"},
                          std::string_view{"--verbose"}};

    auto parsed = kProgram.Parse(Tokens(args));

    REQUIRE(parsed.has_value());
    REQUIRE(parsed->root.verbose);
    REQUIRE(parsed->CommandObject<Commit>().message == "hello");
}

TEST_CASE("CLI parses nested subcommands from the static command tree", "[Sora][Core][CLI]") {
    const std::array args{std::string_view{"remote"}, std::string_view{"--endpoint"}, std::string_view{"upstream"},
                          std::string_view{"add"}, std::string_view{"origin"},
                          std::string_view{"https://example.invalid/repo.git"}};

    auto parsed = kProgram.Parse(Tokens(args));

    REQUIRE(parsed.has_value());
    REQUIRE(parsed->CommandObject<Remote>().endpoint == "upstream");
    REQUIRE(parsed->CommandObject<RemoteAdd>().name == "origin");
    REQUIRE(parsed->CommandObject<RemoteAdd>().url == "https://example.invalid/repo.git");
    REQUIRE(kProgram.Dispatch(*parsed) == 0);
}

TEST_CASE("CLI validates required fields on every selected command path node", "[Sora][Core][CLI]") {
    const std::array args{std::string_view{"remote"}, std::string_view{"add"}, std::string_view{"origin"},
                          std::string_view{"https://example.invalid/repo.git"}};

    auto parsed = kProgram.Parse(Tokens(args));

    REQUIRE_FALSE(parsed.has_value());
    REQUIRE(parsed.error().kind == Sora::CLI::ParseErrorKind::MissingRequiredOption);
    REQUIRE(kProgram.schema.NameText(parsed.error().descriptorName) == "endpoint");
}

TEST_CASE("CLI requires a child command when the selected scope has subcommands", "[Sora][Core][CLI]") {
    const std::array args{std::string_view{"remote"}, std::string_view{"--endpoint"},
                          std::string_view{"upstream"}};

    auto parsed = kProgram.Parse(Tokens(args));

    REQUIRE_FALSE(parsed.has_value());
    REQUIRE(parsed.error().kind == Sora::CLI::ParseErrorKind::MissingCommand);
}

TEST_CASE("CLI reports missing values and required options structurally", "[Sora][Core][CLI]") {
    const std::array missingValue{std::string_view{"commit"}, std::string_view{"-m"}};
    auto missingValueResult = kProgram.Parse(Tokens(missingValue));
    REQUIRE_FALSE(missingValueResult.has_value());
    REQUIRE(missingValueResult.error().kind == Sora::CLI::ParseErrorKind::MissingValue);

    const std::array missingRequired{std::string_view{"commit"}};
    auto missingRequiredResult = kProgram.Parse(Tokens(missingRequired));
    REQUIRE_FALSE(missingRequiredResult.has_value());
    REQUIRE(missingRequiredResult.error().kind == Sora::CLI::ParseErrorKind::MissingRequiredOption);
}

TEST_CASE("CLI reports unknown options without formatting on the parser path", "[Sora][Core][CLI]") {
    const std::array args{std::string_view{"commit"}, std::string_view{"--message"}, std::string_view{"hello"},
                          std::string_view{"--bad"}};

    auto parsed = kProgram.Parse(Tokens(args));

    REQUIRE_FALSE(parsed.has_value());
    REQUIRE(parsed.error().kind == Sora::CLI::ParseErrorKind::UnknownOption);
    REQUIRE(kProgram.FormatError(parsed.error()) == "unknown option '--bad'");
}

TEST_CASE("CLI provides built-in help at root and nested command scopes", "[Sora][Core][CLI]") {
    const Sora::CLI::HelpRenderOptions plain{.color = Sora::CLI::HelpColorPolicy::Never};

    for (const Sora::CLI::CommandDesc& command : kProgram.schema.commands) {
        REQUIRE(command.optionCount >= 1);
        const Sora::CLI::OptionDesc& help = kProgram.schema.options[command.optionBegin];
        REQUIRE(help.kind == Sora::CLI::OptionKind::Help);
        REQUIRE(kProgram.schema.NameText(help.longName) == Sora::CLI::kHelpOptionName);
        REQUIRE(help.shortName == Sora::CLI::kHelpOptionShortName);
    }

    const std::array rootArgs{std::string_view{"--help"}};
    auto root = kProgram.Parse(Tokens(rootArgs));
    REQUIRE(root.has_value());
    REQUIRE(root->HelpRequested());
    REQUIRE(root->helpCommandId == 0);
    const std::string rootHelp = kProgram.FormatHelp(*root, plain);
    REQUIRE(rootHelp.contains("Usage: tool [options] <command>"));
    REQUIRE(rootHelp.contains("Subcommands:"));
    REQUIRE(rootHelp.contains("commit"));
    REQUIRE(rootHelp.contains("remote"));

    const std::array nestedArgs{std::string_view{"remote"}, std::string_view{"--help"}};
    auto nested = kProgram.Parse(Tokens(nestedArgs));
    REQUIRE(nested.has_value());
    REQUIRE(nested->HelpRequested());
    const std::string nestedHelp = kProgram.FormatHelp(*nested, plain);
    REQUIRE(nestedHelp.contains("Usage: tool remote [options] <command>"));
    REQUIRE(nestedHelp.contains("Global options:"));
    REQUIRE(nestedHelp.contains("--verbose"));
    REQUIRE(nestedHelp.contains("add"));
}

TEST_CASE("CLI help bypasses required validation and composes with short switches", "[Sora][Core][CLI]") {
    const std::array args{std::string_view{"commit"}, std::string_view{"-ah"}};
    auto parsed = kProgram.Parse(Tokens(args));

    REQUIRE(parsed.has_value());
    REQUIRE(parsed->HelpRequested());
    REQUIRE(parsed->CommandObject<Commit>().amend);

    const std::string help = kProgram.FormatHelp(
        *parsed, Sora::CLI::HelpRenderOptions{.color = Sora::CLI::HelpColorPolicy::Never});
    REQUIRE(help.contains("Usage: tool commit [options] [paths]..."));
    REQUIRE(help.contains("-m, --message <text>"));
    REQUIRE(help.contains("(required)"));
    REQUIRE(help.contains("Operands:"));
    REQUIRE_FALSE(help.contains("\033["));
}

TEST_CASE("CLI help uses tapioca styling only when requested or allowed", "[Sora][Core][CLI]") {
    const std::string styled = kProgram.FormatHelp(
        0, Sora::CLI::HelpRenderOptions{.color = Sora::CLI::HelpColorPolicy::Always, .width = 100});
    REQUIRE(styled.contains("\033["));

    const std::array args{std::string_view{"--help=value"}};
    auto parsed = kProgram.Parse(Tokens(args));
    REQUIRE_FALSE(parsed.has_value());
    REQUIRE(parsed.error().kind == Sora::CLI::ParseErrorKind::UnexpectedValue);
    REQUIRE(kProgram.FormatError(parsed.error()) == "unexpected value for 'help'");
}

#include "Sora/Core/CLI.h"

#include <string_view>
#include <vector>

using namespace Sora;
using namespace Sora::CLI;

struct Commit {
    [[=Parameter{.name = "message", .shortName = 'm', .valueName = "text", .required = true}]]
    std::string_view message{};

    [[=Switch{.name = "amend", .shortName = 'a'}]]
    bool amend = false;

    [[=Operand{.name = "paths", .cardinality = ValueCardinality::ZeroOrMore}]]
    std::vector<std::string_view> paths;

    [[nodiscard]] int operator()() const noexcept { return message.empty() ? 1 : 0; }
};

struct Push {
    [[=Switch{.name = "force", .shortName = 'f'}]]
    bool force = false;

    [[nodiscard]] int operator()() const noexcept { return force ? 0 : 0; }
};

struct MyProgram {
    [[=Switch{.name = "verbose", .shortName = 'v'}, =Global{}]]
    bool verbose = false;

    using Commands = Sora::CLI::Commands<Sora::CLI::Command<Commit>, Sora::CLI::Command<Push>>;

    static consteval void BuildSchema(SchemaBuilder<MyProgram>& builder) {
        builder.Name("greet").Policy(Policy::Utf8 | Policy::GnuStyle | Policy::GlobalOptionsAnywhere)
            .Command<Commit>("commit")
            .Command<Push>("push");
    }
};

inline constexpr auto program = Compile<MyProgram>();

int main(int argc, char** argv) {
    const auto parsed = program.Parse(ArgvFromMain(argc, argv));
    if (!parsed) {
        return parsed.error().ExitCode();
    }
    return program.Dispatch(*parsed);
}

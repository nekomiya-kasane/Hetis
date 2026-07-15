#include <Sora/Core/CLI.h>
#include <Sora/Platform.h>

#include <cstdint>
#include <print>
#include <string_view>
#include <vector>

namespace {

    enum class ExecutionMode : std::uint8_t {
        Safe,
        Normal,
        Aggressive,
    };

    [[nodiscard]] bool FromString(ExecutionMode& output, std::string_view text) noexcept {
        if (text == "safe") {
            output = ExecutionMode::Safe;
        } else if (text == "normal") {
            output = ExecutionMode::Normal;
        } else if (text == "aggressive") {
            output = ExecutionMode::Aggressive;
        } else {
            return false;
        }
        return true;
    }

    struct SubSub2 {
        [[= Sora::CLI::Switch{
            .name = "run", .shortName = 'r', .about = "Execute rather than only validate."}]] bool run = false;

        [[= Sora::CLI::Parameter{.name = "repeat",
                                 .shortName = 'R',
                                 .valueName = "count",
                                 .about = "Repeat each input."}]] std::uint32_t repeat = 1;

        [[= Sora::CLI::Operand{.name = "input",
                               .cardinality = Sora::CLI::ValueCardinality::OneOrMore,
                               .about = "Inputs processed by subsub2."}]] std::vector<std::string_view> inputs;

        [[nodiscard]] int operator()() const noexcept {
            std::println("linked A: sub1/subsub2 run={} repeat={} inputs={}", run, repeat, inputs.size());
            return 0;
        }
    };

    struct PipelinePlan {
        [[= Sora::CLI::Parameter{.name = "output",
                                 .shortName = 'o',
                                 .valueName = "file",
                                 .about = "Write the generated plan to a file."}]] std::string_view output =
            "plan.json";

        [[= Sora::CLI::Operand{.name = "source",
                               .cardinality = Sora::CLI::ValueCardinality::OneOrMore,
                               .about = "Plan input sources."}]] std::vector<std::string_view> sources;

        [[nodiscard]] int operator()() const noexcept {
            std::println("linked A: pipeline/plan output={} sources={}", output, sources.size());
            return 0;
        }
    };

    struct PipelineExecute {
        [[= Sora::CLI::Switch{.name = "confirm", .about = "Confirm execution of the selected plan."}]] bool confirm =
            false;

        [[= Sora::CLI::Operand{.name = "plan", .about = "Plan file to execute."}]] std::string_view plan;

        [[nodiscard]] int operator()() const noexcept {
            std::println("linked A: pipeline/execute confirm={} plan={}", confirm, plan);
            return 0;
        }
    };

    struct Pipeline {};

    struct ConfigGet {
        [[= Sora::CLI::Operand{.name = "key", .about = "Configuration key."}]] std::string_view key;

        [[nodiscard]] int operator()() const noexcept {
            std::println("linked A: config/get key={}", key);
            return 0;
        }
    };

    struct ConfigSet {
        [[= Sora::CLI::Operand{.name = "key", .about = "Configuration key."}]] std::string_view key;

        [[= Sora::CLI::Operand{.name = "value", .about = "Replacement value."}]] std::string_view value;

        [[nodiscard]] int operator()() const noexcept {
            std::println("linked A: config/set {}={}", key, value);
            return 0;
        }
    };

    struct ConfigUnset {
        [[= Sora::CLI::Operand{.name = "key",
                               .cardinality = Sora::CLI::ValueCardinality::OneOrMore,
                               .about = "Configuration keys removed together."}]] std::vector<std::string_view> keys;

        [[nodiscard]] int operator()() const noexcept {
            std::println("linked A: config/unset keys={}", keys.size());
            return 0;
        }
    };

    struct Config {};

    struct Sub1 {
        [[= Sora::CLI::Switch{.name = "verbose", .shortName = 'v', .about = "Increase sub1-local verbosity."},
          = Sora::CLI::Override{}]] std::uint32_t verbose = 0;

        [[= Sora::CLI::Parameter{.name = "workspace",
                                 .shortName = 'w',
                                 .valueName = "path",
                                 .about = "Workspace shared by every sub1 descendant.",
                                 .required = true}]] std::string_view workspace;

        [[= Sora::CLI::Parameter{.name = "mode",
                                 .shortName = 'm',
                                 .valueName = "mode",
                                 .about = "Execution mode: safe, normal, or aggressive."}]] ExecutionMode mode =
            ExecutionMode::Normal;
    };

    struct ASubprogram {
        using PipelineChildren =
            Sora::CLI::Commands<Sora::CLI::Command<PipelinePlan>, Sora::CLI::Command<PipelineExecute>>;
        using ConfigChildren = Sora::CLI::Commands<Sora::CLI::Command<ConfigGet>, Sora::CLI::Command<ConfigSet>,
                                                   Sora::CLI::Command<ConfigUnset>>;
        using Sub1Children =
            Sora::CLI::Commands<Sora::CLI::Command<SubSub2>, Sora::CLI::Command<Pipeline, PipelineChildren>,
                                Sora::CLI::Command<Config, ConfigChildren>>;
        using Commands = Sora::CLI::Commands<Sora::CLI::Command<Sub1, Sub1Children>>;

        static consteval void BuildSchema(Sora::CLI::SchemaBuilder<ASubprogram>& builder) {
            builder.Command<Sub1>("sub1", "Linked A command scope with local state and nested extension points.")
                .Command<SubSub2>("subsub2", "Linked A leaf with repeated operands.")
                .Command<Pipeline>("pipeline", "Plan or execute a linked A pipeline.")
                .Command<PipelinePlan>("plan", "Construct a pipeline plan.")
                .Command<PipelineExecute>("execute", "Execute a pipeline plan.")
                .Command<Config>("config", "Read or mutate linked A configuration.")
                .Command<ConfigGet>("get", "Read one configuration value.")
                .Command<ConfigSet>("set", "Set one configuration value.")
                .Command<ConfigUnset>("unset", "Remove one or more configuration values.");
        }
    };

    inline constexpr auto kSubprogram = Sora::CLI::CompileSubprogram<ASubprogram>();

} // namespace

extern "C" PLATFORM_EXPORT const Sora::CLI::CommandFragment* SoraCliComplexLinkedA() noexcept {
    static const auto fragment = kSubprogram.Fragment("linked-a");
    return &fragment;
}

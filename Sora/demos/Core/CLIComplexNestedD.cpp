#include <Sora/Core/CLI.h>
#include <Sora/Platform.h>

#include <cstdint>
#include <print>
#include <string_view>
#include <vector>

namespace {

    struct Inspect {
        [[= Sora::CLI::Parameter{.name = "format",
                                 .shortName = 'f',
                                 .valueName = "format",
                                 .about = "Inspection output format."}]] std::string_view format = "text";

        [[= Sora::CLI::Operand{.name = "target",
                               .about = "Target inspected by the nested plugin."}]] std::string_view target;

        [[nodiscard]] int operator()() const noexcept {
            std::println("nested D: inspect format={} target={}", format, target);
            return 0;
        }
    };

    struct Watch {
        [[= Sora::CLI::Parameter{.name = "interval",
                                 .shortName = 'i',
                                 .valueName = "milliseconds",
                                 .about = "Polling interval."}]] std::uint32_t interval = 1000;

        [[= Sora::CLI::Parameter{.name = "limit",
                                 .shortName = 'l',
                                 .valueName = "count",
                                 .about = "Maximum number of observations."}]] std::uint32_t limit = 0;

        [[= Sora::CLI::Operand{.name = "topic",
                               .cardinality = Sora::CLI::ValueCardinality::OneOrMore,
                               .about = "Topics observed together."}]] std::vector<std::string_view> topics;

        [[nodiscard]] int operator()() const noexcept {
            std::println("nested D: watch interval={} limit={} topics={}", interval, limit, topics.size());
            return 0;
        }
    };

    struct Replay {
        [[= Sora::CLI::Parameter{
            .name = "from", .valueName = "timestamp", .about = "Inclusive replay start."}]] std::string_view from =
            "begin";

        [[= Sora::CLI::Parameter{
            .name = "until", .valueName = "timestamp", .about = "Exclusive replay end."}]] std::string_view until =
            "end";

        [[= Sora::CLI::Operand{.name = "trace-file",
                               .about = "Trace file replayed by the nested plugin."}]] std::string_view traceFile;

        [[nodiscard]] int operator()() const noexcept {
            std::println("nested D: replay from={} until={} trace={}", from, until, traceFile);
            return 0;
        }
    };

    struct SubSub1 {
        [[= Sora::CLI::Switch{
            .name = "seen", .shortName = 's', .about = "Count observation passes."}]] std::uint32_t seen = 0;

        [[= Sora::CLI::Switch{.name = "strict", .about = "Reject recoverable inconsistencies."}]] bool strict = false;
    };

    struct DSubprogram {
        using Children =
            Sora::CLI::Commands<Sora::CLI::Command<Inspect>, Sora::CLI::Command<Watch>, Sora::CLI::Command<Replay>>;
        using Commands = Sora::CLI::Commands<Sora::CLI::Command<SubSub1, Children>>;

        static consteval void BuildSchema(Sora::CLI::SchemaBuilder<DSubprogram>& builder) {
            builder.Command<SubSub1>("subsub1", "Runtime D subtree mounted below linked A's sub1 scope.")
                .Command<Inspect>("inspect", "Inspect one nested target.")
                .Command<Watch>("watch", "Watch one or more nested topics.")
                .Command<Replay>("replay", "Replay one nested trace.");
        }
    };

    inline constexpr auto kModule = Sora::CLI::CompileRuntimeModule<DSubprogram>("nested-d");

} // namespace

extern "C" PLATFORM_EXPORT const Sora::CLI::RuntimeModuleDescriptor* SoraCliRuntimeModule() noexcept {
    static const auto descriptor = kModule.Descriptor();
    return &descriptor;
}

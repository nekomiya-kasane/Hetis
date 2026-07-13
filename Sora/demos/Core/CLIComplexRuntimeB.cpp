#include <Sora/Core/CLI.h>
#include <Sora/Platform.h>

#include <cstdint>
#include <print>
#include <string_view>
#include <vector>

namespace {

    struct Query {
        [[= Sora::CLI::Parameter{.name = "state",
                                 .shortName = 's',
                                 .valueName = "state",
                                 .about = "Filter by queued, running, or finished state."}]] std::string_view state =
            "running";

        [[= Sora::CLI::Operand{.name = "filter",
                               .cardinality = Sora::CLI::ValueCardinality::ZeroOrMore,
                               .about = "Additional query filters."}]] std::vector<std::string_view> filters;

        [[nodiscard]] int operator()() const noexcept {
            std::println("runtime B: query state={} filters={}", state, filters.size());
            return 0;
        }
    };

    struct Submit {
        [[= Sora::CLI::Parameter{.name = "priority",
                                 .shortName = 'P',
                                 .valueName = "level",
                                 .about = "Submission priority."}]] std::string_view priority = "normal";

        [[= Sora::CLI::Operand{.name = "job",
                               .cardinality = Sora::CLI::ValueCardinality::OneOrMore,
                               .about = "Job descriptions submitted together."}]] std::vector<std::string_view> jobs;

        [[nodiscard]] int operator()() const noexcept {
            std::println("runtime B: submit priority={} jobs={}", priority, jobs.size());
            return 0;
        }
    };

    struct Cancel {
        [[= Sora::CLI::Switch{.name = "force", .shortName = 'f', .about = "Force cancellation."}]] bool force = false;

        [[= Sora::CLI::Operand{.name = "job-id",
                               .cardinality = Sora::CLI::ValueCardinality::OneOrMore,
                               .about = "Job identifiers to cancel."}]] std::vector<std::string_view> jobIds;

        [[nodiscard]] int operator()() const noexcept {
            std::println("runtime B: cancel force={} jobs={}", force, jobIds.size());
            return 0;
        }
    };

    struct Logs {
        [[= Sora::CLI::Switch{
            .name = "follow", .shortName = 'f', .about = "Follow appended log records."}]] bool follow = false;

        [[= Sora::CLI::Parameter{.name = "lines",
                                 .shortName = 'n',
                                 .valueName = "count",
                                 .about = "Number of trailing records."}]] std::uint32_t lines = 100;

        [[= Sora::CLI::Operand{.name = "job-id", .about = "Job whose logs are requested."}]] std::string_view jobId;

        [[nodiscard]] int operator()() const noexcept {
            std::println("runtime B: logs follow={} lines={} job={}", follow, lines, jobId);
            return 0;
        }
    };

    struct Sub2 {
        [[= Sora::CLI::Parameter{.name = "endpoint",
                                 .shortName = 'e',
                                 .valueName = "uri",
                                 .about = "Remote scheduler endpoint.",
                                 .required = true}]] std::string_view endpoint;

        [[= Sora::CLI::Parameter{
            .name = "timeout", .valueName = "seconds", .about = "Remote operation timeout."}]] std::uint32_t timeout =
            30;
    };

    struct BSubprogram {
        using Children = Sora::CLI::Commands<Sora::CLI::Command<Query>, Sora::CLI::Command<Submit>,
                                             Sora::CLI::Command<Cancel>, Sora::CLI::Command<Logs>>;
        using Commands = Sora::CLI::Commands<Sora::CLI::Command<Sub2, Children>>;

        static consteval void BuildSchema(Sora::CLI::SchemaBuilder<BSubprogram>& builder) {
            builder.Command<Sub2>("sub2", "Explicitly loaded runtime scheduler commands.")
                .Command<Query>("query", "Query remote jobs.")
                .Command<Submit>("submit", "Submit one or more jobs.")
                .Command<Cancel>("cancel", "Cancel remote jobs.")
                .Command<Logs>("logs", "Read remote job logs.");
        }
    };

    inline constexpr auto kModule = Sora::CLI::CompileRuntimeModule<BSubprogram>("runtime-b");

} // namespace

extern "C" PLATFORM_EXPORT const Sora::CLI::RuntimeModuleDescriptor* SoraCliRuntimeModule() noexcept {
    static const auto descriptor = kModule.Descriptor();
    return &descriptor;
}

#include <Sora/Core/CLI.h>
#include <Sora/Platform.h>

#include <cstdint>
#include <print>
#include <string_view>
#include <vector>

namespace {

    struct IndexAdd {
        [[= Sora::CLI::Switch{ .name = "replace", .shortName = 'r', .about = "Replace existing index entries."}]] 
        bool replace = false;

        [[= Sora::CLI::Operand{.name = "resource",
                               .cardinality = Sora::CLI::ValueCardinality::OneOrMore,
                               .about = "Resources added to the index."}]] 
        std::vector<std::string_view> resources;

        [[nodiscard]] int operator()() const noexcept {
            std::println("discovered C: index/add replace={} resources={}", replace, resources.size());
            return 0;
        }
    };

    struct IndexRemove {
        [[= Sora::CLI::Operand{.name = "resource",
                               .cardinality = Sora::CLI::ValueCardinality::OneOrMore,
                               .about = "Resources removed from the index."}]] 
        std::vector<std::string_view> resources;

        [[nodiscard]] int operator()() const noexcept {
            std::println("discovered C: index/remove resources={}", resources.size());
            return 0;
        }
    };

    struct Index {};

    struct Search {
        [[= Sora::CLI::Parameter{.name = "limit", .shortName = 'l', .valueName = "count", 
                                 .about = "Maximum result count."}]] 
        std::uint32_t limit = 20;

        [[= Sora::CLI::Parameter{.name = "format", .shortName = 'f', .valueName = "format", .about = "Output format."}]]
        std::string_view format = "text";

        [[= Sora::CLI::Operand{.name = "query", .about = "Search expression."}]] 
        std::string_view query;

        [[nodiscard]] int operator()() const noexcept {
            std::println("discovered C: search limit={} format={} query={}", limit, format, query);
            return 0;
        }
    };

    struct Compact {
        [[= Sora::CLI::Parameter{.name = "target-size",
                                 .valueName = "bytes",
                                 .about = "Requested compacted shard size."}]] std::uint64_t targetSize = 1u << 20;

        [[nodiscard]] int operator()() const noexcept {
            std::println("discovered C: maintenance/compact target-size={}", targetSize);
            return 0;
        }
    };

    struct Verify {
        [[= Sora::CLI::Switch{.name = "repair", .shortName = 'r', .about = "Repair recoverable index damage."}]] 
        bool repair = false;

        [[= Sora::CLI::Operand{.name = "shard",
                               .cardinality = Sora::CLI::ValueCardinality::ZeroOrMore,
                               .about = "Specific shards to verify."}]] 
        std::vector<std::string_view> shards;

        [[nodiscard]] int operator()() const noexcept {
            std::println("discovered C: maintenance/verify repair={} shards={}", repair, shards.size());
            return 0;
        }
    };

    struct Maintenance {};

    struct Sub3 {
        [[= Sora::CLI::Parameter{.name = "namespace",
                                 .shortName = 'N',
                                 .valueName = "name",
                                 .about = "Logical index namespace."}]] 
        std::string_view nameSpace = "default";
    };

    struct CSubprogram {
        using IndexChildren = Sora::CLI::Commands<Sora::CLI::Command<IndexAdd>, Sora::CLI::Command<IndexRemove>>;
        using MaintenanceChildren = Sora::CLI::Commands<Sora::CLI::Command<Compact>, Sora::CLI::Command<Verify>>;
        using Children = Sora::CLI::Commands<Sora::CLI::Command<Index, IndexChildren>, Sora::CLI::Command<Search>,
                                             Sora::CLI::Command<Maintenance, MaintenanceChildren>>;
        using Commands = Sora::CLI::Commands<Sora::CLI::Command<Sub3, Children>>;

        static consteval void BuildSchema(Sora::CLI::SchemaBuilder<CSubprogram>& builder) {
            builder.Command<Sub3>("sub3", "Manifest-discovered runtime index commands.")
                .Command<Index>("index", "Mutate resource index entries.")
                .Command<IndexAdd>("add", "Add resources to the index.")
                .Command<IndexRemove>("remove", "Remove resources from the index.")
                .Command<Search>("search", "Search indexed resources.")
                .Command<Maintenance>("maintenance", "Maintain index storage.")
                .Command<Compact>("compact", "Compact index shards.")
                .Command<Verify>("verify", "Verify index shard integrity.");
        }
    };

    inline constexpr auto kModule = Sora::CLI::CompileRuntimeModule<CSubprogram>("discovered-c");

} // namespace

extern "C" PLATFORM_EXPORT const Sora::CLI::RuntimeModuleDescriptor* SoraCliRuntimeModule() noexcept {
    static const auto descriptor = kModule.Descriptor();
    return &descriptor;
}

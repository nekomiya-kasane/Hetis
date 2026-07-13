#include "Core/CLIModuleDemo/LinkedModules.h"

#include <Sora/Core/CLI.h>
#include <Sora/Core/PAL/Module.h>

#include <array>
#include <expected>
#include <memory>
#include <print>
#include <string>
#include <string_view>

#ifndef SORA_CLI_PLUGIN_GAMMA
#    error "SORA_CLI_PLUGIN_GAMMA must name the generated runtime plugin file"
#endif

#ifndef SORA_CLI_PLUGIN_DELTA
#    error "SORA_CLI_PLUGIN_DELTA must name the generated runtime plugin file"
#endif

namespace {

    struct HostCommand {
        [[=Sora::CLI::Switch{.name = "details", .shortName = 'd', .about = "Show the executable command details."}]]
        bool details = false;

        [[nodiscard]] int operator()() const noexcept {
            std::println("executable: host{}", details ? " with details" : "");
            return 0;
        }
    };

    struct HostProgram {
        using Commands = Sora::CLI::Commands<Sora::CLI::Command<HostCommand>>;

        static consteval void BuildSchema(Sora::CLI::SchemaBuilder<HostProgram>& builder) {
            builder.Name("sora-cli-modules")
                .Command<HostCommand>("host", "Command defined directly by the executable.");
        }
    };

    inline constexpr auto kHostProgram = Sora::CLI::Compile<HostProgram>();

    [[nodiscard]] auto LoadRuntimeFragment(std::string_view moduleName)
        -> std::expected<Sora::CLI::FragmentRegistration, std::string> {
        const std::array names{moduleName};
        Sora::PAL::ModuleLoadOptions options{
            .nameKind = Sora::PAL::ModuleNameKind::FileName,
            .candidatePolicy = Sora::PAL::ModuleCandidatePolicy::ExactOnly,
            .bindMode = Sora::PAL::ModuleBindMode::Now,
            .visibility = Sora::PAL::ModuleVisibility::Local,
            .cachePolicy = Sora::PAL::ModuleCachePolicy::Private,
        };
        auto loaded = Sora::PAL::LoadModule(names, options);
        if (!loaded) {
            return std::unexpected(std::format("failed to load runtime CLI module '{}'", moduleName));
        }

        const Sora::PAL::ModulePtr& module = *loaded;
        auto* entry = module->TryFindFunction<Sora::CLI::RuntimeModuleEntry>(Sora::CLI::kRuntimeModuleEntryName);
        if (entry == nullptr) {
            return std::unexpected(std::format("runtime CLI module '{}' has no '{}' export", moduleName,
                                               Sora::CLI::kRuntimeModuleEntryName));
        }

        std::shared_ptr<const void> owner{module, module.get()};
        auto imported = Sora::CLI::ImportRuntimeModule(entry(), std::move(owner));
        if (!imported) {
            return std::unexpected(imported.error().Message());
        }
        return std::move(*imported);
    }

} // namespace

int main(int argc, char** argv) {
    Sora::CLI::FragmentRegistry registry;
    registry.Add(*SoraCliLinkedAlpha());
    registry.Add(*SoraCliLinkedBeta());

    auto gamma = LoadRuntimeFragment(SORA_CLI_PLUGIN_GAMMA);
    auto delta = LoadRuntimeFragment(SORA_CLI_PLUGIN_DELTA);
    if (!gamma || !delta) {
        std::println(stderr, "{}", !gamma ? gamma.error() : delta.error());
        return 70;
    }
    registry.Add(std::move(*gamma));
    registry.Add(std::move(*delta));

    const auto snapshot = registry.Snapshot();
    auto linked = Sora::CLI::LinkAtStartup(kHostProgram, snapshot, "executable");
    if (!linked) {
        std::println(stderr, "{}", linked.error().Message());
        return linked.error().ExitCode();
    }

    if (argc == 1) {
        linked->PrintHelp();
        return 0;
    }

    auto result = linked->Run(Sora::CLI::ArgvFromMain(argc, argv));
    if (!result) {
        std::println(stderr, "{}", result.error().Message());
        return result.error().ExitCode();
    }
    return *result;
}

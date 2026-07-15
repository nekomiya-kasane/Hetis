#include "Core/CLIComplexModuleDemo/LinkedModules.h"

#include <Sora/Core/CLI.h>
#include <Sora/Core/PAL/Module.h>

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <print>
#include <string>
#include <string_view>

#ifndef SORA_CLI_COMPLEX_RUNTIME_B
#    error "SORA_CLI_COMPLEX_RUNTIME_B must name the explicit runtime module"
#endif

#ifndef SORA_CLI_COMPLEX_MANIFEST
#    error "SORA_CLI_COMPLEX_MANIFEST must name the discovered runtime module manifest"
#endif

namespace {

    enum class ColorMode : std::uint8_t {
        Auto,
        Always,
        Never,
    };

    [[nodiscard]] bool FromString(ColorMode& output, std::string_view text) noexcept {
        if (text == "auto") {
            output = ColorMode::Auto;
        } else if (text == "always") {
            output = ColorMode::Always;
        } else if (text == "never") {
            output = ColorMode::Never;
        } else {
            return false;
        }
        return true;
    }

    struct Status {
        [[= Sora::CLI::Switch{
            .name = "details", .shortName = 'd', .about = "Show linked graph details."}]] bool details = false;

        [[nodiscard]] int operator()() const noexcept {
            std::println("host status: ready{}", details ? ", graph=sealed" : "");
            return 0;
        }
    };

    struct SelfTest {
        [[= Sora::CLI::Parameter{.name = "focus",
                                 .shortName = 'f',
                                 .valueName = "area",
                                 .about = "Restrict validation to one subsystem."}]] std::string_view focus = "all";

        [[nodiscard]] int operator()() const noexcept {
            std::println("host self-test: focus={}", focus);
            return 0;
        }
    };

    struct HostProgram {
        [[= Sora::CLI::Switch{.name = "verbose", .shortName = 'v', .about = "Increase host verbosity."},
          = Sora::CLI::Global{}]] std::uint32_t verbose = 0;

        [[= Sora::CLI::Parameter{.name = "color", .valueName = "mode", .about = "Color mode: auto, always, or never."},
          = Sora::CLI::Global{}]] ColorMode color = ColorMode::Auto;

        [[= Sora::CLI::Parameter{
              .name = "profile", .shortName = 'p', .valueName = "name", .about = "Select an execution profile."},
          = Sora::CLI::Global{}]] std::string_view profile = "default";

        [[= Sora::CLI::Parameter{
              .name = "jobs", .shortName = 'j', .valueName = "count", .about = "Set host concurrency."},
          = Sora::CLI::Global{}]] std::uint32_t jobs = 1;

        [[= Sora::CLI::Switch{.name = "dry-run", .about = "Suppress external side effects."},
          = Sora::CLI::Global{}]] bool dryRun = false;

        [[= Sora::CLI::Switch{.name = "test", .shortName = 't', .about = "Enable diagnostic validation mode."},
          = Sora::CLI::Global{}]] bool test = false;

        using Commands = Sora::CLI::Commands<Sora::CLI::Command<Status>, Sora::CLI::Command<SelfTest>>;

        static consteval void BuildSchema(Sora::CLI::SchemaBuilder<HostProgram>& builder) {
            builder.Name("sora-cli-complex")
                .Policy(Sora::CLI::Policy::GlobalOptionsAnywhere)
                .Command<Status>("status", "Inspect the executable-owned command graph state.")
                .Command<SelfTest>("self-test", "Run executable-owned CLI validation.");
        }
    };

    inline constexpr auto kHostProgram = Sora::CLI::Compile<HostProgram>();

    [[nodiscard]] std::filesystem::path ExecutableDirectory(const char* executable) {
        std::error_code error;
        if (executable != nullptr && *executable != '\0') {
            std::filesystem::path candidate = std::filesystem::absolute(executable, error);
            if (!error && std::filesystem::exists(candidate, error)) {
                const std::filesystem::path canonical = std::filesystem::weakly_canonical(candidate, error);
                return (error ? candidate : canonical).parent_path();
            }
        }
        const std::filesystem::path current = std::filesystem::current_path(error);
        return error ? std::filesystem::path{"."} : current;
    }

    [[nodiscard]] auto LoadRuntimeFragment(std::string_view moduleName,
                                           const std::filesystem::path& executableDirectory)
        -> std::expected<Sora::CLI::FragmentRegistration, std::string> {
        const std::array names{moduleName};
        const std::array searchPaths{executableDirectory};
        const Sora::PAL::ModuleLoadOptions options{
            .nameKind = Sora::PAL::ModuleNameKind::FileName,
            .candidatePolicy = Sora::PAL::ModuleCandidatePolicy::ExactOnly,
            .bindMode = Sora::PAL::ModuleBindMode::Now,
            .visibility = Sora::PAL::ModuleVisibility::Local,
            .cachePolicy = Sora::PAL::ModuleCachePolicy::Private,
            .searchPaths = searchPaths,
        };
        auto loaded = Sora::PAL::LoadModule(names, options);
        if (!loaded) {
            return std::unexpected(loaded.error().Message());
        }

        Sora::PAL::ModulePtr module = *loaded;
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

    [[nodiscard]] auto AddDiscoveredModules(Sora::CLI::FragmentRegistry& registry,
                                            const std::filesystem::path& executableDirectory)
        -> std::expected<void, std::string> {
        const std::filesystem::path manifestPath = executableDirectory / SORA_CLI_COMPLEX_MANIFEST;
        std::ifstream manifest{manifestPath};
        if (!manifest) {
            return std::unexpected(std::format("failed to open CLI module manifest '{}'", manifestPath.string()));
        }

        std::string line;
        while (std::getline(manifest, line)) {
            if (line.empty() || line.starts_with('#')) {
                continue;
            }
            const std::size_t separator = line.find('|');
            if (separator == std::string::npos || separator == 0) {
                return std::unexpected("CLI module manifest contains a malformed entry: " + line);
            }
            const std::string_view moduleName{line.data(), separator};
            auto registration = LoadRuntimeFragment(moduleName, executableDirectory);
            if (!registration) {
                return std::unexpected(std::move(registration.error()));
            }
            if (separator != std::string::npos) {
                std::string_view mount{line.data() + separator + 1, line.size() - separator - 1};
                while (!mount.empty()) {
                    const std::size_t slash = mount.find('/');
                    registration->mountPath.emplace_back(mount.substr(0, slash));
                    mount = slash == std::string_view::npos ? std::string_view{} : mount.substr(slash + 1);
                }
            }
            registry.Add(std::move(*registration));
        }
        return {};
    }

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path executableDirectory = ExecutableDirectory(argc == 0 ? nullptr : argv[0]);
    Sora::CLI::FragmentRegistry registry;
    registry.Add(*SoraCliComplexLinkedA());

    auto runtimeB = LoadRuntimeFragment(SORA_CLI_COMPLEX_RUNTIME_B, executableDirectory);
    if (!runtimeB) {
        std::println(stderr, "{}", runtimeB.error());
        return 70;
    }
    registry.Add(std::move(*runtimeB));

    if (auto discovered = AddDiscoveredModules(registry, executableDirectory); !discovered) {
        std::println(stderr, "{}", discovered.error());
        return 70;
    }

    auto linked = Sora::CLI::LinkAtStartup(kHostProgram, registry.Snapshot(), "executable");
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

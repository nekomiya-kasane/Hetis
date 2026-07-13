#include <Sora/Core/CLI.h>
#include <Sora/Platform.h>

#include <print>
#include <string_view>

namespace {

    struct PluginGamma {
        [[=Sora::CLI::Operand{.name = "name", .about = "Name reported by the gamma runtime plugin."}]]
        std::string_view name{};

        [[nodiscard]] int operator()() const noexcept {
            std::println("plugin-gamma DLL: name={}", name);
            return 0;
        }
    };

    struct GammaSubprogram {
        using Commands = Sora::CLI::Commands<Sora::CLI::Command<PluginGamma>>;

        static consteval void BuildSchema(Sora::CLI::SchemaBuilder<GammaSubprogram>& builder) {
            builder.Command<PluginGamma>("plugin-gamma", "Command imported through the runtime module ABI.");
        }
    };

    inline constexpr auto kModule = Sora::CLI::CompileRuntimeModule<GammaSubprogram>("plugin-gamma");

} // namespace

extern "C" PLATFORM_EXPORT const Sora::CLI::RuntimeModuleDescriptor* SoraCliRuntimeModule() noexcept {
    static const auto descriptor = kModule.Descriptor();
    return &descriptor;
}

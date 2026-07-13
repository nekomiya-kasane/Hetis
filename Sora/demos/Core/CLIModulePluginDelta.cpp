#include <Sora/Core/CLI.h>
#include <Sora/Platform.h>

#include <print>

namespace {

    struct PluginDelta {
        [[=Sora::CLI::Parameter{.name = "repeat", .shortName = 'r', .valueName = "count",
                               .about = "Number reported by the delta runtime plugin."}]]
        int repeat = 1;

        [[nodiscard]] int operator()() const noexcept {
            std::println("plugin-delta DLL: repeat={}", repeat);
            return 0;
        }
    };

    struct DeltaSubprogram {
        using Commands = Sora::CLI::Commands<Sora::CLI::Command<PluginDelta>>;

        static consteval void BuildSchema(Sora::CLI::SchemaBuilder<DeltaSubprogram>& builder) {
            builder.Command<PluginDelta>("plugin-delta", "Command imported from the second runtime plugin DLL.");
        }
    };

    inline constexpr auto kModule = Sora::CLI::CompileRuntimeModule<DeltaSubprogram>("plugin-delta");

} // namespace

extern "C" PLATFORM_EXPORT const Sora::CLI::RuntimeModuleDescriptor* SoraCliRuntimeModule() noexcept {
    static const auto descriptor = kModule.Descriptor();
    return &descriptor;
}

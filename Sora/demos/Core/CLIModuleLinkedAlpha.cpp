#include <Sora/Core/CLI.h>
#include <Sora/Platform.h>

#include <print>

namespace {

    struct LinkedAlpha {
        [[=Sora::CLI::Parameter{.name = "value", .shortName = 'v', .valueName = "integer",
                               .about = "Integer reported by the linked alpha module."}]]
        int value = 1;

        [[nodiscard]] int operator()() const noexcept {
            std::println("linked-alpha DLL: value={}", value);
            return 0;
        }
    };

    struct AlphaSubprogram {
        using Commands = Sora::CLI::Commands<Sora::CLI::Command<LinkedAlpha>>;

        static consteval void BuildSchema(Sora::CLI::SchemaBuilder<AlphaSubprogram>& builder) {
            builder.Policy(Sora::CLI::Policy::GnuStyle)
                .Command<LinkedAlpha>("linked-alpha", "Command injected by a link-time DLL dependency.");
        }
    };

    inline constexpr auto kSubprogram = Sora::CLI::CompileSubprogram<AlphaSubprogram>();

} // namespace

extern "C" PLATFORM_EXPORT const Sora::CLI::CommandFragment* SoraCliLinkedAlpha() noexcept {
    static const auto fragment = kSubprogram.Fragment("linked-alpha");
    return &fragment;
}

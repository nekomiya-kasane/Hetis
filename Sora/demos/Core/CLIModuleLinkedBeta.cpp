#include <Sora/Core/CLI.h>
#include <Sora/Platform.h>

#include <print>

namespace {

    struct LinkedBeta {
        [[=Sora::CLI::Switch{.name = "loud", .shortName = 'l', .about = "Use the loud beta response."}]]
        bool loud = false;

        [[nodiscard]] int operator()() const noexcept {
            std::println("linked-beta DLL: {}", loud ? "LOUD" : "quiet");
            return 0;
        }
    };

    struct BetaSubprogram {
        using Commands = Sora::CLI::Commands<Sora::CLI::Command<LinkedBeta>>;

        static consteval void BuildSchema(Sora::CLI::SchemaBuilder<BetaSubprogram>& builder) {
            builder.Command<LinkedBeta>("linked-beta", "Command injected by the second link-time DLL dependency.");
        }
    };

    inline constexpr auto kSubprogram = Sora::CLI::CompileSubprogram<BetaSubprogram>();

} // namespace

extern "C" PLATFORM_EXPORT const Sora::CLI::CommandFragment* SoraCliLinkedBeta() noexcept {
    static const auto fragment = kSubprogram.Fragment("linked-beta");
    return &fragment;
}

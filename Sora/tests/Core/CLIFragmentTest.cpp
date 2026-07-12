#include <Sora/Core/CLI.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <string_view>

namespace {

    struct HostCommand {
        [[nodiscard]] int operator()() const noexcept { return 11; }
    };

    struct HostProgram {
        using Commands = Sora::CLI::Commands<Sora::CLI::Command<HostCommand>>;

        static consteval void BuildSchema(Sora::CLI::SchemaBuilder<HostProgram>& builder) {
            builder.Name("module-test").Command<HostCommand>("host", "Host command.");
        }
    };

    struct StartupCommand {
        [[=Sora::CLI::Parameter{.name = "value", .required = true}]]
        int value = 0;

        [[nodiscard]] int operator()() const noexcept { return value; }
    };

    struct StartupSubprogram {
        using Commands = Sora::CLI::Commands<Sora::CLI::Command<StartupCommand>>;

        static consteval void BuildSchema(Sora::CLI::SchemaBuilder<StartupSubprogram>& builder) {
            builder.Command<StartupCommand>("startup", "Startup fragment command.");
        }
    };

    struct RuntimeCommand {
        [[=Sora::CLI::Operand{.name = "value"}]]
        int value = 0;

        [[nodiscard]] int operator()() const noexcept { return value; }
    };

    struct RuntimeSubprogram {
        using Commands = Sora::CLI::Commands<Sora::CLI::Command<RuntimeCommand>>;

        static consteval void BuildSchema(Sora::CLI::SchemaBuilder<RuntimeSubprogram>& builder) {
            builder.Command<RuntimeCommand>("runtime", "Runtime module command.");
        }
    };

    inline constexpr auto kHost = Sora::CLI::Compile<HostProgram>();
    inline constexpr auto kStartup = Sora::CLI::CompileSubprogram<StartupSubprogram>();
    inline constexpr auto kRuntime = Sora::CLI::CompileRuntimeModule<RuntimeSubprogram>("runtime-test");

    [[nodiscard]] Sora::CLI::ArgvView Tokens(std::span<const std::string_view> tokens) noexcept {
        return {.tokens = tokens};
    }

    [[nodiscard]] Sora::CLI::RuntimeInvocationResult InvalidRuntimeStatus(
        const void*, const Sora::CLI::RuntimeArgvView*) noexcept {
        return {.status = static_cast<Sora::CLI::RuntimeInvocationStatus>(99), .exitCode = 0};
    }

} // namespace

TEST_CASE("CLI startup link combines executable, linked, and runtime module commands", "[Sora][Core][CLI]") {
    static const auto runtimeDescriptor = kRuntime.Descriptor();
    auto runtime = Sora::CLI::ImportRuntimeModule(&runtimeDescriptor, std::make_shared<int>(0));
    REQUIRE(runtime.has_value());

    Sora::CLI::FragmentRegistry registry;
    registry.Add(kStartup.Fragment("startup-test"));
    registry.Add(std::move(*runtime));

    const auto snapshot = registry.Snapshot();
    auto linked = Sora::CLI::LinkAtStartup(kHost, snapshot, "test-executable");
    REQUIRE(linked.has_value());
    REQUIRE(linked->CommandCount() == 3);
    REQUIRE(linked->ContainsCommand("host"));
    REQUIRE(linked->ContainsCommand("startup"));
    REQUIRE(linked->ContainsCommand("runtime"));

    const std::array hostArgs{std::string_view{"host"}};
    const std::array startupArgs{std::string_view{"startup"}, std::string_view{"--value"}, std::string_view{"21"}};
    const std::array runtimeArgs{std::string_view{"runtime"}, std::string_view{"31"}};

    REQUIRE(linked->Run(Tokens(hostArgs)).value() == 11);
    REQUIRE(linked->Run(Tokens(startupArgs)).value() == 21);
    REQUIRE(linked->Run(Tokens(runtimeArgs)).value() == 31);
    REQUIRE(linked->FormatHelp().contains("[startup:startup-test]"));
    REQUIRE(linked->FormatHelp().contains("[runtime:runtime-test]"));
}

TEST_CASE("CLI startup link rejects duplicate commands before parsing", "[Sora][Core][CLI]") {
    Sora::CLI::FragmentRegistry registry;
    registry.Add(kStartup.Fragment("first"));
    registry.Add(kStartup.Fragment("second"));

    const auto snapshot = registry.Snapshot();
    auto linked = Sora::CLI::LinkAtStartup(kHost, snapshot);
    REQUIRE_FALSE(linked.has_value());
    REQUIRE(linked.error().kind == Sora::CLI::LinkErrorKind::DuplicateCommand);
    REQUIRE(linked.error().command == "startup");
}

TEST_CASE("CLI runtime import validates its self-describing ABI header", "[Sora][Core][CLI]") {
    auto descriptor = kRuntime.Descriptor();
    descriptor.header.magic = 0;

    auto imported = Sora::CLI::ImportRuntimeModule(&descriptor, std::make_shared<int>(0));
    REQUIRE_FALSE(imported.has_value());
    REQUIRE(imported.error().kind == Sora::CLI::RuntimeAbiErrorKind::InvalidMagic);

    descriptor = kRuntime.Descriptor();
    imported = Sora::CLI::ImportRuntimeModule(&descriptor, {});
    REQUIRE_FALSE(imported.has_value());
    REQUIRE(imported.error().kind == Sora::CLI::RuntimeAbiErrorKind::MissingOwner);
}

TEST_CASE("CLI runtime invocation rejects status values outside the ABI enum", "[Sora][Core][CLI]") {
    static auto descriptor = [] {
        auto value = kRuntime.Descriptor();
        value.invoke = &InvalidRuntimeStatus;
        return value;
    }();

    auto runtime = Sora::CLI::ImportRuntimeModule(&descriptor, std::make_shared<int>(0));
    REQUIRE(runtime.has_value());

    Sora::CLI::FragmentRegistry registry;
    registry.Add(std::move(*runtime));
    const auto snapshot = registry.Snapshot();
    auto linked = Sora::CLI::LinkAtStartup(kHost, snapshot);
    REQUIRE(linked.has_value());

    const std::array args{std::string_view{"runtime"}, std::string_view{"5"}};
    auto result = linked->Run(Tokens(args));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == Sora::CLI::LinkedProgramErrorKind::InvocationFailed);
}

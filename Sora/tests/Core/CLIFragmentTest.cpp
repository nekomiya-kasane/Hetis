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

    struct InvalidHostProgram {
        [[= Sora::CLI::Switch{.name = "verbose"}]] bool verbose = false;

        using Commands = Sora::CLI::Commands<Sora::CLI::Command<HostCommand>>;

        static consteval void BuildSchema(Sora::CLI::SchemaBuilder<InvalidHostProgram>& builder) {
            builder.Name("root-option-test").Command<HostCommand>("host", "Host command.");
        }
    };

    struct StartupCommand {
        [[= Sora::CLI::Parameter{.name = "value", .required = true}]] int value = 0;

        [[nodiscard]] int operator()() const noexcept { return value; }
    };

    struct StartupSubprogram {
        using Commands = Sora::CLI::Commands<Sora::CLI::Command<StartupCommand>>;

        static consteval void BuildSchema(Sora::CLI::SchemaBuilder<StartupSubprogram>& builder) {
            builder.Command<StartupCommand>("startup", "Startup fragment command.");
        }
    };

    struct RuntimeCommand {
        [[= Sora::CLI::Operand{.name = "value"}]] int value = 0;

        [[nodiscard]] int operator()() const noexcept { return value; }
    };

    struct RuntimeSubprogram {
        using Commands = Sora::CLI::Commands<Sora::CLI::Command<RuntimeCommand>>;

        static consteval void BuildSchema(Sora::CLI::SchemaBuilder<RuntimeSubprogram>& builder) {
            builder.Command<RuntimeCommand>("runtime", "Runtime module command.");
        }
    };

    struct NestedLeaf {
        [[= Sora::CLI::Operand{.name = "value"}]] int value = 0;

        [[nodiscard]] int operator()() const noexcept { return value; }
    };

    struct NestedSubprogram {
        using Commands = Sora::CLI::Commands<Sora::CLI::Command<NestedLeaf>>;

        static consteval void BuildSchema(Sora::CLI::SchemaBuilder<NestedSubprogram>& builder) {
            builder.Command<NestedLeaf>("nested", "Nested extension command.");
        }
    };

    struct ParentLocalLeaf {
        [[nodiscard]] int operator()() const noexcept { return 41; }
    };

    struct ParentCommand {
        [[= Sora::CLI::Switch{.name = "verbose", .shortName = 'v'}, = Sora::CLI::Override{}]] std::uint32_t verbose = 0;

        [[= Sora::CLI::Parameter{.name = "count", .required = true}]] int count = 0;
    };

    struct ParentSubprogram {
        using Children = Sora::CLI::Commands<Sora::CLI::Command<ParentLocalLeaf>>;
        using Commands = Sora::CLI::Commands<Sora::CLI::Command<ParentCommand, Children>>;

        static consteval void BuildSchema(Sora::CLI::SchemaBuilder<ParentSubprogram>& builder) {
            builder.Command<ParentCommand>("parent", "Parent fragment scope.")
                .Command<ParentLocalLeaf>("local", "Parent-local leaf.");
        }
    };

    struct CollidingCommand {
        [[= Sora::CLI::Switch{.name = "verbose", .shortName = 'v'}]] std::uint32_t verbose = 0;

        [[nodiscard]] int operator()() const noexcept { return 0; }
    };

    struct CollidingSubprogram {
        using Commands = Sora::CLI::Commands<Sora::CLI::Command<CollidingCommand>>;

        static consteval void BuildSchema(Sora::CLI::SchemaBuilder<CollidingSubprogram>& builder) {
            builder.Command<CollidingCommand>("collision", "Unmarked global-option collision.");
        }
    };

    struct GraphHostProgram {
        [[= Sora::CLI::Switch{.name = "verbose", .shortName = 'v'}, = Sora::CLI::Global{}]] std::uint32_t verbose = 0;

        using Commands = Sora::CLI::Commands<Sora::CLI::Command<HostCommand>>;

        static consteval void BuildSchema(Sora::CLI::SchemaBuilder<GraphHostProgram>& builder) {
            builder.Name("graph-test")
                .Policy(Sora::CLI::Policy::GlobalOptionsAnywhere)
                .Command<HostCommand>("host", "Host command.");
        }
    };

    inline constexpr auto kHost = Sora::CLI::Compile<HostProgram>();
    inline constexpr auto kInvalidHost = Sora::CLI::Compile<InvalidHostProgram>();
    inline constexpr auto kStartup = Sora::CLI::CompileSubprogram<StartupSubprogram>();
    inline constexpr auto kRuntime = Sora::CLI::CompileRuntimeModule<RuntimeSubprogram>("runtime-test");
    inline constexpr auto kNested = Sora::CLI::CompileSubprogram<NestedSubprogram>();
    inline constexpr auto kParent = Sora::CLI::CompileSubprogram<ParentSubprogram>();
    inline constexpr auto kColliding = Sora::CLI::CompileSubprogram<CollidingSubprogram>();
    inline constexpr auto kGraphHost = Sora::CLI::Compile<GraphHostProgram>();

    [[nodiscard]] Sora::CLI::ArgvView Tokens(std::span<const std::string_view> tokens) noexcept {
        return {.tokens = tokens};
    }

    [[nodiscard]] Sora::CLI::RuntimeInvocationResult InvalidRuntimeStatus(const void*,
                                                                          const Sora::CLI::RuntimeArgvView*) noexcept {
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

TEST_CASE("CLI startup link mounts fragments at arbitrary command scopes", "[Sora][Core][CLI]") {
    Sora::CLI::FragmentRegistry registry;
    registry.Add(kNested.Fragment("nested-provider"), {"parent"});
    registry.Add(kParent.Fragment("parent-provider"));

    auto linked = Sora::CLI::LinkAtStartup(kGraphHost, registry.Snapshot(), "test-host");
    REQUIRE(linked.has_value());
    REQUIRE(linked->ContainsCommand("parent nested"));

    const std::array args{std::string_view{"-vv"},     std::string_view{"parent"}, std::string_view{"-v"},
                          std::string_view{"--count"}, std::string_view{"3"},      std::string_view{"nested"},
                          std::string_view{"19"}};
    REQUIRE(linked->Run(Tokens(args)).value() == 19);
}

TEST_CASE("CLI linked recognition validates ancestor values across provider boundaries", "[Sora][Core][CLI]") {
    Sora::CLI::FragmentRegistry registry;
    registry.Add(kParent.Fragment("parent-provider"));
    registry.Add(kNested.Fragment("nested-provider"), {"parent"});
    auto linked = Sora::CLI::LinkAtStartup(kGraphHost, registry.Snapshot());
    REQUIRE(linked.has_value());

    const std::array args{std::string_view{"parent"}, std::string_view{"--count"}, std::string_view{"invalid"},
                          std::string_view{"nested"}, std::string_view{"19"}};
    auto result = linked->Run(Tokens(args));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().Message() == "invalid value 'invalid' for 'count'");

    const std::array missing{std::string_view{"parent"}, std::string_view{"nested"}, std::string_view{"19"}};
    result = linked->Run(Tokens(missing));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == Sora::CLI::LinkedProgramErrorKind::MissingRequiredOption);
    REQUIRE(result.error().Message() == "missing required option 'count'");
}

TEST_CASE("CLI startup link rejects unresolved mount paths", "[Sora][Core][CLI]") {
    Sora::CLI::FragmentRegistry registry;
    registry.Add(kNested.Fragment("nested-provider"), {"missing"});

    auto linked = Sora::CLI::LinkAtStartup(kGraphHost, registry.Snapshot());
    REQUIRE_FALSE(linked.has_value());
    REQUIRE(linked.error().kind == Sora::CLI::LinkErrorKind::InvalidMount);
}

TEST_CASE("CLI startup link requires explicit local overrides of root globals", "[Sora][Core][CLI]") {
    Sora::CLI::FragmentRegistry registry;
    registry.Add(kColliding.Fragment("colliding-provider"));

    auto linked = Sora::CLI::LinkAtStartup(kGraphHost, registry.Snapshot());
    REQUIRE_FALSE(linked.has_value());
    REQUIRE(linked.error().kind == Sora::CLI::LinkErrorKind::InvalidFragment);
}

TEST_CASE("CLI startup link preserves root options in the unified linked recognizer", "[Sora][Core][CLI]") {
    Sora::CLI::FragmentRegistry registry;
    auto linked = Sora::CLI::LinkAtStartup(kInvalidHost, registry.Snapshot());

    REQUIRE(linked.has_value());
    const std::array args{std::string_view{"--verbose"}, std::string_view{"host"}};
    REQUIRE(linked->Run(Tokens(args)).value() == 11);
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
